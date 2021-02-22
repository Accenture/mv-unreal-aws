/* (c) 2021 by Mackevision Mackevision Medien Design GmbH
 * Licensed under the Apache License, Version 2.0.
 * See attached file LICENSE for full details
 */
#include "SQSImpl.h"
#include "IMVAWS.h"
#include "Utils.h"

// Engine
#include "Async/AsyncWork.h"

// AWS SDK
#include "Windows/PreWindowsApi.h"
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/utils/logging/DefaultLogSystem.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/utils/DateTime.h>

#include <aws/sqs/SQSClient.h>
#include <aws/sqs/SQSRequest.h>
#include <aws/sqs/model/ReceiveMessageRequest.h>
#include <aws/sqs/model/ReceiveMessageResult.h>
#include <aws/sqs/model/DeleteMessageRequest.h>
#include "Windows/PostWindowsApi.h"

using namespace Aws::SQS::Model;

void USQSImpl::set_parameters(const FString &n_queue_url, const unsigned int n_wait_time, const bool n_handle_on_game_thread) {

	m_queue_url = TCHAR_TO_UTF8(*n_queue_url);
	m_long_poll_wait_time = n_wait_time;
	m_handler_on_game_thread = n_handle_on_game_thread;
}

bool USQSImpl::start_polling(FOnSQSMessageReceived &&n_delegate) {

	stop_polling();
	join();

	m_poll_interrupted.Store(false);

	if (m_queue_url.empty()) {
		UE_LOG(LogMVAWS, Warning, TEXT("Must have SQS URL"));
		return false;
	}

	Aws::Client::ClientConfiguration client_config;
	client_config.enableEndpointDiscovery = use_endpoint_discovery();
	const FString sqs_endpoint = readenv(TEXT("MVAWS_SQS_ENDPOINT"));
	if (!sqs_endpoint.IsEmpty()) {
		client_config.endpointOverride = TCHAR_TO_UTF8(*sqs_endpoint);
	}

	m_sqs = MakeShareable<Aws::SQS::SQSClient>(new Aws::SQS::SQSClient(client_config));
	m_delegate = n_delegate;

	if (!m_delegate.IsBound()) {
		UE_LOG(LogMVAWS, Warning, TEXT("Cannot start to poll without a bound delegate"));
		return false;
	}

	m_long_poll_max_msg = 1;

	m_poll_thread = MakeUnique<FThread>(TEXT("AWS_Long_Poll"), [this] { this->long_poll(); });

	return true;
}

void USQSImpl::stop_polling() noexcept
{
	// we are running
	if (m_poll_thread)
	{
		UE_LOG(LogMVAWS, Display, TEXT("Shutting down SQS poll thread"));
		m_poll_interrupted.Store(true);
	}
}

void USQSImpl::join() noexcept
{
	// we are running
	if (m_poll_thread)
	{
		UE_LOG(LogMVAWS, Display, TEXT("Joining SQS poll thread"));
		m_poll_interrupted.Store(true);
		m_poll_thread->Join();
		m_poll_thread.Reset();
	}

	m_sqs.Reset();
	m_delegate.Unbind();
}

void USQSImpl::long_poll() noexcept
{
	while (!m_poll_interrupted)
	{
		// It may happen that this component is not yet set up once a caller says start_polling().
		// Right now I only see the way of deferring the actual start until this is done.
		// I do this by waiting for the caller to remedy this
		if (m_queue_url.empty())
		{
			FPlatformProcess::Sleep(0.5);
			continue;
		}

		ReceiveMessageRequest rm_req;
		rm_req.SetQueueUrl(m_queue_url);
		rm_req.SetMaxNumberOfMessages(1);

		// This is not a timeout per se but long polling, which means the call will return
		// After this many seconds even if there are no messages, which is not an error.
		// See https://docs.aws.amazon.com/AWSSimpleQueueService/latest/SQSDeveloperGuide/sqs-short-and-long-polling.html#sqs-long-polling
		rm_req.SetWaitTimeSeconds(m_long_poll_wait_time);

		// In order to enable us to support a number of polling strategies,
		// I request some additional info with each message
		rm_req.SetAttributeNames({

			// For X-Ray tracing, I need an AWSTraceHeader, which in the C++ SDK 
			// seems to be not in the enum like the others. To have it included in the response,
			// only All will do the trick. If tracing is not required, you can remove this to save bandwidth.
			// And yes, "All" supersedes all the other values underneath
			QueueAttributeName::All,

			QueueAttributeName::SentTimestamp,             // to calculate age
			QueueAttributeName::ApproximateReceiveCount    // to see how often that message has been received (approximation)
			});

		// This is how it's supposed to work but it doesn't. Hence the 'All' above
		rm_req.SetMessageAttributeNames({
			MessageSystemAttributeNameMapper::GetNameForMessageSystemAttributeName(MessageSystemAttributeName::AWSTraceHeader)
			});

		ReceiveMessageOutcome rm_out = m_sqs->ReceiveMessage(rm_req);
		if (!rm_out.IsSuccess())
		{
			UE_LOG(LogMVAWS, Warning, TEXT("Failed to receive message from queue '%s': '%s'"),
				UTF8_TO_TCHAR(m_queue_url.c_str()), UTF8_TO_TCHAR(rm_out.GetError().GetMessage().c_str()));
			continue;
		}

		UE_LOG(LogMVAWS, Verbose, TEXT("Long polling returned from queue '%s'"), UTF8_TO_TCHAR(m_queue_url.c_str()));

		if (rm_out.GetResult().GetMessages().empty())
		{
			// no messages in Q
			UE_LOG(LogMVAWS, Display, TEXT("Long polling returned from queue '%s', no messages"), UTF8_TO_TCHAR(m_queue_url.c_str()));
			continue;
		}

		UE_LOG(LogMVAWS, Display, TEXT("Long polling returned from queue '%s', %i messages"), UTF8_TO_TCHAR(m_queue_url.c_str()), rm_out.GetResult().GetMessages().size());

		// Now get the messages and copy into our local storage.
		// They will accumulate until the consumer retrieves them
		process_message(rm_out.GetResult().GetMessages()[0]);

		continue;
	}
}

void USQSImpl::process_message(const Message &n_message) const noexcept
{
	UE_LOG(LogMVAWS, Display, TEXT("process_message '%s'"), UTF8_TO_TCHAR(n_message.GetMessageId().c_str()));
	
	IMVAWSModule::Get().count_sqs_message();

	const int64_t current_epoch_time = Aws::Utils::DateTime::CurrentTimeMillis();
	int64_t sent_epoch_time = 0;
	FString trace_id;

	const Aws::Map<MessageSystemAttributeName, Aws::String> &attributes = n_message.GetAttributes();

	Aws::Map<MessageSystemAttributeName, Aws::String>::const_iterator i =
		attributes.find(MessageSystemAttributeName::SentTimestamp);
	if (i != attributes.cend())
	{
		sent_epoch_time = std::stoull(i->second.c_str());
	};
	const int64_t age = current_epoch_time - sent_epoch_time;

	// Get the AWS trace header for xray
	i = attributes.find(MessageSystemAttributeName::AWSTraceHeader);
	if (i != attributes.cend())
	{
		trace_id = UTF8_TO_TCHAR(i->second.c_str());
	};

	//Get ApproximateReceiveCount
	FString receiveCount;
	i = attributes.find(MessageSystemAttributeName::ApproximateReceiveCount);
	if (i != attributes.cend())
	{
		receiveCount = UTF8_TO_TCHAR(i->second.c_str());
	};

	UE_LOG(LogMVAWS, Display, TEXT("Message ApproximateReceiveCount: %s"), *receiveCount);

	// The trace header comes in the form of "Root=1-235345something"
	// Yet all the examples I saw only use "1-235345something". With the Root=
	// xray upload doesn't work so I remove this
	trace_id = trace_id.Replace(TEXT("Root="), TEXT(""));

	// I have also seen more stuff coming after the first ID, separated by ;
	// Let's remove this as well. I wish that stuff was documented
	int32 idx;
	if (trace_id.FindChar(';', idx))
	{
		trace_id = trace_id.Left(idx);
	}

	// Now construct a message which will be given into the handler
	const FMVAWSMessage m{
		UTF8_TO_TCHAR(n_message.GetMessageId().c_str()),
		UTF8_TO_TCHAR(n_message.GetReceiptHandle().c_str()),
		current_epoch_time - sent_epoch_time,
		trace_id,                                  // @todo get and insert x-ray header
		UTF8_TO_TCHAR(n_message.GetBody().c_str())
	};

	// This promise will be fulfilled by the delegate implementation
	const SQSReturnPromisePtr rp = MakeShareable<SQSReturnPromise>(new SQSReturnPromise());
	TFuture<bool> return_future = rp->GetFuture();

	// Call the delegate on the game thread
	if (m_handler_on_game_thread) {
		FGraphEventRef game_thread_task
			= FFunctionGraphTask::CreateAndDispatchWhenReady([m, handler{ this->m_delegate }, rp]{

				handler.Execute(m, rp);

				}, TStatId(), NULL, ENamedThreads::GameThread);
	} else {
		m_delegate.Execute(m, rp);
	}

	// Now we wait for the delegate impl to call SetValue() on the promise.
	// This might take forever if the implementation is not careful.
	// It might be worthwhile to include a timeout but I want to discuss this first.
	return_future.Wait();
	if (return_future.Get()) {
		delete_message(n_message);
	} else {
		UE_LOG(LogMVAWS, Display, TEXT("Not deleting message '%s', handler returned false"), *m.m_message_id);
	}
}

void USQSImpl::delete_message(const Message &n_message) const noexcept
{
	UE_LOG(LogMVAWS, Verbose, TEXT("Starting deletion of message '%s'"), UTF8_TO_TCHAR(n_message.GetMessageId().c_str()));

	DeleteMessageRequest request;
	request.SetQueueUrl(m_queue_url);
	request.SetReceiptHandle(n_message.GetReceiptHandle());

	// issue the delete request
	const DeleteMessageOutcome outcome = m_sqs->DeleteMessage(request);

	if (outcome.IsSuccess())
	{
		UE_LOG(LogMVAWS, Display, TEXT("Deleted message '%s'"), UTF8_TO_TCHAR(n_message.GetMessageId().c_str()));
	}
	else
	{
		UE_LOG(LogMVAWS, Error, TEXT("Deletion of message '%s' failed: "), UTF8_TO_TCHAR(n_message.GetMessageId().c_str()), UTF8_TO_TCHAR(outcome.GetError().GetMessage().c_str()));
	}
}

