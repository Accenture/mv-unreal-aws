/* (c) 2021 by Mackevision Mackevision Medien Design GmbH
 * Licensed under the Apache License, Version 2.0.
 * See attached file LICENSE for full details
 */
#include "Utils.h"

#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "HTTP/Public/HttpModule.h"

#include <cstdlib>

FString readenv(const FString &n_env_variable_name, const FString &n_default) {

	char *buf = nullptr;
	size_t sz = 0;
	if (_dupenv_s(&buf, &sz, TCHAR_TO_UTF8(*n_env_variable_name)) == 0 && buf != nullptr) {
		return UTF8_TO_TCHAR(buf);
	}

	return n_default;
}

namespace {

bool true_or_false_env(const FString &n_env_name, const bool n_default = false) {
	
	const FString env{ readenv(n_env_name) };

	if (env.Equals("True", ESearchCase::IgnoreCase)) {
		return true;
	}

	if (env.Equals("False", ESearchCase::IgnoreCase)) {
		return false;
	}

	return n_default;
}

} // anon ns


bool use_endpoint_discovery() {

	return true_or_false_env(TEXT("MVAWS_ENABLE_ENDPOINT_DISCOVERY"));
}

bool cloudwatch_metrics_enabled(const bool n_default) {

	return true_or_false_env(TEXT("MVAWS_CLOUDWATCH_METRICS"), n_default);
}

bool cloudwatch_logs_enabled(const bool n_default) {

	return true_or_false_env(TEXT("MVAWS_CLOUDWATCH_LOGS"), n_default);
}

bool xray_enabled(const bool n_default) {

	return true_or_false_env(TEXT("MVAWS_ENABLE_XRAY"), n_default);
}

FString get_instance_id()
{
	FString instance_id{ TEXT("UnknownInstance") };

	const float timout_before_call = FHttpModule::Get().GetHttpTimeout();
	FHttpModule::Get().SetHttpTimeout(4.0);
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> md_request = FHttpModule::Get().CreateRequest();
	md_request->SetVerb("GET");
	md_request->SetURL("http://169.254.169.254/latest/meta-data/instance-id");

	// There seems to be no sync Http request in this module. Mock it by using a future.
	TSharedPtr<TPromise<bool>, ESPMode::ThreadSafe> rp = MakeShareable<TPromise<bool> >(new TPromise<bool>());
	TFuture<bool> return_future = rp->GetFuture();

	md_request->OnProcessRequestComplete().BindLambda(
		[rp, &instance_id](FHttpRequestPtr /*Request*/, FHttpResponsePtr n_response, bool n_success)
	{
		if (!n_success)
		{
			rp->SetValue(false);
			return;
		}

		if (n_response->GetResponseCode() != 200)
		{
			rp->SetValue(false);
			return;
		}

		instance_id = n_response->GetContentAsString();
		rp->SetValue(true);
	});

	md_request->ProcessRequest();

	return_future.WaitFor(FTimespan::FromSeconds(6.0));
	const bool result = return_future.Get();

	FHttpModule::Get().SetHttpTimeout(timout_before_call);

	return instance_id;
}
