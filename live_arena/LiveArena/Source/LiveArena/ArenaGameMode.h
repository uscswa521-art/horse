#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "ArenaTypes.h"
#include "ArenaGameMode.generated.h"

class AArenaVenue;
class AArenaAudience;
class AArenaScreen;
class AArenaShowDirector;
class UArenaNetSubsystem;

/** Resolved settings = Project Settings (DefaultGame.ini) overridden by command-line switches. */
struct FArenaResolvedSettings
{
	FString ServerUrl;
	FString RoomId;
	FString HostKey;
	FString JoinUrl;
	FString YouTubeVideoId;
	EArenaScreenSource ScreenSource = EArenaScreenSource::Auto;
	FString CaptureDeviceName;
	FString StreamUrl;
	FString GuestCallUrl;
	FString StreamerName;
	bool bDemoMode = true;
	int32 MaxFigures = 2500;
	float ExposureBias = 0.f;
};

/**
 * Wires everything together in StartPlay():
 *   - uses an AArenaVenue already placed in the level (so meshes can be swapped in the editor) or spawns one;
 *   - spawns AArenaAudience, AArenaScreen, AArenaShowDirector;
 *   - connects UArenaNetSubsystem when ServerUrl is set, forwards its events to the audience/director;
 *   - runs the demo (fake viewers, claps, crowd count) when bDemoMode is on;
 *   - refreshes the LED banner once a second.
 *
 * Command-line switches: -ArenaServer= -ArenaRoom= -ArenaKey= -ArenaJoin= -ArenaVideo= -ArenaSource=auto|capture|url|none
 *   -ArenaDevice= -ArenaUrl= -ArenaCall= -ArenaName= -ArenaDemo=0|1 -ArenaMaxFigures=
 */
UCLASS()
class LIVEARENA_API AArenaGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AArenaGameMode();

	virtual void StartPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	static FArenaResolvedSettings ResolveSettings();

	const FArenaResolvedSettings& GetSettings() const { return Settings; }
	AArenaVenue* GetVenue() const { return Venue; }
	AArenaAudience* GetAudience() const { return Audience; }
	AArenaScreen* GetScreen() const { return Screen; }
	AArenaShowDirector* GetDirector() const { return Director; }
	UArenaNetSubsystem* GetNet() const;
	bool IsServerConnected() const;
	bool IsDemo() const { return Settings.bDemoMode; }
	FString GetScreenSourceDescription() const { return ScreenSourceDescription; }

	/** Demo helpers (also bound to keys by the player controller). */
	void DemoAddViewers(int32 Count);
	void DemoClaps(int32 Count);

private:
	void BindNet();
	void UpdateBanner();
	void TickDemo(float DeltaSeconds);
	FString NextDemoName();

	UPROPERTY(Transient) TObjectPtr<AArenaVenue> Venue;
	UPROPERTY(Transient) TObjectPtr<AArenaAudience> Audience;
	UPROPERTY(Transient) TObjectPtr<AArenaScreen> Screen;
	UPROPERTY(Transient) TObjectPtr<AArenaShowDirector> Director;

	FArenaResolvedSettings Settings;
	FString ScreenSourceDescription;
	float BannerTimer = 0.f;

	// demo state
	int32 DemoNextSlot = 0;
	int32 DemoCounter = 0;
	int32 DemoTarget = 140;
	float DemoJoinTimer = 0.f;
	float DemoClapTimer = 0.f;
	float DemoCrowd = 0.f;
	float DemoCrowdTarget = 1850.f;
};
