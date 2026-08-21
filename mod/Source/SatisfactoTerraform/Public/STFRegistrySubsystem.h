#pragma once

#include "CoreMinimal.h"
#include "Subsystem/ModSubsystem.h"
#include "FGSaveInterface.h"
#include "STFRegistrySubsystem.generated.h"

class AFGBuildable;

/**
 * Maps Terraform-assigned IDs (tf_id) to the actors the mod spawned for them.
 *
 * The map is marked SaveGame so the mapping survives save/load — that is what
 * makes `terraform plan` against a reloaded session see no drift. Actors that
 * were dismantled in-game resolve to null on lookup, which the API layer
 * reports as 404 (Terraform then plans a recreate).
 */
UCLASS()
class SATISFACTOTERRAFORM_API ASTFRegistrySubsystem : public AModSubsystem, public IFGSaveInterface
{
	GENERATED_BODY()

public:
	static ASTFRegistrySubsystem* Get(UWorld* World);

	void Register(const FString& TFID, AFGBuildable* Buildable);
	void Unregister(const FString& TFID);

	/** Returns nullptr if the id is unknown or the actor no longer exists. */
	AFGBuildable* Find(const FString& TFID) const;

	/** All live tf_id -> buildable pairs (dead entries pruned). */
	TMap<FString, AFGBuildable*> GetAll() const;

	// IFGSaveInterface: persist the registry in the save game.
	virtual bool ShouldSave_Implementation() const override { return true; }

private:
	UPROPERTY(SaveGame)
	TMap<FString, AFGBuildable*> Buildables;
};
