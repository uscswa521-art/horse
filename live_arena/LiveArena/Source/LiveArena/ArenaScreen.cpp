#include "ArenaScreen.h"

#include "ArenaWidgets.h"
#include "LiveArena.h"

#include "Components/RectLightComponent.h"
#include "Components/WidgetComponent.h"
#include "MediaBlueprintFunctionLibrary.h"
#include "MediaPlayer.h"
#include "MediaTexture.h"
#include "TimerManager.h"

// LED 牆：world-space UWidgetComponent（UArenaScreenWidget，1920x1080 再縮放成實際厘米）+ 下面一條 banner，
// 前面一盞 rect light 模擬 LED 散落舞台同前排嘅光。來源優先次序見 SetSource。

// Named (not anonymous) namespace: unity builds paste several .cpp files into one translation unit.
namespace ArenaScreenDetail
{
	static const FVector2D ScreenPixels(1920.f, 1080.f);
	static const FVector2D BannerPixels(1920.f, 120.f);
	static const TCHAR* DefaultCaptureDevice = TEXT("OBS Virtual Camera");
	static const TCHAR* WaitingHint = TEXT("等待直播畫面 · 喺 OBS 撳「Start Virtual Camera」再重新開 Live Arena（Project Settings > Live Arena > CaptureDeviceName）");

	static bool IsYouTubeIdChar(TCHAR C)
	{
		return FChar::IsAlnum(C) || C == TEXT('-') || C == TEXT('_');
	}

	/** youtube.com/watch?v=ID, youtu.be/ID, youtube.com/live/ID, youtube.com/shorts/ID -> ID; "" for anything else. */
	static FString ExtractYouTubeId(const FString& Url)
	{
		const FString Lower = Url.ToLower(); // same length as Url, so indices carry over (ids are case-sensitive)
		if (!Lower.Contains(TEXT("youtube.com")) && !Lower.Contains(TEXT("youtu.be")))
		{
			return FString();
		}

		static const TCHAR* Markers[] = { TEXT("?v="), TEXT("&v="), TEXT("youtu.be/"), TEXT("/live/"), TEXT("/shorts/") };
		for (const TCHAR* Marker : Markers)
		{
			const int32 At = Lower.Find(Marker, ESearchCase::CaseSensitive);
			if (At == INDEX_NONE)
			{
				continue;
			}
			FString Id;
			for (int32 Index = At + FCString::Strlen(Marker); Index < Url.Len() && IsYouTubeIdChar(Url[Index]); ++Index)
			{
				Id.AppendChar(Url[Index]);
			}
			if (Id.Len() >= 6)
			{
				return Id;
			}
		}
		return FString();
	}

	/**
	 * Trims, adds https:// when the scheme is missing and turns YouTube watch links into autoplaying, muted embeds.
	 * Muted: the LED page plays the streamer's own stream, and OBS desktop audio would feed it back into the stream
	 * as a 5-20 s echo (other pages are muted by script, see UArenaScreenWidget::NativeTick).
	 */
	static FString ToLedPageUrl(const FString& In)
	{
		FString Url = In.TrimStartAndEnd();
		if (Url.IsEmpty())
		{
			return Url;
		}
		if (!Url.Contains(TEXT("://")) && !Url.StartsWith(TEXT("about:")))
		{
			Url = TEXT("https://") + Url;
		}
		if (!Url.Contains(TEXT("/embed/")))
		{
			const FString Id = ExtractYouTubeId(Url);
			if (!Id.IsEmpty())
			{
				return FString::Printf(TEXT("https://www.youtube.com/embed/%s?autoplay=1&mute=1&playsinline=1"), *Id);
			}
		}
		return Url;
	}

	/** First video capture device whose display name contains Wanted (case-insensitive). */
	static bool FindCaptureDevice(const FString& Wanted, FMediaCaptureDevice& OutDevice)
	{
		TArray<FMediaCaptureDevice> Devices;
		UMediaBlueprintFunctionLibrary::EnumerateVideoCaptureDevices(Devices, -1);

		bool bFound = false;
		for (const FMediaCaptureDevice& Device : Devices)
		{
			const FString Name = Device.DisplayName.ToString();
			const bool bMatch = !bFound && Name.Contains(Wanted, ESearchCase::IgnoreCase);
			UE_LOG(LogLiveArena, Log, TEXT("擷取裝置：%s%s"), *Name, bMatch ? TEXT("  <- 用呢個") : TEXT(""));
			if (bMatch)
			{
				OutDevice = Device;
				bFound = true;
			}
		}
		if (!bFound)
		{
			UE_LOG(LogLiveArena, Log, TEXT("搵唔到名包含「%s」嘅擷取裝置（共 %d 個裝置）"), *Wanted, Devices.Num());
			UE_LOG(LogLiveArena, Log, TEXT("如果 OBS Virtual Camera 已經開咗都唔喺上面：UE 用 Media Foundation 列裝置，淨係 DirectShow 嘅虛擬鏡頭可能唔會出現。"));
		}
		return bFound;
	}

	static void SetupWidgetComponent(UWidgetComponent* Comp, TSubclassOf<UUserWidget> WidgetClass, const FVector2D& Pixels)
	{
		Comp->SetWidgetSpace(EWidgetSpace::World);
		Comp->SetWidgetClass(WidgetClass);
		Comp->SetDrawSize(Pixels);
		Comp->SetPivot(FVector2D(0.5f, 0.5f));
		Comp->SetBlendMode(EWidgetBlendMode::Opaque);
		Comp->SetTwoSided(false);
		Comp->SetBackgroundColor(FLinearColor::FromSRGBColor(FColor(0x05, 0x05, 0x06, 0xFF)));
		Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Comp->SetGenerateOverlapEvents(false);
		Comp->SetCastShadow(false);
	}
}

AArenaScreen::AArenaScreen()
{
	using namespace ArenaScreenDetail;

	PrimaryActorTick.bCanEverTick = false;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	RootComponent = Root;

	// Widget plane = component local YZ, visible from +X; the actor's +X points at the audience.
	ScreenComp = CreateDefaultSubobject<UWidgetComponent>(TEXT("Screen"));
	ScreenComp->SetupAttachment(Root);
	SetupWidgetComponent(ScreenComp, UArenaScreenWidget::StaticClass(), ScreenPixels);

	BannerComp = CreateDefaultSubobject<UWidgetComponent>(TEXT("Banner"));
	BannerComp->SetupAttachment(Root);
	SetupWidgetComponent(BannerComp, UArenaBannerWidget::StaticClass(), BannerPixels);

	// Warm-white spill from the LED onto the stage and the front rows (emits along +X, toward the audience).
	Glow = CreateDefaultSubobject<URectLightComponent>(TEXT("Glow"));
	Glow->SetupAttachment(Root);
	Glow->SetMobility(EComponentMobility::Movable);
	Glow->IntensityUnits = ELightUnits::Candelas;
	Glow->Intensity = GlowMax * 0.5f;
	Glow->LightColor = FColor(255, 214, 172);
	Glow->AttenuationRadius = 3000.f;
	Glow->SourceWidth = 1800.f;
	Glow->SourceHeight = 1012.5f;
	Glow->CastShadows = false;
	Glow->SetRelativeLocation(FVector(50.f, 0.f, 0.f));
}

void AArenaScreen::BeginPlay()
{
	Super::BeginPlay();

	// UWidgetComponent::BeginPlay already does this; repeated so the widgets exist even if a component's order differs.
	ScreenComp->InitWidget();
	BannerComp->InitWidget();
}

void AArenaScreen::EndPlay(const EEndPlayReason::Type Reason)
{
	CloseMedia();
	Super::EndPlay(Reason);
}

void AArenaScreen::Init(const FTransform& ScreenTransform, const FVector2D& SizeCm, const FTransform& BannerTransform)
{
	using namespace ArenaScreenDetail;

	// LED size comes from SizeCm, so the actor itself stays unscaled.
	FTransform Placement = ScreenTransform;
	Placement.SetScale3D(FVector::OneVector);
	SetActorTransform(Placement);

	// One ratio for width and height keeps the 1920x1080 page at 16:9.
	const float WidthCm = static_cast<float>(SizeCm.X);
	const float HeightCm = static_cast<float>(SizeCm.Y);
	const float Scale = WidthCm > 0.f ? WidthCm / static_cast<float>(ScreenPixels.X) : 1.f;
	ScreenComp->SetRelativeLocationAndRotation(FVector::ZeroVector, FRotator::ZeroRotator);
	ScreenComp->SetRelativeScale3D(FVector(1.f, Scale, Scale));

	// BannerTransform is in world space; the component is attached to Root, so set it as a world transform.
	BannerComp->SetWorldLocationAndRotation(BannerTransform.GetLocation(), BannerTransform.GetRotation());
	BannerComp->SetWorldScale3D(BannerTransform.GetScale3D() * FVector(1.f, Scale, Scale));

	Glow->SetRelativeLocationAndRotation(FVector(50.f, 0.f, 0.f), FRotator::ZeroRotator);
	Glow->SetSourceWidth(WidthCm);
	Glow->SetSourceHeight(HeightCm);
}

FString AArenaScreen::SetSource(EArenaScreenSource Source, const FString& CaptureDeviceName, const FString& Url, const FString& StreamerName)
{
	using namespace ArenaScreenDetail;

	CloseMedia();
	RequestedSource = Source;
	FallbackUrl = Url;
	FallbackTitle = StreamerName;

	if (Source == EArenaScreenSource::Auto || Source == EArenaScreenSource::CaptureDevice)
	{
		const FString Trimmed = CaptureDeviceName.TrimStartAndEnd();
		const FString Wanted = Trimmed.IsEmpty() ? FString(DefaultCaptureDevice) : Trimmed;

		FMediaCaptureDevice Device;
		if (FindCaptureDevice(Wanted, Device) && OpenCaptureDevice(Device.Url))
		{
			ActiveSource = FString::Printf(TEXT("擷取裝置：%s"), *Device.DisplayName.ToString());
			UE_LOG(LogLiveArena, Log, TEXT("LED 來源：%s (%s)"), *ActiveSource, *Device.Url);
			return ActiveSource;
		}

		if (Source == EArenaScreenSource::CaptureDevice)
		{
			ShowWaiting(StreamerName, FString::Printf(
				TEXT("搵唔到擷取裝置「%s」 · 喺 OBS 撳「Start Virtual Camera」再重新開 Live Arena"), *Wanted));
			ActiveSource = FString::Printf(TEXT("搵唔到擷取裝置：%s"), *Wanted);
			UE_LOG(LogLiveArena, Warning, TEXT("LED 來源：%s"), *ActiveSource);
			return ActiveSource;
		}
	}

	if (Source != EArenaScreenSource::None && ShowWebSource(Url))
	{
		UE_LOG(LogLiveArena, Log, TEXT("LED 來源：%s"), *ActiveSource);
		return ActiveSource;
	}

	ShowWaiting(StreamerName, WaitingHint);
	ActiveSource = Source == EArenaScreenSource::None ? TEXT("等待畫面（ScreenSource = None）")
		: Source == EArenaScreenSource::Auto ? TEXT("等待畫面（搵唔到擷取裝置）")
		: TEXT("等待畫面（未設定網址）");
	UE_LOG(LogLiveArena, Log, TEXT("LED 來源：%s"), *ActiveSource);
	return ActiveSource;
}

bool AArenaScreen::OpenCaptureDevice(const FString& DeviceUrl)
{
	MediaPlayer = NewObject<UMediaPlayer>(this, NAME_None, RF_Transient);
	MediaPlayer->PlayOnOpen = true;
	MediaPlayer->OnMediaOpenFailed.AddDynamic(this, &AArenaScreen::HandleMediaOpenFailed);

	MediaTexture = NewObject<UMediaTexture>(this, NAME_None, RF_Transient);
	MediaTexture->SetMediaPlayer(MediaPlayer);
	MediaTexture->UpdateResource();

	if (!MediaPlayer->OpenUrl(DeviceUrl))
	{
		UE_LOG(LogLiveArena, Warning, TEXT("擷取裝置開唔到：%s"), *DeviceUrl);
		CloseMedia();
		return false;
	}

	if (UArenaScreenWidget* Widget = GetScreenWidget())
	{
		Widget->ShowMedia(MediaTexture);
	}
	return true;
}

bool AArenaScreen::ShowWebSource(const FString& Url)
{
	const FString Page = ArenaScreenDetail::ToLedPageUrl(Url);
	if (Page.IsEmpty())
	{
		return false;
	}
	if (UArenaScreenWidget* Widget = GetScreenWidget())
	{
		Widget->ShowUrl(Page);
	}
	ActiveSource = FString::Printf(TEXT("網頁：%s"), *Page);
	return true;
}

void AArenaScreen::ShowWaiting(const FString& Title, const FString& Subtitle)
{
	const FString Trimmed = Title.TrimStartAndEnd();
	if (UArenaScreenWidget* Widget = GetScreenWidget())
	{
		Widget->ShowPlaceholder(Trimmed.IsEmpty() ? FString(TEXT("直播即將開始")) : Trimmed, Subtitle);
	}
}

void AArenaScreen::CloseMedia()
{
	if (MediaPlayer)
	{
		MediaPlayer->OnMediaOpenFailed.RemoveAll(this);
		MediaPlayer->Close();
	}
	MediaPlayer = nullptr;
	MediaTexture = nullptr;
}

void AArenaScreen::HandleMediaOpenFailed(FString FailedUrl)
{
	UE_LOG(LogLiveArena, Warning, TEXT("擷取裝置開唔到（非同步）：%s"), *FailedUrl);

	// Not inside the player's own event broadcast: close it and fall back on the next tick.
	const TWeakObjectPtr<UMediaPlayer> Failed(MediaPlayer.Get());
	GetWorldTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this, Failed]()
	{
		if (!MediaPlayer || MediaPlayer.Get() != Failed.Get())
		{
			return; // the source was changed in the meantime
		}
		CloseMedia();

		if (RequestedSource == EArenaScreenSource::Auto && ShowWebSource(FallbackUrl))
		{
			UE_LOG(LogLiveArena, Log, TEXT("LED 來源改用：%s"), *ActiveSource);
			return;
		}
		ShowWaiting(FallbackTitle, TEXT("擷取裝置開唔到 · 檢查 OBS Virtual Camera 有冇開，或者喺 Project Settings > Live Arena 改用直播網址"));
		ActiveSource = TEXT("擷取裝置開唔到");
	}));
}

void AArenaScreen::SetBanner(const FString& Left, const FString& Centre, const FString& Right)
{
	if (UArenaBannerWidget* Widget = GetBannerWidget())
	{
		Widget->SetTexts(Left, Centre, Right);
	}
}

void AArenaScreen::ShowToast(const FString& Big, const FString& Small, float Seconds)
{
	if (UArenaScreenWidget* Widget = GetScreenWidget())
	{
		Widget->ShowToast(Big, Small, Seconds);
	}
}

void AArenaScreen::ClearToast()
{
	if (UArenaScreenWidget* Widget = GetScreenWidget())
	{
		Widget->ClearToast();
	}
}

void AArenaScreen::SetGlow(float Amount)
{
	const float Clamped = FMath::Clamp(Amount, 0.f, 1.f);
	if (!Glow || FMath::IsNearlyEqual(Clamped, GlowAmount, 0.001f))
	{
		return;
	}
	GlowAmount = Clamped;
	Glow->SetIntensity(Clamped * GlowMax);
}

UArenaScreenWidget* AArenaScreen::GetScreenWidget() const
{
	if (!ScreenComp)
	{
		return nullptr;
	}
	// SetSource may run before BeginPlay (actors spawned during StartPlay): create the widget on first use.
	if (!ScreenComp->GetUserWidgetObject())
	{
		ScreenComp->InitWidget();
	}
	return Cast<UArenaScreenWidget>(ScreenComp->GetUserWidgetObject());
}

UArenaBannerWidget* AArenaScreen::GetBannerWidget() const
{
	if (!BannerComp)
	{
		return nullptr;
	}
	if (!BannerComp->GetUserWidgetObject())
	{
		BannerComp->InitWidget();
	}
	return Cast<UArenaBannerWidget>(BannerComp->GetUserWidgetObject());
}
