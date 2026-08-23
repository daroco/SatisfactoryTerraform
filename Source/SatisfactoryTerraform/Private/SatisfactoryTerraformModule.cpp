#include "SatisfactoryTerraformModule.h"

DEFINE_LOG_CATEGORY(LogSatisfactoryTerraform);

void FSatisfactoryTerraformModule::StartupModule()
{
	UE_LOG(LogSatisfactoryTerraform, Log, TEXT("SatisfactoryTerraform module loaded"));
}

void FSatisfactoryTerraformModule::ShutdownModule()
{
}

IMPLEMENT_GAME_MODULE(FSatisfactoryTerraformModule, SatisfactoryTerraform);
