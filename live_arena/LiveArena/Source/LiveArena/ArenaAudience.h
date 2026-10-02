#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ArenaTypes.h"
#include "ArenaAudience.generated.h"

class AArenaVenue;
class UInstancedStaticMeshComponent;
class UWidgetComponent;
class UStaticMesh;

/**
 * Everyone in the seats.
 *  - Named viewers (entered through the join link) sit in their server-assigned slot, warm-ivory figures, with a
 *    floating name label for the closest MaxLabels of them.
 *  - Anonymous crowd = people watching on the platform who did not enter (YouTube concurrent viewers minus named).
 *    They fill the slots after the highest named slot, darker figures, capped by MaxFigures.
 *  - Clap: that viewer's figure jumps and raises a glow stick for ~0.8 s.
 *  - Hype (0..1): whole-crowd bobbing and glow sticks, driven by the show director.
 * Instances are rebuilt at most once per tick (dirty flag); animation updates transforms in batches.
 */
UCLASS()
class LIVEARENA_API AArenaAudience : public AActor
{
	GENERATED_BODY()

public:
	AArenaAudience();

	virtual void Tick(float DeltaSeconds) override;

	void Init(AArenaVenue* InVenue, int32 InMaxFigures);

	void SetSnapshot(const TArray<FArenaViewer>& Viewers);
	void AddViewer(const FArenaViewer& Viewer);   // label is always recomputed from the venue once Init has run
	void RemoveViewer(const FString& Id);
	/** Total people watching on the platform (-1 = unknown -> crowd = named viewers only). */
	void SetCrowdCount(int32 TotalWatching);
	void Clap(const FString& Id);
	void ClapRandom(int32 Count);                 // demo helper
	void SetHype(float InHype);                   // 0 = calm, 1 = full concert

	bool GetViewer(const FString& Id, FArenaViewer& Out) const;
	bool PickRandomNamed(FArenaViewer& Out, const FString& ExcludeId = FString()) const;
	TArray<FString> GetNamedIds() const;
	int32 GetNamedCount() const { return Named.Num(); }
	int32 GetTotalWatching() const { return TotalWatching; }
	int32 GetDisplayedFigures() const { return DisplayedFigures; }

	/** Current head location of a named viewer's figure (follows them while walking / on stage). */
	bool GetFigureHeadLocation(const FString& Id, FVector& Out) const;

	/** Lottery winner walks along Path (world space) and sits at ChairTransform. Duration ~ path length / 160 cm/s. */
	void SendToStage(const FString& Id, const TArray<FVector>& Path, const FTransform& ChairTransform);
	/** Teleports the guest back to their seat. */
	void ReturnFromStage(const FString& Id);
	bool IsWalking() const { return WalkId.Len() > 0 && WalkT < 1.f; }

	UPROPERTY(EditAnywhere, Category = "Audience")
	int32 MaxLabels = 60;

	/** Optional seated-person mesh (pivot at the seat floor point, facing +X). Replaces the capsule blockout. */
	UPROPERTY(EditAnywhere, Category = "Audience")
	TObjectPtr<UStaticMesh> FigureMesh;

private:
	struct FFigure
	{
		FTransform Base;        // seated transform (world)
		float JumpT = -1.f;     // >= 0 while a clap jump plays (seconds since start)
		float Phase = 0.f;      // random phase for bobbing
		bool bNamed = false;
		int32 Slot = INDEX_NONE; // venue slot; stable key across rebuilds
		FString Id;              // named viewers only
	};

	void MarkDirty() { bDirty = true; }
	void Rebuild();
	void UpdateLabels();
	void Animate(float DeltaSeconds);
	FTransform FigureTransform(const FFigure& F, float Time, float ExtraZ) const;

	// ---- helpers (ArenaAudience.cpp) ----
	FFigure MakeFigure(int32 Slot, bool bNamed) const;
	void SetupVisuals();
	void UpdateWalk(float DeltaSeconds);
	void FinishWalk();
	int32 WalkSegment(float Distance) const;
	/** Standing root (ground point, facing the walk direction) of the walking guest. */
	FTransform WalkRoot() const;
	/** Root transform of a figure right now (seat + jump/bob, walk position, or guest chair). */
	FTransform NamedRoot(int32 Index, bool& bOutStanding) const;
	FTransform FigureRoot(bool bNamedGroup, int32 Index, bool& bOutStanding) const;
	/** Writes the given figures' instances; returns true if anything was written. */
	bool UpdateFigures(bool bNamedGroup, const TArray<int32>& Indices);
	void PushFigures(bool bNamedGroup, int32 First, const TArray<FTransform>& Roots, const TArray<bool>& Standing);
	void MarkFiguresRenderDirty(bool bNamedGroup);
	void UpdateGlowSticks(bool bCrowdFrame);
	UWidgetComponent* CreateLabel(const FVector& Location);
	void RemoveLabel(const FString& Id);

	TMap<int32, FString> SlotOwner;          // slot -> named viewer id
	TMap<FString, FString> LabelSignature;   // id -> "name|seat|highlight" last pushed to the widget
	TArray<float> WalkCumLength;             // cumulative length at each WalkPath point
	float WalkLength = 0.f;
	float WalkYaw = 0.f;
	float LabelTimer = 0.f;
	int32 AnimFrame = 0;
	bool bWasBobbing = false;
	bool bNamedPoseDirty = false;
	bool bVisualsReady = false;

	UPROPERTY(Transient)
	TObjectPtr<AArenaVenue> Venue;

	UPROPERTY(VisibleAnywhere, Category = "Audience")
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere, Category = "Audience")
	TObjectPtr<UInstancedStaticMeshComponent> NamedBodies;
	UPROPERTY(VisibleAnywhere, Category = "Audience")
	TObjectPtr<UInstancedStaticMeshComponent> NamedHeads;
	UPROPERTY(VisibleAnywhere, Category = "Audience")
	TObjectPtr<UInstancedStaticMeshComponent> CrowdBodies;
	UPROPERTY(VisibleAnywhere, Category = "Audience")
	TObjectPtr<UInstancedStaticMeshComponent> CrowdHeads;
	UPROPERTY(VisibleAnywhere, Category = "Audience")
	TObjectPtr<UInstancedStaticMeshComponent> GlowSticks;
	UPROPERTY(VisibleAnywhere, Category = "Audience")
	TObjectPtr<UInstancedStaticMeshComponent> CustomFigures; // used when FigureMesh is set

	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UWidgetComponent>> Labels;

	TMap<FString, FArenaViewer> Named;
	TMap<FString, int32> NamedFigureIndex;  // id -> index into NamedFigures
	TArray<FFigure> NamedFigures;
	TArray<FFigure> CrowdFigures;

	int32 MaxFigures = 2500;
	int32 TotalWatching = -1;
	int32 DisplayedFigures = 0;
	float Hype = 0.f;
	float Time = 0.f;
	bool bDirty = true;

	// walking guest
	FString WalkId;
	TArray<FVector> WalkPath;
	FTransform WalkChair;
	float WalkT = 1.f;
	float WalkDuration = 1.f;
	FString OnStageId;
};
