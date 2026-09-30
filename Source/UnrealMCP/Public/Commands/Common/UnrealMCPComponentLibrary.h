#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#include "UnrealMCPComponentLibrary.generated.h"

class AActor;
class UActorComponent;
class UWorld;

/**
 * Component plumbing the editor python layer cannot reach.
 *
 * `unreal.new_object(ComponentClass, Actor)` creates a bare subobject: it is neither registered (so a
 * rendering component never draws and tick functions never run) nor marked as an instance component
 * (so the editor's component tree - SubobjectDataSubsystem - filters it out). Neither
 * `UActorComponent::RegisterComponentWithWorld` nor `AActor::AddInstanceComponent` is exposed to
 * python, so components built that way are "ghosts": reachable through reflection, invisible and inert
 * in the editor.
 *
 * The editor's own Add Component path calls AddInstanceComponent + RegisterComponentWithWorld; these
 * two helpers expose the same pair, so a python-built actor can be assembled the same way. See
 * Docs/MCP_PCG_Probe_2026-09-21.md section 13.8 for the measurements behind the two symptoms.
 *
 * All functions must be called on the GameThread.
 */
UCLASS()
class UNREALMCP_API UUnrealMCPComponentLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    /** Register the component against its own world. Returns its registration state afterwards. */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Component")
    static bool RegisterComponent(UActorComponent* Component);

    /** Register the component against an explicit world (e.g. the editor world). */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Component")
    static bool RegisterComponentWithWorld(UActorComponent* Component, UWorld* World);

    /** Whether the component is registered (python exposes no IsRegistered). */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Component", meta = (DisplayName = "Is Component Registered"))
    static bool IsComponentRegistered(const UActorComponent* Component);

    /**
     * Turn a component into a real instance component of the actor: owned by it, CreationMethod set to
     * Instance (which is what makes the editor's component tree list it) and tracked in
     * InstanceComponents. Does NOT register it - call RegisterComponent for that.
     */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Component")
    static bool AddInstanceComponent(AActor* Actor, UActorComponent* Component);
};
