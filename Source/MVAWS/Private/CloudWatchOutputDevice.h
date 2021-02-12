// (c) 2021 by Mackevision, All rights reserved

#pragma once

#include "CoreMinimal.h"
#include "HAL/Thread.h"
#include "Templates/Atomic.h"
#include "Logging/LogVerbosity.h"
#include "Misc/OutputDevice.h"
#include "Containers/Queue.h"

#include "Windows/PreWindowsApi.h"
#include <aws/core/Aws.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include "Windows/PostWindowsApi.h"

namespace Aws 
{
	namespace CloudWatchLogs
	{
		class CloudWatchLogsClient;
	}
}

/*! \brief logging backend to be included in Unreal's logs
 *  sends logs to CloudWatch every 5 seconds
 */
class FCloudWatchLogOutputDevice : public FOutputDevice {

	public:
		FCloudWatchLogOutputDevice(const FString &n_log_group_prefix);
		virtual ~FCloudWatchLogOutputDevice() noexcept;

		void TearDown() override;
		void Serialize(const TCHAR *n_message, ELogVerbosity::Type n_verbosity, const FName &n_category) override;
		void Serialize(const TCHAR *n_message, ELogVerbosity::Type n_verbosity, const FName &n_category, const double n_time) override;

		bool CanBeUsedOnMultipleThreads() const override;

		bool CanBeUsedOnAnyThread() const override;

	private:
		/// loop and log
		void log_thread() noexcept;

		/// called from thread
		void send_log_messages() noexcept;

		// Determine a suitable log group name by using the environment 
		// variable MVAWS_STACK_NAME and assemble something.
		FString get_log_group_name() noexcept;

		// Determine a suitable log stream name by using the instance ID
		// and then attach a date stamp
		static FString get_log_stream_name(const FString &n_instance_id) noexcept;

		/*!
		 * Client is owned here and instantiated by thread.
		 * Raw ptr as I'd have to include hdr for unique ptr
		 */
		Aws::CloudWatchLogs::CloudWatchLogsClient  *m_cwl = nullptr;

		TUniquePtr<FThread>      m_logger_thread;
		TAtomic<bool>            m_logger_interrupted;

		struct entry {
			long long            m_timestamp;   //!< millis since epoch (type required by InputLogEvent)
			Aws::String          m_message;
		};

		using LogQueue = TQueue<entry, EQueueMode::Mpsc>;

		LogQueue                 m_log_q;
		FString                  m_instance_id;
		const FString            m_log_group_prefix;
		Aws::String              m_log_group_name;
		Aws::String              m_log_stream_name;
		Aws::String              m_upload_sequence_token;
};

