#pragma once

#include "CoreMinimal.h"
#include "Json.h"
#include "Core/MCPCommandRegistry.h"

/**
 * Handler class for physics-asset authoring commands.
 *
 * Why this exists: AnimDynamics cannot collide with a PhysicsAsset at all - the solver's entry point
 * takes only the node's own bodies (`FAnimPhys::PhysicsUpdate`, AnimPhysicsSolver.h:478: bodies, limits,
 * springs, gravity/force/accel) and the header comments `AnimPhysCollisionType` as "Only how we interact
 * with planes currently" (AnimPhysicsSolver.h:261). The systems that DO collide with a PhysicsAsset are
 * `FAnimNode_RigidBody` (AnimNode_RigidBody.h:150, simulates the asset's bodies with `OverridePhysicsAsset`,
 * `bEnableWorldGeometry`, per-body animation blending) and `UPhysicalAnimationComponent`. Both need bodies
 * on the chain in question, and the only C++ that creates them (`FPhysicsAssetUtils::CreateNewBody` /
 * `CreateNewConstraint`, PhysicsUtilities/PhysicsAssetUtils.h:192-217) is exported engine code with no
 * UFUNCTION and no python binding - so the editor UI was the only way to add a body until this command.
 *
 * What this command set does NOT do: it does not simulate anything. It only authors bodies and constraints;
 * making the result move still needs a RigidBody node (or a physical animation component) pointed at the
 * asset, which `add_blueprint_node_by_class` + `Node.OverridePhysicsAsset` can do from python.
 *
 * `set_physics_asset_collision` exists because a RigidBody node turns every body of the asset into a
 * collider for the simulated ones, so an auto-generated body that is far too fat (the engine sizes a shape
 * from the bounding box of the bone's vertices) will keep pushing the simulated chain. Narrowing that is
 * only expressible in `UPhysicsAsset::CollisionDisableTable` (PhysicsAsset.h:256) - a TMap keyed by a
 * struct, which the property reflector cannot address - and the solver-side switch
 * (`FConstraintInstance::SetDisableCollision`) only covers the one pair a constraint joins.
 */
class UNREALMCP_API FUnrealMCPPhysicsAssetCommands
{
public:
    FUnrealMCPPhysicsAssetCommands();

    // Register this domain's commands with the process-wide command registry.
    void RegisterCommands(FMCPCommandRegistry& Registry);

private:
    TSharedPtr<FJsonObject> HandleAddBody(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddConstraint(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetCollision(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveBody(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveConstraint(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListBodies(const TSharedPtr<FJsonObject>& Params);
};
