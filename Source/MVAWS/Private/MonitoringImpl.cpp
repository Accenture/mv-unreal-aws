/* (c) 2021 by Mackevision Mackevision Medien Design GmbH
 * Licensed under the Apache License, Version 2.0.
 * See attached file LICENSE for full details
 */
#include "MonitoringImpl.h"
#include "Utils.h"

// Engine
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "HTTP/Public/HttpModule.h"
#include "Misc/OutputDevice.h"
#include "Async/AsyncWork.h"

// AWS SDK
#include "Windows/PreWindowsApi.h"
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/monitoring/CloudWatchClient.h>
#include <aws/monitoring/model/PutMetricDataRequest.h>

#include "Windows/PostWindowsApi.h"

// Std
#include <string>

using namespace Aws::CloudWatch::Model;
using Aws::CloudWatch::CloudWatchError;
using Aws::CloudWatch::CloudWatchErrors;

void UMonitoringImpl::start_metrics()
{
	stop_metrics();
	join();

	m_metrics_interrupted.Store(false);

	Aws::Client::ClientConfiguration client_config;
	client_config.enableEndpointDiscovery = use_endpoint_discovery();
	const FString cw_endpoint = readenv(TEXT("MVAWS_CLOUDWATCH_ENDPOINT"));
	if (!cw_endpoint.IsEmpty())
	{
		client_config.endpointOverride = TCHAR_TO_UTF8(*cw_endpoint);
	}

	m_cw_client = MakeShareable<Aws::CloudWatch::CloudWatchClient>(new Aws::CloudWatch::CloudWatchClient(client_config));

	m_metrics_thread = MakeUnique<FThread>(TEXT("AWS_Metrics"), [this] { this->metrics_thread(); });
}

void UMonitoringImpl::stop_metrics() noexcept
{
	// we are running
	if (m_metrics_thread)
	{
		UE_LOG(LogMVAWS, Display, TEXT("Shutting down Metrics send thread"));
		m_metrics_interrupted.Store(true);
	}
}

void UMonitoringImpl::join() noexcept
{
	if (m_metrics_thread)
	{
		UE_LOG(LogMVAWS, Display, TEXT("Joining Metrics thread"));
		m_metrics_interrupted.Store(true);
		m_metrics_thread->Join();
		m_metrics_thread.Reset();
	}

	m_cw_client.Reset();
}

void UMonitoringImpl::metrics_thread() noexcept
{
	m_instance_id = get_instance_id();

	while (!m_metrics_interrupted)
	{
		// Don't wanna block for 10 secs during teardown.
		// Hence a little makeshift sleep_interruptable()
		unsigned int sleep_time = 10;
		while (sleep_time--)
		{
			if (m_metrics_interrupted)
			{
				return;
			}

			FPlatformProcess::Sleep(1.0);
		}

		if (m_single_values.IsEmpty() && !m_sqs_messages)
		{
			send_blank();
		}
		else
		{
			send_values();
		}
	}
}

void UMonitoringImpl::send_blank() noexcept
{
	Aws::CloudWatch::Model::Dimension iid_dimension;
	iid_dimension.SetName("InstanceId");
	iid_dimension.SetValue(TCHAR_TO_UTF8(*m_instance_id));

	// CloudWatch specifically recommends to not have gaps in your values
	// and send metrics even when nothing happened. This way your application
	// looks alive when not busy. I will follow that advise but only for SQS messages
	// as it seems to me sending zero render times might mess up scaling calculations
	// along the way
	Aws::CloudWatch::Model::PutMetricDataRequest request;
	request.SetNamespace("MVAWS/TRAFFIC");

	// Let's add the number of SQS messages first
	Aws::CloudWatch::Model::MetricDatum sqs_datum;
	sqs_datum.SetMetricName("SQS_MESSAGES_RECEIVED");
	sqs_datum.SetUnit(StandardUnit::Count);
	sqs_datum.SetValue(0);
	sqs_datum.AddDimensions(iid_dimension);
	request.AddMetricData(std::move(sqs_datum));

	const PutMetricDataOutcome outcome = m_cw_client->PutMetricData(request);
	if (!outcome.IsSuccess())
	{
		UE_LOG(LogMVAWS, Warning, TEXT("Failed to put blank sample metric data: %s"),
			UTF8_TO_TCHAR(outcome.GetError().GetMessage().c_str()));
	}
	else
	{
		UE_LOG(LogMVAWS, Verbose, TEXT("Successfully put blank sample metric data"));
	}
}


void UMonitoringImpl::send_values() noexcept
{
	Aws::CloudWatch::Model::Dimension iid_dimension;
	iid_dimension.SetName("InstanceId");
	iid_dimension.SetValue(TCHAR_TO_UTF8(*m_instance_id));

	while (!m_single_values.IsEmpty())
	{
		Aws::CloudWatch::Model::PutMetricDataRequest request;
		request.SetNamespace("MVAWS/TRAFFIC");

		// Let's add the number of SQS messages first
		Aws::CloudWatch::Model::MetricDatum sqs_datum;
		sqs_datum.SetMetricName("SQS_MESSAGES_RECEIVED");
		sqs_datum.SetUnit(StandardUnit::Count);
		sqs_datum.SetValue(m_sqs_messages.Exchange(0));
		sqs_datum.AddDimensions(iid_dimension);
		request.AddMetricData(std::move(sqs_datum));
		
		// 20 is the maximum data points in a request.
		// There's still a size limit but I don't know how to query it
		// beforehand. I go for lower data points here as we already 
		// counted SQS messages
		unsigned int max_data_points = 19;

		// One by one, pop render entries. You know the drill...
		UMonitoringImpl::single_entry se;

		while (m_single_values.Dequeue(se) && max_data_points--)
		{
			Aws::CloudWatch::Model::MetricDatum datum;
			datum.SetMetricName(se.m_metric_name);
			datum.SetUnit(se.m_unit);
			datum.SetValue(se.m_value);
			datum.AddDimensions(iid_dimension);
			request.AddMetricData(std::move(datum));
		}

		const PutMetricDataOutcome outcome = m_cw_client->PutMetricData(request);
		if (!outcome.IsSuccess())
		{
			UE_LOG(LogMVAWS, Warning, TEXT("Failed to put sample metric data: %s"),
					UTF8_TO_TCHAR(outcome.GetError().GetMessage().c_str()));
		}
		else
		{
			UE_LOG(LogMVAWS, Verbose, TEXT("Successfully put sample metric data"));
		}
	}
}

void UMonitoringImpl::count_image_rendered(const float n_milliseconds) noexcept
{
	// Early exit in case we are not running.
	// No need to fill that Q
	if (m_metrics_interrupted) {
		return;
	}

	UMonitoringImpl::single_entry se;
	se.m_unit = StandardUnit::Milliseconds;
	se.m_metric_name = "RENDER_TIME";
	se.m_value = n_milliseconds;

	m_single_values.Enqueue(MoveTemp(se));
}

void UMonitoringImpl::count_membuf_s3_upload(const float n_milliseconds) noexcept
{
	if (m_metrics_interrupted) {
		return;
	}

	UMonitoringImpl::single_entry se;
	se.m_unit = StandardUnit::Milliseconds;
	se.m_metric_name = "MEMBUF_UPLOAD";
	se.m_value = n_milliseconds;

	m_single_values.Enqueue(MoveTemp(se));
}

void UMonitoringImpl::count_file_s3_upload(const float n_milliseconds) noexcept
{
	if (m_metrics_interrupted) {
		return;
	}

	UMonitoringImpl::single_entry se;
	se.m_unit = StandardUnit::Milliseconds;
	se.m_metric_name = "FILE_UPLOAD";
	se.m_value = n_milliseconds;

	m_single_values.Enqueue(MoveTemp(se));
}

void UMonitoringImpl::count_sqs_message() noexcept
{
	m_sqs_messages++;
}
