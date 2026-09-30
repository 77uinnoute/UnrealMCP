#pragma once

#include "CoreMinimal.h"
#include "Json.h"
#include "Core/MCPCommandRegistry.h"

/**
 * Handler class for asset pipeline MCP commands that the plain reflection /
 * python paths cannot do safely:
 *   - import_assets: importing through the editor python context crashes the
 *     editor (Interchange re-enters the GameThread task queue -> TaskGraph
 *     RecursionGuard assert), so the import runs here with the Interchange
 *     feature flag for the requested extensions disabled and read back first.
 *   - set_asset_properties: generic asset property writes with friendly names,
 *     enum member tolerance and per-item error collection. The type-specific
 *     setters (set_component_property / set_static_mesh_properties /
 *     set_blueprint_property) cannot write a texture's srgb / lod_group / ...
 */
class UNREALMCP_API FUnrealMCPAssetEditCommands
{
public:
    FUnrealMCPAssetEditCommands();

    // Register this domain's commands with the process-wide command registry.
    void RegisterCommands(FMCPCommandRegistry& Registry);

private:
    TSharedPtr<FJsonObject> HandleImportAssets(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleImportTexture(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleImportSkeletalMesh(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleImportAnimation(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetAssetProperties(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleInspectSkeletalMesh(const TSharedPtr<FJsonObject>& Params);
};
