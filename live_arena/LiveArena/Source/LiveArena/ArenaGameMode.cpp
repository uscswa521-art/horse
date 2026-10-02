#include "ArenaGameMode.h"

#include "ArenaAudience.h"
#include "ArenaNetSubsystem.h"
#include "ArenaPlayerController.h"
#include "ArenaScreen.h"
#include "ArenaShowDirector.h"
#include "ArenaVenue.h"
#include "LiveArena.h"
#include "LiveArenaSettings.h"

#include "Camera/CameraActor.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpectatorPawn.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

// 成個場館喺 StartPlay 砌：venue（level 有就用，冇就 spawn）→ 觀眾 → LED → 燈光鏡頭導演 → 連 server。
// Demo 模式：冇設定 ServerUrl 嘅時候自己加假觀眾、拍手、平台人數，等冇 server 都試到成個流程。
// 有設定 server 就只顯示真觀眾（假觀眾會畀抽獎抽中，server 唔識佢哋），C 拍手仍然可以用。

// Named (not anonymous) namespace: unity builds paste several .cpp files into one translation unit.
namespace ArenaGameModeDetail
{
	static const TCHAR* DemoNames[] = {
		TEXT("阿明"), TEXT("Kelly"), TEXT("小熊"), TEXT("Jason"), TEXT("阿珊"), TEXT("Ken仔"),
		TEXT("Mandy"), TEXT("阿Ben"), TEXT("肥仔"), TEXT("Coco"), TEXT("阿詩"), TEXT("Tommy"),
		TEXT("小薯"), TEXT("Vivian"), TEXT("阿傑"), TEXT("Sammi"), TEXT("豬豬"), TEXT("Peter"),
		TEXT("阿欣"), TEXT("Ivy"), TEXT("蛋撻"), TEXT("Kayan"), TEXT("阿樂"), TEXT("Winnie"),
		TEXT("Hugo"), TEXT("阿芬"), TEXT("Jacky"), TEXT("小龍"), TEXT("Candy"), TEXT("阿Lam"),
		TEXT("菠蘿包"), TEXT("Yoyo"), TEXT("雞蛋仔"), TEXT("Carmen"), TEXT("阿輝"), TEXT("Michelle"),
	};

	static constexpr float BannerInterval = 1.f;
	static constexpr float ToastSeconds = 3.f;
	static constexpr float DemoCrowdInterpSpeed = 0.08f; // ~60 s to get close to the target
	static constexpr float DemoClapsPerSecond = 25.f;    // C key: 30 claps spread over ~1.2 s

	/** -Key=value overrides InOut (an explicit empty value clears it). Quoted values may contain spaces. */
	static void OverrideString(const TCHAR* CommandLine, const TCHAR* Key, FString& InOut)
	{
		FString Value;
		// bShouldStopOnSeparator = false: URLs and names may legitimately contain ',' or ')'.
		if (FParse::Value(CommandLine, Key, Value, false))
		{
			InOut = Value.TrimStartAndEnd();
		}
	}

	static bool ParseScreenSource(const FString& In, EArenaScreenSource& Out)
	{
		const FString Source = In.TrimStartAndEnd().ToLower();
		if (Source == TEXT("auto"))
		{
			Out = EArenaScreenSource::Auto;
		}
		else if (Source == TEXT("capture") || Source == TEXT("device") || Source == TEXT("capturedevice"))
		{
			Out = EArenaScreenSource::CaptureDevice;
		}
		else if (Source == TEXT("url") || Source == TEXT("web"))
		{
			Out = EArenaScreenSource::Url;
		}
		else if (Source == TEXT("none") || Source == TEXT("off"))
		{
			Out = EArenaScreenSource::None;
		}
		else
		{
			return false;
		}
		return true;
	}

	static const TCHAR* ScreenSourceName(EArenaScreenSource Source)
	{
		switch (Source)
		{
		case EArenaScreenSource::CaptureDevice: return TEXT("capture");
		case EArenaScreenSource::Url:           return TEXT("url");
		case EArenaScreenSource::None:          return TEXT("none");
		default:                                return TEXT("auto");
		}
	}

	/** The server serves the audience page at <server>/r/<room>: ws(s)://host:port/ws -> http(s)://host:port/r/<room>. */
	static FString DeriveJoinUrl(const FString& ServerUrl, const FString& Room)
	{
		FString Url = ServerUrl.TrimStartAndEnd();
		if (Url.IsEmpty() || Room.IsEmpty())
		{
			return FString();
		}

		if (Url.StartsWith(TEXT("wss://"), ESearchCase::IgnoreCase))
		{
			Url = TEXT("https://") + Url.RightChop(6);
		}
		else if (Url.StartsWith(TEXT("ws://"), ESearchCase::IgnoreCase))
		{
			Url = TEXT("http://") + Url.RightChop(5);
		}
		else if (!Url.Contains(TEXT("://")))
		{
			Url = TEXT("http://") + Url;
		}

		int32 QueryStart = INDEX_NONE;
		if (Url.FindChar(TEXT('?'), QueryStart))
		{
			Url = Url.Left(QueryStart);
		}
		while (Url.EndsWith(TEXT("/")))
		{
			Url = Url.LeftChop(1);
		}
		if (Url.EndsWith(TEXT("/ws"), ESearchCase::IgnoreCase))
		{
			Url = Url.LeftChop(3);
		}
		return Url + TEXT("/r/") + Room;
	}

	/** Shorter link for the LED banner: browsers accept it without the scheme. */
	static FString ShortUrl(const FString& Url)
	{
		FString Out = Url.TrimStartAndEnd();
		if (Out.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase))
		{
			Out = Out.RightChop(8);
		}
		else if (Out.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase))
		{
			Out = Out.RightChop(7);
		}
		while (Out.EndsWith(TEXT("/")))
		{
			Out = Out.LeftChop(1);
		}
		return Out;
	}

	/** 1234 -> "1,234" (FText::AsNumber groups digits with the current culture). */
	static FString Grouped(int32 Value)
	{
		return FText::AsNumber(Value).ToString();
	}

	/** True for links only this PC can open (localhost, 127.x.x.x, ::1, 0.0.0.0): useless as a join link for viewers. */
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
			// [::1]:8787
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

// ---------------------------------------------------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------------------------------------------------

AArenaGameMode::AArenaGameMode()
{
	PlayerControllerClass = AArenaPlayerController::StaticClass();
	// Spawned and possessed as usual, but never the view target: the controller watches the director's camera.
	DefaultPawnClass = ASpectatorPawn::StaticClass();

	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
}

FArenaResolvedSettings AArenaGameMode::ResolveSettings()
{
	using namespace ArenaGameModeDetail;

	FArenaResolvedSettings Out;

	if (const ULiveArenaSettings* Ini = GetDefault<ULiveArenaSettings>())
	{
		Out.ServerUrl = Ini->ServerUrl;
		Out.RoomId = Ini->RoomId;
		Out.HostKey = Ini->HostKey;
		Out.JoinUrl = Ini->JoinUrl;
		Out.YouTubeVideoId = Ini->YouTubeVideoId;
		Out.ScreenSource = Ini->ScreenSource;
		Out.CaptureDeviceName = Ini->CaptureDeviceName;
		Out.StreamUrl = Ini->StreamUrl;
		Out.GuestCallUrl = Ini->GuestCallUrl;
		Out.StreamerName = Ini->StreamerName;
		Out.bDemoMode = Ini->bDemoMode;
		Out.MaxFigures = Ini->MaxFigures;
		Out.ExposureBias = Ini->ExposureBias;
	}

	// Command-line overrides (packaged builds), e.g. -ArenaServer=wss://example.com/ws -ArenaRoom=amy -ArenaDemo=0
	const TCHAR* CommandLine = FCommandLine::Get();
	OverrideString(CommandLine, TEXT("ArenaServer="), Out.ServerUrl);
	OverrideString(CommandLine, TEXT("ArenaRoom="), Out.RoomId);
	OverrideString(CommandLine, TEXT("ArenaKey="), Out.HostKey);
	OverrideString(CommandLine, TEXT("ArenaJoin="), Out.JoinUrl);
	OverrideString(CommandLine, TEXT("ArenaVideo="), Out.YouTubeVideoId);
	OverrideString(CommandLine, TEXT("ArenaDevice="), Out.CaptureDeviceName);
	OverrideString(CommandLine, TEXT("ArenaUrl="), Out.StreamUrl);
	OverrideString(CommandLine, TEXT("ArenaCall="), Out.GuestCallUrl);
	OverrideString(CommandLine, TEXT("ArenaName="), Out.StreamerName);

	FString SourceText;
	if (FParse::Value(CommandLine, TEXT("ArenaSource="), SourceText))
	{
		if (!ParseScreenSource(SourceText, Out.ScreenSource))
		{
			UE_LOG(LogLiveArena, Warning, TEXT("-ArenaSource=%s 唔識，用返 %s（可以用 auto|capture|url|none）"),
				*SourceText, ScreenSourceName(Out.ScreenSource));
		}
	}

	bool bDemo = Out.bDemoMode;
	if (FParse::Bool(CommandLine, TEXT("ArenaDemo="), bDemo))
	{
		Out.bDemoMode = bDemo;
	}

	int32 MaxFigures = Out.MaxFigures;
	if (FParse::Value(CommandLine, TEXT("ArenaMaxFigures="), MaxFigures))
	{
		Out.MaxFigures = MaxFigures;
	}

	// Tidy up.
	Out.ServerUrl.TrimStartAndEndInline();
	Out.RoomId.TrimStartAndEndInline();
	Out.JoinUrl.TrimStartAndEndInline();
	Out.YouTubeVideoId.TrimStartAndEndInline();
	Out.StreamUrl.TrimStartAndEndInline();
	Out.GuestCallUrl.TrimStartAndEndInline();
	Out.StreamerName.TrimStartAndEndInline();
	if (Out.StreamerName.IsEmpty())
	{
		Out.StreamerName = TEXT("直播主");
	}
	Out.MaxFigures = FMath::Clamp(Out.MaxFigures, 100, 10000);
	Out.ExposureBias = FMath::Clamp(Out.ExposureBias, -4.f, 4.f); // same range as AArenaShowDirector::AdjustExposure

	if (Out.JoinUrl.IsEmpty() && !Out.ServerUrl.IsEmpty())
	{
		Out.JoinUrl = DeriveJoinUrl(Out.ServerUrl, Out.RoomId);
	}
	if (IsLoopbackUrl(Out.JoinUrl))
	{
		UE_LOG(LogLiveArena, Warning,
			TEXT("入場 link %s 係 localhost，淨係呢部機開到：LED 唔會顯示佢。喺 Project Settings > Live Arena > JoinUrl 填 server 印出嚟嘅 LAN／公開網址。"),
			*Out.JoinUrl);
	}

	UE_LOG(LogLiveArena, Log,
		TEXT("Settings: server='%s' room='%s' key=%s join='%s' video='%s' source=%s device='%s' url='%s' call=%s name='%s' demo=%d maxFigures=%d exposure=%.2f"),
		*Out.ServerUrl, *Out.RoomId, Out.HostKey.IsEmpty() ? TEXT("(empty)") : TEXT("(set)"), *Out.JoinUrl, *Out.YouTubeVideoId,
		ScreenSourceName(Out.ScreenSource), *Out.CaptureDeviceName, *Out.StreamUrl,
		Out.GuestCallUrl.IsEmpty() ? TEXT("(empty)") : TEXT("(set)"), *Out.StreamerName,
		Out.bDemoMode ? 1 : 0, Out.MaxFigures, Out.ExposureBias);

	return Out;
}

void AArenaGameMode::StartPlay()
{
	Settings = ResolveSettings();

	// Super::StartPlay dispatches BeginPlay to every actor already in the world, including the player controller (which
	// builds its HUD there). The arena is spawned afterwards: new actors begin play as they spawn, and the HUD already
	// exists for the first toasts.
	Super::StartPlay();

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// A venue placed in the level keeps the meshes swapped in the editor; otherwise build the blockout at the origin.
	for (TActorIterator<AArenaVenue> It(World); It; ++It)
	{
		Venue = *It;
		break;
	}
	if (Venue)
	{
		UE_LOG(LogLiveArena, Log, TEXT("Using the ArenaVenue placed in the level (%s)."), *Venue->GetName());
	}
	else
	{
		Venue = World->SpawnActor<AArenaVenue>(AArenaVenue::StaticClass(), FTransform::Identity, SpawnParams);
	}
	if (!Venue)
	{
		UE_LOG(LogLiveArena, Error, TEXT("Could not find or spawn an ArenaVenue; the arena cannot start."));
		return;
	}

	// Same for the audience: one placed in the level keeps its FigureMesh / MaxLabels set in the editor.
	for (TActorIterator<AArenaAudience> It(World); It; ++It)
	{
		Audience = *It;
		break;
	}
	if (Audience)
	{
		UE_LOG(LogLiveArena, Log, TEXT("Using the ArenaAudience placed in the level (%s)."), *Audience->GetName());
	}
	else
	{
		Audience = World->SpawnActor<AArenaAudience>(AArenaAudience::StaticClass(), FTransform::Identity, SpawnParams);
	}
	Screen = World->SpawnActor<AArenaScreen>(AArenaScreen::StaticClass(), FTransform::Identity, SpawnParams);
	Director = World->SpawnActor<AArenaShowDirector>(AArenaShowDirector::StaticClass(), FTransform::Identity, SpawnParams);
	if (!Audience || !Screen || !Director)
	{
		UE_LOG(LogLiveArena, Error, TEXT("Spawning the arena failed (audience %d, screen %d, director %d)."),
			Audience ? 1 : 0, Screen ? 1 : 0, Director ? 1 : 0);
		return;
	}

	Audience->Init(Venue, Settings.MaxFigures);

	Screen->Init(Venue->GetScreenTransform(), Venue->GetScreenSize(), Venue->GetBannerTransform());
	// StreamUrl is the stream OBS sends out, i.e. this arena itself (it is also what the audience page plays). On the LED
	// it would show the arena inside the arena, 5-20 s late, so Auto never falls back to it: only an explicit Url does.
	const FString LedUrl = Settings.ScreenSource == EArenaScreenSource::Url ? Settings.StreamUrl : FString();
	ScreenSourceDescription = Screen->SetSource(Settings.ScreenSource, Settings.CaptureDeviceName, LedUrl, Settings.StreamerName);

	const bool bUseServer = !Settings.ServerUrl.IsEmpty();
	UArenaNetSubsystem* Net = bUseServer ? GetNet() : nullptr;
	if (bUseServer && !Net)
	{
		UE_LOG(LogLiveArena, Error, TEXT("ServerUrl is set but UArenaNetSubsystem is missing; running offline."));
	}

	Director->Init(Venue, Audience, Screen, Net, Settings.ExposureBias);
	Director->OnMessage.AddUObject(this, &AArenaGameMode::HandleShowMessage);

	if (Net)
	{
		Net->SetLayout(Venue->GetRowCounts());
		Net->SetConfig(Settings.StreamerName, Settings.YouTubeVideoId, Settings.StreamUrl, Settings.GuestCallUrl);
		BindNet();
		Net->Connect(Settings.ServerUrl, Settings.RoomId, Settings.HostKey);
	}

	DemoTarget = FMath::Min(DemoTarget, Venue->GetNumSlots());

	UE_LOG(LogLiveArena, Log, TEXT("Arena ready: %d seats in %d rows, LED: %s, server: %s, demo: %s."),
		Venue->GetNumSlots(), Venue->GetRowCounts().Num(), *ScreenSourceDescription,
		bUseServer ? *Settings.ServerUrl : TEXT("offline"), IsDemo() ? TEXT("on") : TEXT("off"));

	UpdateBanner();
	BannerTimer = 0.f;
	NotifyControllerReady();
}

void AArenaGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (bNetBound)
	{
		if (UArenaNetSubsystem* Net = GetNet())
		{
			Net->OnSnapshot.RemoveAll(this);
			Net->OnJoin.RemoveAll(this);
			Net->OnLeave.RemoveAll(this);
			Net->OnClap.RemoveAll(this);
			Net->OnYouTubeCount.RemoveAll(this);
			Net->OnConnectionChanged.RemoveAll(this);
			Net->OnError.RemoveAll(this);
		}
		bNetBound = false;
	}
	if (Director)
	{
		Director->OnMessage.RemoveAll(this);
	}

	Super::EndPlay(EndPlayReason);
}

void AArenaGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!Director || !Audience)
	{
		return;
	}

	if (!bControllerNotified)
	{
		NotifyControllerReady();
	}

	if (IsDemo())
	{
		TickDemo(DeltaSeconds);
	}
	TickPendingClaps(DeltaSeconds);

	BannerTimer += DeltaSeconds;
	if (BannerTimer >= ArenaGameModeDetail::BannerInterval)
	{
		BannerTimer = 0.f;
		UpdateBanner();
	}
}

UArenaNetSubsystem* AArenaGameMode::GetNet() const
{
	UWorld* World = GetWorld();
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	return GameInstance ? GameInstance->GetSubsystem<UArenaNetSubsystem>() : nullptr;
}

bool AArenaGameMode::IsServerConnected() const
{
	if (Settings.ServerUrl.IsEmpty())
	{
		return false;
	}
	const UArenaNetSubsystem* Net = GetNet();
	return Net && Net->IsConnected();
}

void AArenaGameMode::NotifyControllerReady()
{
	// Both the local player and the director's camera must exist; until then Tick keeps trying.
	if (bControllerNotified || !Director || !Director->GetCameraActor())
	{
		return;
	}

	UWorld* World = GetWorld();
	APlayerController* FirstController = World ? World->GetFirstPlayerController() : nullptr;
	if (!FirstController)
	{
		return;
	}

	if (AArenaPlayerController* ArenaController = Cast<AArenaPlayerController>(FirstController))
	{
		ArenaController->OnArenaReady();
	}
	else
	{
		UE_LOG(LogLiveArena, Warning, TEXT("First player controller is %s, not ArenaPlayerController: no HUD or keys."),
			*FirstController->GetClass()->GetName());
		FirstController->SetViewTarget(Director->GetCameraActor());
	}
	bControllerNotified = true;
}

void AArenaGameMode::Toast(const FString& Big, const FString& Small, float Seconds)
{
	UE_LOG(LogLiveArena, Log, TEXT("Toast: %s | %s"), *Big, *Small);

	UWorld* World = GetWorld();
	if (AArenaPlayerController* ArenaController = Cast<AArenaPlayerController>(World ? World->GetFirstPlayerController() : nullptr))
	{
		ArenaController->ShowToast(Big, Small, Seconds);
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// server events (UArenaNetSubsystem fires them on the game thread)
// ---------------------------------------------------------------------------------------------------------------------

void AArenaGameMode::BindNet()
{
	UArenaNetSubsystem* Net = GetNet();
	if (!Net || bNetBound)
	{
		return;
	}

	Net->OnSnapshot.AddUObject(this, &AArenaGameMode::HandleSnapshot);
	Net->OnJoin.AddUObject(this, &AArenaGameMode::HandleJoin);
	Net->OnLeave.AddUObject(this, &AArenaGameMode::HandleLeave);
	Net->OnClap.AddUObject(this, &AArenaGameMode::HandleClap);
	Net->OnYouTubeCount.AddUObject(this, &AArenaGameMode::HandleYouTubeCount);
	Net->OnConnectionChanged.AddUObject(this, &AArenaGameMode::HandleConnectionChanged);
	Net->OnError.AddUObject(this, &AArenaGameMode::HandleNetError);
	bNetBound = true;
}

void AArenaGameMode::HandleSnapshot(const TArray<FArenaViewer>& Viewers)
{
	// The server sends the snapshot only once it has accepted this app as the host (the socket itself is up earlier,
	// before the key is checked), so this is the real "connected" moment.
	Toast(bServerSeen ? TEXT("重新連線伺服器") : TEXT("已連線伺服器"),
		FString::Printf(TEXT("房間 %s · 已入場 %s 人"), *Settings.RoomId, *ArenaGameModeDetail::Grouped(Viewers.Num())));
	bServerSeen = true;

	if (!Audience)
	{
		return;
	}

	// The snapshot replaces everyone. If the lottery winner / guest is no longer in it (left while we were offline),
	// let the director cancel first so the figure can still be sent back to its seat.
	if (Director && Director->GetShowState() != EArenaShowState::Idle)
	{
		const FString WinnerId = Director->GetWinner().Id;
		const bool bStillHere = Viewers.ContainsByPredicate([&WinnerId](const FArenaViewer& Viewer)
		{
			return Viewer.Id == WinnerId;
		});
		if (!WinnerId.IsEmpty() && !bStillHere)
		{
			Director->NotifyViewerLeft(WinnerId);
		}
	}

	Audience->SetSnapshot(Viewers);
}

void AArenaGameMode::HandleJoin(const FArenaViewer& Viewer)
{
	if (!Audience)
	{
		return;
	}

	// A real viewer takes over a slot that another (stale) entry still holds here; if that entry is the current
	// winner / guest, cancel the show first, like a leave.
	if (Director && Director->GetShowState() != EArenaShowState::Idle)
	{
		const FString WinnerId = Director->GetWinner().Id;
		FArenaViewer Holder;
		if (!WinnerId.IsEmpty() && WinnerId != Viewer.Id && Audience->GetViewer(WinnerId, Holder) && Holder.Slot == Viewer.Slot)
		{
			Director->NotifyViewerLeft(WinnerId);
		}
	}

	Audience->AddViewer(Viewer);
}

void AArenaGameMode::HandleLeave(const FString& Id)
{
	// Director first: if the leaver is the winner / guest it still needs their figure to send it back.
	if (Director)
	{
		Director->NotifyViewerLeft(Id);
	}
	if (Audience)
	{
		Audience->RemoveViewer(Id);
	}
}

void AArenaGameMode::HandleClap(const FString& Id)
{
	if (Audience)
	{
		Audience->Clap(Id);
	}
}

void AArenaGameMode::HandleYouTubeCount(int32 Count)
{
	// -1 = unknown: the audience then draws the named viewers only.
	if (Audience)
	{
		Audience->SetCrowdCount(Count);
	}
}

void AArenaGameMode::HandleConnectionChanged(bool bConnected)
{
	// "Connected" here only means the socket is up; the toast waits for the snapshot (HandleSnapshot).
	if (bConnected)
	{
		bNetRefused = false;
	}
	else if (!bNetRefused)
	{
		Toast(TEXT("伺服器斷咗，重連中"), TEXT("觀眾嘅座位會保留，返嚟會自動補返"));
	}
}

void AArenaGameMode::HandleNetError(const FString& Message)
{
	// For these two the subsystem stops reconnecting; the disconnect that follows must not say "reconnecting".
	if (Message.Equals(TEXT("bad key"), ESearchCase::IgnoreCase))
	{
		bNetRefused = true;
		if (Settings.HostKey.IsEmpty() || Settings.HostKey.Len() > 128)
		{
			Toast(TEXT("伺服器拒絕：HostKey 係空嘅（或者太長）"),
				TEXT("喺 Project Settings > Live Arena > HostKey 填一條 1-128 字嘅 key 再開過"), 8.f);
		}
		else
		{
			Toast(TEXT("伺服器拒絕：HostKey 唔啱"),
				FString::Printf(TEXT("房間「%s」已經有另一條 key · 改 RoomId 或者 HostKey 再開過"), *Settings.RoomId), 8.f);
		}
		return;
	}
	if (Message.Equals(TEXT("replaced"), ESearchCase::IgnoreCase))
	{
		bNetRefused = true;
		Toast(TEXT("另一部機接手咗做 host"),
			TEXT("同一個房同一時間只可以有一個 Live Arena · 呢部已經停止連線，要接返就重開"), 8.f);
		return;
	}
	Toast(TEXT("伺服器錯誤"), Message);
}

void AArenaGameMode::HandleShowMessage(const FString& Big, const FString& Small)
{
	Toast(Big, Small, ArenaGameModeDetail::ToastSeconds);
}

// ---------------------------------------------------------------------------------------------------------------------
// LED banner
// ---------------------------------------------------------------------------------------------------------------------

void AArenaGameMode::UpdateBanner()
{
	using namespace ArenaGameModeDetail;

	if (!Screen)
	{
		return;
	}

	// The capture device may fail (and fall back) asynchronously after SetSource: keep the HUD text current.
	const FString ActiveSource = Screen->GetActiveSource();
	if (!ActiveSource.IsEmpty())
	{
		ScreenSourceDescription = ActiveSource;
	}

	const int32 Named = Audience ? Audience->GetNamedCount() : 0;
	const int32 Watching = Audience ? Audience->GetTotalWatching() : -1;
	const int32 Shown = Watching >= 0 ? FMath::Max(Watching, Named) : Named;
	const FString Left = FString::Printf(TEXT("現場 %s 人"), *Grouped(Shown));

	FString Status = TEXT("直播中");
	if (Director)
	{
		switch (Director->GetShowState())
		{
		case EArenaShowState::Spinning:
		case EArenaShowState::Landed:
			Status = TEXT("抽獎中");
			break;
		case EArenaShowState::Walking:
		case EArenaShowState::Guest:
			Status = FString::Printf(TEXT("嘉賓：%s"), *Director->GetWinner().Name);
			break;
		default:
			Status = Director->GetMode() == EArenaMode::Interactive ? TEXT("互動時間") : TEXT("直播中");
			break;
		}
	}
	const FString Centre = Settings.StreamerName + TEXT(" · ") + Status;

	// The LED is part of the broadcast: never advertise a localhost link nobody else can open (the HUD warns instead).
	FString Right;
	if (!Settings.JoinUrl.IsEmpty() && !IsLoopbackUrl(Settings.JoinUrl))
	{
		Right = TEXT("入場 ") + ShortUrl(Settings.JoinUrl);
	}
	else if (IsDemo() && Settings.ServerUrl.IsEmpty())
	{
		Right = TEXT("示範模式");
	}

	Screen->SetBanner(Left, Centre, Right);
}

// ---------------------------------------------------------------------------------------------------------------------
// demo
// ---------------------------------------------------------------------------------------------------------------------

void AArenaGameMode::TickDemo(float DeltaSeconds)
{
	// With a server configured only real people are shown, even while it cannot be reached (wrong key, server down):
	// the streamer must never broadcast an invented crowd next to a real join link. C still claps by hand.
	if (!Settings.ServerUrl.IsEmpty() || !Audience || !Venue)
	{
		return;
	}

	// People trickling in.
	DemoJoinTimer -= DeltaSeconds;
	if (DemoJoinTimer <= 0.f)
	{
		DemoJoinTimer = FMath::FRandRange(0.3f, 1.5f);
		if (DemoCounter < DemoTarget)
		{
			TSet<int32> Occupied = CollectOccupiedSlots();
			AddDemoViewer(Occupied);
		}
	}

	// A few claps all the time.
	DemoClapTimer -= DeltaSeconds;
	if (DemoClapTimer <= 0.f)
	{
		DemoClapTimer = FMath::FRandRange(0.2f, 0.8f);
		Audience->ClapRandom(FMath::RandRange(1, 4));
	}

	// Platform viewer count: eases towards a target that wanders every 12-25 s.
	DemoDriftTimer -= DeltaSeconds;
	if (DemoDriftTimer <= 0.f)
	{
		DemoDriftTimer = FMath::FRandRange(12.f, 25.f);
		DemoCrowdTarget = FMath::Clamp(DemoCrowdTarget + FMath::FRandRange(-150.f, 260.f), 1200.f, 4200.f);
	}
	DemoCrowd = FMath::FInterpTo(DemoCrowd, DemoCrowdTarget, DeltaSeconds, ArenaGameModeDetail::DemoCrowdInterpSpeed);

	DemoCrowdTimer += DeltaSeconds;
	if (DemoCrowdTimer >= 1.f)
	{
		DemoCrowdTimer = 0.f;
		const float Noise = FMath::FRandRange(-6.f, 6.f) + DemoCrowd * FMath::FRandRange(-0.004f, 0.004f);
		const int32 Count = FMath::Max(Audience->GetNamedCount(), FMath::RoundToInt(DemoCrowd + Noise));
		Audience->SetCrowdCount(Count);
	}
}

void AArenaGameMode::TickPendingClaps(float DeltaSeconds)
{
	if (DemoPendingClaps <= 0 || !Audience)
	{
		DemoClapAccum = 0.f;
		return;
	}

	DemoClapAccum += DeltaSeconds * ArenaGameModeDetail::DemoClapsPerSecond;
	const int32 Now = FMath::Min(DemoPendingClaps, FMath::FloorToInt(DemoClapAccum));
	if (Now > 0)
	{
		Audience->ClapRandom(Now);
		DemoPendingClaps -= Now;
		DemoClapAccum -= static_cast<float>(Now);
	}
}

void AArenaGameMode::DemoAddViewers(int32 Count)
{
	if (!Audience || !Venue || Count <= 0)
	{
		return;
	}
	if (!Settings.ServerUrl.IsEmpty())
	{
		// The server does not know fake ids: a lottery landing on one gets "unknown viewer" and nobody receives it.
		Toast(TEXT("有設定伺服器"), TEXT("示範觀眾只可以喺冇設定 ServerUrl 嘅時候加 · 想試就自己開入場 link"));
		return;
	}

	TSet<int32> Occupied = CollectOccupiedSlots();
	int32 Added = 0;
	while (Added < Count && AddDemoViewer(Occupied))
	{
		++Added;
	}
	if (Added < Count)
	{
		Toast(TEXT("座位滿晒"), FString::Printf(TEXT("呢個場館得 %s 個座位"), *ArenaGameModeDetail::Grouped(Venue->GetNumSlots())));
	}
}

void AArenaGameMode::DemoClaps(int32 Count)
{
	if (Count > 0)
	{
		DemoPendingClaps += Count;
	}
}

TSet<int32> AArenaGameMode::CollectOccupiedSlots() const
{
	TSet<int32> Occupied;
	if (!Audience)
	{
		return Occupied;
	}

	const TArray<FString> Ids = Audience->GetNamedIds();
	Occupied.Reserve(Ids.Num());
	for (const FString& Id : Ids)
	{
		FArenaViewer Viewer;
		if (Audience->GetViewer(Id, Viewer) && Viewer.Slot != INDEX_NONE)
		{
			Occupied.Add(Viewer.Slot);
		}
	}
	return Occupied;
}

bool AArenaGameMode::AddDemoViewer(TSet<int32>& Occupied)
{
	if (!Audience || !Venue)
	{
		return false;
	}

	// Lowest free slot, like the server: slot 0 = row A centre, then centre-outward, row by row.
	const int32 NumSlots = Venue->GetNumSlots();
	int32 Slot = 0;
	while (Slot < NumSlots && Occupied.Contains(Slot))
	{
		++Slot;
	}
	if (Slot >= NumSlots)
	{
		return false;
	}

	FArenaViewer Viewer;
	Viewer.Id = FString::Printf(TEXT("demo-%d"), ++DemoCounter);
	Viewer.Name = NextDemoName();
	Viewer.Slot = Slot;
	Viewer.Label = Venue->GetSlotLabel(Slot);

	Occupied.Add(Slot);
	DemoNextSlot = Slot + 1;
	Audience->AddViewer(Viewer);
	return true;
}

FString AArenaGameMode::NextDemoName()
{
	using namespace ArenaGameModeDetail;

	const int32 NumNames = static_cast<int32>(UE_ARRAY_COUNT(DemoNames));
	const FString Base = DemoNames[FMath::RandRange(0, NumNames - 1)];
	int32& Uses = DemoNameUses.FindOrAdd(Base);
	++Uses;
	return Uses == 1 ? Base : FString::Printf(TEXT("%s%d"), *Base, Uses);
}
