#include "STFApiServerSubsystem.h"

#include "SatisfactoryTerraformModule.h"
#include "STFRegistrySubsystem.h"

#include "Buildables/FGBuildable.h"
#include "Buildables/FGBuildableFactory.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "Buildables/FGBuildableConveyorBelt.h"
#include "Buildables/FGBuildableConveyorAttachment.h"
#include "Buildables/FGBuildableWire.h"
#include "FGBuildableSubsystem.h"
#include "FGRecipe.h"
#include "FGDismantleInterface.h"
#include "FGFactoryConnectionComponent.h"
#include "FGPowerConnectionComponent.h"
#include "Tests/FGBuildableSpawnStrategy_Spline.h"

#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "HttpServerResponse.h"
#include "HttpPath.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"
#include "Engine/Blueprint.h"

#include "FGLightweightBuildableSubsystem.h"

namespace
{
	TUniquePtr<FHttpServerResponse> JsonResponse(int32 Code, const TSharedPtr<FJsonObject>& Body)
	{
		FString Out;
		const auto Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);
		auto Response = FHttpServerResponse::Create(Out, TEXT("application/json"));
		Response->Code = static_cast<EHttpServerResponseCodes>(Code);
		return Response;
	}

	TUniquePtr<FHttpServerResponse> ErrorResponse(int32 Code, const FString& Message)
	{
		const TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("message"), Message);
		return JsonResponse(Code, Body);
	}

	TSharedPtr<FJsonObject> ParseBody(const FHttpServerRequest& Request)
	{
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
		const FString Raw(Converted.Length(), Converted.Get());
		TSharedPtr<FJsonObject> Json;
		const auto Reader = TJsonReaderFactory<>::Create(Raw);
		FJsonSerializer::Deserialize(Reader, Json);
		return Json;
	}

	/** The :tf_id path parameter, parsed by IHttpRouter from a route
	  * registered like "/api/v1/buildables/:tf_id" (see BindRoutesOnce). */
	FString PathID(const FHttpServerRequest& Request)
	{
		return Request.PathParams.FindRef(TEXT("tf_id"));
	}

	TSharedPtr<FJsonObject> BuildableToJson(const FString& TFID, const AFGBuildable* Buildable)
	{
		const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("tf_id"), TFID);
		Json->SetStringField(TEXT("class"), Buildable->GetClass()->GetName());
		const TSharedPtr<FJsonObject> Transform = MakeShared<FJsonObject>();
		const FVector Loc = Buildable->GetActorLocation();
		Transform->SetNumberField(TEXT("x"), Loc.X);
		Transform->SetNumberField(TEXT("y"), Loc.Y);
		Transform->SetNumberField(TEXT("z"), Loc.Z);
		Transform->SetNumberField(TEXT("yaw"), Buildable->GetActorRotation().Yaw);
		Json->SetObjectField(TEXT("transform"), Transform);

		// Foundations/belts/etc. aren't production machines and have no
		// recipe/clock_speed - the API contract makes both fields optional.
		// GetCurrentRecipe/SetRecipe live on AFGBuildableManufacturer (which
		// derives from AFGBuildableFactory, so GetPendingPotential is reached
		// through the same cast) - confirmed against the real FactoryGame
		// header, not AFGBuildableFactory itself.
		if (const AFGBuildableManufacturer* Manufacturer = Cast<AFGBuildableManufacturer>(Buildable))
		{
			if (const TSubclassOf<UFGRecipe> Recipe = Manufacturer->GetCurrentRecipe())
			{
				Json->SetStringField(TEXT("recipe"), Recipe->GetName());
			}
			Json->SetNumberField(TEXT("clock_speed"), Manufacturer->GetPendingPotential());
		}
		return Json;
	}

	/** Same shape as BuildableToJson, for buildables tracked as a lightweight
	  * instance instead of a full actor. Lightweight-eligible classes never
	  * have a recipe/clock_speed (only AFGBuildableManufacturer does, and
	  * manufacturers are never lightweight-eligible), so those fields are
	  * simply omitted, same as BuildableToJson does for non-manufacturers. */
	TSharedPtr<FJsonObject> LightweightToJson(const FString& TFID, const FLightweightBuildableInstanceRef& Ref)
	{
		const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("tf_id"), TFID);
		Json->SetStringField(TEXT("class"), Ref.GetBuildableClass()->GetName());
		const TSharedPtr<FJsonObject> Transform = MakeShared<FJsonObject>();
		const FTransform T = Ref.GetBuildableTransform();
		const FVector Loc = T.GetLocation();
		Transform->SetNumberField(TEXT("x"), Loc.X);
		Transform->SetNumberField(TEXT("y"), Loc.Y);
		Transform->SetNumberField(TEXT("z"), Loc.Z);
		Transform->SetNumberField(TEXT("yaw"), T.Rotator().Yaw);
		Json->SetObjectField(TEXT("transform"), Transform);
		return Json;
	}

	/** The API's "connector" is a zero-based index into a buildable's factory
	  * connection components, in the canonical order the game itself sorts
	  * them (UFGFactoryConnectionComponent::SortComponentList - the same
	  * ordering used by e.g. FOR_EACH_FACTORY_CONNECTION). Returns nullptr if
	  * out of range. */
	UFGFactoryConnectionComponent* GetFactoryConnector(AFGBuildable* Buildable, int32 Index)
	{
		TInlineComponentArray<UFGFactoryConnectionComponent*> Connectors;
		Buildable->GetComponents(Connectors);
		UFGFactoryConnectionComponent::SortComponentList(Connectors);
		return Connectors.IsValidIndex(Index) ? Connectors[Index] : nullptr;
	}

	/** Same as GetFactoryConnector but for power connectors - no canonical
	  * sort helper is exposed for these (most buildables have exactly one),
	  * so this uses plain component-array order. */
	UFGPowerConnectionComponent* GetPowerConnector(AFGBuildable* Buildable, int32 Index)
	{
		TInlineComponentArray<UFGPowerConnectionComponent*> Connectors;
		Buildable->GetComponents(Connectors);
		return Connectors.IsValidIndex(Index) ? Connectors[Index] : nullptr;
	}

	TSharedPtr<FJsonObject> ConnectionToJson(const FString& TFID, const FString& ClassName, const FSTFConnectionEndpoints& Endpoints)
	{
		const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("tf_id"), TFID);
		Json->SetStringField(TEXT("class"), ClassName);
		const TSharedPtr<FJsonObject> From = MakeShared<FJsonObject>();
		From->SetStringField(TEXT("buildable_tf_id"), Endpoints.FromTFID);
		From->SetNumberField(TEXT("connector"), Endpoints.FromConnector);
		Json->SetObjectField(TEXT("from"), From);
		const TSharedPtr<FJsonObject> To = MakeShared<FJsonObject>();
		To->SetStringField(TEXT("buildable_tf_id"), Endpoints.ToTFID);
		To->SetNumberField(TEXT("connector"), Endpoints.ToConnector);
		Json->SetObjectField(TEXT("to"), To);
		return Json;
	}
}

ASTFApiServerSubsystem::ASTFApiServerSubsystem()
{
	PrimaryActorTick.bCanEverTick = false;
	// Only the authoritative side runs the listener and mutates the world.
	ReplicationPolicy = ESubsystemReplicationPolicy::SpawnOnServer;
}

TWeakObjectPtr<ASTFApiServerSubsystem> ASTFApiServerSubsystem::ActiveInstance;
bool ASTFApiServerSubsystem::bRoutesBound = false;

void ASTFApiServerSubsystem::BeginPlay()
{
	Super::BeginPlay();

	FHttpServerModule& Module = FHttpServerModule::Get();
	Router = Module.GetHttpRouter(Port, /*bFailOnBindFailure*/ false);
	if (!Router.IsValid())
	{
		UE_LOG(LogSatisfactoryTerraform, Error, TEXT("Could not bind HTTP router on port %d"), Port);
		return;
	}
	ActiveInstance = this;
	BindRoutesOnce();
	Module.StartAllListeners();
	UE_LOG(LogSatisfactoryTerraform, Log, TEXT("SatisfactoryTerraform API listening on port %d"), Port);
}

void ASTFApiServerSubsystem::EndPlay(const EEndPlayReason::Type Reason)
{
	// Deliberately NOT unbinding the routes: they're process-lifetime and
	// dispatch through ActiveInstance (see the header comment on it for the
	// IHttpRouter re-registration quirk this avoids). Just stop being the
	// dispatch target; until the next session's BeginPlay the handlers 503.
	if (ActiveInstance.Get() == this)
	{
		ActiveInstance = nullptr;
	}
	Super::EndPlay(Reason);
}

void ASTFApiServerSubsystem::BindRoutesOnce()
{
	if (bRoutesBound)
	{
		return;
	}
	bRoutesBound = true;

	// Static-lambda dispatch, not CreateUObject: these handlers outlive any
	// one subsystem instance (they live as long as the process), so they
	// must not be tied to a UObject that gets destroyed on session end -
	// they look up whichever instance is current, per request.
	const auto Bind = [this](const FString& Path, EHttpServerRequestVerbs Verbs,
		bool (ASTFApiServerSubsystem::*Handler)(const FHttpServerRequest&, const FHttpResultCallback&))
	{
		Router->BindRoute(
			FHttpPath(Path), Verbs,
			FHttpRequestHandler::CreateLambda(
				[Handler](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
				{
					ASTFApiServerSubsystem* Instance = ActiveInstance.Get();
					if (!Instance)
					{
						OnComplete(ErrorResponse(503, TEXT("no game session is loaded")));
						return true;
					}
					return (Instance->*Handler)(Request, OnComplete);
				}));
	};

	Bind(TEXT("/api/v1/health"), EHttpServerRequestVerbs::VERB_GET, &ASTFApiServerSubsystem::HandleHealth);
	Bind(TEXT("/api/v1/world"), EHttpServerRequestVerbs::VERB_GET, &ASTFApiServerSubsystem::HandleWorld);
	Bind(TEXT("/api/v1/buildables"),
		EHttpServerRequestVerbs::VERB_GET | EHttpServerRequestVerbs::VERB_POST,
		&ASTFApiServerSubsystem::HandleBuildables);
	// A leading-":" token is IHttpRouter's actual path-parameter syntax
	// (FHttpRequestHandlerRegistrar::MatchesPath/ParsePathParameters) - it
	// lands in Request.PathParams["tf_id"]. A bare trailing-slash route does
	// NOT prefix-match; that was wrong and left GET/PATCH/DELETE-by-id
	// unreachable (confirmed against the real router source and a live
	// mod session: GET fell through to the list handler, PATCH 404'd).
	Bind(TEXT("/api/v1/buildables/:tf_id"),
		EHttpServerRequestVerbs::VERB_GET | EHttpServerRequestVerbs::VERB_PATCH | EHttpServerRequestVerbs::VERB_DELETE,
		&ASTFApiServerSubsystem::HandleBuildableByID);
	Bind(TEXT("/api/v1/connections"),
		EHttpServerRequestVerbs::VERB_GET | EHttpServerRequestVerbs::VERB_POST,
		&ASTFApiServerSubsystem::HandleConnections);
	Bind(TEXT("/api/v1/connections/:tf_id"),
		EHttpServerRequestVerbs::VERB_GET | EHttpServerRequestVerbs::VERB_DELETE,
		&ASTFApiServerSubsystem::HandleConnectionByID);
}

bool ASTFApiServerSubsystem::CheckAuth(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) const
{
	if (Token.IsEmpty())
	{
		return true;
	}
	const TArray<FString>* Auth = Request.Headers.Find(TEXT("Authorization"));
	if (Auth && Auth->Num() > 0 && (*Auth)[0] == FString::Printf(TEXT("Bearer %s"), *Token))
	{
		return true;
	}
	OnComplete(ErrorResponse(401, TEXT("missing or invalid bearer token")));
	return false;
}

bool ASTFApiServerSubsystem::HandleHealth(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	const TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("status"), TEXT("ok"));
	OnComplete(JsonResponse(200, Body));
	return true;
}

bool ASTFApiServerSubsystem::HandleWorld(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (!CheckAuth(Request, OnComplete))
	{
		return true;
	}
	const TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("session_name"), GetWorld()->GetMapName());
	Body->SetStringField(TEXT("game_version"), TEXT("")); // TODO(M1): FEngineVersion / FG changelist
	Body->SetStringField(TEXT("mod_version"), TEXT("0.1.0"));
	OnComplete(JsonResponse(200, Body));
	return true;
}

bool ASTFApiServerSubsystem::HandleBuildables(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (!CheckAuth(Request, OnComplete))
	{
		return true;
	}
	ASTFRegistrySubsystem* Registry = ASTFRegistrySubsystem::Get(GetWorld());

	if (Request.Verb == EHttpServerRequestVerbs::VERB_GET)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const auto& Entry : Registry->GetAll())
		{
			// Belts/power lines share the registry with plain buildables
			// (distinguished by having connection-endpoint metadata) but
			// belong to /api/v1/connections, where they're serialized with
			// their from/to shape - listing them here too mixed the two
			// response schemas in one array (issue #6). Same filter
			// HandleConnections uses, inverted.
			if (Registry->FindConnectionEndpoints(Entry.TFID))
			{
				continue;
			}
			Items.Add(MakeShared<FJsonValueObject>(Entry.IsLightweight()
				? LightweightToJson(Entry.TFID, Entry.LightweightRef)
				: BuildableToJson(Entry.TFID, Entry.Buildable)));
		}
		FString Out;
		const auto Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Items, Writer);
		auto Response = FHttpServerResponse::Create(Out, TEXT("application/json"));
		Response->Code = EHttpServerResponseCodes::Ok;
		OnComplete(MoveTemp(Response));
		return true;
	}

	// POST /api/v1/buildables
	const TSharedPtr<FJsonObject> Body = ParseBody(Request);
	if (!Body.IsValid())
	{
		OnComplete(ErrorResponse(400, TEXT("invalid JSON")));
		return true;
	}
	int32 Status = 201;
	FString Error;
	const TSharedPtr<FJsonObject> Result = SpawnBuildable(Body, Status, Error);
	if (!Result.IsValid())
	{
		OnComplete(ErrorResponse(Status, Error));
		return true;
	}
	OnComplete(JsonResponse(201, Result));
	return true;
}

bool ASTFApiServerSubsystem::HandleBuildableByID(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (!CheckAuth(Request, OnComplete))
	{
		return true;
	}
	const FString TFID = PathID(Request);
	ASTFRegistrySubsystem* Registry = ASTFRegistrySubsystem::Get(GetWorld());

	if (AFGBuildable* Buildable = Registry->Find(TFID))
	{
		if (Request.Verb == EHttpServerRequestVerbs::VERB_GET)
		{
			OnComplete(JsonResponse(200, BuildableToJson(TFID, Buildable)));
			return true;
		}

		if (Request.Verb == EHttpServerRequestVerbs::VERB_PATCH)
		{
			const TSharedPtr<FJsonObject> Body = ParseBody(Request);
			if (!Body.IsValid())
			{
				OnComplete(ErrorResponse(400, TEXT("invalid JSON")));
				return true;
			}
			int32 Status = 200;
			FString Error;
			const TSharedPtr<FJsonObject> Result = PatchBuildable(Buildable, Body, TFID, Status, Error);
			if (!Result.IsValid())
			{
				OnComplete(ErrorResponse(Status, Error));
				return true;
			}
			OnComplete(JsonResponse(200, Result));
			return true;
		}

		// DELETE — dismantle through IFGDismantleInterface when the buildable
		// implements it (refunds + connection cleanup match vanilla
		// behaviour), falling back to a plain Destroy() otherwise.
		Registry->Unregister(TFID);
		DismantleBuildable(Buildable);
		auto Response = FHttpServerResponse::Create(TEXT(""), TEXT("application/json"));
		Response->Code = EHttpServerResponseCodes::NoContent;
		OnComplete(MoveTemp(Response));
		return true;
	}

	if (const FLightweightBuildableInstanceRef* Ref = Registry->FindLightweight(TFID))
	{
		if (Request.Verb == EHttpServerRequestVerbs::VERB_GET)
		{
			OnComplete(JsonResponse(200, LightweightToJson(TFID, *Ref)));
			return true;
		}

		if (Request.Verb == EHttpServerRequestVerbs::VERB_PATCH)
		{
			// Lightweight-eligible classes (foundations, walls, ...) never
			// have a recipe/clock_speed - same 422 a non-manufacturer full
			// actor gets from PatchBuildable.
			OnComplete(ErrorResponse(422, TEXT("this buildable has no recipe/clock_speed to patch")));
			return true;
		}

		// DELETE — FLightweightBuildableInstanceRef::Remove() is the
		// lightweight subsystem's own removal path; copy the ref first since
		// Remove() is non-const and the registry only hands out const access.
		FLightweightBuildableInstanceRef MutableRef = *Ref;
		Registry->Unregister(TFID);
		if (!MutableRef.Remove())
		{
			// Confirmed live: this can fail silently while the registry
			// entry is already gone, orphaning a visible-but-untracked
			// instance in the world with no tf_id left to remove it
			// through. Nothing left to roll back to (Unregister already
			// happened and re-registering under the same tf_id risks a
			// worse state) - just make it loud so it's diagnosable instead
			// of silently vanishing.
			UE_LOG(LogSatisfactoryTerraform, Warning,
				TEXT("Lightweight buildable %s: registry entry removed but the underlying instance's own Remove() reported failure - it may still be visible in-game, untracked"),
				*TFID);
		}
		auto Response = FHttpServerResponse::Create(TEXT(""), TEXT("application/json"));
		Response->Code = EHttpServerResponseCodes::NoContent;
		OnComplete(MoveTemp(Response));
		return true;
	}

	OnComplete(ErrorResponse(404, TEXT("no buildable with that tf_id")));
	return true;
}

bool ASTFApiServerSubsystem::HandleConnections(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (!CheckAuth(Request, OnComplete))
	{
		return true;
	}
	ASTFRegistrySubsystem* Registry = ASTFRegistrySubsystem::Get(GetWorld());

	if (Request.Verb == EHttpServerRequestVerbs::VERB_GET)
	{
		// Connections share the registry's tf_id namespace with buildables
		// (see STFRegistrySubsystem.h); FindConnectionEndpoints is what
		// distinguishes "this id is a connection" from "this id is a
		// buildable" - only entries with endpoints belong in this list.
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const auto& Entry : Registry->GetAll())
		{
			if (Entry.IsLightweight())
			{
				continue; // structural buildables are never connections
			}
			if (const FSTFConnectionEndpoints* Endpoints = Registry->FindConnectionEndpoints(Entry.TFID))
			{
				Items.Add(MakeShared<FJsonValueObject>(ConnectionToJson(Entry.TFID, Entry.Buildable->GetClass()->GetName(), *Endpoints)));
			}
		}
		FString Out;
		const auto Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Items, Writer);
		auto Response = FHttpServerResponse::Create(Out, TEXT("application/json"));
		Response->Code = EHttpServerResponseCodes::Ok;
		OnComplete(MoveTemp(Response));
		return true;
	}
	const TSharedPtr<FJsonObject> Body = ParseBody(Request);
	if (!Body.IsValid())
	{
		OnComplete(ErrorResponse(400, TEXT("invalid JSON")));
		return true;
	}
	int32 Status = 201;
	FString Error;
	const TSharedPtr<FJsonObject> Result = SpawnConnection(Body, Status, Error);
	if (!Result.IsValid())
	{
		OnComplete(ErrorResponse(Status, Error));
		return true;
	}
	OnComplete(JsonResponse(201, Result));
	return true;
}

bool ASTFApiServerSubsystem::HandleConnectionByID(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (!CheckAuth(Request, OnComplete))
	{
		return true;
	}
	const FString TFID = PathID(Request);
	ASTFRegistrySubsystem* Registry = ASTFRegistrySubsystem::Get(GetWorld());

	// A connection is always a full actor (belts/wires are never lightweight-
	// tracked - see SpawnConnection) with endpoint metadata registered; both
	// must be present, otherwise this id is either unknown or a buildable.
	AFGBuildable* Buildable = Registry->Find(TFID);
	const FSTFConnectionEndpoints* Endpoints = Buildable ? Registry->FindConnectionEndpoints(TFID) : nullptr;
	if (!Buildable || !Endpoints)
	{
		OnComplete(ErrorResponse(404, TEXT("no connection with that tf_id")));
		return true;
	}

	if (Request.Verb == EHttpServerRequestVerbs::VERB_GET)
	{
		OnComplete(JsonResponse(200, ConnectionToJson(TFID, Buildable->GetClass()->GetName(), *Endpoints)));
		return true;
	}

	// DELETE
	Registry->Unregister(TFID);
	DismantleBuildable(Buildable);
	auto Response = FHttpServerResponse::Create(TEXT(""), TEXT("application/json"));
	Response->Code = EHttpServerResponseCodes::NoContent;
	OnComplete(MoveTemp(Response));
	return true;
}

void ASTFApiServerSubsystem::BuildClassNameIndex() const
{
	bClassNameIndexBuilt = true;
	const IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TArray<FAssetData> Assets;
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	Filter.bRecursivePaths = true;
	Filter.PackagePaths.Add(TEXT("/Game"));
	AssetRegistry.GetAssets(Filter, Assets);

	for (const FAssetData& Asset : Assets)
	{
		// A Blueprint asset named "Build_ConstructorMk1" generates the class
		// "Build_ConstructorMk1_C" at "<package>.Build_ConstructorMk1_C" -
		// index by that generated name so it matches the API's class
		// strings (e.g. Build_ConstructorMk1_C, Recipe_IronPlate_C) exactly.
		const FString GeneratedClassName = Asset.AssetName.ToString() + TEXT("_C");
		const FString GeneratedClassPath = FString::Printf(TEXT("%s.%s"), *Asset.PackageName.ToString(), *GeneratedClassName);
		ClassNameIndex.Add(GeneratedClassName, FSoftObjectPath(GeneratedClassPath));
	}
	UE_LOG(LogSatisfactoryTerraform, Log, TEXT("Indexed %d Blueprint classes for name resolution"), ClassNameIndex.Num());
}

UClass* ASTFApiServerSubsystem::ResolveClassByName(const FString& ClassName, UClass* ExpectedBase, FString& OutError) const
{
	// Fast path: already loaded (common once the player has encountered it).
	if (UClass* Loaded = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::None))
	{
		if (Loaded->IsChildOf(ExpectedBase))
		{
			return Loaded;
		}
	}

	if (!bClassNameIndexBuilt)
	{
		BuildClassNameIndex();
	}

	if (const FSoftObjectPath* Path = ClassNameIndex.Find(ClassName))
	{
		if (UClass* Resolved = Cast<UClass>(Path->TryLoad()))
		{
			if (Resolved->IsChildOf(ExpectedBase))
			{
				return Resolved;
			}
		}
	}

	OutError = FString::Printf(TEXT("unknown class %s"), *ClassName);
	return nullptr;
}

UClass* ASTFApiServerSubsystem::ResolveBuildableClass(const FString& ClassName, FString& OutError) const
{
	if (!ClassName.StartsWith(TEXT("Build_")) || !ClassName.EndsWith(TEXT("_C")))
	{
		OutError = TEXT("class must be a buildable class name like Build_ConstructorMk1_C");
		return nullptr;
	}
	return ResolveClassByName(ClassName, AFGBuildable::StaticClass(), OutError);
}

UClass* ASTFApiServerSubsystem::ResolveRecipeClass(const FString& ClassName, FString& OutError) const
{
	if (!ClassName.StartsWith(TEXT("Recipe_")) || !ClassName.EndsWith(TEXT("_C")))
	{
		OutError = TEXT("recipe must be a recipe class name like Recipe_IronPlate_C");
		return nullptr;
	}
	return ResolveClassByName(ClassName, UFGRecipe::StaticClass(), OutError);
}

UClass* ASTFApiServerSubsystem::ResolveConnectionClass(const FString& ClassName, FString& OutError) const
{
	if (!ClassName.StartsWith(TEXT("Build_")) || !ClassName.EndsWith(TEXT("_C")))
	{
		OutError = TEXT("class must be a belt (Build_ConveyorBeltMkN_C) or Build_PowerLine_C");
		return nullptr;
	}
	UClass* Class = ResolveClassByName(ClassName, AFGBuildable::StaticClass(), OutError);
	if (!Class)
	{
		return nullptr;
	}
	if (!Class->IsChildOf(AFGBuildableConveyorBelt::StaticClass()) && !Class->IsChildOf(AFGBuildableWire::StaticClass()))
	{
		OutError = TEXT("class must be a belt (Build_ConveyorBeltMkN_C) or Build_PowerLine_C");
		return nullptr;
	}
	return Class;
}

void ASTFApiServerSubsystem::DismantleBuildable(AFGBuildable* Buildable) const
{
	// AFGBuildableConveyorAttachment (splitters/mergers/lifts)'s own
	// Dismantle_Implementation crashes the game: it calls
	// IFGDismantleInterface::Execute_CanDismantle() on something that's
	// null in this context (confirmed live - "Assertion failed: O != 0",
	// FGDismantleInterface.gen.cpp:48, called from
	// FGBuildableConveyorAttachment.cpp:179). That's the real game's
	// compiled logic, not ours, and not something we can fix - only avoid.
	// Skip the interface entirely for this family and destroy directly;
	// the cost is no build-cost refund on dismantle for these classes,
	// which is cheap for a splitter/merger anyway.
	if (Buildable->IsA(AFGBuildableConveyorAttachment::StaticClass()))
	{
		Buildable->Destroy();
	}
	else if (Buildable->GetClass()->ImplementsInterface(UFGDismantleInterface::StaticClass()))
	{
		IFGDismantleInterface::Execute_Dismantle(Buildable);
	}
	else
	{
		Buildable->Destroy();
	}
}

TSharedPtr<FJsonObject> ASTFApiServerSubsystem::PatchBuildable(AFGBuildable* Buildable, const TSharedPtr<FJsonObject>& Body, const FString& TFID, int32& OutStatus, FString& OutError)
{
	AFGBuildableManufacturer* Manufacturer = Cast<AFGBuildableManufacturer>(Buildable);
	if (!Manufacturer)
	{
		OutStatus = 422;
		OutError = TEXT("this buildable has no recipe/clock_speed to patch");
		return nullptr;
	}

	FString RecipeClassName;
	if (Body->TryGetStringField(TEXT("recipe"), RecipeClassName) && !RecipeClassName.IsEmpty())
	{
		UClass* RecipeClass = ResolveRecipeClass(RecipeClassName, OutError);
		if (!RecipeClass)
		{
			OutStatus = 422;
			return nullptr;
		}
		Manufacturer->SetRecipe(RecipeClass);
	}

	double ClockSpeed = 0;
	if (Body->TryGetNumberField(TEXT("clock_speed"), ClockSpeed))
	{
		if (ClockSpeed < 0.01 || ClockSpeed > 2.5)
		{
			OutStatus = 422;
			OutError = TEXT("clock_speed must be between 0.01 and 2.5");
			return nullptr;
		}
		Manufacturer->SetPendingPotential(ClockSpeed);
	}

	return BuildableToJson(TFID, Buildable);
}

TSharedPtr<FJsonObject> ASTFApiServerSubsystem::SpawnBuildable(const TSharedPtr<FJsonObject>& Body, int32& OutStatus, FString& OutError)
{
	const FString TFID = Body->GetStringField(TEXT("tf_id"));
	if (TFID.IsEmpty())
	{
		OutStatus = 422;
		OutError = TEXT("tf_id is required");
		return nullptr;
	}
	ASTFRegistrySubsystem* Registry = ASTFRegistrySubsystem::Get(GetWorld());
	if (Registry->Contains(TFID))
	{
		OutStatus = 409;
		OutError = FString::Printf(TEXT("buildable with tf_id %s already exists"), *TFID);
		return nullptr;
	}

	UClass* Class = ResolveBuildableClass(Body->GetStringField(TEXT("class")), OutError);
	if (!Class)
	{
		OutStatus = 422;
		return nullptr;
	}

	// Resolve the recipe (if any) and validate clock_speed up front too, so a
	// bad value fails before anything is spawned rather than leaving a
	// half-constructed actor behind.
	UClass* RecipeClass = nullptr;
	FString RecipeClassName;
	if (Body->TryGetStringField(TEXT("recipe"), RecipeClassName) && !RecipeClassName.IsEmpty())
	{
		RecipeClass = ResolveRecipeClass(RecipeClassName, OutError);
		if (!RecipeClass)
		{
			OutStatus = 422;
			return nullptr;
		}
	}
	double ClockSpeed = 1.0;
	Body->TryGetNumberField(TEXT("clock_speed"), ClockSpeed);
	if (ClockSpeed < 0.01 || ClockSpeed > 2.5)
	{
		OutStatus = 422;
		OutError = TEXT("clock_speed must be between 0.01 and 2.5");
		return nullptr;
	}

	const TSharedPtr<FJsonObject>* TransformJson = nullptr;
	if (!Body->TryGetObjectField(TEXT("transform"), TransformJson))
	{
		OutStatus = 422;
		OutError = TEXT("transform is required");
		return nullptr;
	}
	const FVector Location(
		(*TransformJson)->GetNumberField(TEXT("x")),
		(*TransformJson)->GetNumberField(TEXT("y")),
		(*TransformJson)->GetNumberField(TEXT("z")));
	double Yaw = 0;
	(*TransformJson)->TryGetNumberField(TEXT("yaw"), Yaw);
	const FTransform Transform(FRotator(0, Yaw, 0), Location);

	// Spawn through the buildable subsystem so the buildable ticks in the
	// factory tick group like a hologram-built one would.
	AFGBuildableSubsystem* BuildableSubsystem = AFGBuildableSubsystem::Get(GetWorld());
	AFGBuildable* Buildable = BuildableSubsystem->BeginSpawnBuildable(Class, Transform);
	if (!Buildable)
	{
		OutStatus = 422;
		OutError = TEXT("game refused to spawn that buildable");
		return nullptr;
	}

	Buildable->FinishSpawning(Transform);

	// Recipe/clock_speed applied AFTER FinishSpawning, not between
	// BeginSpawnBuildable/FinishSpawning: mCurrentRecipe is a Replicated
	// SaveGame property and SetRecipe's real setup (input/output access
	// indices against the buildable's factory connectors) needs those
	// connectors already initialized, which only happens once construction
	// finishes - confirmed live: setting it pre-FinishSpawning silently
	// didn't stick (GetCurrentRecipe read back empty right after).
	if (AFGBuildableManufacturer* Manufacturer = Cast<AFGBuildableManufacturer>(Buildable))
	{
		if (RecipeClass)
		{
			Manufacturer->SetRecipe(RecipeClass);
		}
		Manufacturer->SetPendingPotential(ClockSpeed);
	}

	// Simple structural buildables (foundations, walls, ramps, ...) are
	// eligible for Satisfactory's Lightweight Buildable system, which
	// destroys the actor and migrates it to a memory-efficient non-actor
	// representation shortly after spawning (matching vanilla performance
	// at scale - manufacturers are never eligible, so this never applies to
	// them). Rather than race that async, build-effect-triggered
	// conversion, convert deterministically ourselves right now and keep
	// the resulting FLightweightBuildableInstanceRef instead of the
	// (about to be invalid) actor pointer. This mirrors the real
	// AFGBuildable::HandleLightweightAddition() - which is protected, so
	// not callable directly - using only its public building blocks
	// (confirmed against the real implementation).
	if (Buildable->ManagedByLightweightBuildableSubsystem())
	{
		AFGLightweightBuildableSubsystem* LightweightSubsystem = AFGLightweightBuildableSubsystem::Get(GetWorld());
		const int32 RuntimeIndex = LightweightSubsystem->AddFromBuildable(Buildable);
		if (RuntimeIndex != INDEX_NONE)
		{
			FLightweightBuildableInstanceRef Ref;
			Ref.Initialize(LightweightSubsystem, Class, RuntimeIndex);
			Buildable->SetIsStaleLightweightTemporary(); // destruction below isn't a real dismantle
			Buildable->Destroy();

			Registry->RegisterLightweight(TFID, Ref);
			UE_LOG(LogSatisfactoryTerraform, Log, TEXT("Spawned %s as %s (lightweight)"), *Class->GetName(), *TFID);
			OutStatus = 201;
			return LightweightToJson(TFID, Ref);
		}
		// AddFromBuildable failed (index INDEX_NONE) - fall through and keep
		// the buildable as a regular full-actor registration below.
	}

	Registry->Register(TFID, Buildable);
	UE_LOG(LogSatisfactoryTerraform, Log, TEXT("Spawned %s as %s"), *Class->GetName(), *TFID);
	OutStatus = 201;
	return BuildableToJson(TFID, Buildable);
}

TSharedPtr<FJsonObject> ASTFApiServerSubsystem::SpawnConnection(const TSharedPtr<FJsonObject>& Body, int32& OutStatus, FString& OutError)
{
	const FString TFID = Body->GetStringField(TEXT("tf_id"));
	if (TFID.IsEmpty())
	{
		OutStatus = 422;
		OutError = TEXT("tf_id is required");
		return nullptr;
	}
	ASTFRegistrySubsystem* Registry = ASTFRegistrySubsystem::Get(GetWorld());
	if (Registry->Contains(TFID))
	{
		OutStatus = 409;
		OutError = FString::Printf(TEXT("connection with tf_id %s already exists"), *TFID);
		return nullptr;
	}

	UClass* Class = ResolveConnectionClass(Body->GetStringField(TEXT("class")), OutError);
	if (!Class)
	{
		OutStatus = 422;
		return nullptr;
	}
	const bool bIsBelt = Class->IsChildOf(AFGBuildableConveyorBelt::StaticClass());

	const TSharedPtr<FJsonObject>* FromJson = nullptr;
	const TSharedPtr<FJsonObject>* ToJson = nullptr;
	if (!Body->TryGetObjectField(TEXT("from"), FromJson) || !Body->TryGetObjectField(TEXT("to"), ToJson))
	{
		OutStatus = 422;
		OutError = TEXT("from and to are required");
		return nullptr;
	}
	FSTFConnectionEndpoints Endpoints;
	Endpoints.FromTFID = (*FromJson)->GetStringField(TEXT("buildable_tf_id"));
	Endpoints.FromConnector = (*FromJson)->GetIntegerField(TEXT("connector"));
	Endpoints.ToTFID = (*ToJson)->GetStringField(TEXT("buildable_tf_id"));
	Endpoints.ToConnector = (*ToJson)->GetIntegerField(TEXT("connector"));

	// Endpoints must be full actors - foundations/walls/etc. (lightweight-
	// tracked) have no factory/power connectors to attach to, and Find()
	// correctly returns nullptr for those, same as a genuinely unknown id.
	AFGBuildable* FromBuildable = Registry->Find(Endpoints.FromTFID);
	AFGBuildable* ToBuildable = Registry->Find(Endpoints.ToTFID);
	if (!FromBuildable || !ToBuildable)
	{
		OutStatus = 422;
		OutError = TEXT("from/to buildable_tf_id must reference an existing, connectable buildable");
		return nullptr;
	}

	AFGBuildableSubsystem* BuildableSubsystem = AFGBuildableSubsystem::Get(GetWorld());
	AFGBuildable* Connection = nullptr;

	if (bIsBelt)
	{
		UFGFactoryConnectionComponent* FromConn = GetFactoryConnector(FromBuildable, Endpoints.FromConnector);
		UFGFactoryConnectionComponent* ToConn = GetFactoryConnector(ToBuildable, Endpoints.ToConnector);
		if (!FromConn || !ToConn)
		{
			OutStatus = 422;
			OutError = TEXT("from/to connector index out of range for that buildable");
			return nullptr;
		}

		// Spawn the belt actor at the "from" connector's transform, then
		// route a spline to the "to" connector via the same strategy object
		// UE itself uses for programmatically-placed spline buildables
		// (UFGBuildableSpawnStrategy_Spline - confirmed against the real
		// FactoryGame source; not test-only despite living under a "Tests"
		// header, it's the general-purpose spline placement helper).
		const FTransform FromTransform(FromConn->GetComponentRotation(), FromConn->GetConnectorLocation());
		AFGBuildable* Belt = BuildableSubsystem->BeginSpawnBuildable(Class, FromTransform);
		if (!Belt)
		{
			OutStatus = 422;
			OutError = TEXT("game refused to spawn that belt");
			return nullptr;
		}

		UFGBuildableSpawnStrategy_Spline* Strategy = NewObject<UFGBuildableSpawnStrategy_Spline>(Belt);
		Strategy->mSplineRouteStrategy = EFGSplineBuildableRouteStrategy::Auto;
		Strategy->mSplineBendRadius = 50.0f;
		Strategy->mLocalStartTransform = FTransform::Identity;
		const FVector LocalEndLocation = FromTransform.InverseTransformPosition(ToConn->GetConnectorLocation());
		const FRotator LocalEndRotation = (ToConn->GetComponentRotation() - FromTransform.Rotator());
		Strategy->mLocalEndTransform = FTransform(LocalEndRotation, LocalEndLocation);

		Strategy->PreSpawnBuildable(Belt);
		Belt->FinishSpawning(FromTransform);
		Strategy->PostSpawnBuildable(Belt);

		// SetConnection's real (compiled-game) implementation isn't visible
		// from this source-available stub and returns void, so there's no
		// direct success/failure signal - call from both ends so the
		// connection is correct regardless of whether it's one- or
		// two-sided internally, then verify via GetConnection() (a plain
		// inline accessor, not a stub) that both ends actually ended up
		// pointing at each other before declaring success.
		FromConn->SetConnection(ToConn);
		ToConn->SetConnection(FromConn);

		if (FromConn->GetConnection() != ToConn || ToConn->GetConnection() != FromConn)
		{
			Belt->Destroy();
			OutStatus = 422;
			OutError = TEXT("game refused to connect those factory connectors (already connected to something else?)");
			return nullptr;
		}

		Connection = Belt;
	}
	else
	{
		UFGPowerConnectionComponent* FromConn = GetPowerConnector(FromBuildable, Endpoints.FromConnector);
		UFGPowerConnectionComponent* ToConn = GetPowerConnector(ToBuildable, Endpoints.ToConnector);
		if (!FromConn || !ToConn)
		{
			OutStatus = 422;
			OutError = TEXT("from/to connector index out of range for that buildable");
			return nullptr;
		}

		const FTransform SpawnTransform(FromConn->GetComponentLocation());
		AFGBuildable* WireActor = BuildableSubsystem->BeginSpawnBuildable(Class, SpawnTransform);
		if (!WireActor)
		{
			OutStatus = 422;
			OutError = TEXT("game refused to spawn that power line");
			return nullptr;
		}
		WireActor->FinishSpawning(SpawnTransform);

		AFGBuildableWire* Wire = CastChecked<AFGBuildableWire>(WireActor);
		if (!Wire->Connect(FromConn, ToConn))
		{
			Wire->Destroy();
			OutStatus = 422;
			OutError = TEXT("game refused to connect those power connectors");
			return nullptr;
		}

		Connection = WireActor;
	}

	Registry->Register(TFID, Connection);
	Registry->RegisterConnectionEndpoints(TFID, Endpoints);
	UE_LOG(LogSatisfactoryTerraform, Log, TEXT("Connected %s as %s"), *Class->GetName(), *TFID);
	OutStatus = 201;
	return ConnectionToJson(TFID, Class->GetName(), Endpoints);
}
