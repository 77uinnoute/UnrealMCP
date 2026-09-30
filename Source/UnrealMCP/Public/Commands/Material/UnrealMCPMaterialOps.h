#pragma once

#include "CoreMinimal.h"
#include "Json.h"

class UObject;

/**
 * Read helpers for a material asset's expression graph.
 *
 * `find_blueprint_nodes` accepts a Material / MaterialFunction as well as a Blueprint, and it
 * serializes both into the same node JSON shape. The material half lives here (in the material
 * domain) so the blueprint node commands stay free of material types.
 */
class FUnrealMCPMaterialOps
{
public:
    /** True for the asset classes whose expression graph the node read commands accept. */
    static bool IsMaterialGraphAsset(const UObject* Asset);

    /**
     * Serialize a Material / MaterialFunction's expression graph in the node JSON shape: one entry
     * per expression (name / type / index / properties / pins / connections) plus the virtual
     * Material Output / Function Output node first.
     *
     * Returns false when the asset is not a material graph asset at all, so the caller can report
     * its own error; a material graph asset with an unsupported node_type returns true with an
     * error response.
     */
    static bool TrySerializeGraphNodes(UObject* Asset, const FString& NodeType,
                                       const TSharedPtr<FJsonObject>& Params,
                                       TSharedPtr<FJsonObject>& OutResult);
};
