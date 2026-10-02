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
 * All delegates fire on the game thread. Reconnects automatically (1s, 2s, 4s ... max 15s).
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
