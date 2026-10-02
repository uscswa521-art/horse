#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ArenaTypes.h"
#include "ArenaShowDirector.generated.h"

class AArenaVenue;
class AArenaAudience;
class AArenaScreen;
class UArenaNetSubsystem;
class USpotLightComponent;
class UPointLightComponent;
class ACameraActor;
class APostProcessVolume;
class AExponentialHeightFog;
class ASkyLight;

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnArenaShowMessage, const FString& /*Big*/, const FString& /*Small*/);

/**
 * Lights, camera and the lottery.
 *
 * Lights (all movable, warm palette only — amber #FFB86B / ivory / soft white, never blue):
 *   - House lights over the bowl: Watch 0.15, Interactive 0.45, Spinning/Landed 0.05, Guest 0.25 (relative).
 *   - Stage wash from the front truss.
 *   - 2 follow spots high at the back of the bowl (narrow cone, volumetric).
 *   - 8 moving heads on the stage truss: off in Watch, slow sweeps over the crowd in Interactive, fast during Spinning.
 *   - Exponential height fog with volumetric fog so beams read; post-process volume with manual exposure + bloom.
 *
 * Lottery: StartLottery() -> Spinning (follow spot hops between random named seats, hop interval eases
 * 0.07 s -> 0.6 s over ~5 s) -> Landed (spot locks on winner, LED toast "抽中 <name> · <seat>", net SendLotteryWinner)
 * -> InviteWinner() -> Walking (figure walks via venue walk path, spot follows) -> Guest (sits in guest chair,
 * both follow spots on them, net SendGuest(id)) -> EndGuest() -> back to seat, Idle, net SendGuest("").
 * Winner must not be the current guest; if nobody has entered, StartLottery returns false and posts a message.
 *
 * Camera: one ACameraActor moved smoothly (critically damped) between presets; SetCamera(Free) is handled by the
 * player controller, so the director just stops driving the camera then.
 */
UCLASS()
class LIVEARENA_API AArenaShowDirector : public AActor
{
	GENERATED_BODY()

public:
	AArenaShowDirector();

	virtual void Tick(float DeltaSeconds) override;

	void Init(AArenaVenue* InVenue, AArenaAudience* InAudience, AArenaScreen* InScreen, UArenaNetSubsystem* InNet, float InExposureBias);

	void SetMode(EArenaMode InMode);
	void ToggleMode();
	EArenaMode GetMode() const { return Mode; }

	bool StartLottery();
	void InviteWinner();
	void EndGuest();
	EArenaShowState GetShowState() const { return State; }
	const FArenaViewer& GetWinner() const { return Winner; }

	void SetCamera(EArenaCamera InCamera);
	EArenaCamera GetCamera() const { return Camera; }
	ACameraActor* GetCameraActor() const { return CameraActor; }

	void AdjustExposure(float DeltaEv);
	float GetExposureBias() const { return ExposureBias; }

	/** Called by the game mode when a named viewer leaves; cancels the lottery/guest if it was them. */
	void NotifyViewerLeft(const FString& Id);

	FOnArenaShowMessage OnMessage;

private:
	void SpawnLights();
	void SpawnAtmosphere();
	void ApplyLightLevels(float DeltaSeconds);
	void UpdateLottery(float DeltaSeconds);
	void UpdateMovingHeads(float DeltaSeconds);
	void UpdateCamera(float DeltaSeconds);
	void AimSpot(USpotLightComponent* Spot, const FVector& Target, float DeltaSeconds, float Sharpness);
	void PostMessage(const FString& Big, const FString& Small, float LedSeconds);

	UPROPERTY(Transient) TObjectPtr<AArenaVenue> Venue;
	UPROPERTY(Transient) TObjectPtr<AArenaAudience> Audience;
	UPROPERTY(Transient) TObjectPtr<AArenaScreen> Screen;
	UPROPERTY(Transient) TObjectPtr<UArenaNetSubsystem> Net;

	UPROPERTY(VisibleAnywhere, Category = "Arena") TObjectPtr<USceneComponent> Root;
	UPROPERTY(Transient) TArray<TObjectPtr<UPointLightComponent>> HouseLights;
	UPROPERTY(Transient) TArray<TObjectPtr<USpotLightComponent>> StageWash;
	UPROPERTY(Transient) TArray<TObjectPtr<USpotLightComponent>> MovingHeads;
	UPROPERTY(Transient) TObjectPtr<USpotLightComponent> FollowSpotA;
	UPROPERTY(Transient) TObjectPtr<USpotLightComponent> FollowSpotB;

	UPROPERTY(Transient) TObjectPtr<ACameraActor> CameraActor;
	UPROPERTY(Transient) TObjectPtr<APostProcessVolume> PostVolume;
	UPROPERTY(Transient) TObjectPtr<AExponentialHeightFog> Fog;
	UPROPERTY(Transient) TObjectPtr<ASkyLight> Sky;

	EArenaMode Mode = EArenaMode::Watch;
	EArenaShowState State = EArenaShowState::Idle;
	EArenaCamera Camera = EArenaCamera::Broadcast;

	FArenaViewer Winner;
	FString SpinTargetId;
	float SpinElapsed = 0.f;
	float SpinDuration = 5.f;
	float NextHop = 0.f;
	float StateTime = 0.f;

	float HouseLevel = 0.15f;      // smoothed
	float WashLevel = 1.f;
	float HeadsLevel = 0.f;
	float FollowLevel = 0.f;
	float Time = 0.f;
	float ExposureBias = 0.f;

	FVector CamPos = FVector::ZeroVector;
	FVector CamVel = FVector::ZeroVector;
	FVector CamLook = FVector::ZeroVector;
	FVector CamLookVel = FVector::ZeroVector;
	bool bCamInit = false;
};
