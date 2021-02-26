/* (c) 2021 by Mackevision Mackevision Medien Design GmbH
 * Licensed under the Apache License, Version 2.0.
 * See attached file LICENSE for full details
 */
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "AWSConnectionConfig.generated.h"

/**
 * Placing this Actor in your persistent Level activates usage of the MVAWS
 * system and allow for configuration of basic parameters.
 */
UCLASS(Category = "MVAWS")
class MVAWS_API AAWSConnectionConfig : public AActor 
{
	GENERATED_BODY()

	public:
		AAWSConnectionConfig();

		void BeginPlay() override;
		void EndPlay(const EEndPlayReason::Type n_reason) override;

		UPROPERTY(EditAnywhere, Category = "AWSConnection")
		bool Active = true;

		/** 
		 * @brief Set to true to make the AWS SDK log.
		 * Log files will be generated in the current directory the application
		 * (or editor) is started in and begin with aws_sdk_
		 * Generally this shouldn't be required.
		 */
		UPROPERTY(EditAnywhere, Category = "MVAWS|General")
		bool AWSLogs = false;
	
		/**
		 * @brief Set to true to enable CloudWatch logs.
		 * All logs the engine produces are sent to CloudWatch and end up in
		 * a log group called $CloudWatchLogGroupPrefix/$instance_id
		 * Note that this will involve a CloudWatch API call every 5 seconds.
		 * Although happening in a background thread, this may incur costs.
		 * At startup time, you can set the environment variable MVAWS_CLOUDWATCH_LOGS
		 * to "True" or "False" to override this value.
		 */
		UPROPERTY(EditAnywhere, Category = "MVAWS|CloudWatch")
		bool CloudWatchLogs = false;
		
		/**
		 * @brief specify prefix for CloudWatch log group.
		 * $MVAWS_STACK_NAME and Instance ID will be read and appended.
		 * Please have it start and end with a '/' character.
		 */
		UPROPERTY(EditAnywhere, Category = "MVAWS|CloudWatch")
		FString CloudWatchLogGroupPrefix = TEXT("/mv/render-group/");
		
		/**
		 * @brief Set to true to enable CloudWatch metrics.
		 * A few basic metrics such as the number of received SQS messages
		 * or S3 upload times are implemented.
		 * Note that leaving this on will incur CloudWatch costs.
		 * At startup time, you can set the environment variable MVAWS_CLOUDWATCH_METRICS
		 * to "True" or "False" to override this value.
		 */
		UPROPERTY(EditAnywhere, Category = "MVAWS|CloudWatch")
		bool CloudWatchMetrics = false;

		/**
		 * @brief The name of the Environment Variable where the default bucket name
		 * for S3 uploads can be specified.
		 * If set, it overrides BucketName property. Here you can specify the name of the env.
		 */
		UPROPERTY(EditAnywhere, Category = "MVAWS|S3")
		FString BucketNameEnvVariableName = "MVAWS_BUCKET_NAME";

		/** 
		 * @brief The default bucket name to upload content to. 
		 * This one is used if not explicitly set in the UploadTarget. 
		 */
		UPROPERTY(EditAnywhere, Category = "MVAWS|S3")
		FString BucketName;

		/**
		 * @brief The name of the Environment Variable where the application tries to get the
		 * SQS queue url from, overrides the value defined in the QueueURL property
		 */
		UPROPERTY(EditAnywhere, Category = "MVAWS|SQS")
		FString QueueUrlEnvVariableName = "MVAWS_SQS_QUEUE_URL";

		/**
		 * @brief The default SQS queue url where we listen for messages coming in. 
		 * This url is used if no url was defined in the environment variable with the name
		 * defined in the property QueueUrlEnvVariableName.
		 */
		UPROPERTY(EditAnywhere, Category = "MVAWS|SQS")
		FString QueueURL;

		/**
		 * @brief For long polling, specify time to wait for messages (in seconds).
		 * This affects the time the SDK call for retrieving messages blocks until it loops.
		 * As those calls cannot be interrupted, the plugin has to wait for this to finish
		 * during teardown. This means, the lower the value, the faster the engine can
		 * shut down at the expense of more AWS SDK calls and therefore higher costs.
		 * 5 seconds should be a good start.
		 * 
		 * For unknown reasons, setting this to values of >~5 appears to be buggy on occasion.
		 * See here: https://github.com/aws/aws-sdk-cpp/issues/962
		 */
		UPROPERTY(EditAnywhere, Category = "MVAWS|SQS", Meta = (ClampMin = "1", ClampMax = "20"))
		int LongPollWait = 4;

		/**
		 * @brief set to true if you want SQS handler delegate to fire on game thread.
		 * It may be beneficial in certain use cases to not not wait for the next game tick but
		 * be able to react right away. If setting this to true, the handler will be called in
		 * MVAWS' polling thread. This means, special care must be taken when calling engine
		 * functionality. If unsure, leave true.
		 */
		UPROPERTY(EditAnywhere, Category = "MVAWS|SQS")
		bool SQSHandlerOnGameThread = true;

		/**
		 * @brief enable XRay tracing
		 * Be aware, this requires the plugin to be able to reach an XRay endpoint.
		 * In isolated subnets this might not be the case as XRay doesn't have Vpc Endpoints.
		 * At startup time, you can set the environment variable MVAWS_ENABLE_XRAY
		 * to "True" or "False" to override this value.
		 */
		UPROPERTY(EditAnywhere, Category = "MVAWS|XRay")
		bool XRayEnabled = false;

	private:
		/*!
		* A UBillboardComponent to hold AWS icon sprite
		*/
		UPROPERTY()
		class UBillboardComponent *m_sprite_component;

		/*! 
		* Icon sprite texture
		*/
		UPROPERTY()
		class UTexture2D *m_aws_icon_texture;
};
