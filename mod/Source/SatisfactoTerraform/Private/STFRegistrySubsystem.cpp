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

void ASTFRegistrySubsystem::RegisterLightweight(const FString& TFID, const FLightweightBuildableInstanceRef& Ref)
{
	LightweightBuildables.Add(TFID, Ref);
}

void ASTFRegistrySubsystem::Unregister(const FString& TFID)
{
	Buildables.Remove(TFID);
	LightweightBuildables.Remove(TFID);
	ConnectionEndpoints.Remove(TFID);
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

const FLightweightBuildableInstanceRef* ASTFRegistrySubsystem::FindLightweight(const FString& TFID) const
{
	const FLightweightBuildableInstanceRef* Found = LightweightBuildables.Find(TFID);
	if (!Found || !Found->IsValid())
	{
		return nullptr;
	}
	return Found;
}

TArray<ASTFRegistrySubsystem::FEntry> ASTFRegistrySubsystem::GetAll() const
{
	TArray<FEntry> Out;
	for (const auto& Pair : Buildables)
	{
		if (IsValid(Pair.Value))
		{
			FEntry Entry;
			Entry.TFID = Pair.Key;
			Entry.Buildable = Pair.Value;
			Out.Add(MoveTemp(Entry));
		}
	}
	for (const auto& Pair : LightweightBuildables)
	{
		if (Pair.Value.IsValid())
		{
			FEntry Entry;
			Entry.TFID = Pair.Key;
			Entry.LightweightRef = Pair.Value;
			Out.Add(MoveTemp(Entry));
		}
	}
	return Out;
}

void ASTFRegistrySubsystem::RegisterConnectionEndpoints(const FString& TFID, const FSTFConnectionEndpoints& Endpoints)
{
	ConnectionEndpoints.Add(TFID, Endpoints);
}

const FSTFConnectionEndpoints* ASTFRegistrySubsystem::FindConnectionEndpoints(const FString& TFID) const
{
	return ConnectionEndpoints.Find(TFID);
}
