#include "ArenaAudience.h"

#include "ArenaVenue.h"
#include "ArenaWidgets.h"
#include "LiveArena.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/WidgetComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Math/RandomStream.h"

// 觀眾全部用 ISM 畫：具名觀眾（象牙色）同無名人潮（暗色）各一套「身 + 頭」，另外一套螢光棒。
// 每個 figure 有一個 root transform（座位地面點、面向舞台），身 / 頭 / 螢光棒都係 root 上面嘅 local offset。
// Instance 次序 = NamedFigures / CrowdFigures 次序，所以動畫可以用 BatchUpdateInstancesTransforms 一嚿過寫。

// Named (not anonymous) namespace: unity builds paste several .cpp files into one translation unit.
namespace ArenaAudienceDetail
{
	constexpr float JumpDuration = 0.8f;      // clap jump
	constexpr float JumpHeight = 35.f;
	constexpr float BobHz = 1.6f;             // hype bobbing
	constexpr float BobAmplitude = 8.f;
	constexpr float BobMinHype = 0.01f;
	constexpr float CrowdStickHype = 0.3f;    // above this every 3rd crowd figure raises a glow stick
	constexpr float WalkSpeed = 160.f;        // cm/s
	constexpr float WalkBob = 3.f;
	constexpr float WalkStepHz = 1.8f;
	constexpr float WalkTurnRate = 540.f;     // deg/s
	constexpr float SitTurnDistance = 90.f;   // last stretch of the walk: face the chair's way and back into it
	constexpr float LabelInterval = 0.5f;
	constexpr float LabelAboveHead = 25.f;
	constexpr int32 HalfRateCrowd = 1500;     // bigger crowds bob every second tick

	/** sRGB hex ("CDBB9E") -> linear colour for the material parameter. */
	static FLinearColor Hex(const TCHAR* InHex)
	{
		return FLinearColor(FColor::FromHex(InHex));
	}

	/** Cylinder body relative to the figure root. Seated: 45 x 70 cm centred 75 cm up. Standing: taller, centred 95 cm up. */
	static FTransform BodyLocal(bool bStanding)
	{
		return bStanding
			? FTransform(FQuat::Identity, FVector(0.f, 0.f, 95.f), FVector(0.45f, 0.45f, 1.2f))
			: FTransform(FQuat::Identity, FVector(0.f, 0.f, 75.f), FVector(0.45f, 0.45f, 0.70f));
	}

	/** 22 cm sphere head. */
	static FTransform HeadLocal(bool bStanding)
	{
		return FTransform(FQuat::Identity, FVector(0.f, 0.f, bStanding ? 165.f : 122.f), FVector(0.22f, 0.22f, 0.22f));
	}

	/** Thin 30 cm glow stick held up beside the head, tilted outward (positive roll tips +Z toward +Y). */
	static FTransform StickLocal(float Side, float SwayDeg)
	{
		return FTransform(FRotator(0.f, 0.f, Side * (18.f + SwayDeg)), FVector(6.f, Side * 20.f, 150.f), FVector(0.035f, 0.035f, 0.30f));
	}
}

AArenaAudience::AArenaAudience()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	RootComponent = Root;

	// Meshes and tinted materials are assigned at runtime in SetupVisuals().
	auto MakeLayer = [this](const TCHAR* LayerName, bool bShadow)
	{
		UInstancedStaticMeshComponent* Ism = CreateDefaultSubobject<UInstancedStaticMeshComponent>(LayerName);
		Ism->SetupAttachment(Root);
		Ism->SetMobility(EComponentMobility::Movable);
		Ism->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		Ism->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Ism->SetGenerateOverlapEvents(false);
		Ism->SetCanEverAffectNavigation(false);
		Ism->SetCastShadow(bShadow);
		// These instances move every frame while the crowd bobs: keep them out of the global distance field and the
		// Lumen scene, which would otherwise be rebuilt for the whole bowl each frame (small figures add little GI).
		Ism->bAffectDistanceFieldLighting = false;
		Ism->bAffectDynamicIndirectLighting = false;
		return Ism;
	};
	NamedBodies = MakeLayer(TEXT("NamedBodies"), true);
	NamedHeads = MakeLayer(TEXT("NamedHeads"), true);
	CrowdBodies = MakeLayer(TEXT("CrowdBodies"), true);
	CrowdHeads = MakeLayer(TEXT("CrowdHeads"), true);
	GlowSticks = MakeLayer(TEXT("GlowSticks"), false);
	CustomFigures = MakeLayer(TEXT("CustomFigures"), true);
}

void AArenaAudience::SetupVisuals()
{
	using namespace ArenaAudienceDetail;

	if (bVisualsReady)
	{
		return;
	}
	bVisualsReady = true;

	UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	UMaterialInterface* BaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (!Cylinder || !Sphere || !BaseMaterial)
	{
		UE_LOG(LogLiveArena, Error, TEXT("Audience: engine basic shapes / BasicShapeMaterial not found, figures may be invisible."));
	}

	// MIDs are referenced by the components' override materials, which keeps them alive.
	auto Paint = [this, BaseMaterial](UInstancedStaticMeshComponent* Ism, UStaticMesh* Mesh, const TCHAR* Color)
	{
		if (!Ism)
		{
			return;
		}
		if (Mesh)
		{
			Ism->SetStaticMesh(Mesh);
		}
		if (BaseMaterial)
		{
			UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(BaseMaterial, this);
			Mid->SetVectorParameterValue(TEXT("Color"), Hex(Color));
			Ism->SetMaterial(0, Mid);
		}
	};
	Paint(NamedBodies, Cylinder, TEXT("CDBB9E"));
	Paint(NamedHeads, Sphere, TEXT("CDBB9E"));
	// Dark warm grey: still a silhouette next to the ivory named viewers, but readable under dim house lights.
	Paint(CrowdBodies, Cylinder, TEXT("4A4541"));
	Paint(CrowdHeads, Sphere, TEXT("4A4541"));
	Paint(GlowSticks, Cylinder, TEXT("FFB86B"));
}

// ------------------------------------------------------------------------------------------------
// Viewers
// ------------------------------------------------------------------------------------------------

void AArenaAudience::Init(AArenaVenue* InVenue, int32 InMaxFigures)
{
	Venue = InVenue;
	MaxFigures = FMath::Max(0, InMaxFigures);
	SetupVisuals();

	// Anyone added before Init is re-validated against the venue (slot range, missing labels).
	if (IsValid(Venue) && Named.Num() > 0)
	{
		TArray<FArenaViewer> Existing;
		Named.GenerateValueArray(Existing);
		SetSnapshot(Existing);
	}
	MarkDirty();
}

void AArenaAudience::SetSnapshot(const TArray<FArenaViewer>& Viewers)
{
	Named.Reset();
	SlotOwner.Reset();
	for (const FArenaViewer& Viewer : Viewers)
	{
		AddViewer(Viewer);
	}

	if (!WalkId.IsEmpty() && !Named.Contains(WalkId))
	{
		WalkId.Reset();
		WalkT = 1.f;
	}
	if (!OnStageId.IsEmpty() && !Named.Contains(OnStageId))
	{
		OnStageId.Reset();
	}
	bNamedPoseDirty = true;
	MarkDirty();
}

void AArenaAudience::AddViewer(const FArenaViewer& Viewer)
{
	if (Viewer.Id.IsEmpty())
	{
		return;
	}

	FArenaViewer Entry = Viewer;
	if (IsValid(Venue))
	{
		if (Entry.Slot < 0 || Entry.Slot >= Venue->GetNumSlots())
		{
			UE_LOG(LogLiveArena, Warning, TEXT("Audience: viewer %s has slot %d but the venue has %d seats; not shown."),
				*Entry.Id, Entry.Slot, Venue->GetNumSlots());
			return;
		}
		// The venue is where the layout the server uses comes from, so its label is the current one. The server's
		// label can be stale: the snapshot it sends on connect is computed before it receives this app's layout.
		const FString VenueLabel = Venue->GetSlotLabel(Entry.Slot);
		if (!Entry.Label.IsEmpty() && Entry.Label != VenueLabel)
		{
			UE_LOG(LogLiveArena, Verbose, TEXT("Audience: %s label %s -> %s (venue layout)"), *Entry.Id, *Entry.Label, *VenueLabel);
		}
		Entry.Label = VenueLabel;
	}

	// Same id moving seat: release the old slot.
	if (const FArenaViewer* Existing = Named.Find(Entry.Id))
	{
		if (SlotOwner.FindRef(Existing->Slot) == Entry.Id)
		{
			SlotOwner.Remove(Existing->Slot);
		}
	}

	// Somebody else (a stale id) still holds this slot: they are replaced.
	const FString PreviousHolder = SlotOwner.FindRef(Entry.Slot);
	if (!PreviousHolder.IsEmpty() && PreviousHolder != Entry.Id)
	{
		RemoveViewer(PreviousHolder);
	}

	Named.Add(Entry.Id, Entry);
	SlotOwner.Add(Entry.Slot, Entry.Id);
	MarkDirty();
}

void AArenaAudience::RemoveViewer(const FString& Id)
{
	FArenaViewer Removed;
	if (!Named.RemoveAndCopyValue(Id, Removed))
	{
		return;
	}
	if (SlotOwner.FindRef(Removed.Slot) == Id)
	{
		SlotOwner.Remove(Removed.Slot);
	}
	if (Id == WalkId)
	{
		WalkId.Reset();
		WalkT = 1.f;
	}
	if (Id == OnStageId)
	{
		OnStageId.Reset();
	}
	RemoveLabel(Id);
	MarkDirty();
}

void AArenaAudience::SetCrowdCount(int32 InTotalWatching)
{
	if (InTotalWatching == TotalWatching)
	{
		return;
	}
	TotalWatching = InTotalWatching;
	if (bDirty || !IsValid(Venue))
	{
		MarkDirty();
		return;
	}

	// A full rebuild only when the number of anonymous figures drawn changes (same rule as Rebuild): the count wobbles
	// every few seconds, and once it is above what fits it changes nothing on screen.
	const int32 Wanted = TotalWatching < 0 ? 0 : FMath::Max(0, TotalWatching - Named.Num());
	const int32 Room = FMath::Max(0, FMath::Max(0, MaxFigures) - NamedFigures.Num());
	const int32 FreeSeats = FMath::Max(0, Venue->GetNumSlots() - SlotOwner.Num());
	if (FMath::Min3(Wanted, Room, FreeSeats) != CrowdFigures.Num())
	{
		MarkDirty();
	}
}

void AArenaAudience::Clap(const FString& Id)
{
	const int32* Index = NamedFigureIndex.Find(Id);
	if (!Index || !NamedFigures.IsValidIndex(*Index))
	{
		return;
	}
	FFigure& Figure = NamedFigures[*Index];
	if (Figure.Id != WalkId && Figure.Id != OnStageId)
	{
		Figure.JumpT = 0.f;   // restart even if already jumping
	}
}

void AArenaAudience::ClapRandom(int32 Count)
{
	const int32 Total = NamedFigures.Num() + CrowdFigures.Num();
	if (Total == 0)
	{
		return;
	}
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const int32 Pick = FMath::RandRange(0, Total - 1);
		FFigure& Figure = Pick < NamedFigures.Num() ? NamedFigures[Pick] : CrowdFigures[Pick - NamedFigures.Num()];
		if (Figure.bNamed && (Figure.Id == WalkId || Figure.Id == OnStageId))
		{
			continue;
		}
		Figure.JumpT = 0.f;
	}
}

void AArenaAudience::SetHype(float InHype)
{
	Hype = FMath::Clamp(InHype, 0.f, 1.f);
}

bool AArenaAudience::GetViewer(const FString& Id, FArenaViewer& Out) const
{
	if (const FArenaViewer* Found = Named.Find(Id))
	{
		Out = *Found;
		return true;
	}
	return false;
}

bool AArenaAudience::PickRandomNamed(FArenaViewer& Out, const FString& ExcludeId) const
{
	TArray<const FArenaViewer*> Candidates;
	Candidates.Reserve(Named.Num());
	for (const TPair<FString, FArenaViewer>& Pair : Named)
	{
		if (Pair.Key != ExcludeId)
		{
			Candidates.Add(&Pair.Value);
		}
	}
	if (Candidates.Num() == 0)
	{
		return false;
	}
	Out = *Candidates[FMath::RandRange(0, Candidates.Num() - 1)];
	return true;
}

TArray<FString> AArenaAudience::GetNamedIds() const
{
	TArray<FArenaViewer> Sorted;
	Named.GenerateValueArray(Sorted);
	Sorted.Sort([](const FArenaViewer& A, const FArenaViewer& B) { return A.Slot < B.Slot; });

	TArray<FString> Ids;
	Ids.Reserve(Sorted.Num());
	for (const FArenaViewer& Viewer : Sorted)
	{
		Ids.Add(Viewer.Id);
	}
	return Ids;
}

// ------------------------------------------------------------------------------------------------
// Guest on stage
// ------------------------------------------------------------------------------------------------

void AArenaAudience::SendToStage(const FString& Id, const TArray<FVector>& Path, const FTransform& ChairTransform)
{
	using namespace ArenaAudienceDetail;

	if (!Named.Contains(Id))
	{
		UE_LOG(LogLiveArena, Warning, TEXT("Audience: SendToStage(%s) ignored, not a seated viewer."), *Id);
		return;
	}

	// Only one guest at a time: whoever was on stage goes back to their seat.
	OnStageId.Reset();
	WalkId = Id;
	WalkPath = Path;
	WalkChair = ChairTransform;
	WalkChair.SetScale3D(FVector::OneVector);

	WalkCumLength.Reset();
	WalkLength = 0.f;
	for (int32 Index = 0; Index < WalkPath.Num(); ++Index)
	{
		if (Index > 0)
		{
			WalkLength += static_cast<float>(FVector::Dist(WalkPath[Index - 1], WalkPath[Index]));
		}
		WalkCumLength.Add(WalkLength);
	}

	// Initial facing: first leg of the path, else the seat's own direction.
	WalkYaw = static_cast<float>(WalkChair.Rotator().Yaw);
	if (const int32* FigureIndex = NamedFigureIndex.Find(Id))
	{
		if (NamedFigures.IsValidIndex(*FigureIndex))
		{
			WalkYaw = static_cast<float>(NamedFigures[*FigureIndex].Base.Rotator().Yaw);
		}
	}
	if (WalkPath.Num() >= 2)
	{
		const FVector FirstLeg = WalkPath[1] - WalkPath[0];
		if (FirstLeg.SizeSquared2D() > 1.0)
		{
			WalkYaw = static_cast<float>(FirstLeg.Rotation().Yaw);
		}
	}

	if (WalkPath.Num() < 2 || WalkLength < 1.f)
	{
		FinishWalk();   // no usable path: straight to the chair
	}
	else
	{
		WalkT = 0.f;
		WalkDuration = FMath::Max(WalkLength / WalkSpeed, 0.5f);
		bNamedPoseDirty = true;
		LabelTimer = LabelInterval;   // highlight their tag on the next tick
	}

	if (!NamedFigureIndex.Contains(Id))
	{
		MarkDirty();   // capped out by MaxFigures: the rebuild always includes the guest
	}
}

void AArenaAudience::ReturnFromStage(const FString& Id)
{
	using namespace ArenaAudienceDetail;

	if (Id.IsEmpty())
	{
		return;
	}
	bool bChanged = false;
	if (Id == WalkId)
	{
		WalkId.Reset();
		WalkT = 1.f;
		bChanged = true;
	}
	if (Id == OnStageId)
	{
		OnStageId.Reset();
		bChanged = true;
	}
	if (bChanged)
	{
		bNamedPoseDirty = true;
		LabelTimer = LabelInterval;
	}
}

void AArenaAudience::UpdateWalk(float DeltaSeconds)
{
	using namespace ArenaAudienceDetail;

	if (WalkId.IsEmpty() || WalkT >= 1.f)
	{
		return;
	}

	WalkT = FMath::Min(1.f, WalkT + DeltaSeconds / FMath::Max(WalkDuration, 0.01f));

	// Turn smoothly toward the current leg (vertical step-downs keep the previous heading). Near the end the figure
	// turns to the chair's facing instead: the venue path ends with a short leg backing into the guest chair, which
	// would otherwise be walked facing the chair back and then snap 180 degrees on sitting down.
	if (WalkPath.Num() >= 2)
	{
		const float Travelled = WalkT * WalkLength;
		const int32 Seg = WalkSegment(Travelled);
		const FVector Leg = WalkPath[Seg + 1] - WalkPath[Seg];
		if (WalkLength - Travelled <= SitTurnDistance)
		{
			WalkYaw = FMath::FixedTurn(WalkYaw, static_cast<float>(WalkChair.Rotator().Yaw), WalkTurnRate * DeltaSeconds);
		}
		else if (Leg.SizeSquared2D() > 1.0)
		{
			WalkYaw = FMath::FixedTurn(WalkYaw, static_cast<float>(Leg.Rotation().Yaw), WalkTurnRate * DeltaSeconds);
		}
	}

	if (WalkT >= 1.f)
	{
		FinishWalk();
	}
}

void AArenaAudience::FinishWalk()
{
	using namespace ArenaAudienceDetail;

	WalkT = 1.f;
	if (!WalkId.IsEmpty())
	{
		OnStageId = WalkId;
	}
	WalkId.Reset();
	bNamedPoseDirty = true;
	LabelTimer = LabelInterval;
}

int32 AArenaAudience::WalkSegment(float Distance) const
{
	int32 Seg = 0;
	const int32 LastSeg = WalkCumLength.Num() - 2;
	while (Seg < LastSeg && WalkCumLength[Seg + 1] < Distance)
	{
		++Seg;
	}
	return FMath::Max(Seg, 0);
}

FTransform AArenaAudience::WalkRoot() const
{
	using namespace ArenaAudienceDetail;

	FVector Location = WalkChair.GetLocation();
	if (WalkPath.Num() == 1)
	{
		Location = WalkPath[0];
	}
	else if (WalkPath.Num() >= 2 && WalkCumLength.Num() == WalkPath.Num())
	{
		const float Distance = FMath::Clamp(WalkT, 0.f, 1.f) * WalkLength;
		const int32 Seg = WalkSegment(Distance);
		const float SegLength = WalkCumLength[Seg + 1] - WalkCumLength[Seg];
		const float Alpha = SegLength > UE_KINDA_SMALL_NUMBER ? FMath::Clamp((Distance - WalkCumLength[Seg]) / SegLength, 0.f, 1.f) : 1.f;
		Location = FMath::Lerp(WalkPath[Seg], WalkPath[Seg + 1], Alpha);
	}
	Location.Z += FMath::Abs(FMath::Sin(Time * 2.f * UE_PI * WalkStepHz)) * WalkBob;
	return FTransform(FRotator(0.f, WalkYaw, 0.f), Location);
}

// ------------------------------------------------------------------------------------------------
// Figures
// ------------------------------------------------------------------------------------------------

AArenaAudience::FFigure AArenaAudience::MakeFigure(int32 Slot, bool bNamed) const
{
	FFigure Figure;
	Figure.Slot = Slot;
	Figure.bNamed = bNamed;

	// Per-seat variation seeded by the slot, so nobody changes shape when the crowd is rebuilt.
	FRandomStream Rng(Slot * 7919 + 1013);
	Figure.Phase = Rng.FRandRange(0.f, 2.2f);   // mostly in phase: a crowd bouncing to the same beat
	const float Scale = Rng.FRandRange(0.93f, 1.05f);
	const float YawJitter = Rng.FRandRange(-7.f, 7.f);

	FTransform Base = FTransform(FRotator(0.f, YawJitter, 0.f)) * Venue->GetSlotTransform(Slot);
	Base.SetScale3D(FVector(Scale, Scale, Scale));
	Figure.Base = Base;
	return Figure;
}

FTransform AArenaAudience::FigureTransform(const FFigure& F, float AtTime, float ExtraZ) const
{
	using namespace ArenaAudienceDetail;

	float Z = ExtraZ;
	if (F.JumpT >= 0.f)
	{
		Z += FMath::Sin(UE_PI * FMath::Clamp(F.JumpT / JumpDuration, 0.f, 1.f)) * JumpHeight;
	}
	if (Hype > BobMinHype)
	{
		Z += FMath::Sin(AtTime * 2.f * UE_PI * BobHz + F.Phase) * BobAmplitude * Hype;
	}
	FTransform Result = F.Base;
	Result.AddToTranslation(FVector(0.f, 0.f, Z));
	return Result;
}

FTransform AArenaAudience::NamedRoot(int32 Index, bool& bOutStanding) const
{
	const FFigure& Figure = NamedFigures[Index];
	if (!WalkId.IsEmpty() && WalkT < 1.f && Figure.Id == WalkId)
	{
		bOutStanding = true;
		return WalkRoot();
	}
	bOutStanding = false;
	if (!OnStageId.IsEmpty() && Figure.Id == OnStageId)
	{
		return WalkChair;
	}
	return FigureTransform(Figure, Time, 0.f);
}

FTransform AArenaAudience::FigureRoot(bool bNamedGroup, int32 Index, bool& bOutStanding) const
{
	if (bNamedGroup)
	{
		return NamedRoot(Index, bOutStanding);
	}
	bOutStanding = false;
	return FigureTransform(CrowdFigures[Index], Time, 0.f);
}

void AArenaAudience::Rebuild()
{
	bDirty = false;
	SetupVisuals();

	// Keep clap jumps that are mid-air across the rebuild (slot is the stable key).
	TMap<int32, float> RunningJumps;
	for (const FFigure& Figure : NamedFigures)
	{
		if (Figure.JumpT >= 0.f)
		{
			RunningJumps.Add(Figure.Slot, Figure.JumpT);
		}
	}
	for (const FFigure& Figure : CrowdFigures)
	{
		if (Figure.JumpT >= 0.f)
		{
			RunningJumps.Add(Figure.Slot, Figure.JumpT);
		}
	}

	NamedFigures.Reset();
	NamedFigureIndex.Reset();
	CrowdFigures.Reset();

	if (IsValid(Venue))
	{
		const int32 NumSlots = Venue->GetNumSlots();
		const int32 Cap = FMath::Max(0, MaxFigures);

		// Named: by slot (front rows first). Over the cap only the front ones are drawn, but the guest always is.
		TArray<FArenaViewer> Sorted;
		Named.GenerateValueArray(Sorted);
		Sorted.Sort([](const FArenaViewer& A, const FArenaViewer& B) { return A.Slot < B.Slot; });

		int32 MaxNamedSlot = INDEX_NONE;
		for (const FArenaViewer& Viewer : Sorted)
		{
			if (Viewer.Slot < 0 || Viewer.Slot >= NumSlots)
			{
				continue;
			}
			const bool bGuest = Viewer.Id == WalkId || Viewer.Id == OnStageId;
			if (NamedFigures.Num() >= Cap && !bGuest)
			{
				continue;
			}
			FFigure Figure = MakeFigure(Viewer.Slot, true);
			Figure.Id = Viewer.Id;
			if (const float* Jump = RunningJumps.Find(Viewer.Slot))
			{
				Figure.JumpT = *Jump;
			}
			NamedFigureIndex.Add(Viewer.Id, NamedFigures.Add(Figure));
			MaxNamedSlot = FMath::Max(MaxNamedSlot, Viewer.Slot);
		}

		// Anonymous crowd = platform viewers who did not enter. They sit behind the last named viewer; if the
		// bowl runs out, they take the empty seats further forward (seats freed by viewers who left).
		const int32 Wanted = TotalWatching < 0 ? 0 : FMath::Max(0, TotalWatching - Named.Num());
		const int32 CrowdTarget = FMath::Min(Wanted, FMath::Max(0, Cap - NamedFigures.Num()));
		CrowdFigures.Reserve(CrowdTarget);
		auto AddCrowd = [this, &RunningJumps](int32 Slot)
		{
			FFigure Figure = MakeFigure(Slot, false);
			if (const float* Jump = RunningJumps.Find(Slot))
			{
				Figure.JumpT = *Jump;
			}
			CrowdFigures.Add(Figure);
		};
		for (int32 Slot = MaxNamedSlot + 1; Slot < NumSlots && CrowdFigures.Num() < CrowdTarget; ++Slot)
		{
			AddCrowd(Slot);
		}
		for (int32 Slot = 0; Slot <= MaxNamedSlot && Slot < NumSlots && CrowdFigures.Num() < CrowdTarget; ++Slot)
		{
			if (!SlotOwner.Contains(Slot))
			{
				AddCrowd(Slot);
			}
		}
	}

	// Write every instance from scratch.
	TArray<FTransform> NamedBodyXf;
	TArray<FTransform> NamedHeadXf;
	TArray<FTransform> CrowdBodyXf;
	TArray<FTransform> CrowdHeadXf;
	TArray<FTransform> CustomXf;
	const bool bCustom = FigureMesh != nullptr;
	if (bCustom)
	{
		CustomXf.Reserve(NamedFigures.Num() + CrowdFigures.Num());
	}

	for (int32 Index = 0; Index < NamedFigures.Num(); ++Index)
	{
		bool bStanding = false;
		const FTransform Pose = NamedRoot(Index, bStanding);
		if (bCustom)
		{
			CustomXf.Add(Pose);
		}
		else
		{
			NamedBodyXf.Add(ArenaAudienceDetail::BodyLocal(bStanding) * Pose);
			NamedHeadXf.Add(ArenaAudienceDetail::HeadLocal(bStanding) * Pose);
		}
	}
	for (const FFigure& Figure : CrowdFigures)
	{
		const FTransform Pose = FigureTransform(Figure, Time, 0.f);
		if (bCustom)
		{
			CustomXf.Add(Pose);
		}
		else
		{
			CrowdBodyXf.Add(ArenaAudienceDetail::BodyLocal(false) * Pose);
			CrowdHeadXf.Add(ArenaAudienceDetail::HeadLocal(false) * Pose);
		}
	}

	auto SetInstances = [](UInstancedStaticMeshComponent* Ism, const TArray<FTransform>& Transforms)
	{
		if (!Ism)
		{
			return;
		}
		Ism->ClearInstances();
		if (Transforms.Num() > 0)
		{
			Ism->AddInstances(Transforms, false, true);   // world space
		}
	};
	if (bCustom && CustomFigures && CustomFigures->GetStaticMesh() != FigureMesh.Get())
	{
		CustomFigures->SetStaticMesh(FigureMesh);
	}
	SetInstances(CustomFigures, CustomXf);
	SetInstances(NamedBodies, NamedBodyXf);
	SetInstances(NamedHeads, NamedHeadXf);
	SetInstances(CrowdBodies, CrowdBodyXf);
	SetInstances(CrowdHeads, CrowdHeadXf);

	DisplayedFigures = NamedFigures.Num() + CrowdFigures.Num();
	bNamedPoseDirty = false;
	LabelTimer = ArenaAudienceDetail::LabelInterval;   // reconcile labels on this tick
}

// ------------------------------------------------------------------------------------------------
// Animation
// ------------------------------------------------------------------------------------------------

void AArenaAudience::Tick(float DeltaSeconds)
{
	using namespace ArenaAudienceDetail;

	Super::Tick(DeltaSeconds);

	Time += DeltaSeconds;
	if (bDirty)
	{
		Rebuild();
	}
	UpdateWalk(DeltaSeconds);
	Animate(DeltaSeconds);

	LabelTimer += DeltaSeconds;
	if (LabelTimer >= LabelInterval)
	{
		LabelTimer = 0.f;
		UpdateLabels();
	}
	else if (IsWalking())
	{
		// The walking guest's tag follows them every frame.
		UWidgetComponent* Tag = Labels.FindRef(WalkId);
		FVector Head;
		if (IsValid(Tag) && GetFigureHeadLocation(WalkId, Head))
		{
			Tag->SetWorldLocation(Head + FVector(0.f, 0.f, LabelAboveHead));
		}
	}
}

void AArenaAudience::Animate(float DeltaSeconds)
{
	using namespace ArenaAudienceDetail;

	++AnimFrame;

	// Advance clap jumps; a jump that ends this frame is still listed once so it lands back on the seat.
	TArray<int32> NamedJumpers;
	TArray<int32> CrowdJumpers;
	auto AdvanceJumps = [DeltaSeconds](TArray<FFigure>& Figures, TArray<int32>& OutTouched)
	{
		for (int32 Index = 0; Index < Figures.Num(); ++Index)
		{
			FFigure& Figure = Figures[Index];
			if (Figure.JumpT < 0.f)
			{
				continue;
			}
			Figure.JumpT += DeltaSeconds;
			if (Figure.JumpT >= JumpDuration)
			{
				Figure.JumpT = -1.f;
			}
			OutTouched.Add(Index);
		}
	};
	AdvanceJumps(NamedFigures, NamedJumpers);
	AdvanceJumps(CrowdFigures, CrowdJumpers);

	const bool bBob = Hype > BobMinHype;
	const bool bBobEnded = bWasBobbing && !bBob;   // one last write to settle everyone back on their seats
	bWasBobbing = bBob;
	const bool bCrowdFrame = CrowdFigures.Num() <= HalfRateCrowd || (AnimFrame % 2) == 0;

	// Named: everyone while bobbing / after a pose change, otherwise only jumpers and the walking guest.
	bool bNamedTouched = false;
	if (NamedFigures.Num() > 0)
	{
		TArray<int32> Indices;
		if (bBob || bBobEnded || bNamedPoseDirty)
		{
			Indices.Reserve(NamedFigures.Num());
			for (int32 Index = 0; Index < NamedFigures.Num(); ++Index)
			{
				Indices.Add(Index);
			}
		}
		else
		{
			Indices = NamedJumpers;
			if (IsWalking())
			{
				if (const int32* WalkIndex = NamedFigureIndex.Find(WalkId))
				{
					Indices.AddUnique(*WalkIndex);
				}
			}
		}
		bNamedTouched = UpdateFigures(true, Indices);
	}
	bNamedPoseDirty = false;

	// Crowd: everyone while bobbing (every second tick for big crowds), otherwise only jumpers.
	bool bCrowdTouched = false;
	if (CrowdFigures.Num() > 0)
	{
		TArray<int32> Indices;
		if ((bBob && bCrowdFrame) || bBobEnded)
		{
			Indices.Reserve(CrowdFigures.Num());
			for (int32 Index = 0; Index < CrowdFigures.Num(); ++Index)
			{
				Indices.Add(Index);
			}
		}
		else
		{
			Indices = CrowdJumpers;
		}
		bCrowdTouched = UpdateFigures(false, Indices);
	}

	UpdateGlowSticks(bCrowdFrame);

	if (bNamedTouched)
	{
		MarkFiguresRenderDirty(true);
	}
	if (bCrowdTouched)
	{
		MarkFiguresRenderDirty(false);
	}
}

bool AArenaAudience::UpdateFigures(bool bNamedGroup, const TArray<int32>& Indices)
{
	const TArray<FFigure>& Figures = bNamedGroup ? NamedFigures : CrowdFigures;
	if (Indices.Num() == 0 || Figures.Num() == 0)
	{
		return false;
	}

	TArray<FTransform> Roots;
	TArray<bool> Standing;
	if (Indices.Num() == Figures.Num())
	{
		// Whole group: one batch (indices are 0..N-1 in order).
		Roots.Reserve(Figures.Num());
		Standing.Reserve(Figures.Num());
		for (int32 Index = 0; Index < Figures.Num(); ++Index)
		{
			bool bStanding = false;
			Roots.Add(FigureRoot(bNamedGroup, Index, bStanding));
			Standing.Add(bStanding);
		}
		PushFigures(bNamedGroup, 0, Roots, Standing);
		return true;
	}

	for (const int32 Index : Indices)
	{
		if (!Figures.IsValidIndex(Index))
		{
			continue;
		}
		bool bStanding = false;
		Roots.Reset();
		Standing.Reset();
		Roots.Add(FigureRoot(bNamedGroup, Index, bStanding));
		Standing.Add(bStanding);
		PushFigures(bNamedGroup, Index, Roots, Standing);
	}
	return true;
}

void AArenaAudience::PushFigures(bool bNamedGroup, int32 First, const TArray<FTransform>& Roots, const TArray<bool>& Standing)
{
	using namespace ArenaAudienceDetail;

	if (Roots.Num() == 0)
	{
		return;
	}

	// BatchUpdateInstancesTransforms(StartIndex, Transforms, bWorldSpace, bMarkRenderStateDirty, bTeleport);
	// the render state is marked dirty once per frame in Animate().
	if (FigureMesh)
	{
		const int32 Offset = bNamedGroup ? 0 : NamedFigures.Num();
		if (CustomFigures)
		{
			CustomFigures->BatchUpdateInstancesTransforms(Offset + First, Roots, true, false, true);
		}
		return;
	}

	TArray<FTransform> Bodies;
	TArray<FTransform> Heads;
	Bodies.Reserve(Roots.Num());
	Heads.Reserve(Roots.Num());
	for (int32 Index = 0; Index < Roots.Num(); ++Index)
	{
		const bool bStanding = Standing.IsValidIndex(Index) && Standing[Index];
		Bodies.Add(BodyLocal(bStanding) * Roots[Index]);
		Heads.Add(HeadLocal(bStanding) * Roots[Index]);
	}
	UInstancedStaticMeshComponent* BodyIsm = bNamedGroup ? NamedBodies : CrowdBodies;
	UInstancedStaticMeshComponent* HeadIsm = bNamedGroup ? NamedHeads : CrowdHeads;
	if (BodyIsm)
	{
		BodyIsm->BatchUpdateInstancesTransforms(First, Bodies, true, false, true);
	}
	if (HeadIsm)
	{
		HeadIsm->BatchUpdateInstancesTransforms(First, Heads, true, false, true);
	}
}

void AArenaAudience::MarkFiguresRenderDirty(bool bNamedGroup)
{
	if (FigureMesh)
	{
		if (CustomFigures)
		{
			CustomFigures->MarkRenderStateDirty();
		}
		return;
	}
	UInstancedStaticMeshComponent* BodyIsm = bNamedGroup ? NamedBodies : CrowdBodies;
	UInstancedStaticMeshComponent* HeadIsm = bNamedGroup ? NamedHeads : CrowdHeads;
	if (BodyIsm)
	{
		BodyIsm->MarkRenderStateDirty();
	}
	if (HeadIsm)
	{
		HeadIsm->MarkRenderStateDirty();
	}
}

void AArenaAudience::UpdateGlowSticks(bool bCrowdFrame)
{
	using namespace ArenaAudienceDetail;

	if (!GlowSticks)
	{
		return;
	}

	const bool bHypeSticks = Hype > CrowdStickHype;
	bool bAnyJump = false;
	for (const FFigure& Figure : NamedFigures)
	{
		if (Figure.JumpT >= 0.f)
		{
			bAnyJump = true;
			break;
		}
	}
	if (!bAnyJump)
	{
		for (const FFigure& Figure : CrowdFigures)
		{
			if (Figure.JumpT >= 0.f)
			{
				bAnyJump = true;
				break;
			}
		}
	}
	if (!bHypeSticks && !bAnyJump && GlowSticks->GetInstanceCount() == 0)
	{
		return;   // nothing raised, nothing to lower
	}
	if (bHypeSticks && !bCrowdFrame)
	{
		return;   // big crowd: sticks move at the same half rate as the bobbing
	}

	// Raised sticks only (lowered = not drawn); rebuilt every update.
	TArray<FTransform> Sticks;
	auto AddStick = [this, &Sticks](const FFigure& Figure, float Amount)
	{
		const float Side = (Figure.Slot % 2 == 0) ? 1.f : -1.f;
		const float Sway = FMath::Sin(Time * 2.f * UE_PI * BobHz + Figure.Phase) * 14.f * Amount;
		Sticks.Add(StickLocal(Side, Sway) * FigureTransform(Figure, Time, 0.f));
	};
	for (const FFigure& Figure : NamedFigures)
	{
		if (Figure.JumpT >= 0.f && Figure.Id != WalkId && Figure.Id != OnStageId)
		{
			AddStick(Figure, 1.f);
		}
	}
	for (int32 Index = 0; Index < CrowdFigures.Num(); ++Index)
	{
		const FFigure& Figure = CrowdFigures[Index];
		if (Figure.JumpT >= 0.f)
		{
			AddStick(Figure, 1.f);
		}
		else if (bHypeSticks && Index % 3 == 0)
		{
			AddStick(Figure, Hype);
		}
	}

	if (Sticks.Num() == GlowSticks->GetInstanceCount())
	{
		if (Sticks.Num() > 0)
		{
			GlowSticks->BatchUpdateInstancesTransforms(0, Sticks, true, true, true);
		}
	}
	else
	{
		GlowSticks->ClearInstances();
		if (Sticks.Num() > 0)
		{
			GlowSticks->AddInstances(Sticks, false, true);
		}
	}
}

bool AArenaAudience::GetFigureHeadLocation(const FString& Id, FVector& Out) const
{
	using namespace ArenaAudienceDetail;

	const FArenaViewer* Viewer = Named.Find(Id);
	if (!Viewer)
	{
		return false;
	}

	if (const int32* Index = NamedFigureIndex.Find(Id))
	{
		if (NamedFigures.IsValidIndex(*Index) && NamedFigures[*Index].Id == Id)
		{
			bool bStanding = false;
			const FTransform Pose = NamedRoot(*Index, bStanding);
			Out = (HeadLocal(bStanding) * Pose).GetLocation();
			return true;
		}
	}

	// Not drawn yet (just joined / over the figure cap).
	if (Id == WalkId && IsWalking())
	{
		Out = (HeadLocal(true) * WalkRoot()).GetLocation();
		return true;
	}
	if (Id == OnStageId)
	{
		Out = (HeadLocal(false) * WalkChair).GetLocation();
		return true;
	}
	if (IsValid(Venue))
	{
		Out = Venue->GetSlotHeadLocation(Viewer->Slot);
		return true;
	}
	return false;
}

// ------------------------------------------------------------------------------------------------
// Name labels
// ------------------------------------------------------------------------------------------------

void AArenaAudience::UpdateLabels()
{
	using namespace ArenaAudienceDetail;

	const UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld())
	{
		return;
	}

	// Front-most MaxLabels named viewers (NamedFigures is sorted by slot) + the guest, always.
	TArray<FString> Wanted;
	const int32 Limit = FMath::Max(0, MaxLabels);
	for (int32 Index = 0; Index < NamedFigures.Num() && Wanted.Num() < Limit; ++Index)
	{
		Wanted.Add(NamedFigures[Index].Id);
	}
	if (!WalkId.IsEmpty() && Named.Contains(WalkId))
	{
		Wanted.AddUnique(WalkId);
	}
	if (!OnStageId.IsEmpty() && Named.Contains(OnStageId))
	{
		Wanted.AddUnique(OnStageId);
	}

	TSet<FString> WantedSet;
	for (const FString& Id : Wanted)
	{
		WantedSet.Add(Id);
	}
	for (auto It = Labels.CreateIterator(); It; ++It)
	{
		if (!WantedSet.Contains(It.Key()))
		{
			if (UWidgetComponent* Stale = It.Value())
			{
				Stale->DestroyComponent();
			}
			LabelSignature.Remove(It.Key());
			It.RemoveCurrent();
		}
	}

	for (const FString& Id : Wanted)
	{
		const FArenaViewer* Viewer = Named.Find(Id);
		FVector Head;
		if (!Viewer || !GetFigureHeadLocation(Id, Head))
		{
			continue;
		}
		const FVector Location = Head + FVector(0.f, 0.f, LabelAboveHead);

		UWidgetComponent* Tag = Labels.FindRef(Id);
		if (!IsValid(Tag))
		{
			Tag = CreateLabel(Location);
			if (!Tag)
			{
				continue;
			}
			Labels.Add(Id, Tag);
			LabelSignature.Remove(Id);
		}
		else
		{
			Tag->SetWorldLocation(Location);
		}

		// Only touch the widget when something it shows changed.
		const bool bHighlight = Id == WalkId || Id == OnStageId;
		const FString Signature = FString::Printf(TEXT("%s|%s|%d"), *Viewer->Name, *Viewer->Label, bHighlight ? 1 : 0);
		if (LabelSignature.FindRef(Id) != Signature)
		{
			if (UArenaLabelWidget* Widget = Cast<UArenaLabelWidget>(Tag->GetUserWidgetObject()))
			{
				Widget->SetLabel(Viewer->Name, Viewer->Label, bHighlight);
				LabelSignature.Add(Id, Signature);
			}
		}
	}
}

UWidgetComponent* AArenaAudience::CreateLabel(const FVector& Location)
{
	UWidgetComponent* Tag = NewObject<UWidgetComponent>(
		this, MakeUniqueObjectName(this, UWidgetComponent::StaticClass(), TEXT("NameLabel")), RF_Transient);
	if (!Tag)
	{
		return nullptr;
	}
	// Screen space: always readable and the same pixel size at any distance; pivot = bottom centre above the head.
	Tag->SetWidgetSpace(EWidgetSpace::Screen);
	Tag->SetWidgetClass(UArenaLabelWidget::StaticClass());
	Tag->SetDrawAtDesiredSize(true);
	Tag->SetPivot(FVector2D(0.5f, 1.f));
	Tag->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Tag->SetGenerateOverlapEvents(false);
	Tag->SetCastShadow(false);
	Tag->SetupAttachment(Root);
	Tag->RegisterComponent();
	Tag->SetWorldLocation(Location);
	Tag->InitWidget();   // creates the user widget now so GetUserWidgetObject() works right away
	return Tag;
}

void AArenaAudience::RemoveLabel(const FString& Id)
{
	TObjectPtr<UWidgetComponent> Tag;
	if (Labels.RemoveAndCopyValue(Id, Tag) && Tag)
	{
		Tag->DestroyComponent();
	}
	LabelSignature.Remove(Id);
}
