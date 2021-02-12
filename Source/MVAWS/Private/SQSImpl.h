// (c) 2020 by Mackevision, All rights reserved

#pragma once

#include "CoreMinimal.h"
#include "MVAWS.h"
#include "HAL/Thread.h"

#include "Windows/PreWindowsApi.h"
#include <aws/core/Aws.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include "Windows/PostWindowsApi.h"

#include "SQSImpl.generated.h"

namespace Aws::SQS::Model 
{
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
