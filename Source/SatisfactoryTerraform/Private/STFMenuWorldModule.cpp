#include "STFMenuWorldModule.h"
#include "SatisfactoryTerraformModule.h"

#include "FGSaveSystem.h"
#include "FGSaveManagerInterface.h" // FSaveHeader, FLoadSaveFileParameters
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h" // GetEnvironmentVariable
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "TimerManager.h"

bool USTFMenuWorldModule::bAutoLoadConsumed = false;

USTFMenuWorldModule::USTFMenuWorldModule()
{
	bRootModule = true;
}

FString USTFMenuWorldModule::RequestedSave()
{
	FString SaveName;
	if (!FParse::Value(FCommandLine::Get(), TEXT("STFLoad="), SaveName))
	{
		SaveName = FPlatformMisc::GetEnvironmentVariable(TEXT("SATISFACTORY_TERRAFORM_LOAD"));
	}
	SaveName.TrimStartAndEndInline();
	return SaveName.TrimQuotes();
}

void USTFMenuWorldModule::DispatchLifecycleEvent(ELifecyclePhase Phase)
{
	Super::DispatchLifecycleEvent(Phase);
	if (Phase != ELifecyclePhase::POST_INITIALIZATION || bAutoLoadConsumed)
	{
		return;
	}
	const FString SaveName = RequestedSave();
	if (SaveName.IsEmpty())
	{
		return;
	}
	bAutoLoadConsumed = true;
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	// The menu needs a moment to finish its own initialisation (player
	// controller, online session) before a load request is honoured; the
	// delay is generous rather than tuned. Seen live: too early is ignored.
	UE_LOG(LogSatisfactoryTerraform, Log, TEXT("Auto-loading save '%s' once the menu settles"), *SaveName);
	World->GetTimerManager().SetTimer(AutoLoadTimer, FTimerDelegate::CreateWeakLambda(this, [this, SaveName]()
	{
		LoadSave(SaveName);
	}), 4.0f, false);
}

void USTFMenuWorldModule::LoadSave(const FString& SaveName)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	FString Path;
	if (!UFGSaveSystem::GetAbsolutePathForSaveGame(World, SaveName, Path))
	{
		UE_LOG(LogSatisfactoryTerraform, Error, TEXT("Auto-load: no save named '%s' found"), *SaveName);
		return;
	}

	// The proper path is the one the menu's own load button takes: parse the
	// file's header and hand it to the save system with the menu's player
	// controller. If anything in that chain is missing, fall back to the
	// level travel the game mode parses itself (AFGGameMode::LoadGameOption).
	TArray<uint8> Bytes;
	FSaveHeader Header;
	const bool bHaveHeader = FFileHelper::LoadFileToArray(Bytes, *Path) && UFGSaveSystem::ParseSaveGameHeader(SaveName, Bytes, Header);
	APlayerController* Player = World->GetFirstPlayerController();
	UFGSaveSystem* SaveSystem = UFGSaveSystem::Get(World);
	if (bHaveHeader && SaveSystem && Player)
	{
		FLoadSaveFileParameters Params;
		if (SaveSystem->LoadSaveFile(Header, Params, Player))
		{
			UE_LOG(LogSatisfactoryTerraform, Log, TEXT("Auto-load: loading '%s' (session '%s')"), *SaveName, *Header.SessionName);
			return;
		}
		UE_LOG(LogSatisfactoryTerraform, Warning, TEXT("Auto-load: the save system declined '%s'; travelling with ?loadgame instead"), *SaveName);
	}
	else
	{
		UE_LOG(LogSatisfactoryTerraform, Warning, TEXT("Auto-load: header=%d saveSystem=%d player=%d; travelling with ?loadgame instead"),
			bHaveHeader, SaveSystem != nullptr, Player != nullptr);
	}
	const FString Map = (bHaveHeader && !Header.MapName.IsEmpty()) ? Header.MapName : FString(TEXT("Persistent_Level"));
	UGameplayStatics::OpenLevel(World, FName(*Map), true, FString::Printf(TEXT("?loadgame=%s"), *SaveName));
}
