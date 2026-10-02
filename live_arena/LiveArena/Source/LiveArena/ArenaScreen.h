#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ArenaTypes.h"
#include "ArenaScreen.generated.h"

class UWidgetComponent;
class UMediaPlayer;
class UMediaTexture;
class UArenaScreenWidget;
class UArenaBannerWidget;
class URectLightComponent;

/**
 * The LED wall behind the stage (a world-space UWidgetComponent hosting UArenaScreenWidget) and the banner strip under it.
 * Source priority for Auto: capture device whose name contains CaptureDeviceName -> Url -> placeholder.
 * A rect light in front of the LED tints the stage and the front rows with the screen's light.
 */
UCLASS()
class LIVEARENA_API AArenaScreen : public AActor
{
	GENERATED_BODY()

public:
	AArenaScreen();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

	/** ScreenTransform: centre of the LED, +X pointing at the audience. SizeCm: LED width/height. */
	void Init(const FTransform& ScreenTransform, const FVector2D& SizeCm, const FTransform& BannerTransform);

	/** Returns a short human description of the source that ended up active (for the HUD). */
	FString SetSource(EArenaScreenSource Source, const FString& CaptureDeviceName, const FString& Url, const FString& StreamerName);

	void SetBanner(const FString& Left, const FString& Centre, const FString& Right);
	void ShowToast(const FString& Big, const FString& Small, float Seconds);
	void ClearToast();

	/** 0..1 brightness of the light spilling from the LED onto the stage. */
	void SetGlow(float Amount);

	FString GetActiveSource() const { return ActiveSource; }

private:
	UArenaScreenWidget* GetScreenWidget() const;
	UArenaBannerWidget* GetBannerWidget() const;

	/** Creates MediaPlayer + MediaTexture, opens the device URL and puts the texture on the LED. False if the open was refused. */
	bool OpenCaptureDevice(const FString& DeviceUrl);
	/** Shows Url (YouTube watch links become embeds) on the LED. False if Url is empty. */
	bool ShowWebSource(const FString& Url);
	void ShowWaiting(const FString& Title, const FString& Subtitle);
	void CloseMedia();

	/** Media player reports the capture device could not be opened (asynchronous): fall back like Auto does. */
	UFUNCTION()
	void HandleMediaOpenFailed(FString FailedUrl);

	// Remembered by SetSource for the asynchronous capture-device fallback.
	EArenaScreenSource RequestedSource = EArenaScreenSource::Auto;
	FString FallbackUrl;
	FString FallbackTitle;
	float GlowAmount = -1.f;

	UPROPERTY(VisibleAnywhere, Category = "Arena")
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere, Category = "Arena")
	TObjectPtr<UWidgetComponent> ScreenComp;

	UPROPERTY(VisibleAnywhere, Category = "Arena")
	TObjectPtr<UWidgetComponent> BannerComp;

	UPROPERTY(VisibleAnywhere, Category = "Arena")
	TObjectPtr<URectLightComponent> Glow;

	UPROPERTY(Transient)
	TObjectPtr<UMediaPlayer> MediaPlayer;

	UPROPERTY(Transient)
	TObjectPtr<UMediaTexture> MediaTexture;

	FString ActiveSource;
	float GlowMax = 30.f;
};
