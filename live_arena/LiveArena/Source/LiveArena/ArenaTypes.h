#pragma once

#include "CoreMinimal.h"
#include "ArenaTypes.generated.h"

/** Whether the streamer is just streaming (Watch) or running the interactive show (Interactive). */
UENUM(BlueprintType)
enum class EArenaMode : uint8
{
	Watch,
	Interactive
};

/** Lottery / guest state machine owned by AArenaShowDirector. */
UENUM(BlueprintType)
enum class EArenaShowState : uint8
{
	Idle,      // nothing happening
	Spinning,  // follow spot hopping between seats
	Landed,    // spot locked on the winner, waiting for the streamer to invite them
	Walking,   // winner walking from their seat to the guest chair
	Guest      // winner sitting on stage next to the streamer
};

/** What the big LED screen behind the stage shows. */
UENUM(BlueprintType)
enum class EArenaScreenSource : uint8
{
	Auto,          // capture device if one matches CaptureDeviceName, otherwise Url, otherwise placeholder
	CaptureDevice, // e.g. "OBS Virtual Camera" — zero delay, recommended
	Url,           // a web page (YouTube / Twitch / Kick embed) — has platform delay
	None           // placeholder text only
};

/** Camera presets. */
UENUM(BlueprintType)
enum class EArenaCamera : uint8
{
	Broadcast,   // from the middle of the audience looking at stage + LED screen (what viewers see)
	StreamerPOV, // from the stage looking out at the crowd (the "pressure" view for the streamer)
	Wide,        // high wide shot of the whole arena
	Follow,      // follows the lottery winner / guest
	Free         // fly with WASD + mouse (handled by the player controller)
};

/** A viewer who entered through the join link and holds a named seat. */
USTRUCT(BlueprintType)
struct FArenaViewer
{
	GENERATED_BODY()

	/** Stable id from the server (persisted in the viewer's browser). */
	UPROPERTY(BlueprintReadOnly, Category = "Arena")
	FString Id;

	/** Display name typed on the ticket (max 20 chars, already sanitised by the server). */
	UPROPERTY(BlueprintReadOnly, Category = "Arena")
	FString Name;

	/** Seat slot index assigned by the server: 0 = front row centre, see ArenaSlots below. */
	UPROPERTY(BlueprintReadOnly, Category = "Arena")
	int32 Slot = INDEX_NONE;

	/** Human seat label such as "A12" (server and client compute it the same way). */
	UPROPERTY(BlueprintReadOnly, Category = "Arena")
	FString Label;
};

/**
 * Seat slot formula. MUST stay identical to server/server.py (slot_label / center_out).
 *
 * Rows are numbered from the front (row 0 = closest to stage). Each row r has RowCounts[r] seats,
 * positions 0..n-1 from the audience's left to right (left = -Y when facing the stage at +X).
 * Slots fill row by row; inside a row they fill from the centre outward:
 *   mid = n / 2 (integer division)
 *   j-th slot of the row -> position = (j % 2 == 0) ? mid + j / 2 : mid - (j + 1) / 2
 * e.g. n = 4 -> positions 2,1,3,0 ; n = 5 -> 2,1,3,0,4
 * Label = RowName(r) + (position + 1), RowName: 0..25 -> "A".."Z", 26 -> "AA", 27 -> "AB", ...
 */
namespace ArenaSlots
{
	inline int32 CenterOutPosition(int32 J, int32 N)
	{
		const int32 Mid = N / 2;
		return (J % 2 == 0) ? Mid + J / 2 : Mid - (J + 1) / 2;
	}

	inline FString RowName(int32 Row)
	{
		if (Row < 26)
		{
			return FString::Chr(TCHAR('A' + Row));
		}
		return FString::Chr(TCHAR('A' + (Row / 26 - 1))) + FString::Chr(TCHAR('A' + (Row % 26)));
	}
}
