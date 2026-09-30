#pragma once

#include "CoreMinimal.h"
#include "Json.h"
#include "Core/MCPCommandRegistry.h"

/**
 * Handler class for asset-safety MCP commands.
 *
 * These commands exist because the plain editor paths (python or UI) hit modal
 * dialogs and redirector/reference deadlocks when scripting asset churn
 * (delete + recreate loops):
 *   - safe_delete_asset: redirector-aware, blocker-reporting, zero modal dialogs.
 *     force=true breaks the blocking references first (level actor instances, asset-internal
 *     object properties pointing at the target) and reports them in `detached[]`, which is the
 *     only way out of a mesh <-> skeleton <-> physics reference cycle.
 *   - list_asset_blockers: full referencer inventory for decision making
 *   - asset_status: read-only existence triage (in_memory / on_disk / registry / referencers)
 *   - list_disk_only_assets: .uasset files with no registry entry (lists, never deletes)
 *   - move_asset / move_directory: rename/move on top of the engine's own rename
 *     (IAssetTools::RenameAssets, the path the Content Browser uses) plus the fixup the engine
 *     cannot do on its own: referencer packages are re-saved, so their on-disk pointers stop
 *     naming the old path. Bypassing the command and calling python rename_asset leaves them
 *     stale, and there is no redirector to fall back on.
 *   - clear_blendables / remove_blendable / list_blendables: PostProcessVolume
 *     blendables are protected from python writes, C++ can operate directly.
 *   - add_blendable_to_post_volume: the same array, written through the same C++ path.
 *   - create_asset_safe / delete_asset_safe: the modal-free create/delete pair. python needed them
 *     (AssetTools' create_asset pops the rename/replace dialog when the package already exists, and
 *     that modal freezes the whole bridge), but python tools cannot be called from editor-side
 *     scripts - so the implementation lives here, where both the tool surface and the bridge
 *     loopback of execute_python_* reach it.
 */
class UNREALMCP_API FUnrealMCPAssetCommands
{
public:
    FUnrealMCPAssetCommands();

    // Register this domain's commands with the process-wide command registry.
    void RegisterCommands(FMCPCommandRegistry& Registry);

private:
    TSharedPtr<FJsonObject> HandleSafeDeleteAsset(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListAssetBlockers(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAssetStatus(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListDiskOnlyAssets(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleMoveAsset(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleMoveDirectory(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleClearBlendables(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveBlendable(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListBlendables(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddBlendableToPostVolume(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCreateAssetSafe(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleDeleteAssetSafe(const TSharedPtr<FJsonObject>& Params);

    // Shared resolution: given an asset object path, collect blocking referencer
    // packages (non-redirector) and redirector objects pointing at the asset.
    // Returns false + error response when the asset does not exist.
    bool ResolveAssetBlockers(const FString& AssetPath,
                               TArray<FString>& OutBlockerPackages,
                               TArray<UObject*>& OutRedirectors,
                               TSharedPtr<FJsonObject>& OutError);

    // --- safe_delete_asset steps: the handler reads as a sequence of these -------------------
    // True when the asset package file is gone (the disk half of "the delete really happened").
    static bool IsPackageFileGone(const FString& PackageName);

    // Removes the asset: silent object deletion first (no dialogs), then - only if the file
    // survived it - the path-based editor delete, which also drops a Blueprint's generated class.
    // OutLivingClasses names the objects still holding the package alive when it survives both.
    void RemoveAssetFiles(UObject* AssetObject, const FString& ObjectPath, const FString& PackageName,
                          TArray<FString>& OutLivingClasses);

    // Adds success/deleted/detail to an already-populated result (blockers, was_redirector).
    TSharedPtr<FJsonObject> AddDeleteOutcome(const TSharedPtr<FJsonObject>& ResultJson, bool bDiskGone,
                                            bool bMemGone, const TArray<FString>& LivingClasses);

    // --- force=true: break the references the caller asked us to ignore ----------------------
    // Destroys level actor instances of the target's class, then clears object properties that
    // live inside already-loaded referencer packages and point at the target. Every broken
    // reference is appended to OutDetached as {referencer, how}. Loads nothing.
    static void DetachReferencers(UObject* Target, TArray<TSharedPtr<FJsonValue>>& OutDetached);

    // --- asset_status / list_disk_only_assets -----------------------------------------------
    // Caller path (object path or package path) -> package name + object path, no loading.
    static bool ResolveAssetStatusPaths(const FString& AssetPath, FString& OutPackageName,
                                        FString& OutObjectPath);
};
