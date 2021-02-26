/* (c) 2021 by Mackevision Mackevision Medien Design GmbH
 * Licensed under the Apache License, Version 2.0.
 * See attached file LICENSE for full details
 */
#pragma once

#include "CoreMinimal.h"
#include "IMVAWS.h"
#include "Templates/UniquePtr.h"

#include "Windows/PreWindowsApi.h"
#include <aws/core/Aws.h>
#include "Windows/PostWindowsApi.h"

/*!
 * Implementation module for AWS connectivity
 * comments for overwritten methods can be found in parent class
 */
class FMVAWSModule : public IMVAWSModule 
{

	public:

		bool cache_upload(const FS3UploadTarget &n_target, TUniquePtr<unsigned char []> &&n_data,
				const size_t n_size, const FString &n_trace_id = FString{}, const FOnCacheUploadFinished n_completion = FOnCacheUploadFinished{}) override;

		bool cache_upload(const FS3UploadTarget &n_target, const FString &n_file_path,
			const FString &n_trace_id = FString{}, const FOnCacheUploadFinished n_completion = FOnCacheUploadFinished{}) override;
		
		bool start_sqs_poll(FOnSQSMessageReceived &&n_delegate) override;
		void stop_sqs_poll() override;

		FString start_trace_segment(const FString &n_trace_id, const FString &n_segment_name) override;
		FString start_trace_subsegment(const FString &n_trace_id, const FString &n_name) override;
		void end_trace_subsegment(const FString &n_trace_id, const FString n_subsegment_id, const bool n_error = false) override;
		void end_trace_segment(const FString &n_trace_id, const bool n_error = false) override;

		void count_image_rendered(const float n_milliseconds) noexcept override;
		void count_membuf_upload(const float n_milliseconds) noexcept override;
		void count_file_upload(const float n_milliseconds) noexcept override;
		void count_sqs_message() noexcept override;

		void set_message_visibilty_timeout(const FMVAWSMessage& n_message, const int n_timeout) noexcept override;

		// IModuleInterface interface
		void StartupModule() override;
		void ShutdownModule() override;

		bool SupportsDynamicReloading() override 
		{
			return true; // @todo: Eventually, this should probably not be allowed.
		}

	private:
		friend class AAWSConnectionConfig;
	
		/**
		 * Config actor calls this to fire up the module and let it know it's working
		 * The module will take configuration data from the actor but not hold
		 * any reference to it.
		 */
		void init_actor_ready(const class AAWSConnectionConfig *n_config);

		UPROPERTY()
		class UMonitoringImpl  *m_monitoring_impl = nullptr;

		UPROPERTY()
		class UXRayImpl *m_xray_impl = nullptr;
		bool             m_xray_enabled = false; // Not atomic, not changing at runtime.

		UPROPERTY()
		class US3Impl   *m_s3_impl = nullptr;

		UPROPERTY()
		class USQSImpl  *m_sqs_impl = nullptr;

		Aws::SDKOptions  m_sdk_options;
};
