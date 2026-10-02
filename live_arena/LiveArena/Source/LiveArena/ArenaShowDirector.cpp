#include "ArenaShowDirector.h"
#include "LiveArenaSettings.h"

#include "ArenaAudience.h"
#include "ArenaNetSubsystem.h"
#include "ArenaScreen.h"
#include "ArenaVenue.h"
#include "LiveArena.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Engine/World.h"

// 燈光、鏡頭同抽獎。
// 所有燈都係 Movable、單位 candela；每個 模式 / 抽獎狀態 有一組目標亮度，每 tick 用 FInterpTo 慢慢過去。
// 色調只用暖色（琥珀 #FFB86B、暖白、象牙），唔用藍。
// 鏡頭係一部 ACameraActor，用 critically damped spring 喺 preset 之間滑過去；Free 時由 player controller 自己郁。
// Tick 喺 TG_PostPhysics：觀眾公仔（TG_PrePhysics）已經郁完，而 player camera manager 喺 PostPhysics 之後先讀鏡頭位，
// 所以追光同鏡頭都唔會慢一格。

// Named (not anonymous) namespace: unity builds paste several .cpp files into one translation unit.
namespace ArenaShowDetail
{
	constexpr float TwoPi = 6.28318530718f;

	// Intensity at level 1 (cd). House lights sit at 0.15 (= 180 cd) while just streaming: enough for the dark
	// anonymous crowd to read as silhouettes at the dark-venue exposure floor (EV100 -1).
	constexpr float HouseFullCd = 1200.f;
	constexpr float WashFullCd = 4000.f;
	constexpr float HeadFullCd = 20000.f;
	constexpr float FollowFullCd = 60000.f;
	constexpr float MinVisibleCd = 0.5f;          // dimmer than this the light is hidden instead of rendered dark

	constexpr float HouseAboveHeads = 750.f;
	constexpr float HouseAttenuation = 2500.f;
	constexpr float WashAttenuation = 3500.f;
	constexpr float HeadAttenuation = 6500.f;     // truss -> far corner of the back row is ~50 m
	constexpr float FollowAttenuation = 8000.f;   // back of the bowl -> guest chair is ~50 m

	constexpr float SpinHopFastest = 0.07f;
	constexpr float SpinHopSlowest = 0.6f;
	constexpr float LandedTimeout = 60.f;

	constexpr float CamHalfLife = 0.6f;
	constexpr float SeatedHead = 122.f;           // seated head height above the seat floor (audience figures)
	constexpr float ChestBelowHead = 30.f;        // follow spots aim here so the cone frames head and shoulders

	// Palette (sRGB).
	static const FColor Amber(255, 184, 107);     // #FFB86B
	static const FColor WarmWhite(255, 236, 214);
	static const FColor WashWhite(255, 228, 196);
	static const FColor FollowWhite(255, 243, 228);

	struct FLightTargets
	{
		float House;
		float Wash;
		float Heads;
		float FollowA;
		float FollowB;
		float Glow;
		float Hype;
	};

	/** Target levels (0..1) per mode / show state. Lottery states win over the mode. */
	static FLightTargets TargetsFor(EArenaMode Mode, EArenaShowState State)
	{
		switch (State)
		{
		case EArenaShowState::Spinning: return { 0.05f, 0.30f, 0.80f, 1.f, 0.f, 0.4f, 0.3f };
		case EArenaShowState::Landed:   return { 0.05f, 0.45f, 0.60f, 1.f, 0.f, 0.7f, 1.0f };
		case EArenaShowState::Walking:  return { 0.15f, 0.70f, 0.30f, 1.f, 0.85f, 0.7f, 0.8f };
		case EArenaShowState::Guest:    return { 0.25f, 1.00f, 0.35f, 1.f, 0.85f, 0.7f, 0.5f };
		default: break;
		}
		return Mode == EArenaMode::Watch
			? FLightTargets{ 0.15f, 1.00f, 0.f, 0.f, 0.f, 1.0f, 0.f }
			: FLightTargets{ 0.45f, 0.80f, 1.f, 0.f, 0.f, 0.7f, 0.6f };
	}

	/** A runtime light, configured but not registered yet (the caller sets the type-specific bits, then PlaceLight). */
	template <typename TLight>
	static TLight* NewLight(AActor* Owner, USceneComponent* Parent, const TCHAR* BaseName, const FColor& Color,
		float Attenuation, float Scattering, bool bShadows)
	{
		const FName Name = MakeUniqueObjectName(Owner, TLight::StaticClass(), FName(BaseName));
		TLight* Light = NewObject<TLight>(Owner, TLight::StaticClass(), Name, RF_Transient);
		Light->SetMobility(EComponentMobility::Movable);
		Light->SetupAttachment(Parent);
		Light->IntensityUnits = ELightUnits::Candelas;
		Light->Intensity = 0.f;
		Light->LightColor = Color;
		Light->AttenuationRadius = Attenuation;
		Light->VolumetricScatteringIntensity = Scattering;
		Light->CastShadows = bShadows;
		Light->bCastVolumetricShadow = bShadows;
		Light->SetVisibility(false); // ApplyLightLevels turns it on
		return Light;
	}

	static void PlaceLight(AActor* Owner, ULocalLightComponent* Light, const FVector& Location, const FVector& AimAt)
	{
		Light->RegisterComponent();
		Owner->AddInstanceComponent(Light);
		const FVector Dir = AimAt - Location;
		Light->SetWorldLocationAndRotation(Location, Dir.IsNearlyZero() ? FRotator(-90.f, 0.f, 0.f) : Dir.Rotation());
	}

	/** Sets a light's intensity only when it changed (every SetIntensity re-sends the light to the renderer). */
	static void SetCandelas(ULightComponent* Light, float Candelas)
	{
		if (!Light)
		{
			return;
		}
		const bool bOn = Candelas >= MinVisibleCd;
		if (Light->IsVisible() != bOn)
		{
			Light->SetVisibility(bOn);
		}
		if (bOn && !FMath::IsNearlyEqual(Light->Intensity, Candelas, FMath::Max(0.05f, Candelas * 0.002f)))
		{
			Light->SetIntensity(Candelas);
		}
	}

	/** Critically damped spring (exact for any time step). HalfLife ~ time to cover most of the remaining distance. */
	static void SpringTo(FVector& Value, FVector& Velocity, const FVector& Target, float HalfLife, float DeltaSeconds)
	{
		const float Y = 1.38629436f / FMath::Max(HalfLife, 0.001f); // 2 ln 2 / half-life
		const FVector J0 = Value - Target;
		const FVector J1 = Velocity + J0 * Y;
		const float Decay = FMath::Exp(-Y * DeltaSeconds);
		Value = Target + (J0 + J1 * DeltaSeconds) * Decay;
		Velocity = (Velocity - J1 * (Y * DeltaSeconds)) * Decay;
	}
}

AArenaShowDirector::AArenaShowDirector()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	// After the audience has moved its figures this frame (it ticks in TG_PrePhysics) and before the player camera
	// manager reads the camera actor (right after TG_PostPhysics), so the spots and the camera never lag a frame.
	PrimaryActorTick.TickGroup = TG_PostPhysics;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	RootComponent = Root;
}

// ------------------------------------------------------------------------------------------------
// Setup
// ------------------------------------------------------------------------------------------------

void AArenaShowDirector::Init(AArenaVenue* InVenue, AArenaAudience* InAudience, AArenaScreen* InScreen, UArenaNetSubsystem* InNet, float InExposureBias)
{
	Venue = InVenue;
	Audience = InAudience;
	Screen = InScreen;
	Net = InNet;
	ExposureBias = FMath::Clamp(InExposureBias, -4.f, 4.f);

	if (!IsValid(Venue))
	{
		UE_LOG(LogLiveArena, Warning, TEXT("ShowDirector: no venue, lights and camera use the default layout."));
	}
	if (IsValid(Audience))
	{
		AddTickPrerequisiteActor(Audience); // figures first, then the spots / camera that follow them
	}

	SpawnLights();
	SpawnAtmosphere();

	UWorld* World = GetWorld();
	if (!CameraActor && World)
	{
		FActorSpawnParameters Params;
		Params.Owner = this;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.ObjectFlags |= RF_Transient;
		CameraActor = World->SpawnActor<ACameraActor>(GetActorLocation(), FRotator::ZeroRotator, Params);
		if (CameraActor)
		{
			if (UCameraComponent* CameraComp = CameraActor->GetCameraComponent())
			{
				CameraComp->SetFieldOfView(60.f);
				CameraComp->bConstrainAspectRatio = false; // fill whatever window shape OBS captures
			}
		}
		else
		{
			UE_LOG(LogLiveArena, Error, TEXT("ShowDirector: could not spawn the camera actor."));
		}
	}

	State = EArenaShowState::Idle;
	StateTime = 0.f;
	Winner = FArenaViewer();
	SpinTargetId.Reset();
	SkipId.Reset();

	SetMode(EArenaMode::Watch);
	SetCamera(EArenaCamera::Broadcast);
	bCamInit = false;

	// First frame already shows the Watch look (no fade up from black) and the camera sits on its preset.
	ApplyLightLevels(0.f);
	UpdateCamera(0.f);
	bReady = true;
}

void AArenaShowDirector::SpawnLights()
{
	using namespace ArenaShowDetail;

	if (HouseLights.Num() > 0 || StageWash.Num() > 0 || MovingHeads.Num() > 0 || FollowSpotA || FollowSpotB)
	{
		return; // Init ran twice: keep the rig
	}

	const AArenaVenue* V = Layout();
	const FTransform Xf = V->GetActorTransform();
	const FVector Up = Xf.GetRotation().GetUpVector();
	const float HalfArc = FMath::DegreesToRadians(V->ArcDegrees) * 0.5f;
	const float LastRow = static_cast<float>(FMath::Max(V->NumRows - 1, 0));
	const FVector StageAim = V->GetHostMarkTransform().GetLocation() + Up * 120.f;

	// ---- house lights: two warm rings hanging over the bowl (no shadows: there are many of them) ----
	{
		struct FHouseSpot
		{
			float Row;
			float ThetaAlpha;
		};
		const float InnerRow = FMath::FloorToFloat(LastRow * 0.3f);
		const float OuterRow = FMath::FloorToFloat(LastRow * 0.75f);
		const FHouseSpot Spots[] = {
			{ InnerRow, -0.55f }, { InnerRow, 0.f }, { InnerRow, 0.55f },
			{ OuterRow, -0.75f }, { OuterRow, -0.25f }, { OuterRow, 0.25f }, { OuterRow, 0.75f },
		};
		for (const FHouseSpot& Spot : Spots)
		{
			UPointLightComponent* Light = NewLight<UPointLightComponent>(this, Root, TEXT("HouseLight"), Amber, HouseAttenuation, 0.3f, false);
			const FVector Location = BowlPoint(Spot.Row, Spot.ThetaAlpha * HalfArc, SeatedHead + HouseAboveHeads);
			PlaceLight(this, Light, Location, Location - Up * 100.f);
			HouseLights.Add(Light);
		}
	}

	// ---- stage wash: three wide warm-white spots on the front truss line, aimed down at the deck ----
	const TArray<FVector> Mounts = V->GetTrussLightMounts();
	FVector TrussLocal(150.f, 0.f, V->StageHeight + V->ScreenBottom + V->ScreenHeight + 100.f);
	if (Mounts.Num() > 0)
	{
		TrussLocal = Xf.InverseTransformPosition(Mounts[0]);
	}
	{
		const float WashY[] = { -0.3f, 0.f, 0.3f };
		for (const float Alpha : WashY)
		{
			USpotLightComponent* Light = NewLight<USpotLightComponent>(this, Root, TEXT("StageWash"), WashWhite, WashAttenuation, 0.5f, true);
			Light->InnerConeAngle = 20.f;
			Light->OuterConeAngle = 35.f;
			Light->bCastVolumetricShadow = false; // shadows on the deck, but no shadowed haze (cheaper)
			const FVector From = Xf.TransformPosition(FVector(TrussLocal.X - 40.0, Alpha * V->StageWidth, TrussLocal.Z));
			const FVector To = Xf.TransformPosition(FVector(V->StageDepth * 0.42f, Alpha * V->StageWidth * 0.6f, V->StageHeight));
			PlaceLight(this, Light, From, To);
			StageWash.Add(Light);
		}
	}

	// ---- moving heads: narrow beams from the truss mounts, warm white / amber alternating ----
	for (int32 Index = 0; Index < Mounts.Num(); ++Index)
	{
		USpotLightComponent* Light = NewLight<USpotLightComponent>(this, Root, TEXT("MovingHead"),
			(Index % 2 == 0) ? WarmWhite : Amber, HeadAttenuation, 4.f, false);
		Light->InnerConeAngle = 2.f;
		Light->OuterConeAngle = 6.f;
		PlaceLight(this, Light, Mounts[Index], BowlPoint(LastRow * 0.5f, 0.f, 0.f));
		MovingHeads.Add(Light);
	}

	// ---- follow spots: high at the back of the bowl, tight and strong, shadowed ----
	auto MakeFollowSpot = [this, V, &StageAim](int32 Index)
	{
		USpotLightComponent* Light = NewLight<USpotLightComponent>(this, Root, Index == 0 ? TEXT("FollowSpotA") : TEXT("FollowSpotB"),
			FollowWhite, FollowAttenuation, 3.f, true);
		Light->InnerConeAngle = 2.5f;
		Light->OuterConeAngle = 4.5f;
		PlaceLight(this, Light, V->GetFollowSpotMount(Index), StageAim);
		return Light;
	};
	FollowSpotA = MakeFollowSpot(0);
	FollowSpotB = MakeFollowSpot(1);

	UE_LOG(LogLiveArena, Log, TEXT("ShowDirector: %d house, %d wash, %d moving heads, 2 follow spots."),
		HouseLights.Num(), StageWash.Num(), MovingHeads.Num());
}

void AArenaShowDirector::SpawnAtmosphere()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const AArenaVenue* V = Layout();
	const FVector Floor = V->GetActorLocation();
	const FVector Up = V->GetActorTransform().GetRotation().GetUpVector();

	FActorSpawnParameters Params;
	Params.Owner = this;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.ObjectFlags |= RF_Transient;

	// ---- haze: volumetric fog so the beams read ----
	if (!Fog)
	{
		Fog = World->SpawnActor<AExponentialHeightFog>(Floor, FRotator::ZeroRotator, Params);
		if (UExponentialHeightFogComponent* FogComp = Fog ? Fog->GetComponent() : nullptr)
		{
			FogComp->SetFogDensity(0.015f);
			FogComp->SetFogInscatteringColor(FLinearColor(0.018f, 0.012f, 0.008f)); // very dark, warm
			FogComp->SetVolumetricFog(true);
			FogComp->SetVolumetricFogScatteringDistribution(0.4f);
			FogComp->SetVolumetricFogExtinctionScale(0.6f);
		}
	}

	// ---- a whisper of sky light so Lumen never goes fully black in unlit corners ----
	if (!Sky)
	{
		Sky = World->SpawnActor<ASkyLight>(Floor + Up * 2000.f, FRotator::ZeroRotator, Params);
		if (USkyLightComponent* SkyComp = Sky ? Sky->GetLightComponent() : nullptr)
		{
			// ASkyLight's component starts Stationary; re-register it as a movable real-time capture.
			SkyComp->UnregisterComponent();
			SkyComp->SetMobility(EComponentMobility::Movable);
			SkyComp->SourceType = SLS_CapturedScene;
			SkyComp->bRealTimeCapture = true;
			SkyComp->Intensity = 0.05f;
			SkyComp->LightColor = FColor(255, 214, 178);
			SkyComp->bLowerHemisphereIsBlack = true;
			SkyComp->LowerHemisphereColor = FLinearColor(0.05f, 0.035f, 0.025f);
			SkyComp->RegisterComponent();
		}
	}

	// ---- post: histogram auto exposure clamped to a dark-venue EV range, bloom, vignette, grain ----
	// Not manual exposure: there is no measured scene to pick a fixed EV from, and the clamp keeps the LED bright.
	// Adaptation is slow so dimming for the lottery still reads and moving-head beams do not make the picture pump.
	if (!PostVolume)
	{
		PostVolume = World->SpawnActor<APostProcessVolume>(Floor, FRotator::ZeroRotator, Params);
		if (PostVolume)
		{
			PostVolume->bUnbound = true;
			PostVolume->Priority = 10.f;
			PostVolume->BlendWeight = 1.f;

			FPostProcessSettings& Settings = PostVolume->Settings;
			Settings.bOverride_AutoExposureMethod = true;
			Settings.AutoExposureMethod = AEM_Histogram;
			// EV100 (DefaultEngine.ini sets r.DefaultFeature.AutoExposure.ExtendDefaultLuminanceRange=True).
			Settings.bOverride_AutoExposureMinBrightness = true;
			Settings.AutoExposureMinBrightness = -1.f;
			Settings.bOverride_AutoExposureMaxBrightness = true;
			Settings.AutoExposureMaxBrightness = 3.f;
			Settings.bOverride_AutoExposureBias = true;
			Settings.AutoExposureBias = ExposureBias;
			Settings.bOverride_AutoExposureSpeedUp = true;
			Settings.AutoExposureSpeedUp = 0.8f;
			Settings.bOverride_AutoExposureSpeedDown = true;
			Settings.AutoExposureSpeedDown = 0.8f;
			Settings.bOverride_BloomIntensity = true;
			Settings.BloomIntensity = 0.8f;
			Settings.bOverride_VignetteIntensity = true;
			Settings.VignetteIntensity = 0.5f;
			Settings.bOverride_FilmGrainIntensity = true;
			Settings.FilmGrainIntensity = 0.15f;
		}
	}
}

// ------------------------------------------------------------------------------------------------
// Mode / exposure / messages
// ------------------------------------------------------------------------------------------------

void AArenaShowDirector::SetMode(EArenaMode InMode)
{
	const bool bChanged = InMode != Mode;
	Mode = InMode;

	// Init sends the starting mode once; the net subsystem re-sends it after every reconnect.
	if (Net && (bChanged || !bReady))
	{
		Net->SendMode(Mode);
	}
	if (!bChanged)
	{
		return;
	}

	UE_LOG(LogLiveArena, Log, TEXT("ShowDirector: mode -> %s"), Mode == EArenaMode::Interactive ? TEXT("Interactive") : TEXT("Watch"));

	if (Mode == EArenaMode::Watch && State != EArenaShowState::Idle)
	{
		// Back to plain streaming ends the lottery / guest segment.
		StopShow(TEXT("返回直播模式"), TEXT("抽獎／嘉賓環節已經結束"));
		return;
	}
	if (bReady)
	{
		if (Mode == EArenaMode::Interactive)
		{
			PostMessage(TEXT("互動模式"), TEXT("場燈著咗、搖頭燈掃觀眾席 · L 抽獎"), 0.f);
		}
		else
		{
			PostMessage(TEXT("直播模式"), TEXT("燈光收返，專心直播"), 0.f);
		}
	}
}

void AArenaShowDirector::ToggleMode()
{
	SetMode(Mode == EArenaMode::Watch ? EArenaMode::Interactive : EArenaMode::Watch);
}

void AArenaShowDirector::AdjustExposure(float DeltaEv)
{
	ExposureBias = FMath::Clamp(ExposureBias + DeltaEv, -4.f, 4.f);
	if (IsValid(PostVolume))
	{
		PostVolume->Settings.bOverride_AutoExposureBias = true;
		PostVolume->Settings.AutoExposureBias = ExposureBias;
	}
	UE_LOG(LogLiveArena, Log, TEXT("ShowDirector: exposure bias %.1f EV"), ExposureBias);
}

void AArenaShowDirector::PostMessage(const FString& Big, const FString& Small, float LedSeconds)
{
	UE_LOG(LogLiveArena, Log, TEXT("Show: %s | %s"), *Big, *Small);
	OnMessage.Broadcast(Big, Small);
	if (LedSeconds > 0.f && IsValid(Screen))
	{
		Screen->ShowToast(Big, Small, LedSeconds);
	}
}

// ------------------------------------------------------------------------------------------------
// Lottery / guest
// ------------------------------------------------------------------------------------------------

bool AArenaShowDirector::StartLottery()
{
	using namespace ArenaShowDetail;

	if (!IsValid(Audience))
	{
		PostMessage(TEXT("抽唔到獎"), TEXT("觀眾席未準備好"), 0.f);
		return false;
	}
	if (State == EArenaShowState::Walking || State == EArenaShowState::Guest)
	{
		PostMessage(TEXT("嘉賓仲喺台上"), TEXT("撳 K 請佢返座位先，再抽下一位"), 0.f);
		return false;
	}
	if (State == EArenaShowState::Spinning)
	{
		return false; // already spinning
	}
	const int32 Count = Audience->GetNamedCount();
	if (Count == 0)
	{
		PostMessage(TEXT("未有觀眾入場"), TEXT("分享入場 link 畀觀眾先"), 0.f);
		return false;
	}

	if (Mode != EArenaMode::Interactive)
	{
		SetMode(EArenaMode::Interactive);
	}

	// L again while Landed = re-roll (the winner did not respond): they sit this spin out if anyone else is in.
	SkipId = State == EArenaShowState::Landed ? Winner.Id : FString();
	Winner = FArenaViewer();
	SpinTargetId.Reset();
	State = EArenaShowState::Spinning;
	StateTime = 0.f;
	SpinElapsed = 0.f;
	HopSpinTarget(FString());
	NextHop = HopInterval();

	if (Net)
	{
		Net->SendLotterySpin();
	}
	if (IsValid(Screen))
	{
		Screen->ShowToast(TEXT("抽獎中"), TEXT("追光燈揀緊人…"), 0.f);
	}
	AutoCamera(EArenaCamera::Wide);
	PostMessage(TEXT("抽獎中"), FString::Printf(TEXT("喺 %d 位入咗場嘅觀眾之中抽一位…"), Count), 0.f);
	return true;
}

void AArenaShowDirector::InviteWinner()
{
	if (State != EArenaShowState::Landed)
	{
		if (State == EArenaShowState::Idle)
		{
			PostMessage(TEXT("未有得獎者"), TEXT("撳 L 抽獎先"), 0.f);
		}
		return;
	}

	FArenaViewer Current;
	if (!IsValid(Audience) || !Audience->GetViewer(Winner.Id, Current))
	{
		StopShow(TEXT("得獎者已經離場"), TEXT("撳 L 再抽過"));
		return;
	}
	if (Current.Label.IsEmpty())
	{
		Current.Label = Winner.Label;
	}
	Winner = Current; // fresh slot / label

	const AArenaVenue* V = Layout();
	Audience->SendToStage(Winner.Id, V->GetWalkPath(Winner.Slot), V->GetGuestChairTransform());
	State = EArenaShowState::Walking;
	StateTime = 0.f;

	// Right away, not once seated: the walk takes 10-50 s, which is when the guest needs to open the call link.
	// The server forwards this to everyone and sends the call link to the guest alone.
	if (Net)
	{
		Net->SendGuest(Winner.Id);
	}

	if (IsValid(Screen))
	{
		Screen->ShowToast(FString::Printf(TEXT("有請 %s"), *Winner.Name), Winner.Label + TEXT(" · 上台中"), 0.f);
	}
	OnMessage.Broadcast(FString::Printf(TEXT("有請 %s 上台"), *Winner.Name), GetDefault<ULiveArenaSettings>()->GuestCallUrl.IsEmpty() ? FString(TEXT("未設定 GuestCallUrl：佢網頁冇通話 link · 行緊上台")) : FString(TEXT("佢嘅網頁已經收到通話 link · 行緊上台")));
	AutoCamera(EArenaCamera::Follow);
	UE_LOG(LogLiveArena, Log, TEXT("ShowDirector: inviting %s (%s) on stage."), *Winner.Name, *Winner.Id);
}

void AArenaShowDirector::EndGuest()
{
	switch (State)
	{
	case EArenaShowState::Idle:
		return;

	case EArenaShowState::Spinning:
	case EArenaShowState::Landed:
		StopShow(TEXT("抽獎取消"), TEXT("撳 L 可以再抽"));
		return;

	default: // Walking / Guest
	{
		const FString Name = Winner.Name;
		StopShow(TEXT("嘉賓落台"), FString::Printf(TEXT("%s 返咗座位"), *Name));
		if (IsValid(Screen))
		{
			Screen->ShowToast(FString::Printf(TEXT("多謝 %s"), *Name), TEXT("掌聲送返座位"), 3.f);
		}
		if (IsValid(Audience))
		{
			Audience->ClapRandom(60);
		}
		return;
	}
	}
}

void AArenaShowDirector::NotifyViewerLeft(const FString& Id)
{
	if (Id.IsEmpty() || State == EArenaShowState::Idle)
	{
		return;
	}
	if (Id == SkipId)
	{
		SkipId.Reset();
	}

	if (State == EArenaShowState::Spinning)
	{
		// The spot was on them: hop to someone else right away (they are still in the audience at this point).
		if (Id == SpinTargetId && !HopSpinTarget(Id))
		{
			StopShow(TEXT("抽獎取消"), TEXT("場內已經冇入咗場嘅觀眾"));
		}
		return;
	}

	if (Id != Winner.Id)
	{
		return;
	}
	const FString Name = Winner.Name;
	if (State == EArenaShowState::Landed)
	{
		StopShow(FString::Printf(TEXT("%s 離咗場"), *Name), TEXT("抽獎取消 · 撳 L 再抽過"));
	}
	else
	{
		StopShow(FString::Printf(TEXT("嘉賓 %s 離咗場"), *Name), TEXT("已經請佢落台"));
	}
}

void AArenaShowDirector::UpdateLottery(float DeltaSeconds)
{
	using namespace ArenaShowDetail;

	StateTime += DeltaSeconds;

	switch (State)
	{
	case EArenaShowState::Idle:
		return;

	case EArenaShowState::Spinning:
	{
		if (!IsValid(Audience))
		{
			StopShow(TEXT("抽獎取消"), TEXT("觀眾席唔見咗"));
			return;
		}
		SpinElapsed += DeltaSeconds;

		// Target gone without a leave event (e.g. a snapshot after a reconnect): move on.
		if (!IsViewerPresent(SpinTargetId) && !HopSpinTarget(FString()))
		{
			StopShow(TEXT("抽獎取消"), TEXT("場內已經冇入咗場嘅觀眾"));
			return;
		}
		if (SpinElapsed >= SpinDuration)
		{
			LandLottery();
			return;
		}
		NextHop -= DeltaSeconds;
		if (NextHop <= 0.f)
		{
			HopSpinTarget(FString());
			NextHop = HopInterval();
		}
		return;
	}

	case EArenaShowState::Landed:
		if (!IsViewerPresent(Winner.Id))
		{
			StopShow(TEXT("得獎者離咗場"), TEXT("撳 L 再抽過"));
		}
		else if (StateTime >= LandedTimeout)
		{
			StopShow(TEXT("抽獎已取消"), TEXT("60 秒內冇請上台"));
		}
		return;

	case EArenaShowState::Walking:
		if (!IsViewerPresent(Winner.Id))
		{
			StopShow(TEXT("嘉賓離咗場"), TEXT("已經請佢落台"));
		}
		else if (!Audience->IsWalking())
		{
			BecomeGuest();
		}
		return;

	case EArenaShowState::Guest:
		if (!IsViewerPresent(Winner.Id))
		{
			StopShow(TEXT("嘉賓離咗場"), TEXT("已經請佢落台"));
		}
		return;

	default:
		return;
	}
}

bool AArenaShowDirector::HopSpinTarget(const FString& LeavingId)
{
	if (!IsValid(Audience))
	{
		return false;
	}

	const int32 Count = Audience->GetNamedCount();
	FArenaViewer Pick;
	for (int32 Attempt = 0; Attempt < 6; ++Attempt)
	{
		FArenaViewer Candidate;
		// Excluding the current target makes every hop visibly move.
		if (!Audience->PickRandomNamed(Candidate, SpinTargetId))
		{
			break;
		}
		if (Candidate.Id == LeavingId || (Candidate.Id == SkipId && Count > 2))
		{
			continue;
		}
		Pick = Candidate;
		break;
	}

	if (Pick.Id.IsEmpty())
	{
		// Nobody else usable: stay on the current target if it is still here and not the one leaving.
		if (SpinTargetId != LeavingId && IsViewerPresent(SpinTargetId))
		{
			return true;
		}
		if (!Audience->PickRandomNamed(Pick, LeavingId))
		{
			SpinTargetId.Reset();
			return false;
		}
	}

	SpinTargetId = Pick.Id;
	return true;
}

float AArenaShowDirector::HopInterval() const
{
	using namespace ArenaShowDetail;

	const float Alpha = FMath::Clamp(SpinElapsed / FMath::Max(SpinDuration, 0.1f), 0.f, 1.f);
	return FMath::Lerp(SpinHopFastest, SpinHopSlowest, Alpha * Alpha);
}

void AArenaShowDirector::LandLottery()
{
	FArenaViewer Final;
	bool bFound = IsValid(Audience) && !SpinTargetId.IsEmpty() && Audience->GetViewer(SpinTargetId, Final);
	if (!bFound && HopSpinTarget(FString()))
	{
		bFound = Audience->GetViewer(SpinTargetId, Final);
	}
	if (!bFound)
	{
		StopShow(TEXT("抽獎取消"), TEXT("場內已經冇入咗場嘅觀眾"));
		return;
	}

	// A re-roll never lands on the previous winner while anyone else is in.
	if (Final.Id == SkipId && Audience->GetNamedCount() > 1)
	{
		FArenaViewer Other;
		if (Audience->PickRandomNamed(Other, SkipId))
		{
			Final = Other;
		}
	}
	if (Final.Label.IsEmpty() && IsValid(Venue))
	{
		Final.Label = Venue->GetSlotLabel(Final.Slot);
	}

	Winner = Final;
	SpinTargetId = Final.Id;
	SkipId.Reset();
	State = EArenaShowState::Landed;
	StateTime = 0.f;

	if (Net)
	{
		Net->SendLotteryWinner(Winner.Id);
	}
	const FString Big = FString::Printf(TEXT("抽中 %s"), *Winner.Name);
	if (IsValid(Screen))
	{
		Screen->ShowToast(Big, Winner.Label + TEXT(" · 請上台"), 0.f);
	}
	OnMessage.Broadcast(Big, FString::Printf(TEXT("%s · Enter 請佢上台 · L 再抽 · K 取消"), *Winner.Label));
	Audience->Clap(Winner.Id); // the winner jumps up
	AutoCamera(EArenaCamera::Follow);
	UE_LOG(LogLiveArena, Log, TEXT("ShowDirector: lottery landed on %s (%s, %s)."), *Winner.Name, *Winner.Label, *Winner.Id);
}

void AArenaShowDirector::BecomeGuest()
{
	State = EArenaShowState::Guest;
	StateTime = 0.f;

	if (IsValid(Screen))
	{
		Screen->ShowToast(FString::Printf(TEXT("嘉賓 %s"), *Winner.Name), TEXT("歡迎上台"), 4.f);
	}
	OnMessage.Broadcast(FString::Printf(TEXT("嘉賓 %s 已經上台"), *Winner.Name), GetDefault<ULiveArenaSettings>()->GuestCallUrl.IsEmpty() ? FString(TEXT("未設定 GuestCallUrl · K 送佢返座位")) : FString(TEXT("通話 link 已經發咗畀佢 · K 送佢返座位")));
	if (IsValid(Audience))
	{
		Audience->ClapRandom(80); // welcome applause
	}
	UE_LOG(LogLiveArena, Log, TEXT("ShowDirector: %s is the guest."), *Winner.Name);
}

void AArenaShowDirector::StopShow(const FString& Big, const FString& Small)
{
	const EArenaShowState Old = State;
	if (Old == EArenaShowState::Idle)
	{
		return;
	}

	const bool bOnStage = Old == EArenaShowState::Walking || Old == EArenaShowState::Guest;
	if (bOnStage && IsValid(Audience))
	{
		Audience->ReturnFromStage(Winner.Id);
	}
	// Every step is on the audience pages (spin / winner overlay or the guest): guest "" clears the guest and also
	// cancels a lottery still showing there (the protocol has no separate cancel message).
	if (Net)
	{
		Net->SendGuest(FString());
	}

	State = EArenaShowState::Idle;
	StateTime = 0.f;
	SpinElapsed = 0.f;
	NextHop = 0.f;
	Winner = FArenaViewer();
	SpinTargetId.Reset();
	SkipId.Reset();

	if (IsValid(Screen))
	{
		Screen->ClearToast();
	}
	AutoCamera(EArenaCamera::Broadcast);

	if (!Big.IsEmpty())
	{
		PostMessage(Big, Small, 0.f);
	}
}

// ------------------------------------------------------------------------------------------------
// Tick: lights
// ------------------------------------------------------------------------------------------------

void AArenaShowDirector::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// A hitch must not fling the spots or the camera across the hall.
	const float Dt = FMath::Min(DeltaSeconds, 0.1f);
	Time += Dt;

	UpdateLottery(Dt);
	ApplyLightLevels(Dt);
	UpdateMovingHeads(Dt);
	AimFollowSpots(Dt);
	UpdateCamera(Dt);
}

void AArenaShowDirector::ApplyLightLevels(float DeltaSeconds)
{
	using namespace ArenaShowDetail;

	const FLightTargets Targets = TargetsFor(Mode, State);
	HouseLevel = FMath::FInterpTo(HouseLevel, Targets.House, DeltaSeconds, 1.8f);
	WashLevel = FMath::FInterpTo(WashLevel, Targets.Wash, DeltaSeconds, 1.8f);
	HeadsLevel = FMath::FInterpTo(HeadsLevel, Targets.Heads, DeltaSeconds, 2.5f);
	FollowLevel = FMath::FInterpTo(FollowLevel, Targets.FollowA, DeltaSeconds, 5.f);
	FollowBLevel = FMath::FInterpTo(FollowBLevel, Targets.FollowB, DeltaSeconds, 3.f);
	GlowLevel = FMath::FInterpTo(GlowLevel, Targets.Glow, DeltaSeconds, 2.f);
	HypeLevel = FMath::FInterpTo(HypeLevel, Targets.Hype, DeltaSeconds, 1.5f);

	for (UPointLightComponent* Light : HouseLights)
	{
		SetCandelas(Light, HouseLevel * HouseFullCd);
	}
	for (USpotLightComponent* Light : StageWash)
	{
		SetCandelas(Light, WashLevel * WashFullCd);
	}
	for (USpotLightComponent* Light : MovingHeads)
	{
		SetCandelas(Light, HeadsLevel * HeadFullCd);
	}
	SetCandelas(FollowSpotA.Get(), FollowLevel * FollowFullCd);
	SetCandelas(FollowSpotB.Get(), FollowBLevel * FollowFullCd);

	if (IsValid(Screen))
	{
		Screen->SetGlow(GlowLevel);
	}
	if (IsValid(Audience))
	{
		Audience->SetHype(HypeLevel);
	}
}

void AArenaShowDirector::UpdateMovingHeads(float DeltaSeconds)
{
	using namespace ArenaShowDetail;

	const int32 Count = MovingHeads.Num();
	if (Count == 0)
	{
		return;
	}

	// Phase clock instead of Time * speed, so changing speed never makes the beams jump.
	const float SpeedTarget = State == EArenaShowState::Spinning ? 3.2f
		: (State == EArenaShowState::Walking || State == EArenaShowState::Guest) ? 0.6f : 1.f;
	HeadsSpeed = FMath::FInterpTo(HeadsSpeed, SpeedTarget, DeltaSeconds, 1.5f);
	HeadsClock += DeltaSeconds * HeadsSpeed;

	if (HeadsLevel < 0.002f && TargetsFor(Mode, State).Heads <= 0.f)
	{
		return; // dark and staying dark (Watch): leave them where they are
	}

	const AArenaVenue* V = Layout();
	const FQuat VenueRotation = V->GetActorTransform().GetRotation();
	const float HalfArc = FMath::DegreesToRadians(V->ArcDegrees) * 0.5f;
	const float LastRow = static_cast<float>(FMath::Max(V->NumRows - 1, 0));

	// Landed: every head converges on the winner in a slowly turning ring.
	FVector WinnerHead = FVector::ZeroVector;
	const bool bConverge = State == EArenaShowState::Landed && GetViewerHead(Winner.Id, WinnerHead);
	const float Sharpness = bConverge ? 3.f : (State == EArenaShowState::Spinning ? 9.f : 4.f);

	for (int32 Index = 0; Index < Count; ++Index)
	{
		FVector Target;
		if (bConverge)
		{
			const float Angle = TwoPi * Index / Count + HeadsClock * 0.9f;
			Target = WinnerHead
				+ VenueRotation.GetForwardVector() * (110.f * FMath::Cos(Angle))
				+ VenueRotation.GetRightVector() * (110.f * FMath::Sin(Angle))
				- VenueRotation.GetUpVector() * 70.f;
		}
		else
		{
			// Lissajous sweep over the seats; the two halves of the truss mirror each other.
			const int32 Pair = FMath::Min(Index, Count - 1 - Index);
			const float Mirror = Index < Count / 2 ? -1.f : 1.f;
			const float Phase = Pair * 1.1f;
			const float Across = FMath::Sin(HeadsClock * (0.50f + 0.06f * Pair) + Phase);
			const float Depth = FMath::Sin(HeadsClock * (0.33f + 0.04f * Pair) + Phase * 1.7f + 0.8f);
			const float Theta = Mirror * Across * HalfArc * 0.9f;
			const float Row = FMath::FloorToFloat((0.5f + 0.5f * Depth) * LastRow);
			Target = BowlPoint(Row, Theta, 70.f);
		}
		AimSpot(MovingHeads[Index], Target, DeltaSeconds, Sharpness);
	}
}

void AArenaShowDirector::AimFollowSpots(float DeltaSeconds)
{
	using namespace ArenaShowDetail;

	const AArenaVenue* V = Layout();
	const FVector Up = V->GetActorTransform().GetRotation().GetUpVector();
	const FVector Rest = V->GetHostMarkTransform().GetLocation() + Up * 120.f;

	FVector Target = FVector::ZeroVector;
	bool bHasTarget = false;
	float Sharpness = 6.f;
	switch (State)
	{
	case EArenaShowState::Spinning:
		bHasTarget = GetViewerHead(SpinTargetId, Target);
		Sharpness = 16.f; // snappy: the spot jumps seat to seat
		break;
	case EArenaShowState::Landed:
		bHasTarget = GetViewerHead(Winner.Id, Target);
		Sharpness = 8.f;
		break;
	case EArenaShowState::Walking:
		bHasTarget = GetViewerHead(Winner.Id, Target);
		Sharpness = 10.f;
		break;
	case EArenaShowState::Guest:
		bHasTarget = GetViewerHead(Winner.Id, Target);
		Sharpness = 6.f;
		break;
	default:
		break;
	}
	Target -= Up * ChestBelowHead;

	if (FollowSpotA)
	{
		if (bHasTarget)
		{
			AimSpot(FollowSpotA, Target, DeltaSeconds, Sharpness);
		}
		else if (FollowLevel < 0.01f)
		{
			AimSpot(FollowSpotA, Rest, DeltaSeconds, 2.f); // park on the stage once dark
		}
	}
	if (FollowSpotB)
	{
		// B stays dark until the walk but already tracks the winner while Landed, so it fades in on target.
		if (bHasTarget && State != EArenaShowState::Spinning)
		{
			AimSpot(FollowSpotB, Target, DeltaSeconds, Sharpness);
		}
		else if (FollowBLevel < 0.01f)
		{
			AimSpot(FollowSpotB, Rest, DeltaSeconds, 2.f);
		}
	}
}

void AArenaShowDirector::AimSpot(USpotLightComponent* Spot, const FVector& Target, float DeltaSeconds, float Sharpness)
{
	if (!Spot)
	{
		return;
	}
	const FVector Dir = Target - Spot->GetComponentLocation();
	if (Dir.IsNearlyZero())
	{
		return;
	}
	const FRotator Current = Spot->GetComponentRotation();
	const FRotator Wanted = Dir.Rotation();
	const FRotator Next = Sharpness > 0.f ? FMath::RInterpTo(Current, Wanted, DeltaSeconds, Sharpness) : Wanted;
	if (!Next.Equals(Current, 0.005f))
	{
		Spot->SetWorldRotation(Next); // skipped when parked: every move re-sends the light to the renderer
	}
}

// ------------------------------------------------------------------------------------------------
// Camera
// ------------------------------------------------------------------------------------------------

void AArenaShowDirector::SetCamera(EArenaCamera InCamera)
{
	const EArenaCamera Old = Camera;
	Camera = InCamera;

	if (Old == EArenaCamera::Free && InCamera != EArenaCamera::Free && IsValid(CameraActor))
	{
		// Leave from wherever the streamer flew to: re-seed the springs from the actor.
		CamPos = CameraActor->GetActorLocation();
		CamLook = CamPos + CameraActor->GetActorForwardVector() * 1500.f;
		CamVel = FVector::ZeroVector;
		CamLookVel = FVector::ZeroVector;
		if (const UCameraComponent* CameraComp = CameraActor->GetCameraComponent())
		{
			CamFov = CameraComp->FieldOfView;
		}
		bCamInit = true;
	}
	if (Old != InCamera)
	{
		UE_LOG(LogLiveArena, Verbose, TEXT("ShowDirector: camera %d -> %d"), static_cast<int32>(Old), static_cast<int32>(InCamera));
	}
}

void AArenaShowDirector::AutoCamera(EArenaCamera InCamera)
{
	if (Camera != EArenaCamera::Free)
	{
		SetCamera(InCamera);
	}
}

void AArenaShowDirector::UpdateCamera(float DeltaSeconds)
{
	using namespace ArenaShowDetail;

	if (!IsValid(CameraActor) || Camera == EArenaCamera::Free)
	{
		return; // Free: the player controller flies the camera actor itself
	}

	FVector TargetPos = FVector::ZeroVector;
	FVector TargetLook = FVector::ZeroVector;
	float TargetFov = 60.f;
	ComputeCameraTarget(TargetPos, TargetLook, TargetFov);

	if (!bCamInit)
	{
		CamPos = TargetPos;
		CamLook = TargetLook;
		CamVel = FVector::ZeroVector;
		CamLookVel = FVector::ZeroVector;
		CamFov = TargetFov;
		bCamInit = true;
	}
	else
	{
		SpringTo(CamPos, CamVel, TargetPos, CamHalfLife, DeltaSeconds);
		SpringTo(CamLook, CamLookVel, TargetLook, CamHalfLife * 0.8f, DeltaSeconds);
		CamFov = FMath::FInterpTo(CamFov, TargetFov, DeltaSeconds, 2.5f);
	}

	const FVector Dir = CamLook - CamPos;
	const FRotator Rotation = Dir.IsNearlyZero() ? CameraActor->GetActorRotation() : Dir.Rotation();
	CameraActor->SetActorLocationAndRotation(CamPos, Rotation);

	if (UCameraComponent* CameraComp = CameraActor->GetCameraComponent())
	{
		if (!FMath::IsNearlyEqual(CameraComp->FieldOfView, CamFov, 0.01f))
		{
			CameraComp->SetFieldOfView(CamFov);
		}
	}
}

void AArenaShowDirector::ComputeCameraTarget(FVector& OutPos, FVector& OutLook, float& OutFov) const
{
	using namespace ArenaShowDetail;

	const AArenaVenue* V = Layout();
	const FTransform Xf = V->GetActorTransform();
	const FQuat VenueRotation = Xf.GetRotation();
	const FVector Up = VenueRotation.GetUpVector();
	const FVector Right = VenueRotation.GetRightVector();
	const int32 Rows = FMath::Max(V->NumRows, 1);

	// Follow needs someone on the spot; without a winner / guest it is the broadcast view.
	EArenaCamera Preset = Camera;
	FVector Head = FVector::ZeroVector;
	if (Preset == EArenaCamera::Follow)
	{
		const bool bHasSubject = (State == EArenaShowState::Landed || State == EArenaShowState::Walking || State == EArenaShowState::Guest)
			&& GetViewerHead(Winner.Id, Head);
		if (!bHasSubject)
		{
			Preset = EArenaCamera::Broadcast;
		}
	}

	switch (Preset)
	{
	case EArenaCamera::StreamerPOV:
	{
		// The streamer's eyes on the host mark, looking out over the whole crowd, breathing a little.
		// The sway goes into the target (slow enough to pass the spring) so switching away never pops.
		const FVector Eye = V->GetHostMarkTransform().GetLocation() + Up * 170.f;
		const FVector Look = V->GetAudienceCentre() + Up * 60.f;
		const float Breath = FMath::Sin(Time * TwoPi / 4.8f);
		const float Drift = FMath::Sin(Time * TwoPi / 11.f);
		OutPos = Eye + Up * (2.5f * Breath) + Right * (6.f * Drift);
		OutLook = Look + Up * (8.f * Breath) + Right * (30.f * FMath::Sin(Time * TwoPi / 17.f));
		OutFov = StreamerPovFov(Eye, Look);
		return;
	}

	case EArenaCamera::Wide:
	{
		// High above the back rows, slowly swinging left / right, looking down at the stage and the whole bowl.
		const float Swing = FMath::Sin(Time * TwoPi / 48.f);
		const float Theta = Swing * 0.17f;
		const float Radius = V->GetOuterRadius() - 100.f;
		OutPos = Xf.TransformPosition(FVector(-Radius * FMath::Cos(Theta), Radius * FMath::Sin(Theta), Rows * V->RowRise + 1000.f));
		OutLook = Xf.TransformPosition(FVector(-0.3f * V->FirstRowRadius, -200.f * Swing, V->StageHeight + 250.f));
		OutFov = 70.f;
		return;
	}

	case EArenaCamera::Follow:
	{
		// 6 m from the subject, between them and the stage, offset sideways toward the centre line (stays inside
		// the bowl). On stage the camera sits in front of the guest chair instead.
		const FVector HeadLocal = Xf.InverseTransformPosition(Head);
		const bool bOnStage = HeadLocal.X > -80.0 && HeadLocal.Z > V->StageHeight + 40.f;
		FVector Toward = bOnStage
			? Xf.InverseTransformVectorNoScale(V->GetGuestChairTransform().GetRotation().GetForwardVector())
			: FVector(-HeadLocal.X, -HeadLocal.Y, 0.0); // stage front centre is the venue origin
		Toward.Z = 0.0;
		Toward = Toward.IsNearlyZero() ? FVector(1.0, 0.0, 0.0) : Toward.GetSafeNormal();

		FVector Side(-Toward.Y, Toward.X, 0.0);
		if (Side.Y * HeadLocal.Y > 0.0)
		{
			Side = -Side;
		}
		const FVector CamLocal = HeadLocal + Toward * 520.0 + Side * 290.0 + FVector(0.0, 0.0, bOnStage ? 40.0 : 90.0);
		OutPos = Xf.TransformPosition(CamLocal);
		OutLook = Xf.TransformPosition(HeadLocal - FVector(0.0, 0.0, 20.0));
		OutFov = 40.f;
		return;
	}

	default: // Broadcast (Free never gets here)
	{
		// A camera platform in the middle of the bowl, looking down over the front rows at the stage, so the named
		// viewers (who fill from row A) and their name tags are in the lower part of the frame and the whole LED in
		// the upper part. 2.6 m above the floor keeps the rows right in front (< 2 m away) below the frame.
		// With the default venue: rows A-I in shot, LED top at ~0.9 of the half-height.
		const float Row = Rows * 0.45f;
		const float Radius = V->FirstRowRadius + Row * V->RowDepth;
		const float FloorZ = FMath::FloorToFloat(Row) * V->RowRise;
		OutPos = Xf.TransformPosition(FVector(-Radius, 0.f, FloorZ + 260.f));
		OutLook = Xf.TransformPosition(FVector(V->StageDepth * 0.5f, 0.f, V->StageHeight + 60.f));
		OutFov = 75.f;
		return;
	}
	}
}

float AArenaShowDirector::StreamerPovFov(const FVector& Eye, const FVector& Look) const
{
	using namespace ArenaShowDetail;

	// At least 75 degrees; wider (up to 100) when that is what it takes to get both ends of the front and back rows in.
	const AArenaVenue* V = Layout();
	const float HalfArc = FMath::DegreesToRadians(V->ArcDegrees) * 0.5f;
	const float LastRow = static_cast<float>(FMath::Max(V->NumRows - 1, 0));
	const FVector2D View = FVector2D(Look.X - Eye.X, Look.Y - Eye.Y).GetSafeNormal();

	float MaxDeg = 0.f;
	const float CornerRows[] = { 0.f, LastRow };
	const float CornerSides[] = { -1.f, 1.f };
	for (const float Row : CornerRows)
	{
		for (const float Side : CornerSides)
		{
			const FVector Corner = BowlPoint(Row, Side * HalfArc, SeatedHead);
			const FVector2D To = FVector2D(Corner.X - Eye.X, Corner.Y - Eye.Y).GetSafeNormal();
			const float Cos = FMath::Clamp(static_cast<float>(FVector2D::DotProduct(View, To)), -1.f, 1.f);
			MaxDeg = FMath::Max(MaxDeg, FMath::RadiansToDegrees(FMath::Acos(Cos)));
		}
	}
	return FMath::Clamp(2.f * MaxDeg + 6.f, 75.f, 100.f);
}

// ------------------------------------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------------------------------------

const AArenaVenue* AArenaShowDirector::Layout() const
{
	return IsValid(Venue) ? Venue.Get() : GetDefault<AArenaVenue>();
}

FVector AArenaShowDirector::BowlPoint(float Row, float Theta, float AboveFloor) const
{
	const AArenaVenue* V = Layout();
	const float Radius = V->FirstRowRadius + Row * V->RowDepth;
	const float FloorZ = FMath::FloorToFloat(FMath::Max(Row, 0.f)) * V->RowRise;
	return V->GetActorTransform().TransformPosition(FVector(-Radius * FMath::Cos(Theta), Radius * FMath::Sin(Theta), FloorZ + AboveFloor));
}

bool AArenaShowDirector::GetViewerHead(const FString& Id, FVector& Out) const
{
	return !Id.IsEmpty() && IsValid(Audience) && Audience->GetFigureHeadLocation(Id, Out);
}

bool AArenaShowDirector::IsViewerPresent(const FString& Id) const
{
	FArenaViewer Unused;
	return !Id.IsEmpty() && IsValid(Audience) && Audience->GetViewer(Id, Unused);
}
