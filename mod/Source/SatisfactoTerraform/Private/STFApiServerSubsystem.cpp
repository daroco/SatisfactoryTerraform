#include "STFApiServerSubsystem.h"

#include "SatisfactoTerraformModule.h"
#include "STFRegistrySubsystem.h"

#include "Buildables/FGBuildable.h"
#include "Buildables/FGBuildableFactory.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "FGBuildableSubsystem.h"
#include "FGRecipe.h"
#include "FGDismantleInterface.h"

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

	/** Last path segment, e.g. the tf_id in /api/v1/buildables/{tf_id}. */
	FString PathID(const FHttpServerRequest& Request)
	{
		FString Path = Request.RelativePath.GetPath();
		FString Left, ID;
		if (Path.Split(TEXT("/"), &Left, &ID, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
		{
			return ID;
		}
		return Path;
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
}

ASTFApiServerSubsystem::ASTFApiServerSubsystem()
{
	PrimaryActorTick.bCanEverTick = false;
	// Only the authoritative side runs the listener and mutates the world.
	ReplicationPolicy = ESubsystemReplicationPolicy::SpawnOnServer;
}

void ASTFApiServerSubsystem::BeginPlay()
{
	Super::BeginPlay();

	FHttpServerModule& Module = FHttpServerModule::Get();
	Router = Module.GetHttpRouter(Port, /*bFailOnBindFailure*/ false);
	if (!Router.IsValid())
	{
		UE_LOG(LogSatisfactoTerraform, Error, TEXT("Could not bind HTTP router on port %d"), Port);
		return;
	}
	BindRoutes();
	Module.StartAllListeners();
	UE_LOG(LogSatisfactoTerraform, Log, TEXT("SatisfactoTerraform API listening on port %d"), Port);
}

void ASTFApiServerSubsystem::EndPlay(const EEndPlayReason::Type Reason)
{
	if (Router.IsValid())
	{
		for (const FHttpRouteHandle& Handle : Routes)
		{
			Router->UnbindRoute(Handle);
		}
		Routes.Empty();
	}
	Super::EndPlay(Reason);
}

void ASTFApiServerSubsystem::BindRoutes()
{
	const auto Bind = [this](const FString& Path, EHttpServerRequestVerbs Verbs, auto Handler)
	{
		Routes.Add(Router->BindRoute(
			FHttpPath(Path), Verbs,
			FHttpRequestHandler::CreateUObject(this, Handler)));
	};

	Bind(TEXT("/api/v1/health"), EHttpServerRequestVerbs::VERB_GET, &ASTFApiServerSubsystem::HandleHealth);
	Bind(TEXT("/api/v1/world"), EHttpServerRequestVerbs::VERB_GET, &ASTFApiServerSubsystem::HandleWorld);
	Bind(TEXT("/api/v1/buildables"),
		EHttpServerRequestVerbs::VERB_GET | EHttpServerRequestVerbs::VERB_POST,
		&ASTFApiServerSubsystem::HandleBuildables);
	// FHttpRouter has no path parameters; the handler parses the tf_id suffix.
	Bind(TEXT("/api/v1/buildables/"),
		EHttpServerRequestVerbs::VERB_GET | EHttpServerRequestVerbs::VERB_PATCH | EHttpServerRequestVerbs::VERB_DELETE,
		&ASTFApiServerSubsystem::HandleBuildableByID);
	Bind(TEXT("/api/v1/connections"),
		EHttpServerRequestVerbs::VERB_GET | EHttpServerRequestVerbs::VERB_POST,
		&ASTFApiServerSubsystem::HandleConnections);
	Bind(TEXT("/api/v1/connections/"),
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
		for (const auto& Pair : Registry->GetAll())
		{
			Items.Add(MakeShared<FJsonValueObject>(BuildableToJson(Pair.Key, Pair.Value)));
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
	AFGBuildable* Buildable = Registry->Find(TFID);
	if (!Buildable)
	{
		OnComplete(ErrorResponse(404, TEXT("no buildable with that tf_id")));
		return true;
	}

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
	// implements it (refunds + connection cleanup match vanilla behaviour),
	// falling back to a plain Destroy() otherwise.
	Registry->Unregister(TFID);
	DismantleBuildable(Buildable);
	auto Response = FHttpServerResponse::Create(TEXT(""), TEXT("application/json"));
	Response->Code = EHttpServerResponseCodes::NoContent;
	OnComplete(MoveTemp(Response));
	return true;
}

bool ASTFApiServerSubsystem::HandleConnections(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (!CheckAuth(Request, OnComplete))
	{
		return true;
	}
	if (Request.Verb == EHttpServerRequestVerbs::VERB_GET)
	{
		// TODO(M3): track connections in the registry like buildables.
		auto Response = FHttpServerResponse::Create(TEXT("[]"), TEXT("application/json"));
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
	// TODO(M3): connection lookup/dismantle once connections are registered.
	OnComplete(ErrorResponse(404, TEXT("no connection with that tf_id")));
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
	UE_LOG(LogSatisfactoTerraform, Log, TEXT("Indexed %d Blueprint classes for name resolution"), ClassNameIndex.Num());
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

void ASTFApiServerSubsystem::DismantleBuildable(AFGBuildable* Buildable) const
{
	if (Buildable->GetClass()->ImplementsInterface(UFGDismantleInterface::StaticClass()))
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
	if (Registry->Find(TFID))
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
	// factory tick group like a hologram-built one would. Recipe/clock_speed
	// are set between BeginSpawnBuildable and FinishSpawning - the standard
	// UE deferred-construction pattern - so construction scripts/BeginPlay
	// see the final state.
	AFGBuildableSubsystem* BuildableSubsystem = AFGBuildableSubsystem::Get(GetWorld());
	AFGBuildable* Buildable = BuildableSubsystem->BeginSpawnBuildable(Class, Transform);
	if (!Buildable)
	{
		OutStatus = 422;
		OutError = TEXT("game refused to spawn that buildable");
		return nullptr;
	}

	if (AFGBuildableManufacturer* Manufacturer = Cast<AFGBuildableManufacturer>(Buildable))
	{
		if (RecipeClass)
		{
			Manufacturer->SetRecipe(RecipeClass);
		}
		Manufacturer->SetPendingPotential(ClockSpeed);
	}

	Buildable->FinishSpawning(Transform);

	Registry->Register(TFID, Buildable);
	UE_LOG(LogSatisfactoTerraform, Log, TEXT("Spawned %s as %s"), *Class->GetName(), *TFID);
	OutStatus = 201;
	return BuildableToJson(TFID, Buildable);
}

TSharedPtr<FJsonObject> ASTFApiServerSubsystem::SpawnConnection(const TSharedPtr<FJsonObject>& Body, int32& OutStatus, FString& OutError)
{
	// TODO(M3): belts and power lines.
	//  - Resolve both endpoint buildables from the registry (422 if missing).
	//  - Belts: spawn AFGBuildableConveyorBelt, set spline points between the
	//    two UFGFactoryConnectionComponents, then connect both ends.
	//  - Power: spawn AFGBuildableWire and Connect() the two
	//    UFGPowerConnectionComponents.
	//  - Register under tf_id so GET/DELETE work.
	OutStatus = 422;
	OutError = TEXT("connections not implemented yet (M3)");
	return nullptr;
}
