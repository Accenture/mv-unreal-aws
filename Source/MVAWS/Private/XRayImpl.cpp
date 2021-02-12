// (c) 2020 by Mackevision, All rights reserved

#include "XRayImpl.h"
#include "Utils.h"
#include "JsonObjectConverter.h"
#include "Serialization/JsonSerializer.h"

// Engine
#include "Json/Public/Policies/CondensedJsonPrintPolicy.h"
#include "Async/AsyncWork.h"

// AWS SDK
#include "Windows/PreWindowsApi.h"
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/core/utils/logging/DefaultLogSystem.h>
#include <aws/core/utils/threading/Executor.h>
#include <aws/xray/XRayClient.h>
#include <aws/xray/XRayRequest.h>
#include <aws/xray/model/PutTraceSegmentsRequest.h>
#include <aws/xray/model/PutTraceSegmentsResult.h>
#include "Windows/PostWindowsApi.h"

// Std
#include <string>
#include <chrono>
#include <random>

using namespace Aws::XRay::Model;

FCriticalSection UXRayImpl::s_mutex;

void UXRayImpl::BeginDestroy() 
{

	if (m_xray) {
		Aws::Delete(m_xray);
		m_xray = nullptr;
	}

	Super::BeginDestroy();
};

namespace 
{
	inline FString random_id() 
	{

		std::random_device rd;
		std::mt19937 mt(rd());
		std::uniform_real_distribution<double> dist(0, std::numeric_limits<uint64>::max());
		return FString::Printf(TEXT("%016x"), dist(mt));
	}
}

FString UXRayImpl::start_trace_segment(const FString &n_trace_id, const FString &n_segment_name) 
{

	DocumentPtr trace_segment = MakeShareable(new FJsonObject);

	if (!m_xray) 
	{
		// Using a pool executor is supposed to render the client thread safe
		Aws::Client::ClientConfiguration client_config;
		client_config.enableEndpointDiscovery = use_endpoint_discovery();
		const FString xray_endpoint = readenv(TEXT("MVAWS_XRAY_ENDPOINT"));
		if (!xray_endpoint.IsEmpty()) {
			client_config.endpointOverride = TCHAR_TO_UTF8(*xray_endpoint);
		}

		client_config.executor = Aws::MakeShared<Aws::Utils::Threading::PooledThreadExecutor>("xray", 2);
		m_xray = Aws::New<Aws::XRay::XRayClient>("xray", client_config);
	}

	{
		FScopeLock slock(&s_mutex);
		m_segments.Emplace(n_trace_id, trace_segment);
		m_subsegments.Emplace(n_trace_id, SubSegmentArray{});
	}
	
	// See here for scheme
	// https://docs.aws.amazon.com/xray/latest/devguide/xray-api-segmentdocuments.html
	trace_segment->SetStringField(TEXT("name"), n_segment_name);
	trace_segment->SetStringField(TEXT("trace_id"), n_trace_id);
	trace_segment->SetStringField(TEXT("origin"), TEXT("AWS::EC2::Instance"));

	// Now each document needs to contain a segment ID.
	// I am now assuming a random id is needed but I don't know
	const FString id = random_id();
	trace_segment->SetStringField(TEXT("id"), id);

	// the request begins now, seconds since epoch in milli precision
	trace_segment->SetNumberField(TEXT("start_time"), epoch_millis());

	return id;
}

FString UXRayImpl::start_trace_subsegment(const FString &n_trace_id, const FString &n_name) 
{

	DocumentPtr subsegment = MakeShareable(new FJsonObject);

	// the request begins now, seconds since epoch in milli precision
	subsegment->SetNumberField(TEXT("start_time"), epoch_millis());

	subsegment->SetStringField(TEXT("name"), n_name);
	subsegment->SetStringField(TEXT("namespace"), TEXT("remote"));
	const FString id = random_id();
	subsegment->SetStringField(TEXT("id"), id);

	FScopeLock slock(&s_mutex);
	m_subsegments[n_trace_id].Add(subsegment);

	return id;
}

void UXRayImpl::end_trace_subsegment(const FString &n_trace_id, const FString n_subsegment_id, const bool n_error) 
{

	FScopeLock slock(&s_mutex);
	SubSegmentArray &subsegments = m_subsegments[n_trace_id];

	// Find our subsegment by ID in the array
	const int32 i = subsegments.IndexOfByPredicate([n_subsegment_id](const DocumentPtr &n_subseg) {
		FString id;
		if (!n_subseg->TryGetStringField(TEXT("id"), id)) {
			return false;
		}
		return id.Equals(n_subsegment_id);
	});

	if (i == INDEX_NONE) {
		UE_LOG(LogMVAWS, Warning, TEXT("User code tried to end subsegment tracing for a segment that doesn't exist"));
		return;
	}

	const DocumentPtr subsegment = subsegments[i];

	subsegment->SetBoolField(TEXT("in_progress"), false);
	if (n_error) {
		subsegment->SetBoolField(TEXT("fault"), true);
	}

	// the request ends now
	subsegment->SetNumberField(TEXT("end_time"), epoch_millis());
}

void UXRayImpl::end_trace_segment(const FString &n_trace_id, const bool n_error) 
{
	
	DocumentPtr trace_segment;

	{
		FScopeLock slock(&s_mutex);
		trace_segment = m_segments[n_trace_id];
		checkf(trace_segment, TEXT("invalid trace document id"));

		// Gather all subsegments and put them in
		TArray<TSharedPtr<FJsonValue> > subsegment_array;
		for (const DocumentPtr &s : m_subsegments[n_trace_id]) {
			subsegment_array.Add(MakeShareable(new FJsonValueObject(s)));
		}

		if (subsegment_array.Num()) {
			trace_segment->SetArrayField("subsegments", subsegment_array);
		}

		// delete subsegments we had in store as we can't have any new incoming
		m_subsegments.Remove(n_trace_id);
	}

	// the request ends now
	trace_segment->SetNumberField(TEXT("end_time"), epoch_millis());
	trace_segment->SetBoolField(TEXT("in_progress"), false);

	if (n_error) {
		trace_segment->SetBoolField(TEXT("fault"), true);
	}

	FString json;
	TSharedRef< TJsonWriter< TCHAR, TCondensedJsonPrintPolicy<TCHAR> > > Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR> >::Create(&json);
	FJsonSerializer::Serialize(trace_segment.ToSharedRef(), Writer);

	{
		FScopeLock slock(&s_mutex);
		m_segments.Remove(n_trace_id);
	}

	if (m_xray)
	{
		PutTraceSegmentsRequest request;
		request.AddTraceSegmentDocuments(TCHAR_TO_UTF8(*json));
		
		PutTraceSegmentsOutcome oc = m_xray->PutTraceSegments(request);
		if (oc.IsSuccess()) 
		{
			if (oc.GetResult().GetUnprocessedTraceSegments().size()) 
			{
				UE_LOG(LogMVAWS, Warning, TEXT("Unprocessed segments while sending X-Ray document."));
			}	
		} 
		else 
		{
			// This never seems to happen. It's a quick UDP send. Normally the error will be reported by unprocessed segments
			UE_LOG(LogMVAWS, Warning, TEXT("Failed to send X-Ray document: %s"), UTF8_TO_TCHAR(oc.GetError().GetMessage().c_str()));
		}
	}
	else
	{
		UE_LOG(LogMVAWS, Display, TEXT("Envionment variable MVAWS_XRAY_ENABLED not found or empty, upload of XRay segments is disabled."));
	}
}
