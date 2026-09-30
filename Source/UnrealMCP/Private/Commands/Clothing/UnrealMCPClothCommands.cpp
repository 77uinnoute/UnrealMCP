#include "Commands/Clothing/UnrealMCPClothCommands.h"

#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Core/MCPCommandRegistry.h"
#include "Reflection/MCPObjectPathResolver.h"

#include "ClothingAsset.h"
#include "ClothLODData.h"
#include "ClothPhysicalMeshData.h"
#include "PointWeightMap.h"
#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"

namespace
{
    /**
     * Split a cloth mapping array into skinned-only / cloth-only / blended render vertices.
     *
     * SourceMeshVertIndices[3] is the static alpha ComputeVertexContributions writes: 0xFFFF means the
     * render vertex is skinned only (the cloth simulation is ignored for it), 0 means the cloth drives
     * it alone.
     */
    void AddClothMappingSplit(const TSharedPtr<FJsonObject>& Target, const TArray<FMeshToMeshVertData>& Mapping)
    {
        int32 SkinnedOnlyCount = 0;
        int32 ClothOnlyCount = 0;
        int32 BlendedCount = 0;
        for (const FMeshToMeshVertData& VertData : Mapping)
        {
            const uint16 StaticAlpha = VertData.SourceMeshVertIndices[3];
            if (StaticAlpha == 0xFFFF)
            {
                ++SkinnedOnlyCount;
            }
            else if (StaticAlpha == 0)
            {
                ++ClothOnlyCount;
            }
            else
            {
                ++BlendedCount;
            }
        }

        Target->SetNumberField(TEXT("vertex_count"), Mapping.Num());
        Target->SetNumberField(TEXT("skinned_only_count"), SkinnedOnlyCount);
        Target->SetNumberField(TEXT("cloth_only_count"), ClothOnlyCount);
        Target->SetNumberField(TEXT("blended_count"), BlendedCount);
    }
}

FUnrealMCPClothCommands::FUnrealMCPClothCommands()
{
}

void FUnrealMCPClothCommands::RegisterCommands(FMCPCommandRegistry& Registry)
{
    MCP_REGISTER_COMMAND(Registry, "apply_cloth_masks", "cloth",
        "Rebuild a clothing asset's derived mask data after its baked masks were changed through a property path: pushes the masks into the sim mesh and recomputes the per-vertex cloth contributions.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("object_path"), TEXT("string"), TEXT("Clothing asset path, or a SkeletalMesh path (every clothing asset on it is rebuilt)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save the owning package after the rebuild; default true")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleApplyClothMasks(Params); });
}

TSharedPtr<FJsonObject> FUnrealMCPClothCommands::HandleApplyClothMasks(const TSharedPtr<FJsonObject>& Params)
{
#if !WITH_EDITOR
    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unsupported_property_type"),
        TEXT("apply_cloth_masks needs an editor build: clothing mask data is editor-only"));
#else
    UObject* TargetObject = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    TArray<FString> TriedPaths;
    if (!FMCPObjectPathResolver::ResolveFromParams(Params, TEXT("object_path"), TargetObject, ErrorCode,
                                                   ErrorMessage, TriedPaths))
    {
        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(ErrorCode, ErrorMessage);
        FUnrealMCPCommonUtils::AddStringArrayField(Error, TEXT("tried"), TriedPaths);
        return Error;
    }

    TArray<UClothingAssetCommon*> ClothingAssets;
    if (UClothingAssetCommon* DirectClothAsset = Cast<UClothingAssetCommon>(TargetObject))
    {
        ClothingAssets.Add(DirectClothAsset);
    }
    else if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(TargetObject))
    {
        for (UClothingAssetBase* Base : Mesh->GetMeshClothingAssets())
        {
            if (UClothingAssetCommon* MeshClothAsset = Cast<UClothingAssetCommon>(Base))
            {
                ClothingAssets.Add(MeshClothAsset);
            }
        }
    }

    if (ClothingAssets.Num() == 0)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"),
            FString::Printf(TEXT("object_path must be a clothing asset or a SkeletalMesh with clothing on it; '%s' resolved to %s"),
                *TargetObject->GetPathName(), *TargetObject->GetClass()->GetName()));
    }

    bool bPersist = true;
    Params->TryGetBoolField(TEXT("persist"), bPersist);

    TArray<TSharedPtr<FJsonValue>> AssetsJson;
    bool bSavedAny = false;

    for (UClothingAssetCommon* ClothAsset : ClothingAssets)
    {
        ClothAsset->Modify();
        // The masks only reached the sim mesh if this runs: ApplyParameterMasks pushes every enabled
        // PointWeightMap into PhysicalMeshData.WeightMaps, regenerates the tethers (which are derived from
        // the mask: a zero mask marks every node kinematic), and recomputes the per-render-vertex cloth
        // contributions - without it the renderer keeps treating the whole cloth as plain skinning.
        ClothAsset->ApplyParameterMasks(/*bUpdateFixedVertData=*/true, /*bInvalidateDerivedDataCache=*/true);
        ClothAsset->MarkPackageDirty();

        // The renderer never reads the imported model: it has its own copy of the cloth contributions
        // (FSkelMeshRenderSection::ClothMappingDataLODs, which the skinning shader consults per vertex:
        // 0xFFFF = skinned only, 0 = cloth only). ApplyParameterMasks only updates the imported model, and its
        // own PostEditChange route can be swallowed by the skeletal mesh re-entrancy counter
        // (SkeletalMesh.cpp:1248) - so rebuild here. Without this the cloth keeps rendering as plain
        // skinning however correct the simulation side is.
        USkeletalMesh* Mesh = Cast<USkeletalMesh>(ClothAsset->GetOuter());
        if (Mesh)
        {
            Mesh->Build();
            Mesh->MarkPackageDirty();
        }

        TSharedPtr<FJsonObject> AssetJson = MakeShared<FJsonObject>();
        AssetJson->SetStringField(TEXT("object_path"), ClothAsset->GetPathName());
        AssetJson->SetStringField(TEXT("object_class"), ClothAsset->GetClass()->GetName());

        TArray<TSharedPtr<FJsonValue>> LodsJson;
        for (int32 LodIndex = 0; LodIndex < ClothAsset->LodData.Num(); ++LodIndex)
        {
            const FClothPhysicalMeshData& PhysicalMesh = ClothAsset->LodData[LodIndex].PhysicalMeshData;

            TSharedPtr<FJsonObject> LodJson = MakeShared<FJsonObject>();
            LodJson->SetNumberField(TEXT("lod_index"), LodIndex);
            LodJson->SetNumberField(TEXT("particle_count"), PhysicalMesh.Vertices.Num());
            LodJson->SetNumberField(TEXT("triangle_count"), PhysicalMesh.Indices.Num() / 3);

            if (const FPointWeightMap* MaxDistance = PhysicalMesh.FindWeightMap(EWeightMapTargetCommon::MaxDistance))
            {
                // A pinned particle is one the max distance constraint holds at its skinned position: the
                // count is the direct read-back proof that a written mask did (or did not) take effect.
                int32 PinnedCount = 0;
                float MinValue = 0.f;
                float MaxValue = 0.f;
                for (int32 Index = 0; Index < MaxDistance->Num(); ++Index)
                {
                    const float Value = (*MaxDistance)[Index];
                    if (MaxDistance->IsBelowThreshold(Index))
                    {
                        ++PinnedCount;
                    }
                    MinValue = Index == 0 ? Value : FMath::Min(MinValue, Value);
                    MaxValue = Index == 0 ? Value : FMath::Max(MaxValue, Value);
                }

                LodJson->SetNumberField(TEXT("max_distance_count"), MaxDistance->Num());
                LodJson->SetNumberField(TEXT("max_distance_pinned_count"), PinnedCount);
                LodJson->SetNumberField(TEXT("max_distance_min"), MinValue);
                LodJson->SetNumberField(TEXT("max_distance_max"), MaxValue);
            }

            LodJson->SetNumberField(TEXT("euclidean_tether_batches"), PhysicalMesh.EuclideanTethers.Tethers.Num());
            LodJson->SetNumberField(TEXT("geodesic_tether_batches"), PhysicalMesh.GeodesicTethers.Tethers.Num());
            LodsJson.Add(MakeShared<FJsonValueObject>(LodJson));
        }
        AssetJson->SetArrayField(TEXT("lods"), LodsJson);

        // Per-render-vertex contributions: SourceMeshVertIndices[3] is the static alpha written by
        // ComputeVertexContributions (0xFFFF = fully skinned, 0 = cloth only). Reporting the split is how
        // "the cloth is invisible because everything is skinned" is told apart from "the mask is still off".
        if (Mesh)
        {
            TArray<TSharedPtr<FJsonValue>> SectionsJson;
            if (FSkeletalMeshModel* MeshModel = Mesh->GetImportedModel())
            {
                for (FSkeletalMeshLODModel& LodModel : MeshModel->LODModels)
                {
                    for (int32 SectionIndex = 0; SectionIndex < LodModel.Sections.Num(); ++SectionIndex)
                    {
                        const FSkelMeshSection& Section = LodModel.Sections[SectionIndex];
                        if (!Section.HasClothingData() || Section.ClothingData.AssetGuid != ClothAsset->GetAssetGuid())
                        {
                            continue;
                        }
                        for (int32 ClothLodBias = 0; ClothLodBias < Section.ClothMappingDataLODs.Num(); ++ClothLodBias)
                        {
                            TSharedPtr<FJsonObject> SectionJson = MakeShared<FJsonObject>();
                            SectionJson->SetNumberField(TEXT("section_index"), SectionIndex);
                            SectionJson->SetNumberField(TEXT("material_index"), Section.MaterialIndex);
                            SectionJson->SetNumberField(TEXT("cloth_lod_bias"), ClothLodBias);
                            AddClothMappingSplit(SectionJson, Section.ClothMappingDataLODs[ClothLodBias]);
                            SectionsJson.Add(MakeShared<FJsonValueObject>(SectionJson));
                        }
                    }
                }
            }
            AssetJson->SetArrayField(TEXT("sections"), SectionsJson);
            AssetJson->SetStringField(TEXT("mesh_path"), Mesh->GetPathName());

            // What the renderer will actually use after the Build() above: if this still says skinned-only
            // while `sections` says cloth-only, the mesh was not rebuilt and the cloth can never show.
            TArray<TSharedPtr<FJsonValue>> RenderSectionsJson;
            if (const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering())
            {
                for (int32 LodIndex = 0; LodIndex < RenderData->LODRenderData.Num(); ++LodIndex)
                {
                    const FSkeletalMeshLODRenderData& LodRenderData = RenderData->LODRenderData[LodIndex];
                    for (int32 SectionIndex = 0; SectionIndex < LodRenderData.RenderSections.Num(); ++SectionIndex)
                    {
                        const FSkelMeshRenderSection& RenderSection = LodRenderData.RenderSections[SectionIndex];
                        for (int32 ClothLodBias = 0; ClothLodBias < RenderSection.ClothMappingDataLODs.Num(); ++ClothLodBias)
                        {
                            TSharedPtr<FJsonObject> SectionJson = MakeShared<FJsonObject>();
                            SectionJson->SetNumberField(TEXT("lod_index"), LodIndex);
                            SectionJson->SetNumberField(TEXT("section_index"), SectionIndex);
                            SectionJson->SetNumberField(TEXT("material_index"), RenderSection.MaterialIndex);
                            SectionJson->SetNumberField(TEXT("cloth_lod_bias"), ClothLodBias);
                            AddClothMappingSplit(SectionJson, RenderSection.ClothMappingDataLODs[ClothLodBias]);
                            RenderSectionsJson.Add(MakeShared<FJsonValueObject>(SectionJson));
                        }
                    }
                }
            }
            AssetJson->SetArrayField(TEXT("render_sections"), RenderSectionsJson);
        }

        if (bPersist)
        {
            bSavedAny |= FUnrealMCPCommonUtils::SaveAssetForObject(ClothAsset);
        }

        AssetsJson.Add(MakeShared<FJsonValueObject>(AssetJson));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("assets"), AssetsJson);
    Data->SetNumberField(TEXT("asset_count"), AssetsJson.Num());
    Data->SetBoolField(TEXT("persist_requested"), bPersist);
    Data->SetBoolField(TEXT("saved"), bSavedAny);
    return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
#endif
}
