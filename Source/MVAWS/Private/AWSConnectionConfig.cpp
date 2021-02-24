/* (c) 2021 by Mackevision Mackevision Medien Design GmbH
 * Licensed under the Apache License, Version 2.0.
 * See attached file LICENSE for full details
 */
#include "AWSConnectionConfig.h"
#include "MVAWS.h"
#include "Utils.h"

#include "Components/SceneComponent.h"
#include "Components/BillboardComponent.h"

AAWSConnectionConfig::AAWSConnectionConfig()
	: AActor() {

	PrimaryActorTick.bCanEverTick = false;
	SetActorEnableCollision(false);
	SetReplicates(false);

	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("RootComponent")));
	RootComponent->SetMobility(EComponentMobility::Static);

	static ConstructorHelpers::FObjectFinder<UTexture2D> aws_sprite(TEXT("/MVAWS/Decals/MVAWS_Logo.MVAWS_Logo"));
	if (aws_sprite.Succeeded()) {
		m_aws_icon_texture = aws_sprite.Object;

		// This creates a little in-Editor icon for the config so one can see it in scene
#if WITH_EDITORONLY_DATA
		m_sprite_component = CreateDefaultSubobject<UBillboardComponent>(TEXT("SpriteComponent"));
		m_sprite_component->SetMobility(EComponentMobility::Static);
		m_sprite_component->SetSprite(m_aws_icon_texture);
		m_sprite_component->AttachToComponent(RootComponent, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
#endif
	}
}

// Called when the game starts or when spawned
void AAWSConnectionConfig::BeginPlay() {

	Super::BeginPlay();

	// We notify our module we are in the world and ready to play
	FMVAWSModule *mod = FModuleManager::Get().GetModulePtr<FMVAWSModule>("MVAWS");

	if (!mod) 
	{
		UE_LOG(LogMVAWS, Warning, TEXT("I cannot work without that module. What's going on?"));
		return;
	}

	//get queue url from environment variable
	char* buf = nullptr;
	size_t sz = 0;

	if (!QueueUrlEnvVariableName.IsEmpty()) {

		const FString environmentValue = readenv(QueueUrlEnvVariableName);
		
		if (!environmentValue.IsEmpty()) {
			UE_LOG(LogMVAWS, Log, TEXT("Found Environment Variable '%s'. Use QueueURL '%s'"), *QueueUrlEnvVariableName, *environmentValue);
			QueueURL = environmentValue;
		} else {
			UE_LOG(LogMVAWS, Warning, TEXT("Environment Variable '%s' not found or empty! Used default QueueURL '%s'"), *QueueUrlEnvVariableName, *QueueURL);
		}
	}
	
	mod->init_actor_ready(this);
}


void AAWSConnectionConfig::EndPlay(const EEndPlayReason::Type n_reason) 
{
	FMVAWSModule* mod = FModuleManager::Get().GetModulePtr<FMVAWSModule>("MVAWS");

	// We notify our module that we have stopped and want to tear down
	if (mod) 
	{
		mod->init_actor_ready(nullptr);
	}
	
	Super::EndPlay(n_reason);
}
