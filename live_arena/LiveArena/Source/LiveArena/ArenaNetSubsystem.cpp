#include "ArenaNetSubsystem.h"

#include "LiveArena.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/GameInstance.h"
#include "IWebSocket.h"
#include "Modules/ModuleManager.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TimerManager.h"
#include "WebSocketsModule.h"

// 協議：live_arena/PROTOCOL.md（host 部分）。
// 所有 socket callback 都經 AsyncTask 搬返 game thread，再用 SocketEpoch 過濾舊 socket 嘅遲到事件，
// 所以 delegate 一定喺 game thread、而且唔會喺 IWebSocket 自己嘅 broadcast 入面重入（例如喺 callback 入面 Close）。

// Named (not anonymous) namespace: unity builds paste several .cpp files into one translation unit.
namespace ArenaNetDetail
{
	/** 1, 2, 4, 8, then 15 s forever. */
	static const float ReconnectDelays[] = { 1.f, 2.f, 4.f, 8.f, 15.f };

	/** A link that stayed up this long counts as healthy and restarts the backoff at 1 s. */
	static const double StableLinkSeconds = 10.0;

	/** RFC 3986 percent-encoding of the UTF-8 bytes (unreserved characters are kept). */
	static FString UrlEncode(const FString& In)
	{
		static const TCHAR Hex[] = TEXT("0123456789ABCDEF");

		FTCHARToUTF8 Utf8(*In);
		const uint8* Bytes = reinterpret_cast<const uint8*>(Utf8.Get());
		const int32 Len = Utf8.Length();

		FString Out;
		Out.Reserve(Len * 3);
		for (int32 Index = 0; Index < Len; ++Index)
		{
			const uint8 C = Bytes[Index];
			const bool bUnreserved = (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9')
				|| C == '-' || C == '_' || C == '.' || C == '~';
			if (bUnreserved)
			{
				Out.AppendChar(static_cast<TCHAR>(C));
			}
			else
			{
				Out.AppendChar(TEXT('%'));
				Out.AppendChar(Hex[C >> 4]);
				Out.AppendChar(Hex[C & 0x0F]);
			}
		}
		return Out;
	}

	/** Server rule: [A-Za-z0-9_-]{1,32}; anything else gets HTTP 400 on the upgrade. */
	static bool IsValidRoomName(const FString& Room)
	{
		if (Room.Len() < 1 || Room.Len() > 32)
		{
			return false;
		}
		for (int32 Index = 0; Index < Room.Len(); ++Index)
		{
			const TCHAR C = Room[Index];
			const bool bOk = (C >= TEXT('A') && C <= TEXT('Z')) || (C >= TEXT('a') && C <= TEXT('z'))
				|| (C >= TEXT('0') && C <= TEXT('9')) || C == TEXT('_') || C == TEXT('-');
			if (!bOk)
			{
				return false;
			}
		}
		return true;
	}

	/**
	 * Forgiving clean-up of what people paste into ServerUrl:
	 *   localhost:8787           -> ws://localhost:8787/ws
	 *   http(s)://host:8787      -> ws(s)://host:8787/ws
	 *   ws://host:8787  or  .../ -> ws://host:8787/ws
	 * Anything that already has a path is left alone.
	 */
	static FString NormalizeBaseUrl(const FString& In)
	{
		FString Url = In.TrimStartAndEnd();
		if (Url.IsEmpty())
		{
			return Url;
		}

		if (Url.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase))
		{
			Url = TEXT("ws://") + Url.RightChop(7);
		}
		else if (Url.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase))
		{
			Url = TEXT("wss://") + Url.RightChop(8);
		}
		else if (Url.Find(TEXT("://"), ESearchCase::CaseSensitive) == INDEX_NONE)
		{
			Url = TEXT("ws://") + Url;
		}

		const int32 SchemeEnd = Url.Find(TEXT("://"), ESearchCase::CaseSensitive);
		if (SchemeEnd == INDEX_NONE)
		{
			return Url;
		}

		const int32 HostStart = SchemeEnd + 3;
		int32 QueryStart = INDEX_NONE;
		if (!Url.FindChar(TEXT('?'), QueryStart))
		{
			QueryStart = INDEX_NONE;
		}
		const FString HostAndPath = (QueryStart == INDEX_NONE) ? Url.Mid(HostStart) : Url.Mid(HostStart, QueryStart - HostStart);
		const FString Query = (QueryStart == INDEX_NONE) ? FString() : Url.Mid(QueryStart);

		int32 SlashIndex = INDEX_NONE;
		if (!HostAndPath.FindChar(TEXT('/'), SlashIndex))
		{
			return Url.Left(HostStart) + HostAndPath + TEXT("/ws") + Query;
		}
		if (SlashIndex == HostAndPath.Len() - 1)
		{
			return Url.Left(HostStart) + HostAndPath + TEXT("ws") + Query;
		}
		return Url;
	}

	static const TCHAR* ModeToString(EArenaMode Mode)
	{
		return Mode == EArenaMode::Interactive ? TEXT("interactive") : TEXT("watch");
	}

	/** Close codes the server uses for a permanent refusal (see attach_host in server.py). */
	static constexpr int32 CloseBadKey = 1008;
	static constexpr int32 CloseReplaced = 4000;

	/**
	 * The server only accepts absolute http(s) links for streamUrl / callUrl and blanks anything else, while people
	 * often paste "discord.gg/abc" or "youtube.com/watch?v=..." without a scheme. Same rule as the LED (ArenaScreen.cpp).
	 */
	static FString WithWebScheme(const FString& In)
	{
		FString Url = In.TrimStartAndEnd();
		if (!Url.IsEmpty() && !Url.Contains(TEXT("://")) && !Url.StartsWith(TEXT("about:")))
		{
			Url = TEXT("https://") + Url;
		}
		return Url;
	}

	static void WarnIfNotWeb(const TCHAR* What, const FString& Url)
	{
		if (!Url.IsEmpty() && !Url.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase) && !Url.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase))
		{
			UE_LOG(LogLiveArena, Warning, TEXT("[Net] %s '%s' is not an http(s) link; the server will treat it as empty."), What, *Url);
		}
	}

	static FString ToJsonString(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// lifecycle

void UArenaNetSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Make sure the WebSockets module is up before the first Connect() (packaged builds load it lazily).
	FModuleManager::LoadModuleChecked<FWebSocketsModule>(TEXT("WebSockets"));
}

void UArenaNetSubsystem::Deinitialize()
{
	bWantConnected = false;
	ClearReconnectTimer();
	CloseSocket();
	bConnected = false; // no broadcast while the game instance is tearing down

	OnSnapshot.Clear();
	OnJoin.Clear();
	OnLeave.Clear();
	OnClap.Clear();
	OnYouTubeCount.Clear();
	OnConnectionChanged.Clear();
	OnError.Clear();

	Super::Deinitialize();
}

// ---------------------------------------------------------------------------------------------------------------------
// connection

void UArenaNetSubsystem::Connect(const FString& Url, const FString& Room, const FString& Key)
{
	BaseUrl = ArenaNetDetail::NormalizeBaseUrl(Url);
	RoomId = Room.TrimStartAndEnd();
	HostKey = Key;

	if (BaseUrl.IsEmpty())
	{
		UE_LOG(LogLiveArena, Warning, TEXT("[Net] Connect: ServerUrl is empty, staying offline."));
		Disconnect();
		return;
	}
	if (!ArenaNetDetail::IsValidRoomName(RoomId))
	{
		UE_LOG(LogLiveArena, Warning, TEXT("[Net] Room '%s' is not [A-Za-z0-9_-]{1,32}; the server will refuse it."), *RoomId);
	}
	if (HostKey.IsEmpty())
	{
		UE_LOG(LogLiveArena, Warning, TEXT("[Net] HostKey is empty; anyone who knows the room name could take over as host."));
	}

	bWantConnected = true;
	ReconnectAttempt = 0;
	ClearReconnectTimer();
	OpenSocket();
}

void UArenaNetSubsystem::Disconnect()
{
	bWantConnected = false;
	ReconnectAttempt = 0;
	ClearReconnectTimer();
	CloseSocket();
	SetConnected(false);
}

bool UArenaNetSubsystem::IsConnected() const
{
	return bConnected && Socket.IsValid();
}

FString UArenaNetSubsystem::BuildUrl(bool bForLog) const
{
	int32 QueryIndex = INDEX_NONE;
	const TCHAR* Separator = BaseUrl.FindChar(TEXT('?'), QueryIndex) ? TEXT("&") : TEXT("?");
	const FString KeyPart = bForLog ? FString(TEXT("***")) : ArenaNetDetail::UrlEncode(HostKey);
	return BaseUrl + Separator + TEXT("role=host&room=") + ArenaNetDetail::UrlEncode(RoomId) + TEXT("&key=") + KeyPart;
}

void UArenaNetSubsystem::OpenSocket()
{
	// Also called by the reconnect timer.
	CloseSocket();
	SetConnected(false);

	if (!bWantConnected || BaseUrl.IsEmpty())
	{
		return;
	}

	FWebSocketsModule& WebSockets = FModuleManager::LoadModuleChecked<FWebSocketsModule>(TEXT("WebSockets"));

	UE_LOG(LogLiveArena, Log, TEXT("[Net] Connecting to %s (attempt %d)"), *BuildUrl(true), ReconnectAttempt + 1);

	// No sub-protocol: the server does not negotiate one.
	Socket = WebSockets.CreateWebSocket(BuildUrl(false));

	const uint32 Epoch = SocketEpoch;
	const TWeakObjectPtr<UArenaNetSubsystem> WeakThis(this);

	// IWebSocket may fire these on a worker thread depending on the platform implementation:
	// never touch `this` here, only hop to the game thread with a weak pointer.
	Socket->OnConnected().AddWeakLambda(this, [WeakThis, Epoch]()
	{
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Epoch]()
		{
			UArenaNetSubsystem* Self = WeakThis.Get();
			if (Self && Self->SocketEpoch == Epoch)
			{
				Self->HandleConnected();
			}
		});
	});

	Socket->OnConnectionError().AddWeakLambda(this, [WeakThis, Epoch](const FString& Error)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Epoch, Error]()
		{
			UArenaNetSubsystem* Self = WeakThis.Get();
			if (Self && Self->SocketEpoch == Epoch)
			{
				Self->HandleSocketLost(FString::Printf(TEXT("connection error: %s"), *Error));
			}
		});
	});

	Socket->OnClosed().AddWeakLambda(this, [WeakThis, Epoch](int32 StatusCode, const FString& Reason, bool bWasClean)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Epoch, StatusCode, Reason, bWasClean]()
		{
			UArenaNetSubsystem* Self = WeakThis.Get();
			if (!Self || Self->SocketEpoch != Epoch)
			{
				return;
			}
			UE_LOG(LogLiveArena, Log, TEXT("[Net] Socket closed (code %d%s%s%s)"), StatusCode,
				bWasClean ? TEXT(", clean") : TEXT(""), Reason.IsEmpty() ? TEXT("") : TEXT(": "), *Reason);

			// Normally the {"t":"error"} frame before this close already stopped us (and unbound this socket, so this
			// callback is not reached). If that frame got lost, the close code still says it is final.
			if (StatusCode == ArenaNetDetail::CloseReplaced || StatusCode == ArenaNetDetail::CloseBadKey)
			{
				Self->StopAfterRefusal(StatusCode == ArenaNetDetail::CloseReplaced ? TEXT("replaced") : TEXT("bad key"),
					/*bSocketAlreadyClosed*/ true);
				return;
			}
			Self->HandleSocketLost(FString::Printf(TEXT("closed (code %d)"), StatusCode));
		});
	});

	Socket->OnMessage().AddWeakLambda(this, [WeakThis, Epoch](const FString& Message)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Epoch, Message]()
		{
			UArenaNetSubsystem* Self = WeakThis.Get();
			if (Self && Self->SocketEpoch == Epoch)
			{
				Self->HandleMessage(Message);
			}
		});
	});

	Socket->Connect();
}

void UArenaNetSubsystem::CloseSocket(bool bSendClose)
{
	// Anything the old socket already queued for the game thread becomes stale.
	++SocketEpoch;

	if (!Socket.IsValid())
	{
		return;
	}

	TSharedPtr<IWebSocket> Old = Socket;
	Socket.Reset();

	Old->OnConnected().RemoveAll(this);
	Old->OnConnectionError().RemoveAll(this);
	Old->OnClosed().RemoveAll(this);
	Old->OnMessage().RemoveAll(this);
	if (bSendClose)
	{
		// Close in every state: an attempt still connecting must not come alive later and kick the new host connection.
		Old->Close();
	}
}

void UArenaNetSubsystem::SetConnected(bool bNow)
{
	if (bConnected == bNow)
	{
		return;
	}
	bConnected = bNow;
	OnConnectionChanged.Broadcast(bConnected);
}

void UArenaNetSubsystem::HandleConnected()
{
	UE_LOG(LogLiveArena, Log, TEXT("[Net] Connected as host of room '%s'."), *RoomId);

	// ReconnectAttempt is NOT reset here: a link that is accepted and then dropped right away (e.g. a proxy or an
	// overloaded server closing it) must keep backing off instead of retrying every second.
	ConnectedSince = FPlatformTime::Seconds();
	ClearReconnectTimer();

	// Handshake first so layout/config reach the server before anything a listener sends on OnConnectionChanged.
	bConnected = true;
	SendHandshake();
	OnConnectionChanged.Broadcast(true);
}

void UArenaNetSubsystem::HandleSocketLost(const FString& Why)
{
	if (bWantConnected)
	{
		UE_LOG(LogLiveArena, Warning, TEXT("[Net] Server link lost: %s"), *Why);
	}
	else
	{
		UE_LOG(LogLiveArena, Log, TEXT("[Net] Server link %s"), *Why);
	}

	if (bConnected && FPlatformTime::Seconds() - ConnectedSince >= ArenaNetDetail::StableLinkSeconds)
	{
		ReconnectAttempt = 0;
	}

	CloseSocket(/*bSendClose*/ false); // already closed / failed
	SetConnected(false);
	ScheduleReconnect();
}

void UArenaNetSubsystem::ScheduleReconnect()
{
	if (!bWantConnected || BaseUrl.IsEmpty())
	{
		return;
	}

	UGameInstance* GameInstance = GetGameInstance();
	if (!GameInstance)
	{
		return;
	}

	FTimerManager& Timers = GameInstance->GetTimerManager();
	if (Timers.IsTimerActive(ReconnectTimer))
	{
		return;
	}

	const int32 Last = static_cast<int32>(UE_ARRAY_COUNT(ArenaNetDetail::ReconnectDelays)) - 1;
	const float Delay = ArenaNetDetail::ReconnectDelays[FMath::Clamp(ReconnectAttempt, 0, Last)];
	++ReconnectAttempt;

	UE_LOG(LogLiveArena, Log, TEXT("[Net] Reconnecting in %.0f s."), Delay);
	Timers.SetTimer(ReconnectTimer, this, &UArenaNetSubsystem::OpenSocket, Delay, false);
}

void UArenaNetSubsystem::ClearReconnectTimer()
{
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		GameInstance->GetTimerManager().ClearTimer(ReconnectTimer);
	}
	ReconnectTimer.Invalidate();
}

// ---------------------------------------------------------------------------------------------------------------------
// host -> server

void UArenaNetSubsystem::SendJson(const TSharedRef<FJsonObject>& Object)
{
	if (!Socket.IsValid() || !bConnected)
	{
		return;
	}
	Socket->Send(ArenaNetDetail::ToJsonString(Object)); // text frame, UTF-8
}

void UArenaNetSubsystem::SendHandshake()
{
	SendLayoutNow();
	SendConfigNow();

	if (bHasMode)
	{
		const TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("t"), TEXT("mode"));
		Msg->SetStringField(TEXT("mode"), ArenaNetDetail::ModeToString(LastMode));
		SendJson(Msg);
	}

	if (bGuestPending)
	{
		bGuestPending = false;
		const TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("t"), TEXT("guest"));
		Msg->SetStringField(TEXT("id"), PendingGuestId);
		SendJson(Msg);
	}
}

void UArenaNetSubsystem::SendLayoutNow()
{
	if (LayoutRows.Num() == 0)
	{
		return;
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(LayoutRows.Num());
	for (const int32 Count : LayoutRows)
	{
		// Whole doubles serialise without a fraction ("34"), so the server sees ints.
		Rows.Add(MakeShared<FJsonValueNumber>(static_cast<double>(Count)));
	}

	const TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("t"), TEXT("layout"));
	Msg->SetArrayField(TEXT("rows"), Rows);
	SendJson(Msg);
}

void UArenaNetSubsystem::SendConfigNow()
{
	if (!bHasConfig)
	{
		return;
	}

	const TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("t"), TEXT("config"));
	Msg->SetStringField(TEXT("streamer"), CfgStreamerName);
	Msg->SetStringField(TEXT("videoId"), CfgVideoId);
	Msg->SetStringField(TEXT("streamUrl"), CfgStreamUrl);
	Msg->SetStringField(TEXT("callUrl"), CfgGuestCallUrl);
	SendJson(Msg);
}

void UArenaNetSubsystem::SetLayout(const TArray<int32>& RowCounts)
{
	for (const int32 Count : RowCounts)
	{
		if (Count <= 0)
		{
			UE_LOG(LogLiveArena, Warning, TEXT("[Net] SetLayout: row with %d seats; seat labels may not match the server."), Count);
			break;
		}
	}

	LayoutRows = RowCounts;
	SendLayoutNow();
}

void UArenaNetSubsystem::SetConfig(const FString& StreamerName, const FString& VideoId, const FString& StreamUrl, const FString& GuestCallUrl)
{
	CfgStreamerName = StreamerName;
	CfgVideoId = VideoId.TrimStartAndEnd();
	CfgStreamUrl = ArenaNetDetail::WithWebScheme(StreamUrl);
	CfgGuestCallUrl = ArenaNetDetail::WithWebScheme(GuestCallUrl);
	ArenaNetDetail::WarnIfNotWeb(TEXT("StreamUrl"), CfgStreamUrl);
	ArenaNetDetail::WarnIfNotWeb(TEXT("GuestCallUrl"), CfgGuestCallUrl);
	bHasConfig = true;
	SendConfigNow();
}

void UArenaNetSubsystem::SendMode(EArenaMode Mode)
{
	LastMode = Mode;
	bHasMode = true;

	const TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("t"), TEXT("mode"));
	Msg->SetStringField(TEXT("mode"), ArenaNetDetail::ModeToString(Mode));
	SendJson(Msg);
}

void UArenaNetSubsystem::SendLotterySpin()
{
	const TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("t"), TEXT("lottery"));
	Msg->SetStringField(TEXT("phase"), TEXT("spin"));
	SendJson(Msg);
}

void UArenaNetSubsystem::SendLotteryWinner(const FString& ViewerId)
{
	const TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("t"), TEXT("lottery"));
	Msg->SetStringField(TEXT("phase"), TEXT("winner"));
	Msg->SetStringField(TEXT("id"), ViewerId);
	SendJson(Msg);
}

void UArenaNetSubsystem::SendGuest(const FString& ViewerIdOrEmpty)
{
	if (!IsConnected())
	{
		// The guest must still get the call link once the server is back.
		PendingGuestId = ViewerIdOrEmpty;
		bGuestPending = true;
		return;
	}

	bGuestPending = false;
	const TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("t"), TEXT("guest"));
	Msg->SetStringField(TEXT("id"), ViewerIdOrEmpty);
	SendJson(Msg);
}

// ---------------------------------------------------------------------------------------------------------------------
// server -> host

bool UArenaNetSubsystem::ParseViewer(const TSharedPtr<FJsonObject>& Object, FArenaViewer& Out)
{
	Out = FArenaViewer();
	if (!Object.IsValid())
	{
		return false;
	}
	if (!Object->TryGetStringField(TEXT("id"), Out.Id) || Out.Id.IsEmpty())
	{
		return false;
	}

	if (!Object->TryGetStringField(TEXT("name"), Out.Name) || Out.Name.IsEmpty())
	{
		Out.Name = TEXT("\u89C0\u773E"); // 觀眾 (server default; escaped so the source encoding never matters)
	}

	int32 Slot = INDEX_NONE;
	if (Object->TryGetNumberField(TEXT("slot"), Slot))
	{
		Out.Slot = Slot;
	}

	// Empty / missing label is passed through: the audience recomputes it from the venue.
	if (!Object->TryGetStringField(TEXT("label"), Out.Label))
	{
		Out.Label.Reset();
	}
	return true;
}

void UArenaNetSubsystem::HandleMessage(const FString& Message)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Message);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogLiveArena, Warning, TEXT("[Net] Ignoring non-JSON message: %s"), *Message.Left(200));
		return;
	}

	FString Type;
	if (!Root->TryGetStringField(TEXT("t"), Type))
	{
		UE_LOG(LogLiveArena, Warning, TEXT("[Net] Ignoring message without \"t\": %s"), *Message.Left(200));
		return;
	}

	if (Type == TEXT("clap"))
	{
		FString Id;
		if (Root->TryGetStringField(TEXT("id"), Id) && !Id.IsEmpty())
		{
			OnClap.Broadcast(Id);
		}
	}
	else if (Type == TEXT("join"))
	{
		const TSharedPtr<FJsonObject>* ViewerObject = nullptr;
		FArenaViewer Viewer;
		if (Root->TryGetObjectField(TEXT("viewer"), ViewerObject) && ViewerObject && ParseViewer(*ViewerObject, Viewer))
		{
			OnJoin.Broadcast(Viewer);
		}
		else
		{
			UE_LOG(LogLiveArena, Warning, TEXT("[Net] Bad join message: %s"), *Message.Left(200));
		}
	}
	else if (Type == TEXT("leave"))
	{
		FString Id;
		if (Root->TryGetStringField(TEXT("id"), Id) && !Id.IsEmpty())
		{
			OnLeave.Broadcast(Id);
		}
	}
	else if (Type == TEXT("count"))
	{
		int32 Count = -1;
		if (Root->TryGetNumberField(TEXT("youtube"), Count))
		{
			OnYouTubeCount.Broadcast(Count);
		}
	}
	else if (Type == TEXT("snapshot"))
	{
		TArray<FArenaViewer> Viewers;
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (Root->TryGetArrayField(TEXT("viewers"), Items) && Items)
		{
			Viewers.Reserve(Items->Num());
			for (const TSharedPtr<FJsonValue>& Item : *Items)
			{
				const TSharedPtr<FJsonObject>* ViewerObject = nullptr;
				FArenaViewer Viewer;
				if (Item.IsValid() && Item->TryGetObject(ViewerObject) && ViewerObject && ParseViewer(*ViewerObject, Viewer))
				{
					Viewers.Add(MoveTemp(Viewer));
				}
			}
		}

		int32 Count = -1;
		if (!Root->TryGetNumberField(TEXT("youtube"), Count))
		{
			Count = -1;
		}

		UE_LOG(LogLiveArena, Log, TEXT("[Net] Snapshot: %d named viewers, youtube %d."), Viewers.Num(), Count);
		OnSnapshot.Broadcast(Viewers);
		OnYouTubeCount.Broadcast(Count);
	}
	else if (Type == TEXT("error"))
	{
		FString ErrorMessage;
		if (!Root->TryGetStringField(TEXT("msg"), ErrorMessage))
		{
			ErrorMessage.Reset();
		}
		UE_LOG(LogLiveArena, Warning, TEXT("[Net] Server error: %s"), *ErrorMessage);

		// "replaced" always means another app took the room: our own reconnects unbind the old socket first
		// (SocketEpoch), so its late frames never get here. Retrying would only kick the other app in turn.
		if (ErrorMessage.Equals(TEXT("bad key"), ESearchCase::IgnoreCase) || ErrorMessage.Equals(TEXT("replaced"), ESearchCase::IgnoreCase))
		{
			StopAfterRefusal(ErrorMessage, /*bSocketAlreadyClosed*/ false);
			return;
		}
		OnError.Broadcast(ErrorMessage);
	}
	else
	{
		UE_LOG(LogLiveArena, Verbose, TEXT("[Net] Ignoring message type '%s'."), *Type);
	}
}

void UArenaNetSubsystem::StopAfterRefusal(const FString& ErrorMessage, bool bSocketAlreadyClosed)
{
	if (ErrorMessage.Equals(TEXT("replaced"), ESearchCase::IgnoreCase))
	{
		UE_LOG(LogLiveArena, Error, TEXT("[Net] Another host with the same key took room '%s'. Not reconnecting (restart to take it back)."), *RoomId);
	}
	else if (HostKey.IsEmpty() || HostKey.Len() > 128)
	{
		UE_LOG(LogLiveArena, Error, TEXT("[Net] The server refuses an empty HostKey or one longer than 128 characters. Not reconnecting."));
	}
	else
	{
		UE_LOG(LogLiveArena, Error, TEXT("[Net] HostKey rejected for room '%s'. Another host owns this room: change RoomId or use its key. Not reconnecting."), *RoomId);
	}

	// Retrying can never work; stop until Connect() is called again.
	bWantConnected = false;
	ClearReconnectTimer();

	OnError.Broadcast(ErrorMessage);

	// A listener may have called Connect() again from OnError; only drop the link if nobody did.
	if (!bWantConnected)
	{
		CloseSocket(/*bSendClose*/ !bSocketAlreadyClosed);
		SetConnected(false);
	}
}
