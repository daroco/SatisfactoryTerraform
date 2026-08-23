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
	FSTFLightweightRecord Record;
	Record.BuildableClass = Ref.GetBuildableClass();
	Record.Transform = Ref.GetBuildableTransform();
	Record.RuntimeRef = Ref;
	LightweightBuildables.Add(TFID, MoveTemp(Record));
}

bool ASTFRegistrySubsystem::RegisterLightweightByIdentity(const FString& TFID, TSubclassOf<AFGBuildable> BuildableClass, const FTransform& Transform)
{
	FSTFLightweightRecord Record;
	Record.BuildableClass = BuildableClass;
	Record.Transform = Transform;
	// Same resolve used across session boundaries: find the live instance
	// of this class within 1cm and point RuntimeRef at it.
	if (!RevalidateLightweightRecord(Record))
	{
		return false;
	}
	LightweightBuildables.Add(TFID, MoveTemp(Record));
	return true;
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

bool ASTFRegistrySubsystem::RevalidateLightweightRecord(FSTFLightweightRecord& Record) const
{
	// The cached ref is session-local (Transient - persisting the engine
	// ref round-trips as an empty struct, see the header). Valid means it
	// was resolved earlier this session and the instance still exists.
	if (Record.RuntimeRef.IsValid())
	{
		return true;
	}

	// Re-find the instance by the identity we saved ourselves: class +
	// location. The instance's index in the subsystem's per-class array is
	// NOT part of that identity - the game rebuilds those arrays on load,
	// so indices shift between sessions (issue #2).
	// GetAllLightweightBuildableInstances is a header-inline accessor, so
	// unlike most of this class it works without the stub-source caveat.
	AFGLightweightBuildableSubsystem* LightweightSubsystem = AFGLightweightBuildableSubsystem::Get(GetWorld());
	if (!LightweightSubsystem || !Record.BuildableClass)
	{
		return false;
	}
	const TArray<FRuntimeBuildableInstanceData>* Instances =
		LightweightSubsystem->GetAllLightweightBuildableInstances().Find(Record.BuildableClass);
	if (!Instances)
	{
		return false;
	}

	const FVector SavedLocation = Record.Transform.GetLocation();
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
		Record.RuntimeRef.Initialize(LightweightSubsystem, Record.BuildableClass, Index);
		return Record.RuntimeRef.IsValid();
	}
	// No match: the instance is genuinely gone (dismantled in-game while we
	// weren't looking) - surface as 404 so Terraform plans a recreate.
	return false;
}

const FLightweightBuildableInstanceRef* ASTFRegistrySubsystem::FindLightweight(const FString& TFID)
{
	FSTFLightweightRecord* Found = LightweightBuildables.Find(TFID);
	if (!Found || !RevalidateLightweightRecord(*Found))
	{
		return nullptr;
	}
	return &Found->RuntimeRef;
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
		if (RevalidateLightweightRecord(Pair.Value))
		{
			FEntry Entry;
			Entry.TFID = Pair.Key;
			Entry.LightweightRef = Pair.Value.RuntimeRef;
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
