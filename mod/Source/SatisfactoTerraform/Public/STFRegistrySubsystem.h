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
class SATISFACTOTERRAFORM_API ASTFRegistrySubsystem : public AModSubsystem, public IFGSaveInterface
{
	GENERATED_BODY()

public:
	static ASTFRegistrySubsystem* Get(UWorld* World);

	void Register(const FString& TFID, AFGBuildable* Buildable);
	void RegisterLightweight(const FString& TFID, const FLightweightBuildableInstanceRef& Ref);
	void Unregister(const FString& TFID);

	/** Returns nullptr if the id is unknown, tracked as a lightweight instead, or the actor no longer exists. */
	AFGBuildable* Find(const FString& TFID) const;

	/** Returns nullptr if the id is unknown, tracked as a full actor instead, or the instance is no longer valid. */
	const FLightweightBuildableInstanceRef* FindLightweight(const FString& TFID) const;

	/** True if TFID resolves to a live entry in either representation - use for the POST duplicate-id (409) check. */
	bool Contains(const FString& TFID) const { return Find(TFID) != nullptr || FindLightweight(TFID) != nullptr; }

	/** One resolved entry from GetAll(): exactly one of Buildable/LightweightRef is meaningful, per IsLightweight(). */
	struct FEntry
	{
		FString TFID;
		AFGBuildable* Buildable = nullptr;
		FLightweightBuildableInstanceRef LightweightRef;
		bool IsLightweight() const { return Buildable == nullptr; }
	};

	/** All live tf_id entries across both representations (dead ones pruned). */
	TArray<FEntry> GetAll() const;

	/** Connections (belts/power lines) share the same tf_id namespace and
	  * actor/lightweight tracking as buildables (see Register/RegisterLightweight/
	  * Find/FindLightweight above) - this just records which two buildables+
	  * connectors a given connection tf_id joins, alongside that tracking. */
	void RegisterConnectionEndpoints(const FString& TFID, const FSTFConnectionEndpoints& Endpoints);
	const FSTFConnectionEndpoints* FindConnectionEndpoints(const FString& TFID) const;

	// IFGSaveInterface: persist the registry in the save game.
	virtual bool ShouldSave_Implementation() const override { return true; }

private:
	UPROPERTY(SaveGame)
	TMap<FString, AFGBuildable*> Buildables;

	UPROPERTY(SaveGame)
	TMap<FString, FLightweightBuildableInstanceRef> LightweightBuildables;

	UPROPERTY(SaveGame)
	TMap<FString, FSTFConnectionEndpoints> ConnectionEndpoints;
};
