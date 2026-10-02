#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "ArenaWidgets.generated.h"

class UImage;
class UTextBlock;
class UWebBrowser;
class UMediaTexture;
class UBorder;
class UWidgetSwitcher;

/**
 * All widgets are built in C++ (no .uasset): each class builds its tree in RebuildWidget() when
 * WidgetTree->RootWidget is null. Chinese text relies on the engine's default composite font (has CJK fallback).
 * Palette: background #050506, ivory #F3EBDD, brass #B08D57, warm #FFB86B, dim #9A928A. No blue, no neon.
 */

/** 1920x1080 LED content: live video (media texture) or a web page, plus a big toast for lottery names. */
UCLASS()
class LIVEARENA_API UArenaScreenWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void ShowMedia(UMediaTexture* Texture);
	void ShowUrl(const FString& Url);
	void ShowPlaceholder(const FString& Title, const FString& Subtitle);
	/** Full-screen dim + big centred text over the video for Seconds (<= 0 = until cleared). */
	void ShowToast(const FString& Big, const FString& Small, float Seconds);
	void ClearToast();

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	void Build();

	UPROPERTY(Transient) TObjectPtr<UWidgetSwitcher> Switcher;      // 0 = video, 1 = browser, 2 = placeholder
	UPROPERTY(Transient) TObjectPtr<UImage> Video;
	UPROPERTY(Transient) TObjectPtr<UWebBrowser> Browser;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> PlaceholderTitle;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> PlaceholderSub;
	UPROPERTY(Transient) TObjectPtr<UBorder> Toast;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ToastBig;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ToastSmall;

	FString PendingUrl;
	int32 PendingUrlTicks = 0;
	float ToastRemaining = 0.f;
};

/** Thin strip under the LED: "現場 1,234 人" | streamer name / mode | "入場 link". */
UCLASS()
class LIVEARENA_API UArenaBannerWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetTexts(const FString& Left, const FString& Centre, const FString& Right);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	void Build();

	UPROPERTY(Transient) TObjectPtr<UTextBlock> LeftText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> CentreText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> RightText;
};

/** Floating name tag above a named viewer (screen-space widget component). */
UCLASS()
class LIVEARENA_API UArenaLabelWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetLabel(const FString& Name, const FString& Seat, bool bHighlight);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	void Build();

	UPROPERTY(Transient) TObjectPtr<UBorder> Box;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> NameText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SeatText;
};

/** On-screen HUD for the streamer: status block (top-left), key help (bottom-left, H toggles), toast (centre). */
UCLASS()
class LIVEARENA_API UArenaHudWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetStatus(const FString& MultiLineText);
	void SetHelpVisible(bool bVisible);
	bool IsHelpVisible() const { return bHelpVisible; }
	void ShowToast(const FString& Big, const FString& Small, float Seconds);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	void Build();

	UPROPERTY(Transient) TObjectPtr<UTextBlock> StatusText;
	UPROPERTY(Transient) TObjectPtr<UBorder> HelpBox;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> HelpText;
	UPROPERTY(Transient) TObjectPtr<UBorder> ToastBox;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ToastBig;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ToastSmall;

	bool bHelpVisible = true;
	float ToastRemaining = 0.f;
};
