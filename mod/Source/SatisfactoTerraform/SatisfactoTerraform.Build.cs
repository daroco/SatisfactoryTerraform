using UnrealBuildTool;

public class SatisfactoTerraform : ModuleRules
{
	public SatisfactoTerraform(ReadOnlyTargetRules Target) : base(Target)
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
