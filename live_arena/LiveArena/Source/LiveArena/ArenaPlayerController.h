#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "ArenaTypes.h"
#include "ArenaPlayerController.generated.h"

class UArenaHudWidget;
class AArenaGameMode;
class AArenaShowDirector;

/**
 * Streamer controls (keys polled in PlayerTick, no input assets needed):
 *   Space   toggle Watch / Interactive
 *   L       start lottery            Enter  invite winner on stage        K  end guest (back to seat)
 *   1 2 3 4 cameras: Broadcast / Streamer POV / Wide / Follow             F  free fly (WASD + mouse, again to exit)
 *   [ ]     exposure -/+             H  show/hide help                     Tab  hide whole HUD (clean feed for OBS)
 *   D       demo: +20 viewers        C  demo: 30 claps
 * The HUD status block shows: server connection, room, join link, mode, show state, named / watching / drawn figures,
 * LED source, camera, exposure.
 */
UCLASS()
class LIVEARENA_API AArenaPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AArenaPlayerController();

	virtual void BeginPlay() override;
	virtual void PlayerTick(float DeltaTime) override;

	/** Called by the game mode once the director exists. */
	void OnArenaReady();

	void ShowToast(const FString& Big, const FString& Small, float Seconds);

private:
	void HandleKeys();
	void RefreshStatus(float DeltaTime);
	void SetFreeFly(bool bOn);
	AArenaGameMode* GetArenaMode() const;

	UPROPERTY(Transient)
	TObjectPtr<UArenaHudWidget> Hud;

	bool bFreeFly = false;
	bool bHudHidden = false;
	float StatusTimer = 0.f;

	// ---- private helpers (ArenaPlayerController.cpp) ----
	/** WASD/QE + mouse move the director's camera actor while bFreeFly. */
	void UpdateFreeFly(float DeltaTime);
	/** Leaves free fly and switches the director to a preset camera. */
	void SelectCamera(EArenaCamera Preset);
	AArenaShowDirector* GetDirector() const;

	/** Preset to return to when free fly is switched off with F. */
	EArenaCamera FreeFlyReturnCamera = EArenaCamera::Broadcast;
	float FreeFlyYaw = 0.f;
	float FreeFlyPitch = 0.f;
	/** Forces the next RefreshStatus to rebuild the text (after a key press). */
	bool bStatusDirty = true;
	bool bGreeted = false;
};
