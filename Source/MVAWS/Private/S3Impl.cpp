// (c) 2020 by Mackevision, All rights reserved

#include "S3Impl.h"

// Engine
#include "Async/AsyncWork.h"
#include "Async/TaskGraphInterfaces.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformFilemanager.h"

// AWS SDK
#include "Windows/PreWindowsApi.h"
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/client/AWSError.h>
#include <aws/core/utils/Outcome.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/core/utils/memory/stl/AWSAllocator.h>
#include <aws/core/utils/logging/AWSLogging.h>
#include <aws/core/utils/logging/DefaultLogSystem.h>
#include <aws/core/auth/AWSCredentialsProvider.h>

#include <aws/s3/S3Client.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/s3/model/PutObjectResult.h>
#include "Windows/PostWindowsApi.h"

// Std
#include <iostream>
#include <memory>
#include <strstream>
#include <fstream>

using namespace Aws::S3::Model;

namespace 
{

/** @brief An asynchronous task which will take care of uploading the S3 data in a queued thread pool
	This is a simple form of such a task and only meant to make S3 uploads fire and forget
	parallel threads without having to maintain them or spawn a thread myself.
	It takes ownership over the data.
 */
class MembufUploadAsyncTask : public FNonAbandonableTask 
{
	private:
		MembufUploadAsyncTask() = delete;
		MembufUploadAsyncTask(const MembufUploadAsyncTask &) = delete;
		MembufUploadAsyncTask(MembufUploadAsyncTask &&) = default;

		MembufUploadAsyncTask(const FS3UploadTarget &n_target,
						TUniquePtr<unsigned char []> &&n_data,
						const size_t n_size,
						const FString n_trace_id,
						const FOnCacheUploadFinished n_completion)
				: m_target{ n_target }
				, m_data{ MoveTemp(n_data) }
				, m_size{ n_size }
				, m_trace_id{ n_trace_id }
				, m_completion_delegate{ n_completion } {}

		void DoWork() 
		{
			const long long start_time = epoch_milliseconds();
			FString subseg_id;
			if (!m_trace_id.IsEmpty()) {
				// begin x-ray trace of this command. This is called a subsegment, which is later assembled to a segment
				subseg_id = IMVAWSModule::Get().start_trace_subsegment(m_trace_id, TEXT("S3Upload"));
			}

			// Normally I would specify a credentials profile to use like this:
			//
			//	Aws::Client::ClientConfiguration client_config(TCHAR_TO_ANSI(*m_profile));
			//	client_config.region = "eu-central-1";
			//	Aws::S3::S3Client s3(client_config);
			//
			// However, this doesn't seem to work. It always chooses default profile.
			// Instead, setting an environment variable called AWS_PROFILE to the 
			// desired profile will do the trick.
			// I have removed profile selection as a consequence.
			// Most likely a bug but not really relevant for live cases as this will generally
			// use the role attached to the instance.
		
			UE_LOG(LogMVAWS, Display, TEXT("Starting upload"));

			// Create a S3 client object and assemble request
			Aws::S3::S3Client s3;

			PutObjectRequest request;
			request.SetBucket(TCHAR_TO_ANSI(*m_target.BucketName));
			request.SetKey(TCHAR_TO_ANSI(*m_target.ObjectKey));
			request.SetContentType(TCHAR_TO_ANSI(*m_target.ContentType));

			// Create a streambuf wrapper around our buffer without copying it
			std::strstreambuf sbuf{ m_data.Get(), static_cast<std::streamsize>(m_size) };

			// And a stream to read from it. Sadly, this needs to be an IOStream 
			// even though there's no modifying it
			std::shared_ptr<Aws::IOStream> input_data =
				Aws::MakeShared<Aws::IOStream>("MVAllocationTag", &sbuf);

			request.SetBody(input_data);

			// issue the put request
			const PutObjectOutcome outcome = s3.PutObject(request);

			// If we have a completion handler, execute it on the game thread like guaranteed in the interface
			if (m_completion_delegate.IsBound()) 
			{
			
				FGraphEventRef game_thread_task
						= FFunctionGraphTask::CreateAndDispatchWhenReady([outcome, handler{this->m_completion_delegate}, object_key{ this->m_target.ObjectKey }] {
					
					if (outcome.IsSuccess()) 
					{
						UE_LOG(LogMVAWS, Display, TEXT("Upload of object '%s' complete"), *object_key);
						handler.Execute(true, object_key);
					} 
					else 
					{
						UE_LOG(LogMVAWS, Error, TEXT("Upload of object '%s' failed: %s"), *object_key,
								UTF8_TO_TCHAR(outcome.GetError().GetMessage().c_str()));
						handler.Execute(false, object_key);
					}
					
				}, TStatId(), NULL, ENamedThreads::GameThread);
			} else 
			{
				// Otherwise issue the error directly to the logs
				if (!outcome.IsSuccess()) 
				{
					UE_LOG(LogMVAWS, Error, TEXT("Upload of object '%s' to bucket '%s' failed: %s"), *m_target.ObjectKey, *m_target.BucketName,
							UTF8_TO_TCHAR(outcome.GetError().GetMessage().c_str()));
				} 
				else 
				{
					UE_LOG(LogMVAWS, Display, TEXT("Upload of object '%s' to bucket '%s' complete"), *m_target.ObjectKey, *m_target.BucketName);
				}
			}

			// begin x-ray trace of this command. This is called a subsegment, which is later assembled to a segment
			if (!subseg_id.IsEmpty()) 
			{
				IMVAWSModule::Get().end_trace_subsegment(m_trace_id, subseg_id);
			}

			const long long end_time = epoch_milliseconds();
			IMVAWSModule::Get().count_membuf_upload(static_cast<float>(end_time - start_time));
		}

		FORCEINLINE TStatId GetStatId() const 
		{
			RETURN_QUICK_DECLARE_CYCLE_STAT(MembufUploadAsyncTask, STATGROUP_ThreadPoolAsyncTasks);
		}

	private:
		friend class FAutoDeleteAsyncTask<MembufUploadAsyncTask>;

		const FS3UploadTarget              m_target;
		const TUniquePtr<unsigned char []> m_data;
		const size_t                       m_size;
		const FString                      m_trace_id;
		const FOnCacheUploadFinished       m_completion_delegate;
};


class FileUploadAsyncTask : public FNonAbandonableTask
{

	private:
		FileUploadAsyncTask() = delete;
		FileUploadAsyncTask(const FileUploadAsyncTask &) = delete;
		FileUploadAsyncTask(FileUploadAsyncTask &&) = default;

		FileUploadAsyncTask(const FS3UploadTarget &n_target,
						const FString n_file_path,
						const FString n_trace_id,
						const FOnCacheUploadFinished n_completion)
				: m_target{ n_target }
				, m_file_path{ n_file_path }
				, m_trace_id{ n_trace_id }
				, m_completion_delegate{ n_completion } {}

		void DoWork()
		{
			const long long start_time = epoch_milliseconds();

			FString subseg_id;
			if (!m_trace_id.IsEmpty()) {
				// begin x-ray trace of this command. This is called a subsegment, which is later assembled to a segment
				subseg_id = IMVAWSModule::Get().start_trace_subsegment(m_trace_id, TEXT("S3Upload"));
			}

			UE_LOG(LogMVAWS, Display, TEXT("Starting upload"));

			// Create a S3 client object and assemble request
			Aws::S3::S3Client s3;

			PutObjectRequest request;
			request.SetBucket(TCHAR_TO_ANSI(*m_target.BucketName));
			request.SetKey(TCHAR_TO_ANSI(*m_target.ObjectKey));
			request.SetContentType(TCHAR_TO_ANSI(*m_target.ContentType));

			// create fstream
			std::string l_file_path = std::string(TCHAR_TO_UTF8(*m_file_path));
			std::shared_ptr<Aws::FStream> input_data = Aws::MakeShared<Aws::FStream>("MVFileAllocationTag", l_file_path.c_str(), std::ios_base::in | std::ios_base::binary);

			request.SetBody(input_data);

			// issue the put request
			const PutObjectOutcome outcome = s3.PutObject(request);

			// If we have a completion handler, execute it on the game thread like guaranteed in the interface
			if (m_completion_delegate.IsBound())
			{

				FGraphEventRef game_thread_task
					= FFunctionGraphTask::CreateAndDispatchWhenReady(
						[outcome, handler{ this->m_completion_delegate }, object_key{ this->m_target.ObjectKey }]{

					if (outcome.IsSuccess())
					{
						UE_LOG(LogMVAWS, Display, TEXT("Upload of object '%s' complete"), *object_key);
						handler.Execute(true, object_key);
					}
					else
					{
						UE_LOG(LogMVAWS, Error, TEXT("Upload of object '%s' failed: %s"), *object_key,
								UTF8_TO_TCHAR(outcome.GetError().GetMessage().c_str()));
						handler.Execute(false, object_key);
					}

				}, TStatId(), NULL, ENamedThreads::GameThread);
			}
			else
			{
				// Otherwise issue the error directly to the logs
				if (!outcome.IsSuccess())
				{
					UE_LOG(LogMVAWS, Error, TEXT("Upload of object '%s' to bucket '%s' failed: %s"), *m_target.ObjectKey, *m_target.BucketName,
						UTF8_TO_TCHAR(outcome.GetError().GetMessage().c_str()));
				}
				else
				{
					UE_LOG(LogMVAWS, Display, TEXT("Upload of object '%s' to bucket '%s' complete"), *m_target.ObjectKey, *m_target.BucketName);
				}
			}

			// begin x-ray trace of this command. This is called a subsegment, which is later assembled to a segment
			if (!subseg_id.IsEmpty())
			{
				IMVAWSModule::Get().end_trace_subsegment(m_trace_id, subseg_id);
			}

			const long long end_time = epoch_milliseconds();
			IMVAWSModule::Get().count_file_upload(static_cast<float>(end_time - start_time));
		}

		FORCEINLINE TStatId GetStatId() const
		{
			RETURN_QUICK_DECLARE_CYCLE_STAT(FileUploadAsyncTask, STATGROUP_ThreadPoolAsyncTasks);
		}

	private:
		friend class FAutoDeleteAsyncTask<FileUploadAsyncTask>;

		const FS3UploadTarget         m_target;
		const FString                 m_file_path;
		const FString                 m_trace_id;
		const FOnCacheUploadFinished  m_completion_delegate;
};

} // anon ns

void US3Impl::set_default_bucket_name(const FString &n_bucket_name) 
{
	m_default_bucket_name = n_bucket_name;
}

bool US3Impl::cache_upload(const FS3UploadTarget &n_target, TUniquePtr<unsigned char[]> &&n_data,
		const size_t n_size, const FString &n_trace_id, const FOnCacheUploadFinished n_completion) 
{
	FS3UploadTarget target = n_target;
	if (target.BucketName.IsEmpty()) 
	{
		target.BucketName = m_default_bucket_name;
	}

	if (target.BucketName.IsEmpty()) 
	{
		UE_LOG(LogMVAWS, Error, TEXT("Need a bucket name to upload to cache. Plz configure AWSConnectionConfig actor"));
		return false;
	}
	
	if (n_target.ObjectKey.IsEmpty()) 
	{
		UE_LOG(LogMVAWS, Error, TEXT("Need an object name to upload to cache."));
		return false;
	}

	if (!n_data || !n_size) 
	{
		UE_LOG(LogMVAWS, Warning, TEXT("No data, no upload to S3 cache."));
		return false;
	}

	UE_LOG(LogMVAWS, Display, TEXT("Upload of %u bytes to cache bucket '%s' initiating."), n_size, *m_default_bucket_name);

	// This looks like a memleak but really isn't as the task is self-owned and will delete itself when done.
	// AWS clients also come with a built in thread executor which could handle this use case.
	// However, I am using Unreal's Async task mechanism here to better integrate with the engine
	// and to be able to post on the game thread without any unforseen complications.
	(new FAutoDeleteAsyncTask<MembufUploadAsyncTask>(target, MoveTemp(n_data), n_size, n_trace_id, n_completion))->StartBackgroundTask();

	return true;
}


bool US3Impl::cache_upload(const FS3UploadTarget &n_target, const FString &n_file_path, 
		const FString &n_trace_id, const FOnCacheUploadFinished n_completion)
{
	FS3UploadTarget target = n_target;
	if (target.BucketName.IsEmpty())
	{
		target.BucketName = m_default_bucket_name;
	}

	if (target.BucketName.IsEmpty())
	{
		UE_LOG(LogMVAWS, Error, TEXT("Need a bucket name to upload to cache. Plz configure AWSConnectionConfig actor"));
		return false;
	}

	if (n_target.ObjectKey.IsEmpty())
	{
		UE_LOG(LogMVAWS, Error, TEXT("Need an object name to upload to cache."));
		return false;
	}

	

	if (n_file_path.IsEmpty())
	{
		UE_LOG(LogMVAWS, Warning, TEXT("file path is empty, no upload to S3 cache."));
		return false;
	}

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if(!PlatformFile.FileExists(*n_file_path))
	{
		UE_LOG(LogMVAWS, Warning, TEXT("file does not exist, no upload to S3 cache."));
		return false;
	}

	UE_LOG(LogMVAWS, Display, TEXT("Upload of %s to cache bucket '%s' initiating."), *n_file_path, *target.BucketName);

	(new FAutoDeleteAsyncTask<FileUploadAsyncTask>(target, n_file_path, n_trace_id, n_completion))->StartBackgroundTask();

	return true;
}
