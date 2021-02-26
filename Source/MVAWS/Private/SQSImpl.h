/* (c) 2021 by Mackevision Mackevision Medien Design GmbH
 * Licensed under the Apache License, Version 2.0.
 * See attached file LICENSE for full details
 */
#pragma once

#include "CoreMinimal.h"
#include "MVAWS.h"
#include "HAL/Thread.h"

#include "Windows/PreWindowsApi.h"
#include <aws/core/Aws.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include "Windows/PostWindowsApi.h"

#include "SQSImpl.generated.h"

namespace Aws::SQS::Model {
	class Message;
}

namespace Aws::SQS {
	class SQSClient;
}

/*!
* Implementation wrapper for SQS functions.
* This has no other function than bundle SQS related stuff in one place.
*/
UCLASS()
class USQSImpl : public UObject {

	GENERATED_BODY()

	public:
		void set_parameters(const FString &n_queue_url, const unsigned int n_wait_time, const bool n_handle_on_game_thread);
		
		/*! I assume one queue for all our messages.
		 * This also starts the listening process. If the string is empty,
		 * the listening thread is stopped.
		 */
		bool start_polling(FOnSQSMessageReceived &&n_delegate);

		//! stop the polling thread. Non-blocking. Call join() afterwards to wait for it to complete
		void stop_polling() noexcept;
		void join() noexcept;

		/**
		*	Sets the new visibility timeout value in seconds for the message being in the queue
		*	The message should not visible to other customers, for the delete message request to 
		*	be successful. Calling the client from game thread is thread-safe
		*   https://docs.aws.amazon.com/sdk-for-cpp/v1/developer-guide/using-service-client.html
		*/
		void set_message_visibilty_timeout(const FMVAWSMessage& n_message,const int n_timeout) const noexcept;

	private:
		// running in thread
		void long_poll() noexcept;

		// copy messages out of a received bunch into our local storage
		void process_message(const Aws::SQS::Model::Message &n_message) const noexcept;

		void delete_message(const Aws::SQS::Model::Message &n_message) const noexcept;

		TSharedPtr<Aws::SQS::SQSClient>   m_sqs;
		Aws::String           m_queue_url;
		unsigned int          m_long_poll_max_msg;
		unsigned int          m_long_poll_wait_time;
		bool                  m_handler_on_game_thread;

		TUniquePtr<FThread>   m_poll_thread;
		TAtomic<bool>         m_poll_interrupted;

		FOnSQSMessageReceived m_delegate;

};
