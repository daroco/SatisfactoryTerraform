using UnrealBuildTool;

public class SatisfactoryTerraform : ModuleRules
{
	public SatisfactoryTerraform(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		PublicDependencyModuleNames.AddRange(new[] {
			"Core",
			"CoreUObject",
			"Engine",
			"FactoryGame",
			"SML",
		});

		PrivateDependencyModuleNames.AddRange(new[] {
			"HTTPServer",
			"Json",
			"JsonUtilities",
			"AssetRegistry",
		});
	}
}
