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
 *   - runs the demo (fake viewers, claps, crowd count) when bDemoMode is on and no ServerUrl is set;
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
	/** Unbinds from UArenaNetSubsystem, which belongs to the game instance and outlives this world. */
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	static FArenaResolvedSettings ResolveSettings();

	const FArenaResolvedSettings& GetSettings() const { return Settings; }
	AArenaVenue* GetVenue() const { return Venue; }
	AArenaAudience* GetAudience() const { return Audience; }
	AArenaScreen* GetScreen() const { return Screen; }
	AArenaShowDirector* GetDirector() const { return Director; }
	UArenaNetSubsystem* GetNet() const;
	bool IsServerConnected() const;
	/** True after the server refused the host key or another app took over the room (no more reconnects). */
	bool IsNetRefused() const { return bNetRefused; }
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

	// ---- private helpers (ArenaGameMode.cpp) ----
	void HandleSnapshot(const TArray<FArenaViewer>& Viewers);
	void HandleJoin(const FArenaViewer& Viewer);
	void HandleLeave(const FString& Id);
	void HandleClap(const FString& Id);
	void HandleYouTubeCount(int32 Count);
	void HandleConnectionChanged(bool bConnected);
	void HandleNetError(const FString& Message);
	void HandleShowMessage(const FString& Big, const FString& Small);

	/** Toast on the streamer's HUD (first local player controller). */
	void Toast(const FString& Big, const FString& Small, float Seconds = 3.f);
	/** Points the first player controller at the director's camera once both exist; retried from Tick until it works. */
	void NotifyControllerReady();
	/** Slots currently held by named viewers (real or demo). */
	TSet<int32> CollectOccupiedSlots() const;
	/** Seats one fake viewer in the lowest free slot (same rule as the server). False when the venue is full. */
	bool AddDemoViewer(TSet<int32>& Occupied);
	/** Releases queued demo claps a few per frame so a burst reads as a wave of applause. */
	void TickPendingClaps(float DeltaSeconds);

	bool bNetBound = false;
	bool bControllerNotified = false;
	/** Set by the first snapshot (= the server accepted this app as host); later snapshots are reconnects. */
	bool bServerSeen = false;
	/** The server refused the host key or another app took the room; the subsystem stopped reconnecting. */
	bool bNetRefused = false;

	float DemoCrowdTimer = 0.f;
	float DemoDriftTimer = 15.f;
	int32 DemoPendingClaps = 0;
	float DemoClapAccum = 0.f;
	/** Base name -> times used, so repeated picks become "Kelly2", "Kelly3", ... */
	TMap<FString, int32> DemoNameUses;
};
