/* (c) 2021 by Mackevision Mackevision Medien Design GmbH
 * Licensed under the Apache License, Version 2.0.
 * See attached file LICENSE for full details
 */
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "Templates/UniquePtr.h"
#include "Templates/SharedPointer.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMVAWS, Log, All);

/// First parameter is success, second is name of object
DECLARE_DELEGATE_TwoParams(FOnCacheUploadFinished, bool, FString);

/** An SQS queue message that came in to be handled or disregarded
 */
struct FMVAWSMessage {

	/// internally used
	FString m_message_id;

	/**
	 * use this to delete (acknowledge reception) the message
	 */
	FString m_receipt;

	/**
	 * age is in milliseconds
	 */
	uint32  m_message_age;

	/**
	 * is set when contained in the message response.
	 * This can be used as trace_id for start_trace_segment() and related functions 
	 * to measure steps along the way of this message being processed
	 */
	FString m_xray_header;

	/**
	 * message body
	 */
	FString m_body;
};

using SQSReturnPromise = TPromise<bool>;
using SQSReturnPromisePtr = TSharedPtr<SQSReturnPromise, ESPMode::ThreadSafe>;

/// First parameter is message body
/// Second parameter is a promise the delegate must fulfill. If it's set to true, the message will be deleted
DECLARE_DELEGATE_TwoParams(FOnSQSMessageReceived, FMVAWSMessage, SQSReturnPromisePtr);


/**
 * S3 upload destination info
 */
struct FS3UploadTarget {

	/**
	 * The bucket to upload into
	 * If not set, will default to the Config Actor's settings.
	 * Either BucketName will be taken or the Env override.
	 */
	FString BucketName;

	/**
	 * Full object key including suffix
	 */
	FString ObjectKey;

	/**
	 * S3 objects can have content type. It is important to set this for
	 * compatibility when serving the content using CloudFront.
	 * This is given in the Content-Type HTTP header
	 */
	FString ContentType = TEXT("image/jpg");
};

class MVAWS_API IMVAWSModule : public IModuleInterface 
{
	public:

		/*!
		* Virtual destructor.
		*/
		virtual ~IMVAWSModule() = default;

		/*!
		* Upload the buffer (presumably image data) to the configured AWS bucket.
		* Will assume ownership over the data buffer and call delete[] when it's done.
		* Each upload, when complete, will cause OnCacheUploadFinished delegate to fire
		* in the game thread, in which the caller can react.
		*
		* \param n_target destination information for the content
		* \param n_data buffer with raw data. Will take ownership. Use MoveTemp() to move a buffer in here
		* \param n_size the size of data to be transferred
		* \param n_trace_id if set, call will be measured as a X-Ray subsegment. Must be opened before
		* \param n_completion an optional delegate which will execute on the game thread when upload is complete.
		*		The delegate will not fire when the function returned false. Leave empty if not needed.
		*		If the S3 upload is the last call in a chain, you can use this to finalize the X-Ray trace
		* \return true when operation was successfully started. Doesn't mean it finished. See n_completion for this
		*/
		virtual bool cache_upload(const FS3UploadTarget &n_target, TUniquePtr<unsigned char[]> &&n_data,
				const size_t n_size, const FString &n_trace_id = FString{},
				const FOnCacheUploadFinished n_completion = FOnCacheUploadFinished{}) = 0;

		/*!
		* Upload the file to the configured AWS bucket.
		* Each upload, when complete, will cause OnCacheUploadFinished delegate to fire
		* in the game thread, in which the caller can react.
		*
		* \param n_target destination information for the content
		* \param n_file_path absolute path to the file to upload
		* \param n_trace_id if set, call will be measured as a X-Ray subsegment. Must be opened before
		* \param n_completion an optional delegate which will execute on the game thread when upload is complete.
		*		The delegate will not fire when the function returned false. Leave empty if not needed.
		*		If the S3 upload is the last call in a chain, you can use this to finalize the X-Ray trace
		* \return true when operation was successfully started. Doesn't mean it finished. See n_completion for this
		*/
		virtual bool cache_upload(const FS3UploadTarget &n_target, const FString &n_file_path,
			const FString &n_trace_id = FString{}, const FOnCacheUploadFinished n_completion = FOnCacheUploadFinished{}) = 0;
		
		/** @defgroup SQS functions
		 * @{
		 */

		/**
		 * @brief Start polling the Q given in property QueueURL or environment.
		 * Delegate will be called for each message on the game thread.
		 * 
		 * @param n_delegate In the delegate handler, a return promise is given in. The implementing
		 * function is responsible for setting the value of this promise.
		 * If set to true, the message will be deleted from the Q. If set to false, not.
		 * Either way, once the promise is set, polling continues.
		 * 
		 */
		virtual bool start_sqs_poll(FOnSQSMessageReceived &&n_delegate) = 0;

		/// Stop polling. Blocks until thread ís joined.
		virtual void stop_sqs_poll() = 0;
		
		//! @}


		/*! \defgroup XRay tracing functions
		 * @{
		 */

		/*!
		* start a new logical segment as part of a trace
		* \param n_trace_id assuming you already have a trace id given,
		*		which you intend to add segments to, this is it.
		* \param n_segment_name a name for the complete segment
		* \return the new id that was assigned to this document
		*/
		virtual FString start_trace_segment(const FString &n_trace_id, const FString &n_segment_name) = 0;

		/*!
		* start a subsegment of a trace
		* \param n_trace_id assuming you already have a trace id given,
		*		which you intend to add segments to, this is it.
		* Each named subsegment is recorded as part of an existing segment
		* \return the id that was assigned to this subsegment, use with end_trace_subsegment()
		*/
		virtual FString start_trace_subsegment(const FString &n_trace_id, const FString &n_name) = 0;

		/*!
		* end a subsegment (which records its duration)
		* \param n_trace_id the trace id this subsegment is part of
		* \param n_subsegment_id as given when you started the subsegment, cannot be closed twice
		* \param n_error set to true if you want to make the subsegment appear with an error flag
		*/
		virtual void end_trace_subsegment(const FString &n_trace_id, const FString n_subsegment_id, const bool n_error = false) = 0;

		/*!
		* finalize the trace by sending it to X-Ray. This closes the segment and measures its duration
		* \param n_trace_id will be invalid after this call
		* \param n_error set to true if you want to make the segment appear with an error flag
		*/
		virtual void end_trace_segment(const FString &n_trace_id, const bool n_error = false) = 0;
		
		//! @}

		/*! \defgroup CloudWatch metrics
		 * @{
		 */
		
		/*! \brief register one render operation with total time
		 *  will return immediately and queue for sending with the next batch
		 */
		virtual void count_image_rendered(const float n_milliseconds) noexcept = 0;
		
		/*! \brief register one S3 membuf (image) upload operation
		 *  will return immediately and queue for sending with the next batch
		 */
		virtual void count_membuf_upload(const float n_milliseconds) noexcept = 0;

		/*! \brief register one S3 file (video) upload operation
		 *  will return immediately and queue for sending with the next batch
		 */
		virtual void count_file_upload(const float n_milliseconds) noexcept = 0;
		
		/*! \brief register one received SQS message
		 *  will return immediately and queue for sending with the next batch
		 */
		virtual void count_sqs_message() noexcept = 0;

		//! @}

		/*!
		 * Gets a reference to the search module instance.
		 *
		 * \return A reference to the IMVAWSModule module.
		 */
		static IMVAWSModule &Get() 
		{
			static const FName ModuleName = "MVAWS";
			return FModuleManager::LoadModuleChecked<IMVAWSModule>(ModuleName);
		}
};

