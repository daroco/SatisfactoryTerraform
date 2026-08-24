#include "STFRegistrySubsystem.h"

#include "Buildables/FGBuildable.h"
#include "Subsystem/SubsystemActorManager.h"

namespace
{
	/** How far a live instance may sit from a record's saved location and
	  * still be considered the same buildable, in centimetres. Absorbs the
	  * float32 quantization the save round-trip applies to transforms
	  * (~0.02cm at map-scale coordinates) while staying far below the 800cm
	  * spacing of even the smallest foundation grid. */
	constexpr double LocationToleranceCm = 1.0;
}

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
	// NOTE: there is deliberately no `if (RuntimeRef.IsValid()) return true;`
	// fast path. FLightweightBuildableInstanceRef::IsValid() only checks that
	// the instance's array slot resolves to a non-null pointer, and the
	// subsystem does not shrink those arrays on removal - it calls
	// FRuntimeBuildableInstanceData::Clear() and recycles the slot later
	// (see mBuildableClassToEmptyIndices). So a ref to a dismantled instance
	// keeps reporting IsValid(), which made the API insist a hand-dismantled
	// foundation still existed and broke drift detection entirely. Clear()
	// nulls BuiltWithRecipe, so IsValidOnLoad() is the honest liveness check
	// and every resolve below goes through it.
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

	// Identity is what we saved ourselves: class + location. The array index
	// is NOT identity - it shifts across loads and gets recycled after
	// removals - so it is only ever a hint that must re-verify.
	const FVector SavedLocation = Record.Transform.GetLocation();
	const auto IsOurs = [&](int32 Index)
	{
		const FRuntimeBuildableInstanceData& Data = (*Instances)[Index];
		return Data.IsValidOnLoad() && Data.Transform.GetLocation().Equals(SavedLocation, LocationToleranceCm);
	};

	if (Instances->IsValidIndex(Record.RuntimeIndexHint) && IsOurs(Record.RuntimeIndexHint))
	{
		Record.RuntimeRef.Initialize(LightweightSubsystem, Record.BuildableClass, Record.RuntimeIndexHint);
		return true;
	}

	// Full scan. Bind only when exactly one live instance matches: zero means
	// it is genuinely gone (dismantled in-game) and more than one means we
	// cannot tell them apart. Both fail closed -> 404 -> Terraform plans a
	// recreate. Guessing instead would eventually delete the wrong buildable,
	// which is exactly how co-located duplicates cost real foundations once.
	int32 Found = INDEX_NONE;
	int32 MatchCount = 0;
	for (int32 Index = 0; Index < Instances->Num(); ++Index)
	{
		if (!IsOurs(Index))
		{
			continue;
		}
		Found = Index;
		if (++MatchCount > 1)
		{
			break;
		}
	}

	if (MatchCount != 1)
	{
		Record.RuntimeIndexHint = INDEX_NONE;
		return false;
	}

	Record.RuntimeIndexHint = Found;
	Record.RuntimeRef.Initialize(LightweightSubsystem, Record.BuildableClass, Found);
	return true;
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
