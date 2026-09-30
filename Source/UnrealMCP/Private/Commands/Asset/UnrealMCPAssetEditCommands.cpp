#include "Commands/Asset/UnrealMCPAssetEditCommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Core/MCPCommandRegistry.h"
#include "Reflection/MCPPropertyReflector.h"

#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Factories/FbxAnimSequenceImportData.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxSkeletalMeshImportData.h"
#include "Animation/Skeleton.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Factories/FbxSkeletalMeshImportData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EditorAssetLibrary.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "CoreGlobals.h"
#include "Misc/Guid.h"
#include "Misc/DateTime.h"
#include "Misc/Timespan.h"
#include "MaterialShared.h"
#include "HAL/FileManager.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Animation/Skeleton.h"
#include "ReferenceSkeleton.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "MaterialExpressionIO.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

FUnrealMCPAssetEditCommands::FUnrealMCPAssetEditCommands()
{
}

void FUnrealMCPAssetEditCommands::RegisterCommands(FMCPCommandRegistry& Registry)
{
    MCP_REGISTER_COMMAND(Registry, "import_assets", "asset_edit",
        "Import source files into a content folder through the legacy factory with Interchange gated off. Base entry point: any other asset type, plus mixed batches. Type-specific options live in import_texture / import_skeletal_mesh / import_animation.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("paths"), TEXT("array"), TEXT("Non-empty array of absolute source file paths")),
            MCPParam(TEXT("destination_path"), TEXT("string"), TEXT("Content path starting with '/' to import into")),
            MCPParamOpt(TEXT("force_legacy"), TEXT("bool"), TEXT("Disable Interchange for the requested extensions; default true")),
            MCPParamOpt(TEXT("replace_existing"), TEXT("bool"), TEXT("Overwrite an existing asset of the same name; default false")),
            MCPParamOpt(TEXT("inspect_materials"), TEXT("bool"), TEXT("Post-import read-only material check; adds materials_without_texture[] to the response; default false")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleImportAssets(Params); });

    MCP_REGISTER_COMMAND(Registry, "import_texture", "asset_edit",
        "Import texture source files and apply the requested texture properties in the same call; unset properties keep the importer default.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("paths"), TEXT("array"), TEXT("Non-empty array of absolute source file paths (png/tga/jpg/jpeg/bmp/exr/hdr/psd/dds)")),
            MCPParam(TEXT("destination_path"), TEXT("string"), TEXT("Content path starting with '/' to import into")),
            MCPParamOpt(TEXT("force_legacy"), TEXT("bool"), TEXT("Disable Interchange for the requested extensions; default true")),
            MCPParamOpt(TEXT("replace_existing"), TEXT("bool"), TEXT("Overwrite an existing asset of the same name; default false")),
            MCPParamOpt(TEXT("srgb"), TEXT("bool"), TEXT("sRGB flag; unset = leave the importer default")),
            MCPParamOpt(TEXT("compression"), TEXT("string"), TEXT("Compression settings, e.g. TC_MASKS / TC_NORMALMAP")),
            MCPParamOpt(TEXT("compression_quality"), TEXT("int"), TEXT("Compression quality")),
            MCPParamOpt(TEXT("lod_group"), TEXT("string"), TEXT("Texture group, e.g. TEXTUREGROUP_WORLD")),
            MCPParamOpt(TEXT("no_alpha"), TEXT("bool"), TEXT("compression_no_alpha")),
            MCPParamOpt(TEXT("mip_gen"), TEXT("string"), TEXT("Mip gen settings, e.g. TMGS_NO_MIPMAPS")),
            MCPParamOpt(TEXT("filter"), TEXT("string"), TEXT("Texture filter, e.g. TF_BILINEAR")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleImportTexture(Params); });

    MCP_REGISTER_COMMAND(Registry, "import_skeletal_mesh", "asset_edit",
        "Import skeletal mesh FBX/OBJ with mesh-specific import options (skeleton binding, physics asset, morph targets).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("paths"), TEXT("array"), TEXT("Non-empty array of absolute source file paths (fbx/obj)")),
            MCPParam(TEXT("destination_path"), TEXT("string"), TEXT("Content path starting with '/' to import into")),
            MCPParamOpt(TEXT("force_legacy"), TEXT("bool"), TEXT("Disable Interchange for the requested extensions; default true")),
            MCPParamOpt(TEXT("replace_existing"), TEXT("bool"), TEXT("Overwrite an existing asset of the same name; default false")),
            MCPParamOpt(TEXT("skeleton_path"), TEXT("string"), TEXT("Bind to this Skeleton; omit to create a new one")),
            MCPParamOpt(TEXT("create_physics_asset"), TEXT("bool"), TEXT("Create a PhysicsAsset; unset = leave the importer default")),
            MCPParamOpt(TEXT("physics_asset"), TEXT("string"), TEXT("Bind this PhysicsAsset (implies create_physics_asset=false)")),
            MCPParamOpt(TEXT("import_morph_targets"), TEXT("bool"), TEXT("Import blend shapes as morph targets; default true")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleImportSkeletalMesh(Params); });

    MCP_REGISTER_COMMAND(Registry, "import_animation", "asset_edit",
        "Import animation from FBX onto an existing Skeleton (skeleton_path required); import_mesh=true brings the mesh so morph curves have morph targets to drive.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("paths"), TEXT("array"), TEXT("Non-empty array of absolute source file paths (fbx)")),
            MCPParam(TEXT("destination_path"), TEXT("string"), TEXT("Content path starting with '/' to import into")),
            MCPParam(TEXT("skeleton_path"), TEXT("string"), TEXT("Required: the Skeleton the animation binds to")),
            MCPParamOpt(TEXT("force_legacy"), TEXT("bool"), TEXT("Disable Interchange for the requested extensions; default true")),
            MCPParamOpt(TEXT("replace_existing"), TEXT("bool"), TEXT("Overwrite an existing asset of the same name; default false")),
            MCPParamOpt(TEXT("import_mesh"), TEXT("bool"), TEXT("Import the FBX mesh too (needed for morph curves); default false")),
            MCPParamOpt(TEXT("import_morph_targets"), TEXT("bool"), TEXT("Import blend shapes as morph targets; default true")),
            MCPParamOpt(TEXT("override_animation_name"), TEXT("string"), TEXT("Override the created sequence name")),
            MCPParamOpt(TEXT("frame_rate"), TEXT("int"), TEXT("Custom sample rate; 0/unset = importer default")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleImportAnimation(Params); });

    MCP_REGISTER_COMMAND(Registry, "inspect_skeletal_mesh", "asset_edit",
        "Read-only SkeletalMesh diagnostics: bounds height, root bone, embedded scale (local vs world magnitudes), and per-slot texture presence.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("SkeletalMesh asset path (object or package path)")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleInspectSkeletalMesh(Params); });

    MCP_REGISTER_COMMAND(Registry, "set_asset_properties", "asset_edit",
        "Write asset properties by friendly name with enum tolerance and per-item error reporting.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Asset path (or /Script/Class reference) to edit")),
            MCPParam(TEXT("props"), TEXT("object"), TEXT("Non-empty object of property name -> value")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleSetAssetProperties(Params); });
}

namespace
{
// Extensions UE 5.5 hands to Interchange, mapped to the feature flag suffix.
// Disabling the flag makes the Interchange translator decline the extension
// (CanImportSourceData is a plain extension comparison), so the import falls
// back to the legacy synchronous factory. That fallback matters: Interchange
// drains the GameThread task queue from inside the import and trips the
// TaskGraph RecursionGuard assert when the import runs in the MCP dispatch task.
const TMap<FString, FString>& GetInterchangeExtensionFlags()
{
    static const TMap<FString, FString> Flags = {
        { TEXT("png"),  TEXT("PNG") },
        { TEXT("bmp"),  TEXT("BMP") },
        { TEXT("exr"),  TEXT("EXR") },
        { TEXT("hdr"),  TEXT("HDR") },
        { TEXT("tga"),  TEXT("TGA") },
        { TEXT("tif"),  TEXT("TIFF") },
        { TEXT("tiff"), TEXT("TIFF") },
        { TEXT("jpg"),  TEXT("JPG") },
        { TEXT("jpeg"), TEXT("JPG") },
        { TEXT("psd"),  TEXT("PSD") },
        { TEXT("dds"),  TEXT("DDS") },
        { TEXT("ies"),  TEXT("IES") },
        { TEXT("fbx"),  TEXT("FBX") },
        { TEXT("obj"),  TEXT("OBJ") },
    };
    return Flags;
}

// Disable the Interchange flag for one extension and read it back. The readback
// is the gate: importing without it is what crashes the editor.
bool TryDisableInterchangeForExtension(const FString& Extension,
                                       TSharedPtr<FJsonObject>& OutOverride,
                                       FString& OutError)
{
    const FString* FlagSuffix = GetInterchangeExtensionFlags().Find(Extension);
    if (!FlagSuffix)
    {
        TArray<FString> Supported;
        GetInterchangeExtensionFlags().GetKeys(Supported);
        Supported.Sort();
        OutError = FString::Printf(
            TEXT("unsupported_extension: '.%s' has no Interchange import flag, so force_legacy cannot be honored (supported: %s)"),
            *Extension, *FString::Join(Supported, TEXT(", ")));
        return false;
    }

    const FString CVarName = FString::Printf(TEXT("Interchange.FeatureFlags.Import.%s"), **FlagSuffix);
    IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*CVarName);
    if (!CVar)
    {
        OutError = FString::Printf(TEXT("cvar_override_failed: %s is not registered"), *CVarName);
        return false;
    }

    CVar->Set(TEXT("0"), ECVF_SetByCode);
    const bool bActualEnabled = CVar->GetBool();

    OutOverride = MakeShareable(new FJsonObject);
    OutOverride->SetStringField(TEXT("cvar"), CVarName);
    OutOverride->SetStringField(TEXT("requested"), TEXT("false"));
    OutOverride->SetStringField(TEXT("actual"), bActualEnabled ? TEXT("true") : TEXT("false"));
    OutOverride->SetBoolField(TEXT("verified"), !bActualEnabled);

    if (bActualEnabled)
    {
        OutError = FString::Printf(
            TEXT("cvar_override_failed: %s still reports enabled after being disabled; refusing to import (Interchange would crash the editor)"),
            *CVarName);
        return false;
    }
    return true;
}

// Property names callers actually reach for, mapped to the real engine name.
// "compression" / "no_alpha" / "mip_gen" / "texture_group" are the names that
// failed in practice; the response echoes back the real name.
const TMap<FString, FString>& GetFriendlyPropertyNames()
{
    static const TMap<FString, FString> Map = {
        { TEXT("srgb"),                 TEXT("srgb") },
        { TEXT("compression"),          TEXT("compression_settings") },
        { TEXT("compression_settings"), TEXT("compression_settings") },
        { TEXT("compression_quality"),  TEXT("compression_quality") },
        { TEXT("lod_group"),            TEXT("lod_group") },
        { TEXT("texture_group"),        TEXT("lod_group") },
        { TEXT("no_alpha"),             TEXT("compression_no_alpha") },
        { TEXT("mip_gen"),              TEXT("mip_gen_settings") },
        { TEXT("mip_gen_settings"),     TEXT("mip_gen_settings") },
        { TEXT("filter"),               TEXT("filter") },
    };
    return Map;
}

// Enum member matching that ignores case and underscores, so
// TEXTUREGROUP_WORLD_NORMALMAP and TEXTUREGROUP_WORLD_NORMAL_MAP,
// TMGS_NoMipmaps and TMGS_NO_MIPMAPS all resolve to the same member.
FString NormalizeEnumToken(const FString& In)
{
    FString Out = In;
    Out.ReplaceInline(TEXT("_"), TEXT(""));
    return Out.ToUpper();
}

UEnum* GetPropertyEnum(const FProperty* Property)
{
    if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
    {
        return ByteProperty->GetIntPropertyEnum();
    }
    if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
    {
        return EnumProperty->GetEnum();
    }
    return nullptr;
}

int32 FindEnumIndexByNormalizedName(const UEnum* Enum, const FString& Requested)
{
    if (!Enum)
    {
        return INDEX_NONE;
    }
    const FString Wanted = NormalizeEnumToken(Requested);
    for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
    {
        const FString MemberName = Enum->GetNameStringByIndex(Index);
        if (NormalizeEnumToken(MemberName) == Wanted)
        {
            return Index;
        }
        // Qualified form ("ETextureGroup::TEXTUREGROUP_WORLD") -> compare the tail.
        FString ShortName;
        if (MemberName.Split(TEXT("::"), nullptr, &ShortName, ESearchCase::CaseSensitive, ESearchDir::FromEnd)
            && NormalizeEnumToken(ShortName) == Wanted)
        {
            return Index;
        }
    }
    return INDEX_NONE;
}

// Only properties the editor itself exposes may be written. Reflection can see
// non-editable data (PlatformData, AssetImportData, ...), and python's
// set_editor_property silently writes those, which is exactly the class of
// surprise this command must refuse.
bool IsWritableProperty(const FProperty* Property)
{
    return Property
        && Property->HasAnyPropertyFlags(CPF_Edit)
        && !Property->HasAnyPropertyFlags(CPF_EditConst);
}

// Reflection keeps the C++ spelling (LODGroup, MipGenSettings, CompressionNoAlpha)
// while callers - and python - use the snake_case form (lod_group,
// mip_gen_settings, compression_no_alpha). FName comparison is case-insensitive
// but underscore-SENSITIVE, so FindPropertyByName("lod_group") misses "LODGroup"
// even though python reads exactly that name. Match on the underscore-stripped
// form as the fallback so both spellings resolve to the same property.
FProperty* ResolveAssetProperty(UObject* Asset, const FString& Name)
{
    if (!Asset)
    {
        return nullptr;
    }
    if (FProperty* Exact = Asset->GetClass()->FindPropertyByName(*Name))
    {
        return Exact;
    }
    const FString Wanted = NormalizeEnumToken(Name);
    for (TFieldIterator<FProperty> It(Asset->GetClass()); It; ++It)
    {
        if (NormalizeEnumToken(It->GetName()) == Wanted)
        {
            return *It;
        }
    }
    return nullptr;
}

TSharedPtr<FJsonObject> MakeItemFailure(const FString& Key, const FString& ErrorCode, const FString& Message)
{
    TSharedPtr<FJsonObject> Failure = MakeShareable(new FJsonObject);
    Failure->SetStringField(TEXT("key"), Key);
    Failure->SetStringField(TEXT("error"), ErrorCode);
    Failure->SetStringField(TEXT("message"), Message);
    return Failure;
}

void AddStringArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, const TArray<FString>& Values)
{
    if (!Object.IsValid() || Values.Num() == 0)
    {
        return;
    }
    TArray<TSharedPtr<FJsonValue>> Items;
    for (const FString& Value : Values)
    {
        Items.Add(MakeShareable(new FJsonValueString(Value)));
    }
    Object->SetArrayField(FieldName, Items);
}

// BaseColor 的**直接**输入节点分类：白膜（贴图没接上）在命令面的硬信号。只认直连
// TextureSample 家族；若 BaseColor 走的是数学链，按常量处理并如实报出节点类名。
void DescribeBaseColor(UMaterialInterface* MaterialInterface, FString& OutSourceKind, FString& OutTexturePath)
{
    OutSourceKind = TEXT("None");
    OutTexturePath.Empty();

    UMaterial* Material = MaterialInterface ? MaterialInterface->GetMaterial() : nullptr;
    if (!Material)
    {
        return;
    }
    FExpressionInput* Input = Material->GetExpressionInputForProperty(MP_BaseColor);
    UMaterialExpression* Expression = Input ? Input->Expression : nullptr;
    if (!Expression)
    {
        return;
    }
    if (UMaterialExpressionTextureSample* TextureSample = Cast<UMaterialExpressionTextureSample>(Expression))
    {
        OutSourceKind = TEXT("TextureSample");
        if (TextureSample->Texture)
        {
            OutTexturePath = TextureSample->Texture->GetPathName();
        }
        return;
    }
    OutSourceKind = Expression->GetClass()->GetName();
}

// 覆盖导入的骨架一致性：mesh 的参考姿势与它指向的 Skeleton 资产是否同量级。
// 实测坑：replace_existing 只更新 mesh，Skeleton 沿用首次导入的数据 ⇒ 两者比例差约 100×。
bool SkeletonPoseDiffersFromSkeletonAsset(const USkeletalMesh* Mesh, FString& OutDetail)
{
    const USkeleton* Skeleton = Mesh ? Mesh->GetSkeleton() : nullptr;
    if (!Skeleton)
    {
        return false;
    }

    const FReferenceSkeleton& MeshRef = Mesh->GetRefSkeleton();
    const FReferenceSkeleton& SkelRef = Skeleton->GetReferenceSkeleton();
    if (MeshRef.GetNum() != SkelRef.GetNum())
    {
        OutDetail = FString::Printf(TEXT("bone count differs (mesh %d vs skeleton %d)"), MeshRef.GetNum(), SkelRef.GetNum());
        return true;
    }

    for (int32 Index = 0; Index < MeshRef.GetNum(); ++Index)
    {
        if (MeshRef.GetRefBoneInfo()[Index].Name != SkelRef.GetRefBoneInfo()[Index].Name)
        {
            OutDetail = FString::Printf(TEXT("bone %d name differs (mesh %s vs skeleton %s)"), Index,
                *MeshRef.GetRefBoneInfo()[Index].Name.ToString(), *SkelRef.GetRefBoneInfo()[Index].Name.ToString());
            return true;
        }

        const float MeshLength = MeshRef.GetRefBonePose()[Index].GetTranslation().Size();
        const float SkeletonLength = SkelRef.GetRefBonePose()[Index].GetTranslation().Size();
        if (MeshLength > 1.0f && SkeletonLength > 1.0f)
        {
            const float Ratio = FMath::Max(MeshLength, SkeletonLength) / FMath::Min(MeshLength, SkeletonLength);
            if (Ratio > 20.0f)
            {
                OutDetail = FString::Printf(TEXT("bone '%s' local translation scale differs ~%.0fx (mesh %.2f vs skeleton %.2f)"),
                    *MeshRef.GetRefBoneInfo()[Index].Name.ToString(), Ratio, MeshLength, SkeletonLength);
                return true;
            }
        }
    }
    return false;
}

// 兜底接受的对象必须与源文件**同类**：PNG 不可能产出 Material。不同类说明那是别的资产占了这个
// 名字（典型：同一批里 FBX 导入刚创建的材质），绝不能算成本文件的产物 —— 那就是 §1.7 的假成功。
bool ObjectClassMatchesSource(const UObject* Object, const FString& SourcePath)
{
    if (!Object)
    {
        return false;
    }
    const FString Extension = FPaths::GetExtension(SourcePath).ToLower();
    if (Extension == TEXT("png") || Extension == TEXT("bmp") || Extension == TEXT("exr") || Extension == TEXT("hdr")
        || Extension == TEXT("tga") || Extension == TEXT("tif") || Extension == TEXT("tiff")
        || Extension == TEXT("jpg") || Extension == TEXT("jpeg") || Extension == TEXT("psd") || Extension == TEXT("dds"))
    {
        return Object->IsA<UTexture>();
    }
    if (Extension == TEXT("fbx") || Extension == TEXT("obj"))
    {
        return Object->IsA<UStaticMesh>() || Object->IsA<USkeletalMesh>();
    }
    return true;
}

// One entry of import_assets' results array, always the same shape whether the
// file failed before the import ran or was verified afterwards.
TSharedPtr<FJsonObject> MakeImportFailure(const FString& SourcePath, const FString& Error)
{
    TSharedPtr<FJsonObject> Entry = MakeShareable(new FJsonObject);
    Entry->SetStringField(TEXT("source_path"), SourcePath);
    Entry->SetStringField(TEXT("asset_path"), TEXT(""));
    Entry->SetBoolField(TEXT("imported"), false);
    Entry->SetStringField(TEXT("error"), Error);
    return Entry;
}
} // namespace

/**
 * 一次导入请求：公共参数 + 类型层（扩展名白名单 / 导入期任务选项 / 导入后处理）。
 * `import_assets` 与三条按类型拆分的命令都走同一个核心 `RunImportRequest()`，既有 9 条契约
 * （force_legacy 门禁、模态抑制、逐文件结果、companion 落盘与失败回报、同名不同类型判定…）
 * 因此只有一份实现 —— 拆工具不能拆出三份会漂移的契约副本。
 */
struct FMCPImportRequest
{
    TArray<FString> SourcePaths;
    /** 已归一化（去尾斜杠、以 '/' 开头、目录已确保存在）。 */
    FString DestinationPath;
    bool bForceLegacy = true;
    bool bReplaceExisting = false;
    bool bInspectMaterials = false;
    /** texture / skeletal_mesh / animation；空串 = 未分类（`import_assets`）。 */
    FString AssetType;
    /** 非空时只接受这些扩展名（小写、不含点）；不符的文件项逐项失败，不顺延到别的导入器。 */
    TArray<FString> ExtensionWhitelist;
    /** 类型层：为单个文件的导入任务挂导入期选项；返回非空响应 ⇒ 立即以它结束命令（不导入任何文件）。 */
    TFunction<TSharedPtr<FJsonObject>(UAssetImportTask* Task, const FString& SourcePath)> ConfigureTask;
    /** 类型层：导入后处理（例：贴图属性 applied/failed 回读），可往该文件项里补字段。 */
    TFunction<void(const FString& SourcePath, UObject* Primary, TSharedPtr<FJsonObject>& Entry)> PostProcessEntry;
};

/** 贴图源文件族 / 几何源文件族：类型化命令的扩展名白名单（小写、不含点）。 */
static const TArray<FString>& TextureSourceExtensions()
{
    static const TArray<FString> Extensions = {
        TEXT("png"), TEXT("tga"), TEXT("jpg"), TEXT("jpeg"), TEXT("bmp"),
        TEXT("exr"), TEXT("hdr"), TEXT("psd"), TEXT("dds")};
    return Extensions;
}

static const TArray<FString>& GeometrySourceExtensions()
{
    static const TArray<FString> Extensions = {TEXT("fbx"), TEXT("obj")};
    return Extensions;
}

/**
 * 三条类型化命令与 `import_assets` 共用的公共参数解析。
 * 解析失败（缺 paths / 目标路径非法）时返回 false 并填 OutErrorCode / OutErrorMessage。
 */
static bool ParseCommonImportParams(const TSharedPtr<FJsonObject>& Params, FMCPImportRequest& Request,
    FString& OutErrorCode, FString& OutErrorMessage)
{
    const TArray<TSharedPtr<FJsonValue>>* PathsJson = nullptr;
    if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("paths"), PathsJson)
        || !PathsJson || PathsJson->Num() == 0)
    {
        OutErrorCode = TEXT("missing_paths");
        OutErrorMessage = TEXT("'paths' must be a non-empty array of absolute source file paths");
        return false;
    }
    for (const TSharedPtr<FJsonValue>& Value : *PathsJson)
    {
        if (Value.IsValid() && !Value->AsString().IsEmpty())
        {
            Request.SourcePaths.Add(Value->AsString());
        }
    }
    if (Request.SourcePaths.Num() == 0)
    {
        OutErrorCode = TEXT("missing_paths");
        OutErrorMessage = TEXT("'paths' contained no usable file paths");
        return false;
    }

    FString DestinationPath;
    if (!Params->TryGetStringField(TEXT("destination_path"), DestinationPath))
    {
        OutErrorCode = TEXT("missing_destination_path");
        OutErrorMessage = TEXT("Missing 'destination_path' parameter");
        return false;
    }
    DestinationPath.RemoveFromEnd(TEXT("/"));
    if (!DestinationPath.StartsWith(TEXT("/")))
    {
        OutErrorCode = TEXT("invalid_destination_path");
        OutErrorMessage = FString::Printf(
            TEXT("invalid_destination_path: '%s' must be a content path starting with '/'"), *DestinationPath);
        return false;
    }
    Request.DestinationPath = DestinationPath;

    bool bForceLegacy = true;
    Params->TryGetBoolField(TEXT("force_legacy"), bForceLegacy);
    Request.bForceLegacy = bForceLegacy;

    bool bReplaceExisting = false;
    Params->TryGetBoolField(TEXT("replace_existing"), bReplaceExisting);
    Request.bReplaceExisting = bReplaceExisting;

    bool bInspectMaterials = false;
    Params->TryGetBoolField(TEXT("inspect_materials"), bInspectMaterials);
    Request.bInspectMaterials = bInspectMaterials;

    return true;
}

static TSharedPtr<FJsonObject> RunImportRequest(const FMCPImportRequest& Request)
{
    const TArray<FString>& SourcePaths = Request.SourcePaths;
    const FString DestinationPath = Request.DestinationPath;
    const bool bForceLegacy = Request.bForceLegacy;
    const bool bReplaceExisting = Request.bReplaceExisting;
    const bool bInspectMaterials = Request.bInspectMaterials;

    if (SourcePaths.Num() == 0)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("missing_paths: 'paths' must be a non-empty array of absolute source file paths"));
    }

    // 导入是本插件唯一会撞上引擎模态框的命令（骨树合并询问 "Failed to create Skeleton : Could
    // not merge bone."、覆盖询问都由 FMessageDialog::Open 弹出），而模态是**嵌套 Slate 循环**：
    // 它占住 GameThread，桥的命令队列随之停止派发 —— 整条 MCP 通道会一直死到有人手点为止。
    // 因此把**本命令的执行窗口**标成无人值守，让 FMessageDialog::Open 走「只记日志 / 返回
    // DefaultValue」分支、根本不建窗口。边界必须记清楚：
    //   ① 作用域守卫（TGuardValue<bool>），退出即还原，**不常驻** —— 编辑器是双主体进程，人的
    //      操作不该被算成无人值守；命令窗口内人本来也交互不了（GameThread 被本命令占着）。
    //   ② 被自动回答的询问按引擎默认语义（骨树合并 ⇒ Cancel、覆盖 ⇒ 不覆盖），所以它**只保证
    //      不卡死，不保证导入成功**，失败仍按下面的逐文件判定如实回报（MUST NOT 因"没弹框"报成功）。
    //   ③ 覆盖面：FMessageDialog::Open 全家族与 AddModalWindow(bSlowTaskWindow=false) 都会被取消；
    //      直接调 FPlatformMisc::MessageBoxExt 的 Win32 阻塞路径拦不住（本命令路径不调它）。
    TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true);

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);

    // 1. Legacy fallback gate. Every requested extension must be verifiably out
    //    of Interchange's hands before a single file is touched.
    TArray<TSharedPtr<FJsonValue>> CvarOverrides;
    if (bForceLegacy)
    {
        TSet<FString> HandledExtensions;
        for (const FString& SourcePath : SourcePaths)
        {
            const FString Extension = FPaths::GetExtension(SourcePath).ToLower();
            if (HandledExtensions.Contains(Extension))
            {
                continue;
            }
            HandledExtensions.Add(Extension);

            TSharedPtr<FJsonObject> Override;
            FString OverrideError;
            if (!TryDisableInterchangeForExtension(Extension, Override, OverrideError))
            {
                ResultJson->SetBoolField(TEXT("success"), false);
                ResultJson->SetStringField(TEXT("error"), OverrideError);
                ResultJson->SetNumberField(TEXT("imported_count"), 0);
                ResultJson->SetNumberField(TEXT("failed_count"), SourcePaths.Num());
                ResultJson->SetArrayField(TEXT("cvar_overrides"), CvarOverrides);
                ResultJson->SetArrayField(TEXT("results"), TArray<TSharedPtr<FJsonValue>>());
                return ResultJson;
            }
            CvarOverrides.Add(MakeShareable(new FJsonValueObject(Override)));
        }
    }

    if (!UEditorAssetLibrary::DoesDirectoryExist(DestinationPath))
    {
        UEditorAssetLibrary::MakeDirectory(DestinationPath);
    }

    // 2. Per-file planning. Beyond the old checks this now does two things:
    //    (a) 目标名显式规划成调用内唯一 —— 引擎的覆盖询问因此没有机会出现；
    //    (b) 记下每个文件的计划，供随后的**内部名预检**（沙盒探测导入）使用 ——
    //        legacy FBX 导入器按文件**内部**网格名建资产，只看文件名的预检会漏掉全部内部名冲突。
    struct FPlannedImport
    {
        FString SourcePath;
        FString DestinationName;
        FString AssetPath;
        UAssetImportTask* Task = nullptr;
    };

    TArray<UAssetImportTask*> ImportTasks;
    TArray<TStrongObjectPtr<UAssetImportTask>> TaskOwners;
    TArray<TSharedPtr<FJsonObject>> Results;
    TArray<FPlannedImport> Planned;
    TSet<FString> UsedDestinationNames;

    for (const FString& SourcePath : SourcePaths)
    {
        const FString AssetName = FPaths::GetBaseFilename(SourcePath);

        // 扩展名预检：类型化命令只吃自己那族源文件。不符即**逐项**失败并给出受支持清单，
        // MUST NOT 静默改走别的导入器或别的工具（那会让"看起来成功但类型不对"重现）。
        if (Request.ExtensionWhitelist.Num() > 0)
        {
            const FString Extension = FPaths::GetExtension(SourcePath).ToLower();
            if (!Request.ExtensionWhitelist.Contains(Extension))
            {
                Results.Add(MakeImportFailure(SourcePath, FString::Printf(
                    TEXT("unsupported_extension: '%s' is not one of this command's source types (%s)"),
                    *Extension, *FString::Join(Request.ExtensionWhitelist, TEXT("/")))));
                Planned.Add(FPlannedImport());
                continue;
            }
        }

        if (!FPaths::FileExists(SourcePath))
        {
            Results.Add(MakeImportFailure(SourcePath,
                FString::Printf(TEXT("source_not_found: %s"), *SourcePath)));
            Planned.Add(FPlannedImport());
            continue;
        }

        // 目标名唯一化：同一次调用里的同名源文件不再互相覆盖，覆盖询问也没有机会出现。
        FString DestinationName = AssetName;
        int32 NameSuffix = 1;
        while (UsedDestinationNames.Contains(DestinationName))
        {
            ++NameSuffix;
            DestinationName = FString::Printf(TEXT("%s_%d"), *AssetName, NameSuffix);
        }
        UsedDestinationNames.Add(DestinationName);

        const FString AssetPath = FString::Printf(TEXT("%s/%s"), *DestinationPath, *DestinationName);
        if (!bReplaceExisting && UEditorAssetLibrary::DoesAssetExist(AssetPath))
        {
            Results.Add(MakeImportFailure(SourcePath,
                FString::Printf(TEXT("asset_exists: %s already exists and replace_existing is false"), *AssetPath)));
            Planned.Add(FPlannedImport());
            continue;
        }

        TaskOwners.Emplace(NewObject<UAssetImportTask>());
        UAssetImportTask* Task = TaskOwners.Last().Get();
        Task->Filename = SourcePath;
        Task->DestinationPath = DestinationPath;
        Task->DestinationName = DestinationName;
        Task->bAutomated = true;
        Task->bReplaceExisting = bReplaceExisting;
        Task->bSave = true;

        // 类型层负责挂导入期选项（FBX 的 skeleton / morph / 动画开关等都在请求回调里构建）：
        // 核心不猜类型；回调返回错误响应时原样结束命令（例如骨架路径解析不到 ⇒ skeleton_not_found）。
        if (Request.ConfigureTask)
        {
            if (TSharedPtr<FJsonObject> ConfigureError = Request.ConfigureTask(Task, SourcePath))
            {
                return ConfigureError;
            }
        }

        ImportTasks.Add(Task);

        FPlannedImport Plan;
        Plan.SourcePath = SourcePath;
        Plan.DestinationName = DestinationName;
        Plan.AssetPath = AssetPath;
        Plan.Task = Task;
        Planned.Add(Plan);
        Results.Add(nullptr);
    }

    IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();

    // 不做事前"产出名"探测导入：实测（见 design D2）legacy FBX 导入器把资产名全部从调用方给的
    // DestinationName 派生（X / X_Skeleton / X_PhysicsAsset），因此"产出名冲突"只可能发生在
    // <目标目录>/<基名> 已存在时 —— 那是上面的规划检查已经拦下的情形；而对 316 骨模型探测一次
    // 就是 2 倍导入耗时（实测 65s/次）。真正剩下的假成功风险由下面的"判定"段兜住。

    // 目标目录注册表快照（纯只读查询，不触发 load）。companion 的发现依据必须是"导入前/后快照的
    // 差集" —— 不能只看 task 的 GetObjects()（legacy FBX 只把主网格放进去，Skeleton / PhysicsAsset /
    // 自动材质由工厂另行产出），也不能看"该目录现在有什么"（那会把覆盖导入沿用的既有 Skeleton /
    // PhysicsAsset、以及目录里预先存在的无关资产算成本次产物，并诱发对它们的重复保存）。
    // 非递归：legacy 导入器的 companion 都落在目标目录本身，不必扫子树，也避免把子目录里别人的
    // 资产卷进差集。
    auto SnapshotDestinationAssets = [&DestinationPath]() -> TMap<FString, FAssetData>
    {
        TArray<FAssetData> AssetList;
        IAssetRegistry::GetChecked().GetAssetsByPath(FName(*DestinationPath), AssetList,
            /*bRecursive=*/false, /*bIncludeOnlyOnDiskAssets=*/false);
        TMap<FString, FAssetData> Snapshot;
        for (const FAssetData& Data : AssetList)
        {
            Snapshot.Add(Data.GetObjectPathString(), Data);
        }
        return Snapshot;
    };

    // 4. 正式导入，然后逐文件判定。
    const FDateTime ImportStartUtc = FDateTime::UtcNow();
    const TMap<FString, FAssetData> BeforeAssets = SnapshotDestinationAssets();
    if (ImportTasks.Num() > 0)
    {
        AssetTools.ImportAssetTasks(ImportTasks);
    }
    // 实测：导入刚结束、companion 还没写盘时，注册表里已经含全部条目 —— 差集在这里取得到。
    const TMap<FString, FAssetData> AfterAssets = SnapshotDestinationAssets();

    // 本次导入在目标目录新增的注册表条目（After \ Before）。主网格等 task 产物也在其中，
    // 稍后按 created_assets 剔除，剩下的就是 companion（design D1）。
    TArray<FAssetData> NewRegistryAssets;
    for (const TPair<FString, FAssetData>& Pair : AfterAssets)
    {
        if (!BeforeAssets.Contains(Pair.Key))
        {
            NewRegistryAssets.Add(Pair.Value);
        }
    }

    // 兜底判定用的"这个包是不是本次导入写的"。部分导入器不填 created-objects，
    // 所以需要兜底；但凭 DoesAssetExist 认账会把目录里**预先存在**的异类同名资产
    // 当成本次产物（实测 /Game/MCP/Ganyu/发.png 报 imported:true 而贴图根本不存在）。
    auto WasPackageWrittenSince = [](const UObject* Object, const FDateTime& SinceUtc) -> bool
    {
        const UPackage* Package = Object ? Object->GetOutermost() : nullptr;
        if (!Package)
        {
            return false;
        }
        const FString Filename = FPackageName::LongPackageNameToFilename(
            Package->GetName(), FPackageName::GetAssetPackageExtension());
        const FDateTime Timestamp = IFileManager::Get().GetTimeStamp(*Filename);
        if (Timestamp == FDateTime::MinValue())
        {
            return false;
        }
        return Timestamp >= SinceUtc - FTimespan::FromSeconds(5);
    };

    int32 ImportedCount = 0;
    int32 FailedCount = 0;
    TArray<TSharedPtr<FJsonValue>> ResultsJson;
    TArray<TSharedPtr<FJsonValue>> MaterialsWithoutTexture;
    // 与 ResultsJson 平行：循环后要给每个文件项补 companion_assets[] / save_failures[]，
    // 以及记录每个文件项 task 真正报出的对象（逐产物存盘的落盘范围从这里取）。
    TArray<TSharedPtr<FJsonObject>> FinalEntries;
    TArray<TArray<UObject*>> EntryCreatedObjects;
    EntryCreatedObjects.SetNum(Results.Num());

    for (int32 Index = 0; Index < Results.Num(); ++Index)
    {
        TSharedPtr<FJsonObject> Entry = Results[Index];
        // 本文件项的主产物，供类型层的导入后处理使用（失败项保持 nullptr）。
        UObject* PrimaryForEntry = nullptr;
        if (!Entry.IsValid())
        {
            const FPlannedImport& Plan = Planned[Index];
            UObject* Primary = nullptr;
            TArray<TSharedPtr<FJsonValue>> CreatedJson;
            TArray<UObject*>& CreatedObjects = EntryCreatedObjects[Index];
            if (Plan.Task)
            {
                for (UObject* Created : Plan.Task->GetObjects())
                {
                    if (!Created || Created->HasAnyFlags(RF_Transient))
                    {
                        continue;
                    }
                    CreatedJson.Add(MakeShareable(new FJsonValueString(Created->GetPathName())));
                    CreatedObjects.Add(Created);
                    if (!Primary)
                    {
                        Primary = Created;
                    }
                }
            }

            UObject* PreExisting = nullptr;
            if (!Primary && UEditorAssetLibrary::DoesAssetExist(Plan.AssetPath))
            {
                PreExisting = UEditorAssetLibrary::LoadAsset(Plan.AssetPath);
                if (PreExisting && WasPackageWrittenSince(PreExisting, ImportStartUtc)
                    && ObjectClassMatchesSource(PreExisting, Plan.SourcePath))
                {
                    Primary = PreExisting;
                    PreExisting = nullptr;
                    CreatedJson.Add(MakeShareable(new FJsonValueString(Primary->GetPathName())));
                    CreatedObjects.Add(Primary);
                }
            }

            if (Primary)
            {
                PrimaryForEntry = Primary;
                Entry = MakeShareable(new FJsonObject);
                Entry->SetStringField(TEXT("source_path"), Plan.SourcePath);
                Entry->SetStringField(TEXT("asset_path"), Primary->GetPathName());
                Entry->SetBoolField(TEXT("imported"), true);
                Entry->SetStringField(TEXT("error"), TEXT(""));
                Entry->SetArrayField(TEXT("created_assets"), CreatedJson);

                if (bReplaceExisting)
                {
                    if (const USkeletalMesh* ImportedMesh = Cast<USkeletalMesh>(Primary))
                    {
                        FString MismatchDetail;
                        const bool bMismatch = SkeletonPoseDiffersFromSkeletonAsset(ImportedMesh, MismatchDetail);
                        Entry->SetBoolField(TEXT("skeleton_mismatch"), bMismatch);
                        if (const USkeleton* Skeleton = ImportedMesh->GetSkeleton())
                        {
                            Entry->SetStringField(TEXT("skeleton_path"), Skeleton->GetPathName());
                        }
                        if (bMismatch)
                        {
                            Entry->SetStringField(TEXT("hint"), FString::Printf(
                                TEXT("覆盖导入只更新网格，Skeleton/PhysicsAsset 沿用首次导入的数据（%s）。要真正换骨架需先清理 mesh+skeleton+physics 再导。"),
                                *MismatchDetail));
                        }
                    }
                }

                if (bInspectMaterials)
                {
                    if (const USkeletalMesh* ImportedMesh = Cast<USkeletalMesh>(Primary))
                    {
                        for (const FSkeletalMaterial& Slot : ImportedMesh->GetMaterials())
                        {
                            FString SourceKind;
                            FString TexturePath;
                            DescribeBaseColor(Slot.MaterialInterface, SourceKind, TexturePath);
                            if (SourceKind != TEXT("TextureSample") || TexturePath.IsEmpty())
                            {
                                MaterialsWithoutTexture.Add(MakeShareable(new FJsonValueString(
                                    FString::Printf(TEXT("%s#%s"), *ImportedMesh->GetPathName(), *Slot.MaterialSlotName.ToString()))));
                            }
                        }
                    }
                }
            }
            else if (PreExisting)
            {
                Entry = MakeShareable(new FJsonObject);
                Entry->SetStringField(TEXT("source_path"), Plan.SourcePath);
                Entry->SetStringField(TEXT("asset_path"), TEXT(""));
                Entry->SetBoolField(TEXT("imported"), false);
                Entry->SetStringField(TEXT("error"), FString::Printf(
                    TEXT("name_collision: 目标路径 %s 被一个 %s 占着（可能是目录里预先存在的，也可能是本次调用中别的文件刚创建的），本次导入没有产出任何对象；未把该资产当成本次产物回报"),
                    *Plan.AssetPath, *PreExisting->GetClass()->GetName()));
                Entry->SetStringField(TEXT("existing_asset"), PreExisting->GetPathName());
                Entry->SetStringField(TEXT("existing_class"), PreExisting->GetClass()->GetName());
                Entry->SetStringField(TEXT("hint"), TEXT("换目录或改目标名，或先处理既有资产（safe_delete_asset / rename）后重试"));
            }
            else
            {
                Entry = MakeImportFailure(Plan.SourcePath,
                    FString::Printf(TEXT("import_failed: the import task created no objects, and no asset exists at %s"), *Plan.AssetPath));
            }
        }

        if (Entry->GetBoolField(TEXT("imported")))
        {
            ++ImportedCount;
        }
        else
        {
            ++FailedCount;
        }
        // 类型层的导入后处理（例：贴图属性应用并逐项回读）。它只往该文件项里补字段，
        // MUST NOT 改写 imported 语义 —— "主产物导进来了"与"属性配好了"必须可分别读出。
        if (Request.PostProcessEntry)
        {
            Request.PostProcessEntry(Planned[Index].SourcePath, PrimaryForEntry, Entry);
        }

        FinalEntries.Add(Entry);
        ResultsJson.Add(MakeShareable(new FJsonValueObject(Entry)));
    }

    // ---- companion 发现、归属与完整落盘（design D1 / D3 / D4）----

    // task 已报出的对象路径全集：companion 候选要从整批新增快照差集里扣掉所有文件项实际报出的对象
    // （即 (After \ Before) \ AllCreatedPaths），再按最长前缀归属到具体文件项；匹配不上归本批第一个
    // 文件项。这样只留"没有归属文件"的工厂产物，且多文件批处理下不会串号。
    TSet<FString> AllCreatedPaths;
    for (const TArray<UObject*>& CreatedObjects : EntryCreatedObjects)
    {
        for (UObject* Created : CreatedObjects)
        {
            if (Created)
            {
                AllCreatedPaths.Add(Created->GetPathName());
            }
        }
    }

    struct FCompanion
    {
        FString AssetPath;
        FString ClassName;
        UObject* Asset = nullptr;
        int32 OwnerIndex = INDEX_NONE;
    };

    TArray<FCompanion> Companions;
    for (const FAssetData& Data : NewRegistryAssets)
    {
        const FString ObjectPath = Data.GetObjectPathString();
        if (AllCreatedPaths.Contains(ObjectPath))
        {
            continue;
        }
        FCompanion Item;
        Item.AssetPath = ObjectPath;
        Item.ClassName = Data.AssetClassPath.GetAssetName().ToString();
        // 快照不 load；落到这里的 companion 需要在存盘时拿到 UObject，此处按需解析（本就是刚导入的
        // 内存态资产，解析不产生额外导入）。
        Item.Asset = Data.GetAsset();
        Companions.Add(Item);
    }

    // 归属：先按资产名与 DestinationName 前缀匹配（legacy 导入器的 companion 就是 <基名>_Skeleton /
    // <基名>_PhysicsAsset，前缀能精确落到它自己的文件项上；取最长匹配，避免 Ganyu_UE 抢走
    // Ganyu_UE_2 的 companion）。前缀都对不上的无归属工厂产物（典型：按内部材质名建出的材质）
    // 统一归给"本批第一个跑过导入的文件项" —— 这样多文件批处理里 A 文件的 companion 不会被算到
    // B 文件头上，也没有任何 companion 被丢弃。
    TArray<int32> TaskEntryIndices;
    for (int32 Index = 0; Index < Planned.Num(); ++Index)
    {
        if (Planned[Index].Task)
        {
            TaskEntryIndices.Add(Index);
        }
    }

    for (FCompanion& Companion : Companions)
    {
        const FString AssetName = FPackageName::ObjectPathToObjectName(Companion.AssetPath);
        int32 BestPrefixLength = 0;
        for (const int32 Index : TaskEntryIndices)
        {
            const FString& DestinationName = Planned[Index].DestinationName;
            if (DestinationName.IsEmpty())
            {
                continue;
            }
            if ((AssetName == DestinationName || AssetName.StartsWith(DestinationName + TEXT("_")))
                && DestinationName.Len() > BestPrefixLength)
            {
                BestPrefixLength = DestinationName.Len();
                Companion.OwnerIndex = Index;
            }
        }
    }
    const int32 FallbackOwnerIndex = TaskEntryIndices.Num() > 0 ? TaskEntryIndices[0] : INDEX_NONE;
    for (FCompanion& Companion : Companions)
    {
        if (Companion.OwnerIndex == INDEX_NONE)
        {
            Companion.OwnerIndex = FallbackOwnerIndex;
        }
    }

    // 单产物静默存盘 + 回读（design D3）。存盘口径同 command-effect-verification：以盘上文件是否
    // 存在为准，MUST NOT 用"调用过保存 API"代替结果。逐个产物存盘，绝不做目录级保存 ——
    // SaveDirectory(recursive=true) 会把目标目录里与本次导入无关的既有资产一并写盘（明令禁止）。
    auto SaveAndVerifyAsset = [](UObject* Asset, FString& OutError) -> bool
    {
        if (!Asset)
        {
            OutError = TEXT("asset_unresolved: registry entry has no loadable object");
            return false;
        }
        const FString PackageFile = FPackageName::LongPackageNameToFilename(
            Asset->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
        const bool bSaveReturned = FUnrealMCPCommonUtils::SaveAssetForObject(Asset);
        if (!IFileManager::Get().FileExists(*PackageFile))
        {
            OutError = FString::Printf(TEXT("not_written: no .uasset at %s after save (SavePackages returned %s)"),
                *PackageFile, bSaveReturned ? TEXT("true") : TEXT("false"));
            return false;
        }
        OutError.Reset();
        return true;
    };

    int32 CompanionCount = 0;
    int32 CompanionSaveFailedCount = 0;

    for (int32 Index = 0; Index < FinalEntries.Num(); ++Index)
    {
        const TSharedPtr<FJsonObject>& Entry = FinalEntries[Index];
        if (!Entry.IsValid())
        {
            continue;
        }

        // 落盘范围恰为本次产物集合（created_assets ∪ companion_assets，spec）：created_assets 通常已由
        // task 的 bSave 写过，这里按契约显式补齐；companion 则是本 change 修掉"只在内存态"的那一半。
        for (UObject* Created : EntryCreatedObjects[Index])
        {
            if (Created)
            {
                FString IgnoredError;
                SaveAndVerifyAsset(Created, IgnoredError);
            }
        }

        TArray<TSharedPtr<FJsonValue>> CompanionJson;
        TArray<TSharedPtr<FJsonValue>> SaveFailuresJson;
        for (const FCompanion& Companion : Companions)
        {
            if (Companion.OwnerIndex != Index)
            {
                continue;
            }

            FString SaveError;
            const bool bSaved = SaveAndVerifyAsset(Companion.Asset, SaveError);

            TSharedPtr<FJsonObject> Item = MakeShareable(new FJsonObject);
            Item->SetStringField(TEXT("asset_path"), Companion.AssetPath);
            Item->SetStringField(TEXT("class"), Companion.ClassName);
            Item->SetBoolField(TEXT("saved"), bSaved);
            CompanionJson.Add(MakeShareable(new FJsonValueObject(Item)));
            ++CompanionCount;

            // companion 落盘失败不改写主产物 imported（网格确实导进来了），只加结构化失败信号，
            // 让调用方在"无失败信号"之外还有可分支的读法（design D4）。
            if (!bSaved)
            {
                TSharedPtr<FJsonObject> Failure = MakeShareable(new FJsonObject);
                Failure->SetStringField(TEXT("asset_path"), Companion.AssetPath);
                Failure->SetStringField(TEXT("error"), SaveError);
                SaveFailuresJson.Add(MakeShareable(new FJsonValueObject(Failure)));
                ++CompanionSaveFailedCount;
            }
        }

        // 每个文件项都带这两个字段（含失败项），结果形状统一；全成功时 save_failures 为空数组。
        Entry->SetArrayField(TEXT("companion_assets"), CompanionJson);
        Entry->SetArrayField(TEXT("save_failures"), SaveFailuresJson);
    }

    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("destination_path"), DestinationPath);
    ResultJson->SetStringField(TEXT("importer"), bForceLegacy ? TEXT("legacy") : TEXT("interchange"));
    // 类型化命令的类型标识（`import_assets` 不带该字段），以及该命令接受的源文件扩展名。
    if (!Request.AssetType.IsEmpty())
    {
        ResultJson->SetStringField(TEXT("asset_type"), Request.AssetType);
    }
    if (Request.ExtensionWhitelist.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> SupportedJson;
        for (const FString& Extension : Request.ExtensionWhitelist)
        {
            SupportedJson.Add(MakeShareable(new FJsonValueString(Extension)));
        }
        ResultJson->SetArrayField(TEXT("supported_extensions"), SupportedJson);
    }
    ResultJson->SetNumberField(TEXT("imported_count"), ImportedCount);
    ResultJson->SetNumberField(TEXT("failed_count"), FailedCount);
    ResultJson->SetArrayField(TEXT("results"), ResultsJson);
    // 批级结论，调用方无需逐项汇总。companion_save_failed_count > 0 ⇒ 有附带产物没落盘。
    ResultJson->SetNumberField(TEXT("companion_count"), CompanionCount);
    ResultJson->SetNumberField(TEXT("companion_save_failed_count"), CompanionSaveFailedCount);
    ResultJson->SetArrayField(TEXT("cvar_overrides"), CvarOverrides);
    if (bInspectMaterials)
    {
        ResultJson->SetArrayField(TEXT("materials_without_texture"), MaterialsWithoutTexture);
    }
    return ResultJson;
}

/**
 * FBX 导入期选项（类型层构建，核心不猜类型）。
 * morph MUST 显式打开：`bImportMorphTargets` 是 config 属性、引擎基础 ini 里就是 False
 * （BaseEditorPerProjectUserSettings.ini:676 的 [/Script/UnrealEd.FbxSkeletalMeshImportData]）；
 * 不设 `Task->Options` 时工厂用自己那份默认 UI（FbxFactory.cpp:68-69），自动化导入永远把 morph 关着。
 * 工厂会采纳 `Task->Options`（FbxFactory.cpp:346 Cast<UFbxImportUI> → :354），而 UFbxImportUI 的构造函数
 * 已把四个子对象 LoadOptions 过（FbxFactory.cpp:1252-1262），所以这里**不能**再补工厂那句静态
 * `UFbxImportUI::LoadOptions()` —— 它没有 UNREALED_API，跨模块会 LNK2019。
 */
struct FMCPFbxOptions
{
    /** morph 曲线 / 表情的落点：网格与动画两条路都要它。 */
    bool bImportMorphTargets = true;
    /** 非空 ⇒ 绑定既有骨架（纯动画导入必须给，否则导入器一个对象都不建）。 */
    FString SkeletonPath;
    /** 纯动画导入时是否连带网格（网格的 morph target 存在，动画的 morph 曲线才有落点）。 */
    bool bImportMesh = false;
    bool bImportAnimations = false;
    FString OverrideAnimationName;
    /** >0 ⇒ 关闭默认采样率并指定它（`bUseDefaultSampleRate=false` + `CustomSampleRate`）。 */
    int32 CustomSampleRate = 0;
    /** 未设置 = 不覆盖引擎默认。 */
    TOptional<bool> bCreatePhysicsAsset;
    FString PhysicsAssetPath;
};

/** 返回非空 ⇒ 该错误响应直接结束命令（不导入任何文件）；返回 nullptr = 已挂好选项。 */
static TSharedPtr<FJsonObject> ApplyFbxImportOptions(UAssetImportTask* Task, const FString& SourcePath,
    const FMCPFbxOptions& Options)
{
    if (!FPaths::GetExtension(SourcePath).Equals(TEXT("fbx"), ESearchCase::IgnoreCase))
    {
        return nullptr;
    }

    UFbxImportUI* ImportUI = NewObject<UFbxImportUI>(Task, NAME_None, RF_NoFlags);
    ImportUI->SkeletalMeshImportData->bImportMorphTargets = Options.bImportMorphTargets;
    if (Options.CustomSampleRate > 0)
    {
        ImportUI->AnimSequenceImportData->bUseDefaultSampleRate = false;
        ImportUI->AnimSequenceImportData->CustomSampleRate = Options.CustomSampleRate;
    }

    if (!Options.SkeletonPath.IsEmpty())
    {
        USkeleton* TargetSkeleton = Cast<USkeleton>(FUnrealMCPCommonUtils::FindAsset(Options.SkeletonPath));
        if (!TargetSkeleton)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("skeleton_not_found"),
                FString::Printf(TEXT("no Skeleton at '%s'"), *Options.SkeletonPath));
        }
        // 类型自己定：自动化导入默认会按内容探测类型并回填进我们这份 UI（FbxFactory.cpp:349-352）。
        ImportUI->bAutomatedImportShouldDetectType = false;
        // 用"网格模式（可只导动画）"：morph 曲线只有在有网格的路径上才会被解析 —— 纯动画模式
        // （FBXIT_Animation）不建网格，FbxFactory.cpp:822 的 morph 分支拿不到 BaseSkeletalMesh 就整块跳过。
        ImportUI->MeshTypeToImport = EFBXImportType::FBXIT_SkeletalMesh;
        ImportUI->bImportMesh = Options.bImportMesh;
        ImportUI->bImportAnimations = Options.bImportAnimations;
        ImportUI->Skeleton = TargetSkeleton;
        if (!Options.OverrideAnimationName.IsEmpty())
        {
            ImportUI->OverrideAnimationName = Options.OverrideAnimationName;
        }
    }

    if (Options.bCreatePhysicsAsset.IsSet())
    {
        ImportUI->bCreatePhysicsAsset = Options.bCreatePhysicsAsset.GetValue();
    }
    if (!Options.PhysicsAssetPath.IsEmpty())
    {
        UPhysicsAsset* PhysicsAsset = Cast<UPhysicsAsset>(FUnrealMCPCommonUtils::FindAsset(Options.PhysicsAssetPath));
        if (!PhysicsAsset)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("physics_asset_not_found"),
                FString::Printf(TEXT("no PhysicsAsset at '%s'"), *Options.PhysicsAssetPath));
        }
        ImportUI->bCreatePhysicsAsset = false;
        ImportUI->PhysicsAsset = PhysicsAsset;
    }

    Task->Options = ImportUI;
    return nullptr;
}

/** 把参数里出现的贴图属性键原样收进 props（未出现的键不发，故不会被写）。 */
static TSharedPtr<FJsonObject> CollectTextureProps(const TSharedPtr<FJsonObject>& Params)
{
    static const TCHAR* const Keys[] = {
        TEXT("srgb"), TEXT("compression"), TEXT("compression_quality"), TEXT("lod_group"),
        TEXT("no_alpha"), TEXT("mip_gen"), TEXT("filter")};

    TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
    for (const TCHAR* Key : Keys)
    {
        const TSharedPtr<FJsonValue>* Value = Params->Values.Find(Key);
        if (Value && Value->IsValid())
        {
            Props->SetField(Key, *Value);
        }
    }
    return Props;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetEditCommands::HandleImportAssets(const TSharedPtr<FJsonObject>& Params)
{
    FMCPImportRequest Request;
    FString ErrorCode;
    FString ErrorMessage;
    if (!ParseCommonImportParams(Params, Request, ErrorCode, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(*ErrorCode, ErrorMessage);
    }

    // 类型专属参数已迁到 import_animation。传了就**结构化拒绝**、不导入任何文件 ——
    // MUST NOT 静默忽略：静默会让"响应成功但动画/morph 没进来"这一既知坑重现
    // （纯动画 FBX 缺骨架时导入器一个对象都不建）。
    for (const TCHAR* MovedParam : {TEXT("skeleton_path"), TEXT("import_mesh")})
    {
        if (Params->HasField(MovedParam))
        {
            TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("unsupported_parameter"),
                FString::Printf(TEXT("'%s' is not an import_assets parameter"), MovedParam));
            Error->SetStringField(TEXT("parameter"), MovedParam);
            Error->SetStringField(TEXT("moved_to"), TEXT("import_animation"));
            Error->SetStringField(TEXT("hint"),
                TEXT("改用 import_animation(skeleton_path=…, import_mesh=…)；import_assets 只负责基础能力（其它类型 / 混合批处理）"));
            Error->SetNumberField(TEXT("imported_count"), 0);
            return Error;
        }
    }

    return RunImportRequest(Request);
}

TSharedPtr<FJsonObject> FUnrealMCPAssetEditCommands::HandleImportTexture(const TSharedPtr<FJsonObject>& Params)
{
    FMCPImportRequest Request;
    FString ErrorCode;
    FString ErrorMessage;
    if (!ParseCommonImportParams(Params, Request, ErrorCode, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(*ErrorCode, ErrorMessage);
    }

    Request.AssetType = TEXT("texture");
    Request.ExtensionWhitelist = TextureSourceExtensions();

    const TSharedPtr<FJsonObject> Props = CollectTextureProps(Params);
    if (Props->Values.Num() > 0)
    {
        // 属性写入复用 `set_asset_properties` 的**同一套内核**（友好名 + 忽略大小写与下划线的枚举容错 +
        // 逐项 before→after 回读）：直接调本类的处理器，避免出现第二份名字解析。
        // 未传的键不进 Props ⇒ 不会被写（否则"只设置该资产所需的参数"就变成覆盖别人的默认值）。
        Request.PostProcessEntry = [this, Props](const FString& SourcePath, UObject* Primary,
            TSharedPtr<FJsonObject>& Entry)
        {
            TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject);
            if (!Primary)
            {
                Properties->SetBoolField(TEXT("applied_ok"), false);
                Properties->SetStringField(TEXT("reason"), TEXT("no primary asset to configure"));
                Entry->SetObjectField(TEXT("properties"), Properties);
                return;
            }

            TSharedPtr<FJsonObject> PropertyParams = MakeShareable(new FJsonObject);
            PropertyParams->SetStringField(TEXT("asset_path"), Primary->GetPathName());
            PropertyParams->SetObjectField(TEXT("props"), Props);
            const TSharedPtr<FJsonObject> Applied = HandleSetAssetProperties(PropertyParams);

            if (!Applied.IsValid() || !Applied->HasField(TEXT("applied")))
            {
                Properties->SetBoolField(TEXT("applied_ok"), false);
                Properties->SetStringField(TEXT("error"), Applied.IsValid()
                    ? Applied->GetStringField(TEXT("error")) : TEXT("property_write_failed"));
                Entry->SetObjectField(TEXT("properties"), Properties);
                return;
            }

            Properties->SetBoolField(TEXT("applied_ok"), true);
            for (const TCHAR* Key : {TEXT("applied"), TEXT("failed"), TEXT("applied_count"), TEXT("failed_count")})
            {
                const TSharedPtr<FJsonValue>* Field = Applied->Values.Find(Key);
                if (Field && Field->IsValid())
                {
                    Properties->SetField(Key, *Field);
                }
            }
            Entry->SetObjectField(TEXT("properties"), Properties);
        };
    }

    return RunImportRequest(Request);
}

TSharedPtr<FJsonObject> FUnrealMCPAssetEditCommands::HandleImportSkeletalMesh(const TSharedPtr<FJsonObject>& Params)
{
    FMCPImportRequest Request;
    FString ErrorCode;
    FString ErrorMessage;
    if (!ParseCommonImportParams(Params, Request, ErrorCode, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(*ErrorCode, ErrorMessage);
    }

    Request.AssetType = TEXT("skeletal_mesh");
    Request.ExtensionWhitelist = GeometrySourceExtensions();

    FMCPFbxOptions Options;
    Options.bImportMesh = true;
    Options.bImportAnimations = false;
    bool bImportMorphTargets = true;
    Params->TryGetBoolField(TEXT("import_morph_targets"), bImportMorphTargets);
    Options.bImportMorphTargets = bImportMorphTargets;
    Params->TryGetStringField(TEXT("skeleton_path"), Options.SkeletonPath);
    Params->TryGetStringField(TEXT("physics_asset"), Options.PhysicsAssetPath);
    bool bCreatePhysicsAsset = false;
    if (Params->TryGetBoolField(TEXT("create_physics_asset"), bCreatePhysicsAsset))
    {
        Options.bCreatePhysicsAsset = bCreatePhysicsAsset;
    }

    Request.ConfigureTask = [Options](UAssetImportTask* Task, const FString& SourcePath)
    {
        return ApplyFbxImportOptions(Task, SourcePath, Options);
    };

    return RunImportRequest(Request);
}

TSharedPtr<FJsonObject> FUnrealMCPAssetEditCommands::HandleImportAnimation(const TSharedPtr<FJsonObject>& Params)
{
    FMCPImportRequest Request;
    FString ErrorCode;
    FString ErrorMessage;
    if (!ParseCommonImportParams(Params, Request, ErrorCode, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(*ErrorCode, ErrorMessage);
    }

    FString SkeletonPath;
    Params->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);
    if (SkeletonPath.IsEmpty())
    {
        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_skeleton"),
            TEXT("'skeleton_path' is required: without a target Skeleton the importer creates no objects at all"));
        Error->SetStringField(TEXT("hint"),
            TEXT("先用 import_skeletal_mesh 拿到骨架，或传既有骨架路径；纯动画 FBX 没有骨架一个对象都不建"));
        Error->SetNumberField(TEXT("imported_count"), 0);
        return Error;
    }

    Request.AssetType = TEXT("animation");
    Request.ExtensionWhitelist = GeometrySourceExtensions();

    FMCPFbxOptions Options;
    Options.SkeletonPath = SkeletonPath;
    Options.bImportAnimations = true;
    bool bImportMesh = false;
    Params->TryGetBoolField(TEXT("import_mesh"), bImportMesh);
    Options.bImportMesh = bImportMesh;
    // morph 曲线是动画的一部分：默认开着（带网格时才解析得到）。
    bool bImportMorphTargets = true;
    Params->TryGetBoolField(TEXT("import_morph_targets"), bImportMorphTargets);
    Options.bImportMorphTargets = bImportMorphTargets;
    Params->TryGetStringField(TEXT("override_animation_name"), Options.OverrideAnimationName);
    int32 FrameRate = 0;
    Params->TryGetNumberField(TEXT("frame_rate"), FrameRate);
    Options.CustomSampleRate = FrameRate;

    Request.ConfigureTask = [Options](UAssetImportTask* Task, const FString& SourcePath)
    {
        return ApplyFbxImportOptions(Task, SourcePath, Options);
    };

    return RunImportRequest(Request);
}

TSharedPtr<FJsonObject> FUnrealMCPAssetEditCommands::HandleInspectSkeletalMesh(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("missing_asset_path: 'asset_path' must be a non-empty SkeletalMesh asset path"));
    }

    UObject* Asset = FUnrealMCPCommonUtils::FindAsset(AssetPath);
    USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset);
    if (!Mesh)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("not_a_skeletal_mesh"),
            FString::Printf(TEXT("'%s' did not resolve to a SkeletalMesh (%s)"), *AssetPath,
                Asset ? *FString::Printf(TEXT("resolved to %s"), *Asset->GetClass()->GetName()) : TEXT("not found")));
    }

    auto VectorToJson = [](const FVector& Value)
    {
        TArray<TSharedPtr<FJsonValue>> Items;
        Items.Add(MakeShareable(new FJsonValueNumber(Value.X)));
        Items.Add(MakeShareable(new FJsonValueNumber(Value.Y)));
        Items.Add(MakeShareable(new FJsonValueNumber(Value.Z)));
        return Items;
    };

    const FReferenceSkeleton& RefSkeleton = Mesh->GetRefSkeleton();
    const TArray<FTransform>& LocalPose = RefSkeleton.GetRefBonePose();
    const TArray<FMeshBoneInfo>& BoneInfo = RefSkeleton.GetRefBoneInfo();
    const int32 BoneCount = LocalPose.Num();

    // 世界（component space）姿势 = 沿父链累乘。参考骨架的索引本身是拓扑序（父 < 子），
    // 一次前向遍历即可，且全程只用公开访问器，不碰指针偏移。
    TArray<FTransform> WorldPose;
    WorldPose.SetNum(BoneCount);
    for (int32 Index = 0; Index < BoneCount; ++Index)
    {
        const int32 ParentIndex = RefSkeleton.GetParentIndex(Index);
        WorldPose[Index] = (ParentIndex >= 0 && ParentIndex < Index)
            ? LocalPose[Index] * WorldPose[ParentIndex]
            : LocalPose[Index];
    }

    // 探测骨：跳过根与末端，在索引约 1/3、1/2、2/3 处挑"有子节点"的中段骨。
    TSet<int32> ParentIndices;
    for (int32 Index = 0; Index < BoneCount; ++Index)
    {
        const int32 ParentIndex = RefSkeleton.GetParentIndex(Index);
        if (ParentIndex >= 0)
        {
            ParentIndices.Add(ParentIndex);
        }
    }

    TArray<int32> ProbeIndices;
    if (BoneCount > 2)
    {
        for (const float Fraction : {0.33f, 0.5f, 0.66f})
        {
            const int32 Candidate = FMath::Clamp(FMath::RoundToInt(BoneCount * Fraction), 1, BoneCount - 1);
            // 候选下标本身可能是叶骨（MMD 的 D 骨 / 足先EX 就是），而叶骨在本判据里没有意义。
            // 从候选出发向两侧找最近的一根"有子节点"的骨。
            int32 Picked = INDEX_NONE;
            for (int32 Offset = 0; Offset < BoneCount && Picked == INDEX_NONE; ++Offset)
            {
                const int32 Forward = Candidate + Offset;
                const int32 Backward = Candidate - Offset;
                if (Forward < BoneCount && ParentIndices.Contains(Forward))
                {
                    Picked = Forward;
                }
                else if (Backward >= 1 && ParentIndices.Contains(Backward))
                {
                    Picked = Backward;
                }
            }
            if (Picked != INDEX_NONE && !ProbeIndices.Contains(Picked))
            {
                ProbeIndices.Add(Picked);
            }
        }
    }

    float MaxLocalLength = 0.0f;
    float MaxWorldLength = 0.0f;
    bool bHasEmbeddedScale = false;
    TArray<TSharedPtr<FJsonValue>> ProbeBonesJson;

    for (const int32 Index : ProbeIndices)
    {
        const FVector LocalTranslation = LocalPose[Index].GetTranslation();
        const FVector WorldTranslation = WorldPose[Index].GetTranslation();
        const float LocalLength = LocalTranslation.Size();
        const float WorldLength = WorldTranslation.Size();
        MaxLocalLength = FMath::Max(MaxLocalLength, LocalLength);
        MaxWorldLength = FMath::Max(MaxWorldLength, WorldLength);

        // 判据：任一中段骨 |LOCAL| < 1 且 |WORLD| > 10 ⇒ 骨架内嵌了 ×100 scale。
        if (LocalLength < 1.0f && WorldLength > 10.0f)
        {
            bHasEmbeddedScale = true;
        }

        TSharedPtr<FJsonObject> ProbeBone = MakeShareable(new FJsonObject);
        ProbeBone->SetStringField(TEXT("name"), BoneInfo[Index].Name.ToString());
        ProbeBone->SetArrayField(TEXT("local"), VectorToJson(LocalTranslation));
        ProbeBone->SetArrayField(TEXT("world"), VectorToJson(WorldTranslation));
        ProbeBonesJson.Add(MakeShareable(new FJsonValueObject(ProbeBone)));
    }

    // local 比 world 小约两个数量级 ⇒ 数据是米制、由引擎做了 m→cm 转换。
    const FString BoneLocalUnit = (MaxLocalLength > SMALL_NUMBER && MaxWorldLength > MaxLocalLength * 20.0f)
        ? TEXT("meters")
        : TEXT("cm");

    TSharedPtr<FJsonObject> RootBone = MakeShareable(new FJsonObject);
    if (BoneCount > 0)
    {
        RootBone->SetStringField(TEXT("name"), BoneInfo[0].Name.ToString());
        RootBone->SetArrayField(TEXT("scale"), VectorToJson(LocalPose[0].GetScale3D()));
        RootBone->SetArrayField(TEXT("translation"), VectorToJson(LocalPose[0].GetTranslation()));
    }

    const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
    TArray<TSharedPtr<FJsonValue>> SlotsWithoutTexture;
    for (const FSkeletalMaterial& Slot : Materials)
    {
        FString SourceKind;
        FString TexturePath;
        DescribeBaseColor(Slot.MaterialInterface, SourceKind, TexturePath);
        if (SourceKind != TEXT("TextureSample") || TexturePath.IsEmpty())
        {
            SlotsWithoutTexture.Add(MakeShareable(new FJsonValueString(
                Slot.MaterialSlotName.IsNone() ? FString(TEXT("<unnamed>")) : Slot.MaterialSlotName.ToString())));
        }
    }

    const double HeightCm = FMath::RoundToDouble(static_cast<double>(Mesh->GetBounds().BoxExtent.Z) * 2.0 * 100.0) / 100.0;

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("asset_path"), Mesh->GetPathName());
    ResultJson->SetNumberField(TEXT("height_cm"), HeightCm);
    ResultJson->SetNumberField(TEXT("bone_count"), BoneCount);
    ResultJson->SetObjectField(TEXT("root_bone"), RootBone);
    ResultJson->SetBoolField(TEXT("has_embedded_scale"), bHasEmbeddedScale);
    ResultJson->SetStringField(TEXT("bone_local_unit"), BoneLocalUnit);
    ResultJson->SetArrayField(TEXT("probe_bones"), ProbeBonesJson);
    ResultJson->SetBoolField(TEXT("skeleton_consistent"), !bHasEmbeddedScale);
    ResultJson->SetArrayField(TEXT("slots_without_texture"), SlotsWithoutTexture);
    ResultJson->SetNumberField(TEXT("materials_count"), Materials.Num());
    return ResultJson;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetEditCommands::HandleSetAssetProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' parameter"));
    }

    const TSharedPtr<FJsonObject>* PropsObject = nullptr;
    if (!Params->TryGetObjectField(TEXT("props"), PropsObject) || !PropsObject->IsValid())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("props_missing: 'props' must be an object of property name -> value"));
    }
    if ((*PropsObject)->Values.Num() == 0)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("props_missing: 'props' must not be empty"));
    }

    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetPath);
    if (!Asset)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("asset_not_found: %s"), *AssetPath));
    }

    Asset->Modify();

    TArray<TSharedPtr<FJsonValue>> AppliedJson;
    TArray<TSharedPtr<FJsonValue>> FailedJson;

    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PropsObject)->Values)
    {
        const FString& Key = Pair.Key;
        const TSharedPtr<FJsonValue>& RequestedValue = Pair.Value;

        const FString* MappedName = GetFriendlyPropertyNames().Find(Key);
        const FString PropertyName = MappedName ? *MappedName : Key;

        FProperty* Property = ResolveAssetProperty(Asset, PropertyName);
        if (!Property)
        {
            FailedJson.Add(MakeShareable(new FJsonValueObject(
                MakeItemFailure(Key, TEXT("unknown_property"),
                    FString::Printf(TEXT("no property '%s' (from key '%s') on %s; computed attributes such as has_alpha_channel are not reflected properties"),
                        *PropertyName, *Key, *Asset->GetClass()->GetName())))));
            continue;
        }
        if (!IsWritableProperty(Property))
        {
            FailedJson.Add(MakeShareable(new FJsonValueObject(
                MakeItemFailure(Key, TEXT("property_not_writable"),
                    FString::Printf(TEXT("'%s' is not editable (%s); it is engine-managed data"),
                        *Property->GetName(), *Property->GetClass()->GetName())))));
            continue;
        }

        // Enum members match case- and underscore-insensitively and are rewritten
        // to the canonical member name so the shared writer resolves them.
        TSharedPtr<FJsonValue> ValueToWrite = RequestedValue;
        if (const UEnum* Enum = GetPropertyEnum(Property))
        {
            if (ValueToWrite.IsValid() && ValueToWrite->Type == EJson::String)
            {
                const int32 EnumIndex = FindEnumIndexByNormalizedName(Enum, ValueToWrite->AsString());
                if (EnumIndex == INDEX_NONE)
                {
                    TArray<TSharedPtr<FJsonValue>> CandidatesJson;
                    for (int32 CandidateIndex = 0; CandidateIndex < Enum->NumEnums(); ++CandidateIndex)
                    {
                        CandidatesJson.Add(MakeShareable(new FJsonValueString(Enum->GetNameStringByIndex(CandidateIndex))));
                    }
                    TSharedPtr<FJsonObject> Failure = MakeItemFailure(Key, TEXT("unknown_enum_member"),
                        FString::Printf(TEXT("'%s' is not a member of %s"), *ValueToWrite->AsString(), *Enum->GetName()));
                    Failure->SetArrayField(TEXT("candidates"), CandidatesJson);
                    FailedJson.Add(MakeShareable(new FJsonValueObject(Failure)));
                    continue;
                }
                ValueToWrite = MakeShareable(new FJsonValueString(Enum->GetNameStringByIndex(EnumIndex)));
            }
        }

        void* PropertyAddr = Property->ContainerPtrToValuePtr<void>(Asset);
        const TSharedPtr<FJsonValue> ValueBefore = FUnrealMCPCommonUtils::PropertyValueToJson(Property, PropertyAddr);
        FString WriteError;
        FString WriteErrorCode;
        TArray<FString> AvailableFields;
        FWriteResult WriteResult;
        if (!FUnrealMCPCommonUtils::SetObjectProperty(Asset, Property->GetName(), ValueToWrite, WriteError,
                &AvailableFields, &WriteErrorCode, &WriteResult))
        {
            TSharedPtr<FJsonObject> Failure = MakeItemFailure(Key,
                WriteErrorCode.IsEmpty() ? TEXT("write_failed") : WriteErrorCode, WriteError);
            AddStringArray(Failure, TEXT("candidates"), WriteResult.Candidates);
            AddStringArray(Failure, TEXT("available_fields"), AvailableFields);
            AddStringArray(Failure, TEXT("supported_shapes"), WriteResult.SupportedShapes);
            if (WriteResult.FailedIndex != INDEX_NONE)
            {
                Failure->SetNumberField(TEXT("failed_index"), WriteResult.FailedIndex);
            }
            Failure->SetBoolField(TEXT("unchanged"), WriteResult.bUnchanged);
            FailedJson.Add(MakeShareable(new FJsonValueObject(Failure)));
            continue;
        }
        FPropertyChangedEvent ChangeEvent(Property);
        Asset->PostEditChangeProperty(ChangeEvent);

        TSharedPtr<FJsonObject> Applied = MakeShareable(new FJsonObject);
        Applied->SetStringField(TEXT("key"), Key);
        // The reflected name, which may differ from the key in underscores
        // (key "lod_group" -> property "LODGroup").
        Applied->SetStringField(TEXT("property"), Property->GetName());
        if (ValueBefore.IsValid())
        {
            Applied->SetField(TEXT("value_before"), ValueBefore);
        }
        const TSharedPtr<FJsonValue> ValueAfter = FUnrealMCPCommonUtils::PropertyValueToJson(Property, PropertyAddr);
        if (ValueAfter.IsValid())
        {
            Applied->SetField(TEXT("value_after"), ValueAfter);
        }
        AppliedJson.Add(MakeShareable(new FJsonValueObject(Applied)));
    }

    Asset->MarkPackageDirty();

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("asset_path"), Asset->GetPathName());
    ResultJson->SetArrayField(TEXT("applied"), AppliedJson);
    ResultJson->SetArrayField(TEXT("failed"), FailedJson);
    ResultJson->SetNumberField(TEXT("applied_count"), AppliedJson.Num());
    ResultJson->SetNumberField(TEXT("failed_count"), FailedJson.Num());
    return ResultJson;
}
