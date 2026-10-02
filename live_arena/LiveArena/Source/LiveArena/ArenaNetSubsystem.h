#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "TimerManager.h"
#include "ArenaTypes.h"
#include "ArenaNetSubsystem.generated.h"

class IWebSocket;

DECLARE_MULTICAST_DELEGATE_OneParam(FOnArenaSnapshot, const TArray<FArenaViewer>& /*Viewers*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnArenaViewer, const FArenaViewer& /*Viewer*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnArenaId, const FString& /*ViewerId*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnArenaCount, int32 /*Count, -1 = unknown*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnArenaConnection, bool /*bConnected*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnArenaError, const FString& /*Message*/);

/**
 * WebSocket client to live_arena/server/server.py (protocol: live_arena/PROTOCOL.md).
 * All delegates fire on the game thread. Reconnects automatically (1s, 2s, 4s ... max 15s), except after the server
 * refuses the host for good (error "bad key" or "replaced"; OnError carries the message).
 */
UCLASS()
class LIVEARENA_API UArenaNetSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Url like ws://localhost:8787/ws ; the subsystem appends ?role=host&room=<Room>&key=<Key>. */
	void Connect(const FString& Url, const FString& Room, const FString& Key);
	void Disconnect();
	bool IsConnected() const;

	/** Sent automatically after every (re)connect once set: */
	void SetLayout(const TArray<int32>& RowCounts);
	void SetConfig(const FString& StreamerName, const FString& VideoId, const FString& StreamUrl, const FString& GuestCallUrl);

	void SendMode(EArenaMode Mode);
	void SendLotterySpin();
	void SendLotteryWinner(const FString& ViewerId);
	/** Empty id = guest left the stage. */
	void SendGuest(const FString& ViewerIdOrEmpty);

	FOnArenaSnapshot OnSnapshot;
	FOnArenaViewer OnJoin;
	FOnArenaId OnLeave;
	FOnArenaId OnClap;
	FOnArenaCount OnYouTubeCount;
	FOnArenaConnection OnConnectionChanged;
	FOnArenaError OnError;

private:
	void OpenSocket();
	void ScheduleReconnect();
	void SendJson(const TSharedRef<class FJsonObject>& Object);
	void SendHandshake();
	void HandleMessage(const FString& Message);
	static bool ParseViewer(const TSharedPtr<class FJsonObject>& Object, FArenaViewer& Out);

	// --- private helpers (net-only; public interface unchanged) ---
	void HandleConnected();
	void HandleSocketLost(const FString& Why);
	/** Unbinds the current socket (if any), optionally closes it, and invalidates its pending game-thread callbacks. */
	void CloseSocket(bool bSendClose = true);
	void SetConnected(bool bNow);
	void ClearReconnectTimer();
	void SendLayoutNow();
	void SendConfigNow();
	FString BuildUrl(bool bForLog) const;
	/**
	 * The server refused this host for good ("bad key", or "replaced" = another app with the same key took the room):
	 * stop reconnecting, tell listeners through OnError, then drop the link unless a listener called Connect() again.
	 */
	void StopAfterRefusal(const FString& ErrorMessage, bool bSocketAlreadyClosed);

	/** Bumped whenever a socket is dropped; callbacks queued by an older socket are ignored. */
	uint32 SocketEpoch = 0;

	/** FPlatformTime::Seconds() of the last successful connect; the backoff only resets after a stable link. */
	double ConnectedSince = 0.0;

	/** Last mode sent (re-sent after every reconnect so a restarted server catches up). */
	EArenaMode LastMode = EArenaMode::Watch;
	bool bHasMode = false;

	/** A guest change made while offline is delivered on the next connect. */
	FString PendingGuestId;
	bool bGuestPending = false;

	TSharedPtr<IWebSocket> Socket;
	FString BaseUrl;
	FString RoomId;
	FString HostKey;
	bool bWantConnected = false;
	bool bConnected = false;
	int32 ReconnectAttempt = 0;
	FTimerHandle ReconnectTimer;

	TArray<int32> LayoutRows;
	FString CfgStreamerName;
	FString CfgVideoId;
	FString CfgStreamUrl;
	FString CfgGuestCallUrl;
	bool bHasConfig = false;
};
