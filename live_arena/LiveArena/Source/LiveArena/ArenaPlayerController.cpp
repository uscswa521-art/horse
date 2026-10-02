#include "ArenaPlayerController.h"

#include "ArenaAudience.h"
#include "ArenaGameMode.h"
#include "ArenaShowDirector.h"
#include "ArenaWidgets.h"
#include "LiveArena.h"

#include "Camera/CameraActor.h"
#include "Engine/World.h"
#include "InputCoreTypes.h"

// 直播主嘅控制：冇 input asset，PlayerTick 入面直接 poll 按鍵（WasInputKeyJustPressed / IsInputKeyDown）。
// HUD 左上角狀態每 0.25 秒更新一次；Tab 收埋成個 HUD 等 OBS 擷取到乾淨畫面。

// Named (not anonymous) namespace: unity builds paste several .cpp files into one translation unit.
namespace ArenaPlayerControllerDetail
{
	static constexpr float StatusInterval = 0.25f;
	static constexpr float ToastSeconds = 3.f;
	static constexpr float ExposureStep = 0.25f;
	static constexpr int32 DemoViewersPerPress = 20;
	static constexpr int32 DemoClapsPerPress = 30;

	static constexpr float FreeFlySpeed = 600.f;   // cm/s
	static constexpr float FreeFlyBoost = 3.f;     // with Shift
	/** Mouse axis values already include the input settings' 0.07 sensitivity; 2.5 matches the engine's classic look scale. */
	static constexpr float FreeLookScale = 2.5f;
	static constexpr float FreeLookPitchLimit = 85.f;

	static const TCHAR* CameraName(EArenaCamera Camera)
	{
		switch (Camera)
		{
		case EArenaCamera::Broadcast:   return TEXT("直播（觀眾席望台）");
		case EArenaCamera::StreamerPOV: return TEXT("直播主視角");
		case EArenaCamera::Wide:        return TEXT("全景");
		case EArenaCamera::Follow:      return TEXT("跟隨");
		case EArenaCamera::Free:        return TEXT("自由飛行");
		default:                        return TEXT("?");
		}
	}

	static FString Grouped(int32 Value)
	{
		return FText::AsNumber(Value).ToString();
	}

	static FString SeatSuffix(const FArenaViewer& Viewer)
	{
		return Viewer.Label.IsEmpty() ? FString() : FString::Printf(TEXT("（%s）"), *Viewer.Label);
	}

	/** localhost / 127.x / ::1 / 0.0.0.0 link: only this PC can open it (same rule as the LED banner in ArenaGameMode.cpp). */
	static bool IsLoopbackUrl(const FString& Url)
	{
		FString Host = Url.TrimStartAndEnd();
		const int32 SchemeEnd = Host.Find(TEXT("://"), ESearchCase::CaseSensitive);
		if (SchemeEnd != INDEX_NONE)
		{
			Host.RightChopInline(SchemeEnd + 3);
		}
		int32 Cut = INDEX_NONE;
		if (Host.FindChar(TEXT('/'), Cut))
		{
			Host.LeftInline(Cut);
		}
		if (Host.StartsWith(TEXT("[")))
		{
			int32 Close = INDEX_NONE;
			Host = Host.FindChar(TEXT(']'), Close) ? Host.Mid(1, Close - 1) : Host.Mid(1);
		}
		else if (Host.FindChar(TEXT(':'), Cut))
		{
			Host.LeftInline(Cut);
		}
		Host.ToLowerInline();
		return Host == TEXT("localhost") || Host.EndsWith(TEXT(".localhost")) || Host.StartsWith(TEXT("127."))
			|| Host == TEXT("::1") || Host == TEXT("0.0.0.0");
	}
}

AArenaPlayerController::AArenaPlayerController()
{
	// The view target is the director's camera actor, set in OnArenaReady; possessing the spectator pawn must not steal it.
	bAutoManageActiveCameraTarget = false;
	bShowMouseCursor = false;
}

void AArenaPlayerController::BeginPlay()
{
	Super::BeginPlay();

	if (!IsLocalController())
	{
		return;
	}

	if (!Hud)
	{
		Hud = CreateWidget<UArenaHudWidget>(this, UArenaHudWidget::StaticClass());
		if (Hud)
		{
			Hud->AddToViewport(10);
			Hud->SetVisibility(ESlateVisibility::HitTestInvisible);
		}
		else
		{
			UE_LOG(LogLiveArena, Error, TEXT("Could not create the HUD widget."));
		}
	}

	SetInputMode(FInputModeGameOnly());

	// Normally the game mode calls OnArenaReady after spawning the director; covers a controller that arrives later.
	if (GetDirector())
	{
		OnArenaReady();
	}
}

void AArenaPlayerController::OnArenaReady()
{
	AArenaShowDirector* Director = GetDirector();
	ACameraActor* Camera = Director ? Director->GetCameraActor() : nullptr;
	if (!Camera)
	{
		UE_LOG(LogLiveArena, Warning, TEXT("OnArenaReady: the director has no camera actor yet."));
		return;
	}

	SetViewTarget(Camera);
	bStatusDirty = true;

	if (!bGreeted)
	{
		bGreeted = true;
		ShowToast(TEXT("Live Arena"), TEXT("H 睇按鍵 · Tab 收埋 HUD（OBS 乾淨畫面）"), 4.f);
	}
}

void AArenaPlayerController::ShowToast(const FString& Big, const FString& Small, float Seconds)
{
	if (Hud)
	{
		Hud->ShowToast(Big, Small, Seconds);
	}
}

void AArenaPlayerController::PlayerTick(float DeltaTime)
{
	// Super processes this frame's input first, so the key queries below see this frame's presses.
	Super::PlayerTick(DeltaTime);

	if (!IsLocalController())
	{
		return;
	}

	// The director may leave Free on its own (or never have entered it): follow it.
	if (bFreeFly)
	{
		const AArenaShowDirector* Director = GetDirector();
		if (!Director || Director->GetCamera() != EArenaCamera::Free)
		{
			bFreeFly = false;
			bStatusDirty = true;
		}
	}

	HandleKeys();

	if (bFreeFly)
	{
		UpdateFreeFly(DeltaTime);
	}

	RefreshStatus(DeltaTime);
}

// ---------------------------------------------------------------------------------------------------------------------
// keys
// ---------------------------------------------------------------------------------------------------------------------

void AArenaPlayerController::HandleKeys()
{
	using namespace ArenaPlayerControllerDetail;

	AArenaGameMode* Mode = GetArenaMode();
	AArenaShowDirector* Director = GetDirector();

	auto Pressed = [this](const FKey& Key)
	{
		return WasInputKeyJustPressed(Key);
	};

	bool bAnyKey = false;

	if (Director)
	{
		if (Pressed(EKeys::SpaceBar))
		{
			Director->ToggleMode();
			bAnyKey = true;
		}
		if (Pressed(EKeys::L))
		{
			Director->StartLottery(); // posts its own message when nobody has entered
			bAnyKey = true;
		}
		if (Pressed(EKeys::Enter))
		{
			Director->InviteWinner();
			bAnyKey = true;
		}
		if (Pressed(EKeys::K))
		{
			Director->EndGuest();
			bAnyKey = true;
		}

		if (Pressed(EKeys::One) || Pressed(EKeys::NumPadOne))
		{
			SelectCamera(EArenaCamera::Broadcast);
			bAnyKey = true;
		}
		if (Pressed(EKeys::Two) || Pressed(EKeys::NumPadTwo))
		{
			SelectCamera(EArenaCamera::StreamerPOV);
			bAnyKey = true;
		}
		if (Pressed(EKeys::Three) || Pressed(EKeys::NumPadThree))
		{
			SelectCamera(EArenaCamera::Wide);
			bAnyKey = true;
		}
		if (Pressed(EKeys::Four) || Pressed(EKeys::NumPadFour))
		{
			SelectCamera(EArenaCamera::Follow);
			bAnyKey = true;
		}
		if (Pressed(EKeys::F))
		{
			SetFreeFly(!bFreeFly);
			bAnyKey = true;
		}

		// [ ] (with - = as a fallback for keyboard layouts without brackets)
		if (Pressed(EKeys::LeftBracket) || Pressed(EKeys::Hyphen))
		{
			Director->AdjustExposure(-ExposureStep);
			bAnyKey = true;
		}
		if (Pressed(EKeys::RightBracket) || Pressed(EKeys::Equals))
		{
			Director->AdjustExposure(ExposureStep);
			bAnyKey = true;
		}
	}

	if (Hud)
	{
		if (Pressed(EKeys::H))
		{
			Hud->SetHelpVisible(!Hud->IsHelpVisible());
		}
		if (Pressed(EKeys::Tab))
		{
			bHudHidden = !bHudHidden;
			Hud->SetVisibility(bHudHidden ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
		}
	}

	if (Mode)
	{
		// D is also "strafe right" while flying, so it only adds viewers outside free fly.
		if (!bFreeFly && Pressed(EKeys::D))
		{
			Mode->DemoAddViewers(DemoViewersPerPress);
			bAnyKey = true;
		}
		if (Pressed(EKeys::C))
		{
			Mode->DemoClaps(DemoClapsPerPress);
		}
	}

	if (bAnyKey)
	{
		bStatusDirty = true;
	}
}

void AArenaPlayerController::SelectCamera(EArenaCamera Preset)
{
	AArenaShowDirector* Director = GetDirector();
	if (!Director)
	{
		return;
	}
	bFreeFly = false;
	Director->SetCamera(Preset);
}

void AArenaPlayerController::SetFreeFly(bool bOn)
{
	using namespace ArenaPlayerControllerDetail;

	AArenaShowDirector* Director = GetDirector();
	if (!Director)
	{
		bFreeFly = false;
		return;
	}
	if (bOn == bFreeFly)
	{
		return;
	}

	if (bOn)
	{
		const EArenaCamera Current = Director->GetCamera();
		FreeFlyReturnCamera = (Current == EArenaCamera::Free) ? EArenaCamera::Broadcast : Current;

		// Start flying from wherever the director's camera is right now.
		if (const ACameraActor* Camera = Director->GetCameraActor())
		{
			const FRotator Rotation = Camera->GetActorRotation();
			FreeFlyYaw = static_cast<float>(Rotation.Yaw);
			FreeFlyPitch = FMath::Clamp(static_cast<float>(Rotation.Pitch), -FreeLookPitchLimit, FreeLookPitchLimit);
		}

		Director->SetCamera(EArenaCamera::Free); // the director stops driving the camera
		bFreeFly = true;
		ShowToast(TEXT("自由飛行"), TEXT("WASD 移動 · Q / E 落 / 升 · Shift 加速 · 滑鼠轉向 · 再撳 F 返回"), ToastSeconds);
	}
	else
	{
		bFreeFly = false;
		Director->SetCamera(FreeFlyReturnCamera);
	}
}

void AArenaPlayerController::UpdateFreeFly(float DeltaTime)
{
	using namespace ArenaPlayerControllerDetail;

	AArenaShowDirector* Director = GetDirector();
	ACameraActor* Camera = Director ? Director->GetCameraActor() : nullptr;
	if (!Camera)
	{
		return;
	}

	// Look.
	float MouseX = 0.f;
	float MouseY = 0.f;
	GetInputMouseDelta(MouseX, MouseY);
	FreeFlyYaw = FMath::Fmod(FreeFlyYaw + MouseX * FreeLookScale, 360.f);
	FreeFlyPitch = FMath::Clamp(FreeFlyPitch + MouseY * FreeLookScale, -FreeLookPitchLimit, FreeLookPitchLimit);
	const FRotator Rotation(FreeFlyPitch, FreeFlyYaw, 0.f);

	// Move: W/S along the view, A/D sideways, Q/E straight down / up.
	auto Axis = [this](const FKey& Positive, const FKey& Negative)
	{
		return (IsInputKeyDown(Positive) ? 1.f : 0.f) - (IsInputKeyDown(Negative) ? 1.f : 0.f);
	};
	const float Forward = Axis(EKeys::W, EKeys::S);
	const float Right = Axis(EKeys::D, EKeys::A);
	const float Up = Axis(EKeys::E, EKeys::Q);

	const FQuat Orientation = Rotation.Quaternion();
	FVector Move = Orientation.GetForwardVector() * Forward + Orientation.GetRightVector() * Right + FVector::UpVector * Up;
	Move = Move.GetSafeNormal();

	const bool bBoost = IsInputKeyDown(EKeys::LeftShift) || IsInputKeyDown(EKeys::RightShift);
	const float Speed = FreeFlySpeed * (bBoost ? FreeFlyBoost : 1.f);

	Camera->SetActorLocationAndRotation(Camera->GetActorLocation() + Move * (Speed * DeltaTime), Rotation);
}

// ---------------------------------------------------------------------------------------------------------------------
// HUD status block
// ---------------------------------------------------------------------------------------------------------------------

void AArenaPlayerController::RefreshStatus(float DeltaTime)
{
	using namespace ArenaPlayerControllerDetail;

	StatusTimer += DeltaTime;
	if (!bStatusDirty && StatusTimer < StatusInterval)
	{
		return;
	}
	StatusTimer = 0.f;
	bStatusDirty = false;

	if (!Hud)
	{
		return;
	}

	const AArenaGameMode* Mode = GetArenaMode();
	if (!Mode)
	{
		Hud->SetStatus(TEXT("Live Arena · 搵唔到 ArenaGameMode（World Settings > GameMode Override）"));
		return;
	}

	const FArenaResolvedSettings& Settings = Mode->GetSettings();
	const AArenaShowDirector* Director = Mode->GetDirector();
	const AArenaAudience* Audience = Mode->GetAudience();

	TArray<FString> Lines;

	// Server
	if (Settings.ServerUrl.IsEmpty())
	{
		Lines.Add(TEXT("伺服器：離線（未設定 ServerUrl）"));
	}
	else if (Mode->IsNetRefused())
	{
		Lines.Add(FString::Printf(TEXT("伺服器：被拒絕（HostKey 唔啱，或者另一部機接手咗房間 %s）· 唔會再重連"), *Settings.RoomId));
	}
	else if (Mode->IsServerConnected())
	{
		Lines.Add(FString::Printf(TEXT("伺服器：已連線 · 房間 %s"), *Settings.RoomId));
	}
	else
	{
		Lines.Add(FString::Printf(TEXT("伺服器：連線中… · 房間 %s"), *Settings.RoomId));
	}

	if (Settings.JoinUrl.IsEmpty())
	{
		Lines.Add(TEXT("入場 link：未設定（Project Settings > Live Arena > JoinUrl）"));
	}
	else if (IsLoopbackUrl(Settings.JoinUrl))
	{
		Lines.Add(FString::Printf(TEXT("入場 link：%s"), *Settings.JoinUrl));
		Lines.Add(TEXT("　（localhost：外面嘅觀眾開唔到，LED 唔顯示 · JoinUrl 填 server 印出嚟嘅 LAN／公開網址）"));
	}
	else
	{
		Lines.Add(FString::Printf(TEXT("入場 link：%s"), *Settings.JoinUrl));
	}

	if (Mode->IsDemo())
	{
		// The game mode only invents viewers without a server (fake ids would break the lottery on a real one).
		Lines.Add(Settings.ServerUrl.IsEmpty()
			? FString(TEXT("示範模式：開（D 加觀眾 · C 拍手）"))
			: FString(TEXT("示範模式：停（有設定伺服器，只顯示真觀眾 · C 仍然可以拍手）")));
	}

	if (Director)
	{
		Lines.Add(Director->GetMode() == EArenaMode::Interactive
			? FString(TEXT("模式：互動時間"))
			: FString(TEXT("模式：觀看（Space 開互動）")));

		const FArenaViewer& Winner = Director->GetWinner();
		switch (Director->GetShowState())
		{
		case EArenaShowState::Spinning:
			Lines.Add(TEXT("抽獎：抽緊…"));
			break;
		case EArenaShowState::Landed:
			Lines.Add(FString::Printf(TEXT("抽獎：抽中 %s%s · Enter 請上台"), *Winner.Name, *SeatSuffix(Winner)));
			break;
		case EArenaShowState::Walking:
			Lines.Add(FString::Printf(TEXT("嘉賓：%s%s 行緊上台"), *Winner.Name, *SeatSuffix(Winner)));
			break;
		case EArenaShowState::Guest:
			Lines.Add(FString::Printf(TEXT("嘉賓：%s%s 喺台上 · K 落台"), *Winner.Name, *SeatSuffix(Winner)));
			break;
		default:
			Lines.Add(TEXT("抽獎：未開始（L 開始）"));
			break;
		}
	}
	else
	{
		Lines.Add(TEXT("場館準備中…"));
	}

	if (Audience)
	{
		const int32 Watching = Audience->GetTotalWatching();
		Lines.Add(FString::Printf(TEXT("已入場：%s"), *Grouped(Audience->GetNamedCount())));
		Lines.Add(Watching >= 0
			? FString::Printf(TEXT("平台觀看：%s"), *Grouped(Watching))
			: FString(TEXT("平台觀看：未知")));
		Lines.Add(FString::Printf(TEXT("畫面上人數：%s"), *Grouped(Audience->GetDisplayedFigures())));
	}

	const FString Source = Mode->GetScreenSourceDescription();
	Lines.Add(FString::Printf(TEXT("LED 來源：%s"), Source.IsEmpty() ? TEXT("-") : *Source));

	if (Director)
	{
		Lines.Add(FString::Printf(TEXT("鏡頭：%s"), CameraName(Director->GetCamera())));
		const float Exposure = Director->GetExposureBias();
		Lines.Add(FString::Printf(TEXT("曝光：%s%.2f EV"), Exposure >= 0.f ? TEXT("+") : TEXT(""), Exposure));
	}

	Hud->SetStatus(FString::Join(Lines, TEXT("\n")));
}

// ---------------------------------------------------------------------------------------------------------------------
// lookups
// ---------------------------------------------------------------------------------------------------------------------

AArenaGameMode* AArenaPlayerController::GetArenaMode() const
{
	const UWorld* World = GetWorld();
	return World ? Cast<AArenaGameMode>(World->GetAuthGameMode()) : nullptr;
}

AArenaShowDirector* AArenaPlayerController::GetDirector() const
{
	const AArenaGameMode* Mode = GetArenaMode();
	return Mode ? Mode->GetDirector() : nullptr;
}
