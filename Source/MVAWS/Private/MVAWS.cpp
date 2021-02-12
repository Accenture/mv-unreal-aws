// (c) 2020 by Mackevision, All rights reserved

#include "MVAWS.h"
#include "AWSConnectionConfig.h"
#include "MonitoringImpl.h"
#include "CloudWatchOutputDevice.h"
#include "XRayImpl.h"
#include "S3Impl.h"
#include "SQSImpl.h"

#include "Windows/PreWindowsApi.h"
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/client/AWSError.h>
#include <aws/core/utils/Outcome.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/core/utils/memory/stl/AWSAllocator.h>
#include <aws/core/utils/logging/AWSLogging.h>
#include <aws/core/utils/logging/DefaultLogSystem.h>
#include "Windows/PostWindowsApi.h"

#include <iostream>
//#include <fstream>
#include <memory>
//#include <strstream>

DEFINE_LOG_CATEGORY(LogMVAWS)
#define LOCTEXT_NAMESPACE "FMVAWSModule"

static TUniquePtr<FCloudWatchLogOutputDevice> s_cwl_output_device = nullptr;


void FMVAWSModule::init_actor_ready(const AAWSConnectionConfig *n_config) {

	checkf(m_monitoring_impl, TEXT("Monitoring impl object was not created"));
	checkf(m_xray_impl, TEXT("XRay impl object was not created"));
	checkf(m_s3_impl,   TEXT("S3 impl object was not created"));
	checkf(m_sqs_impl,  TEXT("SQS impl object was not created"));
	
	if (n_config) {
		m_xray_enabled = xray_enabled(n_config->XRayEnabled);

		// Create a CloudWatch log output device so all logs
		// will be sent to CloudWatch
		if (cloudwatch_logs_enabled(n_config->CloudWatchLogs)) {
			if (!s_cwl_output_device) {
				s_cwl_output_device.Reset(new FCloudWatchLogOutputDevice(n_config->CloudWatchLogGroupPrefix));
			}

			GLog->AddOutputDevice(s_cwl_output_device.Get());
		}

		m_s3_impl->set_default_bucket_name(readenv(n_config->BucketNameEnvVariableName, n_config->BucketName));

		if (n_config->AWSLogs) {
			// You won't need logging in live system. This is file IO after all.
			// Disable this for production unless you need it
			Aws::Utils::Logging::InitializeAWSLogging(
				Aws::MakeShared<Aws::Utils::Logging::DefaultLogSystem>(
					"MVAllocationTag", Aws::Utils::Logging::LogLevel::Info, "aws_sdk_"));
		}

		m_sqs_impl->set_parameters(n_config->QueueURL, n_config->LongPollWait, n_config->SQSHandlerOnGameThread);

		if (cloudwatch_metrics_enabled(n_config->CloudWatchMetrics)) {
			m_monitoring_impl->start_metrics();
		}

		// Now let's try our logging right away
		UE_LOG(LogMVAWS, Display, TEXT("MVAWS initialized"));
	} else {
		UE_LOG(LogMVAWS, Display, TEXT("MVAWS shutting down"));
		m_xray_enabled = false;
		m_s3_impl->set_default_bucket_name(FString{});
		m_sqs_impl->stop_polling();
		m_monitoring_impl->stop_metrics();

		m_monitoring_impl->join();
		m_sqs_impl->join();

		if (s_cwl_output_device) {
			GLog->RemoveOutputDevice(s_cwl_output_device.Get());
			// I am not deleting this object as the engine should call TearDown()
			// upon it to release resources
		}
	}
}

void FMVAWSModule::StartupModule() {

	UE_LOG(LogMVAWS, Display, TEXT("Starting AWS Connector Plugin"));

	m_sdk_options.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Info;

	Aws::InitAPI(m_sdk_options);

	m_monitoring_impl = NewObject<UMonitoringImpl>();
	m_monitoring_impl->AddToRoot();
	m_xray_impl = NewObject<UXRayImpl>();
	m_xray_impl->AddToRoot();
	m_s3_impl  = NewObject<US3Impl>();
	m_s3_impl->AddToRoot();
	m_sqs_impl = NewObject<USQSImpl>();
	m_sqs_impl->AddToRoot();
}

void FMVAWSModule::ShutdownModule() {

	UE_LOG(LogMVAWS, Display, TEXT("Shutting down AWS Connector Plugin"));

	Aws::Utils::Logging::ShutdownAWSLogging();

	Aws::ShutdownAPI(m_sdk_options);
}

bool FMVAWSModule::cache_upload(const FS3UploadTarget &n_target, TUniquePtr<unsigned char[]> &&n_data,
		const size_t n_size, const FString &n_trace_id, const FOnCacheUploadFinished n_completion) 
{
	checkf(m_s3_impl, TEXT("S3 impl object was not created"));
	return m_s3_impl->cache_upload(n_target, MoveTemp(n_data), n_size, n_trace_id, n_completion);
}

bool FMVAWSModule::cache_upload(const FS3UploadTarget &n_target, const FString &n_file_path,
	const FString &n_trace_id, const FOnCacheUploadFinished n_completion)
{
	checkf(m_s3_impl, TEXT("S3 impl object was not created"));
	return m_s3_impl->cache_upload(n_target, n_file_path, n_trace_id, n_completion);
}

bool FMVAWSModule::start_sqs_poll(FOnSQSMessageReceived &&n_delegate)
{
	checkf(m_sqs_impl, TEXT("SQS impl object was not created"));
	return m_sqs_impl->start_polling(MoveTemp(n_delegate));
}

void FMVAWSModule::stop_sqs_poll()
{
	checkf(m_sqs_impl, TEXT("SQS impl object was not created"));
	m_sqs_impl->stop_polling();
	m_sqs_impl->join();
}

FString FMVAWSModule::start_trace_segment(const FString &n_trace_id, const FString &n_segment_name) 
{
	checkf(m_xray_impl, TEXT("XRay impl object was not created"));
	if (!m_xray_enabled) {
		return {};
	}
	return m_xray_impl->start_trace_segment(n_trace_id, n_segment_name);
}

FString FMVAWSModule::start_trace_subsegment(const FString &n_trace_id, const FString &n_name) 
{
	checkf(m_xray_impl, TEXT("XRay impl object was not created"));
	if (!m_xray_enabled) {
		return {};
	}
	return m_xray_impl->start_trace_subsegment(n_trace_id, n_name);
}

void FMVAWSModule::end_trace_subsegment(const FString &n_trace_id, const FString n_subsegment_id, const bool n_error) 
{
	checkf(m_xray_impl, TEXT("XRay impl object was not created"));
	if (!m_xray_enabled) {
		return;
	}
	return m_xray_impl->end_trace_subsegment(n_trace_id, n_subsegment_id, n_error);
}

void FMVAWSModule::end_trace_segment(const FString &n_trace_id, const bool n_error) 
{
	checkf(m_xray_impl, TEXT("XRay impl object was not created"));
	if (!m_xray_enabled) {
		return;
	}
	return m_xray_impl->end_trace_segment(n_trace_id, n_error);
}

void FMVAWSModule::count_image_rendered(const float n_milliseconds) noexcept
{
	checkf(m_monitoring_impl, TEXT("Monitoring impl object was not created"));
	return m_monitoring_impl->count_image_rendered(n_milliseconds);
}

void FMVAWSModule::count_membuf_upload(const float n_milliseconds) noexcept
{
	checkf(m_monitoring_impl, TEXT("Monitoring impl object was not created"));
	return m_monitoring_impl->count_membuf_s3_upload(n_milliseconds);
}

void FMVAWSModule::count_file_upload(const float n_milliseconds) noexcept
{
	checkf(m_monitoring_impl, TEXT("Monitoring impl object was not created"));
	return m_monitoring_impl->count_file_s3_upload(n_milliseconds);
}

void FMVAWSModule::count_sqs_message() noexcept
{
	checkf(m_monitoring_impl, TEXT("Monitoring impl object was not created"));
	return m_monitoring_impl->count_sqs_message();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_GAME_MODULE(FMVAWSModule, MVAWS)
