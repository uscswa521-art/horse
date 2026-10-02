using UnrealBuildTool;
using System.Collections.Generic;

public class LiveArenaEditorTarget : TargetRules
{
	public LiveArenaEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("LiveArena");
	}
}
