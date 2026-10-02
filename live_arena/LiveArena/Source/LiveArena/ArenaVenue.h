#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ArenaVenue.generated.h"

class UInstancedStaticMeshComponent;
class UStaticMesh;
class UMaterialInterface;
class UMaterialInstanceDynamic;

/**
 * The concert hall, built procedurally from engine basic shapes so the project runs with zero assets.
 * Every look-defining mesh has a slot (SeatMesh, GuestChairMesh, ...) so AI-generated assets can be dropped in.
 *
 * World layout (cm, UE axes: X forward, Y right, Z up):
 *   - Stage front edge centre is the actor origin. Stage occupies X in [0, StageDepth], Y in [-StageWidth/2, StageWidth/2],
 *     top at Z = StageHeight.
 *   - LED screen hangs at X = StageDepth - 60, faces -X (toward the audience), bottom at StageHeight + ScreenBottom.
 *   - Audience rows are concentric arcs centred on the origin at negative X:
 *       radius(r) = FirstRowRadius + r * RowDepth, floor height(r) = r * RowRise,
 *       seats per row n(r) = floor(radius(r) * ArcDegrees(rad) / SeatSpacing),
 *       seat at position p has angle theta = (p - (n - 1) / 2) * SeatSpacing / radius(r)
 *       location = (-radius * cos(theta), radius * sin(theta), r * RowRise), yaw faces the origin.
 *   - Slots follow ArenaSlots (ArenaTypes.h): slot 0 = row A centre, filling centre-outward, then the next row.
 */
UCLASS()
class LIVEARENA_API AArenaVenue : public AActor
{
	GENERATED_BODY()

public:
	AArenaVenue();

	virtual void OnConstruction(const FTransform& Transform) override;

	// ---------------- layout ----------------
	UPROPERTY(EditAnywhere, Category = "Layout", meta = (ClampMin = "4", ClampMax = "60"))
	int32 NumRows = 28;

	UPROPERTY(EditAnywhere, Category = "Layout")
	float FirstRowRadius = 1400.f;

	UPROPERTY(EditAnywhere, Category = "Layout")
	float RowDepth = 95.f;

	UPROPERTY(EditAnywhere, Category = "Layout")
	float RowRise = 38.f;

	UPROPERTY(EditAnywhere, Category = "Layout")
	float SeatSpacing = 62.f;

	UPROPERTY(EditAnywhere, Category = "Layout", meta = (ClampMin = "30", ClampMax = "170"))
	float ArcDegrees = 110.f;

	UPROPERTY(EditAnywhere, Category = "Layout")
	float StageWidth = 2200.f;

	UPROPERTY(EditAnywhere, Category = "Layout")
	float StageDepth = 900.f;

	UPROPERTY(EditAnywhere, Category = "Layout")
	float StageHeight = 120.f;

	UPROPERTY(EditAnywhere, Category = "Layout")
	float ScreenWidth = 1800.f;

	UPROPERTY(EditAnywhere, Category = "Layout")
	float ScreenHeight = 1012.5f;

	/** Gap between the stage top and the bottom of the LED screen. */
	UPROPERTY(EditAnywhere, Category = "Layout")
	float ScreenBottom = 260.f;

	// ---------------- swappable meshes (leave empty for built-in blockout) ----------------
	/** One mesh for a whole seat (pivot at the floor, front facing +X). Replaces the cube blockout. */
	UPROPERTY(EditAnywhere, Category = "Meshes")
	TObjectPtr<UStaticMesh> SeatMesh;

	UPROPERTY(EditAnywhere, Category = "Meshes")
	FTransform SeatMeshOffset;

	UPROPERTY(EditAnywhere, Category = "Meshes")
	TObjectPtr<UStaticMesh> GuestChairMesh;

	UPROPERTY(EditAnywhere, Category = "Meshes")
	TObjectPtr<UStaticMesh> FollowSpotMesh;

	/** Optional material override for the blockout (must have a "Color" vector parameter, like BasicShapeMaterial). */
	UPROPERTY(EditAnywhere, Category = "Meshes")
	TObjectPtr<UMaterialInterface> BlockoutMaterial;

	// ---------------- queries (world space) ----------------
	int32 GetNumSlots() const { return SlotLocal.Num(); }
	const TArray<int32>& GetRowCounts() const { return RowCounts; }
	FTransform GetSlotTransform(int32 Slot) const;       // seat floor point, rotation facing the stage
	FVector GetSlotHeadLocation(int32 Slot) const;      // where a seated head is (~ +115 cm)
	FString GetSlotLabel(int32 Slot) const;
	int32 GetSlotRow(int32 Slot) const;

	FTransform GetScreenTransform() const;              // centre of the LED, X axis pointing at the audience
	FVector2D GetScreenSize() const { return FVector2D(ScreenWidth, ScreenHeight); }
	FTransform GetBannerTransform() const;              // strip just below the LED
	FTransform GetGuestChairTransform() const;          // seat point of the guest chair on stage
	FTransform GetHostMarkTransform() const;            // where the streamer "stands" (camera for StreamerPOV)
	/** Walk path for the winner from a seat to the guest chair: aisle, stage stairs, chair. */
	TArray<FVector> GetWalkPath(int32 FromSlot) const;
	FVector GetAudienceCentre() const;
	float GetOuterRadius() const { return FirstRowRadius + NumRows * RowDepth; }
	FVector GetFollowSpotMount(int32 Index) const;     // 0 = left, 1 = right, high at the back of the bowl
	TArray<FVector> GetTrussLightMounts() const;       // moving heads above the stage

private:
	void Build();
	void BuildSlots();
	void AddBox(UInstancedStaticMeshComponent* Ism, const FVector& Centre, const FVector& SizeCm, float YawDeg = 0.f);
	UInstancedStaticMeshComponent* MakeIsm(FName Name, UStaticMesh* Mesh, const FLinearColor& Color);

	/**
	 * A venue placed in a saved level is copied for PIE / loaded in a cooked game without re-running OnConstruction,
	 * and everything Build() makes is transient, so it is rebuilt here when missing.
	 */
	virtual void PostInitializeComponents() override;

	/** The instanced components one Build() fills (raw pointers, only alive during Build; Parts owns them). */
	struct FBuildIsms
	{
		UInstancedStaticMeshComponent* Floor = nullptr;       // floor slab, tiers, concourse
		UInstancedStaticMeshComponent* Walls = nullptr;       // stage body, stairs, outer / side / back walls
		UInstancedStaticMeshComponent* Black = nullptr;       // LED frame and legs
		UInstancedStaticMeshComponent* Truss = nullptr;       // trusses, towers, follow spot platforms
		UInstancedStaticMeshComponent* Brass = nullptr;       // stage front edge
		UInstancedStaticMeshComponent* BrassDisc = nullptr;   // host mark (cylinder)
		UInstancedStaticMeshComponent* DarkBrass = nullptr;   // step nosings, wall caps, pilasters
		UInstancedStaticMeshComponent* SeatRed = nullptr;     // seat cushions + stems, guest chair blockout
		UInstancedStaticMeshComponent* SeatBacks = nullptr;
		UInstancedStaticMeshComponent* Seats = nullptr;       // SeatMesh
		UInstancedStaticMeshComponent* GuestChair = nullptr;  // GuestChairMesh
		UInstancedStaticMeshComponent* Fixtures = nullptr;    // moving head bodies (no shadow)
		UInstancedStaticMeshComponent* FollowSpots = nullptr; // follow spot bodies (no shadow)
	};

	void BuildShell(const FBuildIsms& Isms);
	void BuildStage(const FBuildIsms& Isms);
	void BuildRig(const FBuildIsms& Isms);
	void BuildSeats(const FBuildIsms& Isms);

	void QueueInstance(UInstancedStaticMeshComponent* Ism, const FTransform& LocalTransform);
	void AddBar(UInstancedStaticMeshComponent* Ism, const FVector& From, const FVector& To, float Thickness);
	void AddTruss(UInstancedStaticMeshComponent* Ism, const FVector& From, const FVector& To);

	float RowRadius(int32 Row) const;
	float HalfArcRad() const;
	/** Angle (rad) of the aisle centre line at Radius: just past the end of the seat arc. */
	float AisleAngle(float Radius) const;
	float TrussHeight() const;
	FTransform ToWorld(const FTransform& LocalTransform) const;
	FTransform GuestChairLocal() const;
	FTransform HostMarkLocal() const;
	FVector FollowSpotMountLocal(int32 Index) const;
	TArray<FVector> TrussLightMountsLocal() const;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> CubeMesh;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> CylinderMesh;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> BasicShapeMaterial;

	/** Instances gathered during Build() and added with one AddInstances call per component. */
	TMap<UInstancedStaticMeshComponent*, TArray<FTransform>> PendingInstances;

	/** Centroid of the seated heads, actor space. */
	FVector AudienceCentreLocal = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, Category = "Arena")
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> Parts;

	/** Seat transforms in actor space, indexed by slot. */
	TArray<FTransform> SlotLocal;
	TArray<FString> SlotLabels;
	TArray<int32> SlotRows;
	TArray<int32> RowCounts;
};
