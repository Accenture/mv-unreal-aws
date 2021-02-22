/* (c) 2021 by Mackevision Mackevision Medien Design GmbH
 * Licensed under the Apache License, Version 2.0.
 * See attached file LICENSE for full details
 */
#pragma once

#include "CoreMinimal.h"

#include <chrono>

inline long long epoch_milliseconds() {

	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

inline double epoch_millis() {

	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count() / 1000.0;
}

/**
 * @brief read env by using _dupenv_s
 * @param n_env_variable_name read this environment variable
 * @param n_default return this default value if env is not found
 */
FString readenv(const FString &n_env_variable_name, const FString &n_default = FString{});

/**
 * @brief Read env variable MVAWS_ENABLE_ENDPOINT_DISCOVERY to determine if we should use endpoint discovery
 * @return defaults to false, true if env set
 */
bool use_endpoint_discovery();

/**
 * @brief Read env variable MVAWS_CLOUDWATCH_METRICS to determine if we should activate CloudWatch metrics
 * @return defaults to n_default, true if env set to True
 */
bool cloudwatch_metrics_enabled(const bool n_default);

/**
 * @brief Read env variable MVAWS_CLOUDWATCH_LOGS to determine if we should activate CloudWatch metrics
 * @return defaults to n_default, true if env set to True
 */
bool cloudwatch_logs_enabled(const bool n_default);

/**
 * @brief Read env variable MVAWS_ENABLE_XRAY to determine if we should activate XRay tracing
 * @return defaults to n_default, true if env set to True
 */
bool xray_enabled(const bool n_default);

/** @brief Query the AWS metadata server to get the EC2 Instance ID.
 * Synchronous.
 * If not on EC, will get "UnknownInstance" and time out after 4 secs
 */
FString get_instance_id();
