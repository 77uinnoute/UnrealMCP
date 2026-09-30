#pragma once

#include "CoreMinimal.h"
#include "Json.h"
#include "Core/MCPCommandRegistry.h"

/**
 * Handler class for the clothing (legacy UClothingAssetCommon) commands.
 *
 * apply_cloth_masks exists because "the masks took effect" is not reachable any other way:
 *   - UClothingAssetCommon::ApplyParameterMasks (ClothingAsset.h:127) is exported C++ with no
 *     UFUNCTION, so neither python nor a reflection-driven call can invoke it;
 *   - its input, FClothLODDataCommon::PointWeightMaps (ClothLODData.h:50), is editor-only data;
 *   - it only updates the imported model, while the renderer reads its own copy of the per-vertex
 *     cloth contributions (FSkeletalMeshRenderSection::ClothMappingDataLODs), so the mesh has to be
 *     rebuilt as well (USkeletalMesh::Build(), also not a UFUNCTION).
 *
 * A mask written through set_object_property is therefore raw data until this command runs. Its
 * readback is what separates "the mask did not take effect" from "the mesh was not rebuilt": the
 * per-section contribution split is not reflection-readable either (USkeletalMesh::ImportedModel is a
 * TSharedPtr, not a UPROPERTY).
 */
class UNREALMCP_API FUnrealMCPClothCommands
{
public:
    FUnrealMCPClothCommands();

    // Register this domain's commands with the process-wide command registry.
    void RegisterCommands(FMCPCommandRegistry& Registry);

private:
    TSharedPtr<FJsonObject> HandleApplyClothMasks(const TSharedPtr<FJsonObject>& Params);
};
