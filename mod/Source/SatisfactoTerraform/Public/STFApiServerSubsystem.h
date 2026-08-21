#pragma once

#include "CoreMinimal.h"
#include "Subsystem/ModSubsystem.h"
#include "HttpResultCallback.h"
#include "HttpServerRequest.h"
#include "HttpRouteHandle.h"
#include "Containers/Queue.h"
#include "STFApiServerSubsystem.generated.h"

class IHttpRouter;

/**
 * Hosts the SatisfactoTerraform HTTP API (see api/openapi.yaml in the repo).
 *
 * Threading model: FHttpServerModule invokes route handlers on the game
 * thread during its tick, but every world mutation is still funneled through
 * a single Apply() helper so ordering is explicit and the pattern stays
 * correct if the listener is ever moved off-thread.
 *
 * Only runs where there is authority (host / dedicated server).
 */
UCLASS()
class SATISFACTOTERRAFORM_API ASTFApiServerSubsystem : public AModSubsystem
{
	GENERATED_BODY()

public:
	ASTFApiServerSubsystem();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

	/** Listen port; SML mod config can override. */
	UPROPERTY(EditDefaultsOnly, Category = "SatisfactoTerraform")
	int32 Port = 8090;

	/** Optional bearer token; empty disables auth (localhost use). */
	UPROPERTY(EditDefaultsOnly, Category = "SatisfactoTerraform")
	FString Token;

private:
	TSharedPtr<IHttpRouter> Router;
	TArray<FHttpRouteHandle> Routes;

	void BindRoutes();
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
	TSharedPtr<FJsonObject> SpawnConnection(const TSharedPtr<FJsonObject>& Body, int32& OutStatus, FString& OutError);

	/** Resolve a short class name like "Build_ConstructorMk1_C" to a UClass. */
	UClass* ResolveBuildableClass(const FString& ClassName, FString& OutError) const;
};
