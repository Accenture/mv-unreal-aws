// (c) 2020 by Mackevision, All rights reserved

#pragma once

#include "CoreMinimal.h"
#include "MVAWS.h"
#include "Templates/UniquePtr.h"

#include "Windows/PreWindowsApi.h"
#include <aws/core/Aws.h>
#include "Windows/PostWindowsApi.h"

#include "S3Impl.generated.h"

/*!
 * Implementation wrapper for s3 functions.
 * This has no other function than bundle S3 related stuff in one place.
 */
UCLASS()
class US3Impl : public UObject 
{

	GENERATED_BODY()

	public:
		/// This is static here as I assume all uploads to go in one bucket.
		/// If this is not desired, use FS3UploadTarget's setting below and ignore this
		void set_default_bucket_name(const FString &n_bucket_name);

		bool cache_upload(const FS3UploadTarget &n_target, TUniquePtr<unsigned char []> &&n_data,
				const size_t n_size, const FString &n_trace_id, const FOnCacheUploadFinished n_completion);
		
		bool cache_upload(const FS3UploadTarget &n_target, const FString &n_file_path, 
				const FString &n_trace_id, const FOnCacheUploadFinished n_completion);
	private:
		FString   m_default_bucket_name;
};
