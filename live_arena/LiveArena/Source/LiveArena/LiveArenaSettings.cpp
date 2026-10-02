#include "LiveArenaSettings.h"

// Defaults must match [/Script/LiveArena.LiveArenaSettings] in Config/DefaultGame.ini (the ini wins when both exist).
ULiveArenaSettings::ULiveArenaSettings()
{
	ServerUrl = FString();                 // empty = offline, demo only
	RoomId = TEXT("myroom");
	HostKey = TEXT("change-me");
	JoinUrl = FString();
	YouTubeVideoId = FString();
	ScreenSource = EArenaScreenSource::Auto;
	CaptureDeviceName = TEXT("OBS Virtual Camera");
	StreamUrl = FString();
	GuestCallUrl = FString();
	StreamerName = TEXT("\u76F4\u64AD\u4E3B"); // 直播主 (escaped so the source encoding never matters)
	bDemoMode = true;
	MaxFigures = 2500;
	ExposureBias = 0.f;
}
