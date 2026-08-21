#include "STFRegistrySubsystem.h"

#include "Buildables/FGBuildable.h"
#include "Subsystem/SubsystemActorManager.h"

ASTFRegistrySubsystem* ASTFRegistrySubsystem::Get(UWorld* World)
{
	USubsystemActorManager* Manager = World->GetSubsystem<USubsystemActorManager>();
	check(Manager);
	return Manager->GetSubsystemActor<ASTFRegistrySubsystem>();
}

void ASTFRegistrySubsystem::Register(const FString& TFID, AFGBuildable* Buildable)
{
	Buildables.Add(TFID, Buildable);
}

void ASTFRegistrySubsystem::Unregister(const FString& TFID)
{
	Buildables.Remove(TFID);
}

AFGBuildable* ASTFRegistrySubsystem::Find(const FString& TFID) const
{
	AFGBuildable* const* Found = Buildables.Find(TFID);
	if (!Found || !IsValid(*Found))
	{
		return nullptr;
	}
	return *Found;
}

TMap<FString, AFGBuildable*> ASTFRegistrySubsystem::GetAll() const
{
	TMap<FString, AFGBuildable*> Out;
	for (const auto& Pair : Buildables)
	{
		if (IsValid(Pair.Value))
		{
			Out.Add(Pair.Key, Pair.Value);
		}
	}
	return Out;
}
