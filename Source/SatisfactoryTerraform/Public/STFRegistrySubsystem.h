#pragma once

#include "CoreMinimal.h"
#include "Subsystem/ModSubsystem.h"
#include "FGSaveInterface.h"
#include "FGLightweightBuildableSubsystem.h" // FLightweightBuildableInstanceRef
#include "STFRegistrySubsystem.generated.h"

class AFGBuildable;

/** From/to endpoint metadata for a belt or power line - the registry's
  * actor/lightweight maps track *what exists*, this tracks *what it connects*
  * (not derivable from the spawned actor itself). Mirrors api::Connection's
  * from/to shape (see api/openapi.yaml). */
USTRUCT()
struct FSTFConnectionEndpoints
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	FString FromTFID;

	UPROPERTY(SaveGame)
	int32 FromConnector = 0;

	UPROPERTY(SaveGame)
	FString ToTFID;

	UPROPERTY(SaveGame)
	int32 ToConnector = 0;
};

/** What the registry persists for a lightweight-tracked buildable.
  *
  * FLightweightBuildableInstanceRef itself CANNOT be persisted: none of its
  * members (BuildableClass/Transform/LightweightBuildableID) carry the
  * SaveGame flag, so a SaveGame-marked map of refs round-trips as empty
  * structs - the root cause behind every past failure of issue #2 (both
  * the id-reflection recovery and the first class+location recovery were
  * healing refs that had loaded back blank). So the identity that must
  * survive - class + where it is - lives in our own SaveGame-flagged
  * fields, and the engine ref is a session-local cache re-resolved from
  * them on first use each session. */
USTRUCT()
struct FSTFLightweightRecord
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	TSubclassOf<AFGBuildable> BuildableClass;

	UPROPERTY(SaveGame)
	FTransform Transform;

	/** Never saved (Transient); see RevalidateLightweightRecord. */
	UPROPERTY(Transient)
	FLightweightBuildableInstanceRef RuntimeRef;

	/** Index this record last resolved to, as a fast-path hint only - never
	  * trusted without re-verifying the instance there is still ours (the
	  * subsystem recycles slots). Session-local like RuntimeRef. */
	UPROPERTY(Transient)
	int32 RuntimeIndexHint = INDEX_NONE;
};

/**
 * Maps Terraform-assigned IDs (tf_id) to the things the mod spawned for them.
 *
 * Two representations, mutually exclusive per tf_id: a full AFGBuildable*
 * actor, or an FLightweightBuildableInstanceRef. The latter exists because
 * Satisfactory's Lightweight Buildable system destroys simple structural
 * buildables (foundations, walls, ramps, ...) shortly after spawning and
 * migrates them to a memory-efficient non-actor representation - keeping
 * them as full actors would work but costs real per-actor overhead (draw
 * calls, replication, memory) at scale, so the mod converts them itself at
 * spawn time (see STFApiServerSubsystem::SpawnBuildable) and tracks the
 * resulting ref instead. FLightweightBuildableInstanceRef is a UE-provided
 * USTRUCT explicitly documented as safe to store indefinitely without
 * worrying about the referenced instance's lifetime.
 *
 * Both maps are SaveGame so tracking survives save/load. Actors that were
 * dismantled in-game resolve to null on lookup (Find); lightweight
 * instances that were removed in-game resolve IsValid()==false
 * (FindLightweight) - both cases surface as 404 at the API layer, and
 * Terraform plans a recreate.
 */
UCLASS()
class SATISFACTORYTERRAFORM_API ASTFRegistrySubsystem : public AModSubsystem, public IFGSaveInterface
{
	GENERATED_BODY()

public:
	static ASTFRegistrySubsystem* Get(UWorld* World);

	void Register(const FString& TFID, AFGBuildable* Buildable);
	void RegisterLightweight(const FString& TFID, const FLightweightBuildableInstanceRef& Ref);

	/** Register a lightweight-tracked buildable from identity alone (class +
	  * transform), resolving the engine ref by scanning the lightweight
	  * subsystem for the live instance of that class within 1cm - used after
	  * the game itself converts a freshly spawned buildable inside BeginPlay
	  * (see SpawnBuildable). Returns false if no such instance exists. */
	bool RegisterLightweightByIdentity(const FString& TFID, TSubclassOf<AFGBuildable> BuildableClass, const FTransform& Transform);

	void Unregister(const FString& TFID);

	/** Returns nullptr if the id is unknown, tracked as a lightweight instead, or the actor no longer exists. */
	AFGBuildable* Find(const FString& TFID) const;

	/** Returns nullptr if the id is unknown, tracked as a full actor instead, or the instance is no longer valid
	  * (after re-resolving the session-local ref from the record's saved class+location - see
	  * RevalidateLightweightRecord). Non-const because that re-resolve writes back into the stored record. */
	const FLightweightBuildableInstanceRef* FindLightweight(const FString& TFID);

	/** True if TFID resolves to a live entry in either representation - use for the POST duplicate-id (409) check. */
	bool Contains(const FString& TFID) { return Find(TFID) != nullptr || FindLightweight(TFID) != nullptr; }

	/** One resolved entry from GetAll(): exactly one of Buildable/LightweightRef is meaningful, per IsLightweight(). */
	struct FEntry
	{
		FString TFID;
		AFGBuildable* Buildable = nullptr;
		FLightweightBuildableInstanceRef LightweightRef;
		bool IsLightweight() const { return Buildable == nullptr; }
	};

	/** All live tf_id entries across both representations (dead ones pruned). Non-const - see FindLightweight. */
	TArray<FEntry> GetAll();

	/** Connections (belts/power lines) share the same tf_id namespace and
	  * actor/lightweight tracking as buildables (see Register/RegisterLightweight/
	  * Find/FindLightweight above) - this just records which two buildables+
	  * connectors a given connection tf_id joins, alongside that tracking. */
	void RegisterConnectionEndpoints(const FString& TFID, const FSTFConnectionEndpoints& Endpoints);
	const FSTFConnectionEndpoints* FindConnectionEndpoints(const FString& TFID) const;

	// IFGSaveInterface: persist the registry in the save game.
	virtual bool ShouldSave_Implementation() const override { return true; }

private:
	/** Ensures Record.RuntimeRef points at a live instance in THIS
	  * session's AFGLightweightBuildableSubsystem, re-finding it by the
	  * record's saved class + location when the cached ref is dead (fresh
	  * session) or was never resolved. Returns false when no matching
	  * instance exists - genuinely gone (dismantled in-game), so the API
	  * should 404 and Terraform plans a recreate. See FSTFLightweightRecord
	  * for why recovery starts from our own saved fields, never from a
	  * persisted engine ref. */
	bool RevalidateLightweightRecord(FSTFLightweightRecord& Record) const;

	UPROPERTY(SaveGame)
	TMap<FString, AFGBuildable*> Buildables;

	UPROPERTY(SaveGame)
	TMap<FString, FSTFLightweightRecord> LightweightBuildables;

	UPROPERTY(SaveGame)
	TMap<FString, FSTFConnectionEndpoints> ConnectionEndpoints;
};
