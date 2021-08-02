# MV-Unreal-AWS
#### AWS utility functionality for Unreal Engine

## About
This is a plugin for the Unreal Engine which implements AWS connection
functionality for use cases involving remote rendering of images on AWS
EC2 instances. It is capable of uploading file and memory buffer data 
to a S3 bucket as well as receiving  and deleting messages from an SQS queue.
It also implements CloudWatch monitoring and logging, as well as XRay performance tracing.
It does not do any rendering or project specific variant switching
but can be used as an utility to that effect.

## Usage
### Unreal Engine
To use, clone this repository directly into the `Plugins`
folder of your Unreal Project. Like this:

```
cd /your/Unreal/Project
cd Plugins
git clone --depth 1 --recursive git@github.com:Accenture/mv-unreal-aws.git MVAWS
```

Re-create the Visual Studio project solution afterwards.
It may be necessary to 
explicitly load the plugin in your project's Plugins setting.
The plugin has no outside dependencies and includes a binary build
of the [AWS SDK for C++](https://aws.amazon.com/sdk-for-cpp/)

The plugin is meant to be used from C++ code and exposes
an interface called `IMVAWSModule`. Usage from Blueprints is not
supported right now.

Once the plugin is loaded, use it by creating an Actor of type 
`AAWSConnectionConfig` into your scene. The actor exposes a 
number of properties which are used to control the behavior.
All properties are read during startup phase and are considered
invariant thereafter. Most importantly:

* CloudWatchLogs - See below, log engine output to CloudWatch
* BucketName - Specify the bucket to upload content to
* QueueURL - the SQS Queue to receive messages from

### AWS
When used in the cloud (on an EC2 instance) the SDK 
requires permission for every action it takes. This includes:
* writing to S3
* writing to XRay
* writing to CloudWatch (including creation of log group)
* reading from and writing to SQS

Permissions are best supplied using a IAM role with the required
managed policies attached. This role is then attached to the EC2 instance.
Doing so, will enable `MVAWS` to perform the required actions without any
further configuration.

When testing locally, it is generally easiest to create a 
programmatic access capable user and supply credentials using the
`~/.aws/credentials` file. Using this mechanism, both S3 and SQS 
can be accessed over the internet. See 
[here for further information](https://docs.aws.amazon.com/cli/latest/userguide/cli-configure-files.html).

### Environment

The following environment variables can be used to control behavior of a deployed Unreal application.
Some override default settings in the `AAWSConnectionConfig` actor properties.

_MVAWS_STACK_NAME_:<br>
Will determine the CloudWatch log group.

_MVAWS_ENABLE_ENDPOINT_DISCOVERY_:<br>
Globally sets the "endpoint discovery" feature to true on all SDK client objects. Default is false.

_MVAWS_CLOUDWATCH_METRICS_:<br>
Set this to "True" or "False" to enable or disable cloudwatch metrics. Overrides config setting.

_MVAWS_CLOUDWATCH_LOGS_:<br>
Set this to "True" or "False" to enable or disable cloudwatch logs. Overrides config setting.

_MVAWS_CLOUDWATCH_ENDPOINT_:<br>
Force the cloudwatch client object to use this endpoint rather than the one discovered by private DNS.

_MVAWS_SQS_ENDPOINT_:<br>
Force the SQS client object to use this endpoint rather than the one discovered by private DNS.

_MVAWS_ENABLE_XRAY_:<br>
Set this to "True" or "False" to enable or disable xray tracing. Overrides config setting.

_MVAWS_XRAY_ENDPOINT_:<br>
Force the xray client object to use this endpoint rather than the one discovered by private DNS.

## CloudWatch
### Logs
The plugin is capable of sending all engine log output to CloudWatch.
This is very useful when spawning and running unsupervised render nodes. The log system
runs in a background thread and sends accumulated logs every 5 seconds. This will incur costs
as CloudWatch bills API calls. If no logs ocurred during the 5 second period, no 
API call is issued. Log severity is taken into account according to the engine.

To activate CloudWatch logging, set the property `CloudWatchLogs` in `AAWSConnectionConfig` 
actor to true (defaults to false). Leave deactivated if logs are not required or too costly.

Log group name is:

```
$CloudWatchLogGroupPrefix/$MVAWS_STACK_NAME/
```

Each running instance of the engine will create its own log stream. It is named as follows:

```
$year/$month/$day-$hour/$minute-$instance_id
```

### Metrics
Metrics too are sent in a background thread and incur costs. A few basic metrics are implemented, 
more can be added. They can be costly though. Activate metrics in the config actor using the property `CloudWatchMetrics`.
* FILE_UPLOAD    (milliseconds)
* MEMBUF_UPLOAD  (milliseconds)
* SQS_MESSAGES_RECEIVED (count)
* RENDER_TIME    (milliseconds) - must be implemented by user.

In order to measure render times, the caller must provide the time to be measured. Like this:

```C++
IMVAWSModule::Get().count_image_rendered(render_time_in_ms);
```

## S3
There are two methods needed to upload data to S3. 
They are asynchronous and return immediately but can be given a
handler delegate to execute code upon finish. One of these methods 
uploads data from a memory buffer, the other one expects a file and uploads 
contents therein. Both methods upload times are sent to CloudWatch metrics. 
They run asynchronously and support multiple operations at a time.

This uploads a memory buffer:

```C++
const size_t len = 1024 * 1024;
TUniquePtr<unsigned char[]> data = MakeUnique<unsigned char []>(len);

// fill buffer with data. Buffer ownership goes
// into the method

FS3UploadTarget t;
t.BucketName = TEXT("your-bucket-name-typically-comes-in-with-renderjob");
t.ObjectKey = TEXT("mostly-your-image-filepath.jpg");
t.ContentType = TEXT("image/jpg");    // Files in S3 have content types which will be considered by CloudFront

// start the upload process
IMVAWSModule::Get().cache_upload(t, MoveTemp(data), len);
```

Optionally you can specify a return handler which will be executed 
on the game thread once the operation completes. The handler will always be executed.

```C++
IMVAWSModule::Get().cache_upload(t, MoveTemp(data), len,
    FOnCacheUploadFinished::CreateLambda([](const bool n_success, const FString n_object) {
          check(IsInGameThread());

          if (n_success) {
              UE_LOG(LogRayStudio, Display, TEXT("'%s' uploaded"), *n_object);
          } else {
              UE_LOG(LogRayStudio, Error, TEXT("Upload of '%s' failed"), *n_object);
          }
    })
);
```

Uploading a file works much the same way but takes a filename on disk rather than a memory buffer.


## SQS
SQS usage can start during startup phase.
The plugin expects the Q to support long polling with a timeout of `LongPollWait` seconds. Defaults to 4. 
It uses a background thread that continuously long polls. This means, the thread 
will poll with a timeout of 4 seconds to retrieve exactly one message to be processed.
When the message was received it blocks until it is processed and then continue to cycle until stopped.

Business logic must implement a handler function for incoming
messages and acknowledge each received message using a provided thread safe
[return future](https://docs.unrealengine.com/en-US/API/Runtime/Core/Async/TFuture/__ctor/index.html).

```C++
// you need a handler function that will be called when an SQS message comes in

#include "MVAWS/Public/IMVAWS.h"

UCLASS()
class AMyRenderActor : public AActor {
   
    GENERATED_BODY()

    private:
        void OnSqsMsg(FMVAWSMessage n_message, SQSReturnPromisePtr n_promise);
}

void AMyRenderActor::OnSqsMsg(FMVAWSMessage n_message, SQSReturnPromisePtr n_promise) {

    // We are in the game thread here.
    n_message.m_body;        // contains your message
    n_message.m_xray_header; // can be used to trace your operations using XRay

    // Do all kinds of rendering magic. If leaving this stack, DO NOT LOSE n_promise
    // but always maintain ownership.
    // When done, call...
    n_promise->SetValue(true);

    // ... and the message will be implicity deleted from the Q (to acknowledge work)
}

// This is how to start the polling
IMVAWSModule::Get().start_sqs_poll(FOnSQSMessageReceived::CreateUObject(this, &AMyRenderActor::OnSqsMsg));

// stop it like this:
IMVAWSModule::Get().stop_sqs_poll();
```

The user must implement a delegate handler with these parameters.
Each message received will trigger the handler on the game thread.
A return promise is given into the handler and must be fulfilled by the caller.
Use the config property `SQSHandlerOnGameThread` to call the handler directly
in the loop and not post to the game thread. In this case the caller is responsible
for not calling engine logic that is not safe to be used outside the game thread.

The return promise must always be fulfilled for the polling process to continue.
Setting the value to false will cause new messages to come in.
The original one is ignored until visibility timeout is over.
Setting the value to true acknowledges the message has been completed successfully.
In this case, the message is automatically deleted from the Q and new messages will be received.
Again, after the promise has been set, polling continues. 
If the promise is lost and SetValue() is not called, polling will stall indefinitely.
PLEASE DO NOT DO THIS.

Also note that the long poll operation upon the SDK cannot be interrupted.
Therefore, in order to join the background thread, the plugin may
block for up to 5 seconds during shutdown.
If you chose to increase that timeout (e.g. to save money on SQS requests),
plugin teardown times will increase accordingly. Other modules use interruptible 
sleeps so this is your limiting factor when it comes to teardown times.

## X-Ray
You can use AWS X-Ray to trace commands that were received from the
SQS Queue or otherwise. Each received Q item contains an `m_xray_header`
property for that purpose, assuming cloud native code has already 
started to trace a client request.
The implementation is threadsafe and can be used from within
worker threads or asyncs.

To test this locally, one can use the [standalone X-Ray Daemon available
from AWS](https://docs.aws.amazon.com/xray/latest/devguide/xray-daemon.html).

Example usage:

```C++
// First start tracing a segment.
IMVAWSModule::Get().start_trace_segment(trace_id, TEXT("RenderJob"));

// Each individual substep is traced in subsegments
// which will appear underneath the parent segment
const FString sub1 = IMVAWSModule::Get().start_trace_subsegment(trace_id, TEXT("Variants"));

// do work with variants

// close the subsegment, which will measure the time it took
IMVAWSModule::Get().end_trace_subsegment(trace_id, sub1);

// start the next one
const FString sub2 = IMVAWSModule::Get().start_trace_subsegment(trace_id, TEXT("Render"));

// render work

// close the subsegment, which will measure the time it took
IMVAWSModule::Get().end_trace_subsegment(trace_id, sub2);

// measure a possible S3 upload as well to be included after the render
IMVAWSModule::Get().cache_upload(bucket_name, my_render_data,
	my_render_data_size, trace_id);

// finalize by closing the segment itself.
// This will upload to X-Ray.
IMVAWSModule::Get().end_trace_segment(trace_id);
```
Note that when receiving the trace from SQS, even though the 
trace header is present, the trace will still appear 
as a new client request in the AWS console.
This might be considered a known bug in the AWS system.
According to a few sources this is supposed to work but doesn't.

Also note that XRay trace calls in `end_trace_segment()` are 
synchronous. Meaning they block the current thread until 
finished. If the XRay client cannot connect to 
the appropriate backend (for example because it is in an 
isolated subnet without NAT) the application will stall 
until it times out. Use the config Actor's `XRayEnabled` 
property or the env override `MVAWS_ENABLE_XRAY` to control 
the behavior.

## AWS SDK
A note about the [AWS SDK](https://github.com/aws/aws-sdk-cpp).
An optimized release build (created with
Visual Studio 2017) was included as third party binary module
into the plugin. It was built from tag 1.8.131. Generally, updates 
to the SDK should only be necessary when a specific reason arises.
AWS tends to only include new APIs and new functionality to
the SDK and leaves existing implementation more or less stable.
So unless you have a reason, I suggest to stick with this build.

The AWS SDK is released under the 
[Apache 2.0 License](https://github.com/aws/aws-sdk-cpp/blob/master/LICENSE)
and included in binary form. It has not been modified.

## License

Copyright 2021 Mackevision Medien Design GmbH

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
