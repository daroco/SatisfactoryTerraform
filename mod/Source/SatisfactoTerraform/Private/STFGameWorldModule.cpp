#include "STFGameWorldModule.h"

#include "STFApiServerSubsystem.h"
#include "STFRegistrySubsystem.h"

USTFGameWorldModule::USTFGameWorldModule()
{
	bRootModule = true;
	ModSubsystems.Add(ASTFRegistrySubsystem::StaticClass());
	ModSubsystems.Add(ASTFApiServerSubsystem::StaticClass());
}
