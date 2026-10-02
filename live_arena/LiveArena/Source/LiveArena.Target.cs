using UnrealBuildTool;
using System.Collections.Generic;

public class LiveArenaTarget : TargetRules
{
	public LiveArenaTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("LiveArena");
	}
}
