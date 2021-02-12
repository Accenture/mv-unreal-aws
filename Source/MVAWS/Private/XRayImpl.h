// (c) 2020 by Mackevision, All rights reserved

#pragma once

#include "CoreMinimal.h"
#include "HAL/Thread.h"
#include "Dom/JsonObject.h"

#include "XRayImpl.generated.h"

namespace Aws 
{
	namespace XRay
	{
		class XRayClient;
	}
}

/*!
 * Implementation for XRay tracing functions
 */
UCLASS()
class UXRayImpl : public UObject 
{

	GENERATED_BODY()

	public:
		void BeginDestroy() override;

		/*!
		* start tracing a segment
		* \param n_trace_id 
		* \param n_segment_name 
		* \return trace identifier
		*/
		UFUNCTION(BlueprintCallable, Category = "XRayImpl")
		FString start_trace_segment(const FString &n_trace_id, const FString &n_segment_name);

		/*!
		* start tracing a segment
		* \param n_trace_id of the parent trace
		* \param n_segment_name 
		* \return subsegment_id that has to be used to end the subsegment
		*/
		UFUNCTION(BlueprintCallable, Category = "XRayImpl")
		FString start_trace_subsegment(const FString &n_trace_id, const FString &n_name);

		/*!
		* end tracing a segment
		* \param n_trace_id of the parent trace
		* \param n_subsegment_id of the subsegment to end
		* \param n_error adds the "fault" field to the segment to indicate that an error occurred 
		*/
		UFUNCTION(BlueprintCallable, Category = "XRayImpl")
		void end_trace_subsegment(const FString &n_trace_id, const FString n_subsegment_id, const bool n_error = false);

		/*!
		* end tracing a segment
		* \param n_trace_id 
		*/
		UFUNCTION(BlueprintCallable, Category = "XRayImpl")
		void end_trace_segment(const FString &n_trace_id, const bool n_error);

	private:
		using DocumentPtr = TSharedPtr<FJsonObject>;
		/*!
		* a map of currently active segments by trace ID
		*/
		using DocumentMap = TMap<FString, DocumentPtr>; 

		using SubSegmentArray = TArray<DocumentPtr>;

		/*!
		* a map of subsegments to be sent ref'ed by trace ID
		*/
		using SubSegmentMap = TMap<FString, SubSegmentArray>;

		/*!
		* This is pooled with a thread executor so we can re-use it everywhere
		*/
		Aws::XRay::XRayClient  *m_xray;

		/*!
		* complete documents we have in flight
		*/
		DocumentMap             m_segments;

		/*!
		* and their subsegments
		*/
		SubSegmentMap           m_subsegments;

		static FCriticalSection s_mutex;       //Those have to be static in Unreal. Why?
};
