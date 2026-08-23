#include "STFRegistrySubsystem.h"

#include "Buildables/FGBuildable.h"
#include "Subsystem/SubsystemActorManager.h"
#include "UObject/UnrealType.h"

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

	// A connection's own tf_id was just handled above; this instead prunes
	// any OTHER connection whose From/To still points at the buildable
	// being deleted here, so it doesn't keep resolving to a dead tf_id on
	// GET forever. The real game allows dismantling a connected buildable
	// (the belt/wire itself just goes along for the ride or dangles), so
	// this only cleans up our own bookkeeping - it doesn't try to prevent
	// the deletion the way the mock's fake 409 does.
	TArray<FString> StaleConnectionIDs;
	for (const auto& Pair : ConnectionEndpoints)
	{
		if (Pair.Value.FromTFID == TFID || Pair.Value.ToTFID == TFID)
		{
			StaleConnectionIDs.Add(Pair.Key);
		}
	}
	for (const FString& StaleID : StaleConnectionIDs)
	{
		ConnectionEndpoints.Remove(StaleID);
	}
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

bool ASTFRegistrySubsystem::RevalidateLightweightRef(FLightweightBuildableInstanceRef& Ref) const
{
	if (Ref.IsValid())
	{
		return true;
	}
	AFGLightweightBuildableSubsystem* LightweightSubsystem = AFGLightweightBuildableSubsystem::Get(GetWorld());
	if (!LightweightSubsystem)
	{
		return false;
	}
	static FIntProperty* IDProp = FindFProperty<FIntProperty>(FLightweightBuildableInstanceRef::StaticStruct(), TEXT("LightweightBuildableID"));
	if (!IDProp)
	{
		return false;
	}
	const int32 ID = IDProp->GetPropertyValue_InContainer(&Ref);
	Ref.Initialize(LightweightSubsystem, Ref.GetBuildableClass(), ID);
	return Ref.IsValid();
}

const FLightweightBuildableInstanceRef* ASTFRegistrySubsystem::FindLightweight(const FString& TFID)
{
	FLightweightBuildableInstanceRef* Found = LightweightBuildables.Find(TFID);
	if (!Found || !RevalidateLightweightRef(*Found))
	{
		return nullptr;
	}
	return Found;
}

TArray<ASTFRegistrySubsystem::FEntry> ASTFRegistrySubsystem::GetAll()
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
	for (auto& Pair : LightweightBuildables)
	{
		if (RevalidateLightweightRef(Pair.Value))
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
