/* (c) 2021 by Mackevision Mackevision Medien Design GmbH
 * Licensed under the Apache License, Version 2.0.
 * See attached file LICENSE for full details
 */
#include "CloudWatchOutputDevice.h"
#include "IMVAWS.h"
#include "Utils.h"

// Engine
#include "Async/AsyncWork.h"

// AWS SDK
#include "Windows/PreWindowsApi.h"
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/core/utils/logging/DefaultLogSystem.h>
#include <aws/core/utils/threading/Executor.h>
#include <aws/logs/CloudWatchLogsClient.h>
#include <aws/logs/CloudWatchLogsErrors.h>
#include <aws/logs/model/PutLogEventsRequest.h>
#include <aws/logs/model/CreateLogGroupRequest.h>
#include <aws/logs/model/CreateLogStreamRequest.h>
#include <aws/logs/model/DescribeLogStreamsRequest.h>
#include "Windows/PostWindowsApi.h"

// Std
#include <string>

using namespace Aws::CloudWatchLogs::Model;
using Aws::CloudWatchLogs::CloudWatchLogsError;
using Aws::CloudWatchLogs::CloudWatchLogsErrors;


FCloudWatchLogOutputDevice::FCloudWatchLogOutputDevice(const FString &n_log_group_prefix)
		: m_logger_interrupted{ false }
		, m_log_group_prefix(n_log_group_prefix)
		, m_log_group_name{ TCHAR_TO_UTF8(*n_log_group_prefix) }
{
	m_log_group_name.append("unspecified");
	bAutoEmitLineTerminator = false;
	m_logger_thread = MakeUnique<FThread>(TEXT("AWS_Logging"), [this] { this->log_thread(); });
}

FCloudWatchLogOutputDevice::~FCloudWatchLogOutputDevice() noexcept
{
	TearDown();
}

void FCloudWatchLogOutputDevice::TearDown()
{
	// we are running
	if (m_logger_thread)
	{
		// UE_LOG(LogMVAWS, Display, TEXT("Shutting down logging thread"));
		m_logger_interrupted.Store(true);
		m_logger_thread->Join();
		m_logger_thread.Reset();

		Aws::Delete(m_cwl);
		m_cwl = nullptr;
	}
}

void FCloudWatchLogOutputDevice::Serialize(const TCHAR *n_message, ELogVerbosity::Type n_verbosity, const FName &n_category, const double n_time)
{
	Serialize(n_message, n_verbosity, n_category);
}

void FCloudWatchLogOutputDevice::Serialize(const TCHAR* n_message, ELogVerbosity::Type n_verbosity, const FName &n_category)
{
	FCloudWatchLogOutputDevice::entry e;
	e.m_timestamp = epoch_milliseconds();
	e.m_message.reserve(128);

	switch (n_verbosity)
	{
		case ELogVerbosity::NoLogging:
			e.m_message = Aws::String("[NOLOGGING] (");
			break;
		case ELogVerbosity::Fatal:
			e.m_message = Aws::String("[FATAL] (");
			break;
		case ELogVerbosity::Error:
			e.m_message = Aws::String("[ERROR] (");
			break;
		case ELogVerbosity::Warning:
			e.m_message = Aws::String("[WARNING] (");
			break;
		case ELogVerbosity::Display:
			e.m_message = Aws::String("[INFO] (");
			break;
		case ELogVerbosity::Log:
			e.m_message = Aws::String("[LOG] (");
			break;
		case ELogVerbosity::Verbose:
			e.m_message = Aws::String("[VERBOSE] (");
			break;
		default:
			e.m_message = Aws::String("[CATCH_ALL] (");
	}

	e.m_message.append(TCHAR_TO_UTF8(*n_category.ToString()));
	e.m_message.append(") ");
	e.m_message.append(TCHAR_TO_UTF8(n_message));

	// This is thread safe.
	m_log_q.Enqueue(MoveTemp(e));
}

FString FCloudWatchLogOutputDevice::get_log_group_name() noexcept
{
	FString group_name{ m_log_group_prefix };
	group_name.Append(readenv(TEXT("MVAWS_STACK_NAME"), TEXT("unknown_stack")));
	return group_name;
}

FString FCloudWatchLogOutputDevice::get_log_stream_name(const FString &n_instance_id) noexcept
{
	const FDateTime now = FDateTime::Now();

	return FString::Printf(TEXT("%04i/%02i/%02i-%02i/%02i-%s"),
		now.GetYear(), now.GetMonth(), now.GetDay(), now.GetHour(), now.GetMinute(), *n_instance_id);
}

void FCloudWatchLogOutputDevice::log_thread() noexcept
{
	// First let's get our instance ID. We are using this to name our log streams
	// Looks like the SDK doesn't have code for that so I'm going to contact
	// the metadata server myself with a tight-ish timeout
	m_instance_id = get_instance_id();

	// instance id plus prefix
	m_log_group_name = TCHAR_TO_UTF8(*get_log_group_name());

	// timestamp ++
	m_log_stream_name = TCHAR_TO_UTF8(*get_log_stream_name(m_instance_id));

	Aws::Client::ClientConfiguration client_config;
	client_config.enableEndpointDiscovery = use_endpoint_discovery();
	const FString cw_endpoint = readenv(TEXT("MVAWS_CLOUDWATCH_ENDPOINT"));
	if (!cw_endpoint.IsEmpty())
	{
		client_config.endpointOverride = TCHAR_TO_UTF8(*cw_endpoint);
	}
	m_cwl = Aws::New<Aws::CloudWatchLogs::CloudWatchLogsClient>("cloudwatchlogs", client_config);

	// Now we should have all the data to create a log group and stream for us
	CreateLogGroupRequest clgr;
	clgr.SetLogGroupName(m_log_group_name);
	CreateLogGroupOutcome lgoc = m_cwl->CreateLogGroup(clgr);
	if (!lgoc.IsSuccess())
	{
		const CloudWatchLogsError &err{ lgoc.GetError() };
		if (err.GetErrorType() != CloudWatchLogsErrors::RESOURCE_ALREADY_EXISTS)
		{
			UE_LOG(LogMVAWS, Error, TEXT("Failed to create cloudwatch log group: %s"), UTF8_TO_TCHAR(err.GetMessage().c_str()));
			return;
		}
	}

	CreateLogStreamRequest clsr;
	clsr.SetLogGroupName(m_log_group_name);
	clsr.SetLogStreamName(m_log_stream_name);
	CreateLogStreamOutcome oc = m_cwl->CreateLogStream(clsr);
	if (!oc.IsSuccess())
	{
		UE_LOG(LogMVAWS, Error, TEXT("Failed to create cloudwatch log stream: %s"), UTF8_TO_TCHAR(oc.GetError().GetMessage().c_str()));
		return;
	}

	while (!m_logger_interrupted)
	{
		// CloudWatch requests cost serious money. So I opt for a longer logging delay
		// here in order to not send too often. If you require more timely logs at 
		// the expense of CW costs, feel free to reduce to, say 3 or 1
		unsigned int sleep_time = 5;
		while (sleep_time--)
		{
			if (m_logger_interrupted)
			{
				return;
			}
			else
			{
				FPlatformProcess::Sleep(1.0);
			}
		}

		if (!m_log_q.IsEmpty())
		{
			send_log_messages();
		}
	}
}

void FCloudWatchLogOutputDevice::send_log_messages() noexcept
{
	int max_log_events = 50;

	PutLogEventsRequest request;
	if (!m_upload_sequence_token.empty())
	{
		request.SetSequenceToken(m_upload_sequence_token);
	}
	request.SetLogGroupName(m_log_group_name);
	request.SetLogStreamName(m_log_stream_name);

	// One by one, pop log entries from the q and add them to the CloudWatch
	// request. Not more than 50 to not overload the body of the request.
	FCloudWatchLogOutputDevice::entry le;

	while (m_log_q.Dequeue(le) && max_log_events--)
	{
		InputLogEvent ile;
		ile.SetTimestamp(le.m_timestamp);
		ile.SetMessage(le.m_message);
		request.AddLogEvents(std::move(ile));
	}

	// Send the logs to CloudWatch. They should appear in the AWS console a
	// few seconds later
	PutLogEventsOutcome oc = m_cwl->PutLogEvents(request);

	if (oc.IsSuccess())
	{
		m_upload_sequence_token = oc.GetResult().GetNextSequenceToken();
	}
	else
	{
		const CloudWatchLogsError &err{ oc.GetError() };
		UE_LOG(LogMVAWS, Error, TEXT("Failed to send CloudWatch Logs: %s"), UTF8_TO_TCHAR(err.GetMessage().c_str()));
	}
}

bool FCloudWatchLogOutputDevice::CanBeUsedOnMultipleThreads() const
{
	return true;
}

bool FCloudWatchLogOutputDevice::CanBeUsedOnAnyThread() const
{
	return true;
}

