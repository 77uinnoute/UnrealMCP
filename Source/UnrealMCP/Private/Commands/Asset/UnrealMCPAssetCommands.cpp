#include "Commands/Asset/UnrealMCPAssetCommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Core/MCPCommandRegistry.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "EditorAssetLibrary.h"
#include "Editor.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/UObjectHash.h"
// New in 2026-09-27: the modal-free create/delete pair and the blendable writer (see the header).
#include "AssetToolsModule.h"
#include "Factories/Factory.h"
#include "Commands/Blueprint/UnrealMCPBlueprintGraphOps.h"

FUnrealMCPAssetCommands::FUnrealMCPAssetCommands()
{
}

void FUnrealMCPAssetCommands::RegisterCommands(FMCPCommandRegistry& Registry)
{
    MCP_REGISTER_COMMAND(Registry, "safe_delete_asset", "asset",
        "Redirector-aware asset delete that reports blockers and uses no modal dialogs. force=true breaks the "
        "blocking references first (level actor instances, in-asset object properties pointing at the target) "
        "and lists them in detached[], which is the only way out of a mesh/skeleton/physics reference cycle.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Asset object path to delete")),
            MCPParamOpt(TEXT("force"), TEXT("bool"), TEXT("Break blocking references, then delete (default false)")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleSafeDeleteAsset(Params); });

    MCP_REGISTER_COMMAND(Registry, "list_asset_blockers", "asset",
        "List the referencer packages and redirectors that keep an asset alive.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Asset object path to inspect")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleListAssetBlockers(Params); });

    MCP_REGISTER_COMMAND(Registry, "asset_status", "asset",
        "Read-only existence triage for one asset: in_memory / on_disk / registry / referencers, "
        "resolved without loading anything (load_asset() is None cannot distinguish 'not loaded' from 'gone').",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("path"), TEXT("string"), TEXT("'/Game/Dir/Asset' or '/Game/Dir/Asset.Asset'")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleAssetStatus(Params); });

    MCP_REGISTER_COMMAND(Registry, "list_disk_only_assets", "asset",
        "List .uasset files that are on disk but have no asset-registry entry. Read-only: it lists, never deletes.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("dir"), TEXT("string"), TEXT("Content directory, e.g. '/Game/MCP/Ganyu'")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleListDiskOnlyAssets(Params); });

    MCP_REGISTER_COMMAND(Registry, "move_asset", "asset",
        "Rename or move an asset (a path change is both) and fix the packages that reference it, so no "
        "on-disk referencer keeps naming the old path. Uses the engine's own rename "
        "(IAssetTools::RenameAssets - the Content Browser path) and then re-saves every referencer it "
        "can load; referencers that cannot be loaded (map packages) are reported in unstorable[] and the "
        "engine leaves a redirector so the old path still resolves. Do NOT hand-roll this with python "
        "unreal.EditorAssetLibrary.rename_asset: that route leaves referencers stale on disk and leaves "
        "no redirector, so the references break silently after an editor restart. update_referencers=false "
        "does the move only and reports the referencer list. dry_run=true reports without touching anything.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Existing asset: '/Game/Dir/Asset' or '/Game/Dir/Asset.Asset'")),
            MCPParam(TEXT("new_path"), TEXT("string"), TEXT("Destination: '/Game/NewDir/Name' (rename and/or new directory)")),
            MCPParamOpt(TEXT("update_referencers"), TEXT("bool"), TEXT("Re-save the referencer packages so their disk data points at the new path (default true)")),
            MCPParamOpt(TEXT("refresh_registry"), TEXT("bool"), TEXT("Re-scan search_paths before reading referencers (default true; the dependency graph lags for assets saved in this session)")),
            MCPParamOpt(TEXT("search_paths"), TEXT("string_array"), TEXT("Paths to refresh, default ['/Game']")),
            MCPParamOpt(TEXT("dry_run"), TEXT("bool"), TEXT("Report referencers and the redirector decision without moving anything (default false)")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleMoveAsset(Params); });

    MCP_REGISTER_COMMAND(Registry, "move_directory", "asset",
        "Move a whole content directory (subdirectories included) to a new directory, running every asset "
        "through the same referencer fixup as move_asset. Referencers outside the moved directory are "
        "handled the same way. Refuses when the destination directory already exists.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("dir"), TEXT("string"), TEXT("Existing content directory, e.g. '/Game/MCP/Ganyu'")),
            MCPParam(TEXT("new_dir"), TEXT("string"), TEXT("Destination directory, e.g. '/Game/MCP/Ganyu2'")),
            MCPParamOpt(TEXT("update_referencers"), TEXT("bool"), TEXT("Re-save the referencer packages (default true)")),
            MCPParamOpt(TEXT("refresh_registry"), TEXT("bool"), TEXT("Re-scan search_paths before reading referencers (default true)")),
            MCPParamOpt(TEXT("search_paths"), TEXT("string_array"), TEXT("Paths to refresh, default ['/Game']")),
            MCPParamOpt(TEXT("dry_run"), TEXT("bool"), TEXT("Report only; move nothing (default false)")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleMoveDirectory(Params); });

    MCP_REGISTER_COMMAND(Registry, "clear_blendables", "asset",
        "Empty a PostProcessVolume's blendables array and refresh its render state.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("volume_name"), TEXT("string"), TEXT("PostProcessVolume actor name or label")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleClearBlendables(Params); });

    MCP_REGISTER_COMMAND(Registry, "remove_blendable", "asset",
        "Remove one material from a PostProcessVolume's blendables array.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("volume_name"), TEXT("string"), TEXT("PostProcessVolume actor name or label")),
            MCPParam(TEXT("material_path"), TEXT("string"), TEXT("Object path of the blendable to remove")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleRemoveBlendable(Params); });

    MCP_REGISTER_COMMAND(Registry, "list_blendables", "asset",
        "List the blendables and weights on a PostProcessVolume.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("volume_name"), TEXT("string"), TEXT("PostProcessVolume actor name or label")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleListBlendables(Params); });

    MCP_REGISTER_COMMAND(Registry, "add_blendable_to_post_volume", "asset",
        "Bind a post-process material to a PostProcessVolume and refresh the volume so the binding takes "
        "effect (the enabled flag is bounced - without it the entry exists but renders nothing). "
        "volume_name=null prefers a volume whose name/label matches the material's short name (with or "
        "without the M_ prefix), else the single volume in the level, and reports volume_ambiguous with "
        "the candidates instead of guessing.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("material_path"), TEXT("string"), TEXT("Post-process material object path")),
            MCPParamOpt(TEXT("volume_name"), TEXT("string"), TEXT("PostProcessVolume actor name or label; omit to auto-pick")),
            MCPParamOpt(TEXT("weight"), TEXT("number"), TEXT("Blendable weight 0..1 (default 1.0)")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleAddBlendableToPostVolume(Params); });

    MCP_REGISTER_COMMAND(Registry, "create_asset_safe", "asset",
        "Create an asset with no modal dialog: an existing package reports asset_exists instead of "
        "popping the rename/replace dialog that freezes the whole bridge; recreate=true silently saves "
        "and deletes the existing asset first and reports delete_pending when the registry has not "
        "released it yet. Reachable from editor-side scripts too (execute_python_* loopback), which "
        "python-only tools are not.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_name"), TEXT("string"), TEXT("New asset name, e.g. \"M_MyMaterial\"")),
            MCPParam(TEXT("package_path"), TEXT("string"), TEXT("Content directory, e.g. /Game/Materials")),
            MCPParam(TEXT("asset_class"), TEXT("string"), TEXT("unreal class, e.g. Material, ParticleSystem")),
            MCPParamOpt(TEXT("factory"), TEXT("string"), TEXT("Factory class; default \"<asset_class>FactoryNew\"")),
            MCPParamOpt(TEXT("recreate"), TEXT("bool"), TEXT("Save+delete an existing asset first (default false)")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleCreateAssetSafe(Params); });

    MCP_REGISTER_COMMAND(Registry, "delete_asset_safe", "asset",
        "Delete an asset without the save-changes modal: a dirty asset is saved silently first, then the "
        "delete runs through the same redirector-aware, blocker-reporting path as safe_delete_asset (a "
        "blocked asset is reported, never prompted).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Asset object path to delete")),
            MCPParamOpt(TEXT("save_dirty"), TEXT("bool"), TEXT("Silently save a dirty asset first (default true)")),
            MCPParamOpt(TEXT("force"), TEXT("bool"), TEXT("Break blocking references, then delete (default false)")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleDeleteAssetSafe(Params); });
}

// Collect the referencer situation for an asset without touching any UI:
//  - OutBlockerPackages: referencer packages that are NOT redirectors (they
//    block a safe delete and are reported instead of prompting)
//  - OutRedirectors: loaded UObjectRedirector instances pointing at the asset
//    (they are deleted first by safe_delete_asset)
bool FUnrealMCPAssetCommands::ResolveAssetBlockers(const FString& AssetPath,
                                                    TArray<FString>& OutBlockerPackages,
                                                    TArray<UObject*>& OutRedirectors,
                                                    TSharedPtr<FJsonObject>& OutError)
{
    const FSoftObjectPath SoftPath(AssetPath);
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& Registry = AssetRegistryModule.Get();

    FAssetData AssetData = Registry.GetAssetByObjectPath(SoftPath);
    if (!AssetData.IsValid() && !AssetPath.Contains(TEXT(".")))
    {
        // Callers pass either the object path ("/Game/Dir/Asset.Asset") or the package path
        // ("/Game/Dir/Asset"). The registry only resolves the former, so build it instead of
        // reporting asset_not_found for an asset that is right there.
        const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *AssetPath, *FPackageName::GetShortName(AssetPath));
        AssetData = Registry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
    }

    if (!AssetData.IsValid())
    {
        OutError = FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("asset_not_found: %s (pass either '/Game/Dir/Asset' or '/Game/Dir/Asset.Asset')"), *AssetPath));
        return false;
    }

    // Take the package name from the resolved AssetData: the caller's string may be a short name or
    // either path form, and referencer lookups need the real package path.
    const FString PackageName = AssetData.PackageName.ToString();
    const FString AssetShortName = AssetData.AssetName.ToString();

    TArray<FName> ReferencerPackages;
    Registry.GetReferencers(FName(*PackageName), ReferencerPackages);

    for (const FName& ReferencerPkg : ReferencerPackages)
    {
        // Skip the asset's own package (self-reference artifacts)
        if (ReferencerPkg.ToString() == PackageName)
        {
            continue;
        }

        // A redirector is resolved through the referencer package: load the
        // redirector object with the target asset's short name.
        UObjectRedirector* Redirector = FindObjectFast<UObjectRedirector>(
            FindObject<UPackage>(nullptr, *ReferencerPkg.ToString()), *AssetShortName);
        if (!Redirector)
        {
            Redirector = LoadObject<UObjectRedirector>(nullptr,
                *(ReferencerPkg.ToString() + TEXT(".") + AssetShortName));
        }

        if (Redirector && Redirector->DestinationObject)
        {
            OutRedirectors.Add(Redirector);
        }
        else
        {
            OutBlockerPackages.Add(ReferencerPkg.ToString());
        }
    }

    // Live level actors never appear as package referencers: a spawned actor of a blueprint class
    // keeps the generated class (and through it the package) alive without any registry entry, so
    // deleting underneath it would silently break the level. Report the owning level as a blocker.
    //
    // FAssetData::GetAsset() returns null for these assets even when they are loaded (the registry
    // entry came from a path form its object-path helper does not resolve), so resolve the object
    // the same way the delete path itself does.
    UObject* AssetObject = FUnrealMCPCommonUtils::FindAsset(AssetPath);
    UClass* AssetClass = Cast<UClass>(AssetObject);
    if (const UBlueprint* Blueprint = Cast<UBlueprint>(AssetObject))
    {
        AssetClass = Blueprint->GeneratedClass;
    }
    if (AssetClass && GWorld)
    {
        for (TActorIterator<AActor> It(GWorld); It; ++It)
        {
            AActor* Actor = *It;
            const ULevel* Level = Actor ? Actor->GetLevel() : nullptr;
            if (Level && Actor->IsA(AssetClass))
            {
                OutBlockerPackages.AddUnique(Level->GetOutermost()->GetName());
            }
        }
    }

    return true;
}

// Self-managed, modal-free object + package file removal.
//
// ObjectTools' delete pipeline is uncontrollable headless: it pops
// SReferenceChecker dialogs, silently returns zero on in-memory references, or
// hangs on synchronous FAssetDeleteModel ticks. External references were
// already refused by the caller (blockers), so finish the delete ourselves:
// MarkAsGarbage + GC (releases the object from most native pins) + direct
// package file removal.
static void SilentDeleteObjectAndPackage(UObject* Object, TArray<FString>* OutLivingClasses = nullptr)
{
    if (!Object || !Object->IsValidLowLevel())
    {
        return;
    }
    UPackage* Package = Object->GetOutermost();
    const FString PackageName = Package ? Package->GetName()
        : FPackageName::ObjectPathToPackageName(Object->GetPathName());

    // Decide whether the package files may be removed BEFORE marking garbage:
    // after GC the package contents (and GetObjectsWithOuter results) are dead
    // memory - dereferencing them crashes. The files are only removed when the
    // package holds nothing else alive (e.g. a redirector-only package).
    bool bSafeToRemoveFiles = false;
    if (Package)
    {
        TArray<UObject*> ObjectsInPackage;
        GetObjectsWithOuter(Package, ObjectsInPackage, /*bIncludeNestedObjects=*/ true);
        bSafeToRemoveFiles = true;
        for (const UObject* PkgObject : ObjectsInPackage)
        {
            // The package files are removed unless the package holds ANOTHER
            // live asset (multi-asset packages must not lose siblings).
            // Editor helper objects (thumbnails, package MetaData) and anything
            // nested inside the deleted object (a material's expressions) are
            // not assets and die with the package.
            if (PkgObject && PkgObject != Object && !PkgObject->IsIn(Object)
                && PkgObject->IsAsset() && !PkgObject->HasAnyFlags(RF_MirroredGarbage))
            {
                bSafeToRemoveFiles = false;
                if (OutLivingClasses)
                {
                    OutLivingClasses->AddUnique(PkgObject->GetClass()->GetName());
                }
            }
        }
    }

    // Drop the in-memory object. MarkAsGarbage checks !IsRooted(), so
    // defensively unroot first (headless crash-safety).
    if (Object->IsRooted())
    {
        Object->RemoveFromRoot();
    }
    Object->MarkAsGarbage();
    CollectGarbage(RF_NoFlags);

    if (!bSafeToRemoveFiles)
    {
        return;
    }

    auto TryDeleteFile = [](const FString& File)
    {
        if (IFileManager::Get().FileExists(*File))
        {
            IFileManager::Get().Delete(*File, /*bRequireExists=*/ true, /*bTree=*/ false, /*bReadOnly=*/ true);
        }
    };
    const FString AssetFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    const FString MapFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetMapPackageExtension());
    TryDeleteFile(AssetFile);
    TryDeleteFile(MapFile);
    const FString BaseNoExt = FPaths::GetBaseFilename(AssetFile, /*bRemovePath=*/ false);
    TryDeleteFile(BaseNoExt + TEXT(".uexp"));
    TryDeleteFile(BaseNoExt + TEXT(".ubulk"));
}

// Read-only twin of ClearReferencesOnObject: does Owner still hold a hard reference to Target?
// Used after the detach pass, where the asset registry's dependency data cannot be trusted: it
// converges on save/rescan, so it keeps naming a referencer whose property we just cleared.
static bool ObjectHoldsReferenceTo(UObject* Owner, UObject* Target)
{
    if (!Owner || !Target)
    {
        return false;
    }

    for (TFieldIterator<FProperty> PropIt(Owner->GetClass()); PropIt; ++PropIt)
    {
        FProperty* Property = *PropIt;
        void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Owner);

        if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
        {
            if (ObjectProperty->GetObjectPropertyValue(ValuePtr) == Target)
            {
                return true;
            }
            continue;
        }

        const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property);
        const FObjectPropertyBase* InnerObjectProperty = ArrayProperty
            ? CastField<FObjectPropertyBase>(ArrayProperty->Inner) : nullptr;
        if (!InnerObjectProperty)
        {
            continue;
        }

        FScriptArrayHelper ArrayHelper(ArrayProperty, ValuePtr);
        for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
        {
            if (InnerObjectProperty->GetObjectPropertyValue(ArrayHelper.GetRawPtr(Index)) == Target)
            {
                return true;
            }
        }
    }
    return false;
}

// Clear every object reference on Owner that points at Target. Covers plain object properties
// (SkeletalMesh::Skeleton / PhysicsAsset, AnimBlueprint::TargetSkeleton, retargeter/preview mesh
// pointers) and arrays of object references. Returns the number of properties changed; their names
// land in OutClearedProperties so the caller can report exactly what was broken.
static int32 ClearReferencesOnObject(UObject* Owner, UObject* Target, TArray<FString>& OutClearedProperties)
{
    if (!Owner || !Target)
    {
        return 0;
    }

    int32 Changed = 0;
    for (TFieldIterator<FProperty> PropIt(Owner->GetClass()); PropIt; ++PropIt)
    {
        FProperty* Property = *PropIt;
        void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Owner);

        if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
        {
            if (ObjectProperty->GetObjectPropertyValue(ValuePtr) == Target)
            {
                Owner->Modify();
                ObjectProperty->SetObjectPropertyValue(ValuePtr, nullptr);
                OutClearedProperties.Add(Property->GetName());
                ++Changed;
            }
            continue;
        }

        const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property);
        const FObjectPropertyBase* InnerObjectProperty = ArrayProperty
            ? CastField<FObjectPropertyBase>(ArrayProperty->Inner) : nullptr;
        if (!InnerObjectProperty)
        {
            continue;
        }

        FScriptArrayHelper ArrayHelper(ArrayProperty, ValuePtr);
        bool bArrayChanged = false;
        for (int32 Index = ArrayHelper.Num() - 1; Index >= 0; --Index)
        {
            if (InnerObjectProperty->GetObjectPropertyValue(ArrayHelper.GetRawPtr(Index)) == Target)
            {
                if (!bArrayChanged)
                {
                    Owner->Modify();
                }
                ArrayHelper.RemoveValues(Index, 1);
                bArrayChanged = true;
            }
        }
        if (bArrayChanged)
        {
            OutClearedProperties.Add(Property->GetName());
            ++Changed;
        }
    }
    return Changed;
}

void FUnrealMCPAssetCommands::DetachReferencers(UObject* Target, TArray<TSharedPtr<FJsonValue>>& OutDetached)
{
    if (!Target)
    {
        return;
    }

    auto AddDetached = [&OutDetached](const FString& Referencer, const FString& How)
    {
        TSharedPtr<FJsonObject> Item = MakeShareable(new FJsonObject);
        Item->SetStringField(TEXT("referencer"), Referencer);
        Item->SetStringField(TEXT("how"), How);
        OutDetached.Add(MakeShareable(new FJsonValueObject(Item)));
    };

    // (1) Level actor instances of this asset's class. They hold no registry entry at all, so the
    // referencer list never mentions them; deleting underneath one would silently break the level.
    UClass* TargetClass = Cast<UClass>(Target);
    if (const UBlueprint* Blueprint = Cast<UBlueprint>(Target))
    {
        TargetClass = Blueprint->GeneratedClass;
    }
    if (TargetClass && GWorld)
    {
        TArray<AActor*> Instances;
        for (TActorIterator<AActor> ActorIt(GWorld); ActorIt; ++ActorIt)
        {
            if (ActorIt->IsA(TargetClass))
            {
                Instances.Add(*ActorIt);
            }
        }
        for (AActor* Instance : Instances)
        {
            AddDetached(Instance->GetPathName(), TEXT("destroyed_level_actor"));
            Instance->Destroy();
        }
    }

    // (2) Object properties inside already-loaded referencer packages. FindPackage never loads, so a
    // referencer that is only on disk is reported as undetachable rather than silently loaded.
    const FString TargetPackageName = Target->GetOutermost()
        ? Target->GetOutermost()->GetName()
        : FPackageName::ObjectPathToPackageName(Target->GetPathName());

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    TArray<FName> ReferencerPackages;
    AssetRegistryModule.Get().GetReferencers(FName(*TargetPackageName), ReferencerPackages);

    for (const FName& ReferencerPackage : ReferencerPackages)
    {
        const FString ReferencerPackageName = ReferencerPackage.ToString();
        if (ReferencerPackageName == TargetPackageName)
        {
            continue;
        }
        UPackage* Package = FindPackage(/*Outer=*/ nullptr, *ReferencerPackageName);
        if (!Package)
        {
            continue;
        }

        TArray<UObject*> ObjectsInPackage;
        GetObjectsWithOuter(Package, ObjectsInPackage, /*bIncludeNestedObjects=*/ true);
        for (UObject* Owner : ObjectsInPackage)
        {
            if (!Owner || Owner == Target || Owner->HasAnyFlags(RF_MirroredGarbage))
            {
                continue;
            }
            TArray<FString> ClearedProperties;
            if (ClearReferencesOnObject(Owner, Target, ClearedProperties) > 0)
            {
                for (const FString& PropertyName : ClearedProperties)
                {
                    AddDetached(Owner->GetPathName(),
                        FString::Printf(TEXT("cleared_property:%s"), *PropertyName));
                }
            }
        }
    }
}

TSharedPtr<FJsonObject> FUnrealMCPAssetCommands::HandleSafeDeleteAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' parameter"));
    }

    // force is opt-in and default-off: with it absent every branch below is byte-for-byte the
    // previous behaviour, so existing callers cannot be surprised by a delete that broke something.
    bool bForce = false;
    Params->TryGetBoolField(TEXT("force"), bForce);

    // Deleting while a play session is live is a trap, not a shortcut: the file half of the delete
    // cannot run in play mode (UEditorAssetLibrary answers "The Editor is currently in a play mode"),
    // while the in-memory objects are removed anyway. That leaves any open asset editor with zero
    // editing objects and trips `Assertion failed: EditingObjects.Num() > 0`
    // (AssetEditorToolkit.cpp:592) on the next tick - measured 2026-09-29, it killed the editor.
    // Refusing the whole call is the honest option; `force` does not bypass it (the danger is the
    // memory half, which force only makes more aggressive).
    if (GEditor && (GEditor->PlayWorld != nullptr || GEditor->bIsSimulatingInEditor))
    {
        TSharedPtr<FJsonObject> Busy = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("pie_running"),
            TEXT("A play session is running; asset deletion is refused because it cannot complete in play mode and would still remove the in-memory objects"));
        Busy->SetStringField(TEXT("hint"), TEXT("Stop the session first (stop_pie), then delete"));
        Busy->SetBoolField(TEXT("pie_running"), GEditor->PlayWorld != nullptr);
        Busy->SetBoolField(TEXT("simulating_in_editor"), GEditor->bIsSimulatingInEditor);
        return Busy;
    }

    TArray<FString> BlockerPackages;
    TArray<UObject*> Redirectors;
    TSharedPtr<FJsonObject> ResolveError;
    if (!ResolveAssetBlockers(AssetPath, BlockerPackages, Redirectors, ResolveError))
    {
        return ResolveError;
    }

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    const bool bWasRedirector = Redirectors.Num() > 0;
    TArray<TSharedPtr<FJsonValue>> BlockersJson;
    for (const FString& Blocker : BlockerPackages)
    {
        BlockersJson.Add(MakeShareable(new FJsonValueString(Blocker)));
    }
    ResultJson->SetArrayField(TEXT("blockers"), BlockersJson);
    ResultJson->SetBoolField(TEXT("was_redirector"), bWasRedirector);

    // Every broken reference is recorded here (empty for a non-force call), so the caller can see
    // which other assets this delete touched instead of having to trust a bare `deleted: true`.
    TArray<TSharedPtr<FJsonValue>> DetachedJson;
    auto FinalizeResult = [&ResultJson, &DetachedJson]()
    {
        ResultJson->SetArrayField(TEXT("detached"), DetachedJson);
        return ResultJson;
    };

    // Referenced by something other than a redirector: refuse to delete and
    // report instead of letting the editor prompt (or silently break refs).
    if (BlockerPackages.Num() > 0)
    {
        if (!bForce)
        {
            ResultJson->SetBoolField(TEXT("success"), true);
            ResultJson->SetBoolField(TEXT("deleted"), false);
            // Measured cause, not a guessed code: this branch is taken exactly when the referencer
            // scan above returned packages, and those packages are the evidence.
            ResultJson->SetStringField(TEXT("reason"), TEXT("blocked_by_referencers"));
            ResultJson->SetArrayField(TEXT("referencers"), BlockersJson);
            ResultJson->SetStringField(TEXT("detail"),
                TEXT("referenced by the packages in blockers; pass force=true to break those references and delete anyway"));
            return FinalizeResult();
        }

        UObject* TargetObject = FUnrealMCPCommonUtils::FindAsset(AssetPath);
        if (TargetObject)
        {
            DetachReferencers(TargetObject, DetachedJson);
        }

        // Verify against the real objects, never against the registry: its dependency data converges
        // only on save/rescan, so GetReferencers keeps naming a referencer whose property we just
        // cleared (measured: the first physics-asset force delete was refused for exactly that
        // reason). A blocker is genuinely undetachable only when we cannot see it at all (package
        // not loaded, so it can be neither inspected nor cleared) or when a loaded object still
        // holds a live pointer at the target after the detach pass.
        TArray<FString> Undetachable;
        if (!TargetObject)
        {
            Undetachable = BlockerPackages;
        }
        else
        {
            for (const FString& Blocker : BlockerPackages)
            {
                UPackage* BlockerPackage = FindPackage(/*Outer=*/ nullptr, *Blocker);
                if (!BlockerPackage)
                {
                    Undetachable.AddUnique(Blocker);
                    continue;
                }
                TArray<UObject*> ObjectsInPackage;
                GetObjectsWithOuter(BlockerPackage, ObjectsInPackage, /*bIncludeNestedObjects=*/ true);
                for (UObject* Owner : ObjectsInPackage)
                {
                    if (ObjectHoldsReferenceTo(Owner, TargetObject))
                    {
                        Undetachable.AddUnique(Blocker);
                        break;
                    }
                }
            }
        }

        if (Undetachable.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> UndetachableJson;
            for (const FString& Blocker : Undetachable)
            {
                UndetachableJson.Add(MakeShareable(new FJsonValueString(Blocker)));
            }
            ResultJson->SetBoolField(TEXT("success"), true);
            ResultJson->SetBoolField(TEXT("deleted"), false);
            ResultJson->SetStringField(TEXT("reason"), TEXT("referencers_not_detachable"));
            ResultJson->SetArrayField(TEXT("referencers"), UndetachableJson);
            ResultJson->SetStringField(TEXT("detail"),
                TEXT("force could not reach these referencers (their packages are not loaded, or a loaded object still points at the target); nothing was deleted"));
            return FinalizeResult();
        }
    }

    for (UObject* Redirector : Redirectors)
    {
        SilentDeleteObjectAndPackage(Redirector);
    }

    // Resolve through the plugin's own asset lookup, which accepts the object path, the package path
    // and a short name. Resolving with FSoftObjectPath(AssetPath) alone only worked for the dotted
    // form; for a package path the fallback found the PACKAGE object instead of the asset, and the
    // verification then compared a path derived from the caller's string - which is how this command
    // reported deleted=true while nothing had been removed.
    UObject* AssetObject = FUnrealMCPCommonUtils::FindAsset(AssetPath);
    if (!AssetObject)
    {
        ResultJson->SetBoolField(TEXT("success"), true);
        ResultJson->SetBoolField(TEXT("deleted"), true);
        return FinalizeResult();
    }

    const FString ObjectPath = AssetObject->GetPathName();
    const FString PackageName = AssetObject->GetOutermost()
        ? AssetObject->GetOutermost()->GetName()
        : FPackageName::ObjectPathToPackageName(ObjectPath);

    // Silently save the dirty package first (mirrors ue-safe delete_asset_safe):
    // a dirty asset that survives the delete would surface as a rename/replace
    // dialog on the next same-name create.
    if (UPackage* Package = AssetObject->GetOutermost())
    {
        if (Package->IsDirty())
        {
            TArray<UPackage*> PackagesToSave;
            PackagesToSave.Add(Package);
            UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, /*bOnlyDirty=*/true);
        }
    }

    // Tell the asset registry about the delete while the object is still valid, exactly the order the
    // engine's own ObjectTools::DeleteSingleObject uses (ObjectTools.cpp:3212). Without it the
    // registry keeps advertising the deleted asset, and every caller that still sees it may try to
    // resolve it - which loads a missing package, and the loader remembers that failure per package
    // id; recreating the same path afterwards then trips AsyncLoading2's check(!bHasFailed) and takes
    // the editor down. A directory-wide forced rescan would achieve the same thing far more
    // expensively, and inside a deferred job it is the rescan that stalls the game thread.
    FAssetRegistryModule::AssetDeleted(AssetObject);

    TArray<FString> LivingClasses;
    RemoveAssetFiles(AssetObject, ObjectPath, PackageName, LivingClasses);

    // Memory half of "the delete really happened": the object must be gone or already marked for GC.
    UObject* StillThere = StaticFindObject(/*Class=*/ nullptr, /*Outer=*/ nullptr, *ObjectPath);
    const bool bMemGone = !StillThere || StillThere->HasAnyFlags(RF_MirroredGarbage);

    const bool bDiskGone = IsPackageFileGone(PackageName);
    TSharedPtr<FJsonObject> Outcome = AddDeleteOutcome(ResultJson, bDiskGone, bMemGone, LivingClasses);
    Outcome->SetBoolField(TEXT("registry_notified"), true);
    Outcome->SetStringField(TEXT("package_name"), PackageName);

    // Report the registry half as a field instead of gating on it: AssetDeleted() only notifies
    // listeners and marks the package empty (AssetRegistry.cpp:4277 - it never calls RemoveAssetData),
    // so the entry itself converges on the editor's own next scan or an explicit
    // scan_paths_synchronous. The direction the spec forbids - registry already empty while the file
    // is still there - is bDiskGone == false above, which already fails this outcome.
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    TArray<FAssetData> RemainingAssetData;
    AssetRegistryModule.Get().GetAssetsByPackageName(FName(*PackageName), RemainingAssetData);
    Outcome->SetBoolField(TEXT("registry_entry_remaining"), RemainingAssetData.Num() > 0);

    return FinalizeResult();
}

bool FUnrealMCPAssetCommands::IsPackageFileGone(const FString& PackageName)
{
    const FString PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    return !IFileManager::Get().FileExists(*PackageFile);
}

void FUnrealMCPAssetCommands::RemoveAssetFiles(UObject* AssetObject, const FString& ObjectPath,
                                               const FString& PackageName, TArray<FString>& OutLivingClasses)
{
    // A Blueprint's package is kept alive by its BlueprintGeneratedClass, which the silent object
    // deletion cannot drop. Order matters here: the path-based editor delete resolves the asset from
    // the registry, and once the blueprint object has been destroyed it has nothing left to resolve -
    // measured: silent-first left the .uasset on disk permanently. So blueprints go through the
    // editor delete first (it cannot prompt at this point, every blocker was ruled out by the caller).
    if (Cast<UBlueprint>(AssetObject))
    {
        UEditorAssetLibrary::DeleteAsset(ObjectPath);
        if (IsPackageFileGone(PackageName))
        {
            return;
        }
    }

    SilentDeleteObjectAndPackage(AssetObject, &OutLivingClasses);

    if (!IsPackageFileGone(PackageName))
    {
        // Last resort for whatever the silent path could not free.
        UEditorAssetLibrary::DeleteAsset(ObjectPath);
    }
}

TSharedPtr<FJsonObject> FUnrealMCPAssetCommands::AddDeleteOutcome(const TSharedPtr<FJsonObject>& ResultJson,
                                                                  bool bDiskGone, bool bMemGone,
                                                                  const TArray<FString>& LivingClasses)
{
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetBoolField(TEXT("deleted"), bDiskGone && bMemGone);

    // A failed delete MUST name the cause this call actually measured; each value maps to exactly
    // one branch here. There is deliberately no code table for causes nobody has observed - a
    // guessed code is worse than no code, because callers branch on it.
    if (!(bDiskGone && bMemGone))
    {
        ResultJson->SetStringField(TEXT("reason"), LivingClasses.Num() > 0
            ? TEXT("living_objects_in_package")
            : (bDiskGone ? TEXT("memory_pinned") : TEXT("package_file_locked")));
        ResultJson->SetArrayField(TEXT("referencers"), TArray<TSharedPtr<FJsonValue>>());
    }

    if (bDiskGone && !bMemGone)
    {
        ResultJson->SetStringField(TEXT("detail"),
            TEXT("package files removed but the object is still pinned in memory (e.g. python globals); clear references and retry"));
    }
    else if (!bDiskGone)
    {
        if (LivingClasses.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> LivingClassesJson;
            for (const FString& LivingClass : LivingClasses)
            {
                LivingClassesJson.Add(MakeShareable(new FJsonValueString(LivingClass)));
            }
            ResultJson->SetArrayField(TEXT("living_object_classes"), LivingClassesJson);
        }
        ResultJson->SetStringField(TEXT("detail"), LivingClasses.Num() > 0
            ? FString::Printf(TEXT("package file kept: living objects in package: %s"), *FString::Join(LivingClasses, TEXT(", ")))
            : TEXT("failed to remove the package file (read-only or locked); delete it manually"));
    }
    return ResultJson;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetCommands::HandleListAssetBlockers(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' parameter"));
    }

    TArray<FString> BlockerPackages;
    TArray<UObject*> Redirectors;
    TSharedPtr<FJsonObject> ResolveError;
    if (!ResolveAssetBlockers(AssetPath, BlockerPackages, Redirectors, ResolveError))
    {
        return ResolveError;
    }

    TArray<TSharedPtr<FJsonValue>> BlockersJson;
    for (const FString& Blocker : BlockerPackages)
    {
        BlockersJson.Add(MakeShareable(new FJsonValueString(Blocker)));
    }
    TArray<TSharedPtr<FJsonValue>> RedirectorsJson;
    for (const UObject* Redirector : Redirectors)
    {
        RedirectorsJson.Add(MakeShareable(new FJsonValueString(Redirector->GetPathName())));
    }

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetArrayField(TEXT("blockers"), BlockersJson);
    ResultJson->SetArrayField(TEXT("redirectors"), RedirectorsJson);
    return ResultJson;
}

bool FUnrealMCPAssetCommands::ResolveAssetStatusPaths(const FString& AssetPath, FString& OutPackageName,
                                                      FString& OutObjectPath)
{
    if (AssetPath.IsEmpty() || !AssetPath.StartsWith(TEXT("/")))
    {
        return false;
    }

    if (AssetPath.Contains(TEXT(".")))
    {
        OutObjectPath = AssetPath;
        OutPackageName = FPackageName::ObjectPathToPackageName(AssetPath);
    }
    else
    {
        OutPackageName = AssetPath;
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *AssetPath, *FPaths::GetBaseFilename(AssetPath));
    }
    return !OutPackageName.IsEmpty() && !OutObjectPath.IsEmpty();
}

TSharedPtr<FJsonObject> FUnrealMCPAssetCommands::HandleAssetStatus(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("path"), AssetPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'path' parameter"));
    }

    FString PackageName;
    FString ObjectPath;
    if (!ResolveAssetStatusPaths(AssetPath, PackageName, ObjectPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("invalid_path: %s (pass '/Game/Dir/Asset' or '/Game/Dir/Asset.Asset')"), *AssetPath));
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& Registry = AssetRegistryModule.Get();

    // The file is the half that cannot lie even for a moment: memory empties out right after PIE
    // ends or a GC, and the registry keeps advertising until its next scan.
    const FString PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    const bool bOnDisk = IFileManager::Get().FileExists(*PackageFile);

    const FAssetData AssetData = Registry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
    bool bRegistry = AssetData.IsValid();
    if (!bRegistry)
    {
        TArray<FAssetData> AssetsInPackage;
        Registry.GetAssetsByPackageName(FName(*PackageName), AssetsInPackage);
        bRegistry = AssetsInPackage.Num() > 0;
    }

    // Object-table lookup by the resolved object path - deliberately NOT "LoadObject() == nullptr":
    // that returns null for every asset after PIE/GC, which is the exact conflation that once got
    // 29 .uasset files deleted as "disk orphans". This also never triggers a load.
    const bool bInMemory = StaticFindObject(/*Class=*/ nullptr, /*Outer=*/ nullptr, *ObjectPath) != nullptr;

    TArray<FName> ReferencerPackages;
    Registry.GetReferencers(FName(*PackageName), ReferencerPackages);

    TArray<TSharedPtr<FJsonValue>> ReferencersJson;
    for (const FName& Referencer : ReferencerPackages)
    {
        ReferencersJson.Add(MakeShareable(new FJsonValueString(Referencer.ToString())));
    }

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("path"), AssetPath);
    ResultJson->SetStringField(TEXT("package_name"), PackageName);
    ResultJson->SetStringField(TEXT("object_path"), ObjectPath);
    ResultJson->SetBoolField(TEXT("in_memory"), bInMemory);
    ResultJson->SetBoolField(TEXT("on_disk"), bOnDisk);
    ResultJson->SetBoolField(TEXT("registry"), bRegistry);
    ResultJson->SetArrayField(TEXT("referencers"), ReferencersJson);
    return ResultJson;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetCommands::HandleListDiskOnlyAssets(const TSharedPtr<FJsonObject>& Params)
{
    FString Directory;
    if (!Params->TryGetStringField(TEXT("dir"), Directory))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'dir' parameter"));
    }

    const FString FileDirectory = FPackageName::LongPackageNameToFilename(Directory, TEXT(""));
    if (!IFileManager::Get().DirectoryExists(*FileDirectory))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("dir_not_found: %s (content path expected, e.g. '/Game/MCP/Ganyu')"), *Directory));
    }

    TArray<FString> FileNames;
    IFileManager::Get().FindFiles(FileNames, *(FileDirectory / TEXT("*.uasset")), /*Files=*/ true, /*Directories=*/ false);

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& Registry = AssetRegistryModule.Get();

    const FString CleanDirectory = Directory.EndsWith(TEXT("/")) ? Directory.LeftChop(1) : Directory;

    TArray<TSharedPtr<FJsonValue>> DiskOnlyJson;
    for (const FString& FileName : FileNames)
    {
        // Package path derived from the file name, never by loading: these files have no registry
        // entry, and a load attempt would leave the loader with a permanent failure mark for them.
        const FString PackageName = FPaths::Combine(CleanDirectory, FPaths::GetBaseFilename(FileName));
        TArray<FAssetData> AssetsInPackage;
        Registry.GetAssetsByPackageName(FName(*PackageName), AssetsInPackage);
        if (AssetsInPackage.Num() == 0)
        {
            DiskOnlyJson.Add(MakeShareable(new FJsonValueString(PackageName)));
        }
    }

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("dir"), Directory);
    ResultJson->SetNumberField(TEXT("scanned_files"), FileNames.Num());
    ResultJson->SetNumberField(TEXT("count"), DiskOnlyJson.Num());
    ResultJson->SetArrayField(TEXT("disk_only"), DiskOnlyJson);
    ResultJson->SetStringField(TEXT("note"),
        TEXT("read-only: files listed here are NOT deleted. Remove them from the filesystem with the editor closed."));
    return ResultJson;
}

// Shared volume lookup: by actor name first, falling back to a unique label.
// Returns null and an error response when no PostProcessVolume matches.
static APostProcessVolume* FindPostProcessVolume(const FString& VolumeName, TSharedPtr<FJsonObject>& OutError)
{
    TArray<AActor*> Volumes;
    UGameplayStatics::GetAllActorsOfClass(GWorld, APostProcessVolume::StaticClass(), Volumes);

    for (AActor* Actor : Volumes)
    {
        APostProcessVolume* Volume = Cast<APostProcessVolume>(Actor);
        if (Volume && Actor->GetName() == VolumeName)
        {
            return Volume;
        }
    }
    // Label fallback
    for (AActor* Actor : Volumes)
    {
        APostProcessVolume* Volume = Cast<APostProcessVolume>(Actor);
        if (Volume && Actor->GetActorLabel() == VolumeName)
        {
            return Volume;
        }
    }

    OutError = FUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("volume_not_found: %s"), *VolumeName));
    return nullptr;
}

// The blendables array changes are not picked up by the renderer until the
// volume state is refreshed (same trick as add_blendable_to_post_volume).
static void RefreshVolumeRenderState(APostProcessVolume* Volume)
{
    Volume->bEnabled = false;
    Volume->bEnabled = true;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetCommands::HandleClearBlendables(const TSharedPtr<FJsonObject>& Params)
{
    FString VolumeName;
    if (!Params->TryGetStringField(TEXT("volume_name"), VolumeName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'volume_name' parameter"));
    }

    TSharedPtr<FJsonObject> FindError;
    APostProcessVolume* Volume = FindPostProcessVolume(VolumeName, FindError);
    if (!Volume)
    {
        return FindError;
    }

    const int32 ClearedCount = Volume->Settings.WeightedBlendables.Array.Num();
    Volume->Settings.WeightedBlendables.Array.Empty();
    RefreshVolumeRenderState(Volume);
    Volume->Modify();

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetNumberField(TEXT("cleared"), ClearedCount);
    return ResultJson;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetCommands::HandleRemoveBlendable(const TSharedPtr<FJsonObject>& Params)
{
    FString VolumeName;
    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("volume_name"), VolumeName) ||
        !Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'volume_name' or 'material_path' parameter"));
    }

    TSharedPtr<FJsonObject> FindError;
    APostProcessVolume* Volume = FindPostProcessVolume(VolumeName, FindError);
    if (!Volume)
    {
        return FindError;
    }

    UObject* Material = StaticLoadObject(UObject::StaticClass(), nullptr, *MaterialPath);
    if (!Material)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("material_not_found: %s"), *MaterialPath));
    }

    const int32 Before = Volume->Settings.WeightedBlendables.Array.Num();
    Volume->Settings.WeightedBlendables.Array.RemoveAll([Material](const FWeightedBlendable& Entry)
    {
        return Entry.Object == Material;
    });
    const int32 RemovedCount = Before - Volume->Settings.WeightedBlendables.Array.Num();
    RefreshVolumeRenderState(Volume);
    Volume->Modify();
    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetNumberField(TEXT("removed"), RemovedCount);
    return ResultJson;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetCommands::HandleListBlendables(const TSharedPtr<FJsonObject>& Params)
{
    FString VolumeName;
    if (!Params->TryGetStringField(TEXT("volume_name"), VolumeName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'volume_name' parameter"));
    }

    TSharedPtr<FJsonObject> FindError;
    APostProcessVolume* Volume = FindPostProcessVolume(VolumeName, FindError);
    if (!Volume)
    {
        return FindError;
    }

    TArray<TSharedPtr<FJsonValue>> Entries;
    for (const FWeightedBlendable& Entry : Volume->Settings.WeightedBlendables.Array)
    {
        TSharedPtr<FJsonObject> EntryJson = MakeShareable(new FJsonObject);
        EntryJson->SetStringField(TEXT("object"), Entry.Object ? Entry.Object->GetPathName() : TEXT(""));
        EntryJson->SetNumberField(TEXT("weight"), Entry.Weight);
        Entries.Add(MakeShareable(new FJsonValueObject(EntryJson)));
    }

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetArrayField(TEXT("blendables"), Entries);
    return ResultJson;
}

// ---------------------------------------------------------------------------
// move_asset / move_directory
//
// Why this is a command instead of "just call python rename_asset":
//
// A hard reference is a pointer, so a rename is free for the in-memory references - they follow the
// object. What goes stale is the *serialized* path inside every referencer package: it keeps naming
// the old path until that package is written out again. The engine's own FAssetRenameManager
// (AssetRenameManager.cpp) reads the referencer list from the AssetRegistry dependency graph, loads
// those packages and writes them all back unconditionally
// (PromptForCheckoutAndSave(..., bCheckDirty=false), AssetRenameManager.cpp:1879); referencers it
// cannot load - map packages (IsMapPackageAsset, :1008) or packages whose load fails (:1046) - make
// it leave a redirector instead, so the old path still resolves.
//
// Two gaps are closed on top of that, both measured:
//   1. The dependency graph is gathered data, so an asset created and saved in this session is not
//      in it yet (get_referencers -> [] before a scan, [referencer] after). The engine then neither
//      fixes nor protects that referencer. Refreshing search_paths before the rename makes the
//      engine's own read see it.
//   2. Referencers the refreshed graph still does not know are found by scanning loaded packages for
//      hard references, and written here afterwards.
// ---------------------------------------------------------------------------

namespace
{
    // One package that references the asset being moved.
    struct FMoveReferencer
    {
        FString PackageName;
        FString Source;          // "registry" (dependency graph) or "loaded" (hard-reference scan)
        bool bMapPackage = false;
        bool bLoadable = false;  // not a map package, and either in memory or loadable from disk
    };

    struct FMoveOptions
    {
        bool bUpdateReferencers = true;
        bool bRefreshRegistry = true;
        bool bDryRun = false;
        TArray<FString> SearchPaths;
    };

    bool IsPieActive()
    {
        return GEditor && GEditor->PlayWorld != nullptr;
    }

    FString NormalizeContentDirectory(const FString& Directory)
    {
        FString Result = Directory;
        while (Result.EndsWith(TEXT("/")))
        {
            Result.LeftChopInline(1);
        }
        return Result;
    }

    // "/Game/A/B" or "/Game/A/B.B" -> package "/Game/A/B" + object name "B".
    // Deeper validation is the engine's (IsAValidPathForCreateNewAsset inside RenameLoadedAsset); this
    // only rejects what would otherwise surface as an opaque rename_failed.
    bool ResolveMoveDestination(const FString& NewPath, FString& OutPackageName, FString& OutObjectName)
    {
        if (NewPath.IsEmpty() || !NewPath.StartsWith(TEXT("/")))
        {
            return false;
        }

        OutPackageName = NewPath.Contains(TEXT("."))
            ? FPackageName::ObjectPathToPackageName(NewPath)
            : NewPath;
        OutObjectName = NewPath.Contains(TEXT("."))
            ? FPackageName::ObjectPathToObjectName(NewPath)
            : FPackageName::GetShortName(NewPath);

        return !OutObjectName.IsEmpty() && FPackageName::IsValidLongPackageName(OutPackageName);
    }

    bool ContentPackageExists(const FString& PackageName)
    {
        if (FPackageName::DoesPackageExist(PackageName))
        {
            return true;
        }
        return FindPackage(/*Outer=*/ nullptr, *PackageName) != nullptr;
    }

    bool ContentDirectoryExists(const FString& Directory)
    {
        const FString FileDirectory = FPackageName::LongPackageNameToFilename(Directory, TEXT(""));
        if (IFileManager::Get().DirectoryExists(*FileDirectory))
        {
            return true;
        }

        FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        TArray<FAssetData> Assets;
        AssetRegistryModule.Get().GetAssetsByPath(FName(*Directory), Assets, /*bRecursive=*/ true);
        return Assets.Num() > 0;
    }

    // Non-forced: the point is to gather what is already on disk (fresh saves), not to re-read every
    // file. Measured 0.58s for all of /Game on this project.
    void RefreshRegistryPaths(const TArray<FString>& Paths)
    {
        if (Paths.Num() == 0)
        {
            return;
        }
        FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        AssetRegistryModule.Get().ScanPathsSynchronous(Paths, /*bForceRescan=*/ false, /*bWaitForCompletion=*/ true);
    }

    // Referencer superset: the dependency graph's answer, plus every loaded package whose objects hold
    // a hard reference to the target. The second half is what catches referencers the graph has not
    // gathered yet; it never loads anything (FindPackage-style only), so it cannot pull in the world.
    void CollectMoveReferencers(UObject* Target, TArray<FMoveReferencer>& OutReferencers, int32& OutLoadedScanned)
    {
        if (!Target)
        {
            return;
        }

        const FString TargetPackageName = Target->GetOutermost()
            ? Target->GetOutermost()->GetName()
            : FPackageName::ObjectPathToPackageName(Target->GetPathName());

        TSet<FString> Seen;
        Seen.Add(TargetPackageName);

        FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        TArray<FName> RegistryReferencers;
        AssetRegistryModule.Get().GetReferencers(FName(*TargetPackageName), RegistryReferencers);
        for (const FName& PackageName : RegistryReferencers)
        {
            const FString Name = PackageName.ToString();
            if (Seen.Contains(Name))
            {
                continue;
            }
            Seen.Add(Name);
            FMoveReferencer Referencer;
            Referencer.PackageName = Name;
            Referencer.Source = TEXT("registry");
            OutReferencers.Add(Referencer);
        }

        TArray<UObject*> LoadedPackages;
        GetObjectsOfClass(UPackage::StaticClass(), LoadedPackages, /*bIncludeDerivedClasses=*/ false);
        for (UObject* PackageObject : LoadedPackages)
        {
            UPackage* Package = Cast<UPackage>(PackageObject);
            if (!Package || Package == GetTransientPackage())
            {
                continue;
            }

            const FString PackageName = Package->GetName();
            // Script packages hold engine classes, never authored referencers.
            if (PackageName.StartsWith(TEXT("/Script")))
            {
                continue;
            }

            ++OutLoadedScanned;
            if (Seen.Contains(PackageName))
            {
                continue;
            }

            TArray<UObject*> ObjectsInPackage;
            GetObjectsWithOuter(Package, ObjectsInPackage, /*bIncludeNestedObjects=*/ true);
            for (UObject* Owner : ObjectsInPackage)
            {
                if (!Owner || Owner == Target || Owner->HasAnyFlags(RF_MirroredGarbage))
                {
                    continue;
                }
                if (ObjectHoldsReferenceTo(Owner, Target))
                {
                    Seen.Add(PackageName);
                    FMoveReferencer Referencer;
                    Referencer.PackageName = PackageName;
                    Referencer.Source = TEXT("loaded");
                    OutReferencers.Add(Referencer);
                    break;
                }
            }
        }
    }

    void ClassifyMoveReferencers(TArray<FMoveReferencer>& Referencers)
    {
        for (FMoveReferencer& Referencer : Referencers)
        {
            // Same test the engine uses to decide it must leave a redirector instead of fixing up.
            Referencer.bMapPackage = FEditorFileUtils::IsMapPackageAsset(Referencer.PackageName);
            Referencer.bLoadable = !Referencer.bMapPackage && ContentPackageExists(Referencer.PackageName);
        }
    }

    // "Fixing" a hard-reference referencer on disk == writing its package out again: the serialized
    // path is read from the object's current name, so the rename itself is the fix (soft references
    // are rewritten by the engine's own rename). The write is unconditional on purpose - a fixed-up
    // referencer is not marked dirty (measured), so a dirty-only save would skip exactly the packages
    // that need writing; the engine writes its referencer list unconditionally for the same reason.
    bool SaveMoveReferencer(const FString& PackageName, FString& OutReason)
    {
        UPackage* Package = FindPackage(/*Outer=*/ nullptr, *PackageName);
        if (!Package)
        {
            if (!FPackageName::DoesPackageExist(PackageName))
            {
                OutReason = TEXT("package_missing");
                return false;
            }
            Package = LoadPackage(/*Outer=*/ nullptr, *PackageName, LOAD_None);
        }
        if (!Package)
        {
            OutReason = TEXT("load_failed");
            return false;
        }
        if (!FUnrealMCPCommonUtils::SaveAssetForObject(Package))
        {
            OutReason = TEXT("save_failed");
            return false;
        }
        return true;
    }

    TSharedPtr<FJsonObject> MakeReferencerJson(const FMoveReferencer& Referencer)
    {
        TSharedPtr<FJsonObject> Item = MakeShareable(new FJsonObject);
        Item->SetStringField(TEXT("package"), Referencer.PackageName);
        Item->SetStringField(TEXT("source"), Referencer.Source);
        Item->SetBoolField(TEXT("map_package"), Referencer.bMapPackage);
        Item->SetBoolField(TEXT("loadable"), Referencer.bLoadable);
        return Item;
    }

    TSharedPtr<FJsonObject> MakeMoveFailure(const FString& Entry, const FString& Reason)
    {
        TSharedPtr<FJsonObject> Item = MakeShareable(new FJsonObject);
        Item->SetStringField(TEXT("package"), Entry);
        Item->SetStringField(TEXT("reason"), Reason);
        return Item;
    }

    // The engine leaves a redirector only for the referencers ITS list knows (AssetRenameManager reads
    // the registry, and the registry can be stale even after a refresh - measured with a blueprint and
    // with a map). A referencer that only the loaded-package scan found therefore gets neither a fixup
    // nor a redirector, and the old path stops resolving while that referencer still names it. Create
    // the redirector here in exactly that case, the same shape the engine creates for its own
    // (ObjectTools.cpp:4243 -> UEditorEngine::RenameObject(..., REN_None) -> UObjectRedirector saved
    // in the old package + AssetRegistry notification at ObjectTools.cpp:4265-4275).
    // The parameter must not be called NewObject: that shadows the NewObject<> template used below.
    bool CreateRedirectorAtOldPath(const FString& OldObjectPath, UObject* NewDestination, FString& OutError)
    {
        if (!NewDestination)
        {
            OutError = TEXT("no_destination_object");
            return false;
        }

        const FString OldPackageName = FPackageName::ObjectPathToPackageName(OldObjectPath);
        const FString OldAssetName = FPackageName::ObjectPathToObjectName(OldObjectPath);
        if (OldAssetName.IsEmpty() || OldPackageName.IsEmpty())
        {
            OutError = TEXT("old_path_unparsable");
            return false;
        }
        // Never stomp on something that took the old path in the meantime.
        if (FPackageName::DoesPackageExist(OldPackageName) || FindPackage(/*Outer=*/ nullptr, *OldPackageName))
        {
            OutError = TEXT("old_path_occupied");
            return false;
        }

        UPackage* OldPackage = CreatePackage(*OldPackageName);
        if (!OldPackage)
        {
            OutError = TEXT("package_create_failed");
            return false;
        }

        UObjectRedirector* Redirector = NewObject<UObjectRedirector>(OldPackage, *OldAssetName,
                                                                    RF_Standalone | RF_Public);
        if (!Redirector)
        {
            OutError = TEXT("redirector_create_failed");
            return false;
        }
        Redirector->DestinationObject = NewDestination;

        // Saved unconditionally: the point of the redirector is to still be there after a restart, and
        // it has to be on disk for the old path to resolve through the loader.
        if (!FUnrealMCPCommonUtils::SaveAssetForObject(Redirector))
        {
            OutError = TEXT("redirector_save_failed");
            return false;
        }
        FAssetRegistryModule::AssetCreated(Redirector);
        return true;
    }
}

// Shared by move_asset (one asset) and move_directory (per asset). Assumes the caller already
// validated the destination, checked PIE and refreshed the registry.
static TSharedPtr<FJsonObject> MoveOneAsset(UObject* SourceObject, const FString& NewPath,
                                            const FMoveOptions& Options, TSharedPtr<FJsonObject>& OutError)
{
    UEditorAssetSubsystem* AssetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
    if (!SourceObject || !AssetSubsystem)
    {
        OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("asset_subsystem_unavailable"),
            TEXT("no UEditorAssetSubsystem: this command only runs inside a full editor"));
        return nullptr;
    }

    FString DestPackageName;
    FString DestObjectName;
    if (!ResolveMoveDestination(NewPath, DestPackageName, DestObjectName))
    {
        OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_destination"),
            FString::Printf(TEXT("'%s' is not a content path ('/Game/Dir/Name')"), *NewPath));
        return nullptr;
    }

    const FString OldObjectPath = SourceObject->GetPathName();
    const FString OldPackageName = SourceObject->GetOutermost() ? SourceObject->GetOutermost()->GetName() : TEXT("");
    const FString NewObjectPath = FString::Printf(TEXT("%s.%s"), *DestPackageName, *DestObjectName);

    // The referencer reading is shared by dry_run and the real move, so a preview cannot disagree
    // with what the move would do.
    TArray<FMoveReferencer> Referencers;
    int32 LoadedScanned = 0;
    CollectMoveReferencers(SourceObject, Referencers, LoadedScanned);
    ClassifyMoveReferencers(Referencers);

    TArray<TSharedPtr<FJsonValue>> FoundJson;
    TArray<TSharedPtr<FJsonValue>> UnstorableJson;
    for (const FMoveReferencer& Referencer : Referencers)
    {
        FoundJson.Add(MakeShareable(new FJsonValueObject(MakeReferencerJson(Referencer))));
        if (!Referencer.bLoadable)
        {
            UnstorableJson.Add(MakeShareable(new FJsonValueObject(MakeMoveFailure(Referencer.PackageName,
                Referencer.bMapPackage ? TEXT("map_package") : TEXT("package_missing")))));
        }
    }

    // The engine decides the redirector itself, from its own (now refreshed) referencer list: it is
    // forced whenever a referencer cannot be loaded, which is the same condition as unstorable[].
    const bool bExpectRedirector = UnstorableJson.Num() > 0;

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("source"), OldObjectPath);
    ResultJson->SetStringField(TEXT("destination"), NewObjectPath);
    ResultJson->SetBoolField(TEXT("dry_run"), Options.bDryRun);
    ResultJson->SetBoolField(TEXT("update_referencers"), Options.bUpdateReferencers);
    ResultJson->SetArrayField(TEXT("referencers_found"), FoundJson);
    ResultJson->SetArrayField(TEXT("unstorable"), UnstorableJson);
    ResultJson->SetNumberField(TEXT("loaded_scan_packages"), LoadedScanned);

    if (Options.bDryRun)
    {
        ResultJson->SetBoolField(TEXT("renamed"), false);
        ResultJson->SetBoolField(TEXT("redirector_left"), bExpectRedirector);
        ResultJson->SetArrayField(TEXT("referencers_saved"), TArray<TSharedPtr<FJsonValue>>());
        ResultJson->SetArrayField(TEXT("referencers_failed"), TArray<TSharedPtr<FJsonValue>>());
        ResultJson->SetStringField(TEXT("note"),
            TEXT("dry run: nothing was moved, saved or created. redirector_left is what the move would leave."));
        return ResultJson;
    }

    if (!AssetSubsystem->RenameLoadedAsset(SourceObject, NewPath))
    {
        OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("rename_failed"),
            FString::Printf(TEXT("the engine refused to rename %s to %s (see the editor log)"),
                *OldObjectPath, *NewObjectPath));
        return nullptr;
    }

    TArray<TSharedPtr<FJsonValue>> SavedJson;
    TArray<TSharedPtr<FJsonValue>> FailedJson;
    if (Options.bUpdateReferencers)
    {
        for (const FMoveReferencer& Referencer : Referencers)
        {
            if (!Referencer.bLoadable)
            {
                continue;  // reported in unstorable[]; the engine left a redirector for it
            }
            FString Reason;
            if (SaveMoveReferencer(Referencer.PackageName, Reason))
            {
                SavedJson.Add(MakeShareable(new FJsonValueObject(MakeReferencerJson(Referencer))));
            }
            else
            {
                FailedJson.Add(MakeShareable(new FJsonValueObject(MakeMoveFailure(Referencer.PackageName, Reason))));
            }
        }
    }

    // Compensation for referencers the engine's own list cannot see: it only leaves a redirector when
    // a referencer of ITS list fails to load. Measured case: a map that references the asset, found
    // only by the loaded-package scan, so the engine left nothing and the old path went dangling while
    // the map's disk data still named it.
    FString RedirectorError;
    bool bCommandCreatedRedirector = false;
    if (UnstorableJson.Num() > 0
        && !StaticFindObject(UObject::StaticClass(), /*Outer=*/ nullptr, *OldObjectPath))
    {
        UObject* RenamedObject = StaticFindObject(UObject::StaticClass(), /*Outer=*/ nullptr, *NewObjectPath);
        bCommandCreatedRedirector = CreateRedirectorAtOldPath(OldObjectPath, RenamedObject, RedirectorError);
    }

    // Measured, not inferred: resolve the old path and look at what is actually there.
    UObject* OldResolved = StaticFindObject(UObject::StaticClass(), /*Outer=*/ nullptr, *OldObjectPath);
    if (!OldResolved && FPackageName::DoesPackageExist(OldPackageName))
    {
        OldResolved = LoadObject<UObject>(/*Outer=*/ nullptr, *OldObjectPath);
    }
    const UObjectRedirector* OldRedirector = Cast<UObjectRedirector>(OldResolved);
    const UObject* NewResolved = StaticFindObject(UObject::StaticClass(), /*Outer=*/ nullptr, *NewObjectPath);

    ResultJson->SetBoolField(TEXT("renamed"), true);
    ResultJson->SetArrayField(TEXT("referencers_saved"), SavedJson);
    ResultJson->SetArrayField(TEXT("referencers_failed"), FailedJson);
    ResultJson->SetBoolField(TEXT("redirector_left"), OldRedirector != nullptr);
    ResultJson->SetBoolField(TEXT("redirector_created_by_command"), bCommandCreatedRedirector);
    if (!RedirectorError.IsEmpty())
    {
        ResultJson->SetStringField(TEXT("redirector_error"), RedirectorError);
    }
    ResultJson->SetBoolField(TEXT("old_path_resolves"), OldResolved != nullptr);
    ResultJson->SetStringField(TEXT("old_path_target"),
        OldRedirector && OldRedirector->DestinationObject ? OldRedirector->DestinationObject->GetPathName()
        : (OldResolved ? OldResolved->GetPathName() : TEXT("")));
    ResultJson->SetStringField(TEXT("new_path_loaded"), NewResolved ? NewResolved->GetPathName() : TEXT(""));
    ResultJson->SetBoolField(TEXT("in_memory_after"), NewResolved != nullptr);
    ResultJson->SetBoolField(TEXT("on_disk_after"), FPackageName::DoesPackageExist(DestPackageName));
    ResultJson->SetStringField(TEXT("old_package"), OldPackageName);
    ResultJson->SetStringField(TEXT("note"),
        TEXT("referencer packages are written unconditionally (a fixed-up referencer is not marked dirty); "
             "unstorable[] entries have no redirector-free alternative"));
    return ResultJson;
}

// Shared option reading for both commands: identical defaults, identical meaning.
static bool ReadMoveOptions(const TSharedPtr<FJsonObject>& Params, FMoveOptions& OutOptions)
{
    Params->TryGetBoolField(TEXT("update_referencers"), OutOptions.bUpdateReferencers);
    Params->TryGetBoolField(TEXT("refresh_registry"), OutOptions.bRefreshRegistry);
    Params->TryGetBoolField(TEXT("dry_run"), OutOptions.bDryRun);

    const TArray<TSharedPtr<FJsonValue>>* SearchPaths = nullptr;
    if (Params->TryGetArrayField(TEXT("search_paths"), SearchPaths) && SearchPaths)
    {
        for (const TSharedPtr<FJsonValue>& Value : *SearchPaths)
        {
            FString Path;
            if (Value.IsValid() && Value->TryGetString(Path) && Path.StartsWith(TEXT("/")))
            {
                OutOptions.SearchPaths.Add(Path);
            }
        }
    }
    if (OutOptions.SearchPaths.Num() == 0)
    {
        OutOptions.SearchPaths.Add(TEXT("/Game"));
    }
    return true;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetCommands::HandleMoveAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    FString NewPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("new_path"), NewPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'new_path' parameter"));
    }

    FMoveOptions Options;
    ReadMoveOptions(Params, Options);

    // Every refusal happens before the first write: a rejected move must leave no half-renamed state.
    if (IsPieActive())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("in_pie"),
            TEXT("asset moves are refused while a Play In Editor session is running"));
    }

    FString DestPackageName;
    FString DestObjectName;
    if (!ResolveMoveDestination(NewPath, DestPackageName, DestObjectName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_destination"),
            FString::Printf(TEXT("'%s' is not a content path ('/Game/Dir/Name')"), *NewPath));
    }

    // Resolve the source without loading a path whose package file is gone: a failed load leaves a
    // permanent failed-load marker for the package id (see FUnrealMCPCommonUtils::FindAsset).
    FString SourcePackageName;
    FString SourceObjectPath;
    if (!ResolveAssetStatusPaths(AssetPath, SourcePackageName, SourceObjectPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_source"),
            FString::Printf(TEXT("'%s' is not a content path ('/Game/Dir/Asset')"), *AssetPath));
    }
    if (!FPackageName::DoesPackageExist(SourcePackageName)
        && FindPackage(/*Outer=*/ nullptr, *SourcePackageName) == nullptr)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("source_missing"),
            FString::Printf(TEXT("%s is neither in memory nor on disk"), *SourcePackageName));
    }

    if (ContentPackageExists(DestPackageName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("destination_exists"),
            FString::Printf(TEXT("%s already exists; pick another name (this command never overwrites)"),
                *DestPackageName));
    }

    const bool bRefreshed = Options.bRefreshRegistry;
    if (bRefreshed)
    {
        RefreshRegistryPaths(Options.SearchPaths);
    }

    UObject* SourceObject = FUnrealMCPCommonUtils::FindAsset(AssetPath);
    if (!SourceObject)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("source_load_failed"),
            FString::Printf(TEXT("%s exists on disk but could not be loaded"), *SourceObjectPath));
    }

    TSharedPtr<FJsonObject> MoveError;
    TSharedPtr<FJsonObject> ResultJson = MoveOneAsset(SourceObject, NewPath, Options, MoveError);
    if (!ResultJson)
    {
        return MoveError;
    }
    ResultJson->SetBoolField(TEXT("registry_refreshed"), bRefreshed);
    return ResultJson;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetCommands::HandleMoveDirectory(const TSharedPtr<FJsonObject>& Params)
{
    FString Directory;
    FString NewDirectory;
    if (!Params->TryGetStringField(TEXT("dir"), Directory))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'dir' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("new_dir"), NewDirectory))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'new_dir' parameter"));
    }

    FMoveOptions Options;
    ReadMoveOptions(Params, Options);

    const FString SourceDir = NormalizeContentDirectory(Directory);
    const FString DestDir = NormalizeContentDirectory(NewDirectory);

    if (IsPieActive())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("in_pie"),
            TEXT("directory moves are refused while a Play In Editor session is running"));
    }
    if (!DestDir.StartsWith(TEXT("/")) || !FPackageName::IsValidLongPackageName(DestDir))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_destination"),
            FString::Printf(TEXT("'%s' is not a content directory ('/Game/Dir')"), *NewDirectory));
    }
    if (SourceDir == DestDir || DestDir.StartsWith(SourceDir + TEXT("/")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_destination"),
            TEXT("the destination must be outside the source directory"));
    }
    if (!ContentDirectoryExists(SourceDir))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("source_missing"),
            FString::Printf(TEXT("%s is not a content directory with assets"), *SourceDir));
    }
    if (ContentDirectoryExists(DestDir))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("destination_exists"),
            FString::Printf(TEXT("%s already exists; pick another directory (this command never merges)"),
                *DestDir));
    }

    const bool bRefreshed = Options.bRefreshRegistry;
    if (bRefreshed)
    {
        RefreshRegistryPaths(Options.SearchPaths);
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    TArray<FAssetData> AssetsInDirectory;
    AssetRegistryModule.Get().GetAssetsByPath(FName(*SourceDir), AssetsInDirectory, /*bRecursive=*/ true);
    AssetsInDirectory.Sort([](const FAssetData& A, const FAssetData& B)
    {
        return A.GetObjectPathString() < B.GetObjectPathString();
    });

    TArray<TSharedPtr<FJsonValue>> AssetsJson;
    TArray<TSharedPtr<FJsonValue>> MovedJson;
    TArray<TSharedPtr<FJsonValue>> FailedAssetJson;
    TSet<FString> SeenReferencerPackages;
    TArray<TSharedPtr<FJsonValue>> FoundJson;
    TArray<TSharedPtr<FJsonValue>> SavedJson;
    TArray<TSharedPtr<FJsonValue>> FailedReferencerJson;
    TArray<TSharedPtr<FJsonValue>> UnstorableJson;
    int32 LoadedScanned = 0;
    bool bRedirectorLeft = false;

    for (const FAssetData& AssetData : AssetsInDirectory)
    {
        // The asset keeps its object name and its position inside the tree: the engine's own directory
        // rename moves the whole tree with its subdirectories, so flattening here would both surprise
        // the caller and let /A/S and /B/S collide on one destination path.
        const FString AssetName = AssetData.AssetName.ToString();
        const FString AssetDirectory = FPackageName::GetLongPackagePath(AssetData.PackageName.ToString());
        FString RelativeSubDirectory;
        if (AssetDirectory == SourceDir)
        {
            RelativeSubDirectory.Reset();
        }
        else if (AssetDirectory.StartsWith(SourceDir + TEXT("/")))
        {
            RelativeSubDirectory = AssetDirectory.RightChop(SourceDir.Len() + 1);
        }
        else
        {
            FailedAssetJson.Add(MakeShareable(new FJsonValueObject(MakeMoveFailure(
                AssetData.GetObjectPathString(), TEXT("outside_source_directory")))));
            continue;
        }
        const FString TargetPath = RelativeSubDirectory.IsEmpty()
            ? FString::Printf(TEXT("%s/%s"), *DestDir, *AssetName)
            : FString::Printf(TEXT("%s/%s/%s"), *DestDir, *RelativeSubDirectory, *AssetName);

        if (ContentPackageExists(TargetPath))
        {
            FailedAssetJson.Add(MakeShareable(new FJsonValueObject(MakeMoveFailure(
                AssetData.GetObjectPathString(), TEXT("destination_exists")))));
            continue;
        }

        UObject* Asset = AssetData.GetAsset();
        if (!Asset)
        {
            FailedAssetJson.Add(MakeShareable(new FJsonValueObject(MakeMoveFailure(
                AssetData.GetObjectPathString(), TEXT("source_load_failed")))));
            continue;
        }

        TSharedPtr<FJsonObject> MoveError;
        TSharedPtr<FJsonObject> AssetResult = MoveOneAsset(Asset, TargetPath, Options, MoveError);
        if (!AssetResult)
        {
            FailedAssetJson.Add(MakeShareable(new FJsonValueObject(MakeMoveFailure(
                AssetData.GetObjectPathString(),
                MoveError.IsValid() ? MoveError->GetStringField(TEXT("error")) : TEXT("rename_failed")))));
            continue;
        }

        AssetsJson.Add(MakeShareable(new FJsonValueObject(AssetResult)));
        MovedJson.Add(MakeShareable(new FJsonValueString(AssetResult->GetStringField(TEXT("destination")))));
        LoadedScanned += static_cast<int32>(AssetResult->GetNumberField(TEXT("loaded_scan_packages")));
        bRedirectorLeft |= AssetResult->GetBoolField(TEXT("redirector_left"));

        // Aggregate the per-asset referencer verdicts once per package: the same referencer can be
        // named by several moved assets, and "saved" must mean it really was written.
        const TArray<TSharedPtr<FJsonValue>>* PerAssetFound = nullptr;
        if (AssetResult->TryGetArrayField(TEXT("referencers_found"), PerAssetFound) && PerAssetFound)
        {
            for (const TSharedPtr<FJsonValue>& Value : *PerAssetFound)
            {
                const TSharedPtr<FJsonObject>* Item = nullptr;
                if (!Value.IsValid() || !Value->TryGetObject(Item) || !Item) { continue; }
                const FString PackageName = (*Item)->GetStringField(TEXT("package"));
                if (SeenReferencerPackages.Contains(PackageName)) { continue; }
                SeenReferencerPackages.Add(PackageName);
                FoundJson.Add(Value);
                if (!(*Item)->GetBoolField(TEXT("loadable")))
                {
                    UnstorableJson.Add(MakeShareable(new FJsonValueObject(MakeMoveFailure(PackageName,
                        (*Item)->GetBoolField(TEXT("map_package")) ? TEXT("map_package") : TEXT("package_missing")))));
                }
            }
        }
        const TArray<TSharedPtr<FJsonValue>>* PerAssetSaved = nullptr;
        if (AssetResult->TryGetArrayField(TEXT("referencers_saved"), PerAssetSaved) && PerAssetSaved)
        {
            for (const TSharedPtr<FJsonValue>& Value : *PerAssetSaved)
            {
                SavedJson.Add(Value);
            }
        }
        const TArray<TSharedPtr<FJsonValue>>* PerAssetFailed = nullptr;
        if (AssetResult->TryGetArrayField(TEXT("referencers_failed"), PerAssetFailed) && PerAssetFailed)
        {
            for (const TSharedPtr<FJsonValue>& Value : *PerAssetFailed)
            {
                FailedReferencerJson.Add(Value);
            }
        }
    }

    // The engine's directory rename takes the folder with it; per-asset moves leave the (now empty)
    // source tree behind, which would then block a later move back into that path. Only remove it when
    // nothing is left there, so a failed item's asset can never be deleted by this cleanup.
    bool bSourceDirectoryRemoved = false;
    if (!Options.bDryRun)
    {
        TArray<FAssetData> RemainingInSource;
        AssetRegistryModule.Get().GetAssetsByPath(FName(*SourceDir), RemainingInSource, /*bRecursive=*/ true);
        if (RemainingInSource.Num() == 0)
        {
            UEditorAssetSubsystem* AssetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
            bSourceDirectoryRemoved = AssetSubsystem && AssetSubsystem->DeleteDirectory(SourceDir);
        }
    }

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("source"), SourceDir);
    ResultJson->SetStringField(TEXT("destination"), DestDir);
    ResultJson->SetBoolField(TEXT("dry_run"), Options.bDryRun);
    ResultJson->SetBoolField(TEXT("update_referencers"), Options.bUpdateReferencers);
    ResultJson->SetBoolField(TEXT("registry_refreshed"), bRefreshed);
    ResultJson->SetNumberField(TEXT("asset_count"), AssetsInDirectory.Num());
    ResultJson->SetNumberField(TEXT("moved_count"), MovedJson.Num());
    ResultJson->SetArrayField(TEXT("moved"), MovedJson);
    ResultJson->SetArrayField(TEXT("failed"), FailedAssetJson);
    ResultJson->SetArrayField(TEXT("referencers_found"), FoundJson);
    ResultJson->SetArrayField(TEXT("referencers_saved"), SavedJson);
    ResultJson->SetArrayField(TEXT("referencers_failed"), FailedReferencerJson);
    ResultJson->SetArrayField(TEXT("unstorable"), UnstorableJson);
    ResultJson->SetBoolField(TEXT("redirector_left"), bRedirectorLeft);
    ResultJson->SetNumberField(TEXT("loaded_scan_packages"), LoadedScanned);
    ResultJson->SetBoolField(TEXT("source_dir_exists"), ContentDirectoryExists(SourceDir));
    ResultJson->SetBoolField(TEXT("destination_dir_exists"), ContentDirectoryExists(DestDir));
    ResultJson->SetBoolField(TEXT("source_dir_removed"), bSourceDirectoryRemoved);
    ResultJson->SetArrayField(TEXT("assets"), AssetsJson);
    // Written out again at the end: some of these referencers are inside the moved directory, so the
    // per-asset snapshot of whether the source directory still exists would be misleading on its own.
    ResultJson->SetStringField(TEXT("note"), Options.bDryRun
        ? TEXT("dry run: nothing was moved, saved or created")
        : TEXT("per-asset results are in assets[]; source_dir_exists is measured after the whole move"));
    return ResultJson;
}

//---------------------------------------------------------------------------------------------
// create_asset_safe / delete_asset_safe
//
// Both existed only in the python tool layer (Content/Python/tools/ue_safe_api.py), where they
// assembled editor-side python and ran it. They carry the modal-dialog protection: AssetTools'
// create_asset pops the rename/replace dialog when the package already exists, and that modal
// blocks the GameThread - which freezes every other bridge call. As python-only tools they were
// unreachable from execute_python_* scripts (a python tool cannot be called from inside the
// editor), so a script that needed a scratch asset had no safe option. Living here, the tool
// surface and the script loopback reach the same implementation.
//---------------------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPAssetCommands::HandleCreateAssetSafe(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetName;
    FString PackagePath;
    FString ClassName;
    if (!Params->TryGetStringField(TEXT("asset_name"), AssetName) ||
        !Params->TryGetStringField(TEXT("package_path"), PackagePath) ||
        !Params->TryGetStringField(TEXT("asset_class"), ClassName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"),
            TEXT("Missing 'asset_name', 'package_path' or 'asset_class' parameter"));
    }

    FString FactoryName;
    Params->TryGetStringField(TEXT("factory"), FactoryName);
    bool bRecreate = false;
    Params->TryGetBoolField(TEXT("recreate"), bRecreate);

    FString CleanPath = PackagePath;
    while (CleanPath.EndsWith(TEXT("/")))
    {
        CleanPath.LeftChopInline(1);
    }
    const FString FullPath = FString::Printf(TEXT("%s/%s"), *CleanPath, *AssetName);

    UClass* AssetClass = nullptr;
    TArray<FString> ClassCandidates;
    if (!FUnrealMCPBlueprintGraphOps::ResolveClass(ClassName, AssetClass, ClassCandidates) || !AssetClass)
    {
        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unknown_class"),
            FString::Printf(TEXT("asset_class '%s' did not resolve to a UClass"), *ClassName));
        FUnrealMCPCommonUtils::AddStringArrayField(Error, TEXT("candidates"), ClassCandidates);
        return Error;
    }

    const FString FactoryKey = FactoryName.IsEmpty()
        ? FString::Printf(TEXT("%sFactoryNew"), *AssetClass->GetName())
        : FactoryName;
    UClass* FactoryClass = nullptr;
    TArray<FString> FactoryCandidates;
    FUnrealMCPBlueprintGraphOps::ResolveClass(FactoryKey, FactoryClass, FactoryCandidates);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("created"), false);
    Data->SetStringField(TEXT("asset_path"), FullPath);
    Data->SetStringField(TEXT("package_path"), CleanPath);
    Data->SetStringField(TEXT("asset_class"), AssetClass->GetName());

    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        if (!bRecreate)
        {
            // Same contract the python tool had: report instead of letting the editor prompt.
            Data->SetStringField(TEXT("reason"), TEXT("asset_exists"));
            return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
        }

        if (UObject* Existing = FUnrealMCPCommonUtils::FindAsset(FullPath))
        {
            if (Existing->GetOutermost() && Existing->GetOutermost()->IsDirty())
            {
                // Silent save: a dirty asset would raise the editor's own "save changes?" dialog.
                UEditorAssetLibrary::SaveLoadedAsset(Existing, /*bOnlyIfIsDirty=*/true);
            }
        }

        TSharedPtr<FJsonObject> DeleteParams = MakeShared<FJsonObject>();
        DeleteParams->SetStringField(TEXT("asset_path"), FullPath);
        DeleteParams->SetBoolField(TEXT("force"), false);
        TSharedPtr<FJsonObject> DeleteResult = HandleSafeDeleteAsset(DeleteParams);
        bool bDeleted = false;
        if (DeleteResult.IsValid())
        {
            DeleteResult->TryGetBoolField(TEXT("deleted"), bDeleted);
        }
        if (!bDeleted)
        {
            // The command's own verdict is the one to trust: it already verified both halves (package
            // file gone, object gone from memory). Re-checking with DoesAssetExist here would read the
            // asset registry, which lags behind a delete that just happened in this session and would
            // report a phantom delete_pending. A refused delete keeps its own reason (e.g.
            // blocked_by_referencers) so the caller sees why nothing was recreated.
            FString DeleteReason;
            const bool bHasReason = DeleteResult.IsValid()
                && DeleteResult->TryGetStringField(TEXT("reason"), DeleteReason)
                && !DeleteReason.IsEmpty();
            Data->SetStringField(TEXT("reason"), bHasReason ? DeleteReason : TEXT("delete_pending"));
            Data->SetObjectField(TEXT("delete_result"), DeleteResult);
            return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
        }
    }

    if (!FactoryClass)
    {
        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("no_factory"),
            FString::Printf(TEXT("factory '%s' did not resolve to a UFactory"), *FactoryKey));
        FUnrealMCPCommonUtils::AddStringArrayField(Error, TEXT("candidates"), FactoryCandidates);
        return Error;
    }

    IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
    UObject* NewAsset = AssetTools.CreateAsset(AssetName, CleanPath, AssetClass, Factory);
    if (!NewAsset)
    {
        Data->SetStringField(TEXT("reason"), TEXT("create_failed"));
        return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
    }

    Data->SetBoolField(TEXT("created"), true);
    Data->SetStringField(TEXT("asset_path"), NewAsset->GetPathName());
    return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
}

TSharedPtr<FJsonObject> FUnrealMCPAssetCommands::HandleDeleteAssetSafe(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"),
            TEXT("Missing 'asset_path' parameter"));
    }

    bool bSaveDirty = true;
    Params->TryGetBoolField(TEXT("save_dirty"), bSaveDirty);
    bool bForce = false;
    Params->TryGetBoolField(TEXT("force"), bForce);

    bool bSaved = false;
    if (bSaveDirty)
    {
        if (UObject* Existing = FUnrealMCPCommonUtils::FindAsset(AssetPath))
        {
            if (Existing->GetOutermost() && Existing->GetOutermost()->IsDirty())
            {
                // The save prompt is what freezes the bridge, so the save happens here instead.
                bSaved = UEditorAssetLibrary::SaveLoadedAsset(Existing, /*bOnlyIfIsDirty=*/true);
            }
        }
    }

    // The delete itself is the blocker-aware, redirector-aware path: a referenced asset comes back
    // as deleted=false with the referencers listed, instead of a modal asking what to do.
    TSharedPtr<FJsonObject> DeleteParams = MakeShared<FJsonObject>();
    DeleteParams->SetStringField(TEXT("asset_path"), AssetPath);
    DeleteParams->SetBoolField(TEXT("force"), bForce);

    TSharedPtr<FJsonObject> Result = HandleSafeDeleteAsset(DeleteParams);
    if (Result.IsValid())
    {
        Result->SetBoolField(TEXT("saved_before_delete"), bSaved);
    }
    return Result;
}

// add_blendable_to_post_volume: the blendables array is read-protected from python, and a changed
// list only renders after the volume's state is refreshed (the enabled bounce), so both halves live
// here next to clear/remove/list_blendables.
TSharedPtr<FJsonObject> FUnrealMCPAssetCommands::HandleAddBlendableToPostVolume(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"),
            TEXT("Missing 'material_path' parameter"));
    }

    FString VolumeName;
    const bool bVolumeGiven = Params->TryGetStringField(TEXT("volume_name"), VolumeName) && !VolumeName.IsEmpty();
    double WeightValue = 1.0;
    Params->TryGetNumberField(TEXT("weight"), WeightValue);

    UObject* Material = StaticLoadObject(UObject::StaticClass(), nullptr, *MaterialPath);
    if (!Material)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("material_not_found"), MaterialPath);
    }

    TArray<AActor*> VolumeActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, APostProcessVolume::StaticClass(), VolumeActors);

    APostProcessVolume* Volume = nullptr;
    if (bVolumeGiven)
    {
        for (AActor* Actor : VolumeActors)
        {
            if (Actor->GetName() == VolumeName)
            {
                Volume = Cast<APostProcessVolume>(Actor);
                break;
            }
        }
        if (!Volume)
        {
            for (AActor* Actor : VolumeActors)
            {
                if (Actor->GetActorLabel() == VolumeName)
                {
                    Volume = Cast<APostProcessVolume>(Actor);
                    break;
                }
            }
        }
        if (!Volume)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("volume_not_found"), VolumeName);
        }
    }
    else
    {
        // No name: prefer the volume whose name/label matches the material's short name (with or
        // without the M_ prefix), then the single volume in the level, and refuse ambiguity.
        const FString ShortName = Material->GetName();
        const FString StrippedName = ShortName.StartsWith(TEXT("M_")) ? ShortName.RightChop(2) : FString();

        TArray<APostProcessVolume*> Matches;
        for (AActor* Actor : VolumeActors)
        {
            APostProcessVolume* Candidate = Cast<APostProcessVolume>(Actor);
            if (!Candidate)
            {
                continue;
            }
            const FString CandidateName = Candidate->GetName();
            const FString CandidateLabel = Candidate->GetActorLabel();
            const bool bMatch = CandidateName == ShortName || CandidateLabel == ShortName
                || (!StrippedName.IsEmpty() && (CandidateName == StrippedName || CandidateLabel == StrippedName));
            if (bMatch)
            {
                Matches.Add(Candidate);
            }
        }

        if (Matches.Num() == 1)
        {
            Volume = Matches[0];
        }
        else if (Matches.Num() == 0 && VolumeActors.Num() == 1)
        {
            Volume = Cast<APostProcessVolume>(VolumeActors[0]);
        }

        if (!Volume)
        {
            TArray<FString> Candidates;
            for (AActor* Actor : VolumeActors)
            {
                Candidates.Add(FString::Printf(TEXT("%s(label=%s)"), *Actor->GetName(), *Actor->GetActorLabel()));
            }
            TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("volume_ambiguous"),
                TEXT("volume_name was omitted and no unique volume matches the material's short name"));
            FUnrealMCPCommonUtils::AddStringArrayField(Error, TEXT("candidates"), Candidates);
            return Error;
        }
    }

    Volume->Modify();
    const int32 Before = Volume->Settings.WeightedBlendables.Array.Num();
    bool bUpdated = false;
    for (FWeightedBlendable& Entry : Volume->Settings.WeightedBlendables.Array)
    {
        if (Entry.Object == Material)
        {
            Entry.Weight = static_cast<float>(WeightValue);
            bUpdated = true;
        }
    }
    if (!bUpdated)
    {
        Volume->Settings.WeightedBlendables.Array.Add(FWeightedBlendable(static_cast<float>(WeightValue), Material));
    }

    RefreshVolumeRenderState(Volume);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("volume_name"), Volume->GetName());
    Data->SetStringField(TEXT("volume_label"), Volume->GetActorLabel());
    Data->SetStringField(TEXT("material_path"), Material->GetPathName());
    Data->SetBoolField(TEXT("added"), !bUpdated);
    Data->SetBoolField(TEXT("updated"), bUpdated);
    Data->SetNumberField(TEXT("weight"), WeightValue);
    Data->SetNumberField(TEXT("blendable_count_before"), Before);
    Data->SetNumberField(TEXT("blendable_count"), Volume->Settings.WeightedBlendables.Array.Num());
    return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
}
