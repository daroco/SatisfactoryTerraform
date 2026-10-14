#include "STFApiServerSubsystem.h"

#include "SatisfactoryTerraformModule.h"
#include "STFRegistrySubsystem.h"

#include "Buildables/FGBuildable.h"
#include "Buildables/FGBuildableFactory.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "Buildables/FGBuildableConveyorBase.h" // GetConnection0/1 (belt in/out)
#include "Buildables/FGBuildableConveyorBelt.h"
#include "Buildables/FGBuildableConveyorAttachment.h"
#include "Buildables/FGBuildableWire.h"
#include "FGBuildableSubsystem.h"
#include "FGRecipe.h"
#include "FGDismantleInterface.h"
#include "FGClearanceData.h" // FFGClearanceData / EClearanceType
#include "FGFactoryConnectionComponent.h"
#include "FGPowerConnectionComponent.h"
#include "FGPipeConnectionComponent.h"
#include "FGPipeConnectionComponentHyper.h"
#include "Buildables/FGBuildablePipeBase.h"
#include "Buildables/FGBuildablePipeline.h"
#include "Buildables/FGBuildablePipeHyper.h"
#include "Buildables/FGBuildableConveyorLift.h"
#include "Buildables/FGBuildableRailroadTrack.h"
#include "Buildables/FGBuildableRailroadStation.h"
#include "Buildables/FGBuildableTrainPlatform.h"
#include "Buildables/FGBuildableRailroadSignal.h"
#include "Buildables/FGBuildableRailroadSwitchControl.h"
#include "Buildables/FGBuildableResourceExtractorBase.h"
#include "Buildables/FGBuildableGeneratorGeoThermal.h"
#include "Resources/FGExtractableResourceInterface.h"
#include "Resources/FGResourceNodeBase.h"
#include "Resources/FGResourceNode.h"
#include "Resources/FGResourceNodeFrackingCore.h"
#include "Resources/FGResourceNodeFrackingSatellite.h"
#include "Resources/FGResourceDescriptor.h"
#include "Resources/FGResourceDescriptorGeyser.h"
#include "Resources/FGBuildingDescriptor.h"
#include "FGWaterVolume.h"
#include "Hologram/FGHologram.h"
#include "FGConstructDisqualifier.h"
#include "Engine/ActorInstanceHandle.h"
#include "Components/PrimitiveComponent.h"
#include "FGRecipeManager.h"
#include "FGCharacterPlayer.h"
#include "Equipment/FGBuildGun.h"
#include "EngineUtils.h"   // TActorIterator
#include "TimerManager.h"
#include "Buildables/FGBuildableSignBase.h"
#include "Buildables/FGBuildableSplitterSmart.h"
#include "Buildables/FGBuildableCircuitSwitch.h"
#include "Buildables/FGBuildableLightSource.h"
#include "FGSplineBuildableInterface.h"
#include "Tests/FGBuildableSpawnStrategy_Spline.h"
#include "Tests/FGTestBlueprintFunctionLibrary.h"

#include "Misc/ConfigCacheIni.h" // GConfig - pin the listener to loopback
#include "HAL/PlatformMisc.h"    // FPlatformMisc::GetEnvironmentVariable (token)
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

#include "GameFramework/PlayerState.h" // GetPlayerName() - forward-declared via PlayerController

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

	/** The node_id of an extractor: the actor path of the feature it is bound
	  * to, i.e. exactly what GET /world/resource-nodes reports as id and what
	  * POST /buildables took as node_id. Empty when unbound (an extractor that
	  * lost its node, or one spawned without binding - which this API never
	  * does, but a hand-built one can end up that way after a map change). */
	FString ExtractorNodeID(const AFGBuildable* Buildable)
	{
		const AFGBuildableResourceExtractorBase* Extractor = Cast<AFGBuildableResourceExtractorBase>(Buildable);
		if (!Extractor)
		{
			return FString();
		}
		// GetExtractableResourceActor is protected; the public accessor hands
		// back the TScriptInterface whose object is that same actor.
		const UObject* Resource = Extractor->GetExtractableResource().GetObject();
		return Resource ? Resource->GetPathName() : FString();
	}

	/** recipe / clock_speed / node_id, with the same presence rules on every
	  * read path (GET /buildables, POST response, GET /world/buildables):
	  *
	  * - recipe: manufacturers only (GetCurrentRecipe/SetRecipe live on
	  *   AFGBuildableManufacturer - confirmed against the real header).
	  * - clock_speed: any AFGBuildableFactory whose class can change
	  *   potential (mCanChangePotential, read through the header-inline
	  *   GetCanChangePotential) - that is manufacturers, extractors and
	  *   generators. A splitter or a foundation has no clock and the field is
	  *   omitted, so the provider can tell "no clock" from "clock 0".
	  * - node_id: extractors that are bound to a feature. */
	void AppendProductionFields(const TSharedPtr<FJsonObject>& Json, const AFGBuildable* Buildable)
	{
		if (const AFGBuildableManufacturer* Manufacturer = Cast<AFGBuildableManufacturer>(Buildable))
		{
			if (const TSubclassOf<UFGRecipe> Recipe = Manufacturer->GetCurrentRecipe())
			{
				Json->SetStringField(TEXT("recipe"), Recipe->GetName());
			}
		}
		if (const AFGBuildableFactory* Factory = Cast<AFGBuildableFactory>(Buildable))
		{
			if (Factory->GetCanChangePotential())
			{
				Json->SetNumberField(TEXT("clock_speed"), Factory->GetPendingPotential());
			}
		}
		const FString NodeID = ExtractorNodeID(Buildable);
		if (!NodeID.IsEmpty())
		{
			Json->SetStringField(TEXT("node_id"), NodeID);
		}
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

		AppendProductionFields(Json, Buildable);
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

	/** Same as GetFactoryConnector but for pipe connectors, which come in two
	  * mutually exclusive kinds: fluid (UFGPipeConnectionComponent) and
	  * hypertube (UFGPipeConnectionComponentHyper). Indices are per-kind, so
	  * connector 0 of a machine's fluid connectors is unrelated to connector 0
	  * of anything else it has - the class of the connection being placed
	  * decides which list is indexed.
	  *
	  * No canonical sort helper is exposed for these (unlike factory
	  * connectors), so this uses plain component-array order, as the power
	  * connectors above do. */
	UFGPipeConnectionComponentBase* GetPipeConnector(AFGBuildable* Buildable, int32 Index, bool bHyper)
	{
		if (bHyper)
		{
			TInlineComponentArray<UFGPipeConnectionComponentHyper*> Connectors;
			Buildable->GetComponents(Connectors);
			return Connectors.IsValidIndex(Index) ? Connectors[Index] : nullptr;
		}
		TInlineComponentArray<UFGPipeConnectionComponent*> Connectors;
		Buildable->GetComponents(Connectors);
		return Connectors.IsValidIndex(Index) ? Connectors[Index] : nullptr;
	}

	/** True when RecipeClass can actually be produced by BuildableClass.
	  *
	  * Recipes declare their producers in mProducedIn, and
	  * UFGRecipe::GetProducedIn resolves that list. Deliberately NOT
	  * UFGRecipe::IsProducedIn, which would be the obvious choice: it is
	  * stub-only in the available source (`return bool();`), so if it ever
	  * resolved to the stub it would reject every recipe. GetProducedIn has a
	  * real body we can read, so its behaviour is known rather than assumed.
	  *
	  * Fails OPEN: a recipe that declares no producers at all is allowed
	  * through. This is the opposite of how co-location is handled, and on
	  * purpose - a wrong rejection here breaks a legitimate apply, while a
	  * wrong acceptance only yields a machine that does not run. Fail closed
	  * where the cost is corruption, open where the cost is inconvenience. */
	bool RecipeFitsBuildable(UClass* RecipeClass, UClass* BuildableClass)
	{
		if (!RecipeClass || !BuildableClass)
		{
			return true;
		}
		const TArray<TSubclassOf<UObject>> Producers = UFGRecipe::GetProducedIn(RecipeClass);
		if (Producers.Num() == 0)
		{
			return true; // nothing declared - cannot judge, so do not block
		}
		for (const TSubclassOf<UObject>& Producer : Producers)
		{
			if (Producer == BuildableClass)
			{
				return true;
			}
		}
		return false;
	}

	/** Wire spelling for EClearanceType - snake_case to match the rest of the
	  * API, and stable regardless of how the enum is displayed in-editor. */
	const TCHAR* ClearanceTypeName(EClearanceType Type)
	{
		switch (Type)
		{
		case EClearanceType::CT_Soft:            return TEXT("soft");
		case EClearanceType::CT_BlockEverything: return TEXT("block_everything");
		default:                                 return TEXT("default");
		}
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

	// Bind to loopback only, BEFORE GetHttpRouter creates the listener.
	//
	// This API can build and dismantle anything in the world and has no auth
	// by default, so it must never be reachable off-machine. UE's own code
	// default for BindAddress is "localhost", but FactoryGame's engine config
	// overrides it to "any" - confirmed live: the listener came up on
	// 0.0.0.0:8090 and answered unauthenticated requests from another host on
	// the LAN, including a full factory listing.
	//
	// GetListenerConfig reads a per-port entry out of the ListenerOverrides
	// array in [HTTPServer.Listeners], so this pins only OUR port and leaves
	// any other listener (another mod's, the game's) exactly as configured.
	{
		static const FString IniSection(TEXT("HTTPServer.Listeners"));
		static const FString OverridesKey(TEXT("ListenerOverrides"));
		const FString PortPrefix = FString::Printf(TEXT("(Port=%d,"), Port);

		TArray<FString> Overrides;
		GConfig->GetArray(*IniSection, *OverridesKey, Overrides, GEngineIni);
		Overrides.RemoveAll([&PortPrefix](const FString& Entry)
		{
			return Entry.TrimStartAndEnd().StartsWith(PortPrefix);
		});
		Overrides.Add(FString::Printf(TEXT("(Port=%d, BindAddress=localhost)"), Port));
		GConfig->SetArray(*IniSection, *OverridesKey, Overrides, GEngineIni);
	}

	// Let SATISFACTORY_TOKEN fill in the bearer token when the (EditDefaultsOnly)
	// UPROPERTY is empty - which it is by default, so this is the only way a
	// user can actually set one. The provider reads the same variable, so one
	// env var configures both halves. Read once here: the environment is fixed
	// at process start, so changing it needs a game restart.
	if (Token.IsEmpty())
	{
		Token = FPlatformMisc::GetEnvironmentVariable(TEXT("SATISFACTORY_TOKEN"));
	}

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

	// Preview holograms are transient and time-limited (see HandlePreview);
	// a once-a-second sweep removes the expired ones. Coarse on purpose - a
	// TTL is a cleanup guarantee, not a precise schedule.
	GetWorldTimerManager().SetTimer(PreviewExpiryTimer, this, &ASTFApiServerSubsystem::PurgeExpiredPreviews, 1.0f, /*bLoop*/ true);
	UE_LOG(LogSatisfactoryTerraform, Log,
		TEXT("SatisfactoryTerraform API listening on 127.0.0.1:%d (loopback only; host-allowlist + CSRF guards active; %s)"),
		Port, Token.IsEmpty() ? TEXT("no auth token set") : TEXT("bearer token required"));
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
	GetWorldTimerManager().ClearTimer(PreviewExpiryTimer);
	RemoveAllPreviews();
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
	Bind(TEXT("/api/v1/players"), EHttpServerRequestVerbs::VERB_GET, &ASTFApiServerSubsystem::HandlePlayers);
	Bind(TEXT("/api/v1/world/buildables"), EHttpServerRequestVerbs::VERB_GET, &ASTFApiServerSubsystem::HandleWorldBuildables);
	Bind(TEXT("/api/v1/world/resource-nodes"), EHttpServerRequestVerbs::VERB_GET, &ASTFApiServerSubsystem::HandleWorldResourceNodes);
	Bind(TEXT("/api/v1/preview"),
		EHttpServerRequestVerbs::VERB_GET | EHttpServerRequestVerbs::VERB_POST | EHttpServerRequestVerbs::VERB_DELETE,
		&ASTFApiServerSubsystem::HandlePreview);
	Bind(TEXT("/api/v1/preview/:tf_id"), EHttpServerRequestVerbs::VERB_GET | EHttpServerRequestVerbs::VERB_DELETE, &ASTFApiServerSubsystem::HandlePreviewByID);
	Bind(TEXT("/api/v1/classes"), EHttpServerRequestVerbs::VERB_GET, &ASTFApiServerSubsystem::HandleClassCatalog);
	Bind(TEXT("/api/v1/classes/:class"), EHttpServerRequestVerbs::VERB_GET, &ASTFApiServerSubsystem::HandleBuildableClass);
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

namespace
{
	/** First value of a header, or empty. The UE HTTP server lowercases every
	  * header key on ingest (HttpConnectionRequestReadContext.cpp), so all
	  * lookups here use lowercase keys. */
	FString FirstHeader(const FHttpServerRequest& Request, const TCHAR* LowercaseKey)
	{
		const TArray<FString>* Values = Request.Headers.Find(LowercaseKey);
		return (Values && Values->Num() > 0) ? (*Values)[0] : FString();
	}

	/** True if the Host header names this machine. Accepts an empty host
	  * (HTTP/1.0 clients, some tools) since the loopback bind already means
	  * the connection reached us over the loopback interface; the check exists
	  * to defeat DNS rebinding, where the browser sends a *foreign* hostname. */
	bool IsLoopbackHost(const FString& HostHeader)
	{
		if (HostHeader.IsEmpty())
		{
			return true;
		}
		FString Host = HostHeader;
		// Strip a :port suffix. IPv6 literals are bracketed ("[::1]:8090"), so
		// only split on the last colon when there's no closing bracket after it.
		int32 Colon;
		if (Host.FindLastChar(TEXT(':'), Colon) && !Host.RightChop(Colon).Contains(TEXT("]")))
		{
			Host = Host.Left(Colon);
		}
		Host.TrimStartAndEndInline();
		// IPv6 loopback in a Host header is always bracketed per RFC 3986
		// ("[::1]"); a bare "::1" is malformed and correctly falls through to
		// a 403.
		return Host.Equals(TEXT("localhost"), ESearchCase::IgnoreCase)
			|| Host == TEXT("127.0.0.1")
			|| Host == TEXT("[::1]");
	}
}

bool ASTFApiServerSubsystem::CheckTransport(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) const
{
	// Host allowlist: the listener is pinned to loopback, but a page using DNS
	// rebinding connects to 127.0.0.1 while carrying an attacker hostname in
	// Host. Rejecting non-loopback hosts closes that path.
	if (!IsLoopbackHost(FirstHeader(Request, TEXT("host"))))
	{
		OnComplete(ErrorResponse(403, TEXT("host not allowed")));
		return false;
	}

	// Any Origin header means a browser is making a cross-origin request. The
	// Terraform client never sends one, so its mere presence is hostile.
	if (!FirstHeader(Request, TEXT("origin")).IsEmpty())
	{
		OnComplete(ErrorResponse(403, TEXT("cross-origin requests are not allowed")));
		return false;
	}

	// Mutating verbs must be application/json. text/plain, form, and multipart
	// are CORS "simple" content types a page can POST without a preflight;
	// requiring JSON forces a preflight (which this server never answers) for
	// any browser-driven write. The Go client always sends this on bodies.
	const bool bIsMutating = EnumHasAnyFlags(Request.Verb,
		EHttpServerRequestVerbs::VERB_POST | EHttpServerRequestVerbs::VERB_PATCH | EHttpServerRequestVerbs::VERB_PUT);
	if (bIsMutating && !FirstHeader(Request, TEXT("content-type")).StartsWith(TEXT("application/json")))
	{
		OnComplete(ErrorResponse(415, TEXT("Content-Type must be application/json")));
		return false;
	}

	return true;
}

bool ASTFApiServerSubsystem::CheckRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) const
{
	if (!CheckTransport(Request, OnComplete))
	{
		return false;
	}

	// Optional bearer token. Empty (default) means no auth, which is safe only
	// because of the loopback pin + the transport checks above; set
	// SATISFACTORY_TOKEN for any non-loopback deployment (see BeginPlay).
	if (Token.IsEmpty())
	{
		return true;
	}
	if (FirstHeader(Request, TEXT("authorization")) == FString::Printf(TEXT("Bearer %s"), *Token))
	{
		return true;
	}
	OnComplete(ErrorResponse(401, TEXT("missing or invalid bearer token")));
	return false;
}

bool ASTFApiServerSubsystem::HandleHealth(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// Health needs no token (it's how a client probes whether the mod is up
	// before it has anything to authenticate against), but it still runs the
	// transport guard so a rebinding/cross-origin page can't even confirm the
	// API exists.
	if (!CheckTransport(Request, OnComplete))
	{
		return true;
	}
	const TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("status"), TEXT("ok"));
	OnComplete(JsonResponse(200, Body));
	return true;
}

bool ASTFApiServerSubsystem::HandleWorld(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (!CheckRequest(Request, OnComplete))
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

bool ASTFApiServerSubsystem::HandleClassCatalog(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (!CheckRequest(Request, OnComplete))
	{
		return true;
	}

	// Loading every buildable class costs seconds the first time (hundreds of
	// Blueprint loads), and the answer never changes within a session, so it
	// is computed once and served from the cache afterwards.
	if (ClassCatalogJson.IsEmpty())
	{
		if (!bClassNameIndexBuilt)
		{
			BuildClassNameIndex();
		}

		TArray<FString> Names;
		ClassNameIndex.GetKeys(Names);
		Names.Sort();

		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FString& Name : Names)
		{
			if (!Name.StartsWith(TEXT("Build_")))
			{
				continue;
			}
			UClass* Class = Cast<UClass>(ClassNameIndex.FindRef(Name).TryLoad());
			if (!Class || !Class->IsChildOf(AFGBuildable::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract))
			{
				continue;
			}
			const AFGBuildable* CDO = Class->GetDefaultObject<AFGBuildable>();
			if (!CDO)
			{
				continue;
			}

			// Most specific first. Everything below "building" is a mechanism
			// the provider does not have yet; the point of this endpoint is
			// to count those honestly rather than let them hide behind the
			// generic transform-placement path, which would accept the class
			// and produce something broken.
			const TCHAR* Mechanism = TEXT("building");
			const TCHAR* Resource = TEXT("satisfactory_building");
			const TCHAR* Why = TEXT("");
			if (Name.StartsWith(TEXT("Build_Cheat")))
			{
				continue; // dev-only sinks/spawners, not placeable in a normal game
			}
			else if (Class->IsChildOf(AFGBuildableConveyorBelt::StaticClass()) || Class->IsChildOf(AFGBuildableConveyorLift::StaticClass()))
			{
				Mechanism = TEXT("belt");
				Resource = TEXT("satisfactory_belt");
			}
			else if (Class->IsChildOf(AFGBuildableWire::StaticClass()))
			{
				Mechanism = TEXT("power_line");
				Resource = TEXT("satisfactory_power_line");
			}
			else if (Class->IsChildOf(AFGBuildablePipeHyper::StaticClass()))
			{
				Mechanism = TEXT("hypertube");
				Resource = TEXT("satisfactory_hypertube");
			}
			else if (Class->IsChildOf(AFGBuildablePipeline::StaticClass()))
			{
				Mechanism = TEXT("pipeline");
				Resource = TEXT("satisfactory_pipeline");
			}
			else if (Class->IsChildOf(AFGBuildableRailroadTrack::StaticClass()))
			{
				Mechanism = TEXT("rail_track");
				Resource = TEXT("");
				Why = TEXT("a spline network with switches and signals bound to positions along it; no resource models that yet");
			}
			else if (Class->ImplementsInterface(UFGSplineBuildableInterface::StaticClass()))
			{
				Mechanism = TEXT("spline");
				Resource = TEXT("");
				Why = TEXT("routed along a path rather than placed at a point, and not one of the connection kinds the provider knows");
			}
			else if (Class->IsChildOf(AFGBuildableGeneratorGeoThermal::StaticClass()))
			{
				// Sits on a geyser like an extractor does, but it is a
				// generator, not an AFGBuildableResourceExtractorBase, so the
				// node-binding path below does not reach it. Called out
				// explicitly so it does not fall through to "building" and
				// get placed on nothing.
				Mechanism = TEXT("node_bound");
				Resource = TEXT("");
				Why = TEXT("a generator bound to a geyser; not an extractor, and not modelled yet");
			}
			else if (Class->IsChildOf(AFGBuildableResourceExtractorBase::StaticClass()))
			{
				// Miners, water extractors, oil extractors, fracking: placed
				// ON a world feature found via GET /world/resource-nodes and
				// referenced by node_id, not at a coordinate.
				Mechanism = TEXT("node_bound");
				Resource = TEXT("satisfactory_extractor");
			}
			else if (Class->IsChildOf(AFGBuildableRailroadStation::StaticClass()) || Class->IsChildOf(AFGBuildableTrainPlatform::StaticClass()) ||
				Class->IsChildOf(AFGBuildableRailroadSignal::StaticClass()) || Class->IsChildOf(AFGBuildableRailroadSwitchControl::StaticClass()))
			{
				Mechanism = TEXT("rail");
				Resource = TEXT("");
				Why = TEXT("attaches to track rather than standing alone");
			}
			else if (CDO->GetLightweightInstanceData() != nullptr)
			{
				Mechanism = TEXT("foundation");
				Resource = TEXT("satisfactory_foundation");
			}

			// Placeable today, but with state the contract cannot express.
			// Terraform can put it down; it cannot make it do anything.
			const TCHAR* Settings = TEXT("");
			if (Class->IsChildOf(AFGBuildableSignBase::StaticClass()))
			{
				Settings = TEXT("text, icon and colours");
			}
			else if (Class->IsChildOf(AFGBuildableSplitterSmart::StaticClass()))
			{
				Settings = TEXT("per-output item filter rules");
			}
			else if (Class->IsChildOf(AFGBuildableCircuitSwitch::StaticClass()))
			{
				Settings = TEXT("on/off state (and priority tier for priority switches)");
			}
			else if (Class->IsChildOf(AFGBuildableLightSource::StaticClass()))
			{
				Settings = TEXT("colour, intensity and on/off");
			}

			const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("class"), Name);
			Json->SetStringField(TEXT("display_name"), CDO->mDisplayName.ToString());
			Json->SetStringField(TEXT("mechanism"), Mechanism);
			Json->SetBoolField(TEXT("supported"), Resource[0] != 0);
			if (Resource[0])
			{
				Json->SetStringField(TEXT("resource"), Resource);
			}
			if (Why[0])
			{
				Json->SetStringField(TEXT("why_unsupported"), Why);
			}
			if (Settings[0])
			{
				Json->SetStringField(TEXT("settings_not_modelled"), Settings);
			}
			Items.Add(MakeShared<FJsonValueObject>(Json));
		}

		const auto Writer = TJsonWriterFactory<>::Create(&ClassCatalogJson);
		FJsonSerializer::Serialize(Items, Writer);
		UE_LOG(LogSatisfactoryTerraform, Log, TEXT("Built class catalog: %d placeable classes"), Items.Num());
	}

	auto Response = FHttpServerResponse::Create(ClassCatalogJson, TEXT("application/json"));
	Response->Code = EHttpServerResponseCodes::Ok;
	OnComplete(MoveTemp(Response));
	return true;
}

bool ASTFApiServerSubsystem::HandleBuildableClass(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (!CheckRequest(Request, OnComplete))
	{
		return true;
	}

	const FString ClassName = Request.PathParams.FindRef(TEXT("class"));
	FString Error;
	UClass* Class = ResolveBuildableClass(ClassName, Error);
	if (!Class)
	{
		OnComplete(ErrorResponse(404, FString::Printf(TEXT("no buildable class named %s"), *ClassName)));
		return true;
	}

	// Read the class default object. Clearance is authored per class
	// (mClearanceData is EditDefaultsOnly), so this needs nothing spawned and
	// touches no world state - which is what makes the endpoint safe to call
	// during a Terraform plan. GetClearanceData_Implementation is public and,
	// unusually for this codebase, has a real body in the available source
	// (it appends mClearanceData), so calling it on a CDO is well understood
	// rather than assumed.
	const AFGBuildable* CDO = Class->GetDefaultObject<AFGBuildable>();
	TArray<FFGClearanceData> ClearanceData;
	if (CDO)
	{
		CDO->GetClearanceData_Implementation(ClearanceData);
	}

	const TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("class"), Class->GetName());

	const auto VecJson = [](const FVector& V)
	{
		const TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetNumberField(TEXT("x"), V.X);
		Out->SetNumberField(TEXT("y"), V.Y);
		Out->SetNumberField(TEXT("z"), V.Z);
		return Out;
	};

	TArray<TSharedPtr<FJsonValue>> Boxes;
	FBox Union(ForceInit);
	bool bAnyValid = false;
	for (const FFGClearanceData& Entry : ClearanceData)
	{
		if (!Entry.IsValid())
		{
			continue; // a declared-but-empty box reserves nothing
		}
		// Fold in the relative transform so callers get boxes in the
		// buildable's own frame and never have to know about it.
		const FBox Box = Entry.GetTransformedClearanceBox();

		const TSharedPtr<FJsonObject> BoxJson = MakeShared<FJsonObject>();
		BoxJson->SetStringField(TEXT("type"), ClearanceTypeName(Entry.Type));
		BoxJson->SetObjectField(TEXT("min"), VecJson(Box.Min));
		BoxJson->SetObjectField(TEXT("max"), VecJson(Box.Max));
		Boxes.Add(MakeShared<FJsonValueObject>(BoxJson));

		Union = bAnyValid ? Union + Box : Box;
		bAnyValid = true;
	}
	Body->SetArrayField(TEXT("clearance"), Boxes);

	// Omit bounds entirely when nothing was declared. Reporting a zero box
	// would read as "needs no room" and quietly stack buildables; absent makes
	// a caller that depends on a size fail loudly instead.
	if (bAnyValid)
	{
		const TSharedPtr<FJsonObject> Bounds = MakeShared<FJsonObject>();
		Bounds->SetObjectField(TEXT("min"), VecJson(Union.Min));
		Bounds->SetObjectField(TEXT("max"), VecJson(Union.Max));
		Bounds->SetObjectField(TEXT("size"), VecJson(Union.GetSize()));
		Body->SetObjectField(TEXT("bounds"), Bounds);
	}

	OnComplete(JsonResponse(200, Body));
	return true;
}

bool ASTFApiServerSubsystem::HandlePlayers(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (!CheckRequest(Request, OnComplete))
	{
		return true;
	}

	// Plain engine API on purpose. FactoryGame offers GetLocalPlayerController
	// and AFGPlayerController::GetPawnLocation, but both are stub-only in the
	// available source; the controller iterator is real engine code we can
	// read, and works the same on a listen or dedicated server.
	TArray<TSharedPtr<FJsonValue>> Players;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* PC = It->Get();
		if (!PC)
		{
			continue;
		}
		const APawn* Pawn = PC->GetPawn();
		if (!Pawn)
		{
			continue; // connected but not spawned in yet
		}
		const TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		if (const APlayerState* State = PC->PlayerState)
		{
			Entry->SetStringField(TEXT("name"), State->GetPlayerName());
		}
		const FVector Loc = Pawn->GetActorLocation();
		const TSharedPtr<FJsonObject> Location = MakeShared<FJsonObject>();
		Location->SetNumberField(TEXT("x"), Loc.X);
		Location->SetNumberField(TEXT("y"), Loc.Y);
		Location->SetNumberField(TEXT("z"), Loc.Z);
		Entry->SetObjectField(TEXT("location"), Location);
		Entry->SetNumberField(TEXT("yaw"), Pawn->GetActorRotation().Yaw);
		Players.Add(MakeShared<FJsonValueObject>(Entry));
	}

	FString Out;
	const auto Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Players, Writer);
	auto Response = FHttpServerResponse::Create(Out, TEXT("application/json"));
	Response->Code = EHttpServerResponseCodes::Ok;
	OnComplete(MoveTemp(Response));
	return true;
}

bool ASTFApiServerSubsystem::HandleWorldBuildables(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (!CheckRequest(Request, OnComplete))
	{
		return true;
	}

	// A spatial filter is mandatory, not a convenience: a mature save holds
	// tens of thousands of buildables and serialising all of them would be
	// unusable. Callers say where and how far.
	double CenterX = 0, CenterY = 0, CenterZ = 0, Radius = 0;
	const auto Param = [&Request](const TCHAR* Key, double& Out)
	{
		if (const FString* Raw = Request.QueryParams.Find(Key))
		{
			Out = FCString::Atod(**Raw);
			return true;
		}
		return false;
	};
	if (!Param(TEXT("x"), CenterX) || !Param(TEXT("y"), CenterY) || !Param(TEXT("z"), CenterZ) || !Param(TEXT("radius"), Radius))
	{
		OnComplete(ErrorResponse(422, TEXT("x, y, z and radius query parameters are all required")));
		return true;
	}
	if (Radius <= 0 || Radius > 100000)
	{
		OnComplete(ErrorResponse(422, TEXT("radius must be between 0 and 100000 centimetres")));
		return true;
	}
	const FVector Center(CenterX, CenterY, CenterZ);
	const double RadiusSq = Radius * Radius;

	// tf_id lookup, so the response can say which of these Terraform already
	// manages - an exporter needs to know what it would be duplicating.
	ASTFRegistrySubsystem* Registry = ASTFRegistrySubsystem::Get(GetWorld());
	TMap<FString, FString> ActorKeyToTFID;    // actor path name -> tf_id
	TMap<FString, FString> LocationKeyToTFID; // rounded location -> tf_id
	const auto LocationKey = [](const FVector& V)
	{
		return FString::Printf(TEXT("%.0f,%.0f,%.0f"), V.X, V.Y, V.Z);
	};
	if (Registry)
	{
		for (const ASTFRegistrySubsystem::FEntry& Entry : Registry->GetAll())
		{
			if (Entry.IsLightweight())
			{
				LocationKeyToTFID.Add(LocationKey(Entry.LightweightRef.GetBuildableTransform().GetLocation()), Entry.TFID);
			}
			else if (Entry.Buildable)
			{
				ActorKeyToTFID.Add(Entry.Buildable->GetPathName(), Entry.TFID);
			}
		}
	}

	// Collected first, serialised second: a belt reports the two buildables it
	// joins by their index in this response, and the far end is often gathered
	// after the belt itself.
	struct FWorldItem
	{
		UClass* Class = nullptr;
		FTransform Transform;
		AFGBuildable* Actor = nullptr;
		bool bLightweight = false;
		FString TFID;
	};
	TArray<FWorldItem> Items;
	TMap<const AFGBuildable*, int32> ActorToIndex;

	// Actors. GetAllBuildablesRef is a header-inline accessor over the
	// subsystem's own list, so this does not walk the whole world.
	if (AFGBuildableSubsystem* BuildableSubsystem = AFGBuildableSubsystem::Get(GetWorld()))
	{
		for (AFGBuildable* Buildable : BuildableSubsystem->GetAllBuildablesRef())
		{
			if (!IsValid(Buildable) || FVector::DistSquared(Buildable->GetActorLocation(), Center) > RadiusSq)
			{
				continue;
			}
			ActorToIndex.Add(Buildable, Items.Num());
			Items.Add(FWorldItem{Buildable->GetClass(), Buildable->GetActorTransform(), Buildable, false,
				ActorKeyToTFID.FindRef(Buildable->GetPathName())});
		}
	}

	// Lightweight instances are NOT actors and never appear in the list above -
	// foundations and walls live only here, so an export that skipped this
	// would silently omit every floor.
	if (AFGLightweightBuildableSubsystem* LightweightSubsystem = AFGLightweightBuildableSubsystem::Get(GetWorld()))
	{
		for (const auto& Pair : LightweightSubsystem->GetAllLightweightBuildableInstances())
		{
			UClass* Class = Pair.Key.Get();
			if (!Class)
			{
				continue;
			}
			for (const FRuntimeBuildableInstanceData& Data : Pair.Value)
			{
				if (!Data.IsValidOnLoad() || FVector::DistSquared(Data.Transform.GetLocation(), Center) > RadiusSq)
				{
					continue;
				}
				Items.Add(FWorldItem{Class, Data.Transform, nullptr, true,
					LocationKeyToTFID.FindRef(LocationKey(Data.Transform.GetLocation()))});
			}
		}
	}

	// Which buildable+connector a belt or wire actually attaches to. The
	// connector index must be produced the same way SpawnConnection consumes
	// it, or an exported belt would be re-created against the wrong port -
	// hence the shared GetFactoryConnector/GetPowerConnector ordering.
	const auto EndpointJson = [&ActorToIndex](const UActorComponent* Mating) -> TSharedPtr<FJsonObject>
	{
		if (!Mating)
		{
			return nullptr;
		}
		AFGBuildable* Owner = Cast<AFGBuildable>(Mating->GetOwner());
		if (!Owner)
		{
			return nullptr;
		}
		const int32* Index = ActorToIndex.Find(Owner);
		if (!Index)
		{
			return nullptr; // the other end is outside the exported radius
		}

		int32 Connector = INDEX_NONE;
		if (const UFGFactoryConnectionComponent* Factory = Cast<UFGFactoryConnectionComponent>(Mating))
		{
			TInlineComponentArray<UFGFactoryConnectionComponent*> Connectors;
			Owner->GetComponents(Connectors);
			UFGFactoryConnectionComponent::SortComponentList(Connectors);
			Connector = Connectors.IndexOfByKey(Factory);
		}
		else if (const UFGPowerConnectionComponent* Power = Cast<UFGPowerConnectionComponent>(Mating))
		{
			TInlineComponentArray<UFGPowerConnectionComponent*> Connectors;
			Owner->GetComponents(Connectors);
			Connector = Connectors.IndexOfByKey(Power);
		}
		else if (const UFGPipeConnectionComponentHyper* Hyper = Cast<UFGPipeConnectionComponentHyper>(Mating))
		{
			// Indices are per-kind, matching GetPipeConnector on the spawn side.
			TInlineComponentArray<UFGPipeConnectionComponentHyper*> Connectors;
			Owner->GetComponents(Connectors);
			Connector = Connectors.IndexOfByKey(Hyper);
		}
		else if (const UFGPipeConnectionComponent* Fluid = Cast<UFGPipeConnectionComponent>(Mating))
		{
			TInlineComponentArray<UFGPipeConnectionComponent*> Connectors;
			Owner->GetComponents(Connectors);
			Connector = Connectors.IndexOfByKey(Fluid);
		}
		if (Connector == INDEX_NONE)
		{
			// Reporting a guessed index would produce configuration that
			// applies and wires the wrong port. Say nothing instead.
			return nullptr;
		}

		const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("index"), *Index);
		Json->SetNumberField(TEXT("connector"), Connector);
		return Json;
	};

	TArray<TSharedPtr<FJsonValue>> Out;
	for (int32 i = 0; i < Items.Num(); ++i)
	{
		const FWorldItem& It = Items[i];
		const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("index"), i);
		if (!It.TFID.IsEmpty())
		{
			Json->SetStringField(TEXT("tf_id"), It.TFID);
		}
		Json->SetStringField(TEXT("class"), It.Class->GetName());
		Json->SetBoolField(TEXT("lightweight"), It.bLightweight);
		const FVector Loc = It.Transform.GetLocation();
		const TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
		T->SetNumberField(TEXT("x"), Loc.X);
		T->SetNumberField(TEXT("y"), Loc.Y);
		T->SetNumberField(TEXT("z"), Loc.Z);
		T->SetNumberField(TEXT("yaw"), It.Transform.Rotator().Yaw);
		Json->SetObjectField(TEXT("transform"), T);

		if (It.Actor)
		{
			AppendProductionFields(Json, It.Actor);
		}

		// A belt or wire is defined by what it joins, not by where it sits.
		TSharedPtr<FJsonObject> From, To;
		if (const AFGBuildableConveyorBase* Conveyor = Cast<AFGBuildableConveyorBase>(It.Actor))
		{
			// Connection0 is the belt's input, so it mates with the *output*
			// connector of the buildable upstream - matching SpawnConnection.
			const UFGFactoryConnectionComponent* In = Conveyor->GetConnection0();
			const UFGFactoryConnectionComponent* OutC = Conveyor->GetConnection1();
			From = EndpointJson(In ? In->GetConnection() : nullptr);
			To = EndpointJson(OutC ? OutC->GetConnection() : nullptr);
		}
		else if (const AFGBuildableWire* Wire = Cast<AFGBuildableWire>(It.Actor))
		{
			From = EndpointJson(Wire->GetConnection(0));
			To = EndpointJson(Wire->GetConnection(1));
		}
		else if (const AFGBuildablePipeBase* Pipe = Cast<AFGBuildablePipeBase>(It.Actor))
		{
			// Same shape as a belt: connection0 is the pipe end joined to the
			// upstream buildable, connection1 the downstream one.
			const UFGPipeConnectionComponentBase* In = Pipe->GetConnection0();
			const UFGPipeConnectionComponentBase* OutC = Pipe->GetConnection1();
			From = EndpointJson(In ? In->GetConnection() : nullptr);
			To = EndpointJson(OutC ? OutC->GetConnection() : nullptr);
		}
		if (From.IsValid() && To.IsValid())
		{
			const TSharedPtr<FJsonObject> Connects = MakeShared<FJsonObject>();
			Connects->SetObjectField(TEXT("from"), From);
			Connects->SetObjectField(TEXT("to"), To);
			Json->SetObjectField(TEXT("connects"), Connects);
		}

		Out.Add(MakeShared<FJsonValueObject>(Json));
	}

	FString Body;
	const auto Writer = TJsonWriterFactory<>::Create(&Body);
	FJsonSerializer::Serialize(Out, Writer);
	auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
	Response->Code = EHttpServerResponseCodes::Ok;
	OnComplete(MoveTemp(Response));
	return true;
}

bool ASTFApiServerSubsystem::HandleBuildables(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (!CheckRequest(Request, OnComplete))
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
	if (!CheckRequest(Request, OnComplete))
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
		//
		// An extractor also frees its node first. The game's own dismantle
		// most likely does this too (Destroyed() -> DisconnectExtractableResource
		// is in the header), but that body is stub-only locally, and a node
		// left marked occupied is a node no extractor - ours or the player's -
		// can ever use again. Setting it false twice costs nothing.
		if (const AFGBuildableResourceExtractorBase* Extractor = Cast<AFGBuildableResourceExtractorBase>(Buildable))
		{
			if (IFGExtractableResourceInterface* Resource = Extractor->GetExtractableResource().GetInterface())
			{
				Resource->SetIsOccupied(false);
			}
		}
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
	if (!CheckRequest(Request, OnComplete))
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
	if (!CheckRequest(Request, OnComplete))
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
		OutError = TEXT("class must be a belt (Build_ConveyorBeltMkN_C), Build_PowerLine_C, a pipeline or a hypertube");
		return nullptr;
	}
	UClass* Class = ResolveClassByName(ClassName, AFGBuildable::StaticClass(), OutError);
	if (!Class)
	{
		return nullptr;
	}
	if (!Class->IsChildOf(AFGBuildableConveyorBelt::StaticClass()) &&
		!Class->IsChildOf(AFGBuildableWire::StaticClass()) &&
		!Class->IsChildOf(AFGBuildablePipeBase::StaticClass()))
	{
		OutError = TEXT("class must be a belt (Build_ConveyorBeltMkN_C), Build_PowerLine_C, a pipeline or a hypertube");
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
	// Two independent capabilities: a recipe (manufacturers only) and a clock
	// (anything whose class can change potential - manufacturers, extractors,
	// generators). A buildable with neither has nothing to patch.
	AFGBuildableManufacturer* Manufacturer = Cast<AFGBuildableManufacturer>(Buildable);
	AFGBuildableFactory* Factory = Cast<AFGBuildableFactory>(Buildable);
	const bool bHasClock = Factory && Factory->GetCanChangePotential();
	if (!Manufacturer && !bHasClock)
	{
		OutStatus = 422;
		OutError = TEXT("this buildable has no recipe/clock_speed to patch");
		return nullptr;
	}

	FString RecipeClassName;
	if (Body->TryGetStringField(TEXT("recipe"), RecipeClassName) && !RecipeClassName.IsEmpty())
	{
		if (!Manufacturer)
		{
			OutStatus = 422;
			OutError = TEXT("this buildable has no recipe to patch");
			return nullptr;
		}
		UClass* RecipeClass = ResolveRecipeClass(RecipeClassName, OutError);
		if (!RecipeClass)
		{
			OutStatus = 422;
			return nullptr;
		}
		// Same check as the spawn path: PATCH reached the identical broken
		// state (a smelter reporting a constructor recipe) before this.
		if (!RecipeFitsBuildable(RecipeClass, Buildable->GetClass()))
		{
			OutStatus = 422;
			OutError = FString::Printf(TEXT("%s cannot be produced in %s"), *RecipeClassName, *Buildable->GetClass()->GetName());
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
		if (!bHasClock)
		{
			OutStatus = 422;
			OutError = TEXT("this buildable has no clock_speed to patch");
			return nullptr;
		}
		Factory->SetPendingPotential(ClockSpeed);
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
		// Confirmed live: the game accepts a recipe its machine cannot make
		// (a constructor recipe on a smelter), and even displays it in the
		// machine UI, so nothing downstream surfaces the mistake - the factory
		// simply never produces, and Terraform reports zero drift on it.
		if (!RecipeFitsBuildable(RecipeClass, Class))
		{
			OutStatus = 422;
			OutError = FString::Printf(TEXT("%s cannot be produced in %s"), *RecipeClassName, *Class->GetName());
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

	// Where it goes: a transform for ordinary classes, a world feature (plus
	// the transform the feature dictates) for extractors. Same resolver the
	// preview uses, so the two never disagree about what is a valid request.
	FResolvedPlacement Placement;
	if (!ResolvePlacement(Class, Body, /*bCheckOccupied*/ true, Placement, OutStatus, OutError))
	{
		return nullptr;
	}
	const FTransform& Transform = Placement.Transform;

	// A standing preview hologram for this tf_id is the ghost of exactly this
	// buildable; the real thing replaces it (see HandlePreview).
	RemovePreview(TFID);

	// Spawn through the buildable subsystem so the buildable ticks in the
	// factory tick group like a hologram-built one would.
	// Capture which lightweight instances of this class exist BEFORE spawning.
	// If the game converts this buildable during FinishSpawning below, the one
	// index that appears is unambiguously ours - which matters when something
	// already occupies the target position (see AdoptSpawnedLightweight).
	const TSet<int32> LightweightIndicesBefore = Registry->SnapshotLiveIndices(Class);

	AFGBuildableSubsystem* BuildableSubsystem = AFGBuildableSubsystem::Get(GetWorld());
	AFGBuildable* Buildable = BuildableSubsystem->BeginSpawnBuildable(Class, Transform);
	if (!Buildable)
	{
		OutStatus = 422;
		OutError = TEXT("game refused to spawn that buildable");
		return nullptr;
	}

	// Extractors learn their node BEFORE FinishSpawning, mirroring the
	// hologram: AFGResourceExtractorHologram::ConfigureActor runs on the
	// deferred-spawned actor and sets the extractable resource, and only then
	// does BeginPlay (inside FinishSpawning) see a bound extractor. Binding
	// afterwards would leave BeginPlay's own setup - whatever of it keys off
	// the resource - already done with a null node.
	TScriptInterface<IFGExtractableResourceInterface> Resource;
	if (Placement.ResourceActor)
	{
		Resource = TScriptInterface<IFGExtractableResourceInterface>(Placement.ResourceActor);
		if (AFGBuildableResourceExtractorBase* Extractor = Cast<AFGBuildableResourceExtractorBase>(Buildable))
		{
			Extractor->SetExtractableResource(Resource);
		}
	}

	Buildable->FinishSpawning(Transform);

	// The node is the game's one-extractor-per-feature lock. The real
	// BeginPlay very probably marks it (the hologram flow relies on that), but
	// that body is stub-only locally - so mark it here if it isn't, which is a
	// no-op when the game already did.
	if (Resource && !Resource->IsOccupied() && Resource->CanBecomeOccupied())
	{
		Resource->SetIsOccupied(true);
	}

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
	}
	// Clock on anything that has one (manufacturer, extractor, generator) -
	// the same predicate AppendProductionFields reports it by.
	if (AFGBuildableFactory* Factory = Cast<AFGBuildableFactory>(Buildable))
	{
		if (Factory->GetCanChangePotential())
		{
			Factory->SetPendingPotential(ClockSpeed);
		}
	}

	// Simple structural buildables (foundations, walls, ramps, ...) are
	// converted to Satisfactory's memory-efficient lightweight
	// representation by the game ITSELF, synchronously, inside BeginPlay -
	// i.e. during the FinishSpawning call above: AFGBuildable::BeginPlay
	// early-outs into HandleLightweightAddition() when
	// ShouldConvertToLightweight(), adding the instance and destroying the
	// actor (confirmed in the real source). An earlier version of this code
	// did its own AddFromBuildable() here on top of that, which silently
	// DOUBLED every such buildable - two pixel-perfectly overlapping
	// instances, ours tracked and the game's orphaned - the root cause of
	// the whole "phantom tile" bug family (deleting via the API removed our
	// copy while the orphan stayed standing). So: never convert here. If
	// the game converted (the actor is pending destruction), register the
	// game's own instance, re-found by class + location.
	if (Buildable->IsActorBeingDestroyed() || Buildable->ManagedByLightweightBuildableSubsystem())
	{
		const ASTFRegistrySubsystem::EAdoptResult Adopted =
			Registry->AdoptSpawnedLightweight(TFID, Class, Transform, LightweightIndicesBefore);
		if (Adopted == ASTFRegistrySubsystem::EAdoptResult::Adopted)
		{
			UE_LOG(LogSatisfactoryTerraform, Log, TEXT("Spawned %s as %s (lightweight)"), *Class->GetName(), *TFID);
			OutStatus = 201;
			return LightweightToJson(TFID, *Registry->FindLightweight(TFID));
		}
		if (Adopted == ASTFRegistrySubsystem::EAdoptResult::Occupied)
		{
			// Something is already there. AdoptSpawnedLightweight has already
			// removed the instance this request created, so the world is
			// unchanged. Refusing matters: two instances at one position are
			// indistinguishable on reload and strand each other permanently.
			OutStatus = 409;
			OutError = TEXT("a buildable of that class already exists at that position");
			return nullptr;
		}
		if (Buildable->IsActorBeingDestroyed())
		{
			// Converted but we couldn't identify the instance - should not
			// happen (the add is synchronous); fail loudly rather than
			// registering a dead actor pointer.
			OutStatus = 500;
			OutError = TEXT("buildable was converted to a lightweight instance but the instance could not be resolved");
			return nullptr;
		}
		// Eligible class but the game didn't convert (e.g. no instance
		// data) and the actor is still alive - fall through and keep it as
		// a regular full-actor registration below.
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
	const bool bIsHyper = Class->IsChildOf(AFGBuildablePipeHyper::StaticClass());
	const bool bIsPipe = !bIsHyper && Class->IsChildOf(AFGBuildablePipeline::StaticClass());

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

		// Wire the belt INTO the chain: source output -> belt input, belt
		// output -> destination input. An earlier version connected
		// FromConn straight to ToConn and left the belt's own connectors
		// dangling - items still moved (the direct link is honoured) but
		// the belt was decorative, and the world was in a state vanilla can
		// never produce: a machine connector wired to another machine
		// connector. That crashes the game on dismantle of a conveyor
		// attachment, whose Dismantle_Implementation assumes anything on
		// its connectors is a conveyor and passes the failed cast straight
		// into Execute_CanDismantle (check(O != NULL) - see repo issue #2).
		//
		// mConnection0 is the input and mConnection1 the output, always in
		// that order (FGBuildableConveyorBase.h), and the spline runs from
		// FromConn to ToConn, so the mapping is unambiguous. SetConnection
		// returns void, so success is verified via GetConnection() (a plain
		// inline accessor, not a stub); calling from both ends keeps it
		// correct whether or not the real implementation is two-sided.
		AFGBuildableConveyorBase* ConveyorBase = Cast<AFGBuildableConveyorBase>(Belt);
		UFGFactoryConnectionComponent* BeltIn = ConveyorBase ? ConveyorBase->GetConnection0() : nullptr;
		UFGFactoryConnectionComponent* BeltOut = ConveyorBase ? ConveyorBase->GetConnection1() : nullptr;
		if (!BeltIn || !BeltOut)
		{
			Belt->Destroy();
			OutStatus = 422;
			OutError = TEXT("that belt class has no usable input/output connectors");
			return nullptr;
		}

		FromConn->SetConnection(BeltIn);
		BeltIn->SetConnection(FromConn);
		BeltOut->SetConnection(ToConn);
		ToConn->SetConnection(BeltOut);

		if (FromConn->GetConnection() != BeltIn || BeltIn->GetConnection() != FromConn ||
			BeltOut->GetConnection() != ToConn || ToConn->GetConnection() != BeltOut)
		{
			Belt->Destroy();
			OutStatus = 422;
			OutError = TEXT("game refused to connect those factory connectors (already connected to something else?)");
			return nullptr;
		}

		Connection = Belt;
	}
	else if (bIsPipe || bIsHyper)
	{
		UFGPipeConnectionComponentBase* FromConn = GetPipeConnector(FromBuildable, Endpoints.FromConnector, bIsHyper);
		UFGPipeConnectionComponentBase* ToConn = GetPipeConnector(ToBuildable, Endpoints.ToConnector, bIsHyper);
		if (!FromConn || !ToConn)
		{
			OutStatus = 422;
			OutError = TEXT("from/to connector index out of range for that buildable");
			return nullptr;
		}

		// Spawn the pipe with the game's own helper, which paths a spline from
		// one connector to the other and places the pipe's two connection
		// components the way the pipe hologram does. The hand-rolled spline
		// spawn used for belts left the pipe's connection0 facing the wrong
		// way: live, every typed (producer/consumer) machine connector refused
		// it through CanConnectTo while the junction's untyped one accepted
		// it, and connection1 was accepted by everything.
		AFGBuildable* PipeActor = UFGTestBlueprintFunctionLibrary::SpawnSplineBuildable(Class, FromConn, ToConn, nullptr);
		if (!PipeActor)
		{
			OutStatus = 422;
			OutError = TEXT("game refused to spawn that pipe");
			return nullptr;
		}

		AFGBuildablePipeBase* PipeBase = Cast<AFGBuildablePipeBase>(PipeActor);
		UFGPipeConnectionComponentBase* PipeIn = PipeBase ? PipeBase->GetConnection0() : nullptr;
		UFGPipeConnectionComponentBase* PipeOut = PipeBase ? PipeBase->GetConnection1() : nullptr;
		if (!PipeIn || !PipeOut)
		{
			PipeActor->Destroy();
			OutStatus = 422;
			OutError = TEXT("that pipe class has no usable connectors");
			return nullptr;
		}

		// The helper may or may not join the ends (its comment only promises
		// the path). Join whatever is still loose, machine connector against
		// the pipe's own end, after asking the game whether that join is legal
		// - every refusal reports everything the check could have looked at.
		const auto Describe = [](const UFGPipeConnectionComponentBase* C)
		{
			return FString::Printf(TEXT("type=%d connected=%d at (%.0f,%.0f,%.0f) normal (%.2f,%.2f,%.2f)"),
				static_cast<int32>(C->GetPipeConnectionType()), C->IsConnected() ? 1 : 0,
				C->GetComponentLocation().X, C->GetComponentLocation().Y, C->GetComponentLocation().Z,
				C->GetConnectorNormal().X, C->GetConnectorNormal().Y, C->GetConnectorNormal().Z);
		};
		const bool bJoinedA = FromConn->GetConnection() == PipeIn && PipeIn->GetConnection() == FromConn;
		const bool bJoinedB = PipeOut->GetConnection() == ToConn && ToConn->GetConnection() == PipeOut;
		if (!bJoinedA || !bJoinedB)
		{
			const bool bCanA = bJoinedA || FromConn->CanConnectTo(PipeIn);
			const bool bCanB = bJoinedB || PipeOut->CanConnectTo(ToConn);
			const bool bCompatA = bJoinedA || FromConn->CheckCompatibility(PipeIn, nullptr);
			const bool bCompatB = bJoinedB || PipeOut->CheckCompatibility(ToConn, nullptr);
			const FString Diagnostics = FString::Printf(
				TEXT("from[%s] pipe0[%s] pipe1[%s] to[%s]; joined by spawn from/pipe0=%d pipe1/to=%d; canConnect from->pipe0=%d pipe1->to=%d; compatible from/pipe0=%d pipe1/to=%d"),
				*Describe(FromConn), *Describe(PipeIn), *Describe(PipeOut), *Describe(ToConn),
				bJoinedA ? 1 : 0, bJoinedB ? 1 : 0, bCanA ? 1 : 0, bCanB ? 1 : 0, bCompatA ? 1 : 0, bCompatB ? 1 : 0);
			if (!bCanA || !bCanB || !bCompatA || !bCompatB)
			{
				UE_LOG(LogSatisfactoryTerraform, Warning, TEXT("Pipe %s: join refused; %s"), *TFID, *Diagnostics);
				PipeActor->Destroy();
				OutStatus = 422;
				OutError = FString::Printf(TEXT("the game refused that pipe joint (wrong connector direction or kind, or already connected): %s"), *Diagnostics);
				return nullptr;
			}
			if (!bJoinedA)
			{
				UFGTestBlueprintFunctionLibrary::MakeConnectionBetweenComponents(FromConn, PipeIn);
			}
			if (!bJoinedB)
			{
				UFGTestBlueprintFunctionLibrary::MakeConnectionBetweenComponents(PipeOut, ToConn);
			}
		}

		// Success is read back through GetConnection from both ends, since
		// nothing above returns a verdict.
		if (FromConn->GetConnection() != PipeIn || PipeIn->GetConnection() != FromConn ||
			PipeOut->GetConnection() != ToConn || ToConn->GetConnection() != PipeOut)
		{
			PipeActor->Destroy();
			OutStatus = 422;
			OutError = TEXT("game refused to connect those pipe connectors (already connected to something else?)");
			return nullptr;
		}

		Connection = PipeActor;
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

// ---------------------------------------------------------------------------
// Resource nodes and node-bound placement (extractors)
// ---------------------------------------------------------------------------

namespace
{
	const TCHAR* PurityName(EResourcePurity Purity)
	{
		switch (Purity)
		{
		case RP_Inpure: return TEXT("impure");
		case RP_Normal: return TEXT("normal");
		case RP_Pure:   return TEXT("pure");
		default:        return TEXT("");
		}
	}

	const TCHAR* FormName(EResourceForm Form)
	{
		switch (Form)
		{
		case EResourceForm::RF_SOLID:  return TEXT("solid");
		case EResourceForm::RF_LIQUID: return TEXT("liquid");
		case EResourceForm::RF_GAS:    return TEXT("gas");
		default:                       return TEXT("");
		}
	}

	/** The wire "type" of a node actor. Not a closed set, per the contract. */
	const TCHAR* NodeTypeName(const AFGResourceNodeBase* Node)
	{
		if (Node->IsA<AFGResourceNodeFrackingCore>())
		{
			return TEXT("fracking");
		}
		if (Node->IsA<AFGResourceNodeFrackingSatellite>())
		{
			return TEXT("fracking_satellite");
		}
		if (Node->GetResourceNodeType() == EResourceNodeType::Geyser)
		{
			return TEXT("geyser");
		}
		if (const TSubclassOf<UFGResourceDescriptor> Resource = Node->GetResourceClass())
		{
			if (Resource->IsChildOf(UFGResourceDescriptorGeyser::StaticClass()))
			{
				return TEXT("geyser");
			}
		}
		return TEXT("node");
	}

	/** Where an extractor goes when placed on Resource. The feature decides
	  * (that is what GetPlacementLocation/Rotation are for - the hologram asks
	  * the same thing), with the aim point as the fallback for a feature that
	  * answers zero. Hit is the point the "build gun" is aiming at: the node
	  * itself for nodes, a chosen surface point for water. */
	FTransform PlacementOn(IFGExtractableResourceInterface* Resource, const FVector& Hit)
	{
		FVector Loc = Resource->GetPlacementLocation(Hit);
		if (Loc.IsNearlyZero())
		{
			Loc = Hit;
		}
		const FRotator Rot = Resource->GetPlacementRotation(Hit);
		return FTransform(Rot, Loc);
	}

	/** Surface box of a water volume; x/y centre + Max.Z is the default pump spot. */
	FBox WaterBounds(const AFGWaterVolume* Water)
	{
		return Water->GetComponentsBoundingBox(/*bNonColliding*/ true);
	}

	TSharedPtr<FJsonObject> Vec3Json(const FVector& V)
	{
		const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("x"), V.X);
		Json->SetNumberField(TEXT("y"), V.Y);
		Json->SetNumberField(TEXT("z"), V.Z);
		return Json;
	}

	TUniquePtr<FHttpServerResponse> NoContentResponse()
	{
		auto Response = FHttpServerResponse::Create(TEXT(""), TEXT("application/json"));
		Response->Code = EHttpServerResponseCodes::NoContent;
		return Response;
	}

	TUniquePtr<FHttpServerResponse> ArrayResponse(const TArray<TSharedPtr<FJsonValue>>& Items)
	{
		FString Body;
		const auto Writer = TJsonWriterFactory<>::Create(&Body);
		FJsonSerializer::Serialize(Items, Writer);
		auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
		Response->Code = EHttpServerResponseCodes::Ok;
		return Response;
	}
}

AActor* ASTFApiServerSubsystem::ResolveExtractableResource(const FString& Path, FString& OutError) const
{
	// node_id is the actor's full path name, which is what
	// GET /world/resource-nodes hands out, so a direct object lookup is the
	// normal case. The iterators are the fallback for a path FindObject does
	// not resolve (an actor that moved package, a sublevel the lookup cannot
	// see through) - bounded cost, since only node/water actors are visited.
	AActor* Actor = Path.IsEmpty() ? nullptr : FindObject<AActor>(nullptr, *Path);
	if (!Actor)
	{
		for (TActorIterator<AFGResourceNodeBase> It(GetWorld()); It && !Actor; ++It)
		{
			if (It->GetPathName() == Path)
			{
				Actor = *It;
			}
		}
		for (TActorIterator<AFGWaterVolume> It(GetWorld()); It && !Actor; ++It)
		{
			if (It->GetPathName() == Path)
			{
				Actor = *It;
			}
		}
	}
	if (!Actor || !Cast<IFGExtractableResourceInterface>(Actor))
	{
		OutError = FString::Printf(TEXT("unknown node_id %s (see GET /world/resource-nodes)"), *Path);
		return nullptr;
	}
	return Actor;
}

bool ASTFApiServerSubsystem::ResolvePlacement(UClass* Class, const TSharedPtr<FJsonObject>& Body, bool bCheckOccupied,
	FResolvedPlacement& Out, int32& OutStatus, FString& OutError) const
{
	const bool bIsExtractor = Class->IsChildOf(AFGBuildableResourceExtractorBase::StaticClass());
	FString NodeID;
	Body->TryGetStringField(TEXT("node_id"), NodeID);

	// The two modes are mutually exclusive, and both mistakes are worth
	// refusing: an extractor spawned at a coordinate stands on nothing and
	// never produces (the catalog used to call that "something broken"), and
	// a node_id on a constructor is a config error nothing downstream would
	// ever surface.
	if (bIsExtractor && NodeID.IsEmpty())
	{
		OutStatus = 422;
		OutError = TEXT("extractor classes must be placed on a node: set node_id (see GET /world/resource-nodes)");
		return false;
	}
	if (!bIsExtractor && !NodeID.IsEmpty())
	{
		OutStatus = 422;
		OutError = FString::Printf(TEXT("%s is placed at a transform, not on a node: remove node_id"), *Class->GetName());
		return false;
	}

	if (!bIsExtractor)
	{
		const TSharedPtr<FJsonObject>* TransformJson = nullptr;
		if (!Body->TryGetObjectField(TEXT("transform"), TransformJson))
		{
			OutStatus = 422;
			OutError = TEXT("transform is required");
			return false;
		}
		const FVector Location(
			(*TransformJson)->GetNumberField(TEXT("x")),
			(*TransformJson)->GetNumberField(TEXT("y")),
			(*TransformJson)->GetNumberField(TEXT("z")));
		double Yaw = 0;
		(*TransformJson)->TryGetNumberField(TEXT("yaw"), Yaw);
		Out.Transform = FTransform(FRotator(0, Yaw, 0), Location);
		return true;
	}

	AActor* ResourceActor = ResolveExtractableResource(NodeID, OutError);
	if (!ResourceActor)
	{
		OutStatus = 422;
		return false;
	}
	IFGExtractableResourceInterface* Resource = Cast<IFGExtractableResourceInterface>(ResourceActor);

	// One extractor per feature is the game's rule (and the hologram's
	// disqualifier); a water volume reports CanBecomeOccupied false and takes
	// any number of pumps.
	if (bCheckOccupied && Resource->IsOccupied())
	{
		OutStatus = 409;
		OutError = FString::Printf(TEXT("node %s is already occupied by another extractor"), *NodeID);
		return false;
	}

	// Form / allowed-resource / node-type compatibility, asked of the class
	// default object through the same public predicate the hologram uses
	// (AFGBuildableResourceExtractorBase::IsAllowedOnResource - "can this
	// extractor occupy this, given it is free"). Real compiled game code; the
	// local body is a stub, so this is not inspectable but is exactly the
	// vanilla rule rather than a re-implementation of it.
	const TScriptInterface<IFGExtractableResourceInterface> ScriptResource(ResourceActor);
	if (const AFGBuildableResourceExtractorBase* CDO = Class->GetDefaultObject<AFGBuildableResourceExtractorBase>())
	{
		if (!CDO->IsAllowedOnResource(ScriptResource))
		{
			const TSubclassOf<UFGResourceDescriptor> ResourceClass = Resource->GetResourceClass();
			OutStatus = 422;
			OutError = FString::Printf(TEXT("%s cannot extract %s (node %s)"), *Class->GetName(),
				ResourceClass ? *ResourceClass->GetName() : TEXT("that resource"), *NodeID);
			return false;
		}
	}

	// Placement overrides: yaw for anything, a surface point for water.
	const TSharedPtr<FJsonObject>* PlacementJson = nullptr;
	Body->TryGetObjectField(TEXT("placement"), PlacementJson);

	FVector Hit = ResourceActor->GetActorLocation();
	if (const AFGWaterVolume* Water = Cast<AFGWaterVolume>(ResourceActor))
	{
		// A lake is an area, so "on the node" needs a point: the caller's x/y
		// if given, the centre otherwise, always at the surface. Outside the
		// box is refused rather than clamped - a pump 200m from where the
		// config said is a surprise, a 422 is a fix.
		const FBox Bounds = WaterBounds(Water);
		Hit = Bounds.GetCenter();
		Hit.Z = Bounds.Max.Z;
		if (PlacementJson)
		{
			double X = 0, Y = 0;
			if ((*PlacementJson)->TryGetNumberField(TEXT("x"), X))
			{
				Hit.X = X;
			}
			if ((*PlacementJson)->TryGetNumberField(TEXT("y"), Y))
			{
				Hit.Y = Y;
			}
		}
		if (Hit.X < Bounds.Min.X || Hit.X > Bounds.Max.X || Hit.Y < Bounds.Min.Y || Hit.Y > Bounds.Max.Y)
		{
			OutStatus = 422;
			OutError = FString::Printf(TEXT("placement (%.0f, %.0f) is outside water volume %s (x %.0f..%.0f, y %.0f..%.0f)"),
				Hit.X, Hit.Y, *NodeID, Bounds.Min.X, Bounds.Max.X, Bounds.Min.Y, Bounds.Max.Y);
			return false;
		}
	}

	Out.Transform = PlacementOn(Resource, Hit);
	if (PlacementJson)
	{
		double Yaw = 0;
		if ((*PlacementJson)->TryGetNumberField(TEXT("yaw"), Yaw))
		{
			FRotator Rot = Out.Transform.Rotator();
			Rot.Yaw = Yaw;
			Out.Transform.SetRotation(Rot.Quaternion());
		}
	}
	Out.ResourceActor = ResourceActor;
	Out.NodeID = NodeID;
	return true;
}

bool ASTFApiServerSubsystem::HandleWorldResourceNodes(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (!CheckRequest(Request, OnComplete))
	{
		return true;
	}

	// Filters. x/y/z travel together; radius needs them; purity is an enum.
	FString ResourceFilter, PurityFilter;
	if (const FString* Raw = Request.QueryParams.Find(TEXT("resource")))
	{
		ResourceFilter = *Raw;
	}
	if (const FString* Raw = Request.QueryParams.Find(TEXT("purity")))
	{
		PurityFilter = *Raw;
		if (PurityFilter != TEXT("impure") && PurityFilter != TEXT("normal") && PurityFilter != TEXT("pure"))
		{
			OnComplete(ErrorResponse(422, TEXT("purity must be one of impure, normal, pure")));
			return true;
		}
	}
	double CenterX = 0, CenterY = 0, CenterZ = 0, Radius = 0;
	const auto Param = [&Request](const TCHAR* Key, double& Out)
	{
		if (const FString* Raw = Request.QueryParams.Find(Key))
		{
			Out = FCString::Atod(**Raw);
			return true;
		}
		return false;
	};
	const bool bHasX = Param(TEXT("x"), CenterX);
	const bool bHasY = Param(TEXT("y"), CenterY);
	const bool bHasZ = Param(TEXT("z"), CenterZ);
	const bool bHasCenter = bHasX && bHasY && bHasZ;
	if ((bHasX || bHasY || bHasZ) && !bHasCenter)
	{
		OnComplete(ErrorResponse(422, TEXT("x, y and z must be given together")));
		return true;
	}
	const bool bHasRadius = Param(TEXT("radius"), Radius);
	if (bHasRadius && !bHasCenter)
	{
		OnComplete(ErrorResponse(422, TEXT("radius requires x, y and z")));
		return true;
	}
	if (bHasRadius && Radius <= 0)
	{
		OnComplete(ErrorResponse(422, TEXT("radius must be positive")));
		return true;
	}
	bool bIncludeOccupied = false;
	if (const FString* Raw = Request.QueryParams.Find(TEXT("include_occupied")))
	{
		bIncludeOccupied = Raw->Equals(TEXT("true"), ESearchCase::IgnoreCase) || *Raw == TEXT("1");
	}
	const FVector Center(CenterX, CenterY, CenterZ);

	// Which features our own extractors sit on, so a config can keep choosing
	// the nodes it already owns (occupant_tf_id) instead of seeing them vanish
	// from a free-nodes-only listing after the first apply.
	TMap<FString, FString> OccupantByNodePath;
	if (ASTFRegistrySubsystem* Registry = ASTFRegistrySubsystem::Get(GetWorld()))
	{
		for (const ASTFRegistrySubsystem::FEntry& Entry : Registry->GetAll())
		{
			const FString NodeID = Entry.Buildable ? ExtractorNodeID(Entry.Buildable) : FString();
			if (!NodeID.IsEmpty())
			{
				OccupantByNodePath.Add(NodeID, Entry.TFID);
			}
		}
	}

	struct FNodeEntry
	{
		FString ID, Resource, Purity, Form, Type, OccupantTFID;
		bool bOccupied = false;
		FVector Location = FVector::ZeroVector;
		double Yaw = 0;
		bool bHasBounds = false;
		FBox Bounds = FBox(ForceInit);
		double DistSq = 0;
	};
	TArray<FNodeEntry> Entries;

	const auto Keep = [&](FNodeEntry& E)
	{
		if (!ResourceFilter.IsEmpty() && E.Resource != ResourceFilter)
		{
			return;
		}
		if (!PurityFilter.IsEmpty() && E.Purity != PurityFilter)
		{
			return;
		}
		if (!bIncludeOccupied && E.bOccupied)
		{
			return;
		}
		if (bHasCenter)
		{
			E.DistSq = FVector::DistSquared(E.Location, Center);
			if (bHasRadius && E.DistSq > Radius * Radius)
			{
				return;
			}
		}
		E.OccupantTFID = OccupantByNodePath.FindRef(E.ID);
		Entries.Add(MoveTemp(E));
	};

	// Nodes (ore, geysers, fracking). Deposits are the small pickable rocks,
	// not something an extractor binds to. Plain actor iteration: the node
	// manager keeps its lists private and its accessor is stub-only.
	for (TActorIterator<AFGResourceNodeBase> It(GetWorld()); It; ++It)
	{
		AFGResourceNodeBase* Node = *It;
		if (!Node || Node->GetResourceNodeType() == EResourceNodeType::Deposit)
		{
			continue;
		}
		const TSubclassOf<UFGResourceDescriptor> ResourceClass = Node->GetResourceClass();
		if (!ResourceClass)
		{
			continue;
		}
		FNodeEntry E;
		E.ID = Node->GetPathName();
		E.Resource = ResourceClass->GetName();
		if (const AFGResourceNode* Typed = Cast<AFGResourceNode>(Node))
		{
			E.Purity = PurityName(Typed->GetResourcePurity());
		}
		E.Type = NodeTypeName(Node);
		// The game's EResourceForm has no "heat"; a geyser is the only feature
		// without a solid/liquid/gas form, so name it for what it yields.
		E.Form = E.Type == TEXT("geyser") ? FString(TEXT("heat")) : FString(FormName(Node->GetResourceForm()));
		E.bOccupied = Node->IsOccupied();
		const FTransform Placement = PlacementOn(Node, Node->GetActorLocation());
		E.Location = Placement.GetLocation();
		E.Yaw = Placement.Rotator().Yaw;
		Keep(E);
	}

	// Water volumes: an area, reported at its centre/surface with its box so
	// a config can spread several pumps along it.
	for (TActorIterator<AFGWaterVolume> It(GetWorld()); It; ++It)
	{
		AFGWaterVolume* Water = *It;
		if (!Water || !Water->CanPlaceResourceExtractor())
		{
			continue;
		}
		const TSubclassOf<UFGResourceDescriptor> ResourceClass = Water->GetResourceClass();
		if (!ResourceClass)
		{
			continue;
		}
		FNodeEntry E;
		E.ID = Water->GetPathName();
		E.Resource = ResourceClass->GetName();
		E.Form = TEXT("liquid");
		E.Type = TEXT("water");
		E.bOccupied = Water->IsOccupied();
		E.bHasBounds = true;
		E.Bounds = WaterBounds(Water);
		E.Location = E.Bounds.GetCenter();
		E.Location.Z = E.Bounds.Max.Z;
		E.Yaw = 0;
		Keep(E);
	}

	// Ordering is contractual: nearest first when a centre is given, else by
	// id - a config that takes "the first free iron node" depends on it.
	Entries.Sort([bHasCenter](const FNodeEntry& A, const FNodeEntry& B)
	{
		if (bHasCenter && A.DistSq != B.DistSq)
		{
			return A.DistSq < B.DistSq;
		}
		return A.ID < B.ID;
	});

	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FNodeEntry& E : Entries)
	{
		const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("id"), E.ID);
		Json->SetStringField(TEXT("resource"), E.Resource);
		Json->SetStringField(TEXT("purity"), E.Purity);
		Json->SetStringField(TEXT("form"), E.Form);
		Json->SetStringField(TEXT("type"), E.Type);
		Json->SetBoolField(TEXT("occupied"), E.bOccupied);
		if (!E.OccupantTFID.IsEmpty())
		{
			Json->SetStringField(TEXT("occupant_tf_id"), E.OccupantTFID);
		}
		Json->SetNumberField(TEXT("x"), E.Location.X);
		Json->SetNumberField(TEXT("y"), E.Location.Y);
		Json->SetNumberField(TEXT("z"), E.Location.Z);
		Json->SetNumberField(TEXT("yaw"), E.Yaw);
		if (E.bHasBounds)
		{
			Json->SetObjectField(TEXT("bounds_min"), Vec3Json(E.Bounds.Min));
			Json->SetObjectField(TEXT("bounds_max"), Vec3Json(E.Bounds.Max));
		}
		Out.Add(MakeShared<FJsonValueObject>(Json));
	}
	OnComplete(ArrayResponse(Out));
	return true;
}

// ---------------------------------------------------------------------------
// Preview holograms
// ---------------------------------------------------------------------------

namespace
{
	/** CheckValidPlacement is protected on AFGHologram (the build gun reaches
	  * it through ValidatePlacementAndCost, which also runs the cost check
	  * against a player inventory - not wanted here: a preview is about
	  * geometry, and "cannot afford it" would read as "cannot place it").
	  * This is the usual derived-class access shim: no instances, no state. */
	struct FHologramPlacementAccess : public AFGHologram
	{
		void RunCheckValidPlacement() { CheckValidPlacement(); }
	};

	void CheckHologramPlacement(AFGHologram* Hologram)
	{
		static_cast<FHologramPlacementAccess*>(Hologram)->RunCheckValidPlacement();
	}

	TArray<TSharedPtr<FJsonValue>> DisqualifierNames(const AFGHologram* Hologram)
	{
		TArray<TSubclassOf<UFGConstructDisqualifier>> Disqualifiers;
		Hologram->GetConstructDisqualifiers(Disqualifiers);
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const TSubclassOf<UFGConstructDisqualifier>& D : Disqualifiers)
		{
			if (D)
			{
				Out.Add(MakeShared<FJsonValueString>(D->GetName()));
			}
		}
		return Out;
	}

	/** PreviewResult for a standing hologram. Re-runs the placement check
	  * first: a hologram only validates when asked (the build gun asks every
	  * frame), and the answer right after spawning is "FGCDInitializing"
	  * until it has ticked once - seen live. So the POST response can be
	  * provisional and the next read, a tick or more later, is the real one. */
	/** Drive a standing hologram through one build-gun update with the aim it
	  * was requested with, then run the placement check. UpdateHologramPlacement
	  * is the build gun's one entry point per frame (pre-placement, snap or
	  * place, post-placement, and whatever bookkeeping the hologram does on
	  * being updated); calling its parts separately left the hologram
	  * reporting "initializing" forever - seen live. */
	void DriveHologram(const FSTFPreview& Preview)
	{
		AFGHologram* Hologram = Preview.Hologram.Get();
		if (!Hologram)
		{
			return;
		}
		Hologram->SetScrollRotateValue(Preview.ScrollRotate);
		Hologram->UpdateHologramPlacement(Preview.Hit);
		if (Preview.bPin)
		{
			Hologram->SetActorLocationAndRotation(Preview.PinLocation, FRotator(0, Preview.PinYaw, 0));
		}
		CheckHologramPlacement(Hologram);
		const bool bValid = Hologram->CanConstruct();
		Hologram->SetPlacementMaterialState(bValid ? EHologramMaterialState::HMS_OK : EHologramMaterialState::HMS_ERROR);
	}

	TSharedPtr<FJsonObject> PreviewToJson(const FString& Key, const FSTFPreview& Preview)
	{
		const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("tf_id"), Key);
		Json->SetStringField(TEXT("class"), Preview.Class);
		if (AFGHologram* Hologram = Preview.Hologram.Get())
		{
			DriveHologram(Preview);
			const bool bValid = Hologram->CanConstruct();
			Json->SetBoolField(TEXT("valid"), bValid);
			Json->SetArrayField(TEXT("disqualifiers"), DisqualifierNames(Hologram));
			const FVector Loc = Hologram->GetActorLocation();
			Json->SetNumberField(TEXT("x"), Loc.X);
			Json->SetNumberField(TEXT("y"), Loc.Y);
			Json->SetNumberField(TEXT("z"), Loc.Z);
			Json->SetNumberField(TEXT("yaw"), Hologram->GetActorRotation().Yaw);
		}
		Json->SetNumberField(TEXT("expires_in"), FMath::Max(0.0, FMath::CeilToDouble(Preview.ExpiresAt - FPlatformTime::Seconds())));
		return Json;
	}
}

void ASTFApiServerSubsystem::RemovePreview(const FString& Key)
{
	FSTFPreview Preview;
	if (Previews.RemoveAndCopyValue(Key, Preview))
	{
		if (AFGHologram* Hologram = Preview.Hologram.Get())
		{
			Hologram->Destroy();
		}
	}
}

void ASTFApiServerSubsystem::RemoveAllPreviews()
{
	for (auto& Pair : Previews)
	{
		if (AFGHologram* Hologram = Pair.Value.Hologram.Get())
		{
			Hologram->Destroy();
		}
	}
	Previews.Empty();
}

void ASTFApiServerSubsystem::PurgeExpiredPreviews()
{
	const double Now = FPlatformTime::Seconds();
	TArray<FString> Expired;
	for (const auto& Pair : Previews)
	{
		// A hologram the game removed on its own (level change, dismantle-all)
		// is just a dead entry; drop it along with the timed-out ones.
		if (Pair.Value.ExpiresAt <= Now || !Pair.Value.Hologram.IsValid())
		{
			Expired.Add(Pair.Key);
		}
	}
	for (const FString& Key : Expired)
	{
		RemovePreview(Key);
	}
}

TSharedPtr<FJsonObject> ASTFApiServerSubsystem::SpawnPreview(const TSharedPtr<FJsonObject>& Body, int32& OutStatus, FString& OutError)
{
	const FString Key = Body->GetStringField(TEXT("tf_id"));
	if (Key.IsEmpty())
	{
		OutStatus = 422;
		OutError = TEXT("tf_id is required");
		return nullptr;
	}
	UClass* Class = ResolveBuildableClass(Body->GetStringField(TEXT("class")), OutError);
	if (!Class)
	{
		OutStatus = 422;
		return nullptr;
	}
	// Same placement rules and 422s as the real spawn, minus the occupancy
	// 409: previewing a miner on a node that already has one is exactly the
	// case where the hologram's own disqualifier is the useful answer.
	FResolvedPlacement Placement;
	if (!ResolvePlacement(Class, Body, /*bCheckOccupied*/ false, Placement, OutStatus, OutError))
	{
		return nullptr;
	}
	double TTL = 120;
	Body->TryGetNumberField(TEXT("ttl_seconds"), TTL);
	TTL = FMath::Clamp(TTL, 1.0, 600.0);

	// Re-previewing the same key replaces the ghost, so a re-plan after an
	// edit moves the hologram rather than stacking a second one.
	RemovePreview(Key);

	const FVector Loc = Placement.Transform.GetLocation();
	const double Yaw = Placement.Transform.Rotator().Yaw;

	// The response when the game will not give us a hologram for this class:
	// not an error (the preview is advisory) but an honest "could not check".
	const auto Unavailable = [&]()
	{
		const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("tf_id"), Key);
		Json->SetStringField(TEXT("class"), Class->GetName());
		Json->SetField(TEXT("valid"), MakeShared<FJsonValueNull>());
		TArray<TSharedPtr<FJsonValue>> Reasons;
		Reasons.Add(MakeShared<FJsonValueString>(TEXT("preview_unavailable")));
		Json->SetArrayField(TEXT("disqualifiers"), Reasons);
		Json->SetNumberField(TEXT("x"), Loc.X);
		Json->SetNumberField(TEXT("y"), Loc.Y);
		Json->SetNumberField(TEXT("z"), Loc.Z);
		Json->SetNumberField(TEXT("yaw"), Yaw);
		Json->SetNumberField(TEXT("expires_in"), 0);
		OutStatus = 200;
		return Json;
	};

	// A hologram is spawned from the recipe that builds the class, which the
	// recipe manager can look up from the class's building descriptor. The
	// naming-convention fallback (Build_X_C -> Recipe_X_C) covers vanilla
	// content when the manager has nothing, e.g. before it has initialised.
	TSubclassOf<UFGRecipe> Recipe;
	if (AFGRecipeManager* RecipeManager = AFGRecipeManager::Get(GetWorld()))
	{
		if (const TSubclassOf<UFGBuildingDescriptor> Descriptor = RecipeManager->FindBuildingDescriptorByClass(Class))
		{
			const TArray<TSubclassOf<UFGRecipe>> Recipes = RecipeManager->FindRecipesByProduct(Descriptor, /*onlyAvailableRecipes*/ false);
			if (Recipes.Num() > 0)
			{
				Recipe = Recipes[0];
			}
		}
	}
	if (!Recipe)
	{
		const FString Name = Class->GetName();
		FString Ignored;
		Recipe = ResolveRecipeClass(TEXT("Recipe_") + Name.Mid(6), Ignored); // strip "Build_"
	}
	if (!Recipe)
	{
		UE_LOG(LogSatisfactoryTerraform, Warning, TEXT("Preview: no build recipe found for %s"), *Class->GetName());
		return Unavailable();
	}

	// Owner and instigator. A hologram normally belongs to a build gun, and
	// some hologram classes assume theirs is one, so the local player's gun
	// is the first choice; this subsystem (an actor) is the fallback for a
	// session with no local player (dedicated server).
	AActor* HologramOwner = this;
	APawn* HologramInstigator = nullptr;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* PC = It->Get();
		AFGCharacterPlayer* Character = PC ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;
		if (Character)
		{
			HologramInstigator = Character;
			if (AFGBuildGun* BuildGun = Character->GetBuildGun())
			{
				HologramOwner = BuildGun;
			}
			break;
		}
	}
	AFGHologram* Hologram = AFGHologram::SpawnHologramFromRecipe(Recipe, HologramOwner, Loc, HologramInstigator);
	if (!Hologram && HologramOwner != this)
	{
		Hologram = AFGHologram::SpawnHologramFromRecipe(Recipe, this, Loc, HologramInstigator);
	}
	if (!Hologram)
	{
		UE_LOG(LogSatisfactoryTerraform, Warning, TEXT("Preview: the game returned no hologram for %s"), *Recipe->GetName());
		return Unavailable();
	}

	// Drive the hologram the way the build gun does for one aim: a hit at the
	// requested point (on the node actor, for extractors, so the extractor
	// hologram snaps to it), the scroll-rotation set to the requested yaw,
	// then pre / snap-or-place / post, then the placement check.
	FHitResult Hit(ForceInit);
	Hit.bBlockingHit = true;
	Hit.Location = Loc;
	Hit.ImpactPoint = Loc;
	Hit.Normal = FVector::UpVector;
	Hit.ImpactNormal = FVector::UpVector;
	if (Placement.ResourceActor)
	{
		Hit.HitObjectHandle = FActorInstanceHandle(Placement.ResourceActor);
		Hit.Component = Cast<UPrimitiveComponent>(Placement.ResourceActor->GetRootComponent());
	}
	FSTFPreview Preview;
	Preview.Hologram = Hologram;
	Preview.Class = Class->GetName();
	Preview.ExpiresAt = FPlatformTime::Seconds() + TTL;
	Preview.Hit = Hit;
	Preview.ScrollRotate = FMath::RoundToInt(Yaw);
	// The API spawns at exactly the requested transform (no snapping), so
	// the ghost - and the clearance check on it - should sit exactly there
	// too, whatever grid the hologram would have snapped to. Extractors are
	// the exception: their hologram must snap to the node.
	Preview.bPin = Placement.ResourceActor == nullptr;
	Preview.PinLocation = Loc;
	Preview.PinYaw = Yaw;
	DriveHologram(Preview);
	Hologram->SetDisabled(false);
	Hologram->SetActorHiddenInGame(false);
	Previews.Add(Key, Preview);
	UE_LOG(LogSatisfactoryTerraform, Verbose, TEXT("Preview %s: %s %s"), *Key, *Class->GetName(), Hologram->CanConstruct() ? TEXT("valid") : TEXT("INVALID"));
	OutStatus = 200;
	return PreviewToJson(Key, Preview);
}

bool ASTFApiServerSubsystem::HandlePreview(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (!CheckRequest(Request, OnComplete))
	{
		return true;
	}
	PurgeExpiredPreviews();

	if (Request.Verb == EHttpServerRequestVerbs::VERB_GET)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const auto& Pair : Previews)
		{
			Items.Add(MakeShared<FJsonValueObject>(PreviewToJson(Pair.Key, Pair.Value)));
		}
		OnComplete(ArrayResponse(Items));
		return true;
	}

	if (Request.Verb == EHttpServerRequestVerbs::VERB_DELETE)
	{
		const int32 Removed = Previews.Num();
		RemoveAllPreviews();
		const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("removed"), Removed);
		OnComplete(JsonResponse(200, Json));
		return true;
	}

	// POST
	const TSharedPtr<FJsonObject> Body = ParseBody(Request);
	if (!Body.IsValid())
	{
		OnComplete(ErrorResponse(400, TEXT("invalid JSON")));
		return true;
	}
	int32 Status = 200;
	FString Error;
	const TSharedPtr<FJsonObject> Result = SpawnPreview(Body, Status, Error);
	if (!Result.IsValid())
	{
		OnComplete(ErrorResponse(Status, Error));
		return true;
	}
	OnComplete(JsonResponse(Status, Result));
	return true;
}

bool ASTFApiServerSubsystem::HandlePreviewByID(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (!CheckRequest(Request, OnComplete))
	{
		return true;
	}
	const FString Key = PathID(Request);
	if (Request.Verb == EHttpServerRequestVerbs::VERB_GET)
	{
		// One standing hologram, re-validated now (see PreviewToJson). The
		// provider reads this when a POST came back "initializing".
		PurgeExpiredPreviews();
		const FSTFPreview* Preview = Previews.Find(Key);
		if (!Preview)
		{
			OnComplete(ErrorResponse(404, TEXT("no preview with that tf_id (expired or never shown)")));
			return true;
		}
		OnComplete(JsonResponse(200, PreviewToJson(Key, *Preview)));
		return true;
	}
	// DELETE. Idempotent: an already-expired preview is simply not there.
	RemovePreview(Key);
	OnComplete(NoContentResponse());
	return true;
}
