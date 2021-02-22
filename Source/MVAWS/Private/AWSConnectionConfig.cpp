/* (c) 2021 by Mackevision Mackevision Medien Design GmbH
 * Licensed under the Apache License, Version 2.0.
 * See attached file LICENSE for full details
 */
#include "AWSConnectionConfig.h"
#include "MVAWS.h"

#include "Components/SceneComponent.h"
#include "Components/BillboardComponent.h"

AAWSConnectionConfig::AAWSConnectionConfig()
	: AActor()
{
	PrimaryActorTick.bCanEverTick = false;
	SetActorEnableCollision(false);
	SetReplicates(false);

	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("RootComponent")));
	RootComponent->SetMobility(EComponentMobility::Static);

	static ConstructorHelpers::FObjectFinder<UTexture2D> aws_sprite(TEXT("/MVAWS/Decals/AWSLogo.AWSLogo"));
	if (aws_sprite.Succeeded()) 
	{
		m_aws_icon_texture = aws_sprite.Object;

		// This creates a little in-Editor icon for the config so one can see it in scene
#if WITH_EDITORONLY_DATA
		m_sprite_component = CreateDefaultSubobject<UBillboardComponent>(TEXT("SpriteComponent"));
		m_sprite_component->SetMobility(EComponentMobility::Static);
		m_sprite_component->SetSprite(m_aws_icon_texture);
#endif
	}
}

// Called when the game starts or when spawned
void AAWSConnectionConfig::BeginPlay() 
{
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

	if(this->QueueUrlEnvVariableName.IsEmpty() == false)
	{
		FString environmentValue;
		if (_dupenv_s(&buf, &sz, TCHAR_TO_ANSI(*this->QueueUrlEnvVariableName)) == 0 && buf != nullptr)
		{
			environmentValue = buf;
			free(buf);
		}

		if (environmentValue.IsEmpty() == false)
		{
			UE_LOG(LogMVAWS, Log, TEXT("Found Environment Variable \"%s\". Use QueueURL \"%s\""), *this->QueueUrlEnvVariableName, *environmentValue);
			this->QueueURL = environmentValue;
		}
		else
		{
			UE_LOG(LogMVAWS, Warning, TEXT("Environment Variable \"%s\" not found or empty! Used default QueueURL \"%s\""), *this->QueueUrlEnvVariableName, *this->QueueURL);
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
