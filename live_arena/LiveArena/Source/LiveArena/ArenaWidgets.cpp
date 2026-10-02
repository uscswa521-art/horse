#include "ArenaWidgets.h"

#include "LiveArena.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WidgetSwitcher.h"
#include "Components/WidgetSwitcherSlot.h"
#include "MediaTexture.h"
#include "UObject/UnrealType.h"
#include "WebBrowser.h"

// 全部 widget 都喺 C++ 砌（冇 .uasset）。每個 Build() 都係 idempotent：RootWidget 有咗就唔再砌，
// 所以 public setter 可以喺 slate 未建好之前先 call（UMG 會記住值，RebuildWidget 時套用）。

// Named (not anonymous) namespace: unity builds paste several .cpp files into one translation unit.
namespace ArenaWidgetsDetail
{
	static const float ToastFadeSeconds = 0.4f;
	static const float HudToastDefaultSeconds = 3.f;

	/**
	 * The LED page is the streamer's own stream: its sound would reach OBS desktop audio and come back in the stream as
	 * an echo. Re-run every second because players create (and sometimes unmute) their media elements late.
	 * Only the top frame is reached, which is where the video lives for the pages the LED loads (embed / channel page).
	 */
	static const float BrowserMuteInterval = 1.f;
	static const TCHAR* MuteMediaScript =
		TEXT("document.querySelectorAll('video,audio').forEach(function(m){m.muted=true;m.volume=0;});");

	/** Palette colour from sRGB hex components; Slate colours are linear. */
	static FLinearColor Srgb(uint8 R, uint8 G, uint8 B, float Alpha = 1.f)
	{
		FLinearColor Color = FLinearColor::FromSRGBColor(FColor(R, G, B, 255));
		Color.A = Alpha;
		return Color;
	}

	static FLinearColor Bg(float Alpha = 1.f)   { return Srgb(0x05, 0x05, 0x06, Alpha); }
	static FLinearColor Wall(float Alpha = 1.f) { return Srgb(0x14, 0x15, 0x1A, Alpha); }
	static FLinearColor Ivory()                 { return Srgb(0xF3, 0xEB, 0xDD); }
	static FLinearColor Brass()                 { return Srgb(0xB0, 0x8D, 0x57); }
	static FLinearColor Warm()                  { return Srgb(0xFF, 0xB8, 0x6B); }
	static FLinearColor Grey()                  { return Srgb(0x9A, 0x92, 0x8A); }

	/** Starts from the text block's default font (engine composite font with CJK fallback) and only changes size / weight. */
	static void StyleText(UTextBlock* Text, int32 Size, const FLinearColor& Color, bool bBold, int32 LetterSpacing = 0)
	{
		FSlateFontInfo FontInfo = Text->GetFont();
		FontInfo.Size = Size;
		FontInfo.TypefaceFontName = bBold ? FName(TEXT("Bold")) : FName(TEXT("Regular"));
		FontInfo.LetterSpacing = LetterSpacing;
		Text->SetFont(FontInfo);
		Text->SetColorAndOpacity(FSlateColor(Color));
	}

	static UTextBlock* MakeText(UWidgetTree* Tree, const FString& Initial, int32 Size, const FLinearColor& Color, bool bBold,
		ETextJustify::Type Justify, int32 LetterSpacing = 0)
	{
		UTextBlock* Text = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		StyleText(Text, Size, Color, bBold, LetterSpacing);
		Text->SetJustification(Justify);
		Text->SetText(FText::FromString(Initial));
		return Text;
	}

	/** Solid box. Padding / alignment are set before SetContent so the border slot copies them. */
	static UBorder* MakeBox(UWidgetTree* Tree, const FLinearColor& Color, const FMargin& Pad, UWidget* Content,
		EHorizontalAlignment HAlign = HAlign_Fill, EVerticalAlignment VAlign = VAlign_Fill)
	{
		UBorder* Box = Tree->ConstructWidget<UBorder>(UBorder::StaticClass());
		Box->SetBrushColor(Color);
		Box->SetPadding(Pad);
		Box->SetHorizontalAlignment(HAlign);
		Box->SetVerticalAlignment(VAlign);
		if (Content)
		{
			Box->SetContent(Content);
		}
		return Box;
	}

	/** Solid bar; a size <= 0 leaves that axis to the parent slot (e.g. Width 0 + Height 2 = full-width hairline). */
	static UWidget* MakeRule(UWidgetTree* Tree, const FLinearColor& Color, float Width, float Height)
	{
		USizeBox* Size = Tree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		if (Width > 0.f)
		{
			Size->SetWidthOverride(Width);
		}
		if (Height > 0.f)
		{
			Size->SetHeightOverride(Height);
		}
		Size->SetContent(MakeBox(Tree, Color, FMargin(0.f), nullptr));
		return Size;
	}

	static UVerticalBoxSlot* AddToColumn(UVerticalBox* Column, UWidget* Child, EHorizontalAlignment HAlign, const FMargin& Pad,
		bool bFill = false)
	{
		UVerticalBoxSlot* ColumnSlot = Column->AddChildToVerticalBox(Child);
		ColumnSlot->SetHorizontalAlignment(HAlign);
		ColumnSlot->SetVerticalAlignment(VAlign_Fill);
		ColumnSlot->SetPadding(Pad);
		ColumnSlot->SetSize(FSlateChildSize(bFill ? ESlateSizeRule::Fill : ESlateSizeRule::Automatic));
		return ColumnSlot;
	}

	static UHorizontalBoxSlot* AddToRow(UHorizontalBox* Row, UWidget* Child, const FMargin& Pad, bool bFill)
	{
		UHorizontalBoxSlot* RowSlot = Row->AddChildToHorizontalBox(Child);
		RowSlot->SetHorizontalAlignment(HAlign_Fill);
		RowSlot->SetVerticalAlignment(VAlign_Center);
		RowSlot->SetPadding(Pad);
		RowSlot->SetSize(FSlateChildSize(bFill ? ESlateSizeRule::Fill : ESlateSizeRule::Automatic));
		return RowSlot;
	}

	static void AddFilling(UOverlay* Overlay, UWidget* Child)
	{
		UOverlaySlot* OverlaySlot = Overlay->AddChildToOverlay(Child);
		OverlaySlot->SetHorizontalAlignment(HAlign_Fill);
		OverlaySlot->SetVerticalAlignment(VAlign_Fill);
	}

	static void AddPage(UWidgetSwitcher* Switcher, UWidget* Page)
	{
		if (UWidgetSwitcherSlot* PageSlot = Cast<UWidgetSwitcherSlot>(Switcher->AddChild(Page)))
		{
			PageSlot->SetHorizontalAlignment(HAlign_Fill);
			PageSlot->SetVerticalAlignment(VAlign_Fill);
		}
	}

	static void SetTextOrCollapse(UTextBlock* Text, const FString& Value)
	{
		Text->SetText(FText::FromString(Value));
		Text->SetVisibility(Value.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	}

	/**
	 * UWebBrowser::LoadURL is dropped while CEF is still creating the browser, so the first page is handed over as
	 * InitialURL (protected, hence reflection) before the browser's Slate widget exists. False if the property is gone.
	 */
	static bool SetBrowserInitialUrl(UWebBrowser* Browser, const FString& Url)
	{
		if (FStrProperty* Property = FindFProperty<FStrProperty>(UWebBrowser::StaticClass(), TEXT("InitialURL")))
		{
			Property->SetPropertyValue_InContainer(Browser, Url);
			return true;
		}
		return false;
	}
}

// =====================================================================================================================
// UArenaScreenWidget — 1920x1080 LED content
// =====================================================================================================================

TSharedRef<SWidget> UArenaScreenWidget::RebuildWidget()
{
	Build();
	return Super::RebuildWidget();
}

void UArenaScreenWidget::Build()
{
	using namespace ArenaWidgetsDetail;

	if (!WidgetTree)
	{
		Initialize();
	}
	if (!WidgetTree || WidgetTree->RootWidget)
	{
		return;
	}
	UWidgetTree* Tree = WidgetTree;

	UOverlay* Stack = Tree->ConstructWidget<UOverlay>(UOverlay::StaticClass());
	Stack->SetVisibility(ESlateVisibility::HitTestInvisible);

	// ---- bottom layer: source pages ----
	Switcher = Tree->ConstructWidget<UWidgetSwitcher>(UWidgetSwitcher::StaticClass());

	// 0: live video (media texture)
	Video = Tree->ConstructWidget<UImage>(UImage::StaticClass());
	FSlateBrush VideoBrush;
	VideoBrush.DrawAs = ESlateBrushDrawType::Image;
	VideoBrush.Tiling = ESlateBrushTileType::NoTile;
	VideoBrush.ImageSize = FVector2D(1920.f, 1080.f);
	Video->SetBrush(VideoBrush);
	AddPage(Switcher, Video);

	// 1: web page (browser created on demand inside this host)
	BrowserHost = Tree->ConstructWidget<UOverlay>(UOverlay::StaticClass());
	AddPage(Switcher, BrowserHost);

	// 2: placeholder — LIVE ARENA / title / brass rule / subtitle on near-black
	PlaceholderTitle = MakeText(Tree, TEXT("直播即將開始"), 96, Ivory(), true, ETextJustify::Center);
	PlaceholderSub = MakeText(Tree, TEXT("等待直播畫面"), 32, Grey(), false, ETextJustify::Center);
	PlaceholderSub->SetAutoWrapText(true);
	USizeBox* SubWidth = Tree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	SubWidth->SetMaxDesiredWidth(1400.f);
	SubWidth->SetContent(PlaceholderSub);

	UVerticalBox* PlaceholderColumn = Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	AddToColumn(PlaceholderColumn, MakeText(Tree, TEXT("LIVE ARENA"), 22, Brass(), false, ETextJustify::Center, 600),
		HAlign_Center, FMargin(0.f, 0.f, 0.f, 40.f));
	AddToColumn(PlaceholderColumn, PlaceholderTitle, HAlign_Center, FMargin(0.f, 0.f, 0.f, 36.f));
	AddToColumn(PlaceholderColumn, MakeRule(Tree, Brass(), 180.f, 2.f), HAlign_Center, FMargin(0.f, 0.f, 0.f, 36.f));
	AddToColumn(PlaceholderColumn, SubWidth, HAlign_Center, FMargin(0.f));
	AddPage(Switcher, MakeBox(Tree, Bg(), FMargin(120.f, 80.f), PlaceholderColumn, HAlign_Center, VAlign_Center));

	Switcher->SetActiveWidgetIndex(2);
	AddFilling(Stack, Switcher);

	// ---- top layer: toast (dims the video, big name + small line) ----
	ToastBig = MakeText(Tree, FString(), 120, Ivory(), true, ETextJustify::Center);
	ToastBig->SetShadowOffset(FVector2D(0.f, 4.f));
	ToastBig->SetShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.6f));
	ToastSmall = MakeText(Tree, FString(), 48, Warm(), false, ETextJustify::Center);

	UVerticalBox* ToastColumn = Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	AddToColumn(ToastColumn, ToastBig, HAlign_Center, FMargin(0.f));
	AddToColumn(ToastColumn, MakeRule(Tree, Brass(), 240.f, 3.f), HAlign_Center, FMargin(0.f, 28.f));
	AddToColumn(ToastColumn, ToastSmall, HAlign_Center, FMargin(0.f));

	Toast = MakeBox(Tree, Bg(0.72f), FMargin(80.f), ToastColumn, HAlign_Center, VAlign_Center);
	Toast->SetVisibility(ESlateVisibility::Collapsed);
	AddFilling(Stack, Toast);

	WidgetTree->RootWidget = Stack;
}

void UArenaScreenWidget::ReleaseBrowser()
{
	PendingUrl.Reset();
	PendingUrlTicks = 0;
	if (Browser)
	{
		// RemoveFromParent releases the Slate widget -> SWebBrowser closes the CEF browser (and its audio).
		Browser->RemoveFromParent();
		Browser = nullptr;
	}
}

void UArenaScreenWidget::ShowMedia(UMediaTexture* Texture)
{
	Build();
	if (!Texture || !Switcher || !Video)
	{
		return;
	}
	ReleaseBrowser();
	Video->SetBrushResourceObject(Texture);
	Switcher->SetActiveWidgetIndex(0);
}

void UArenaScreenWidget::ShowUrl(const FString& Url)
{
	using namespace ArenaWidgetsDetail;

	Build();
	if (!Switcher || !BrowserHost || Url.IsEmpty())
	{
		return;
	}

	Switcher->SetActiveWidgetIndex(1);
	PendingUrlTicks = 0;

	if (Browser)
	{
		// The browser already runs: navigate it from NativeTick (its Slate widget may be rebuilt this frame).
		PendingUrl = Url;
		return;
	}

	Browser = WidgetTree->ConstructWidget<UWebBrowser>(UWebBrowser::StaticClass());
	const bool bInitialSet = SetBrowserInitialUrl(Browser, Url);
	AddFilling(BrowserHost, Browser);
	// Fallback when InitialURL could not be set: LoadURL a couple of ticks after the Slate widget exists.
	PendingUrl = bInitialSet ? FString() : Url;
	UE_LOG(LogLiveArena, Log, TEXT("LED 網頁：%s"), *Url);
}

void UArenaScreenWidget::ShowPlaceholder(const FString& Title, const FString& Subtitle)
{
	Build();
	if (!Switcher || !PlaceholderTitle || !PlaceholderSub)
	{
		return;
	}
	ReleaseBrowser();
	PlaceholderTitle->SetText(FText::FromString(Title));
	ArenaWidgetsDetail::SetTextOrCollapse(PlaceholderSub, Subtitle);
	Switcher->SetActiveWidgetIndex(2);
}

void UArenaScreenWidget::ShowToast(const FString& Big, const FString& Small, float Seconds)
{
	Build();
	if (!Toast || !ToastBig || !ToastSmall)
	{
		return;
	}
	ArenaWidgetsDetail::SetTextOrCollapse(ToastBig, Big);
	ArenaWidgetsDetail::SetTextOrCollapse(ToastSmall, Small);
	Toast->SetRenderOpacity(1.f);
	Toast->SetVisibility(ESlateVisibility::HitTestInvisible);
	// < 0 marks a sticky toast that stays until ClearToast().
	ToastRemaining = Seconds > 0.f ? Seconds : -1.f;
}

void UArenaScreenWidget::ClearToast()
{
	ToastRemaining = 0.f;
	if (Toast)
	{
		Toast->SetVisibility(ESlateVisibility::Collapsed);
	}
}

void UArenaScreenWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	if (!PendingUrl.IsEmpty() && Browser && ++PendingUrlTicks >= 2)
	{
		const FString Url = PendingUrl;
		PendingUrl.Reset();
		Browser->LoadURL(Url);
	}

	if (Browser)
	{
		BrowserMuteTimer -= InDeltaTime;
		if (BrowserMuteTimer <= 0.f)
		{
			BrowserMuteTimer = ArenaWidgetsDetail::BrowserMuteInterval;
			Browser->ExecuteJavascript(ArenaWidgetsDetail::MuteMediaScript); // no-op until the page exists
		}
	}

	if (ToastRemaining > 0.f && Toast)
	{
		ToastRemaining -= InDeltaTime;
		if (ToastRemaining <= 0.f)
		{
			ClearToast();
		}
		else
		{
			Toast->SetRenderOpacity(FMath::Clamp(ToastRemaining / ArenaWidgetsDetail::ToastFadeSeconds, 0.f, 1.f));
		}
	}
}

// =====================================================================================================================
// UArenaBannerWidget — 1920x120 strip under the LED
// =====================================================================================================================

TSharedRef<SWidget> UArenaBannerWidget::RebuildWidget()
{
	Build();
	return Super::RebuildWidget();
}

void UArenaBannerWidget::Build()
{
	using namespace ArenaWidgetsDetail;

	if (!WidgetTree)
	{
		Initialize();
	}
	if (!WidgetTree || WidgetTree->RootWidget)
	{
		return;
	}
	UWidgetTree* Tree = WidgetTree;

	LeftText = MakeText(Tree, FString(), 46, Ivory(), true, ETextJustify::Left, 40);
	CentreText = MakeText(Tree, FString(), 38, Brass(), true, ETextJustify::Center, 120);
	RightText = MakeText(Tree, FString(), 30, Warm(), false, ETextJustify::Right);
	CentreText->SetClipping(EWidgetClipping::ClipToBounds);

	UHorizontalBox* Row = Tree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	AddToRow(Row, LeftText, FMargin(56.f, 0.f, 32.f, 0.f), false);
	AddToRow(Row, CentreText, FMargin(0.f), true);
	AddToRow(Row, RightText, FMargin(32.f, 0.f, 56.f, 0.f), false);

	UVerticalBox* Column = Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	AddToColumn(Column, MakeRule(Tree, Brass(), 0.f, 3.f), HAlign_Fill, FMargin(0.f));
	AddToColumn(Column, Row, HAlign_Fill, FMargin(0.f), true);
	AddToColumn(Column, MakeRule(Tree, Brass(), 0.f, 3.f), HAlign_Fill, FMargin(0.f));

	UBorder* Background = MakeBox(Tree, Bg(), FMargin(0.f), Column);
	Background->SetVisibility(ESlateVisibility::HitTestInvisible);
	WidgetTree->RootWidget = Background;
}

void UArenaBannerWidget::SetTexts(const FString& Left, const FString& Centre, const FString& Right)
{
	Build();
	if (!LeftText || !CentreText || !RightText)
	{
		return;
	}
	LeftText->SetText(FText::FromString(Left));
	CentreText->SetText(FText::FromString(Centre));
	RightText->SetText(FText::FromString(Right));
}

// =====================================================================================================================
// UArenaLabelWidget — name tag above a named viewer
// =====================================================================================================================

TSharedRef<SWidget> UArenaLabelWidget::RebuildWidget()
{
	Build();
	return Super::RebuildWidget();
}

void UArenaLabelWidget::Build()
{
	using namespace ArenaWidgetsDetail;

	if (!WidgetTree)
	{
		Initialize();
	}
	if (!WidgetTree || WidgetTree->RootWidget)
	{
		return;
	}
	UWidgetTree* Tree = WidgetTree;

	NameText = MakeText(Tree, FString(), 14, Ivory(), true, ETextJustify::Center);
	SeatText = MakeText(Tree, FString(), 11, Brass(), false, ETextJustify::Center, 60);

	UVerticalBox* Column = Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	AddToColumn(Column, NameText, HAlign_Center, FMargin(0.f));
	AddToColumn(Column, SeatText, HAlign_Center, FMargin(0.f, 1.f, 0.f, 0.f));

	// 1 px brass frame = brass border around the dark (90 %) inner box.
	Box = MakeBox(Tree, Wall(0.9f), FMargin(10.f, 4.f, 10.f, 5.f), Column, HAlign_Center, VAlign_Center);
	UBorder* Frame = MakeBox(Tree, Brass(), FMargin(1.f), Box);
	Frame->SetVisibility(ESlateVisibility::HitTestInvisible);
	WidgetTree->RootWidget = Frame;
}

void UArenaLabelWidget::SetLabel(const FString& Name, const FString& Seat, bool bHighlight)
{
	using namespace ArenaWidgetsDetail;

	Build();
	if (!Box || !NameText || !SeatText)
	{
		return;
	}
	NameText->SetText(FText::FromString(Name));
	SetTextOrCollapse(SeatText, Seat);

	const FLinearColor Ink = Bg();
	Box->SetBrushColor(bHighlight ? Warm() : Wall(0.9f));
	NameText->SetColorAndOpacity(FSlateColor(bHighlight ? Ink : Ivory()));
	SeatText->SetColorAndOpacity(FSlateColor(bHighlight ? Ink : Brass()));
}

// =====================================================================================================================
// UArenaHudWidget — streamer HUD
// =====================================================================================================================

TSharedRef<SWidget> UArenaHudWidget::RebuildWidget()
{
	Build();
	return Super::RebuildWidget();
}

void UArenaHudWidget::Build()
{
	using namespace ArenaWidgetsDetail;

	if (!WidgetTree)
	{
		Initialize();
	}
	if (!WidgetTree || WidgetTree->RootWidget)
	{
		return;
	}
	UWidgetTree* Tree = WidgetTree;

	UCanvasPanel* Canvas = Tree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass());
	// The HUD never takes the mouse: the player controller polls keys and free-fly uses the mouse.
	Canvas->SetVisibility(ESlateVisibility::HitTestInvisible);

	// ---- top-left: status block ----
	StatusText = MakeText(Tree, TEXT("Live Arena · 準備中…"), 13, Ivory(), false, ETextJustify::Left, 20);
	UBorder* StatusBox = MakeBox(Tree, Bg(0.62f), FMargin(14.f, 10.f, 16.f, 12.f), StatusText, HAlign_Left, VAlign_Top);
	UCanvasPanelSlot* StatusSlot = Canvas->AddChildToCanvas(StatusBox);
	StatusSlot->SetAnchors(FAnchors(0.f, 0.f));
	StatusSlot->SetAlignment(FVector2D(0.f, 0.f));
	StatusSlot->SetPosition(FVector2D(24.f, 24.f));
	StatusSlot->SetAutoSize(true);

	// ---- bottom-left: key help (one row per key so CJK and Latin lines stay aligned) ----
	struct FHelpRow
	{
		const TCHAR* Key;
		const TCHAR* Text;
	};
	static const FHelpRow Rows[] = {
		{ TEXT("Space"), TEXT("切換 觀看 / 互動 模式") },
		{ TEXT("L"), TEXT("開始抽獎（抽中之後再撳 = 重抽）") },
		{ TEXT("Enter"), TEXT("請中獎觀眾上台（佢即刻收到通話 link）") },
		{ TEXT("K"), TEXT("取消抽獎／嘉賓落台（返原位）") },
		{ TEXT("1 2 3 4"), TEXT("鏡頭：直播 / 直播主視角 / 全景 / 跟隨") },
		{ TEXT("F"), TEXT("自由飛行（WASD + 滑鼠，再撳 F 離開）") },
		{ TEXT("[  ]"), TEXT("曝光 減 / 加") },
		{ TEXT("H"), TEXT("顯示 / 收埋 呢個說明") },
		{ TEXT("Tab"), TEXT("收埋成個 HUD（OBS 乾淨畫面）") },
		{ TEXT("D"), TEXT("示範：加 20 個觀眾（冇設定伺服器先用得）") },
		{ TEXT("C"), TEXT("示範：30 下拍手") },
	};

	UVerticalBox* HelpColumn = Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	HelpText = MakeText(Tree, TEXT("按鍵 · H 收埋"), 11, Brass(), true, ETextJustify::Left, 80);
	AddToColumn(HelpColumn, HelpText, HAlign_Left, FMargin(0.f, 0.f, 0.f, 8.f));
	for (const FHelpRow& Row : Rows)
	{
		USizeBox* KeyCell = Tree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		KeyCell->SetWidthOverride(92.f);
		KeyCell->SetContent(MakeText(Tree, Row.Key, 13, Warm(), true, ETextJustify::Left));

		UHorizontalBox* Line = Tree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		AddToRow(Line, KeyCell, FMargin(0.f), false);
		AddToRow(Line, MakeText(Tree, Row.Text, 13, Ivory(), false, ETextJustify::Left), FMargin(0.f), true);
		AddToColumn(HelpColumn, Line, HAlign_Fill, FMargin(0.f, 1.f));
	}

	HelpBox = MakeBox(Tree, Bg(0.62f), FMargin(16.f, 12.f, 20.f, 14.f), HelpColumn, HAlign_Left, VAlign_Top);
	HelpBox->SetVisibility(bHelpVisible ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	UCanvasPanelSlot* HelpSlot = Canvas->AddChildToCanvas(HelpBox);
	HelpSlot->SetAnchors(FAnchors(0.f, 1.f));
	HelpSlot->SetAlignment(FVector2D(0.f, 1.f));
	HelpSlot->SetPosition(FVector2D(24.f, -24.f));
	HelpSlot->SetAutoSize(true);

	// ---- centre: toast ----
	ToastBig = MakeText(Tree, FString(), 40, Ivory(), true, ETextJustify::Center);
	ToastSmall = MakeText(Tree, FString(), 20, Warm(), false, ETextJustify::Center);
	UVerticalBox* ToastColumn = Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	AddToColumn(ToastColumn, MakeRule(Tree, Brass(), 64.f, 2.f), HAlign_Center, FMargin(0.f, 0.f, 0.f, 14.f));
	AddToColumn(ToastColumn, ToastBig, HAlign_Center, FMargin(0.f));
	AddToColumn(ToastColumn, ToastSmall, HAlign_Center, FMargin(0.f, 6.f, 0.f, 0.f));

	ToastBox = MakeBox(Tree, Bg(0.78f), FMargin(44.f, 22.f, 44.f, 26.f), ToastColumn, HAlign_Center, VAlign_Center);
	ToastBox->SetVisibility(ESlateVisibility::Collapsed);
	UCanvasPanelSlot* ToastSlot = Canvas->AddChildToCanvas(ToastBox);
	ToastSlot->SetAnchors(FAnchors(0.5f, 0.5f));
	ToastSlot->SetAlignment(FVector2D(0.5f, 0.5f));
	ToastSlot->SetPosition(FVector2D(0.f, 0.f));
	ToastSlot->SetAutoSize(true);

	WidgetTree->RootWidget = Canvas;
}

void UArenaHudWidget::SetStatus(const FString& MultiLineText)
{
	Build();
	if (StatusText)
	{
		StatusText->SetText(FText::FromString(MultiLineText));
	}
}

void UArenaHudWidget::SetHelpVisible(bool bVisible)
{
	Build();
	bHelpVisible = bVisible;
	if (HelpBox)
	{
		HelpBox->SetVisibility(bVisible ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
}

void UArenaHudWidget::ShowToast(const FString& Big, const FString& Small, float Seconds)
{
	Build();
	if (!ToastBox || !ToastBig || !ToastSmall)
	{
		return;
	}
	ArenaWidgetsDetail::SetTextOrCollapse(ToastBig, Big);
	ArenaWidgetsDetail::SetTextOrCollapse(ToastSmall, Small);
	ToastBox->SetRenderOpacity(1.f);
	ToastBox->SetVisibility(ESlateVisibility::HitTestInvisible);
	// The HUD has no ClearToast, so a non-positive duration falls back to a default instead of sticking forever.
	ToastRemaining = Seconds > 0.f ? Seconds : ArenaWidgetsDetail::HudToastDefaultSeconds;
}

void UArenaHudWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	if (ToastRemaining > 0.f && ToastBox)
	{
		ToastRemaining -= InDeltaTime;
		if (ToastRemaining <= 0.f)
		{
			ToastRemaining = 0.f;
			ToastBox->SetVisibility(ESlateVisibility::Collapsed);
		}
		else
		{
			ToastBox->SetRenderOpacity(FMath::Clamp(ToastRemaining / ArenaWidgetsDetail::ToastFadeSeconds, 0.f, 1.f));
		}
	}
}
