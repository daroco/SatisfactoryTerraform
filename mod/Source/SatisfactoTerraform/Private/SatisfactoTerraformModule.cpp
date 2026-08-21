#include "SatisfactoTerraformModule.h"

DEFINE_LOG_CATEGORY(LogSatisfactoTerraform);

void FSatisfactoTerraformModule::StartupModule()
{
	UE_LOG(LogSatisfactoTerraform, Log, TEXT("SatisfactoTerraform module loaded"));
}

void FSatisfactoTerraformModule::ShutdownModule()
{
}

IMPLEMENT_GAME_MODULE(FSatisfactoTerraformModule, SatisfactoTerraform);
