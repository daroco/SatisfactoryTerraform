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

	// A ref deserialized from the save has a dead OwnerSubsystem weak
	// pointer, and its saved LightweightBuildableID is NOT trustworthy
	// either: that id is just an index into the subsystem's per-class
	// instance array, which the game rebuilds on load, so indices can
	// shift (confirmed live, issue #2: foundations vanished from the API
	// after a relaunch while still standing in the world - and an
	// id-based re-Initialize could silently bind a *different* tile).
	// The identity that survives the round trip is what the ref itself
	// saved: buildable class + transform. Re-find the instance by those.
	// GetAllLightweightBuildableInstances is a header-inline accessor, so
	// unlike most of this class it works without the stub-source caveat.
	AFGLightweightBuildableSubsystem* LightweightSubsystem = AFGLightweightBuildableSubsystem::Get(GetWorld());
	const TSubclassOf<AFGBuildable> BuildableClass = Ref.GetBuildableClass();
	if (!LightweightSubsystem || !BuildableClass)
	{
		return false;
	}
	const TArray<FRuntimeBuildableInstanceData>* Instances =
		LightweightSubsystem->GetAllLightweightBuildableInstances().Find(BuildableClass);
	if (!Instances)
	{
		return false;
	}

	const FVector SavedLocation = Ref.GetBuildableTransform().GetLocation();
	for (int32 Index = 0; Index < Instances->Num(); ++Index)
	{
		const FRuntimeBuildableInstanceData& Data = (*Instances)[Index];
		// Removed instances keep their array slot as a hole; IsValidOnLoad
		// (header-inline) filters those. Two same-class instances can't
		// coexist within 1cm, so first location match is THE instance.
		if (!Data.IsValidOnLoad() || !Data.Transform.GetLocation().Equals(SavedLocation, 1.0f))
		{
			continue;
		}
		Ref.Initialize(LightweightSubsystem, BuildableClass, Index);
		return Ref.IsValid();
	}
	// No match: the instance is genuinely gone (dismantled in-game while we
	// weren't looking) - surface as 404 so Terraform plans a recreate.
	return false;
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
