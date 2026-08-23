#pragma once

#include "CoreMinimal.h"
#include "Subsystem/ModSubsystem.h"
#include "HttpResultCallback.h"
#include "HttpServerRequest.h"
#include "HttpRouteHandle.h"
#include "Containers/Queue.h"
#include "STFApiServerSubsystem.generated.h"

class IHttpRouter;
class AFGBuildable;

/**
 * Hosts the SatisfactoryTerraform HTTP API (see api/openapi.yaml in the repo).
 *
 * Threading model: FHttpServerModule invokes route handlers on the game
 * thread during its tick, but every world mutation is still funneled through
 * a single Apply() helper so ordering is explicit and the pattern stays
 * correct if the listener is ever moved off-thread.
 *
 * Only runs where there is authority (host / dedicated server).
 */
UCLASS()
class SATISFACTORYTERRAFORM_API ASTFApiServerSubsystem : public AModSubsystem
{
	GENERATED_BODY()

public:
	ASTFApiServerSubsystem();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

	/** Listen port; SML mod config can override. */
	UPROPERTY(EditDefaultsOnly, Category = "SatisfactoryTerraform")
	int32 Port = 8090;

	/** Optional bearer token; empty disables auth (localhost use). */
	UPROPERTY(EditDefaultsOnly, Category = "SatisfactoryTerraform")
	FString Token;

private:
	/** Routes are bound to the process-lifetime router exactly once (see
	  * BindRoutesOnce) and dispatch through this: the subsystem instance for
	  * the currently loaded session. Set in BeginPlay, cleared in EndPlay;
	  * handlers 503 while it's unset (main menu, mid-load). Never unbinding
	  * sidesteps a live-confirmed IHttpRouter quirk where re-registering a
	  * ":param" route template after a same-process save switch leaves it
	  * permanently unmatched (issue #3). */
	static TWeakObjectPtr<ASTFApiServerSubsystem> ActiveInstance;
	static bool bRoutesBound;

	TSharedPtr<IHttpRouter> Router;

	void BindRoutesOnce();
	bool CheckAuth(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) const;

	// Route handlers.
	bool HandleHealth(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleWorld(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleBuildables(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleBuildableByID(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleConnections(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleConnectionByID(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	// World mutations (game thread only).
	TSharedPtr<FJsonObject> SpawnBuildable(const TSharedPtr<FJsonObject>& Body, int32& OutStatus, FString& OutError);
	TSharedPtr<FJsonObject> PatchBuildable(AFGBuildable* Buildable, const TSharedPtr<FJsonObject>& Body, const FString& TFID, int32& OutStatus, FString& OutError);
	TSharedPtr<FJsonObject> SpawnConnection(const TSharedPtr<FJsonObject>& Body, int32& OutStatus, FString& OutError);

	/** Dismantle via IFGDismantleInterface if implemented, else a plain Destroy(). */
	void DismantleBuildable(AFGBuildable* Buildable) const;

	/** Resolve a short class name like "Build_ConstructorMk1_C" to a UClass. */
	UClass* ResolveBuildableClass(const FString& ClassName, FString& OutError) const;
	/** Resolve a short class name like "Recipe_IronPlate_C" to a UClass. */
	UClass* ResolveRecipeClass(const FString& ClassName, FString& OutError) const;
	/** Resolve a short class name like "Build_ConveyorBeltMk1_C" or "Build_PowerLine_C" to a UClass. */
	UClass* ResolveConnectionClass(const FString& ClassName, FString& OutError) const;

	/**
	 * Resolve any short class name to a UClass, requiring it derive from
	 * ExpectedBase. Blueprint classes (everything Build_*_C / Recipe_*_C is)
	 * aren't necessarily loaded yet, so this falls back to an asset-registry
	 * name index (built lazily, once, on first miss) rather than only
	 * checking already-loaded classes.
	 */
	UClass* ResolveClassByName(const FString& ClassName, UClass* ExpectedBase, FString& OutError) const;

	/** name (without _C) -> the Blueprint asset's generated class path. Built lazily. */
	mutable TMap<FString, FSoftObjectPath> ClassNameIndex;
	mutable bool bClassNameIndexBuilt = false;
	void BuildClassNameIndex() const;
};
