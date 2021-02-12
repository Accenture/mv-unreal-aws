// (c) 2021 by Mackevision, All rights reserved

#pragma once

#include "CoreMinimal.h"
#include "HAL/Thread.h"
#include "Templates/Atomic.h"
#include "Logging/LogVerbosity.h"
#include "Containers/Queue.h"

#include "Windows/PreWindowsApi.h"
#include <aws/core/Aws.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include "Windows/PostWindowsApi.h"

#include <string>

#include "MonitoringImpl.generated.h"

namespace Aws 
{
	namespace CloudWatch
	{
		class CloudWatchClient;

		namespace Model
		{
			enum class StandardUnit;
		}
	}
}

/*! \brief Implementation for cloudwatch metrics
 * Runs a background thread that will send custom metrics
 * to CloudWatch every 10 seconds as long as values
 * have been accumulated during that time
 */
UCLASS()
class UMonitoringImpl : public UObject 
{
	GENERATED_BODY()

	public:
		//! start the sending thread
		void start_metrics();

		//! stop the send thread. Call join afterwards to wait for this to complete
		void stop_metrics() noexcept;
		void join() noexcept;

		/*! \brief register one rendered image
		 *  Counters will end up in namespace MV/TRAFFIC.
		 *  Function will return immediately, metrics to be sent in next
		 *  thread cycle
		 */
		void count_image_rendered(const float n_milliseconds) noexcept;

		/*! \brief register one memory buffer (image) upload to S3
		 *  will return immediately and queue for sending with the next batch
		 */
		void count_membuf_s3_upload(const float n_milliseconds) noexcept;

		/*! \brief register one file (video) upload to S3
		 *  will return immediately and queue for sending with the next batch
		 */
		void count_file_s3_upload(const float n_milliseconds) noexcept;

		/*! \brief register one received SQS message
		 *  will return immediately and queue for sending with the next batch
		 */
		void count_sqs_message() noexcept;

	private:

		struct single_entry {
			float                                m_value;
			Aws::CloudWatch::Model::StandardUnit m_unit;
			Aws::String                          m_metric_name;
		};

		using SingleSampleQueue = TQueue<single_entry, EQueueMode::Mpsc>;

		void metrics_thread() noexcept;

		// CloudWatch specifically recommends to not have gaps in your values
		// and send metrics even when nothing happened. This way your application
		// looks alive when not busy. I will follow that advise but only for SQS messages
		// as it seems to me sending zero render times might mess up scaling calculations
		// along the way
		void send_blank() noexcept;

		void send_values() noexcept;

		TSharedPtr<Aws::CloudWatch::CloudWatchClient>  m_cw_client;
		TUniquePtr<FThread>                 m_metrics_thread;
		TAtomic<bool>                       m_metrics_interrupted;

		// set by thread and only accessed there, hence unprotected
		FString                             m_instance_id;

		SingleSampleQueue                   m_single_values;
		TAtomic<unsigned int>               m_sqs_messages;
};
