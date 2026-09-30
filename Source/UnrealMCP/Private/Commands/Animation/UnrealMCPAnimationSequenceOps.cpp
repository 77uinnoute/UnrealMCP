#include "Commands/Animation/UnrealMCPAnimationCommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimTypes.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimBoneCompressionSettings.h"
#include "Animation/AnimCurveCompressionSettings.h"
#include "Animation/Skeleton.h"
#include "Curves/RichCurve.h"
#include "EditorFramework/AssetImportData.h"
#include "Factories/AnimSequenceFactory.h"
#include "Misc/Char.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealMCPAnimationOps, Log, All);

namespace
{
    /** Registry searches clamp their result; a broad /Game query must not build a huge payload. */
    constexpr int32 DefaultMaxResults = 200;

    /** How close two notify / marker times must be to count as "the same" one. */
    constexpr float TimeTolerance = 0.01f;

    FString NormalizePath(const FString& InPath)
    {
        // Callers pass "/Game/A/B", "/Game/A/B.B" or the short name; the registry answers only
        // the package form, and every string comparison below wants one shape.
        int32 DotIndex = INDEX_NONE;
        FString Package = InPath;
        if (InPath.FindChar(TEXT('.'), DotIndex))
        {
            Package = InPath.Left(DotIndex);
        }
        return Package;
    }

    /**
     * The registry stores object tags as export text, e.g.
     * "/Script/Engine.Skeleton'/Game/Characters/.../SK_Mannequin.SK_Mannequin'". Returning that
     * verbatim would be unusable: a caller cannot compare it with the skeleton path it passed in,
     * and the filter below would never match a plain path. Unwrap to the inner object path.
     */
    FString UnwrapAssetTagPath(const FString& TagValue)
    {
        int32 FirstQuote = INDEX_NONE;
        int32 LastQuote = INDEX_NONE;
        if (TagValue.FindChar(TEXT('\''), FirstQuote) && TagValue.FindLastChar(TEXT('\''), LastQuote)
            && LastQuote > FirstQuote)
        {
            return TagValue.Mid(FirstQuote + 1, LastQuote - FirstQuote - 1);
        }
        return TagValue;
    }

    FString ColorToHex(const FColor& Color)
    {
        return FString::Printf(TEXT("#%s"), *Color.ToHex());
    }

    FString ColorToHex(const FLinearColor& Color)
    {
        return ColorToHex(Color.ToFColor(true));
    }

    bool ParseColorValue(const TSharedPtr<FJsonValue>& Value, FLinearColor& OutColor, FString& OutError)
    {
        if (!Value.IsValid())
        {
            OutError = TEXT("color value is missing");
            return false;
        }

        if (Value->Type == EJson::String)
        {
            const FString Text = Value->AsString();
            // FColor::FromHex reads '#RGB' / '#RRGGBB' / '#RRGGBBAA' and turns anything else into
            // black without complaining, so the shape is validated before it is called.
            FString Hex = Text;
            if (Hex.StartsWith(TEXT("#")))
            {
                Hex.RightChopInline(1);
            }
            bool bValidHex = Hex.Len() == 3 || Hex.Len() == 6 || Hex.Len() == 8;
            for (int32 Index = 0; bValidHex && Index < Hex.Len(); ++Index)
            {
                bValidHex = FChar::IsHexDigit(Hex[Index]);
            }
            if (!bValidHex)
            {
                OutError = FString::Printf(TEXT("color '%s' is not a hex value like '#RRGGBB(AA)'"), *Text);
                return false;
            }

            OutColor = FLinearColor::FromSRGBColor(FColor::FromHex(Text));
            return true;
        }

        const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
        if (Value->TryGetArray(Array) && Array && Array->Num() >= 3)
        {
            TArray<double> Numbers;
            Numbers.Reserve(Array->Num());
            for (const TSharedPtr<FJsonValue>& Item : *Array)
            {
                double Number = 0.0;
                if (!Item->TryGetNumber(Number))
                {
                    OutError = TEXT("color array must hold numbers");
                    return false;
                }
                Numbers.Add(Number);
            }

            const bool bByteRange = Numbers.ContainsByPredicate([](double Number) { return Number > 1.0; });
            const float Scale = bByteRange ? 1.0f / 255.0f : 1.0f;
            OutColor = FLinearColor(
                static_cast<float>(Numbers[0]) * Scale,
                static_cast<float>(Numbers[1]) * Scale,
                static_cast<float>(Numbers[2]) * Scale,
                Numbers.IsValidIndex(3) ? static_cast<float>(Numbers[3]) * Scale : 1.0f);
            return true;
        }

        OutError = TEXT("color must be a hex string or an [r,g,b(,a)] array");
        return false;
    }

    /** Read [x,y,z(,w)] or {x,y,z,w} / {roll,pitch,yaw} into a quaternion. */
    bool ParseRotationValue(const TSharedPtr<FJsonValue>& Value, FQuat& OutRotation, FString& OutError)
    {
        if (!Value.IsValid())
        {
            OutError = TEXT("rotation value is missing");
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
        if (Value->TryGetArray(Array) && Array)
        {
            TArray<double> Numbers;
            for (const TSharedPtr<FJsonValue>& Item : *Array)
            {
                double Number = 0.0;
                if (!Item->TryGetNumber(Number))
                {
                    OutError = TEXT("rotation array must hold numbers");
                    return false;
                }
                Numbers.Add(Number);
            }

            if (Numbers.Num() == 4)
            {
                OutRotation = FQuat(
                    static_cast<double>(Numbers[0]), static_cast<double>(Numbers[1]),
                    static_cast<double>(Numbers[2]), static_cast<double>(Numbers[3]));
                OutRotation.Normalize();
                return true;
            }
            if (Numbers.Num() == 3)
            {
                OutRotation = FRotator(
                    static_cast<double>(Numbers[1]), static_cast<double>(Numbers[2]),
                    static_cast<double>(Numbers[0])).Quaternion();
                return true;
            }

            OutError = TEXT("rotation array must hold 3 euler degrees or 4 quaternion components");
            return false;
        }

        const TSharedPtr<FJsonObject>* Object = nullptr;
        if (Value->TryGetObject(Object) && Object)
        {
            double X = 0.0;
            double Y = 0.0;
            double Z = 0.0;
            double W = 0.0;
            const bool bHasQuat = (*Object)->TryGetNumberField(TEXT("x"), X) &&
                                  (*Object)->TryGetNumberField(TEXT("y"), Y) &&
                                  (*Object)->TryGetNumberField(TEXT("z"), Z) &&
                                  (*Object)->TryGetNumberField(TEXT("w"), W);
            if (bHasQuat)
            {
                OutRotation = FQuat(X, Y, Z, W);
                OutRotation.Normalize();
                return true;
            }

            double Roll = 0.0;
            double Pitch = 0.0;
            double Yaw = 0.0;
            if ((*Object)->TryGetNumberField(TEXT("roll"), Roll) ||
                (*Object)->TryGetNumberField(TEXT("pitch"), Pitch) ||
                (*Object)->TryGetNumberField(TEXT("yaw"), Yaw))
            {
                OutRotation = FRotator(Pitch, Yaw, Roll).Quaternion();
                return true;
            }

            OutError = TEXT("rotation object must hold x/y/z/w or roll/pitch/yaw");
            return false;
        }

        OutError = TEXT("rotation must be an array or an object");
        return false;
    }

    TSharedPtr<FJsonObject> TransformToJson(const FTransform& Transform)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        const FVector Location = Transform.GetLocation();
        const FRotator Rotation = Transform.Rotator();
        const FVector Scale = Transform.GetScale3D();

        Json->SetArrayField(TEXT("position"), {
            MakeShared<FJsonValueNumber>(Location.X),
            MakeShared<FJsonValueNumber>(Location.Y),
            MakeShared<FJsonValueNumber>(Location.Z)});
        Json->SetArrayField(TEXT("rotation"), {
            MakeShared<FJsonValueNumber>(Rotation.Roll),
            MakeShared<FJsonValueNumber>(Rotation.Pitch),
            MakeShared<FJsonValueNumber>(Rotation.Yaw)});
        Json->SetArrayField(TEXT("quaternion"), {
            MakeShared<FJsonValueNumber>(Transform.GetRotation().X),
            MakeShared<FJsonValueNumber>(Transform.GetRotation().Y),
            MakeShared<FJsonValueNumber>(Transform.GetRotation().Z),
            MakeShared<FJsonValueNumber>(Transform.GetRotation().W)});
        Json->SetArrayField(TEXT("scale"), {
            MakeShared<FJsonValueNumber>(Scale.X),
            MakeShared<FJsonValueNumber>(Scale.Y),
            MakeShared<FJsonValueNumber>(Scale.Z)});
        return Json;
    }

    TSharedPtr<FJsonObject> SequenceInfoToJson(const FUnrealMCPAnimSequenceInfo& Info)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("asset_path"), Info.AssetPath);
        Json->SetStringField(TEXT("name"), Info.AssetName);
        Json->SetStringField(TEXT("skeleton"), Info.SkeletonPath);
        Json->SetNumberField(TEXT("length"), Info.Length);
        Json->SetNumberField(TEXT("frame_rate"), Info.FrameRate);
        Json->SetNumberField(TEXT("frame_count"), Info.FrameCount);
        Json->SetNumberField(TEXT("bone_track_count"), Info.BoneTrackCount);
        Json->SetNumberField(TEXT("curve_count"), Info.CurveCount);
        Json->SetNumberField(TEXT("notify_count"), Info.NotifyCount);
        Json->SetNumberField(TEXT("notify_track_count"), Info.NotifyTrackCount);
        Json->SetNumberField(TEXT("sync_marker_count"), Info.SyncMarkerCount);
        Json->SetNumberField(TEXT("rate_scale"), Info.RateScale);
        Json->SetBoolField(TEXT("enable_root_motion"), Info.bEnableRootMotion);
        Json->SetBoolField(TEXT("force_root_lock"), Info.bForceRootLock);
        Json->SetStringField(TEXT("root_motion_root_lock"), Info.RootMotionRootLock);
        Json->SetStringField(TEXT("additive_anim_type"), Info.AdditiveAnimType);
        Json->SetStringField(TEXT("additive_base_pose"), Info.AdditiveBasePose);
        Json->SetNumberField(TEXT("raw_size"), static_cast<double>(Info.RawSize));
        Json->SetNumberField(TEXT("compressed_size"), Info.CompressedSize);
        Json->SetStringField(TEXT("compression_scheme"), Info.CompressionScheme);
        Json->SetStringField(TEXT("info_source"), Info.InfoSource);
        return Json;
    }

    TSharedPtr<FJsonObject> CurveInfoToJson(const FUnrealMCPAnimCurveInfo& Info)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("curve_name"), Info.CurveName);
        Json->SetNumberField(TEXT("key_count"), Info.KeyCount);
        Json->SetNumberField(TEXT("default_value"), Info.DefaultValue);
        Json->SetNumberField(TEXT("curve_type_flags"), Info.CurveTypeFlags);
        Json->SetBoolField(TEXT("editable"), Info.bEditable);
        Json->SetBoolField(TEXT("metadata"), Info.bMetadata);
        Json->SetBoolField(TEXT("disabled"), Info.bDisabled);
        return Json;
    }

    TSharedPtr<FJsonObject> NotifyToJson(const FUnrealMCPAnimNotifyInfo& Info)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetNumberField(TEXT("notify_index"), Info.NotifyIndex);
        Json->SetStringField(TEXT("notify_name"), Info.NotifyName);
        Json->SetStringField(TEXT("notify_class"), Info.NotifyClass);
        Json->SetStringField(TEXT("entry_type"), Info.EntryType);
        Json->SetNumberField(TEXT("trigger_time"), Info.TriggerTime);
        Json->SetNumberField(TEXT("duration"), Info.Duration);
        Json->SetNumberField(TEXT("end_time"), Info.EndTime);
        Json->SetNumberField(TEXT("track_index"), Info.TrackIndex);
        Json->SetStringField(TEXT("track_name"), Info.TrackName);
        Json->SetNumberField(TEXT("trigger_chance"), Info.TriggerChance);
        Json->SetNumberField(TEXT("weight_threshold"), Info.WeightThreshold);
        Json->SetBoolField(TEXT("trigger_on_server"), Info.bTriggerOnServer);
        Json->SetBoolField(TEXT("trigger_on_follower"), Info.bTriggerOnFollower);
        Json->SetStringField(TEXT("filter_type"), Info.FilterType);
        Json->SetNumberField(TEXT("filter_lod"), Info.FilterLOD);
        Json->SetStringField(TEXT("color"), Info.Color);
        Json->SetStringField(TEXT("guid"), Info.Guid);
        return Json;
    }

    TSharedPtr<FJsonObject> NotifyTrackToJson(const FUnrealMCPAnimNotifyTrackInfo& Info)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetNumberField(TEXT("track_index"), Info.TrackIndex);
        Json->SetStringField(TEXT("track_name"), Info.TrackName);
        Json->SetStringField(TEXT("color"), Info.Color);
        Json->SetNumberField(TEXT("notify_count"), Info.NotifyCount);
        Json->SetNumberField(TEXT("sync_marker_count"), Info.SyncMarkerCount);
        return Json;
    }

    TSharedPtr<FJsonObject> SyncMarkerToJson(const FUnrealMCPAnimSyncMarkerInfo& Info)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("marker_name"), Info.MarkerName);
        Json->SetNumberField(TEXT("time"), Info.Time);
        Json->SetNumberField(TEXT("track_index"), Info.TrackIndex);
        return Json;
    }
}

// ---------------------------------------------------------------------------
// resolution
// ---------------------------------------------------------------------------

FFrameRate FUnrealMCPAnimationOps::GetSequenceFrameRate(const UAnimSequence* Sequence)
{
    if (Sequence)
    {
        if (const IAnimationDataModel* Model = Sequence->GetDataModel())
        {
            return Model->GetFrameRate();
        }
    }
    return FFrameRate(30, 1);
}

int32 FUnrealMCPAnimationOps::GetSequenceFrameCount(const UAnimSequence* Sequence)
{
    if (Sequence)
    {
        if (const IAnimationDataModel* Model = Sequence->GetDataModel())
        {
            return Model->GetNumberOfFrames();
        }
    }
    return 0;
}

bool FUnrealMCPAnimationOps::ResolveSequence(const FString& AssetPath, UAnimSequence*& OutSequence,
                                             FUnrealMCPAnimError& OutError)
{
    OutSequence = nullptr;
    if (AssetPath.IsEmpty())
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }

    UObject* Asset = FUnrealMCPCommonUtils::FindAsset(AssetPath);
    if (!Asset)
    {
        OutError.Set(EUnrealMCPAnimError::AssetNotFound,
                     FString::Printf(TEXT("no asset at '%s' (pass '/Game/Dir/Asset' or '/Game/Dir/Asset.Asset')"), *AssetPath));
        return false;
    }

    OutSequence = Cast<UAnimSequence>(Asset);
    if (!OutSequence)
    {
        OutError.Set(EUnrealMCPAnimError::AssetNotAnimSequence,
                     FString::Printf(TEXT("'%s' is a %s, not an animation sequence (anim composite / blend space are not covered)"),
                                     *AssetPath, *Asset->GetClass()->GetName()));
        return false;
    }

    return true;
}

bool FUnrealMCPAnimationOps::ResolveSkeleton(const FString& AssetPath, USkeleton*& OutSkeleton,
                                             FUnrealMCPAnimError& OutError)
{
    OutSkeleton = nullptr;
    if (AssetPath.IsEmpty())
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("skeleton is required"));
        return false;
    }

    UObject* Asset = FUnrealMCPCommonUtils::FindAsset(AssetPath);
    if (!Asset)
    {
        OutError.Set(EUnrealMCPAnimError::AssetNotFound, FString::Printf(TEXT("no asset at '%s'"), *AssetPath));
        return false;
    }

    OutSkeleton = Cast<USkeleton>(Asset);
    if (!OutSkeleton)
    {
        OutError.Set(EUnrealMCPAnimError::AssetNotSkeleton,
                     FString::Printf(TEXT("'%s' is a %s, not a skeleton"), *AssetPath, *Asset->GetClass()->GetName()));
        return false;
    }

    return true;
}

bool FUnrealMCPAnimationOps::ResolveBoneIndex(UAnimSequence* Sequence, const FString& BoneName, int32& OutBoneIndex,
                                              FUnrealMCPAnimError& OutError)
{
    OutBoneIndex = INDEX_NONE;
    USkeleton* Skeleton = Sequence ? Sequence->GetSkeleton() : nullptr;
    if (!Skeleton)
    {
        OutError.Set(EUnrealMCPAnimError::AssetNotSkeleton,
                     FString::Printf(TEXT("'%s' has no skeleton"), *GetNameSafe(Sequence)));
        return false;
    }

    const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
    const int32 BoneIndex = RefSkeleton.FindBoneIndex(FName(*BoneName));
    if (BoneIndex == INDEX_NONE)
    {
        OutError.Set(EUnrealMCPAnimError::BoneNotFound,
                     FString::Printf(TEXT("bone '%s' is not part of skeleton '%s'"), *BoneName, *Skeleton->GetName()));
        // Bone name suggestions only: the full list is what the caller needs to retry.
        const int32 MaxCandidates = 60;
        for (int32 Index = 0; Index < RefSkeleton.GetNum() && OutError.Candidates.Num() < MaxCandidates; ++Index)
        {
            OutError.Candidates.Add(RefSkeleton.GetBoneName(Index).ToString());
        }
        return false;
    }

    OutBoneIndex = BoneIndex;
    return true;
}

// ---------------------------------------------------------------------------
// discovery
// ---------------------------------------------------------------------------

namespace
{
    /** Shared registry query: metadata only, the assets are never loaded. */
    bool QuerySequenceRegistry(const FString& SearchPath, TArray<FAssetData>& OutAssets, FUnrealMCPAnimError& OutError)
    {
        const FString Root = SearchPath.IsEmpty() ? TEXT("/Game") : SearchPath;

        FAssetRegistryModule& AssetRegistryModule =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        IAssetRegistry& Registry = AssetRegistryModule.Get();

        FARFilter Filter;
        Filter.bRecursivePaths = true;
        Filter.bRecursiveClasses = true;
        Filter.PackagePaths.Add(FName(*Root));
        Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());

        Registry.GetAssets(Filter, OutAssets);
        if (OutAssets.Num() == 0)
        {
            // An empty result is not an error (an empty folder is a legitimate answer), but a folder
            // that does not exist at all usually means a typo, so say which one was searched.
            OutError.Message = FString::Printf(TEXT("no animation sequences found below '%s'"), *Root);
        }
        return true;
    }

    void FillRegistryInfo(const FAssetData& AssetData, FUnrealMCPAnimSequenceInfo& OutInfo)
    {
        OutInfo.AssetPath = AssetData.GetObjectPathString();
        OutInfo.AssetName = AssetData.AssetName.ToString();
        OutInfo.InfoSource = TEXT("registry");

        FString TagValue;
        if (AssetData.GetTagValue(TEXT("Skeleton"), TagValue))
        {
            OutInfo.SkeletonPath = UnwrapAssetTagPath(TagValue);
        }
        if (AssetData.GetTagValue(TEXT("SequenceLength"), TagValue))
        {
            OutInfo.Length = FCString::Atof(*TagValue);
        }
        if (AssetData.GetTagValue(TEXT("SamplingFrameRate"), TagValue))
        {
            FString RateNumerator = TagValue;
            FString RateDenominator;
            if (TagValue.Split(TEXT("/"), &RateNumerator, &RateDenominator))
            {
                const double Denominator = FCString::Atof(*RateDenominator);
                OutInfo.FrameRate = Denominator != 0.0
                    ? static_cast<float>(FCString::Atof(*RateNumerator) / Denominator)
                    : 0.0f;
            }
            else
            {
                OutInfo.FrameRate = FCString::Atof(*TagValue);
            }
        }
    }
}

bool FUnrealMCPAnimationOps::ListSequences(const FString& SearchPath, const FString& SkeletonFilter, int32 MaxResults,
                                           TArray<FUnrealMCPAnimSequenceInfo>& OutInfos, int32& OutFoundCount,
                                           FUnrealMCPAnimError& OutError)
{
    OutInfos.Reset();
    OutFoundCount = 0;

    TArray<FAssetData> Assets;
    QuerySequenceRegistry(SearchPath, Assets, OutError);

    const FString WantedSkeleton = NormalizePath(SkeletonFilter);
    const int32 Limit = MaxResults > 0 ? MaxResults : DefaultMaxResults;

    for (const FAssetData& AssetData : Assets)
    {
        if (!WantedSkeleton.IsEmpty())
        {
            FString TagValue;
            if (!AssetData.GetTagValue(TEXT("Skeleton"), TagValue)
                || NormalizePath(UnwrapAssetTagPath(TagValue)) != WantedSkeleton)
            {
                continue;
            }
        }

        ++OutFoundCount;
        if (OutInfos.Num() >= Limit)
        {
            continue;
        }

        FUnrealMCPAnimSequenceInfo Info;
        FillRegistryInfo(AssetData, Info);
        OutInfos.Add(Info);
    }

    OutError.Message = FString::Printf(TEXT("%d sequence(s) matched below '%s'"), OutFoundCount,
                                       *(SearchPath.IsEmpty() ? TEXT("/Game") : SearchPath));
    return true;
}

bool FUnrealMCPAnimationOps::SearchSequences(const FString& SearchPath, const FString& Query, int32 MaxResults,
                                             TArray<FUnrealMCPAnimSequenceInfo>& OutInfos, int32& OutFoundCount,
                                             FUnrealMCPAnimError& OutError)
{
    OutInfos.Reset();
    OutFoundCount = 0;

    if (Query.IsEmpty())
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("query is required"));
        return false;
    }

    TArray<FAssetData> Assets;
    QuerySequenceRegistry(SearchPath, Assets, OutError);

    const int32 Limit = MaxResults > 0 ? MaxResults : DefaultMaxResults;
    const bool bHasWildcard = Query.Contains(TEXT("*")) || Query.Contains(TEXT("?"));

    for (const FAssetData& AssetData : Assets)
    {
        const FString AssetName = AssetData.AssetName.ToString();
        const bool bMatch = bHasWildcard ? AssetName.MatchesWildcard(Query, ESearchCase::IgnoreCase)
                                         : AssetName.Contains(Query, ESearchCase::IgnoreCase);
        if (!bMatch)
        {
            continue;
        }

        ++OutFoundCount;
        if (OutInfos.Num() >= Limit)
        {
            continue;
        }

        FUnrealMCPAnimSequenceInfo Info;
        FillRegistryInfo(AssetData, Info);
        OutInfos.Add(Info);
    }

    OutError.Message = FString::Printf(TEXT("%d sequence(s) matched query '%s'"), OutFoundCount, *Query);
    return true;
}

void FUnrealMCPAnimationOps::FillSequenceInfo(UAnimSequence* Sequence, FUnrealMCPAnimSequenceInfo& OutInfo)
{
    OutInfo = FUnrealMCPAnimSequenceInfo();
    if (!Sequence)
    {
        return;
    }

    OutInfo.AssetPath = Sequence->GetPathName();
    OutInfo.AssetName = Sequence->GetName();
    OutInfo.InfoSource = TEXT("asset");
    OutInfo.Length = Sequence->GetPlayLength();
    OutInfo.FrameRate = GetSequenceFrameRate(Sequence).AsDecimal();
    OutInfo.FrameCount = GetSequenceFrameCount(Sequence);
    OutInfo.RateScale = Sequence->RateScale;
    OutInfo.bEnableRootMotion = Sequence->bEnableRootMotion;
    OutInfo.bForceRootLock = Sequence->bForceRootLock;
    OutInfo.RootMotionRootLock = RootLockToString(static_cast<int32>(Sequence->RootMotionRootLock.GetValue()));
    OutInfo.AdditiveAnimType = AdditiveTypeToString(static_cast<int32>(Sequence->AdditiveAnimType.GetValue()));
    OutInfo.AdditiveBasePose = Sequence->RefPoseSeq ? Sequence->RefPoseSeq->GetPathName() : FString();
    OutInfo.SkeletonPath = Sequence->GetSkeleton() ? Sequence->GetSkeleton()->GetPathName() : FString();
    OutInfo.NotifyCount = Sequence->Notifies.Num();
    OutInfo.NotifyTrackCount = Sequence->AnimNotifyTracks.Num();
    OutInfo.SyncMarkerCount = Sequence->AuthoredSyncMarkers.Num();
    OutInfo.RawSize = Sequence->GetApproxRawSize();
    OutInfo.CompressedSize = Sequence->GetApproxCompressedSize();
    OutInfo.CompressionScheme = Sequence->BoneCompressionSettings
        ? Sequence->BoneCompressionSettings->GetPathName()
        : FString();

    if (const IAnimationDataModel* Model = Sequence->GetDataModel())
    {
        OutInfo.BoneTrackCount = Model->GetNumBoneTracks();
        OutInfo.CurveCount = Model->GetNumberOfFloatCurves() + Model->GetNumberOfTransformCurves();
    }
}

bool FUnrealMCPAnimationOps::GetBoneNames(UAnimSequence* Sequence, TArray<FString>& OutBoneNames,
                                          FUnrealMCPAnimError& OutError)
{
    OutBoneNames.Reset();

    USkeleton* Skeleton = Sequence ? Sequence->GetSkeleton() : nullptr;
    if (!Skeleton)
    {
        OutError.Set(EUnrealMCPAnimError::AssetNotSkeleton,
                     FString::Printf(TEXT("'%s' has no skeleton"), *GetNameSafe(Sequence)));
        return false;
    }

    const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
    OutBoneNames.Reserve(RefSkeleton.GetNum());
    for (int32 Index = 0; Index < RefSkeleton.GetNum(); ++Index)
    {
        OutBoneNames.Add(RefSkeleton.GetBoneName(Index).ToString());
    }
    return true;
}

// ---------------------------------------------------------------------------
// sampling
// ---------------------------------------------------------------------------

bool FUnrealMCPAnimationOps::SampleBoneTransform(UAnimSequence* Sequence, int32 BoneIndex, float Time,
                                                 FTransform& OutTransform, FUnrealMCPAnimError& OutError)
{
    OutTransform = FTransform::Identity;
    if (!Sequence)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("sequence is required"));
        return false;
    }

    const float Length = Sequence->GetPlayLength();
    const double ClampedTime = FMath::Clamp(static_cast<double>(Time), 0.0, static_cast<double>(Length));

    // Raw data on purpose: a freshly created (uncompressed) sequence has no compressed data to
    // sample from, and the raw data is what the editor shows as authored.
    Sequence->GetBoneTransform(OutTransform, FSkeletonPoseBoneIndex(BoneIndex), ClampedTime, /*bUseRawData*/ true);
    return true;
}

bool FUnrealMCPAnimationOps::SamplePose(UAnimSequence* Sequence, float Time, TArray<FUnrealMCPAnimBonePose>& OutPose,
                                        FUnrealMCPAnimError& OutError)
{
    OutPose.Reset();

    USkeleton* Skeleton = Sequence ? Sequence->GetSkeleton() : nullptr;
    if (!Skeleton)
    {
        OutError.Set(EUnrealMCPAnimError::AssetNotSkeleton,
                     FString::Printf(TEXT("'%s' has no skeleton"), *GetNameSafe(Sequence)));
        return false;
    }

    const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
    const float Length = Sequence->GetPlayLength();
    const double ClampedTime = FMath::Clamp(static_cast<double>(Time), 0.0, static_cast<double>(Length));

    OutPose.Reserve(RefSkeleton.GetNum());
    for (int32 BoneIndex = 0; BoneIndex < RefSkeleton.GetNum(); ++BoneIndex)
    {
        FUnrealMCPAnimBonePose Pose;
        Pose.BoneName = RefSkeleton.GetBoneName(BoneIndex).ToString();
        Pose.BoneIndex = BoneIndex;
        Sequence->GetBoneTransform(Pose.Transform, FSkeletonPoseBoneIndex(BoneIndex), ClampedTime, true);
        OutPose.Add(Pose);
    }
    return true;
}

bool FUnrealMCPAnimationOps::SampleRootMotion(UAnimSequence* Sequence, float Time, bool bTotal,
                                              FTransform& OutTransform, FUnrealMCPAnimError& OutError)
{
    OutTransform = FTransform::Identity;
    if (!Sequence)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("sequence is required"));
        return false;
    }

    const float Length = Sequence->GetPlayLength();
    // 5.5 has no FAnimExtractContext overload on UAnimSequence; the range form is what the engine
    // itself uses, and it reads the authored data.
    const float DeltaTime = bTotal ? Length : FMath::Clamp(Time, 0.0f, Length);
    OutTransform = Sequence->ExtractRootMotion(0.0f, DeltaTime, /*bAllowLooping*/ false);
    return true;
}

// ---------------------------------------------------------------------------
// curves
// ---------------------------------------------------------------------------

namespace
{
    const FFloatCurve* FindFloatCurve(const UAnimSequence* Sequence, const FString& CurveName)
    {
        const IAnimationDataModel* Model = Sequence ? Sequence->GetDataModel() : nullptr;
        if (!Model)
        {
            return nullptr;
        }

        for (const FFloatCurve& Curve : Model->GetCurveData().FloatCurves)
        {
            if (Curve.GetName().ToString().Equals(CurveName, ESearchCase::IgnoreCase))
            {
                return &Curve;
            }
        }
        return nullptr;
    }

    TArray<FString> FloatCurveNames(const UAnimSequence* Sequence)
    {
        TArray<FString> Names;
        const IAnimationDataModel* Model = Sequence ? Sequence->GetDataModel() : nullptr;
        if (Model)
        {
            for (const FFloatCurve& Curve : Model->GetCurveData().FloatCurves)
            {
                Names.Add(Curve.GetName().ToString());
            }
        }
        return Names;
    }

    bool CollectRichCurveKeys(const FFloatCurve* Curve, TArray<FRichCurveKey>& OutKeys)
    {
        OutKeys.Reset();
        if (!Curve)
        {
            return false;
        }

        for (auto It = Curve->FloatCurve.GetKeyIterator(); It; ++It)
        {
            OutKeys.Add(*It);
        }
        return true;
    }

    FAnimationCurveIdentifier MakeFloatCurveId(const FString& CurveName)
    {
        return FAnimationCurveIdentifier(FName(*CurveName), ERawCurveTrackTypes::RCT_Float);
    }
}

bool FUnrealMCPAnimationOps::ListCurves(UAnimSequence* Sequence, TArray<FUnrealMCPAnimCurveInfo>& OutCurves,
                                        int32& OutTransformCurveCount, FUnrealMCPAnimError& OutError)
{
    OutCurves.Reset();
    OutTransformCurveCount = 0;

    const IAnimationDataModel* Model = Sequence ? Sequence->GetDataModel() : nullptr;
    if (!Model)
    {
        OutError.Set(EUnrealMCPAnimError::WriteFailed,
                     FString::Printf(TEXT("'%s' has no animation data model"), *GetNameSafe(Sequence)));
        return false;
    }

    const FAnimationCurveData& CurveData = Model->GetCurveData();
    OutTransformCurveCount = CurveData.TransformCurves.Num();

    for (const FFloatCurve& Curve : CurveData.FloatCurves)
    {
        FUnrealMCPAnimCurveInfo Info;
        Info.CurveName = Curve.GetName().ToString();
        Info.KeyCount = Curve.FloatCurve.GetNumKeys();
        Info.DefaultValue = Curve.FloatCurve.GetDefaultValue();
        Info.CurveTypeFlags = Curve.GetCurveTypeFlags();
        Info.bEditable = Curve.GetCurveTypeFlag(EAnimAssetCurveFlags::AACF_Editable);
        Info.bMetadata = Curve.GetCurveTypeFlag(EAnimAssetCurveFlags::AACF_Metadata);
        Info.bDisabled = Curve.GetCurveTypeFlag(EAnimAssetCurveFlags::AACF_Disabled);
        OutCurves.Add(Info);
    }
    return true;
}

bool FUnrealMCPAnimationOps::GetCurveKeys(UAnimSequence* Sequence, const FString& CurveName,
                                          TArray<FUnrealMCPAnimCurveKey>& OutKeys, FUnrealMCPAnimError& OutError)
{
    OutKeys.Reset();

    const FFloatCurve* Curve = FindFloatCurve(Sequence, CurveName);
    if (!Curve)
    {
        OutError.Set(EUnrealMCPAnimError::CurveNotFound,
                     FString::Printf(TEXT("curve '%s' does not exist on '%s'"), *CurveName, *GetNameSafe(Sequence)));
        OutError.Candidates = FloatCurveNames(Sequence);
        return false;
    }

    for (auto It = Curve->FloatCurve.GetKeyIterator(); It; ++It)
    {
        const FRichCurveKey& Key = *It;
        FUnrealMCPAnimCurveKey Out;
        Out.Time = Key.Time;
        Out.Value = Key.Value;
        Out.ArriveTangent = Key.ArriveTangent;
        Out.LeaveTangent = Key.LeaveTangent;
        Out.InterpMode = InterpModeToString(static_cast<int32>(Key.InterpMode));
        Out.TangentMode = TangentModeToString(static_cast<int32>(Key.TangentMode));
        OutKeys.Add(Out);
    }
    return true;
}

bool FUnrealMCPAnimationOps::GetCurveValue(UAnimSequence* Sequence, const FString& CurveName, float Time,
                                           float& OutValue, FUnrealMCPAnimError& OutError)
{
    OutValue = 0.0f;

    const FFloatCurve* Curve = FindFloatCurve(Sequence, CurveName);
    if (!Curve)
    {
        OutError.Set(EUnrealMCPAnimError::CurveNotFound,
                     FString::Printf(TEXT("curve '%s' does not exist on '%s'"), *CurveName, *GetNameSafe(Sequence)));
        OutError.Candidates = FloatCurveNames(Sequence);
        return false;
    }

    OutValue = Curve->FloatCurve.Eval(Time);
    return true;
}

bool FUnrealMCPAnimationOps::AddCurve(UAnimSequence* Sequence, const FString& CurveName, FUnrealMCPAnimError& OutError)
{
    if (!Sequence || CurveName.IsEmpty())
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path and curve_name are required"));
        return false;
    }

    if (FindFloatCurve(Sequence, CurveName))
    {
        OutError.Set(EUnrealMCPAnimError::CurveExists,
                     FString::Printf(TEXT("curve '%s' already exists on '%s'"), *CurveName, *Sequence->GetName()));
        OutError.Candidates = FloatCurveNames(Sequence);
        return false;
    }

    Sequence->Modify();
    IAnimationDataController& Controller = Sequence->GetController();
    // Flags stay at the engine default (editable); the transaction belongs to the command layer.
    if (!Controller.AddCurve(MakeFloatCurveId(CurveName), static_cast<int32>(AACF_DefaultCurve)))
    {
        OutError.Set(EUnrealMCPAnimError::WriteFailed,
                     FString::Printf(TEXT("AddCurve('%s') was refused by the data controller"), *CurveName));
        return false;
    }

    Sequence->MarkPackageDirty();
    return true;
}

bool FUnrealMCPAnimationOps::RemoveCurve(UAnimSequence* Sequence, const FString& CurveName, FUnrealMCPAnimError& OutError)
{
    if (!Sequence || CurveName.IsEmpty())
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path and curve_name are required"));
        return false;
    }

    if (!FindFloatCurve(Sequence, CurveName))
    {
        OutError.Set(EUnrealMCPAnimError::CurveNotFound,
                     FString::Printf(TEXT("curve '%s' does not exist on '%s'"), *CurveName, *Sequence->GetName()));
        OutError.Candidates = FloatCurveNames(Sequence);
        return false;
    }

    Sequence->Modify();
    IAnimationDataController& Controller = Sequence->GetController();
    if (!Controller.RemoveCurve(MakeFloatCurveId(CurveName)))
    {
        OutError.Set(EUnrealMCPAnimError::WriteFailed,
                     FString::Printf(TEXT("RemoveCurve('%s') was refused by the data controller"), *CurveName));
        return false;
    }

    Sequence->MarkPackageDirty();
    return true;
}

bool FUnrealMCPAnimationOps::SetCurveKeys(UAnimSequence* Sequence, const FString& CurveName,
                                          const TArray<FUnrealMCPAnimCurveKey>& Keys, bool bReplaceAll,
                                          FUnrealMCPAnimError& OutError)
{
    if (!Sequence || CurveName.IsEmpty())
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path and curve_name are required"));
        return false;
    }
    if (Keys.Num() == 0)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidValue, TEXT("keys must hold at least one key"));
        return false;
    }

    const FFloatCurve* Curve = FindFloatCurve(Sequence, CurveName);
    if (!Curve)
    {
        OutError.Set(EUnrealMCPAnimError::CurveNotFound,
                     FString::Printf(TEXT("curve '%s' does not exist on '%s' (add_curve first)"),
                                     *CurveName, *Sequence->GetName()));
        OutError.Candidates = FloatCurveNames(Sequence);
        return false;
    }

    TArray<FRichCurveKey> RichKeys;
    if (!bReplaceAll)
    {
        CollectRichCurveKeys(Curve, RichKeys);
    }

    for (const FUnrealMCPAnimCurveKey& Key : Keys)
    {
        FRichCurveKey RichKey;
        RichKey.Time = Key.Time;
        RichKey.Value = Key.Value;
        RichKey.ArriveTangent = Key.ArriveTangent;
        RichKey.LeaveTangent = Key.LeaveTangent;

        int32 InterpMode = static_cast<int32>(ERichCurveInterpMode::RCIM_Cubic);
        if (!Key.InterpMode.IsEmpty() && !StringToInterpMode(Key.InterpMode, InterpMode))
        {
            OutError.Set(EUnrealMCPAnimError::UnknownValue,
                         FString::Printf(TEXT("unknown interp '%s'"), *Key.InterpMode));
            OutError.Candidates = { TEXT("constant"), TEXT("linear"), TEXT("cubic"), TEXT("none") };
            return false;
        }
        int32 TangentMode = static_cast<int32>(ERichCurveTangentMode::RCTM_Auto);
        if (!Key.TangentMode.IsEmpty() && !StringToTangentMode(Key.TangentMode, TangentMode))
        {
            OutError.Set(EUnrealMCPAnimError::UnknownValue,
                         FString::Printf(TEXT("unknown tangent '%s'"), *Key.TangentMode));
            OutError.Candidates = { TEXT("auto"), TEXT("user"), TEXT("break"), TEXT("none") };
            return false;
        }

        RichKey.InterpMode = static_cast<ERichCurveInterpMode>(InterpMode);
        RichKey.TangentMode = static_cast<ERichCurveTangentMode>(TangentMode);
        RichKeys.Add(RichKey);
    }

    RichKeys.Sort([](const FRichCurveKey& A, const FRichCurveKey& B) { return A.Time < B.Time; });

    Sequence->Modify();
    IAnimationDataController& Controller = Sequence->GetController();
    if (!Controller.SetCurveKeys(MakeFloatCurveId(CurveName), RichKeys))
    {
        OutError.Set(EUnrealMCPAnimError::WriteFailed,
                     FString::Printf(TEXT("SetCurveKeys('%s') was refused by the data controller"), *CurveName));
        return false;
    }

    Sequence->MarkPackageDirty();
    return true;
}

// ---------------------------------------------------------------------------
// notifies
// ---------------------------------------------------------------------------

namespace
{
    FString NotifyTrackName(const UAnimSequenceBase* Sequence, int32 TrackIndex)
    {
        if (Sequence && Sequence->AnimNotifyTracks.IsValidIndex(TrackIndex))
        {
            return Sequence->AnimNotifyTracks[TrackIndex].TrackName.ToString();
        }
        return FString();
    }

    int32 FindNotifyIndexByGuid(const UAnimSequenceBase* Sequence, const FGuid& Guid)
    {
        if (!Sequence)
        {
            return INDEX_NONE;
        }
        for (int32 Index = 0; Index < Sequence->Notifies.Num(); ++Index)
        {
            if (Sequence->Notifies[Index].Guid == Guid)
            {
                return Index;
            }
        }
        return INDEX_NONE;
    }

    void FillNotifyInfo(const UAnimSequenceBase* Sequence, int32 NotifyIndex, FUnrealMCPAnimNotifyInfo& OutInfo)
    {
        const FAnimNotifyEvent& Notify = Sequence->Notifies[NotifyIndex];
        const UObject* NotifyObject = Notify.NotifyStateClass ? static_cast<const UObject*>(Notify.NotifyStateClass)
                                                              : static_cast<const UObject*>(Notify.Notify);

        OutInfo.NotifyIndex = NotifyIndex;
        OutInfo.NotifyName = Notify.NotifyName.ToString();
        OutInfo.NotifyClass = NotifyObject ? NotifyObject->GetClass()->GetName() : FString();
        OutInfo.EntryType = Notify.NotifyStateClass ? TEXT("notify_state") : TEXT("notify");
        OutInfo.TriggerTime = Notify.GetTriggerTime();
        OutInfo.Duration = Notify.GetDuration();
        OutInfo.EndTime = OutInfo.TriggerTime + OutInfo.Duration;
        OutInfo.TrackIndex = Notify.TrackIndex;
        OutInfo.TrackName = NotifyTrackName(Sequence, Notify.TrackIndex);
        OutInfo.TriggerChance = Notify.NotifyTriggerChance;
        OutInfo.WeightThreshold = Notify.TriggerWeightThreshold;
        OutInfo.bTriggerOnServer = Notify.bTriggerOnDedicatedServer;
        OutInfo.bTriggerOnFollower = Notify.bTriggerOnFollower;
        OutInfo.FilterType = FUnrealMCPAnimationOps::FilterTypeToString(static_cast<int32>(Notify.NotifyFilterType.GetValue()));
        OutInfo.FilterLOD = Notify.NotifyFilterLOD;
        OutInfo.Color = ColorToHex(Notify.NotifyColor);
        OutInfo.Guid = Notify.Guid.ToString();
    }
}

void FUnrealMCPAnimationOps::ListNotifies(UAnimSequence* Sequence, TArray<FUnrealMCPAnimNotifyInfo>& OutNotifies)
{
    OutNotifies.Reset();
    if (!Sequence)
    {
        return;
    }

    OutNotifies.Reserve(Sequence->Notifies.Num());
    for (int32 Index = 0; Index < Sequence->Notifies.Num(); ++Index)
    {
        FUnrealMCPAnimNotifyInfo Info;
        FillNotifyInfo(Sequence, Index, Info);
        OutNotifies.Add(Info);
    }
}

bool FUnrealMCPAnimationOps::ResolveNotify(UAnimSequence* Sequence, int32 NotifyIndex, const FString& NotifyName,
                                           const FString& Guid, float Time, bool bHasTime, int32& OutNotifyIndex,
                                           FUnrealMCPAnimError& OutError)
{
    OutNotifyIndex = INDEX_NONE;
    if (!Sequence)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("sequence is required"));
        return false;
    }

    if (!Guid.IsEmpty())
    {
        FGuid ParsedGuid;
        if (!FGuid::Parse(Guid, ParsedGuid))
        {
            OutError.Set(EUnrealMCPAnimError::InvalidValue,
                         FString::Printf(TEXT("guid '%s' is not a valid guid"), *Guid));
            return false;
        }

        const int32 Index = FindNotifyIndexByGuid(Sequence, ParsedGuid);
        if (Index == INDEX_NONE)
        {
            OutError.Set(EUnrealMCPAnimError::NotifyNotFound,
                         FString::Printf(TEXT("no notify with guid '%s' on '%s'"), *Guid, *Sequence->GetName()));
            return false;
        }

        OutNotifyIndex = Index;
        return true;
    }

    if (!NotifyName.IsEmpty())
    {
        TArray<int32> Matches;
        for (int32 Index = 0; Index < Sequence->Notifies.Num(); ++Index)
        {
            const FAnimNotifyEvent& Notify = Sequence->Notifies[Index];
            if (!Notify.NotifyName.ToString().Equals(NotifyName, ESearchCase::IgnoreCase))
            {
                continue;
            }
            if (bHasTime && !FMath::IsNearlyEqual(Notify.GetTriggerTime(), Time, TimeTolerance))
            {
                continue;
            }
            Matches.Add(Index);
        }

        if (Matches.Num() == 1)
        {
            OutNotifyIndex = Matches[0];
            return true;
        }

        for (int32 Index = 0; Index < Sequence->Notifies.Num(); ++Index)
        {
            const FAnimNotifyEvent& Notify = Sequence->Notifies[Index];
            OutError.Candidates.Add(FString::Printf(TEXT("%d: %s @ %.4fs (track %d)"), Index,
                                                    *Notify.NotifyName.ToString(), Notify.GetTriggerTime(),
                                                    Notify.TrackIndex));
        }

        if (Matches.Num() == 0)
        {
            OutError.Set(EUnrealMCPAnimError::NotifyNotFound,
                         bHasTime
                             ? FString::Printf(TEXT("no notify named '%s' at %.4fs on '%s'"),
                                               *NotifyName, Time, *Sequence->GetName())
                             : FString::Printf(TEXT("no notify named '%s' on '%s'"), *NotifyName, *Sequence->GetName()));
        }
        else
        {
            OutError.Set(EUnrealMCPAnimError::AmbiguousNotify,
                         FString::Printf(TEXT("'%s' matches %d notifies on '%s'; pass notify_index or 'time' to pick one"),
                                         *NotifyName, Matches.Num(), *Sequence->GetName()));
        }
        return false;
    }

    if (!Sequence->Notifies.IsValidIndex(NotifyIndex))
    {
        OutError.Set(EUnrealMCPAnimError::NotifyNotFound,
                     FString::Printf(TEXT("notify_index %d is out of range (%d notify(ies) on '%s')"),
                                     NotifyIndex, Sequence->Notifies.Num(), *Sequence->GetName()));
        for (int32 Index = 0; Index < Sequence->Notifies.Num(); ++Index)
        {
            OutError.Candidates.Add(FString::Printf(TEXT("%d: %s @ %.4fs"), Index,
                                                    *Sequence->Notifies[Index].NotifyName.ToString(),
                                                    Sequence->Notifies[Index].GetTriggerTime()));
        }
        return false;
    }

    OutNotifyIndex = NotifyIndex;
    return true;
}

bool FUnrealMCPAnimationOps::AddNotify(UAnimSequence* Sequence, const FString& NotifyName,
                                       const FString& NotifyClassName, float Time, float Duration, int32 TrackIndex,
                                       bool bState, int32& OutNotifyIndex, FUnrealMCPAnimError& OutError)
{
    OutNotifyIndex = INDEX_NONE;
    if (!Sequence)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }

    UClass* NotifyClass = nullptr;
    if (!NotifyClassName.IsEmpty())
    {
        TArray<FString> Tried;
        FString ResolvedPath;
        NotifyClass = FUnrealMCPCommonUtils::ResolveUClass(NotifyClassName, Tried, ResolvedPath);
        if (!NotifyClass)
        {
            OutError.Set(EUnrealMCPAnimError::InvalidParams,
                         FString::Printf(TEXT("class '%s' was not found (tried: %s)"),
                                         *NotifyClassName, *FString::Join(Tried, TEXT(", "))));
            return false;
        }

        UClass* RequiredBase = bState ? UAnimNotifyState::StaticClass() : UAnimNotify::StaticClass();
        if (!NotifyClass->IsChildOf(RequiredBase))
        {
            OutError.Set(EUnrealMCPAnimError::InvalidParams,
                         FString::Printf(TEXT("class '%s' is not a %s"), *NotifyClass->GetName(),
                                         bState ? TEXT("UAnimNotifyState") : TEXT("UAnimNotify")));
            return false;
        }

        // UAnimNotify / UAnimNotifyState themselves are abstract in 5.5: instancing them is fatal,
        // so an abstract class is reported instead of leaving a null notify behind.
        if (NotifyClass->HasAnyClassFlags(CLASS_Abstract))
        {
            OutError.Set(EUnrealMCPAnimError::InvalidParams,
                         FString::Printf(TEXT("class '%s' is abstract and cannot be instanced; pass a concrete subclass or omit notify_class"),
                                         *NotifyClass->GetName()));
            return false;
        }
    }

    Sequence->Modify();
    Sequence->InitializeNotifyTrack();

    FAnimNotifyEvent& NewNotify = Sequence->Notifies.AddDefaulted_GetRef();
    const FName FinalName = NotifyName.IsEmpty()
        ? (NotifyClass ? FName(*NotifyClass->GetName()) : FName(TEXT("Notify")))
        : FName(*NotifyName);
    NewNotify.NotifyName = FinalName;
    NewNotify.Link(Sequence, Time);
    NewNotify.TriggerTimeOffset = GetTriggerTimeOffsetForType(Sequence->CalculateOffsetForNotify(Time));
    NewNotify.TrackIndex = TrackIndex > 0 && Sequence->AnimNotifyTracks.IsValidIndex(TrackIndex) ? TrackIndex : 0;
    NewNotify.Guid = FGuid::NewGuid();
    NewNotify.NotifyColor = FColor::White;

    if (NotifyClass)
    {
        if (bState)
        {
            NewNotify.NotifyStateClass = NewObject<UAnimNotifyState>(Sequence, NotifyClass, FinalName, RF_Transactional);
        }
        else
        {
            NewNotify.Notify = NewObject<UAnimNotify>(Sequence, NotifyClass, FinalName, RF_Transactional);
        }
    }

    if (bState)
    {
        NewNotify.SetDuration(FMath::Max(0.0f, Duration));
    }

    const FGuid AssignedGuid = NewNotify.Guid;
    Sequence->RefreshCacheData();
    Sequence->MarkPackageDirty();

    OutNotifyIndex = FindNotifyIndexByGuid(Sequence, AssignedGuid);
    if (OutNotifyIndex == INDEX_NONE)
    {
        OutError.Set(EUnrealMCPAnimError::WriteFailed,
                     FString::Printf(TEXT("the notify was added but could not be located again on '%s'"), *Sequence->GetName()));
        return false;
    }

    return true;
}

bool FUnrealMCPAnimationOps::RemoveNotify(UAnimSequence* Sequence, int32 NotifyIndex, FUnrealMCPAnimError& OutError)
{
    if (!Sequence || !Sequence->Notifies.IsValidIndex(NotifyIndex))
    {
        OutError.Set(EUnrealMCPAnimError::NotifyNotFound,
                     FString::Printf(TEXT("notify_index %d is out of range"), NotifyIndex));
        return false;
    }

    Sequence->Modify();
    Sequence->Notifies.RemoveAt(NotifyIndex);
    Sequence->RefreshCacheData();
    Sequence->MarkPackageDirty();
    return true;
}

const TArray<FString>& FUnrealMCPAnimationOps::NotifyPropertyNames()
{
    static const TArray<FString> Names = {
        TEXT("trigger_time"), TEXT("duration"), TEXT("track_index"), TEXT("name"), TEXT("color"),
        TEXT("trigger_chance"), TEXT("trigger_on_server"), TEXT("trigger_on_follower"),
        TEXT("trigger_weight_threshold"), TEXT("lod_filter")};
    return Names;
}

bool FUnrealMCPAnimationOps::SetNotifyProperty(UAnimSequence* Sequence, int32 NotifyIndex, const FString& PropertyName,
                                               const TSharedPtr<FJsonValue>& Value, FUnrealMCPAnimError& OutError)
{
    if (!Sequence || !Sequence->Notifies.IsValidIndex(NotifyIndex))
    {
        OutError.Set(EUnrealMCPAnimError::NotifyNotFound,
                     FString::Printf(TEXT("notify_index %d is out of range"), NotifyIndex));
        return false;
    }

    FAnimNotifyEvent& Notify = Sequence->Notifies[NotifyIndex];
    const FString Property = PropertyName.ToLower();

    if (Property == TEXT("trigger_time"))
    {
        double NewTime = 0.0;
        if (!Value.IsValid() || !Value->TryGetNumber(NewTime))
        {
            OutError.Set(EUnrealMCPAnimError::InvalidValue, TEXT("trigger_time needs a number (seconds)"));
            return false;
        }

        Sequence->Modify();
        Notify.Link(Sequence, static_cast<float>(NewTime));
        Notify.TriggerTimeOffset = GetTriggerTimeOffsetForType(Sequence->CalculateOffsetForNotify(static_cast<float>(NewTime)));
        Sequence->RefreshCacheData();
        Sequence->MarkPackageDirty();
        return true;
    }

    if (Property == TEXT("duration"))
    {
        double NewDuration = 0.0;
        if (!Value.IsValid() || !Value->TryGetNumber(NewDuration))
        {
            OutError.Set(EUnrealMCPAnimError::InvalidValue, TEXT("duration needs a number (seconds)"));
            return false;
        }
        if (!Notify.NotifyStateClass)
        {
            OutError.Set(EUnrealMCPAnimError::NotANotifyState,
                         FString::Printf(TEXT("notify %d ('%s') is an instant notify: only notify states have a duration"),
                                         NotifyIndex, *Notify.NotifyName.ToString()));
            return false;
        }

        Sequence->Modify();
        Notify.SetDuration(FMath::Max(0.0f, static_cast<float>(NewDuration)));
        Sequence->RefreshCacheData();
        Sequence->MarkPackageDirty();
        return true;
    }

    if (Property == TEXT("track_index"))
    {
        int32 NewTrackIndex = 0;
        if (!Value.IsValid() || !Value->TryGetNumber(NewTrackIndex))
        {
            OutError.Set(EUnrealMCPAnimError::InvalidValue, TEXT("track_index needs an integer"));
            return false;
        }
        if (!Sequence->AnimNotifyTracks.IsValidIndex(NewTrackIndex))
        {
            OutError.Set(EUnrealMCPAnimError::NotifyTrackNotFound,
                         FString::Printf(TEXT("track_index %d is out of range (%d track(s))"),
                                         NewTrackIndex, Sequence->AnimNotifyTracks.Num()));
            for (int32 Index = 0; Index < Sequence->AnimNotifyTracks.Num(); ++Index)
            {
                OutError.Candidates.Add(FString::Printf(TEXT("%d: %s"), Index,
                                                        *Sequence->AnimNotifyTracks[Index].TrackName.ToString()));
            }
            return false;
        }

        Sequence->Modify();
        Notify.TrackIndex = NewTrackIndex;
        // Moving a notify between tracks re-points it inside the track's Notifies array.
        Sequence->RefreshCacheData();
        Sequence->MarkPackageDirty();
        return true;
    }

    if (Property == TEXT("name"))
    {
        FString NewName;
        if (!Value.IsValid() || !Value->TryGetString(NewName) || NewName.IsEmpty())
        {
            OutError.Set(EUnrealMCPAnimError::InvalidValue, TEXT("name needs a non-empty string"));
            return false;
        }

        Sequence->Modify();
        Notify.NotifyName = FName(*NewName);
        Sequence->RefreshCacheData();
        Sequence->MarkPackageDirty();
        return true;
    }

    if (Property == TEXT("color"))
    {
        FLinearColor NewColor;
        FString ColorError;
        if (!ParseColorValue(Value, NewColor, ColorError))
        {
            OutError.Set(EUnrealMCPAnimError::InvalidValue, ColorError);
            return false;
        }

        Sequence->Modify();
        Notify.NotifyColor = NewColor.ToFColor(true);
        Sequence->MarkPackageDirty();
        return true;
    }

    if (Property == TEXT("trigger_chance"))
    {
        double NewChance = 1.0;
        if (!Value.IsValid() || !Value->TryGetNumber(NewChance))
        {
            OutError.Set(EUnrealMCPAnimError::InvalidValue, TEXT("trigger_chance needs a number in [0,1]"));
            return false;
        }

        Sequence->Modify();
        Notify.NotifyTriggerChance = FMath::Clamp(static_cast<float>(NewChance), 0.0f, 1.0f);
        Sequence->MarkPackageDirty();
        return true;
    }

    if (Property == TEXT("trigger_on_server") || Property == TEXT("trigger_on_follower"))
    {
        bool bFlag = false;
        if (!Value.IsValid() || !Value->TryGetBool(bFlag))
        {
            OutError.Set(EUnrealMCPAnimError::InvalidValue,
                         FString::Printf(TEXT("%s needs a boolean"), *Property));
            return false;
        }

        Sequence->Modify();
        if (Property == TEXT("trigger_on_server"))
        {
            Notify.bTriggerOnDedicatedServer = bFlag;
        }
        else
        {
            Notify.bTriggerOnFollower = bFlag;
        }
        Sequence->MarkPackageDirty();
        return true;
    }

    if (Property == TEXT("trigger_weight_threshold"))
    {
        double NewThreshold = 0.0;
        if (!Value.IsValid() || !Value->TryGetNumber(NewThreshold))
        {
            OutError.Set(EUnrealMCPAnimError::InvalidValue, TEXT("trigger_weight_threshold needs a number in [0,1]"));
            return false;
        }

        Sequence->Modify();
        Notify.TriggerWeightThreshold = FMath::Clamp(static_cast<float>(NewThreshold), 0.0f, 1.0f);
        Sequence->MarkPackageDirty();
        return true;
    }

    if (Property == TEXT("lod_filter"))
    {
        FString FilterText;
        int32 FilterLOD = Notify.NotifyFilterLOD;

        if (Value.IsValid() && Value->Type == EJson::String)
        {
            FilterText = Value->AsString();
        }
        else if (Value.IsValid() && Value->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject>& Object = Value->AsObject();
            Object->TryGetStringField(TEXT("type"), FilterText);
            Object->TryGetNumberField(TEXT("lod"), FilterLOD);
        }
        else
        {
            OutError.Set(EUnrealMCPAnimError::InvalidValue,
                         TEXT("lod_filter needs a string ('none'/'lod') or an object {type, lod}"));
            return false;
        }

        int32 FilterType = 0;
        if (!StringToFilterType(FilterText, FilterType))
        {
            OutError.Set(EUnrealMCPAnimError::UnknownValue,
                         FString::Printf(TEXT("unknown filter type '%s'"), *FilterText));
            OutError.Candidates = { TEXT("none"), TEXT("no_filtering"), TEXT("lod") };
            return false;
        }

        Sequence->Modify();
        Notify.NotifyFilterType = static_cast<ENotifyFilterType::Type>(FilterType);
        Notify.NotifyFilterLOD = FMath::Max(0, FilterLOD);
        Sequence->MarkPackageDirty();
        return true;
    }

    OutError.Set(EUnrealMCPAnimError::UnsupportedProperty,
                 FString::Printf(TEXT("'%s' is not a writable notify field"), *PropertyName));
    OutError.AvailableFields = NotifyPropertyNames();
    return false;
}

void FUnrealMCPAnimationOps::ListNotifyTracks(UAnimSequence* Sequence, TArray<FUnrealMCPAnimNotifyTrackInfo>& OutTracks)
{
    OutTracks.Reset();
    if (!Sequence)
    {
        return;
    }

    for (int32 TrackIndex = 0; TrackIndex < Sequence->AnimNotifyTracks.Num(); ++TrackIndex)
    {
        const FAnimNotifyTrack& Track = Sequence->AnimNotifyTracks[TrackIndex];

        FUnrealMCPAnimNotifyTrackInfo Info;
        Info.TrackIndex = TrackIndex;
        Info.TrackName = Track.TrackName.ToString();
        Info.Color = ColorToHex(Track.TrackColor);
        // Counted from the notify array instead of Track.Notifies: those pointers are only rebuilt
        // by RefreshCacheData, and a stale count would be worse than a slow one.
        for (const FAnimNotifyEvent& Notify : Sequence->Notifies)
        {
            if (Notify.TrackIndex == TrackIndex)
            {
                ++Info.NotifyCount;
            }
        }
        Info.SyncMarkerCount = Track.SyncMarkers.Num();
        OutTracks.Add(Info);
    }
}

bool FUnrealMCPAnimationOps::ResolveNotifyTrack(UAnimSequence* Sequence, int32 TrackIndex, const FString& TrackName,
                                                int32& OutTrackIndex, FUnrealMCPAnimError& OutError)
{
    OutTrackIndex = INDEX_NONE;
    if (!Sequence)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("sequence is required"));
        return false;
    }

    if (!TrackName.IsEmpty())
    {
        TArray<int32> Matches;
        for (int32 Index = 0; Index < Sequence->AnimNotifyTracks.Num(); ++Index)
        {
            if (Sequence->AnimNotifyTracks[Index].TrackName.ToString().Equals(TrackName, ESearchCase::IgnoreCase))
            {
                Matches.Add(Index);
            }
        }

        if (Matches.Num() == 1)
        {
            OutTrackIndex = Matches[0];
            return true;
        }

        TArray<FString>& Candidates = OutError.Candidates;
        for (int32 Index = 0; Index < Sequence->AnimNotifyTracks.Num(); ++Index)
        {
            Candidates.Add(FString::Printf(TEXT("%d: %s"), Index,
                                           *Sequence->AnimNotifyTracks[Index].TrackName.ToString()));
        }

        const TCHAR* Code = Matches.Num() == 0 ? EUnrealMCPAnimError::NotifyTrackNotFound
                                               : EUnrealMCPAnimError::AmbiguousNotify;
        OutError.Set(Code, Matches.Num() == 0
            ? FString::Printf(TEXT("no notify track named '%s' on '%s'"), *TrackName, *Sequence->GetName())
            : FString::Printf(TEXT("'%s' matches %d notify tracks"), *TrackName, Matches.Num()));
        return false;
    }

    if (!Sequence->AnimNotifyTracks.IsValidIndex(TrackIndex))
    {
        OutError.Set(EUnrealMCPAnimError::NotifyTrackNotFound,
                     FString::Printf(TEXT("track_index %d is out of range (%d track(s))"),
                                     TrackIndex, Sequence->AnimNotifyTracks.Num()));
        for (int32 Index = 0; Index < Sequence->AnimNotifyTracks.Num(); ++Index)
        {
            OutError.Candidates.Add(FString::Printf(TEXT("%d: %s"), Index,
                                                    *Sequence->AnimNotifyTracks[Index].TrackName.ToString()));
        }
        return false;
    }

    OutTrackIndex = TrackIndex;
    return true;
}

bool FUnrealMCPAnimationOps::AddNotifyTrack(UAnimSequence* Sequence, const FString& TrackName,
                                            const FLinearColor& Color, int32& OutTrackIndex,
                                            FUnrealMCPAnimError& OutError)
{
    OutTrackIndex = INDEX_NONE;
    if (!Sequence)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }

    // The engine names the tracks it creates after their number; a caller-supplied name must not
    // collide with an existing one, otherwise lookups by name become ambiguous.
    const FName WantedName = TrackName.IsEmpty()
        ? FName(*FString::FromInt(Sequence->AnimNotifyTracks.Num() + 1))
        : FName(*TrackName);
    for (const FAnimNotifyTrack& Track : Sequence->AnimNotifyTracks)
    {
        if (Track.TrackName == WantedName)
        {
            OutError.Set(EUnrealMCPAnimError::NotifyTrackNameTaken,
                         FString::Printf(TEXT("notify track '%s' already exists"), *WantedName.ToString()));
            return false;
        }
    }

    Sequence->Modify();
    OutTrackIndex = Sequence->AnimNotifyTracks.Add(FAnimNotifyTrack(WantedName, Color));
    Sequence->MarkPackageDirty();
    return true;
}

bool FUnrealMCPAnimationOps::RenameNotifyTrack(UAnimSequence* Sequence, int32 TrackIndex, const FString& NewName,
                                               FUnrealMCPAnimError& OutError)
{
    if (!Sequence || !Sequence->AnimNotifyTracks.IsValidIndex(TrackIndex))
    {
        OutError.Set(EUnrealMCPAnimError::NotifyTrackNotFound,
                     FString::Printf(TEXT("track_index %d is out of range"), TrackIndex));
        return false;
    }
    if (NewName.IsEmpty())
    {
        OutError.Set(EUnrealMCPAnimError::InvalidValue, TEXT("name must not be empty"));
        return false;
    }

    const FName WantedName(*NewName);
    for (int32 Index = 0; Index < Sequence->AnimNotifyTracks.Num(); ++Index)
    {
        if (Index != TrackIndex && Sequence->AnimNotifyTracks[Index].TrackName == WantedName)
        {
            OutError.Set(EUnrealMCPAnimError::NotifyTrackNameTaken,
                         FString::Printf(TEXT("notify track '%s' already exists"), *NewName));
            return false;
        }
    }

    Sequence->Modify();
    // 5.5 stores track names (5.7+ made them implicit indices), so renaming is a real write.
    Sequence->AnimNotifyTracks[TrackIndex].TrackName = WantedName;
    Sequence->MarkPackageDirty();
    return true;
}

bool FUnrealMCPAnimationOps::RemoveNotifyTrack(UAnimSequence* Sequence, int32 TrackIndex, FUnrealMCPAnimError& OutError)
{
    if (!Sequence || !Sequence->AnimNotifyTracks.IsValidIndex(TrackIndex))
    {
        OutError.Set(EUnrealMCPAnimError::NotifyTrackNotFound,
                     FString::Printf(TEXT("track_index %d is out of range"), TrackIndex));
        return false;
    }

    // A track that still carries notifies is rebuilt by RefreshCacheData the moment it is dropped,
    // so removing the last one would silently undo the removal.
    int32 NotifiesOnTrack = 0;
    for (const FAnimNotifyEvent& Notify : Sequence->Notifies)
    {
        if (Notify.TrackIndex == TrackIndex)
        {
            ++NotifiesOnTrack;
        }
    }
    if (Sequence->AnimNotifyTracks.Num() <= 1 && NotifiesOnTrack > 0)
    {
        OutError.Set(EUnrealMCPAnimError::LastNotifyTrack,
                     FString::Printf(TEXT("track %d still holds %d notify(ies) and is the only track: rebuild the notifies onto another track first"),
                                     TrackIndex, NotifiesOnTrack));
        return false;
    }

    Sequence->Modify();
    Sequence->AnimNotifyTracks.RemoveAt(TrackIndex);
    for (FAnimNotifyEvent& Notify : Sequence->Notifies)
    {
        if (Notify.TrackIndex == TrackIndex)
        {
            Notify.TrackIndex = 0;
        }
        else if (Notify.TrackIndex > TrackIndex)
        {
            --Notify.TrackIndex;
        }
    }
    Sequence->RefreshCacheData();
    Sequence->MarkPackageDirty();
    return true;
}

// ---------------------------------------------------------------------------
// sync markers
// ---------------------------------------------------------------------------

void FUnrealMCPAnimationOps::ListSyncMarkers(UAnimSequence* Sequence, TArray<FUnrealMCPAnimSyncMarkerInfo>& OutMarkers)
{
    OutMarkers.Reset();
    if (!Sequence)
    {
        return;
    }

    OutMarkers.Reserve(Sequence->AuthoredSyncMarkers.Num());
    for (const FAnimSyncMarker& Marker : Sequence->AuthoredSyncMarkers)
    {
        FUnrealMCPAnimSyncMarkerInfo Info;
        Info.MarkerName = Marker.MarkerName.ToString();
        Info.Time = Marker.Time;
        Info.TrackIndex = Marker.TrackIndex;
        OutMarkers.Add(Info);
    }
}

bool FUnrealMCPAnimationOps::AddSyncMarker(UAnimSequence* Sequence, const FString& MarkerName, float Time,
                                           int32& OutMarkerIndex, FUnrealMCPAnimError& OutError)
{
    OutMarkerIndex = INDEX_NONE;
    if (!Sequence)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }
    if (MarkerName.IsEmpty())
    {
        OutError.Set(EUnrealMCPAnimError::InvalidValue, TEXT("marker_name is required"));
        return false;
    }

    Sequence->Modify();

    FAnimSyncMarker Marker;
    Marker.MarkerName = FName(*MarkerName);
    Marker.Time = Time;
    Marker.TrackIndex = 0;
    Sequence->AuthoredSyncMarkers.Add(Marker);

    // Sorted by time and pushed into the runtime marker table, or the runtime keeps the old order.
    Sequence->SortSyncMarkers();
    Sequence->RefreshSyncMarkerDataFromAuthored();
    Sequence->MarkPackageDirty();

    for (int32 Index = 0; Index < Sequence->AuthoredSyncMarkers.Num(); ++Index)
    {
        const FAnimSyncMarker& Candidate = Sequence->AuthoredSyncMarkers[Index];
        if (Candidate.MarkerName == Marker.MarkerName && FMath::IsNearlyEqual(Candidate.Time, Time, TimeTolerance))
        {
            OutMarkerIndex = Index;
            break;
        }
    }

    if (OutMarkerIndex == INDEX_NONE)
    {
        OutError.Set(EUnrealMCPAnimError::WriteFailed,
                     FString::Printf(TEXT("the sync marker was added but could not be located again on '%s'"), *Sequence->GetName()));
        return false;
    }

    return true;
}

bool FUnrealMCPAnimationOps::ResolveSyncMarker(UAnimSequence* Sequence, const FString& MarkerName, float Time,
                                               bool bHasTime, int32 MarkerIndex, int32& OutMarkerIndex,
                                               FUnrealMCPAnimError& OutError)
{
    OutMarkerIndex = INDEX_NONE;
    if (!Sequence)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("sequence is required"));
        return false;
    }

    TArray<FString>& Candidates = OutError.Candidates;
    for (int32 Index = 0; Index < Sequence->AuthoredSyncMarkers.Num(); ++Index)
    {
        const FAnimSyncMarker& Marker = Sequence->AuthoredSyncMarkers[Index];
        Candidates.Add(FString::Printf(TEXT("%d: %s @ %.4fs"), Index, *Marker.MarkerName.ToString(), Marker.Time));
    }

    if (!MarkerName.IsEmpty())
    {
        TArray<int32> Matches;
        for (int32 Index = 0; Index < Sequence->AuthoredSyncMarkers.Num(); ++Index)
        {
            const FAnimSyncMarker& Marker = Sequence->AuthoredSyncMarkers[Index];
            if (!Marker.MarkerName.ToString().Equals(MarkerName, ESearchCase::IgnoreCase))
            {
                continue;
            }
            if (bHasTime && !FMath::IsNearlyEqual(Marker.Time, Time, TimeTolerance))
            {
                continue;
            }
            Matches.Add(Index);
        }

        if (Matches.Num() == 1)
        {
            OutMarkerIndex = Matches[0];
            return true;
        }

        OutError.Set(Matches.Num() == 0 ? EUnrealMCPAnimError::SyncMarkerNotFound : EUnrealMCPAnimError::AmbiguousNotify,
                     Matches.Num() == 0
                         ? FString::Printf(TEXT("no sync marker named '%s'%s on '%s'"), *MarkerName,
                                           bHasTime ? *FString::Printf(TEXT(" at %.4fs"), Time) : TEXT(""),
                                           *Sequence->GetName())
                         : FString::Printf(TEXT("'%s' matches %d sync markers; pass marker_index or 'time'"),
                                           *MarkerName, Matches.Num()));
        return false;
    }

    if (!Sequence->AuthoredSyncMarkers.IsValidIndex(MarkerIndex))
    {
        OutError.Set(EUnrealMCPAnimError::SyncMarkerNotFound,
                     FString::Printf(TEXT("marker_index %d is out of range (%d marker(s))"),
                                     MarkerIndex, Sequence->AuthoredSyncMarkers.Num()));
        return false;
    }

    OutMarkerIndex = MarkerIndex;
    return true;
}

bool FUnrealMCPAnimationOps::RemoveSyncMarker(UAnimSequence* Sequence, int32 MarkerIndex, FUnrealMCPAnimError& OutError)
{
    if (!Sequence || !Sequence->AuthoredSyncMarkers.IsValidIndex(MarkerIndex))
    {
        OutError.Set(EUnrealMCPAnimError::SyncMarkerNotFound,
                     FString::Printf(TEXT("marker_index %d is out of range"), MarkerIndex));
        return false;
    }

    Sequence->Modify();
    Sequence->AuthoredSyncMarkers.RemoveAt(MarkerIndex);
    Sequence->RefreshSyncMarkerDataFromAuthored();
    Sequence->MarkPackageDirty();
    return true;
}

bool FUnrealMCPAnimationOps::SetSyncMarkerTime(UAnimSequence* Sequence, int32 MarkerIndex, float NewTime,
                                               FUnrealMCPAnimError& OutError)
{
    if (!Sequence || !Sequence->AuthoredSyncMarkers.IsValidIndex(MarkerIndex))
    {
        OutError.Set(EUnrealMCPAnimError::SyncMarkerNotFound,
                     FString::Printf(TEXT("marker_index %d is out of range"), MarkerIndex));
        return false;
    }

    Sequence->Modify();
    Sequence->AuthoredSyncMarkers[MarkerIndex].Time = NewTime;
    Sequence->SortSyncMarkers();
    Sequence->RefreshSyncMarkerDataFromAuthored();
    Sequence->MarkPackageDirty();
    return true;
}

// ---------------------------------------------------------------------------
// creation and configuration
// ---------------------------------------------------------------------------

bool FUnrealMCPAnimationOps::CreateSequence(const FString& Name, const FString& Folder, const FString& SkeletonPath,
                                            float FrameRate, float Duration, bool bFromReferencePose,
                                            FString& OutAssetPath, FUnrealMCPAnimError& OutError)
{
    OutAssetPath.Reset();

    if (Name.IsEmpty())
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("name is required"));
        return false;
    }

    USkeleton* Skeleton = nullptr;
    if (!ResolveSkeleton(SkeletonPath, Skeleton, OutError))
    {
        return false;
    }

    const FString AssetFolder = Folder.IsEmpty() ? TEXT("/Game") : Folder;
    const FString PackagePath = AssetFolder + TEXT("/") + Name;
    if (UObject* Existing = FUnrealMCPCommonUtils::FindAsset(PackagePath))
    {
        // A previously deleted asset can still be in memory: when it was never saved, the delete has
        // no file to remove and the object stays behind. That leftover must not block a fresh create
        // with a name conflict - only a real asset does.
        const bool bLeftover = !IsValid(Existing) || Existing->IsUnreachable();
        if (bLeftover)
        {
            Existing->MarkAsGarbage();
            CollectGarbage(RF_NoFlags);
        }
        else
        {
            OutError.Set(EUnrealMCPAnimError::AssetExists,
                         FString::Printf(TEXT("'%s' already exists; delete it (safe_delete_asset) or pick another name"), *PackagePath));
            return false;
        }
    }

    UAnimSequenceFactory* Factory = NewObject<UAnimSequenceFactory>();
    Factory->TargetSkeleton = Skeleton;

    // Not IAssetTools::CreateAsset: its CanCreateAsset step fully loads the target package, which
    // never returns for a path whose file was deleted in this session.
    UAnimSequence* NewSequence = Cast<UAnimSequence>(
        FUnrealMCPCommonUtils::CreateAssetDirect(Name, AssetFolder, UAnimSequence::StaticClass(), Factory));
    if (!NewSequence)
    {
        OutError.Set(EUnrealMCPAnimError::CreateFailed,
                     FString::Printf(TEXT("the asset factory did not produce a sequence at '%s'"), *PackagePath));
        return false;
    }

    const FFrameRate Rate(FMath::Max(1, FMath::RoundToInt32(FrameRate)), 1);
    const int32 NumFrames = FMath::Max(1, FMath::RoundToInt(Duration * Rate.AsDecimal()));

    IAnimationDataController& Controller = NewSequence->GetController();
    Controller.OpenBracket(FText::FromString(TEXT("UnrealMCP create animation sequence")));
    // Same reason as set_animation_frame_rate: a frame rate change only takes effect when the
    // controller is allowed to transact, so the requested rate is not applied otherwise.
    Controller.SetFrameRate(Rate);
    Controller.SetNumberOfFrames(FFrameNumber(NumFrames));

    if (bFromReferencePose)
    {
        const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
        const TArray<FTransform>& RefPose = RefSkeleton.GetRefBonePose();

        TArray<FName> ExistingTracks;
        const IAnimationDataModel* Model = NewSequence->GetDataModel();
        if (Model)
        {
            Model->GetBoneTrackNames(ExistingTracks);
        }

        for (int32 BoneIndex = 0; BoneIndex < RefSkeleton.GetNum(); ++BoneIndex)
        {
            const FName BoneName = RefSkeleton.GetBoneName(BoneIndex);
            if (!ExistingTracks.Contains(BoneName))
            {
                Controller.AddBoneCurve(BoneName);
            }

            const FTransform& BoneTransform = RefPose.IsValidIndex(BoneIndex) ? RefPose[BoneIndex] : FTransform::Identity;
            const FVector3f Position(BoneTransform.GetLocation());
            const FQuat4f Rotation(BoneTransform.GetRotation());
            const FVector3f Scale(BoneTransform.GetScale3D());

            TArray<FVector3f> Positions;
            TArray<FQuat4f> Rotations;
            TArray<FVector3f> Scales;
            Positions.Init(Position, NumFrames);
            Rotations.Init(Rotation, NumFrames);
            Scales.Init(Scale, NumFrames);

            Controller.SetBoneTrackKeys(BoneName, Positions, Rotations, Scales);
        }
    }

    Controller.CloseBracket();
    NewSequence->MarkPackageDirty();

    OutAssetPath = NewSequence->GetPathName();
    return true;
}

bool FUnrealMCPAnimationOps::SetFrameRate(UAnimSequence* Sequence, float FrameRate, float& OutReadback,
                                          FUnrealMCPAnimError& OutError)
{
    OutReadback = 0.0f;
    if (!Sequence)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }
    if (FrameRate <= 0.0f)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidValue,
                     FString::Printf(TEXT("frame_rate must be > 0, got %f"), FrameRate));
        return false;
    }

    // The data controller refuses a rate that is neither a multiple nor a factor of the current one,
    // and it reports that through an engine error log rather than a return value - which the editor
    // python loopback turns into an exception. Rejecting it here keeps the failure structured.
    const FFrameRate NewRate(FMath::Max(1, FMath::RoundToInt32(FrameRate)), 1);
    const FFrameRate CurrentRate = GetSequenceFrameRate(Sequence);
    const IAnimationDataModel* Model = Sequence->GetDataModel();
    const bool bPopulated = Model ? Model->HasBeenPopulated() : true;

    if (bPopulated && !NewRate.IsMultipleOf(CurrentRate) && !NewRate.IsFactorOf(CurrentRate))
    {
        OutError.Set(EUnrealMCPAnimError::IncompatibleFrameRate,
                     FString::Printf(TEXT("%s is not a multiple or a factor of the current %s"),
                                     *FString::SanitizeFloat(NewRate.AsDecimal()),
                                     *FString::SanitizeFloat(CurrentRate.AsDecimal())));
        const double Current = CurrentRate.AsDecimal();
        const double Candidates[] = { Current, Current * 2.0, Current * 4.0, Current / 2.0, Current / 4.0 };
        for (const double Candidate : Candidates)
        {
            if (Candidate > 0.0 && !FMath::IsNearlyEqual(Candidate, NewRate.AsDecimal()))
            {
                OutError.Candidates.Add(FString::SanitizeFloat(Candidate));
            }
        }
        return false;
    }

    Sequence->Modify();
    IAnimationDataController& Controller = Sequence->GetController();
    // The controller applies a frame rate change through its own model action, which is skipped when
    // it is told not to transact - the change silently does not happen. The command layer already
    // owns the user-visible undo step, so the controller keeps its own (nested) transaction.
    Controller.SetFrameRate(FFrameRate(FMath::Max(1, FMath::RoundToInt32(FrameRate)), 1));
    Sequence->MarkPackageDirty();

    OutReadback = GetSequenceFrameRate(Sequence).AsDecimal();
    return true;
}

bool FUnrealMCPAnimationOps::SetRateScale(UAnimSequence* Sequence, float RateScale, float& OutReadback,
                                          FUnrealMCPAnimError& OutError)
{
    OutReadback = 0.0f;
    if (!Sequence)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }

    Sequence->Modify();
    Sequence->RateScale = RateScale;
    Sequence->MarkPackageDirty();

    OutReadback = Sequence->RateScale;
    return true;
}

namespace
{
    bool HasBoneTrack(const UAnimSequence* Sequence, const FName& BoneName)
    {
        const IAnimationDataModel* Model = Sequence ? Sequence->GetDataModel() : nullptr;
        if (!Model)
        {
            return false;
        }

        TArray<FName> Names;
        Model->GetBoneTrackNames(Names);
        return Names.Contains(BoneName);
    }
}

bool FUnrealMCPAnimationOps::AddBoneTrack(UAnimSequence* Sequence, const FString& BoneName, FUnrealMCPAnimError& OutError)
{
    int32 BoneIndex = INDEX_NONE;
    if (!ResolveBoneIndex(Sequence, BoneName, BoneIndex, OutError))
    {
        return false;
    }

    const FName BoneFName(*BoneName);
    if (HasBoneTrack(Sequence, BoneFName))
    {
        OutError.Set(EUnrealMCPAnimError::BoneTrackExists,
                     FString::Printf(TEXT("bone '%s' already has a track on '%s'"), *BoneName, *Sequence->GetName()));
        return false;
    }

    Sequence->Modify();
    IAnimationDataController& Controller = Sequence->GetController();
    if (!Controller.AddBoneCurve(BoneFName))
    {
        OutError.Set(EUnrealMCPAnimError::WriteFailed,
                     FString::Printf(TEXT("AddBoneCurve('%s') was refused by the data controller"), *BoneName));
        return false;
    }

    Sequence->MarkPackageDirty();
    return true;
}

bool FUnrealMCPAnimationOps::RemoveBoneTrack(UAnimSequence* Sequence, const FString& BoneName, FUnrealMCPAnimError& OutError)
{
    const FName BoneFName(*BoneName);
    if (!Sequence || !HasBoneTrack(Sequence, BoneFName))
    {
        OutError.Set(EUnrealMCPAnimError::BoneTrackNotFound,
                     FString::Printf(TEXT("bone '%s' has no track on '%s'"), *BoneName, *GetNameSafe(Sequence)));
        return false;
    }

    Sequence->Modify();
    IAnimationDataController& Controller = Sequence->GetController();
    if (!Controller.RemoveBoneTrack(BoneFName))
    {
        OutError.Set(EUnrealMCPAnimError::WriteFailed,
                     FString::Printf(TEXT("RemoveBoneTrack('%s') was refused by the data controller"), *BoneName));
        return false;
    }

    Sequence->MarkPackageDirty();
    return true;
}

bool FUnrealMCPAnimationOps::SetBoneTrackKeys(UAnimSequence* Sequence, const FString& BoneName,
                                              const TArray<FUnrealMCPAnimKeyframe>& Keys, bool bBakeEveryFrame,
                                              FUnrealMCPAnimError& OutError)
{
    int32 BoneIndex = INDEX_NONE;
    if (!ResolveBoneIndex(Sequence, BoneName, BoneIndex, OutError))
    {
        return false;
    }
    if (Keys.Num() == 0)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidValue, TEXT("keys must hold at least one key"));
        return false;
    }

    TArray<FUnrealMCPAnimKeyframe> SortedKeys = Keys;
    SortedKeys.Sort([](const FUnrealMCPAnimKeyframe& A, const FUnrealMCPAnimKeyframe& B) { return A.Time < B.Time; });

    TArray<FVector3f> Positions;
    TArray<FQuat4f> Rotations;
    TArray<FVector3f> Scales;

    if (bBakeEveryFrame)
    {
        // The sequence editor stores one key per frame: resample the authored keys onto the frame
        // grid so the written track evaluates exactly like it does in Persona.
        const FFrameRate Rate = GetSequenceFrameRate(Sequence);
        // One key per frame plus the T0 key, which is what the data model stores.
        const int32 NumFrames = FMath::Max(1, GetSequenceFrameCount(Sequence) + 1);
        Positions.Reserve(NumFrames);
        Rotations.Reserve(NumFrames);
        Scales.Reserve(NumFrames);

        for (int32 FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
        {
            const float FrameTime = static_cast<float>(Rate.AsSeconds(FrameIndex));

            int32 NextIndex = 0;
            while (NextIndex < SortedKeys.Num() && SortedKeys[NextIndex].Time < FrameTime)
            {
                ++NextIndex;
            }

            if (NextIndex == 0)
            {
                Positions.Add(FVector3f(SortedKeys[0].Position));
                Rotations.Add(FQuat4f(SortedKeys[0].Rotation));
                Scales.Add(FVector3f(SortedKeys[0].Scale));
                continue;
            }
            if (NextIndex >= SortedKeys.Num())
            {
                const FUnrealMCPAnimKeyframe& Last = SortedKeys.Last();
                Positions.Add(FVector3f(Last.Position));
                Rotations.Add(FQuat4f(Last.Rotation));
                Scales.Add(FVector3f(Last.Scale));
                continue;
            }

            const FUnrealMCPAnimKeyframe& Before = SortedKeys[NextIndex - 1];
            const FUnrealMCPAnimKeyframe& After = SortedKeys[NextIndex];
            const float Span = After.Time - Before.Time;
            const float Alpha = Span > KINDA_SMALL_NUMBER
                ? FMath::Clamp((FrameTime - Before.Time) / Span, 0.0f, 1.0f)
                : 0.0f;

            Positions.Add(FVector3f(FMath::Lerp(Before.Position, After.Position, Alpha)));
            Rotations.Add(FQuat4f(FQuat::Slerp(Before.Rotation, After.Rotation, Alpha)));
            Scales.Add(FVector3f(FMath::Lerp(Before.Scale, After.Scale, Alpha)));
        }
    }
    else
    {
        Positions.Reserve(SortedKeys.Num());
        Rotations.Reserve(SortedKeys.Num());
        Scales.Reserve(SortedKeys.Num());
        for (const FUnrealMCPAnimKeyframe& Key : SortedKeys)
        {
            Positions.Add(FVector3f(Key.Position));
            Rotations.Add(FQuat4f(Key.Rotation));
            Scales.Add(FVector3f(Key.Scale));
        }
    }

    const FName BoneFName(*BoneName);
    Sequence->Modify();

    IAnimationDataController& Controller = Sequence->GetController();
    Controller.OpenBracket(FText::FromString(TEXT("UnrealMCP set bone track keys")));

    if (!HasBoneTrack(Sequence, BoneFName))
    {
        Controller.AddBoneCurve(BoneFName);
    }

    const bool bWritten = Controller.SetBoneTrackKeys(BoneFName, Positions, Rotations, Scales);
    Controller.CloseBracket();

    if (!bWritten)
    {
        OutError.Set(EUnrealMCPAnimError::WriteFailed,
                     FString::Printf(TEXT("SetBoneTrackKeys('%s') was refused by the data controller"), *BoneName));
        return false;
    }

    Sequence->MarkPackageDirty();
    return true;
}

bool FUnrealMCPAnimationOps::SetAdditiveAnimType(UAnimSequence* Sequence, const FString& Type, FUnrealMCPAnimError& OutError)
{
    int32 AdditiveType = 0;
    if (!StringToAdditiveType(Type, AdditiveType))
    {
        OutError.Set(EUnrealMCPAnimError::UnknownValue,
                     FString::Printf(TEXT("unknown additive type '%s'"), *Type));
        OutError.Candidates = { TEXT("None"), TEXT("LocalSpace"), TEXT("MeshSpace") };
        return false;
    }

    if (!Sequence)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }

    Sequence->Modify();
    Sequence->AdditiveAnimType = static_cast<EAdditiveAnimationType>(AdditiveType);
    Sequence->MarkPackageDirty();
    return true;
}

bool FUnrealMCPAnimationOps::SetAdditiveBasePose(UAnimSequence* Sequence, const FString& BasePoseSequencePath,
                                                 FUnrealMCPAnimError& OutError)
{
    if (!Sequence)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }

    UAnimSequence* BasePose = nullptr;
    if (!BasePoseSequencePath.IsEmpty())
    {
        if (!ResolveSequence(BasePoseSequencePath, BasePose, OutError))
        {
            return false;
        }
    }

    Sequence->Modify();
    Sequence->RefPoseSeq = BasePose;
    Sequence->RefPoseType = BasePose ? EAdditiveBasePoseType::ABPT_AnimScaled : EAdditiveBasePoseType::ABPT_RefPose;
    Sequence->MarkPackageDirty();
    return true;
}

bool FUnrealMCPAnimationOps::SetEnableRootMotion(UAnimSequence* Sequence, bool bEnable, FUnrealMCPAnimError& OutError)
{
    if (!Sequence)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }

    Sequence->Modify();
    Sequence->bEnableRootMotion = bEnable;
    Sequence->MarkPackageDirty();
    return true;
}

bool FUnrealMCPAnimationOps::SetRootMotionRootLock(UAnimSequence* Sequence, const FString& LockType, FUnrealMCPAnimError& OutError)
{
    int32 Lock = 0;
    if (!StringToRootLock(LockType, Lock))
    {
        OutError.Set(EUnrealMCPAnimError::UnknownValue,
                     FString::Printf(TEXT("unknown root lock '%s'"), *LockType));
        OutError.Candidates = { TEXT("RefPose"), TEXT("AnimFirstFrame"), TEXT("Zero") };
        return false;
    }

    Sequence->Modify();
    Sequence->RootMotionRootLock = static_cast<ERootMotionRootLock::Type>(Lock);
    Sequence->MarkPackageDirty();
    return true;
}

bool FUnrealMCPAnimationOps::SetForceRootLock(UAnimSequence* Sequence, bool bForce, FUnrealMCPAnimError& OutError)
{
    if (!Sequence)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }

    Sequence->Modify();
    Sequence->bForceRootLock = bForce;
    Sequence->MarkPackageDirty();
    return true;
}

bool FUnrealMCPAnimationOps::SetCompressionScheme(UAnimSequence* Sequence, const FString& SettingsPath,
                                                  FString& OutReadback, FUnrealMCPAnimError& OutError)
{
    OutReadback.Reset();
    if (!Sequence)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }

    UAnimBoneCompressionSettings* Settings = Cast<UAnimBoneCompressionSettings>(
        FUnrealMCPCommonUtils::FindAsset(SettingsPath));
    if (!Settings)
    {
        OutError.Set(EUnrealMCPAnimError::AssetNotFound,
                     FString::Printf(TEXT("'%s' is not a UAnimBoneCompressionSettings asset"), *SettingsPath));
        return false;
    }

    Sequence->Modify();
    Sequence->BoneCompressionSettings = Settings;
    Sequence->MarkPackageDirty();

    OutReadback = Sequence->BoneCompressionSettings->GetPathName();
    return true;
}

bool FUnrealMCPAnimationOps::CompressSequence(UAnimSequence* Sequence, int32& OutCompressedSize, bool& bOutDataValid,
                                              FUnrealMCPAnimError& OutError)
{
    OutCompressedSize = 0;
    bOutDataValid = false;
    if (!Sequence)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }

    Sequence->Modify();

    // 5.5 dropped CompressAnimationData; the derived-data path is the supported one and it is
    // synchronous, so the command never reports "started" without the result.
    Sequence->CacheDerivedDataForCurrentPlatform();
    Sequence->WaitOnExistingCompression(true);

    bOutDataValid = Sequence->IsCompressedDataValid();
    OutCompressedSize = Sequence->GetApproxCompressedSize();
    Sequence->MarkPackageDirty();

    if (!bOutDataValid)
    {
        OutError.Set(EUnrealMCPAnimError::CompressionFailed,
                     FString::Printf(TEXT("'%s' still has stale compressed data after recompression (compressed_size=%d)"),
                                     *Sequence->GetName(), OutCompressedSize));
        OutError.AvailableFields = { TEXT("compressed_data_valid"), TEXT("compressed_size"), TEXT("raw_size") };
        return false;
    }

    return true;
}

void FUnrealMCPAnimationOps::FillCompressionInfo(UAnimSequence* Sequence, FUnrealMCPAnimCompressionInfo& OutInfo)
{
    OutInfo = FUnrealMCPAnimCompressionInfo();
    if (!Sequence)
    {
        return;
    }

    OutInfo.CompressionScheme = Sequence->BoneCompressionSettings
        ? Sequence->BoneCompressionSettings->GetPathName()
        : FString();
    OutInfo.CurveCompressionScheme = Sequence->CurveCompressionSettings
        ? Sequence->CurveCompressionSettings->GetPathName()
        : FString();
    OutInfo.RawSize = Sequence->GetApproxRawSize();
    OutInfo.CompressedSize = Sequence->GetApproxCompressedSize();
    OutInfo.Ratio = OutInfo.RawSize > 0
        ? static_cast<double>(OutInfo.CompressedSize) / static_cast<double>(OutInfo.RawSize)
        : 0.0;
}

bool FUnrealMCPAnimationOps::GetSourceFiles(UAnimSequence* Sequence, TArray<FString>& OutFiles,
                                            FUnrealMCPAnimError& OutError)
{
    OutFiles.Reset();
    if (!Sequence)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }

    if (!Sequence->AssetImportData)
    {
        return true;
    }

    Sequence->AssetImportData->ExtractFilenames(OutFiles);
    OutFiles.Sort();
    return true;
}

FString FUnrealMCPAnimationOps::ExportToJson(UAnimSequence* Sequence)
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    FUnrealMCPAnimSequenceInfo Info;
    FillSequenceInfo(Sequence, Info);
    Root->SetObjectField(TEXT("sequence"), SequenceInfoToJson(Info));

    TArray<FString> BoneNames;
    FUnrealMCPAnimError Ignored;
    if (GetBoneNames(Sequence, BoneNames, Ignored))
    {
        TArray<TSharedPtr<FJsonValue>> BoneValues;
        for (const FString& BoneName : BoneNames)
        {
            BoneValues.Add(MakeShared<FJsonValueString>(BoneName));
        }
        Root->SetArrayField(TEXT("bones"), BoneValues);
    }

    TArray<FUnrealMCPAnimCurveInfo> Curves;
    int32 TransformCurveCount = 0;
    if (ListCurves(Sequence, Curves, TransformCurveCount, Ignored))
    {
        TArray<TSharedPtr<FJsonValue>> CurveValues;
        for (const FUnrealMCPAnimCurveInfo& Curve : Curves)
        {
            CurveValues.Add(MakeShared<FJsonValueObject>(CurveInfoToJson(Curve)));
        }
        Root->SetArrayField(TEXT("curves"), CurveValues);
        Root->SetNumberField(TEXT("transform_curve_count"), TransformCurveCount);
    }

    TArray<FUnrealMCPAnimNotifyInfo> Notifies;
    ListNotifies(Sequence, Notifies);
    TArray<TSharedPtr<FJsonValue>> NotifyValues;
    for (const FUnrealMCPAnimNotifyInfo& Notify : Notifies)
    {
        NotifyValues.Add(MakeShared<FJsonValueObject>(NotifyToJson(Notify)));
    }
    Root->SetArrayField(TEXT("notifies"), NotifyValues);

    TArray<FUnrealMCPAnimNotifyTrackInfo> Tracks;
    ListNotifyTracks(Sequence, Tracks);
    TArray<TSharedPtr<FJsonValue>> TrackValues;
    for (const FUnrealMCPAnimNotifyTrackInfo& Track : Tracks)
    {
        TrackValues.Add(MakeShared<FJsonValueObject>(NotifyTrackToJson(Track)));
    }
    Root->SetArrayField(TEXT("notify_tracks"), TrackValues);

    TArray<FUnrealMCPAnimSyncMarkerInfo> Markers;
    ListSyncMarkers(Sequence, Markers);
    TArray<TSharedPtr<FJsonValue>> MarkerValues;
    for (const FUnrealMCPAnimSyncMarkerInfo& Marker : Markers)
    {
        MarkerValues.Add(MakeShared<FJsonValueObject>(SyncMarkerToJson(Marker)));
    }
    Root->SetArrayField(TEXT("sync_markers"), MarkerValues);

    FString Output;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
    FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
    return Output;
}

// ---------------------------------------------------------------------------
// string conversions
// ---------------------------------------------------------------------------

FString FUnrealMCPAnimationOps::InterpModeToString(int32 Mode)
{
    switch (static_cast<ERichCurveInterpMode>(Mode))
    {
    case ERichCurveInterpMode::RCIM_Constant: return TEXT("constant");
    case ERichCurveInterpMode::RCIM_Linear:   return TEXT("linear");
    case ERichCurveInterpMode::RCIM_Cubic:    return TEXT("cubic");
    default:                                  return TEXT("none");
    }
}

FString FUnrealMCPAnimationOps::TangentModeToString(int32 Mode)
{
    switch (static_cast<ERichCurveTangentMode>(Mode))
    {
    case ERichCurveTangentMode::RCTM_Auto:  return TEXT("auto");
    case ERichCurveTangentMode::RCTM_User:  return TEXT("user");
    case ERichCurveTangentMode::RCTM_Break: return TEXT("break");
    default:                                return TEXT("none");
    }
}

bool FUnrealMCPAnimationOps::StringToInterpMode(const FString& Text, int32& OutMode)
{
    const FString Lower = Text.ToLower();
    if (Lower == TEXT("constant") || Lower == TEXT("rcim_constant"))
    {
        OutMode = static_cast<int32>(ERichCurveInterpMode::RCIM_Constant);
        return true;
    }
    if (Lower == TEXT("linear") || Lower == TEXT("rcim_linear"))
    {
        OutMode = static_cast<int32>(ERichCurveInterpMode::RCIM_Linear);
        return true;
    }
    if (Lower == TEXT("cubic") || Lower == TEXT("rcim_cubic"))
    {
        OutMode = static_cast<int32>(ERichCurveInterpMode::RCIM_Cubic);
        return true;
    }
    if (Lower == TEXT("none") || Lower == TEXT("rcim_none"))
    {
        OutMode = static_cast<int32>(ERichCurveInterpMode::RCIM_None);
        return true;
    }
    return false;
}

bool FUnrealMCPAnimationOps::StringToTangentMode(const FString& Text, int32& OutMode)
{
    const FString Lower = Text.ToLower();
    if (Lower == TEXT("auto") || Lower == TEXT("rctm_auto"))
    {
        OutMode = static_cast<int32>(ERichCurveTangentMode::RCTM_Auto);
        return true;
    }
    if (Lower == TEXT("user") || Lower == TEXT("rctm_user"))
    {
        OutMode = static_cast<int32>(ERichCurveTangentMode::RCTM_User);
        return true;
    }
    if (Lower == TEXT("break") || Lower == TEXT("rctm_break"))
    {
        OutMode = static_cast<int32>(ERichCurveTangentMode::RCTM_Break);
        return true;
    }
    if (Lower == TEXT("none") || Lower == TEXT("rctm_none"))
    {
        OutMode = static_cast<int32>(ERichCurveTangentMode::RCTM_None);
        return true;
    }
    return false;
}

FString FUnrealMCPAnimationOps::AdditiveTypeToString(int32 Type)
{
    switch (static_cast<EAdditiveAnimationType>(Type))
    {
    case EAdditiveAnimationType::AAT_LocalSpaceBase:         return TEXT("LocalSpace");
    case EAdditiveAnimationType::AAT_RotationOffsetMeshSpace: return TEXT("MeshSpace");
    default:                                                 return TEXT("None");
    }
}

bool FUnrealMCPAnimationOps::StringToAdditiveType(const FString& Text, int32& OutType)
{
    const FString Lower = Text.ToLower().Replace(TEXT("_"), TEXT(""));
    if (Lower == TEXT("none") || Lower == TEXT("aatnone"))
    {
        OutType = static_cast<int32>(EAdditiveAnimationType::AAT_None);
        return true;
    }
    if (Lower == TEXT("localspace") || Lower == TEXT("localspacebase") || Lower == TEXT("aatlocalspacebase"))
    {
        OutType = static_cast<int32>(EAdditiveAnimationType::AAT_LocalSpaceBase);
        return true;
    }
    if (Lower == TEXT("meshspace") || Lower == TEXT("rotationoffsetmeshspace") ||
        Lower == TEXT("aatrotationoffsetmeshspace"))
    {
        OutType = static_cast<int32>(EAdditiveAnimationType::AAT_RotationOffsetMeshSpace);
        return true;
    }
    return false;
}

FString FUnrealMCPAnimationOps::RootLockToString(int32 LockType)
{
    switch (static_cast<ERootMotionRootLock::Type>(LockType))
    {
    case ERootMotionRootLock::AnimFirstFrame: return TEXT("AnimFirstFrame");
    case ERootMotionRootLock::Zero:           return TEXT("Zero");
    default:                                  return TEXT("RefPose");
    }
}

bool FUnrealMCPAnimationOps::StringToRootLock(const FString& Text, int32& OutLockType)
{
    const FString Lower = Text.ToLower();
    if (Lower == TEXT("refpose") || Lower == TEXT("animrefpose"))
    {
        OutLockType = static_cast<int32>(ERootMotionRootLock::RefPose);
        return true;
    }
    if (Lower == TEXT("animfirstframe") || Lower == TEXT("firstframe"))
    {
        OutLockType = static_cast<int32>(ERootMotionRootLock::AnimFirstFrame);
        return true;
    }
    if (Lower == TEXT("zero"))
    {
        OutLockType = static_cast<int32>(ERootMotionRootLock::Zero);
        return true;
    }
    return false;
}

FString FUnrealMCPAnimationOps::FilterTypeToString(int32 FilterType)
{
    switch (static_cast<ENotifyFilterType::Type>(FilterType))
    {
    case ENotifyFilterType::LOD: return TEXT("lod");
    default:                     return TEXT("none");
    }
}

bool FUnrealMCPAnimationOps::StringToFilterType(const FString& Text, int32& OutFilterType)
{
    const FString Lower = Text.ToLower().Replace(TEXT("_"), TEXT(""));
    if (Lower == TEXT("none") || Lower == TEXT("nofiltering"))
    {
        OutFilterType = static_cast<int32>(ENotifyFilterType::NoFiltering);
        return true;
    }
    if (Lower == TEXT("lod"))
    {
        OutFilterType = static_cast<int32>(ENotifyFilterType::LOD);
        return true;
    }
    return false;
}
