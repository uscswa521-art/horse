#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "ArenaTypes.h"
#include "LiveArenaSettings.generated.h"

/**
 * Project Settings > Game > Live Arena. Stored in Config/DefaultGame.ini.
 * Every field can also be overridden on the command line of a packaged build, e.g.
 *   LiveArena.exe -ArenaServer=wss://example.com/ws -ArenaRoom=amy -ArenaKey=secret -ArenaDemo=0
 * (see AArenaGameMode::ResolveSettings for the full list).
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Live Arena"))
class LIVEARENA_API ULiveArenaSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	ULiveArenaSettings();

	virtual FName GetCategoryName() const override { return TEXT("Game"); }

	/** WebSocket URL of the arena server, e.g. ws://localhost:8787/ws. Empty = offline (demo only). */
	UPROPERTY(config, EditAnywhere, Category = "Server")
	FString ServerUrl;

	/** Room name = the streamer's channel in the server. Viewers join at <server>/r/<RoomId>. */
	UPROPERTY(config, EditAnywhere, Category = "Server")
	FString RoomId;

	/** Secret that proves this UE app is the host of the room. The first host to connect sets it. */
	UPROPERTY(config, EditAnywhere, Category = "Server")
	FString HostKey;

	/** Link shown on the HUD and LED banner for viewers to enter, e.g. https://arena.example.com/r/amy */
	UPROPERTY(config, EditAnywhere, Category = "Server")
	FString JoinUrl;

	/** YouTube live video id; the server polls its concurrent viewer count to fill the crowd. */
	UPROPERTY(config, EditAnywhere, Category = "Stream")
	FString YouTubeVideoId;

	UPROPERTY(config, EditAnywhere, Category = "Stream")
	EArenaScreenSource ScreenSource;

	/** Substring of the capture device name to use, default "OBS Virtual Camera". */
	UPROPERTY(config, EditAnywhere, Category = "Stream")
	FString CaptureDeviceName;

	/** Web page shown on the LED screen when ScreenSource is Url (or Auto with no capture device). */
	UPROPERTY(config, EditAnywhere, Category = "Stream")
	FString StreamUrl;

	/** Call link (Discord / Meet / Zoom) sent only to the lottery winner when invited on stage. */
	UPROPERTY(config, EditAnywhere, Category = "Show")
	FString GuestCallUrl;

	/** Shown on the LED banner and on the audience page. */
	UPROPERTY(config, EditAnywhere, Category = "Show")
	FString StreamerName;

	/** Fake viewers, claps and crowd count so the arena can be tried without a server. */
	UPROPERTY(config, EditAnywhere, Category = "Show")
	bool bDemoMode;

	/** Upper bound of crowd figures drawn (named + anonymous). The LED always shows the real number. */
	UPROPERTY(config, EditAnywhere, Category = "Show", meta = (ClampMin = "100", ClampMax = "10000"))
	int32 MaxFigures;

	/** Manual exposure bias (EV). [ and ] adjust it at runtime. */
	UPROPERTY(config, EditAnywhere, Category = "Show")
	float ExposureBias;
};
