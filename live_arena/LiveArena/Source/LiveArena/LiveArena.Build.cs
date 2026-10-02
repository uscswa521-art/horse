using UnrealBuildTool;

public class LiveArena : ModuleRules
{
	public LiveArena(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"UMG",
			"Slate",
			"SlateCore",
			"DeveloperSettings",
			"Json",
			"JsonUtilities",
			"WebSockets",
			"MediaAssets",
			"WebBrowserWidget"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"MediaUtils"
		});
	}
}
