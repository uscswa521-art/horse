#include "ArenaVenue.h"

#include "ArenaTypes.h"
#include "LiveArena.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

// 場館全部用引擎基本形狀砌（Cube 100cm 立方、Cylinder 直徑 100 高 100，pivot 都喺中心），零 asset。
// Actor space（cm）：X 向舞台、Y 向右、Z 向上；舞台前緣中心 = 原點；觀眾席係以原點為圓心、喺 -X 嗰邊嘅同心弧。
// 弧上一點用角度 Theta 表示：0 = 舞台正前方，正數 = 觀眾望住舞台嘅右手邊（+Y）。
// Build() 整出嚟嘅 component 全部 RF_Transient：唔會存落 level，每次 OnConstruction / 開波時重新砌。

// Named (not anonymous) namespace: unity builds paste several .cpp files into one translation unit.
namespace ArenaVenueDetail
{
	constexpr float AisleWidth = 160.f;          // 每行兩端嘅走廊（座位弧尾 -> 封邊牆）
	constexpr float SideWallThickness = 30.f;
	constexpr float RailHeight = 105.f;          // 封邊牆高出嗰行地台幾多
	constexpr float OuterWallGap = 300.f;        // 最後一行 -> 外圍牆內面
	constexpr float OuterWallThickness = 60.f;
	constexpr float OuterWallAbove = 600.f;      // 外圍牆高出最後一行幾多
	constexpr float BackWallThickness = 50.f;
	constexpr float FrontWalkGap = 150.f;        // 前緣走道半徑 = FirstRowRadius - 150
	constexpr int32 StairSteps = 4;
	constexpr float StairTread = 35.f;
	constexpr float StairWidth = 200.f;
	constexpr float StairInset = 250.f;          // 樓梯中線 Y = StageWidth / 2 - 250
	constexpr float TrussSize = 44.f;            // 方形桁架外徑
	constexpr float TrussChord = 8.f;
	constexpr float TrussBrace = 4.f;
	constexpr float TrussStation = 70.f;
	constexpr float FrontTrussX = 150.f;
	constexpr float TrussAboveScreen = 150.f;
	constexpr float TowerInset = 60.f;
	constexpr int32 NumTrussLights = 8;
	constexpr float MaxTreadSegment = 300.f;
	constexpr float MaxWallSegment = 400.f;
	constexpr float WalkPointSpacing = 120.f;

	/** sRGB hex ("4A1C1F") -> linear colour for the material parameter. */
	static FLinearColor Hex(const TCHAR* InHex)
	{
		return FLinearColor(FColor::FromHex(InHex));
	}

	/** Point on the bowl at Radius / Theta, height Z (actor space). */
	static FVector Polar(float Radius, float Theta, float Z)
	{
		return FVector(-Radius * FMath::Cos(Theta), Radius * FMath::Sin(Theta), Z);
	}

	/** Yaw (deg) whose +X points from a bowl point at angle Theta toward the stage (the origin). */
	static float InwardYaw(float Theta)
	{
		return -FMath::RadiansToDegrees(Theta);
	}

	/** Appends points along an arc (excluding the start point), at most WalkPointSpacing apart. */
	static void AddArc(TArray<FVector>& Out, float Radius, float From, float To, float Z)
	{
		const float Span = To - From;
		if (FMath::Abs(Span) * Radius < 1.f)
		{
			return;
		}
		const int32 Steps = FMath::Max(1, FMath::CeilToInt(FMath::Abs(Span) * Radius / WalkPointSpacing));
		for (int32 Step = 1; Step <= Steps; ++Step)
		{
			Out.Add(Polar(Radius, From + Span * Step / Steps, Z));
		}
	}
}

AArenaVenue::AArenaVenue()
{
	PrimaryActorTick.bCanEverTick = false;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	RootComponent = Root;
}

void AArenaVenue::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	Build();
}

void AArenaVenue::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	// Spawned venues already built in OnConstruction; level-placed copies (PIE / cooked) arrive empty.
	if (SlotLocal.Num() == 0 || Parts.Num() == 0)
	{
		Build();
	}
}

// ------------------------------------------------------------------------------------------------
// Build
// ------------------------------------------------------------------------------------------------

void AArenaVenue::Build()
{
	using namespace ArenaVenueDetail;

	// 清走上一次嘅結果，連埋唔喺 Parts 入面、但係我哋整嘅 ISM（transient、唔係 default subobject，
	// 例如 PIE 複製過嚟嘅殘留）。Blueprint 子類自己加嘅 ISM 唔郁。
	TArray<UInstancedStaticMeshComponent*> OldIsms;
	GetComponents<UInstancedStaticMeshComponent>(OldIsms);
	OldIsms.RemoveAll([](const UInstancedStaticMeshComponent* Comp)
	{
		return !Comp || !Comp->HasAnyFlags(RF_Transient) || Comp->IsDefaultSubobject();
	});
	for (UInstancedStaticMeshComponent* Part : Parts)
	{
		OldIsms.AddUnique(Part);
	}
	for (UInstancedStaticMeshComponent* Old : OldIsms)
	{
		if (IsValid(Old))
		{
			Old->DestroyComponent();
		}
	}
	Parts.Reset();
	PendingInstances.Reset();

	if (!CubeMesh)
	{
		CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	}
	if (!CylinderMesh)
	{
		CylinderMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	}
	if (!BasicShapeMaterial)
	{
		BasicShapeMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	}

	// 座位資料唔靠 mesh，一定要有（audience / server layout 都靠佢）。
	BuildSlots();

	if (!CubeMesh || !CylinderMesh)
	{
		UE_LOG(LogLiveArena, Error, TEXT("ArenaVenue: engine basic shapes not found; seats are valid but nothing is drawn."));
		return;
	}

	const FLinearColor SeatColor = Hex(TEXT("4A1C1F"));
	const FLinearColor FixtureColor = Hex(TEXT("1A1918"));

	FBuildIsms Isms;
	Isms.Floor = MakeIsm(TEXT("Floor"), CubeMesh, Hex(TEXT("0C0D11")));
	Isms.Walls = MakeIsm(TEXT("Walls"), CubeMesh, Hex(TEXT("14151A")));
	Isms.Black = MakeIsm(TEXT("LedFrame"), CubeMesh, Hex(TEXT("050506")));
	Isms.Truss = MakeIsm(TEXT("Truss"), CubeMesh, Hex(TEXT("2E2C2A")));
	Isms.Brass = MakeIsm(TEXT("Brass"), CubeMesh, Hex(TEXT("B08D57")));
	Isms.BrassDisc = MakeIsm(TEXT("HostMark"), CylinderMesh, Hex(TEXT("B08D57")));
	Isms.DarkBrass = MakeIsm(TEXT("DarkBrass"), CubeMesh, Hex(TEXT("6E5636")));

	// 燈具外殼包住 / 貼住燈嘅位置，唔投影，免得擋咗 director 放喺 mount 點嘅燈。
	Isms.Fixtures = MakeIsm(TEXT("Fixtures"), CubeMesh, FixtureColor);
	Isms.Fixtures->SetCastShadow(false);
	Isms.FollowSpots = MakeIsm(TEXT("FollowSpots"), FollowSpotMesh ? FollowSpotMesh.Get() : CylinderMesh.Get(), FixtureColor);
	Isms.FollowSpots->SetCastShadow(false);

	if (SeatMesh)
	{
		Isms.Seats = MakeIsm(TEXT("Seats"), SeatMesh, SeatColor);
	}
	else
	{
		Isms.SeatRed = MakeIsm(TEXT("SeatCushions"), CubeMesh, SeatColor);
		Isms.SeatBacks = MakeIsm(TEXT("SeatBacks"), CubeMesh, SeatColor);
	}
	if (GuestChairMesh)
	{
		Isms.GuestChair = MakeIsm(TEXT("GuestChair"), GuestChairMesh, SeatColor);
	}
	else if (!Isms.SeatRed)
	{
		Isms.SeatRed = MakeIsm(TEXT("GuestChair"), CubeMesh, SeatColor);
	}

	BuildShell(Isms);
	BuildStage(Isms);
	BuildRig(Isms);
	BuildSeats(Isms);

	int32 NumInstances = 0;
	for (TPair<UInstancedStaticMeshComponent*, TArray<FTransform>>& Pending : PendingInstances)
	{
		if (IsValid(Pending.Key) && Pending.Value.Num() > 0)
		{
			Pending.Key->AddInstances(Pending.Value, false);
			NumInstances += Pending.Value.Num();
		}
	}
	PendingInstances.Reset();

	// OnConstruction runs on every drag in the editor: only report in game worlds.
	const UWorld* World = GetWorld();
	if (World && World->IsGameWorld())
	{
		UE_LOG(LogLiveArena, Log, TEXT("ArenaVenue built: %d rows, %d seats, %d parts, %d instances."),
			RowCounts.Num(), SlotLocal.Num(), Parts.Num(), NumInstances);
	}
}

void AArenaVenue::BuildSlots()
{
	SlotLocal.Reset();
	SlotLabels.Reset();
	SlotRows.Reset();
	RowCounts.Reset();

	const int32 Rows = FMath::Max(NumRows, 1);
	const float ArcRad = FMath::DegreesToRadians(ArcDegrees);
	const float Spacing = FMath::Max(SeatSpacing, 1.f);

	int32 Total = 0;
	for (int32 Row = 0; Row < Rows; ++Row)
	{
		const float Radius = FMath::Max(RowRadius(Row), 1.f);
		const int32 Count = FMath::Max(1, FMath::FloorToInt(Radius * ArcRad / Spacing));
		RowCounts.Add(Count);
		Total += Count;
	}

	SlotLocal.Reserve(Total);
	SlotLabels.Reserve(Total);
	SlotRows.Reserve(Total);

	FVector Sum = FVector::ZeroVector;
	for (int32 Row = 0; Row < Rows; ++Row)
	{
		const int32 Count = RowCounts[Row];
		const float Radius = FMath::Max(RowRadius(Row), 1.f);
		const FString RowLabel = ArenaSlots::RowName(Row);

		// 行內由中間向兩邊填（同 server.py 一模一樣），所以 slot 次序 != 位置次序。
		for (int32 J = 0; J < Count; ++J)
		{
			const int32 Pos = ArenaSlots::CenterOutPosition(J, Count);
			const float Theta = (Pos - (Count - 1) * 0.5f) * Spacing / Radius;
			const FVector Location = ArenaVenueDetail::Polar(Radius, Theta, Row * RowRise);

			SlotLocal.Add(FTransform(FRotator(0.f, ArenaVenueDetail::InwardYaw(Theta), 0.f), Location));
			SlotLabels.Add(RowLabel + FString::FromInt(Pos + 1));
			SlotRows.Add(Row);
			Sum += Location;
		}
	}

	AudienceCentreLocal = Total > 0
		? Sum / Total + FVector(0.f, 0.f, 115.f)
		: ArenaVenueDetail::Polar(FirstRowRadius + Rows * RowDepth * 0.5f, 0.f, Rows * RowRise * 0.5f + 115.f);
}

void AArenaVenue::BuildShell(const FBuildIsms& Isms)
{
	using namespace ArenaVenueDetail;

	const int32 Rows = FMath::Max(NumRows, 1);
	const float LastRadius = RowRadius(Rows - 1);
	const float LastFloor = (Rows - 1) * RowRise;
	const float WallInner = LastRadius + OuterWallGap;
	const float WallCentre = WallInner + OuterWallThickness * 0.5f;
	const float WallTop = LastFloor + OuterWallAbove;
	// 外圍牆沿住同一個半徑一路包到舞台後牆（X = StageDepth）先停，成個場封晒。
	const float WallEnd = FMath::Acos(FMath::Clamp(-StageDepth / WallCentre, -1.f, 1.f));
	const float BackHalfWidth = WallCentre * FMath::Sin(WallEnd) + OuterWallThickness;
	const float BackTop = FMath::Max(WallTop, TrussHeight() + 450.f);
	const float Signs[] = { -1.f, 1.f };

	// ---- floor slab (top at Z = 0) ----
	{
		const float MinX = -(WallInner + OuterWallThickness + 200.f);
		const float MaxX = StageDepth + BackWallThickness + 200.f;
		const float HalfY = FMath::Max(WallInner + OuterWallThickness + 200.f, BackHalfWidth + 200.f);
		AddBox(Isms.Floor, FVector((MinX + MaxX) * 0.5f, 0.f, -10.f), FVector(MaxX - MinX, HalfY * 2.f, 20.f));
	}

	// ---- tiers: one stepped platform per row, then the concourse behind the last row ----
	// 第 r 行地台由 radius(r) - 0.55 RowDepth 去到 radius(r) + 0.45 RowDepth（座位前面留腳位），頂 = r * RowRise。
	// 地台橫向包埋兩邊走廊同封邊牆；走廊就係呢啲台階本身。
	const float HalfArc = HalfArcRad();
	for (int32 Row = 0; Row <= Rows; ++Row)
	{
		const bool bConcourse = Row == Rows;
		const float Inner = bConcourse ? LastRadius + 0.45f * RowDepth : RowRadius(Row) - 0.55f * RowDepth;
		const float Outer = bConcourse ? WallInner + 10.f : RowRadius(Row) + 0.45f * RowDepth;
		const float Top = bConcourse ? LastFloor : Row * RowRise;
		const float Depth = Outer - Inner;
		const float Mid = (Inner + Outer) * 0.5f;
		if (Depth <= 1.f || Mid <= 1.f)
		{
			continue;
		}

		if (Top > 0.5f)
		{
			const float Extent = HalfArc + (AisleWidth + SideWallThickness) / Mid;
			const int32 Segments = FMath::Max(1, FMath::CeilToInt(2.f * Extent * Mid / MaxTreadSegment));
			const float Step = 2.f * Extent / Segments;
			const float HalfTan = FMath::Tan(Step * 0.5f);
			const float Width = 2.f * Outer * HalfTan + 2.f;       // covers the outer edge of the arc
			const float NoseWidth = 2.f * Inner * HalfTan + 1.f;

			for (int32 Seg = 0; Seg < Segments; ++Seg)
			{
				const float Theta = -Extent + (Seg + 0.5f) * Step;
				AddBox(Isms.Floor, Polar(Mid, Theta, Top * 0.5f), FVector(Depth, Width, Top), InwardYaw(Theta));
				if (!bConcourse)
				{
					// 級邊暗銅細條：暗場都睇到一行行
					AddBox(Isms.DarkBrass, Polar(Inner - 1.f, Theta, Top - 1.5f), FVector(4.f, NoseWidth, 4.f), InwardYaw(Theta));
				}
			}
		}

		// side walls: stepped with the tiers, RailHeight above each tread, brass cap on top
		const float WallAngle = HalfArc + (AisleWidth + SideWallThickness * 0.5f) / Mid;
		const float SideHeight = Top + RailHeight;
		for (const float Sign : Signs)
		{
			const float Theta = Sign * WallAngle;
			AddBox(Isms.Walls, Polar(Mid, Theta, SideHeight * 0.5f), FVector(Depth + 2.f, SideWallThickness, SideHeight), InwardYaw(Theta));
			AddBox(Isms.DarkBrass, Polar(Mid, Theta, SideHeight + 3.f), FVector(Depth + 2.f, SideWallThickness + 6.f, 6.f), InwardYaw(Theta));
		}
	}

	// ---- outer wall: arc segments behind the bowl, wrapping round to the back wall ----
	{
		const int32 Segments = FMath::Max(1, FMath::CeilToInt(2.f * WallEnd * WallCentre / MaxWallSegment));
		const float Step = 2.f * WallEnd / Segments;
		const float HalfTan = FMath::Tan(Step * 0.5f);
		const float Width = 2.f * (WallInner + OuterWallThickness) * HalfTan + 2.f;
		const float TrimWidth = 2.f * WallInner * HalfTan + 1.f;
		for (int32 Seg = 0; Seg < Segments; ++Seg)
		{
			const float Theta = -WallEnd + (Seg + 0.5f) * Step;
			AddBox(Isms.Walls, Polar(WallCentre, Theta, WallTop * 0.5f), FVector(OuterWallThickness, Width, WallTop), InwardYaw(Theta));
			AddBox(Isms.DarkBrass, Polar(WallInner - 2.f, Theta, WallTop - 40.f), FVector(4.f, TrimWidth, 8.f), InwardYaw(Theta));
		}
	}

	// ---- back wall behind the stage + a few dark-brass pilasters beside the LED ----
	AddBox(Isms.Walls, FVector(StageDepth + BackWallThickness * 0.5f, 0.f, BackTop * 0.5f),
		FVector(BackWallThickness, BackHalfWidth * 2.f, BackTop));
	const float PilasterHeight = BackTop - 300.f;
	if (PilasterHeight > 100.f)
	{
		for (float Y = ScreenWidth * 0.5f + 300.f; Y < BackHalfWidth - 200.f; Y += 550.f)
		{
			for (const float Sign : Signs)
			{
				AddBox(Isms.DarkBrass, FVector(StageDepth - 5.f, Sign * Y, PilasterHeight * 0.5f), FVector(10.f, 14.f, PilasterHeight));
			}
		}
	}
}

void AArenaVenue::BuildStage(const FBuildIsms& Isms)
{
	using namespace ArenaVenueDetail;

	const float Signs[] = { -1.f, 1.f };

	// ---- stage deck + brass nosing on the front edge (4 cm proud, 0.5 cm above the deck) ----
	AddBox(Isms.Walls, FVector(StageDepth * 0.5f, 0.f, StageHeight * 0.5f), FVector(StageDepth, StageWidth, StageHeight));
	AddBox(Isms.Brass, FVector(0.f, 0.f, StageHeight - 4.5f), FVector(8.f, StageWidth + 8.f, 10.f));

	// ---- stairs: front-right of the stage, StairSteps treads going down toward the audience (-X) ----
	{
		const float StairY = StageWidth * 0.5f - StairInset;
		const float StepRise = StageHeight / (StairSteps + 1);
		for (int32 Step = 0; Step < StairSteps; ++Step)
		{
			const float Top = StageHeight - (Step + 1) * StepRise;
			const float Front = -(Step + 1) * StairTread;
			AddBox(Isms.Walls, FVector(Front + StairTread * 0.5f, StairY, Top * 0.5f), FVector(StairTread, StairWidth, Top));
			AddBox(Isms.DarkBrass, FVector(Front + 1.f, StairY, Top - 1.f), FVector(4.f, StairWidth + 2.f, 3.f));
		}
	}

	// ---- LED black frame: 30 cm border, its front face 5 cm behind the LED plane (X = StageDepth - 60) ----
	const float ScreenBottomZ = StageHeight + ScreenBottom;
	AddBox(Isms.Black, FVector(StageDepth - 40.f, 0.f, ScreenBottomZ + ScreenHeight * 0.5f),
		FVector(30.f, ScreenWidth + 60.f, ScreenHeight + 60.f));
	const float LegTop = ScreenBottomZ - 30.f;
	if (LegTop > StageHeight + 1.f)
	{
		for (const float Sign : Signs)
		{
			AddBox(Isms.Black, FVector(StageDepth - 40.f, Sign * FMath::Max(ScreenWidth * 0.5f - 200.f, 50.f), (StageHeight + LegTop) * 0.5f),
				FVector(20.f, 20.f, LegTop - StageHeight));
		}
	}

	// ---- host mark: small brass disc on the deck ----
	QueueInstance(Isms.BrassDisc, FTransform(FQuat::Identity, HostMarkLocal().GetLocation() + FVector(0.f, 0.f, 1.f), FVector(0.6f, 0.6f, 0.02f)));

	// ---- guest chair: custom mesh, or a lounge chair from boxes (seat + back + arms) ----
	const FTransform Chair = GuestChairLocal();
	if (Isms.GuestChair)
	{
		QueueInstance(Isms.GuestChair, Chair);
	}
	else if (Isms.SeatRed)
	{
		auto ChairPart = [this, &Isms, &Chair](const FVector& Centre, const FVector& Size, float PitchDeg)
		{
			QueueInstance(Isms.SeatRed, FTransform(FRotator(PitchDeg, 0.f, 0.f), Centre, Size / 100.f) * Chair);
		};
		ChairPart(FVector(0.f, 0.f, 20.f), FVector(50.f, 56.f, 40.f), 0.f);     // base
		ChairPart(FVector(2.f, 0.f, 45.f), FVector(56.f, 62.f, 12.f), 0.f);     // cushion
		ChairPart(FVector(-28.f, 0.f, 82.f), FVector(12.f, 62.f, 66.f), 10.f);  // back, leaning back
		ChairPart(FVector(0.f, -36.f, 61.f), FVector(56.f, 10.f, 20.f), 0.f);   // arm rests
		ChairPart(FVector(0.f, 36.f, 61.f), FVector(56.f, 10.f, 20.f), 0.f);
	}
}

void AArenaVenue::BuildRig(const FBuildIsms& Isms)
{
	using namespace ArenaVenueDetail;

	const float Signs[] = { -1.f, 1.f };

	// ---- ground-supported truss: front + back spans, side spans, four towers standing on the deck ----
	const float TrussZ = TrussHeight();
	const float BackTrussX = FMath::Max(FrontTrussX + 200.f, StageDepth - 200.f);
	const float TowerY = FMath::Max(StageWidth * 0.5f - TowerInset, 200.f);
	const float TowerTop = TrussZ + TrussSize * 0.5f;
	const float TowerXs[] = { FrontTrussX, BackTrussX };

	AddTruss(Isms.Truss, FVector(FrontTrussX, -TowerY, TrussZ), FVector(FrontTrussX, TowerY, TrussZ));
	AddTruss(Isms.Truss, FVector(BackTrussX, -TowerY, TrussZ), FVector(BackTrussX, TowerY, TrussZ));
	for (const float Sign : Signs)
	{
		AddTruss(Isms.Truss, FVector(FrontTrussX, Sign * TowerY, TrussZ), FVector(BackTrussX, Sign * TowerY, TrussZ));
		for (const float X : TowerXs)
		{
			AddTruss(Isms.Truss, FVector(X, Sign * TowerY, StageHeight), FVector(X, Sign * TowerY, TowerTop));
			AddBox(Isms.Truss, FVector(X, Sign * TowerY, StageHeight + 3.f), FVector(70.f, 70.f, 6.f));
		}
	}

	// ---- moving head bodies hanging under the front truss, just above each mount point ----
	for (const FVector& Mount : TrussLightMountsLocal())
	{
		AddBox(Isms.Fixtures, Mount + FVector(0.f, 0.f, 25.f), FVector(28.f, 28.f, 40.f));
	}

	// ---- follow spot booths high at the back of the bowl ----
	// 燈身同平台全部喺 mount 點後面，director 喺 mount 放嘅 spot light 向前射唔會畀佢哋擋住。
	const float ConcourseZ = (FMath::Max(NumRows, 1) - 1) * RowRise;
	const FVector Target(StageDepth * 0.5f, 0.f, StageHeight + 150.f);
	for (int32 Index = 0; Index < 2; ++Index)
	{
		const FVector Mount = FollowSpotMountLocal(Index);
		const FVector Aim = (Target - Mount).GetSafeNormal();
		const FVector Flat = FVector(Aim.X, Aim.Y, 0.0).GetSafeNormal();
		const FVector Across(-Flat.Y, Flat.X, 0.0);
		const float FlatYaw = static_cast<float>(Flat.Rotation().Yaw);

		const FVector BodyCentre = Mount - Aim * 65.0;
		if (FollowSpotMesh)
		{
			// custom mesh: pivot at its centre, lens toward +X
			QueueInstance(Isms.FollowSpots, FTransform(FRotationMatrix::MakeFromX(Aim).ToQuat(), Mount - Aim * 60.0));
		}
		else
		{
			// cylinder axis is Z: turn Z toward the stage; 42 cm wide, 110 cm long, front end 10 cm behind the mount
			QueueInstance(Isms.FollowSpots, FTransform(FRotationMatrix::MakeFromZ(Aim).ToQuat(), BodyCentre, FVector(0.42f, 0.42f, 1.1f)));
		}

		const FVector Deck = Mount - Flat * 100.0 - FVector(0.0, 0.0, 75.0);   // 20..180 cm behind the mount
		AddBox(Isms.Truss, Deck, FVector(160.f, 200.f, 12.f), FlatYaw);
		AddBar(Isms.Fixtures, FVector(BodyCentre.X, BodyCentre.Y, Deck.Z + 6.0), BodyCentre, 8.f);
		AddBar(Isms.Truss, Deck - Flat * 78.0 + Across * 100.0 + FVector(0.0, 0.0, 100.0),
			Deck - Flat * 78.0 - Across * 100.0 + FVector(0.0, 0.0, 100.0), 5.f);
		for (const float Sign : Signs)
		{
			const FVector Foot = Deck - Flat * 60.0 + Across * (Sign * 80.0);
			if (Deck.Z - 6.0 > ConcourseZ + 1.0)
			{
				AddBar(Isms.Truss, FVector(Foot.X, Foot.Y, ConcourseZ), FVector(Foot.X, Foot.Y, Deck.Z - 6.0), 14.f);
			}
		}
	}
}

void AArenaVenue::BuildSeats(const FBuildIsms& Isms)
{
	if (SeatMesh && Isms.Seats)
	{
		PendingInstances.FindOrAdd(Isms.Seats).Reserve(SlotLocal.Num());
		for (const FTransform& Seat : SlotLocal)
		{
			QueueInstance(Isms.Seats, SeatMeshOffset * Seat);
		}
		return;
	}
	if (!Isms.SeatRed || !Isms.SeatBacks)
	{
		return;
	}

	// 座位 local：+X 向舞台，pivot 喺地面。座墊頂 46 cm，椅背向後傾 12 度。
	const FTransform Cushion(FQuat::Identity, FVector(3.f, 0.f, 40.f), FVector(0.45f, 0.50f, 0.12f));
	const FTransform Stem(FQuat::Identity, FVector(-2.f, 0.f, 17.f), FVector(0.14f, 0.30f, 0.34f));
	const FTransform Backrest(FRotator(12.f, 0.f, 0.f), FVector(-23.f, 0.f, 73.f), FVector(0.07f, 0.50f, 0.52f));

	PendingInstances.FindOrAdd(Isms.SeatRed).Reserve(SlotLocal.Num() * 2 + 8);
	PendingInstances.FindOrAdd(Isms.SeatBacks).Reserve(SlotLocal.Num());
	for (const FTransform& Seat : SlotLocal)
	{
		QueueInstance(Isms.SeatRed, Cushion * Seat);
		QueueInstance(Isms.SeatRed, Stem * Seat);
		QueueInstance(Isms.SeatBacks, Backrest * Seat);
	}
}

// ------------------------------------------------------------------------------------------------
// Instance helpers
// ------------------------------------------------------------------------------------------------

UInstancedStaticMeshComponent* AArenaVenue::MakeIsm(FName Name, UStaticMesh* Mesh, const FLinearColor& Color)
{
	// Unique name: components destroyed by the previous Build() are not garbage collected yet.
	UInstancedStaticMeshComponent* Ism = NewObject<UInstancedStaticMeshComponent>(
		this, MakeUniqueObjectName(this, UInstancedStaticMeshComponent::StaticClass(), Name), RF_Transient);
	Ism->SetupAttachment(Root);
	Ism->SetMobility(EComponentMobility::Movable);
	Ism->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	Ism->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Ism->SetGenerateOverlapEvents(false);
	Ism->SetCanEverAffectNavigation(false);
	Ism->SetCastShadow(true);
	Ism->SetStaticMesh(Mesh);

	// Only the blockout shapes get tinted; swapped-in meshes keep their own materials.
	const bool bBlockoutShape = Mesh != nullptr && (Mesh == CubeMesh.Get() || Mesh == CylinderMesh.Get());
	UMaterialInterface* BaseMaterial = BlockoutMaterial ? BlockoutMaterial.Get() : BasicShapeMaterial.Get();
	if (bBlockoutShape && BaseMaterial)
	{
		UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(BaseMaterial, Ism);
		Mid->SetVectorParameterValue(TEXT("Color"), Color);
		Ism->SetMaterial(0, Mid);
	}

	Ism->RegisterComponent();
	Parts.Add(Ism);
	return Ism;
}

void AArenaVenue::QueueInstance(UInstancedStaticMeshComponent* Ism, const FTransform& LocalTransform)
{
	if (Ism)
	{
		PendingInstances.FindOrAdd(Ism).Add(LocalTransform);
	}
}

void AArenaVenue::AddBox(UInstancedStaticMeshComponent* Ism, const FVector& Centre, const FVector& SizeCm, float YawDeg)
{
	// Cube is 100 cm with its pivot in the middle, so scale = size / 100.
	QueueInstance(Ism, FTransform(FRotator(0.f, YawDeg, 0.f), Centre, SizeCm / 100.f));
}

void AArenaVenue::AddBar(UInstancedStaticMeshComponent* Ism, const FVector& From, const FVector& To, float Thickness)
{
	const FVector Delta = To - From;
	const double Length = Delta.Size();
	if (Length < 0.5)
	{
		return;
	}
	const FQuat Rotation = FRotationMatrix::MakeFromX(Delta / Length).ToQuat();
	QueueInstance(Ism, FTransform(Rotation, (From + To) * 0.5, FVector(Length, Thickness, Thickness) / 100.0));
}

void AArenaVenue::AddTruss(UInstancedStaticMeshComponent* Ism, const FVector& From, const FVector& To)
{
	using namespace ArenaVenueDetail;

	const FVector Axis = To - From;
	const double Length = Axis.Size();
	if (Length < 1.0)
	{
		return;
	}
	const FVector Dir = Axis / Length;
	const FVector Ref = FMath::Abs(Dir.Z) > 0.9 ? FVector::ForwardVector : FVector::UpVector;
	const FVector U = FVector::CrossProduct(Ref, Dir).GetSafeNormal();
	const FVector V = FVector::CrossProduct(Dir, U).GetSafeNormal();
	const double H = (TrussSize - TrussChord) * 0.5;
	const FVector Corners[4] = { (U + V) * H, (V - U) * H, (-U - V) * H, (U - V) * H };

	// four chords
	for (const FVector& Corner : Corners)
	{
		AddBar(Ism, From + Corner, To + Corner, TrussChord);
	}

	// square frames every TrussStation cm + zigzag diagonals on all four faces
	const int32 Stations = FMath::Max(1, FMath::RoundToInt(static_cast<float>(Length) / TrussStation));
	for (int32 S = 0; S <= Stations; ++S)
	{
		const FVector P = From + Axis * (static_cast<double>(S) / Stations);
		for (int32 C = 0; C < 4; ++C)
		{
			AddBar(Ism, P + Corners[C], P + Corners[(C + 1) % 4], TrussBrace);
		}
		if (S == Stations)
		{
			break;
		}
		const FVector Q = From + Axis * (static_cast<double>(S + 1) / Stations);
		for (int32 C = 0; C < 4; ++C)
		{
			const int32 N = (C + 1) % 4;
			if (S % 2 == 0)
			{
				AddBar(Ism, P + Corners[C], Q + Corners[N], TrussBrace);
			}
			else
			{
				AddBar(Ism, P + Corners[N], Q + Corners[C], TrussBrace);
			}
		}
	}
}

// ------------------------------------------------------------------------------------------------
// Geometry helpers (actor space)
// ------------------------------------------------------------------------------------------------

float AArenaVenue::RowRadius(int32 Row) const
{
	return FirstRowRadius + Row * RowDepth;
}

float AArenaVenue::HalfArcRad() const
{
	return FMath::DegreesToRadians(ArcDegrees) * 0.5f;
}

float AArenaVenue::AisleAngle(float Radius) const
{
	return HalfArcRad() + ArenaVenueDetail::AisleWidth * 0.5f / FMath::Max(Radius, 1.f);
}

float AArenaVenue::TrussHeight() const
{
	return StageHeight + ScreenBottom + ScreenHeight + ArenaVenueDetail::TrussAboveScreen;
}

FTransform AArenaVenue::ToWorld(const FTransform& LocalTransform) const
{
	FTransform Result = LocalTransform * GetActorTransform();
	Result.SetScale3D(FVector::OneVector);
	return Result;
}

FTransform AArenaVenue::GuestChairLocal() const
{
	// Stage right of the host, facing the audience, turned 20 degrees toward the centre (yaw 180 + 20).
	const float X = FMath::Min(320.f, StageDepth - 150.f);
	const float Y = FMath::Min(380.f, StageWidth * 0.5f - 150.f);
	return FTransform(FRotator(0.f, 200.f, 0.f), FVector(X, Y, StageHeight));
}

FTransform AArenaVenue::HostMarkLocal() const
{
	return FTransform(FRotator(0.f, 180.f, 0.f), FVector(FMath::Min(300.f, StageDepth - 150.f), 0.f, StageHeight));
}

FVector AArenaVenue::FollowSpotMountLocal(int32 Index) const
{
	// On the concourse behind the last row, +-40 degrees, well above the outer wall.
	const int32 Rows = FMath::Max(NumRows, 1);
	const float Radius = RowRadius(Rows - 1) + 0.45f * RowDepth + 100.f;
	const float Theta = FMath::DegreesToRadians(Index == 0 ? -40.f : 40.f);
	return ArenaVenueDetail::Polar(Radius, Theta, NumRows * RowRise + 900.f);
}

TArray<FVector> AArenaVenue::TrussLightMountsLocal() const
{
	using namespace ArenaVenueDetail;

	TArray<FVector> Mounts;
	Mounts.Reserve(NumTrussLights);
	const float Z = TrussHeight() - TrussSize * 0.5f - 45.f;
	const float Half = FMath::Max(StageWidth * 0.5f - 150.f, 100.f);
	for (int32 Index = 0; Index < NumTrussLights; ++Index)
	{
		const float Alpha = static_cast<float>(Index) / (NumTrussLights - 1);
		Mounts.Add(FVector(FrontTrussX, FMath::Lerp(-Half, Half, Alpha), Z));
	}
	return Mounts;
}

// ------------------------------------------------------------------------------------------------
// Queries (world space)
// ------------------------------------------------------------------------------------------------

FTransform AArenaVenue::GetSlotTransform(int32 Slot) const
{
	return ToWorld(SlotLocal.IsValidIndex(Slot) ? SlotLocal[Slot] : FTransform::Identity);
}

FVector AArenaVenue::GetSlotHeadLocation(int32 Slot) const
{
	if (!SlotLocal.IsValidIndex(Slot))
	{
		return GetActorLocation();
	}
	// seated head: 115 cm up, leaning 10 cm toward the stage
	return ToWorld(SlotLocal[Slot]).TransformPosition(FVector(10.f, 0.f, 115.f));
}

FString AArenaVenue::GetSlotLabel(int32 Slot) const
{
	return SlotLabels.IsValidIndex(Slot) ? SlotLabels[Slot] : FString();
}

int32 AArenaVenue::GetSlotRow(int32 Slot) const
{
	return SlotRows.IsValidIndex(Slot) ? SlotRows[Slot] : INDEX_NONE;
}

FTransform AArenaVenue::GetScreenTransform() const
{
	// yaw 180: the LED's +X points back at the audience (-X)
	return ToWorld(FTransform(FRotator(0.f, 180.f, 0.f), FVector(StageDepth - 60.f, 0.f, StageHeight + ScreenBottom + ScreenHeight * 0.5f)));
}

FTransform AArenaVenue::GetBannerTransform() const
{
	// centre 70 cm below the bottom edge of the LED, same plane and direction
	return ToWorld(FTransform(FRotator(0.f, 180.f, 0.f), FVector(StageDepth - 60.f, 0.f, StageHeight + ScreenBottom - 70.f)));
}

FTransform AArenaVenue::GetGuestChairTransform() const
{
	return ToWorld(GuestChairLocal());
}

FTransform AArenaVenue::GetHostMarkTransform() const
{
	return ToWorld(HostMarkLocal());
}

TArray<FVector> AArenaVenue::GetWalkPath(int32 FromSlot) const
{
	using namespace ArenaVenueDetail;

	TArray<FVector> Path;
	if (!SlotLocal.IsValidIndex(FromSlot) || !SlotRows.IsValidIndex(FromSlot))
	{
		return Path;
	}

	const int32 Row = SlotRows[FromSlot];
	const FVector Seat = SlotLocal[FromSlot].GetLocation();
	const float Floor = Row * RowRise;
	const float Theta = FMath::Atan2(static_cast<float>(Seat.Y), static_cast<float>(-Seat.X));
	const float LaneRadius = RowRadius(Row) - 0.5f * RowDepth;          // leg room in front of the seats
	const float FrontRadius = FMath::Max(FirstRowRadius - FrontWalkGap, 100.f);
	const float FrontLimit = AisleAngle(FrontRadius);

	const float StairY = StageWidth * 0.5f - StairInset;
	const FVector StairFoot(-(StairSteps * StairTread) - 40.f, StairY, 0.f);
	const FVector StairTop(40.f, StairY, StageHeight);
	const float FootTheta = FMath::Atan2(static_cast<float>(StairFoot.Y), static_cast<float>(-StairFoot.X));
	const float StairSide = StairY >= 0.f ? 1.f : -1.f;

	// 揀較近嗰條走廊：直接行去樓梯嗰邊，定係行去另一邊再沿前緣弧兜返過嚟（落走廊嗰段兩邊一樣長）。
	const float NearLength = LaneRadius * FMath::Abs(StairSide * AisleAngle(LaneRadius) - Theta);
	const float FarLength = LaneRadius * FMath::Abs(Theta + StairSide * AisleAngle(LaneRadius)) + FrontRadius * 2.f * FrontLimit;
	const float Side = FarLength < NearLength ? -StairSide : StairSide;

	// 1) stand up, step into the leg room, walk along the row to its end
	Path.Add(Seat);
	Path.Add(Polar(LaneRadius, Theta, Floor));
	AddArc(Path, LaneRadius, Theta, Side * AisleAngle(LaneRadius), Floor);

	// 2) down the aisle steps: walk to the front edge of each tread, then step down onto the next one
	for (int32 K = Row; K >= 1; --K)
	{
		const float Edge = RowRadius(K) - 0.55f * RowDepth;
		if (K != Row)
		{
			Path.Add(Polar(Edge + 12.f, Side * AisleAngle(Edge + 12.f), K * RowRise));
		}
		Path.Add(Polar(Edge - 20.f, Side * AisleAngle(Edge - 20.f), (K - 1) * RowRise));
	}

	// 3) out to the walkway in front of the bowl, along its arc toward the stairs side, to the stair foot
	const float FrontStart = Side * FrontLimit;
	Path.Add(Polar(FrontRadius, FrontStart, 0.f));
	AddArc(Path, FrontRadius, FrontStart, FMath::Clamp(FootTheta, -FrontLimit, FrontLimit), 0.f);
	Path.Add(StairFoot);

	// 4) up the stairs, across the deck, in front of the guest chair, then the chair itself
	Path.Add(StairTop);
	const FTransform Chair = GuestChairLocal();
	const FVector ChairForward = Chair.GetRotation().GetForwardVector();
	Path.Add(Chair.GetLocation() + FVector(ChairForward.X, ChairForward.Y, 0.0) * 70.0);
	Path.Add(Chair.GetLocation());

	const FTransform& ActorTransform = GetActorTransform();
	for (FVector& Point : Path)
	{
		Point = ActorTransform.TransformPosition(Point);
	}
	return Path;
}

FVector AArenaVenue::GetAudienceCentre() const
{
	return GetActorTransform().TransformPosition(AudienceCentreLocal);
}

FVector AArenaVenue::GetFollowSpotMount(int32 Index) const
{
	return GetActorTransform().TransformPosition(FollowSpotMountLocal(Index));
}

TArray<FVector> AArenaVenue::GetTrussLightMounts() const
{
	TArray<FVector> Mounts = TrussLightMountsLocal();
	const FTransform& ActorTransform = GetActorTransform();
	for (FVector& Mount : Mounts)
	{
		Mount = ActorTransform.TransformPosition(Mount);
	}
	return Mounts;
}
