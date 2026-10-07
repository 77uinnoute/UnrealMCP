#include "Commands/Physics/UnrealMCPPhysicsAssetCommands.h"

#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Engine/SkeletalMesh.h"
#include "EngineUtils.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsAssetUtils.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PhysicsEngine/SphereElem.h"
#include "PhysicsEngine/SphylElem.h"
#include "ReferenceSkeleton.h"

namespace
{
    /** Resolve the `asset_path` parameter to a UPhysicsAsset, or fill OutError. */
    UPhysicsAsset* ResolvePhysicsAsset(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutError)
    {
        FString AssetPath;
        if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
        {
            OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"),
                TEXT("Missing 'asset_path' parameter (a /Game path to a PhysicsAsset)"));
            return nullptr;
        }

        UObject* Object = FUnrealMCPCommonUtils::FindAsset(AssetPath);
        UPhysicsAsset* PhysicsAsset = Cast<UPhysicsAsset>(Object);
        if (!PhysicsAsset)
        {
            OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("asset_not_physics_asset"),
                FString::Printf(TEXT("'%s' resolved to %s, not a PhysicsAsset"),
                    *AssetPath, Object ? *Object->GetClass()->GetName() : TEXT("<nothing>")));
            return nullptr;
        }
        return PhysicsAsset;
    }

    /** The first skeletal mesh whose PhysicsAsset is this asset (used for bone/child lookups). */
    USkeletalMesh* FindMeshFor(const UPhysicsAsset* PhysicsAsset)
    {
        for (TObjectIterator<USkeletalMesh> It; It; ++It)
        {
            if (It->GetPhysicsAsset() == PhysicsAsset)
            {
                return *It;
            }
        }
        return nullptr;
    }

    /**
     * Body lookup that does NOT trust UPhysicsAsset::BodySetupIndexMap.
     *
     * That map is a cache (PhysicsAsset.h:254), rebuilt only in PostLoad and by
     * UpdateBodySetupIndexMap(), while FindBodyIndex (PhysicsAsset.cpp:455-464) returns whatever index
     * the cache holds without validating it against SkeletalBodySetups. Any add/remove that mutates the
     * array and forgets to refresh the cache therefore makes FindBodyIndex hand out out-of-range
     * indices, and a caller doing SkeletalBodySetups[Index] / RemoveAt(Index) trips the Array.h:1095
     * assert ("Array index out of bounds: N into an array of size N") and takes the editor down.
     * The array itself is authoritative, so scan it.
     */
    int32 FindBodyIndexByName(const UPhysicsAsset* PhysicsAsset, const FName& BoneName)
    {
        for (int32 Index = 0; Index < PhysicsAsset->SkeletalBodySetups.Num(); ++Index)
        {
            if (const USkeletalBodySetup* Body = Cast<USkeletalBodySetup>(PhysicsAsset->SkeletalBodySetups[Index].Get()))
            {
                if (Body->BoneName == BoneName)
                {
                    return Index;
                }
            }
        }
        return INDEX_NONE;
    }

    /**
     * Rebuild every int32 index cache UPhysicsAsset keeps beside SkeletalBodySetups.
     *
     * There are two of them and neither is serialized, so both are only ever rebuilt on
     * PostLoad/editor property change:
     *   - BodySetupIndexMap (PhysicsAsset.h:255) -> read by FindBodyIndex, which does NOT range-check;
     *   - BoundsBodies (PhysicsAsset.h:206) -> index list of bConsiderForBounds bodies, consumed when the
     *     component builds/updates its bounds.
     * Any add/remove that skips them leaves stale indices behind, which then crash the editor with
     * "Array index out of bounds: N into an array of size N" - from FindBodyIndex/RemoveAt on the next
     * removal, or from the Engine when PIE starts and the bounds are computed.
     */
    void RefreshBodyIndexCaches(UPhysicsAsset* PhysicsAsset)
    {
        PhysicsAsset->UpdateBodySetupIndexMap();
        PhysicsAsset->UpdateBoundsBodiesArray();
    }

    ELinearConstraintMotion ParseLinear(const FString& Value, bool& bOutValid)
    {
        bOutValid = true;
        if (Value.Equals(TEXT("locked"), ESearchCase::IgnoreCase)) { return LCM_Locked; }
        if (Value.Equals(TEXT("free"), ESearchCase::IgnoreCase)) { return LCM_Free; }
        if (Value.Equals(TEXT("limited"), ESearchCase::IgnoreCase)) { return LCM_Limited; }
        bOutValid = false;
        return LCM_Locked;
    }

    void AddShapeSummary(const UBodySetup* Body, TSharedPtr<FJsonObject>& Out)
    {
        if (!Body)
        {
            return;
        }
        Out->SetNumberField(TEXT("sphere_count"), Body->AggGeom.SphereElems.Num());
        Out->SetNumberField(TEXT("capsule_count"), Body->AggGeom.SphylElems.Num());
        Out->SetNumberField(TEXT("box_count"), Body->AggGeom.BoxElems.Num());
        if (Body->AggGeom.SphereElems.Num() > 0)
        {
            Out->SetNumberField(TEXT("sphere_radius"), Body->AggGeom.SphereElems[0].Radius);
        }
        if (Body->AggGeom.SphylElems.Num() > 0)
        {
            const FKSphylElem& Capsule = Body->AggGeom.SphylElems[0];
            Out->SetNumberField(TEXT("capsule_radius"), Capsule.Radius);
            Out->SetNumberField(TEXT("capsule_length"), Capsule.Length);
            TArray<TSharedPtr<FJsonValue>> Center;
            Center.Add(MakeShared<FJsonValueNumber>(Capsule.Center.X));
            Center.Add(MakeShared<FJsonValueNumber>(Capsule.Center.Y));
            Center.Add(MakeShared<FJsonValueNumber>(Capsule.Center.Z));
            Out->SetArrayField(TEXT("capsule_center"), Center);
        }
    }
}

FUnrealMCPPhysicsAssetCommands::FUnrealMCPPhysicsAssetCommands()
{
}

void FUnrealMCPPhysicsAssetCommands::RegisterCommands(FMCPCommandRegistry& Registry)
{
    MCP_REGISTER_COMMAND(Registry, "add_physics_asset_body", "physics",
        "Add a collision body (sphere, or a capsule aligned to the bone) for one bone of a PhysicsAsset, plus its constraint tools' prerequisites. This is the only scriptable way to author PhysicsAsset bodies: FPhysicsAssetUtils::CreateNewBody has no python binding. Nothing is simulated by this command - a RigidBody node or a physical animation component still has to be pointed at the asset.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("PhysicsAsset path, e.g. /Game/MCP/Ganyu/Ganyu_UE_PhysicsAsset")),
            MCPParam(TEXT("bone_name"), TEXT("string"), TEXT("Bone to create the body for")),
            MCPParam(TEXT("radius"), TEXT("number"), TEXT("Sphere/capsule radius in cm")),
            MCPParamOpt(TEXT("length"), TEXT("number"), TEXT("Capsule segment length in cm; 0 (default) creates a sphere")),
            MCPParamOpt(TEXT("mesh_path"), TEXT("string"), TEXT("Mesh used to resolve the bone (default: the loaded mesh whose PhysicsAsset is this asset)")),
            MCPParamOpt(TEXT("simulate"), TEXT("bool"), TEXT("Mark the body PhysType_Simulated (dynamic in a RigidBody node); false = PhysType_Default (kinematic, i.e. a collider driven by the animation)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save the asset after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, false, false, true),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleAddBody(Params); });

    MCP_REGISTER_COMMAND(Registry, "add_physics_asset_constraint", "physics",
        "Add a constraint between two already-present bodies of a PhysicsAsset (child bone drives the constraint's name, as in the editor). Linear axes are locked or free; the three angular limits are set in degrees.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("PhysicsAsset path")),
            MCPParam(TEXT("child_bone"), TEXT("string"), TEXT("Bone whose body is constrained (ConstraintBone1)")),
            MCPParam(TEXT("parent_bone"), TEXT("string"), TEXT("Bone it is constrained to (ConstraintBone2); must already have a body")),
            MCPParamOpt(TEXT("swing1_deg"), TEXT("number"), TEXT("Swing1 limit in degrees (default 30)")),
            MCPParamOpt(TEXT("swing2_deg"), TEXT("number"), TEXT("Swing2 limit in degrees (default 30)")),
            MCPParamOpt(TEXT("twist_deg"), TEXT("number"), TEXT("Twist limit in degrees (default 20)")),
            MCPParamOpt(TEXT("linear"), TEXT("string"), TEXT("'locked' (default) or 'free'")),
            MCPParamOpt(TEXT("disable_collision"), TEXT("bool"), TEXT("Disable collision between the two bodies (default true)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save the asset after the command; default true")),
        }), MCPFlags(false, false, false, true),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleAddConstraint(Params); });

    MCP_REGISTER_COMMAND(Registry, "set_physics_asset_collision", "physics",
        "Disable (or re-enable) collision between one body of a PhysicsAsset and a list of other bodies - every other body when the list is omitted. The only scriptable way to narrow what a body collides with: the pairs live in UPhysicsAsset::CollisionDisableTable, a map keyed by a struct the property reflector cannot address, and the solver-side switch only covers the pair a constraint joins.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("PhysicsAsset path")),
            MCPParam(TEXT("bone_name"), TEXT("string"), TEXT("Body whose collision set is changed")),
            MCPParamOpt(TEXT("other_bones"), TEXT("array"), TEXT("Bones to pair with; omitted or empty = every other body of the asset")),
            MCPParamOpt(TEXT("disable"), TEXT("bool"), TEXT("true (default) disables those pairs, false re-enables them")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save the asset after the command; default true")),
        }), MCPFlags(false, false, false, true),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleSetCollision(Params); });

    MCP_REGISTER_COMMAND(Registry, "remove_physics_asset_body", "physics",
        "Remove a body from a PhysicsAsset for good, together with every constraint that referenced it and its collision-table entries (the remaining table indices are renumbered so each key still addresses the same pair). Until this existed a body could only be parked as PhysType_Kinematic.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("PhysicsAsset path")),
            MCPParam(TEXT("bone_name"), TEXT("string"), TEXT("Bone whose body is removed")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save the asset after the command; default true")),
        }), MCPFlags(false, false, false, true),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleRemoveBody(Params); });

    MCP_REGISTER_COMMAND(Registry, "remove_physics_asset_constraint", "physics",
        "Remove one constraint of a PhysicsAsset, identified by the same (child, parent) bone pair add_physics_asset_constraint uses.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("PhysicsAsset path")),
            MCPParam(TEXT("child_bone"), TEXT("string"), TEXT("ConstraintBone1 of the constraint to remove")),
            MCPParam(TEXT("parent_bone"), TEXT("string"), TEXT("ConstraintBone2 of the constraint to remove")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save the asset after the command; default true")),
        }), MCPFlags(false, false, false, true),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleRemoveConstraint(Params); });

    MCP_REGISTER_COMMAND(Registry, "list_physics_asset_bodies", "physics",
        "Read back every body (bone, shapes, radii) and constraint (bones, limits) of a PhysicsAsset in one call.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("PhysicsAsset path")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleListBodies(Params); });
}

TSharedPtr<FJsonObject> FUnrealMCPPhysicsAssetCommands::HandleAddBody(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UPhysicsAsset* PhysicsAsset = ResolvePhysicsAsset(Params, Error);
    if (!PhysicsAsset)
    {
        return Error;
    }

    FString BoneName;
    double Radius = 0.0;
    if (!Params->TryGetStringField(TEXT("bone_name"), BoneName) ||
        !Params->TryGetNumberField(TEXT("radius"), Radius) || Radius <= 0.0)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"),
            TEXT("'bone_name' (string) and a positive 'radius' are required"));
    }
    double Length = 0.0;
    Params->TryGetNumberField(TEXT("length"), Length);
    bool bSimulate = false;
    const bool bHasSimulate = Params->TryGetBoolField(TEXT("simulate"), bSimulate);
    const bool bPersist = FUnrealMCPCommonUtils::IsPersistRequested(Params);

    // Clearing these entries is what makes this repairable. Two traps meet here:
    //   * `FPhysAssetCreateParams::bDisableCollisionsByDefault` defaults to true (PhysicsAssetUtils.h:52)
    //     and `CreateNewBody` then disables collision between the new body and every other body
    //     (PhysicsAssetUtils.cpp:1219-1225);
    //   * the table's semantics are "**the key being present means disabled**" - `DisableCollision`
    //     stores `Add(Key, 0)` and the lookup is a plain `Find(Key)` (PhysicsAsset.cpp:199-224), so the
    //     stored bool carries no meaning.
    auto ClearDisabledPairs = [PhysicsAsset](int32 BodyIndex)
    {
        int32 Removed = 0;
        for (auto It = PhysicsAsset->CollisionDisableTable.CreateIterator(); It; ++It)
        {
            const FRigidBodyIndexPair& Pair = It->Key;
            if (Pair.Indices[0] == BodyIndex || Pair.Indices[1] == BodyIndex)
            {
                It.RemoveCurrent();
                ++Removed;
            }
        }
        return Removed;
    };

    const int32 ExistingIndex = FindBodyIndexByName(PhysicsAsset, FName(*BoneName));
    if (ExistingIndex != INDEX_NONE)
    {
        USkeletalBodySetup* Existing = Cast<USkeletalBodySetup>(PhysicsAsset->SkeletalBodySetups[ExistingIndex].Get());
        const int32 EnabledPairs = ClearDisabledPairs(ExistingIndex);
        if (bHasSimulate && Existing)
        {
            Existing->PhysicsType = bSimulate ? PhysType_Simulated : PhysType_Default;
        }
        PhysicsAsset->MarkPackageDirty();
        const bool bSaved = bPersist ? FUnrealMCPCommonUtils::SaveAssetForObject(PhysicsAsset) : false;

        TSharedPtr<FJsonObject> BodyJson = MakeShared<FJsonObject>();
        BodyJson->SetNumberField(TEXT("body_index"), ExistingIndex);
        BodyJson->SetStringField(TEXT("bone_name"), BoneName);
        AddShapeSummary(Existing, BodyJson);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("created"), false);
        Result->SetBoolField(TEXT("repaired"), true);
        Result->SetNumberField(TEXT("collision_pairs_enabled"), EnabledPairs);
        Result->SetObjectField(TEXT("body"), BodyJson);
        Result->SetNumberField(TEXT("body_count"), PhysicsAsset->SkeletalBodySetups.Num());
        Result->SetBoolField(TEXT("saved"), bSaved);
        Result->SetBoolField(TEXT("persist_requested"), bPersist);
        return Result;
    }

    // Resolve the mesh so the capsule can be aligned with the bone: the child bone's local translation
    // (from the reference skeleton) is the bone direction expressed in this bone's own space.
    FString MeshPath;
    Params->TryGetStringField(TEXT("mesh_path"), MeshPath);
    USkeletalMesh* Mesh = MeshPath.IsEmpty() ? FindMeshFor(PhysicsAsset) : Cast<USkeletalMesh>(FUnrealMCPCommonUtils::FindAsset(MeshPath));
    if (Length > 0.0 && !Mesh)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("mesh_not_found"),
            TEXT("a capsule needs a mesh to resolve the bone direction: pass mesh_path (no loaded mesh references this PhysicsAsset)"));
    }

    FPhysAssetCreateParams CreateParams;
    // Default is true, which would disable collision between this body and every other body in the asset
    // (PhysicsAssetUtils.cpp:1219-1225) - the opposite of what a collider body is for.
    CreateParams.bDisableCollisionsByDefault = false;
    const int32 BodyIndex = FPhysicsAssetUtils::CreateNewBody(PhysicsAsset, FName(*BoneName), CreateParams);
    USkeletalBodySetup* Body = PhysicsAsset->SkeletalBodySetups.IsValidIndex(BodyIndex)
        ? Cast<USkeletalBodySetup>(PhysicsAsset->SkeletalBodySetups[BodyIndex].Get())
        : nullptr;
    if (!Body)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("create_failed"),
            FString::Printf(TEXT("CreateNewBody returned index %d but no body setup is there"), BodyIndex));
    }

    FString ShapeKind;
    if (Length > 0.0)
    {
        const FReferenceSkeleton& ReferenceSkeleton = Mesh->GetRefSkeleton();
        const int32 BoneIndex = ReferenceSkeleton.FindBoneIndex(FName(*BoneName));
        FVector Direction = FVector(0.0, 0.0, 1.0);
        bool bDirectionResolved = false;
        for (int32 Index = 0; Index < ReferenceSkeleton.GetNum(); ++Index)
        {
            if (ReferenceSkeleton.GetParentIndex(Index) == BoneIndex)
            {
                Direction = ReferenceSkeleton.GetRefBonePose()[Index].GetTranslation().GetSafeNormal();
                bDirectionResolved = true;
                break;
            }
        }
        if (!bDirectionResolved)
        {
            // Leaf bone: no child to read a direction from, so fall back to the offset this bone's own
            // parent gave it, expressed in this bone's space - the same "forward along the chain" the
            // non-leaf branch gets. Without this the capsule keeps the (0,0,1) initialiser and sticks
            // out sideways at 90 degrees to the chain.
            const FTransform& OwnLocal = ReferenceSkeleton.GetRefBonePose()[BoneIndex];
            const FVector Forward = OwnLocal.GetRotation().UnrotateVector(OwnLocal.GetTranslation());
            if (!Forward.IsNearlyZero())
            {
                Direction = Forward.GetSafeNormal();
            }
        }

        // FKSphylElem is a capsule along its local Z (SphylElem.h:36-93): rotate Z onto the bone direction
        // and centre the segment on the bone.
        FKSphylElem Capsule(static_cast<float>(Radius), static_cast<float>(Length));
        Capsule.SetTransform(FTransform(FQuat::FindBetweenNormals(FVector(0.0, 0.0, 1.0), Direction),
                                        Direction * (Length * 0.5)));
        Body->AggGeom.SphylElems.Add(Capsule);
        ShapeKind = TEXT("capsule");
    }
    else
    {
        Body->AggGeom.SphereElems.Add(FKSphereElem(static_cast<float>(Radius)));
        ShapeKind = TEXT("sphere");
    }

    Body->BoneName = FName(*BoneName);
    if (bHasSimulate)
    {
        Body->PhysicsType = bSimulate ? PhysType_Simulated : PhysType_Default;
    }
    Body->InvalidatePhysicsData();
    RefreshBodyIndexCaches(PhysicsAsset);
    PhysicsAsset->InvalidateAllPhysicsMeshes();
    PhysicsAsset->MarkPackageDirty();

    TSharedPtr<FJsonObject> BodyJson = MakeShared<FJsonObject>();
    BodyJson->SetNumberField(TEXT("body_index"), BodyIndex);
    BodyJson->SetStringField(TEXT("bone_name"), BoneName);
    BodyJson->SetStringField(TEXT("shape"), ShapeKind);
    AddShapeSummary(Body, BodyJson);

    const bool bSaved = bPersist ? FUnrealMCPCommonUtils::SaveAssetForObject(PhysicsAsset) : false;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("created"), true);
    Result->SetObjectField(TEXT("body"), BodyJson);
    Result->SetNumberField(TEXT("body_count"), PhysicsAsset->SkeletalBodySetups.Num());
    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetBoolField(TEXT("persist_requested"), bPersist);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPPhysicsAssetCommands::HandleAddConstraint(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UPhysicsAsset* PhysicsAsset = ResolvePhysicsAsset(Params, Error);
    if (!PhysicsAsset)
    {
        return Error;
    }

    FString ChildBone;
    FString ParentBone;
    if (!Params->TryGetStringField(TEXT("child_bone"), ChildBone) ||
        !Params->TryGetStringField(TEXT("parent_bone"), ParentBone))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"),
            TEXT("'child_bone' and 'parent_bone' are required"));
    }

    const int32 ChildBodyIndex = FindBodyIndexByName(PhysicsAsset, FName(*ChildBone));
    const int32 ParentBodyIndex = FindBodyIndexByName(PhysicsAsset, FName(*ParentBone));
    if (ChildBodyIndex == INDEX_NONE || ParentBodyIndex == INDEX_NONE)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("body_not_found"),
            FString::Printf(TEXT("both bones need bodies first: '%s' %s, '%s' %s"),
                *ChildBone, ChildBodyIndex == INDEX_NONE ? TEXT("missing") : TEXT("ok"),
                *ParentBone, ParentBodyIndex == INDEX_NONE ? TEXT("missing") : TEXT("ok")));
    }

    if (PhysicsAsset->FindConstraintIndex(FName(*ChildBone), FName(*ParentBone)) != INDEX_NONE)
    {
        // Re-initialised below instead of refused: the joint frames are what makes the constraint work,
        // so an existing constraint is exactly the case that must be repairable.
    }

    double Swing1 = 30.0, Swing2 = 30.0, Twist = 20.0;
    Params->TryGetNumberField(TEXT("swing1_deg"), Swing1);
    Params->TryGetNumberField(TEXT("swing2_deg"), Swing2);
    Params->TryGetNumberField(TEXT("twist_deg"), Twist);
    FString LinearText = TEXT("locked");
    Params->TryGetStringField(TEXT("linear"), LinearText);
    bool bLinearValid = false;
    const ELinearConstraintMotion LinearMotion = ParseLinear(LinearText, bLinearValid);
    if (!bLinearValid)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unknown_value"),
            FString::Printf(TEXT("'linear' must be locked / free / limited, got '%s'"), *LinearText));
    }
    bool bDisableCollision = true;
    Params->TryGetBoolField(TEXT("disable_collision"), bDisableCollision);
    const bool bPersist = FUnrealMCPCommonUtils::IsPersistRequested(Params);

    // A constraint is identified by its (ConstraintBone1, ConstraintBone2) pair, so that is what decides
    // "re-initialise" vs "append". Do NOT route this through FPhysicsAssetUtils::CreateNewConstraint():
    // it looks the template up by JointName - which this command sets to the child bone - and returns the
    // existing template instead of appending a new one (PhysicsAssetUtils.cpp:1444-1451). Calling it twice
    // for the same child therefore silently rewrites the first joint, so a body could never be the child
    // of two constraints (an in-column joint *and* a lateral joint to the neighbouring column).
    const int32 ExistingConstraintIndex =
        PhysicsAsset->FindConstraintIndex(FName(*ChildBone), FName(*ParentBone));
    const bool bIsNew = ExistingConstraintIndex == INDEX_NONE;
    int32 ConstraintIndex = ExistingConstraintIndex;
    if (bIsNew)
    {
        if (!FPhysicsAssetUtils::CanCreateConstraints())
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("create_failed"),
                TEXT("FPhysicsAssetUtils::CanCreateConstraints() refused another constraint template"));
        }
        UPhysicsConstraintTemplate* NewTemplate =
            NewObject<UPhysicsConstraintTemplate>(PhysicsAsset, NAME_None, RF_Transactional);
        if (!NewTemplate)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("create_failed"),
                TEXT("could not allocate a UPhysicsConstraintTemplate"));
        }
        ConstraintIndex = PhysicsAsset->ConstraintSetup.Add(NewTemplate);
        // JointName has to stay unique: FindConstraintIndex(FName) matches on it, so two templates sharing
        // a name are indistinguishable to the editor and to the profile lookups.
        NewTemplate->DefaultInstance.JointName =
            FName(*FString::Printf(TEXT("%s_%s"), *ChildBone, *ParentBone));
    }
    UPhysicsConstraintTemplate* Template = PhysicsAsset->ConstraintSetup.IsValidIndex(ConstraintIndex)
        ? PhysicsAsset->ConstraintSetup[ConstraintIndex].Get()
        : nullptr;
    if (!Template)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("create_failed"),
            FString::Printf(TEXT("CreateNewConstraint returned index %d but no template is there"), ConstraintIndex));
    }

    FConstraintInstance& Instance = Template->DefaultInstance;
    Instance.ConstraintBone1 = FName(*ChildBone);
    Instance.ConstraintBone2 = FName(*ParentBone);
    Instance.SetLinearXMotion(LinearMotion);
    Instance.SetLinearYMotion(LinearMotion);
    Instance.SetLinearZMotion(LinearMotion);
    Instance.SetAngularSwing1Limit(ACM_Limited, static_cast<float>(Swing1));
    Instance.SetAngularSwing2Limit(ACM_Limited, static_cast<float>(Swing2));
    Instance.SetAngularTwistLimit(ACM_Limited, static_cast<float>(Twist));
    Instance.SetDisableCollision(bDisableCollision);

    // The joint frames. `CreateNewConstraint` only makes an empty template; the asset's own auto-create
    // path initialises the frames right after creating the constraint and before setting the default
    // profile (PhysicsUtilities/Private/PhysicsAssetUtils.cpp:427-435). Without this the frames stay zero
    // and every joint drags the child body onto the parent's origin - the whole chain collapses to a point.
    Instance.SnapTransformsToDefault(EConstraintTransformComponentFlags::All, PhysicsAsset);
    Template->SetDefaultProfile(Instance);
    if (bDisableCollision)
    {
        PhysicsAsset->DisableCollision(ChildBodyIndex, ParentBodyIndex);
    }

    PhysicsAsset->MarkPackageDirty();
    const bool bSaved = bPersist ? FUnrealMCPCommonUtils::SaveAssetForObject(PhysicsAsset) : false;

    TSharedPtr<FJsonObject> ConstraintJson = MakeShared<FJsonObject>();
    ConstraintJson->SetNumberField(TEXT("constraint_index"), ConstraintIndex);
    ConstraintJson->SetStringField(TEXT("child_bone"), ChildBone);
    ConstraintJson->SetStringField(TEXT("parent_bone"), ParentBone);
    ConstraintJson->SetNumberField(TEXT("swing1_deg"), Swing1);
    ConstraintJson->SetNumberField(TEXT("swing2_deg"), Swing2);
    ConstraintJson->SetNumberField(TEXT("twist_deg"), Twist);
    ConstraintJson->SetStringField(TEXT("linear"), LinearText.ToLower());
    ConstraintJson->SetBoolField(TEXT("disable_collision"), bDisableCollision);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("created"), bIsNew);
    Result->SetBoolField(TEXT("reinitialized"), !bIsNew);
    Result->SetObjectField(TEXT("constraint"), ConstraintJson);
    Result->SetNumberField(TEXT("constraint_count"), PhysicsAsset->ConstraintSetup.Num());
    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetBoolField(TEXT("persist_requested"), bPersist);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPPhysicsAssetCommands::HandleSetCollision(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UPhysicsAsset* PhysicsAsset = ResolvePhysicsAsset(Params, Error);
    if (!PhysicsAsset)
    {
        return Error;
    }

    FString BoneName;
    if (!Params->TryGetStringField(TEXT("bone_name"), BoneName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"),
            TEXT("'bone_name' (string) is required"));
    }
    const int32 BodyIndex = FindBodyIndexByName(PhysicsAsset, FName(*BoneName));
    if (BodyIndex == INDEX_NONE)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("body_not_found"),
            FString::Printf(TEXT("'%s' has no body in this asset"), *BoneName));
    }

    bool bDisable = true;
    Params->TryGetBoolField(TEXT("disable"), bDisable);
    const bool bPersist = FUnrealMCPCommonUtils::IsPersistRequested(Params);

    // Which bodies to pair with: the named list, or every other body of the asset.
    //
    // Unknown names used to fail the whole call, which meant one typo in a long list silently threw
    // away every edit before it (measured: a sleeve BLOCK pass applied nothing at all and the caller
    // only noticed days later). Each entry is reported instead: the resolvable ones are applied, the
    // rest come back in `unknown`.
    TArray<int32> Targets;
    TArray<FString> AppliedNames;
    TArray<FString> Unknown;
    const TArray<TSharedPtr<FJsonValue>>* OtherBones = nullptr;
    const bool bHasExplicitList = Params->TryGetArrayField(TEXT("other_bones"), OtherBones) && OtherBones->Num() > 0;
    if (bHasExplicitList)
    {
        for (const TSharedPtr<FJsonValue>& Value : *OtherBones)
        {
            const FString Other = Value->AsString();
            const int32 OtherIndex = FindBodyIndexByName(PhysicsAsset, FName(*Other));
            if (OtherIndex == INDEX_NONE)
            {
                Unknown.Add(Other);
            }
            else if (OtherIndex != BodyIndex)
            {
                Targets.AddUnique(OtherIndex);
                AppliedNames.AddUnique(Other);
            }
        }

        if (Targets.Num() == 0)
        {
            TSharedPtr<FJsonObject> NoMatch = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("no_matching_bodies"),
                FString::Printf(TEXT("none of the requested bones has a body in this asset (unknown: %s)"),
                    *FString::Join(Unknown, TEXT(", "))));
            FUnrealMCPCommonUtils::AddStringArrayField(NoMatch, TEXT("unknown"), Unknown);
            return NoMatch;
        }
    }
    else
    {
        for (int32 Index = 0; Index < PhysicsAsset->SkeletalBodySetups.Num(); ++Index)
        {
            if (Index != BodyIndex)
            {
                Targets.Add(Index);
            }
        }
    }

    for (const int32 Other : Targets)
    {
        if (bDisable)
        {
            PhysicsAsset->DisableCollision(BodyIndex, Other);
        }
        else
        {
            PhysicsAsset->EnableCollision(BodyIndex, Other);
        }
    }

    PhysicsAsset->MarkPackageDirty();
    const bool bSaved = bPersist ? FUnrealMCPCommonUtils::SaveAssetForObject(PhysicsAsset) : false;

    // Read the table back: the write is a key insertion/removal, so "which bones is this body paired
    // with now" is the only meaningful report.
    TArray<TSharedPtr<FJsonValue>> PairedWith;
    for (const TPair<FRigidBodyIndexPair, bool>& Pair : PhysicsAsset->CollisionDisableTable)
    {
        const int32 Other = Pair.Key.Indices[0] == BodyIndex ? Pair.Key.Indices[1]
                          : (Pair.Key.Indices[1] == BodyIndex ? Pair.Key.Indices[0] : INDEX_NONE);
        if (Other != INDEX_NONE && PhysicsAsset->SkeletalBodySetups.IsValidIndex(Other))
        {
            const USkeletalBodySetup* OtherBody = Cast<USkeletalBodySetup>(PhysicsAsset->SkeletalBodySetups[Other].Get());
            PairedWith.Add(MakeShared<FJsonValueString>(OtherBody ? OtherBody->BoneName.ToString() : FString::FromInt(Other)));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("bone_name"), BoneName);
    Result->SetNumberField(TEXT("body_index"), BodyIndex);
    Result->SetBoolField(TEXT("disabled"), bDisable);
    Result->SetNumberField(TEXT("pairs_changed"), Targets.Num());
    Result->SetNumberField(TEXT("applied_count"), Targets.Num());
    TArray<TSharedPtr<FJsonValue>> AppliedJson;
    for (const FString& Applied : AppliedNames)
    {
        AppliedJson.Add(MakeShared<FJsonValueString>(Applied));
    }
    Result->SetArrayField(TEXT("applied"), AppliedJson);
    FUnrealMCPCommonUtils::AddStringArrayField(Result, TEXT("unknown"), Unknown);
    Result->SetNumberField(TEXT("collision_disabled_pairs"), PairedWith.Num());
    Result->SetArrayField(TEXT("collision_disabled_with"), PairedWith);
    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetBoolField(TEXT("persist_requested"), bPersist);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPPhysicsAssetCommands::HandleRemoveBody(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UPhysicsAsset* PhysicsAsset = ResolvePhysicsAsset(Params, Error);
    if (!PhysicsAsset)
    {
        return Error;
    }

    FString BoneName;
    if (!Params->TryGetStringField(TEXT("bone_name"), BoneName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"),
            TEXT("'bone_name' (string) is required"));
    }
    const int32 BodyIndex = FindBodyIndexByName(PhysicsAsset, FName(*BoneName));
    if (BodyIndex == INDEX_NONE)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("body_not_found"),
            FString::Printf(TEXT("'%s' has no body in this asset"), *BoneName));
    }
    const bool bPersist = FUnrealMCPCommonUtils::IsPersistRequested(Params);
    const FName BoneFName(*BoneName);

    // Constraints first: a template names its two bones, so it has to be found by name. Nothing else in
    // the asset points at the body by name, which is why the order here is constraints -> table -> body.
    int32 RemovedConstraints = 0;
    for (int32 Index = PhysicsAsset->ConstraintSetup.Num() - 1; Index >= 0; --Index)
    {
        const UPhysicsConstraintTemplate* Template = PhysicsAsset->ConstraintSetup[Index].Get();
        if (!Template)
        {
            continue;
        }
        const FConstraintInstance& Instance = Template->DefaultInstance;
        if (Instance.ConstraintBone1 == BoneFName || Instance.ConstraintBone2 == BoneFName)
        {
            PhysicsAsset->ConstraintSetup.RemoveAt(Index);
            ++RemovedConstraints;
        }
    }

    // Drop the pairs that involve this body BEFORE the array shrinks, otherwise the index in those keys
    // would point at a different body by the time they are tested.
    int32 RemovedPairs = 0;
    for (auto It = PhysicsAsset->CollisionDisableTable.CreateIterator(); It; ++It)
    {
        const FRigidBodyIndexPair& Pair = It->Key;
        if (Pair.Indices[0] == BodyIndex || Pair.Indices[1] == BodyIndex)
        {
            It.RemoveCurrent();
            ++RemovedPairs;
        }
    }

    PhysicsAsset->SkeletalBodySetups.RemoveAt(BodyIndex);
    // Refresh both index caches: BodySetupIndexMap keeps FindBodyIndex honest (it does not range-check,
    // so a stale entry makes the next removal crash), and BoundsBodies holds bare indices that the Engine
    // walks when it computes the mesh bounds - stale ones crash PIE with "index N into an array of size N".
    RefreshBodyIndexCaches(PhysicsAsset);

    // ... and now renumber. CollisionDisableTable is keyed by bare indices into SkeletalBodySetups, so
    // every key above the removed index now addresses the wrong pair unless it is shifted down. This is
    // the step that silently corrupts the collision set when it is forgotten.
    TMap<FRigidBodyIndexPair, bool> Renumbered;
    Renumbered.Reserve(PhysicsAsset->CollisionDisableTable.Num());
    for (const TPair<FRigidBodyIndexPair, bool>& Pair : PhysicsAsset->CollisionDisableTable)
    {
        FRigidBodyIndexPair Key = Pair.Key;
        for (int32 Axis = 0; Axis < 2; ++Axis)
        {
            if (Key.Indices[Axis] > BodyIndex)
            {
                --Key.Indices[Axis];
            }
        }
        Renumbered.Add(Key, Pair.Value);
    }
    PhysicsAsset->CollisionDisableTable = MoveTemp(Renumbered);

    PhysicsAsset->MarkPackageDirty();
    const bool bSaved = bPersist ? FUnrealMCPCommonUtils::SaveAssetForObject(PhysicsAsset) : false;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("bone_name"), BoneName);
    Result->SetNumberField(TEXT("removed_body_index"), BodyIndex);
    Result->SetBoolField(TEXT("removed_body"), true);
    Result->SetNumberField(TEXT("removed_constraints"), RemovedConstraints);
    Result->SetNumberField(TEXT("removed_collision_pairs"), RemovedPairs);
    Result->SetNumberField(TEXT("body_count"), PhysicsAsset->SkeletalBodySetups.Num());
    Result->SetNumberField(TEXT("constraint_count"), PhysicsAsset->ConstraintSetup.Num());
    Result->SetNumberField(TEXT("collision_disabled_pairs"), PhysicsAsset->CollisionDisableTable.Num());
    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetBoolField(TEXT("persist_requested"), bPersist);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPPhysicsAssetCommands::HandleRemoveConstraint(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UPhysicsAsset* PhysicsAsset = ResolvePhysicsAsset(Params, Error);
    if (!PhysicsAsset)
    {
        return Error;
    }

    FString ChildBone;
    FString ParentBone;
    if (!Params->TryGetStringField(TEXT("child_bone"), ChildBone) ||
        !Params->TryGetStringField(TEXT("parent_bone"), ParentBone))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"),
            TEXT("'child_bone' and 'parent_bone' (strings) are required"));
    }

    const FName ChildFName(*ChildBone);
    const FName ParentFName(*ParentBone);
    const int32 ConstraintIndex = PhysicsAsset->FindConstraintIndex(ChildFName, ParentFName);
    if (ConstraintIndex == INDEX_NONE)
    {
        // Report what the asset does hold: a pair is easy to get backwards, and the list is the fastest
        // way for the caller to see the direction this asset actually uses.
        TArray<FString> ExistingPairs;
        for (const TObjectPtr<UPhysicsConstraintTemplate>& Template : PhysicsAsset->ConstraintSetup)
        {
            if (const UPhysicsConstraintTemplate* ConstraintTemplate = Template.Get())
            {
                ExistingPairs.Add(FString::Printf(TEXT("%s<-%s"),
                    *ConstraintTemplate->DefaultInstance.ConstraintBone1.ToString(),
                    *ConstraintTemplate->DefaultInstance.ConstraintBone2.ToString()));
            }
        }
        TSharedPtr<FJsonObject> NotFound = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("constraint_not_found"),
            FString::Printf(TEXT("no constraint with child '%s' and parent '%s'"), *ChildBone, *ParentBone));
        FUnrealMCPCommonUtils::AddStringArrayField(NotFound, TEXT("constraints"), ExistingPairs);
        return NotFound;
    }

    const bool bPersist = FUnrealMCPCommonUtils::IsPersistRequested(Params);
    PhysicsAsset->ConstraintSetup.RemoveAt(ConstraintIndex);

    PhysicsAsset->MarkPackageDirty();
    const bool bSaved = bPersist ? FUnrealMCPCommonUtils::SaveAssetForObject(PhysicsAsset) : false;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("child_bone"), ChildBone);
    Result->SetStringField(TEXT("parent_bone"), ParentBone);
    Result->SetNumberField(TEXT("removed_constraint_index"), ConstraintIndex);
    Result->SetBoolField(TEXT("removed"), true);
    Result->SetNumberField(TEXT("constraint_count"), PhysicsAsset->ConstraintSetup.Num());
    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetBoolField(TEXT("persist_requested"), bPersist);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPPhysicsAssetCommands::HandleListBodies(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UPhysicsAsset* PhysicsAsset = ResolvePhysicsAsset(Params, Error);
    if (!PhysicsAsset)
    {
        return Error;
    }

    TArray<TSharedPtr<FJsonValue>> Bodies;
    for (int32 Index = 0; Index < PhysicsAsset->SkeletalBodySetups.Num(); ++Index)
    {
        const USkeletalBodySetup* Body = Cast<USkeletalBodySetup>(PhysicsAsset->SkeletalBodySetups[Index].Get());
        if (!Body)
        {
            continue;
        }
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetNumberField(TEXT("index"), Index);
        Entry->SetStringField(TEXT("bone_name"), Body->BoneName.ToString());
        Entry->SetStringField(TEXT("physics_type"), StaticEnum<EPhysicsType>()
            ? StaticEnum<EPhysicsType>()->GetNameStringByValue(static_cast<int64>(Body->PhysicsType)) : TEXT("?"));
        int32 DisabledPairs = 0;
        for (const TPair<FRigidBodyIndexPair, bool>& Pair : PhysicsAsset->CollisionDisableTable)
        {
            // Presence of the key means "disabled" (PhysicsAsset.cpp:199-224); the stored bool is unused.
            if (Pair.Key.Indices[0] == Index || Pair.Key.Indices[1] == Index)
            {
                ++DisabledPairs;
            }
        }
        Entry->SetNumberField(TEXT("collision_disabled_pairs"), DisabledPairs);
        // Every body this one may not collide with, so "collides with nothing" is visible in one read.
        TArray<TSharedPtr<FJsonValue>> DisabledWith;
        for (const TPair<FRigidBodyIndexPair, bool>& Pair : PhysicsAsset->CollisionDisableTable)
        {
            const int32 Other = Pair.Key.Indices[0] == Index ? Pair.Key.Indices[1]
                              : (Pair.Key.Indices[1] == Index ? Pair.Key.Indices[0] : INDEX_NONE);
            if (Other != INDEX_NONE && PhysicsAsset->SkeletalBodySetups.IsValidIndex(Other))
            {
                const USkeletalBodySetup* OtherBody = Cast<USkeletalBodySetup>(PhysicsAsset->SkeletalBodySetups[Other].Get());
                DisabledWith.Add(MakeShared<FJsonValueString>(OtherBody ? OtherBody->BoneName.ToString() : FString::FromInt(Other)));
            }
        }
        Entry->SetArrayField(TEXT("collision_disabled_with"), DisabledWith);
        AddShapeSummary(Body, Entry);
        Bodies.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TArray<TSharedPtr<FJsonValue>> Constraints;
    for (int32 Index = 0; Index < PhysicsAsset->ConstraintSetup.Num(); ++Index)
    {
        const UPhysicsConstraintTemplate* Template = PhysicsAsset->ConstraintSetup[Index].Get();
        if (!Template)
        {
            continue;
        }
        const FConstraintInstance& Instance = Template->DefaultInstance;
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetNumberField(TEXT("index"), Index);
        Entry->SetStringField(TEXT("bone1"), Instance.ConstraintBone1.ToString());
        Entry->SetStringField(TEXT("bone2"), Instance.ConstraintBone2.ToString());
        Entry->SetNumberField(TEXT("swing1_limit_deg"), Instance.GetAngularSwing1Limit());
        Entry->SetNumberField(TEXT("swing2_limit_deg"), Instance.GetAngularSwing2Limit());
        Entry->SetNumberField(TEXT("twist_limit_deg"), Instance.GetAngularTwistLimit());
        Entry->SetBoolField(TEXT("disable_collision"), Instance.IsCollisionDisabled());
        Constraints.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), PhysicsAsset->GetPathName());
    Result->SetNumberField(TEXT("body_count"), PhysicsAsset->SkeletalBodySetups.Num());
    Result->SetNumberField(TEXT("constraint_count"), PhysicsAsset->ConstraintSetup.Num());
    Result->SetArrayField(TEXT("bodies"), Bodies);
    Result->SetArrayField(TEXT("constraints"), Constraints);
    return Result;
}
