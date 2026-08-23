#pragma once

#include "CoreMinimal.h"
#include "Module/GameWorldModule.h"
#include "STFGameWorldModule.generated.h"

/**
 * Root game-world module: tells SML to spawn our subsystems into every level.
 *
 * If native root-module discovery gives you trouble on first build, the
 * fallback is the standard editor route: create a Blueprint subclass of this
 * (or of UGameWorldModule) at /SatisfactoryTerraform/RootGameWorld_SatisfactoryTerraform.
 */
UCLASS()
class SATISFACTORYTERRAFORM_API USTFGameWorldModule : public UGameWorldModule
{
	GENERATED_BODY()

public:
	USTFGameWorldModule();
};
