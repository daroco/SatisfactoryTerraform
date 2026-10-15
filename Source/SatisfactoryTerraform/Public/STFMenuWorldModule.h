#pragma once

#include "CoreMinimal.h"
#include "Module/MenuWorldModule.h"
#include "STFMenuWorldModule.generated.h"

/**
 * Root menu-world module: auto-loads a save when the game was launched for
 * tooling.
 *
 * The API subsystem only exists inside a loaded session, so nothing there can
 * get the game from the main menu into a save. This module runs in the menu
 * world instead. If the process was started with `-STFLoad=<save name>` on
 * its command line, or with the environment variable
 * SATISFACTORY_TERRAFORM_LOAD set, it loads that save a few seconds after the
 * menu is up. Together with POST /session/quit this closes the loop: quit,
 * relaunch, back in the world, no clicks.
 *
 * Once per process: quitting a session back to the menu must not reload it.
 */
UCLASS()
class SATISFACTORYTERRAFORM_API USTFMenuWorldModule : public UMenuWorldModule
{
	GENERATED_BODY()

public:
	USTFMenuWorldModule();

	virtual void DispatchLifecycleEvent(ELifecyclePhase Phase) override;

private:
	/** The save name asked for on the command line or in the environment; empty for none. */
	static FString RequestedSave();
	void LoadSave(const FString& SaveName);

	FTimerHandle AutoLoadTimer;
	static bool bAutoLoadConsumed;
};
