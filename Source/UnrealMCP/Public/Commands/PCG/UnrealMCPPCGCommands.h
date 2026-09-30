#pragma once

#include "CoreMinimal.h"
#include "Json.h"
#include "Core/MCPCommandRegistry.h"

class UPCGComponent;
class UPCGGraph;
class UPCGNode;
class UPCGPin;

/**
 * Handler class for the PCG commands.
 *
 * PCG graphs are opaque to the editor python layer: property names have to be guessed
 * (`scale_min` exists on PCGTransformPointsSettings while `b_uniform_scale` does not),
 * pin labels come in two spellings (the display label used by `add_edge` and the
 * snake_case name used for properties), a wrong pin label fails silently and a float
 * written into an int32 property is truncated without a word. These commands turn that
 * surface into structured readback:
 *   - list_pcg_assets:            PCG asset inventory straight from the asset registry
 *   - get_pcg_graph:              node/pin/edge dump of a graph (empty pin arrays stay empty)
 *   - get_pcg_node:               reflected property table of one node's settings, with the
 *                                 display label, the snake_case name, the type, the current
 *                                 value and - importantly - the writability of instanced
 *                                 read-only subobjects such as MeshSelectorParameters
 *   - list_pcg_components:        PCG components in the current world and their managed resources
 *   - get_pcg_generated_output:   the existing generation result (tagged data + per-attribute stats)
 *   - add_pcg_node / remove_pcg_node / connect_pcg_pins / disconnect_pcg_pins / set_pcg_node_property:
 *                                 graph structure and settings writes, each read back before it is
 *                                 reported, each persisted on success
 *   - generate_pcg_component / cleanup_pcg_component: dispatch generation / cleanup and report the
 *                                 component state as it is (the work itself is asynchronous)
 *
 * The five read commands never generate, clean up, save or otherwise mutate PCG state: they are
 * registered without the graph-mutating flag and never touch a package's dirty state. The seven
 * write commands are registered with it and persist their asset on success - a write that only
 * lives in memory is a write that the build script's editor kill would throw away.
 */
class UNREALMCP_API FUnrealMCPPCGCommands
{
public:
    FUnrealMCPPCGCommands();

    // Register this domain's commands with the process-wide command registry.
    void RegisterCommands(FMCPCommandRegistry& Registry);

private:
    TSharedPtr<FJsonObject> HandleListPCGAssets(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetPCGGraph(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetPCGNode(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListPCGComponents(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetPCGGeneratedOutput(const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FJsonObject> HandleAddPCGNode(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleConnectPCGPins(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleDisconnectPCGPins(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemovePCGNode(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetPCGNodeProperty(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGeneratePCGComponent(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCleanupPCGComponent(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCreatePCGGraph(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetPCGComponentGraph(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetPCGMeshSelectorType(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetPCGMeshSelectorEntries(const TSharedPtr<FJsonObject>& Params);

    // Resolve an asset path to a loaded UPCGGraph. Accepts the object path, the package path and a
    // short name (same lookup the rest of the plugin uses) and loads the asset when it is not in
    // memory yet - these commands only read, so the load is side-effect free.
    // On failure returns nullptr with OutError carrying "asset_not_found" / "not_a_pcg_graph".
    UPCGGraph* ResolveGraph(const FString& AssetPath, TSharedPtr<FJsonObject>& OutError);

    // Locate one node by name, falling back to the 0-based index when no name is given.
    // On failure returns nullptr with OutError carrying "node_not_found" and the node names of
    // the graph as candidates.
    UPCGNode* ResolveNode(UPCGGraph* Graph, const FString& NodeName, int32 NodeIndex, TSharedPtr<FJsonObject>& OutError);

    // {"label", "connected", "edges_to": [node names]} for one pin - the label is the engine's
    // display label (what add_edge takes), never a rebuilt or guessed name.
    TSharedPtr<FJsonObject> PinToJson(const UPCGPin* Pin) const;

    // One node: name, settings class, editor position, input/output pins, and - when
    // bIncludeProperties is set - the reflected property table of its settings.
    TSharedPtr<FJsonObject> NodeToJson(const UPCGNode* Node, bool bIncludeProperties, int32 MaxProperties) const;

    // Locate a pin on a node by its display label or by the snake_case spelling of it (callers read
    // one and write the other). On failure returns nullptr with OutError carrying "pin_not_found"
    // and every candidate label of that node's pins in that direction.
    UPCGPin* ResolvePin(UPCGNode* Node, const FString& Label, bool bOutputPin, TSharedPtr<FJsonObject>& OutError) const;

    // Locate a UPCGSettings subclass by class name or object path. On failure returns nullptr with
    // OutError carrying "unknown_node_class" and the candidate class names.
    UClass* ResolveSettingsClass(const FString& NodeClass, TSharedPtr<FJsonObject>& OutError) const;

    // Locate one PCG component in the editor world by component name or by a substring of the owning
    // actor's label. On failure returns nullptr with OutError carrying "component_not_found" and the
    // "actor / component" candidates; OutActor receives the owning actor on success.
    UPCGComponent* FindPCGComponent(const FString& ActorLabel, const FString& ComponentName,
                                    AActor** OutActor, TSharedPtr<FJsonObject>& OutError) const;
};
