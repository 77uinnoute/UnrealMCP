#include "Commands/Animation/UnrealMCPAnimationCommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"

#include "AlphaBlend.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Animation/AnimCompositeBase.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimTypes.h"
#include "Animation/Skeleton.h"
#include "Factories/AnimMontageFactory.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealMCPAnimationMontageOps, Log, All);

namespace
{
    /** Registry searches clamp their result; a broad /Game query must not build a huge payload. */
    constexpr int32 DefaultMaxResults = 200;

    /** How close two notify / section times must be to count as "the same" one. */
    constexpr float TimeTolerance = 0.01f;

    FString NormalizePath(const FString& InPath)
    {
        // Callers pass "/Game/A/B", "/Game/A/B.B" or the short name; the registry answers only the
        // package form, and every string comparison below wants one shape.
        int32 DotIndex = INDEX_NONE;
        FString Package = InPath;
        if (InPath.FindChar(TEXT('.'), DotIndex))
        {
            Package = InPath.Left(DotIndex);
        }
        return Package;
    }

    /** The asset registry stores object tags as export text; unwrap to the inner object path. */
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

    /** Shared registry query for montages: metadata only, the assets are never loaded here. */
    void QueryMontageRegistry(const FString& SearchPath, TArray<FAssetData>& OutAssets)
    {
        const FString Root = SearchPath.IsEmpty() ? TEXT("/Game") : SearchPath;

        FAssetRegistryModule& AssetRegistryModule =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        IAssetRegistry& Registry = AssetRegistryModule.Get();

        FARFilter Filter;
        Filter.bRecursivePaths = true;
        Filter.bRecursiveClasses = true;
        Filter.PackagePaths.Add(FName(*Root));
        Filter.ClassPaths.Add(UAnimMontage::StaticClass()->GetClassPathName());

        Registry.GetAssets(Filter, OutAssets);
    }

    void FillRegistryMontageInfo(const FAssetData& AssetData, FUnrealMCPAnimMontageInfo& OutInfo)
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
    }

    bool IsBranchingPointNotify(const FAnimNotifyEvent& Notify)
    {
        // 5.5 keeps branching points as notifies whose montage tick type is BranchingPoint; the old
        // FBranchingPoint struct is deprecated, so this flag is the only representation.
        return Notify.MontageTickType == EMontageNotifyTickType::BranchingPoint;
    }

    FString NotifyClassName(const FAnimNotifyEvent& Notify)
    {
        const UObject* NotifyObject = Notify.NotifyStateClass ? static_cast<const UObject*>(Notify.NotifyStateClass)
                                                              : static_cast<const UObject*>(Notify.Notify);
        return NotifyObject ? NotifyObject->GetClass()->GetName() : FString();
    }

    /** Section time of a section, used to place a notify on a section start. */
    bool FindSectionStartTime(UAnimMontage* Montage, const FString& SectionName, float& OutTime)
    {
        OutTime = 0.0f;
        if (!Montage || SectionName.IsEmpty())
        {
            return false;
        }

        const int32 SectionIndex = Montage->GetSectionIndex(FName(*SectionName));
        if (SectionIndex == INDEX_NONE)
        {
            return false;
        }

        float StartTime = 0.0f;
        float EndTime = 0.0f;
        Montage->GetSectionStartAndEndTime(SectionIndex, StartTime, EndTime);
        OutTime = StartTime;
        return true;
    }

    /** Sorts the sections by start time, the order the montage editor keeps them in. */
    void SortSectionsByTime(UAnimMontage* Montage)
    {
        Montage->CompositeSections.Sort([](const FCompositeSection& A, const FCompositeSection& B)
        {
            return A.GetTime() < B.GetTime();
        });
    }

    /**
     * Every structural montage write ends with this: the sections and notifies are linked to segments
     * by index, the play length is the longest slot track, and the branching point markers are built
     * from the notify array. Skipping any of the three leaves an asset that only looks right in memory.
     *
     * FAnimTrack::ValidateSegmentTimes would tidy the segment times as well, but 5.5 does not export
     * it (no ENGINE_API), so a plugin cannot call it.
     */
    void FinishMontageEdit(UAnimMontage* Montage, bool bRecomputeLength)
    {
        if (bRecomputeLength)
        {
            Montage->SetCompositeLength(Montage->CalculateSequenceLength());
        }

        Montage->UpdateLinkableElements();
        Montage->RefreshCacheData();
        Montage->MarkPackageDirty();
    }

    /** Index of a segment after a re-sort, so the response can point at what was written. */
    int32 FindSegmentIndex(const FAnimTrack& Track, const UAnimSequenceBase* AnimReference, float StartPos)
    {
        for (int32 Index = 0; Index < Track.AnimSegments.Num(); ++Index)
        {
            const FAnimSegment& Segment = Track.AnimSegments[Index];
            if (Segment.GetAnimReference() == AnimReference && FMath::IsNearlyEqual(Segment.StartPos, StartPos, TimeTolerance))
            {
                return Index;
            }
        }
        return INDEX_NONE;
    }

    struct FBlendOptionName
    {
        EAlphaBlendOption Option;
        const TCHAR* Name;
    };

    const TArray<FBlendOptionName>& BlendOptionTable()
    {
        static const TArray<FBlendOptionName> Table = {
            {EAlphaBlendOption::Linear, TEXT("Linear")},
            {EAlphaBlendOption::Cubic, TEXT("Cubic")},
            {EAlphaBlendOption::HermiteCubic, TEXT("HermiteCubic")},
            {EAlphaBlendOption::Sinusoidal, TEXT("Sinusoidal")},
            {EAlphaBlendOption::QuadraticInOut, TEXT("QuadraticInOut")},
            {EAlphaBlendOption::CubicInOut, TEXT("CubicInOut")},
            {EAlphaBlendOption::QuarticInOut, TEXT("QuarticInOut")},
            {EAlphaBlendOption::QuinticInOut, TEXT("QuinticInOut")},
            {EAlphaBlendOption::CircularIn, TEXT("CircularIn")},
            {EAlphaBlendOption::CircularOut, TEXT("CircularOut")},
            {EAlphaBlendOption::CircularInOut, TEXT("CircularInOut")},
            {EAlphaBlendOption::ExpIn, TEXT("ExpIn")},
            {EAlphaBlendOption::ExpOut, TEXT("ExpOut")},
            {EAlphaBlendOption::ExpInOut, TEXT("ExpInOut")},
            {EAlphaBlendOption::Custom, TEXT("Custom")},
        };
        return Table;
    }
}

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------

bool FUnrealMCPAnimationMontageOps::ResolveMontage(const FString& AssetPath, UAnimMontage*& OutMontage,
                                                   FUnrealMCPAnimError& OutError)
{
    OutMontage = nullptr;
    if (AssetPath.IsEmpty())
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }

    UObject* Asset = FUnrealMCPCommonUtils::FindAsset(AssetPath);
    if (!Asset)
    {
        OutError.Set(EUnrealMCPAnimError::AssetNotFound,
                     FString::Printf(TEXT("'%s' was not found; pass an object path (/Game/A/B.B) or a package path (/Game/A/B)"), *AssetPath));
        return false;
    }

    OutMontage = Cast<UAnimMontage>(Asset);
    if (!OutMontage)
    {
        OutError.Set(EUnrealMCPAnimError::AssetNotAnimMontage,
                     FString::Printf(TEXT("'%s' is a %s, not an animation montage"), *Asset->GetPathName(),
                                     *Asset->GetClass()->GetName()));
        return false;
    }

    return true;
}

bool FUnrealMCPAnimationMontageOps::ResolveSection(UAnimMontage* Montage, const FString& SectionName,
                                                   int32 SectionIndex, int32& OutSectionIndex,
                                                   FUnrealMCPAnimError& OutError)
{
    OutSectionIndex = INDEX_NONE;
    if (!Montage)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }

    if (!SectionName.IsEmpty())
    {
        const int32 Index = Montage->GetSectionIndex(FName(*SectionName));
        if (Index == INDEX_NONE)
        {
            OutError.Set(EUnrealMCPAnimError::SectionNotFound,
                         FString::Printf(TEXT("no section named '%s' on '%s'"), *SectionName, *Montage->GetName()));
            for (int32 I = 0; I < Montage->CompositeSections.Num(); ++I)
            {
                OutError.Candidates.Add(FString::Printf(TEXT("%d: %s @ %.4fs"), I,
                                                        *Montage->CompositeSections[I].SectionName.ToString(),
                                                        Montage->CompositeSections[I].GetTime()));
            }
            return false;
        }

        OutSectionIndex = Index;
        return true;
    }

    if (!Montage->CompositeSections.IsValidIndex(SectionIndex))
    {
        OutError.Set(EUnrealMCPAnimError::SectionNotFound,
                     FString::Printf(TEXT("section_index %d is out of range (%d section(s) on '%s')"),
                                     SectionIndex, Montage->CompositeSections.Num(), *Montage->GetName()));
        for (int32 I = 0; I < Montage->CompositeSections.Num(); ++I)
        {
            OutError.Candidates.Add(FString::Printf(TEXT("%d: %s @ %.4fs"), I,
                                                    *Montage->CompositeSections[I].SectionName.ToString(),
                                                    Montage->CompositeSections[I].GetTime()));
        }
        return false;
    }

    OutSectionIndex = SectionIndex;
    return true;
}

bool FUnrealMCPAnimationMontageOps::ResolveSlotTrack(UAnimMontage* Montage, int32 TrackIndex,
                                                     const FString& SlotName, int32& OutTrackIndex,
                                                     FUnrealMCPAnimError& OutError)
{
    OutTrackIndex = INDEX_NONE;
    if (!Montage)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }

    if (!SlotName.IsEmpty())
    {
        for (int32 Index = 0; Index < Montage->SlotAnimTracks.Num(); ++Index)
        {
            if (Montage->SlotAnimTracks[Index].SlotName.ToString().Equals(SlotName, ESearchCase::IgnoreCase))
            {
                OutTrackIndex = Index;
                return true;
            }
        }

        OutError.Set(EUnrealMCPAnimError::SlotTrackNotFound,
                     FString::Printf(TEXT("no slot named '%s' on '%s'"), *SlotName, *Montage->GetName()));
    }
    else if (Montage->SlotAnimTracks.IsValidIndex(TrackIndex))
    {
        OutTrackIndex = TrackIndex;
        return true;
    }
    else
    {
        OutError.Set(EUnrealMCPAnimError::SlotTrackNotFound,
                     FString::Printf(TEXT("track_index %d is out of range (%d slot track(s) on '%s')"),
                                     TrackIndex, Montage->SlotAnimTracks.Num(), *Montage->GetName()));
    }

    for (int32 Index = 0; Index < Montage->SlotAnimTracks.Num(); ++Index)
    {
        OutError.Candidates.Add(FString::Printf(TEXT("%d: %s (%d segment(s))"), Index,
                                                *Montage->SlotAnimTracks[Index].SlotName.ToString(),
                                                Montage->SlotAnimTracks[Index].AnimTrack.AnimSegments.Num()));
    }
    return false;
}

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

void FUnrealMCPAnimationMontageOps::FillMontageInfo(UAnimMontage* Montage, FUnrealMCPAnimMontageInfo& OutInfo)
{
    OutInfo = FUnrealMCPAnimMontageInfo();
    if (!Montage)
    {
        return;
    }

    OutInfo.AssetPath = Montage->GetPathName();
    OutInfo.AssetName = Montage->GetName();
    OutInfo.SkeletonPath = Montage->GetSkeleton() ? Montage->GetSkeleton()->GetPathName() : FString();
    OutInfo.Length = Montage->GetPlayLength();
    OutInfo.SectionCount = Montage->CompositeSections.Num();
    OutInfo.SlotTrackCount = Montage->SlotAnimTracks.Num();
    OutInfo.RateScale = Montage->RateScale;
    OutInfo.SyncGroup = Montage->SyncGroup.IsNone() ? FString() : Montage->SyncGroup.ToString();
    OutInfo.InfoSource = TEXT("asset");

    int32 SegmentCount = 0;
    for (const FSlotAnimationTrack& Slot : Montage->SlotAnimTracks)
    {
        SegmentCount += Slot.AnimTrack.AnimSegments.Num();
    }
    OutInfo.SegmentCount = SegmentCount;

    OutInfo.NotifyCount = Montage->Notifies.Num();
    for (const FAnimNotifyEvent& Notify : Montage->Notifies)
    {
        if (IsBranchingPointNotify(Notify))
        {
            ++OutInfo.BranchingPointCount;
        }
    }

    FUnrealMCPAnimBlendInfo Blend;
    GetBlendInfo(Montage, Blend);
    OutInfo.BlendInTime = Blend.BlendInTime;
    OutInfo.BlendInOption = Blend.BlendInOption;
    OutInfo.BlendInMode = Blend.BlendInMode;
    OutInfo.BlendOutTime = Blend.BlendOutTime;
    OutInfo.BlendOutOption = Blend.BlendOutOption;
    OutInfo.BlendOutMode = Blend.BlendOutMode;
    OutInfo.BlendOutTriggerTime = Blend.BlendOutTriggerTime;
    OutInfo.bEnableAutoBlendOut = Blend.bEnableAutoBlendOut;

    bool bTranslation = false;
    bool bRotation = false;
    GetEnableRootMotion(Montage, bTranslation, bRotation);
    OutInfo.bEnableRootMotionTranslation = bTranslation;
    OutInfo.bEnableRootMotionRotation = bRotation;
}

bool FUnrealMCPAnimationMontageOps::ListMontages(const FString& SearchPath, const FString& SkeletonFilter,
                                                 int32 MaxResults, TArray<FUnrealMCPAnimMontageInfo>& OutInfos,
                                                 int32& OutFoundCount, FUnrealMCPAnimError& OutError)
{
    OutInfos.Reset();
    OutFoundCount = 0;

    TArray<FAssetData> Assets;
    QueryMontageRegistry(SearchPath, Assets);

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

        FUnrealMCPAnimMontageInfo Info;
        FillRegistryMontageInfo(AssetData, Info);
        OutInfos.Add(Info);
    }

    OutError.Message = FString::Printf(TEXT("%d montage(s) matched below '%s'"), OutFoundCount,
                                       *(SearchPath.IsEmpty() ? TEXT("/Game") : SearchPath));
    return true;
}

bool FUnrealMCPAnimationMontageOps::FindMontagesUsingAnimation(const FString& AnimationPath, int32 MaxResults,
                                                               int32 MaxScan,
                                                               TArray<FUnrealMCPAnimMontageInfo>& OutInfos,
                                                               int32& OutFoundCount, FString& OutStrategy,
                                                               FUnrealMCPAnimError& OutError)
{
    OutInfos.Reset();
    OutFoundCount = 0;
    OutStrategy.Reset();

    if (AnimationPath.IsEmpty())
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("anim_path is required"));
        return false;
    }

    UObject* Animation = FUnrealMCPCommonUtils::FindAsset(AnimationPath);
    if (!Animation)
    {
        OutError.Set(EUnrealMCPAnimError::AssetNotFound,
                     FString::Printf(TEXT("'%s' was not found"), *AnimationPath));
        return false;
    }

    const UAnimSequenceBase* AnimationAsset = Cast<UAnimSequenceBase>(Animation);
    if (!AnimationAsset)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams,
                     FString::Printf(TEXT("'%s' is a %s, not an animation asset"), *AnimationPath,
                                     *Animation->GetClass()->GetName()));
        return false;
    }

    const FString AnimationPackage = NormalizePath(Animation->GetPathName());
    const int32 Limit = MaxResults > 0 ? MaxResults : DefaultMaxResults;

    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& Registry = AssetRegistryModule.Get();

    // Fast path: the registry knows which packages reference this one, so nothing has to be loaded.
    TArray<FName> Referencers;
    Registry.GetReferencers(FName(*AnimationPackage), Referencers);

    OutStrategy = TEXT("registry_referencers");
    for (const FName& ReferencerPackage : Referencers)
    {
        TArray<FAssetData> ReferencerAssets;
        if (!Registry.GetAssetsByPackageName(ReferencerPackage, ReferencerAssets))
        {
            continue;
        }

        for (const FAssetData& AssetData : ReferencerAssets)
        {
            if (AssetData.AssetClassPath != UAnimMontage::StaticClass()->GetClassPathName())
            {
                continue;
            }

            ++OutFoundCount;
            if (OutInfos.Num() >= Limit)
            {
                continue;
            }

            FUnrealMCPAnimMontageInfo Info;
            FillRegistryMontageInfo(AssetData, Info);
            OutInfos.Add(Info);
        }
    }

    if (OutInfos.Num() > 0)
    {
        return true;
    }

    // Fallback: an asset created moments ago can still have no dependency data, and then the only
    // answer is to look at the montages themselves. The scan is bounded so it cannot walk the library.
    TArray<FAssetData> CandidateAssets;
    QueryMontageRegistry(TEXT("/Game"), CandidateAssets);

    const int32 ScanLimit = MaxScan > 0 ? MaxScan : 100;
    int32 Scanned = 0;
    OutStrategy = TEXT("loaded_scan");
    for (const FAssetData& AssetData : CandidateAssets)
    {
        if (Scanned >= ScanLimit)
        {
            break;
        }
        ++Scanned;

        UAnimMontage* Montage = Cast<UAnimMontage>(AssetData.GetAsset());
        if (!Montage)
        {
            continue;
        }

        TArray<UAnimationAsset*> Referred;
        Montage->GetAllAnimationSequencesReferred(Referred, true);
        if (!Referred.Contains(const_cast<UAnimSequenceBase*>(AnimationAsset)))
        {
            continue;
        }

        ++OutFoundCount;
        if (OutInfos.Num() >= Limit)
        {
            continue;
        }

        FUnrealMCPAnimMontageInfo Info;
        FillMontageInfo(Montage, Info);
        OutInfos.Add(Info);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Sections
// ---------------------------------------------------------------------------

void FUnrealMCPAnimationMontageOps::FillSectionInfo(UAnimMontage* Montage, int32 SectionIndex,
                                                    FUnrealMCPAnimSectionInfo& OutInfo)
{
    OutInfo = FUnrealMCPAnimSectionInfo();
    if (!Montage || !Montage->CompositeSections.IsValidIndex(SectionIndex))
    {
        return;
    }

    const FCompositeSection& Section = Montage->CompositeSections[SectionIndex];

    OutInfo.SectionIndex = SectionIndex;
    OutInfo.SectionName = Section.SectionName.ToString();
    Montage->GetSectionStartAndEndTime(SectionIndex, OutInfo.StartTime, OutInfo.EndTime);
    OutInfo.Length = Montage->GetSectionLength(SectionIndex);
    // NAME_None means "no link"; reporting the literal "None" would look like a section of that name.
    OutInfo.NextSectionName = Section.NextSectionName.IsNone() ? FString() : Section.NextSectionName.ToString();
    OutInfo.bIsLooping = !Section.NextSectionName.IsNone() && Section.NextSectionName == Section.SectionName;
    OutInfo.SlotIndex = Section.GetSlotIndex();
    OutInfo.SegmentIndex = Section.GetSegmentIndex();
    OutInfo.LinkMethod = LinkMethodToString(static_cast<int32>(Section.GetLinkMethod()));
}

void FUnrealMCPAnimationMontageOps::ListSections(UAnimMontage* Montage, TArray<FUnrealMCPAnimSectionInfo>& OutSections)
{
    OutSections.Reset();
    if (!Montage)
    {
        return;
    }

    OutSections.Reserve(Montage->CompositeSections.Num());
    for (int32 Index = 0; Index < Montage->CompositeSections.Num(); ++Index)
    {
        FUnrealMCPAnimSectionInfo Info;
        FillSectionInfo(Montage, Index, Info);
        OutSections.Add(Info);
    }
}

int32 FUnrealMCPAnimationMontageOps::GetSectionIndexAtTime(UAnimMontage* Montage, float Time)
{
    if (!Montage)
    {
        return INDEX_NONE;
    }
    return Montage->GetSectionIndexFromPosition(Time);
}

void FUnrealMCPAnimationMontageOps::ListSectionLinks(UAnimMontage* Montage, TArray<TPair<FString, FString>>& OutLinks)
{
    OutLinks.Reset();
    if (!Montage)
    {
        return;
    }

    for (const FCompositeSection& Section : Montage->CompositeSections)
    {
        if (!Section.NextSectionName.IsNone())
        {
            OutLinks.Add(TPair<FString, FString>(Section.SectionName.ToString(), Section.NextSectionName.ToString()));
        }
    }
}

bool FUnrealMCPAnimationMontageOps::AddSection(UAnimMontage* Montage, const FString& SectionName, float StartTime,
                                               int32& OutSectionIndex, FUnrealMCPAnimError& OutError)
{
    OutSectionIndex = INDEX_NONE;
    if (!Montage)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }
    if (SectionName.IsEmpty())
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("section_name is required"));
        return false;
    }
    if (StartTime < 0.0f)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidValue,
                     FString::Printf(TEXT("start_time %.4f is negative"), StartTime));
        return false;
    }

    const FName Name(*SectionName);
    if (Montage->GetSectionIndex(Name) != INDEX_NONE)
    {
        OutError.Set(EUnrealMCPAnimError::SectionNameTaken,
                     FString::Printf(TEXT("a section named '%s' already exists on '%s'"),
                                     *SectionName, *Montage->GetName()));
        for (int32 I = 0; I < Montage->CompositeSections.Num(); ++I)
        {
            OutError.Candidates.Add(FString::Printf(TEXT("%d: %s @ %.4fs"), I,
                                                    *Montage->CompositeSections[I].SectionName.ToString(),
                                                    Montage->CompositeSections[I].GetTime()));
        }
        return false;
    }

    Montage->Modify();

    // AddAnimCompositeSection appends; the sections are time-ordered everywhere else in the editor,
    // and get_section_index_at_time walks them in order, so they are sorted here.
    const int32 AddedIndex = Montage->AddAnimCompositeSection(Name, StartTime);
    if (AddedIndex == INDEX_NONE)
    {
        OutError.Set(EUnrealMCPAnimError::WriteFailed,
                     FString::Printf(TEXT("the montage refused to add a section named '%s'"), *SectionName));
        return false;
    }

    SortSectionsByTime(Montage);
    FinishMontageEdit(Montage, /*bRecomputeLength=*/false);

    OutSectionIndex = Montage->GetSectionIndex(Name);
    if (OutSectionIndex == INDEX_NONE)
    {
        OutError.Set(EUnrealMCPAnimError::WriteFailed,
                     FString::Printf(TEXT("the section '%s' was added but is not in the montage afterwards"), *SectionName));
        return false;
    }

    return true;
}

bool FUnrealMCPAnimationMontageOps::RemoveSection(UAnimMontage* Montage, int32 SectionIndex,
                                                  FUnrealMCPAnimError& OutError)
{
    if (!Montage || !Montage->CompositeSections.IsValidIndex(SectionIndex))
    {
        OutError.Set(EUnrealMCPAnimError::SectionNotFound,
                     FString::Printf(TEXT("section_index %d is out of range"), SectionIndex));
        return false;
    }

    // The montage editor refuses this too: a montage without a section has nowhere to put a segment.
    if (Montage->CompositeSections.Num() <= 1)
    {
        OutError.Set(EUnrealMCPAnimError::LastSection,
                     FString::Printf(TEXT("'%s' has a single section; a montage needs at least one"),
                                     *Montage->GetName()));
        OutError.Candidates.Add(Montage->CompositeSections[0].SectionName.ToString());
        return false;
    }

    const FName RemovedName = Montage->CompositeSections[SectionIndex].SectionName;

    Montage->Modify();
    if (!Montage->DeleteAnimCompositeSection(SectionIndex))
    {
        OutError.Set(EUnrealMCPAnimError::WriteFailed,
                     FString::Printf(TEXT("the montage refused to delete section '%s'"), *RemovedName.ToString()));
        return false;
    }

    // Links that pointed at the removed section would otherwise dangle: the editor clears them.
    for (FCompositeSection& Section : Montage->CompositeSections)
    {
        if (Section.NextSectionName == RemovedName)
        {
            Section.NextSectionName = NAME_None;
        }
    }

    SortSectionsByTime(Montage);
    FinishMontageEdit(Montage, /*bRecomputeLength=*/false);
    return true;
}

bool FUnrealMCPAnimationMontageOps::RenameSection(UAnimMontage* Montage, int32 SectionIndex, const FString& NewName,
                                                  FUnrealMCPAnimError& OutError)
{
    if (!Montage || !Montage->CompositeSections.IsValidIndex(SectionIndex))
    {
        OutError.Set(EUnrealMCPAnimError::SectionNotFound,
                     FString::Printf(TEXT("section_index %d is out of range"), SectionIndex));
        return false;
    }
    if (NewName.IsEmpty())
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("new_name is required"));
        return false;
    }

    const FName OldName = Montage->CompositeSections[SectionIndex].SectionName;
    const FName NewNameName(*NewName);
    if (OldName == NewNameName)
    {
        return true;
    }

    const int32 Existing = Montage->GetSectionIndex(NewNameName);
    if (Existing != INDEX_NONE && Existing != SectionIndex)
    {
        OutError.Set(EUnrealMCPAnimError::SectionNameTaken,
                     FString::Printf(TEXT("a section named '%s' already exists on '%s'"),
                                     *NewName, *Montage->GetName()));
        OutError.Candidates.Add(FString::Printf(TEXT("%d: %s"), Existing, *NewName));
        return false;
    }

    Montage->Modify();
    Montage->CompositeSections[SectionIndex].SectionName = NewNameName;

    // Section links are stored by name, so every reference to the old name has to follow the rename.
    for (FCompositeSection& Section : Montage->CompositeSections)
    {
        if (Section.NextSectionName == OldName)
        {
            Section.NextSectionName = NewNameName;
        }
    }

    FinishMontageEdit(Montage, /*bRecomputeLength=*/false);
    return true;
}

bool FUnrealMCPAnimationMontageOps::SetSectionStartTime(UAnimMontage* Montage, int32 SectionIndex, float Time,
                                                        FUnrealMCPAnimError& OutError)
{
    if (!Montage || !Montage->CompositeSections.IsValidIndex(SectionIndex))
    {
        OutError.Set(EUnrealMCPAnimError::SectionNotFound,
                     FString::Printf(TEXT("section_index %d is out of range"), SectionIndex));
        return false;
    }
    if (Time < 0.0f)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidValue,
                     FString::Printf(TEXT("start_time %.4f is negative"), Time));
        return false;
    }
    if (Montage->GetPlayLength() > 0.0f && Time > Montage->GetPlayLength())
    {
        OutError.Set(EUnrealMCPAnimError::InvalidValue,
                     FString::Printf(TEXT("start_time %.4f is past the montage length %.4fs"),
                                     Time, Montage->GetPlayLength()));
        return false;
    }

    Montage->Modify();
    Montage->CompositeSections[SectionIndex].SetTime(Time);
    SortSectionsByTime(Montage);
    FinishMontageEdit(Montage, /*bRecomputeLength=*/false);
    return true;
}

bool FUnrealMCPAnimationMontageOps::SetNextSection(UAnimMontage* Montage, int32 SectionIndex,
                                                   const FString& NextSectionName, FUnrealMCPAnimError& OutError)
{
    if (!Montage || !Montage->CompositeSections.IsValidIndex(SectionIndex))
    {
        OutError.Set(EUnrealMCPAnimError::SectionNotFound,
                     FString::Printf(TEXT("section_index %d is out of range"), SectionIndex));
        return false;
    }

    FName NextName = NAME_None;
    if (!NextSectionName.IsEmpty())
    {
        const int32 NextIndex = Montage->GetSectionIndex(FName(*NextSectionName));
        if (NextIndex == INDEX_NONE)
        {
            OutError.Set(EUnrealMCPAnimError::SectionNotFound,
                         FString::Printf(TEXT("next section '%s' does not exist on '%s'"),
                                         *NextSectionName, *Montage->GetName()));
            for (int32 I = 0; I < Montage->CompositeSections.Num(); ++I)
            {
                OutError.Candidates.Add(Montage->CompositeSections[I].SectionName.ToString());
            }
            return false;
        }
        NextName = Montage->CompositeSections[NextIndex].SectionName;
    }

    Montage->Modify();
    Montage->CompositeSections[SectionIndex].NextSectionName = NextName;
    FinishMontageEdit(Montage, /*bRecomputeLength=*/false);
    return true;
}

// ---------------------------------------------------------------------------
// Slot tracks
// ---------------------------------------------------------------------------

void FUnrealMCPAnimationMontageOps::ListSlotTracks(UAnimMontage* Montage, TArray<FUnrealMCPAnimSlotTrackInfo>& OutTracks)
{
    OutTracks.Reset();
    if (!Montage)
    {
        return;
    }

    const USkeleton* Skeleton = Montage->GetSkeleton();
    OutTracks.Reserve(Montage->SlotAnimTracks.Num());
    for (int32 Index = 0; Index < Montage->SlotAnimTracks.Num(); ++Index)
    {
        const FSlotAnimationTrack& Slot = Montage->SlotAnimTracks[Index];

        FUnrealMCPAnimSlotTrackInfo Info;
        Info.TrackIndex = Index;
        Info.SlotName = Slot.SlotName.ToString();
        if (Skeleton)
        {
            const FName GroupName = Skeleton->GetSlotGroupName(Slot.SlotName);
            Info.GroupName = GroupName.IsNone() ? FString() : GroupName.ToString();
        }
        Info.SegmentCount = Slot.AnimTrack.AnimSegments.Num();
        Info.TrackLength = Slot.AnimTrack.GetLength();
        OutTracks.Add(Info);
    }
}

bool FUnrealMCPAnimationMontageOps::AddSlotTrack(UAnimMontage* Montage, const FString& SlotName, int32& OutTrackIndex,
                                                 FUnrealMCPAnimError& OutError)
{
    OutTrackIndex = INDEX_NONE;
    if (!Montage)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }
    if (SlotName.IsEmpty())
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("slot_name is required"));
        return false;
    }

    for (const FSlotAnimationTrack& Slot : Montage->SlotAnimTracks)
    {
        if (Slot.SlotName.ToString().Equals(SlotName, ESearchCase::IgnoreCase))
        {
            OutError.Set(EUnrealMCPAnimError::SlotNameTaken,
                         FString::Printf(TEXT("'%s' already has a slot named '%s'"), *Montage->GetName(), *SlotName));
            return false;
        }
    }

    Montage->Modify();
    Montage->AddSlot(FName(*SlotName));
    OutTrackIndex = Montage->SlotAnimTracks.Num() - 1;

    FinishMontageEdit(Montage, /*bRecomputeLength=*/false);
    return true;
}

bool FUnrealMCPAnimationMontageOps::RemoveSlotTrack(UAnimMontage* Montage, int32 TrackIndex,
                                                    FUnrealMCPAnimError& OutError)
{
    if (!Montage || !Montage->SlotAnimTracks.IsValidIndex(TrackIndex))
    {
        OutError.Set(EUnrealMCPAnimError::SlotTrackNotFound,
                     FString::Printf(TEXT("track_index %d is out of range"), TrackIndex));
        return false;
    }

    // The montage editor keeps a single slot track as well ("Can't delete the last slot track").
    if (Montage->SlotAnimTracks.Num() <= 1)
    {
        OutError.Set(EUnrealMCPAnimError::LastSlotTrack,
                     FString::Printf(TEXT("'%s' has a single slot track; a montage needs at least one"),
                                     *Montage->GetName()));
        OutError.Candidates.Add(Montage->SlotAnimTracks[0].SlotName.ToString());
        return false;
    }

    Montage->Modify();
    Montage->SlotAnimTracks.RemoveAt(TrackIndex);
    FinishMontageEdit(Montage, /*bRecomputeLength=*/true);
    return true;
}

bool FUnrealMCPAnimationMontageOps::SetSlotName(UAnimMontage* Montage, int32 TrackIndex, const FString& NewName,
                                                FUnrealMCPAnimError& OutError)
{
    if (!Montage || !Montage->SlotAnimTracks.IsValidIndex(TrackIndex))
    {
        OutError.Set(EUnrealMCPAnimError::SlotTrackNotFound,
                     FString::Printf(TEXT("track_index %d is out of range"), TrackIndex));
        return false;
    }
    if (NewName.IsEmpty())
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("new_name is required"));
        return false;
    }

    const FName NewSlotName(*NewName);
    for (int32 Index = 0; Index < Montage->SlotAnimTracks.Num(); ++Index)
    {
        if (Index != TrackIndex && Montage->SlotAnimTracks[Index].SlotName == NewSlotName)
        {
            OutError.Set(EUnrealMCPAnimError::SlotNameTaken,
                         FString::Printf(TEXT("another slot track is already named '%s'"), *NewName));
            return false;
        }
    }

    Montage->Modify();
    Montage->SlotAnimTracks[TrackIndex].SlotName = NewSlotName;
    FinishMontageEdit(Montage, /*bRecomputeLength=*/false);
    return true;
}

void FUnrealMCPAnimationMontageOps::ListUsedSlotNames(UAnimMontage* Montage, TArray<FString>& OutUsed,
                                                      TArray<FString>& OutSkeletonSlots)
{
    OutUsed.Reset();
    OutSkeletonSlots.Reset();

    if (!Montage)
    {
        return;
    }

    for (const FSlotAnimationTrack& Slot : Montage->SlotAnimTracks)
    {
        if (!Slot.SlotName.IsNone())
        {
            OutUsed.AddUnique(Slot.SlotName.ToString());
        }
    }

    if (const USkeleton* Skeleton = Montage->GetSkeleton())
    {
        for (const FAnimSlotGroup& Group : Skeleton->GetSlotGroups())
        {
            for (const FName& SlotName : Group.SlotNames)
            {
                if (!SlotName.IsNone())
                {
                    OutSkeletonSlots.AddUnique(SlotName.ToString());
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Segments
// ---------------------------------------------------------------------------

void FUnrealMCPAnimationMontageOps::FillSegmentInfo(const FAnimSegment& Segment, int32 TrackIndex, int32 SegmentIndex,
                                                    FUnrealMCPAnimSegmentInfo& OutInfo)
{
    OutInfo = FUnrealMCPAnimSegmentInfo();
    OutInfo.TrackIndex = TrackIndex;
    OutInfo.SegmentIndex = SegmentIndex;

    const UAnimSequenceBase* Anim = Segment.GetAnimReference();
    OutInfo.AnimPath = Anim ? Anim->GetPathName() : FString();
    OutInfo.AnimName = Anim ? Anim->GetName() : FString();
    OutInfo.StartPos = Segment.StartPos;
    OutInfo.Length = Segment.GetLength();
    OutInfo.EndPos = Segment.GetEndPos();
    OutInfo.AnimStartTime = Segment.AnimStartTime;
    OutInfo.AnimEndTime = Segment.AnimEndTime;
    OutInfo.PlayRate = Segment.AnimPlayRate;
    OutInfo.LoopCount = Segment.LoopingCount;
    OutInfo.bValid = Segment.IsValid();
}

void FUnrealMCPAnimationMontageOps::ListAnimSegments(UAnimMontage* Montage, int32 TrackIndex,
                                                     TArray<FUnrealMCPAnimSegmentInfo>& OutSegments,
                                                     FUnrealMCPAnimError& OutError)
{
    OutSegments.Reset();
    int32 ResolvedTrack = INDEX_NONE;
    if (!ResolveSlotTrack(Montage, TrackIndex, FString(), ResolvedTrack, OutError))
    {
        return;
    }

    const FAnimTrack& Track = Montage->SlotAnimTracks[ResolvedTrack].AnimTrack;
    OutSegments.Reserve(Track.AnimSegments.Num());
    for (int32 Index = 0; Index < Track.AnimSegments.Num(); ++Index)
    {
        FUnrealMCPAnimSegmentInfo Info;
        FillSegmentInfo(Track.AnimSegments[Index], ResolvedTrack, Index, Info);
        OutSegments.Add(Info);
    }
}

bool FUnrealMCPAnimationMontageOps::AddAnimSegment(UAnimMontage* Montage, int32 TrackIndex, const FString& AnimPath,
                                                   float StartTime, float PlayRate, int32 LoopCount,
                                                   bool bHasStartTime, bool bHasPlayRate, bool bHasLoopCount,
                                                   int32& OutSegmentIndex, FUnrealMCPAnimError& OutError)
{
    OutSegmentIndex = INDEX_NONE;

    int32 ResolvedTrack = INDEX_NONE;
    if (!ResolveSlotTrack(Montage, TrackIndex, FString(), ResolvedTrack, OutError))
    {
        return false;
    }

    if (AnimPath.IsEmpty())
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("anim_path is required"));
        return false;
    }

    UObject* Asset = FUnrealMCPCommonUtils::FindAsset(AnimPath);
    if (!Asset)
    {
        OutError.Set(EUnrealMCPAnimError::AssetNotFound, FString::Printf(TEXT("'%s' was not found"), *AnimPath));
        return false;
    }

    UAnimSequenceBase* Anim = Cast<UAnimSequenceBase>(Asset);
    if (!Anim)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams,
                     FString::Printf(TEXT("'%s' is a %s, not an animation the montage can play"),
                                     *AnimPath, *Asset->GetClass()->GetName()));
        return false;
    }
    if (Cast<UAnimMontage>(Anim))
    {
        // A montage inside a montage is recursive; the engine refuses it too, so say it in words.
        OutError.Set(EUnrealMCPAnimError::InvalidParams,
                     FString::Printf(TEXT("'%s' is itself a montage and cannot be a segment of another montage"),
                                     *AnimPath));
        return false;
    }

    FAnimTrack& Track = Montage->SlotAnimTracks[ResolvedTrack].AnimTrack;
    FText Reason;
    if (!Track.IsValidToAdd(Anim, &Reason))
    {
        OutError.Set(EUnrealMCPAnimError::InvalidValue,
                     FString::Printf(TEXT("'%s' cannot be added to slot '%s': %s"), *Anim->GetName(),
                                     *Montage->SlotAnimTracks[ResolvedTrack].SlotName.ToString(),
                                     *Reason.ToString()));
        return false;
    }

    const float ResolvedStartTime = bHasStartTime ? StartTime : 0.0f;
    if (ResolvedStartTime < 0.0f)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidValue,
                     FString::Printf(TEXT("start_time %.4f is negative"), ResolvedStartTime));
        return false;
    }

    const float ResolvedPlayRate = bHasPlayRate ? PlayRate : 1.0f;
    if (FMath::IsNearlyZero(ResolvedPlayRate))
    {
        OutError.Set(EUnrealMCPAnimError::InvalidValue, TEXT("play_rate must not be 0"));
        return false;
    }

    const int32 ResolvedLoopCount = bHasLoopCount ? LoopCount : 1;
    if (ResolvedLoopCount < 1)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidValue,
                     FString::Printf(TEXT("loop_count %d must be at least 1"), ResolvedLoopCount));
        return false;
    }

    Montage->Modify();

    FAnimSegment NewSegment;
    // SetAnimReference with bInitialize fills the segment with the whole animation at rate 1; the
    // caller's fields are applied on top so an unset parameter does not overwrite the default.
    NewSegment.SetAnimReference(Anim, /*bInitialize=*/true);
    NewSegment.StartPos = ResolvedStartTime;
    NewSegment.AnimPlayRate = ResolvedPlayRate;
    NewSegment.LoopingCount = ResolvedLoopCount;

    const int32 AddedIndex = Track.AnimSegments.Add(NewSegment);
    FinishMontageEdit(Montage, /*bRecomputeLength=*/true);

    OutSegmentIndex = FindSegmentIndex(Track, Anim, ResolvedStartTime);
    if (OutSegmentIndex == INDEX_NONE)
    {
        OutError.Set(EUnrealMCPAnimError::WriteFailed,
                     FString::Printf(TEXT("the segment was added but could not be located again in slot '%s' (added at %d)"),
                                     *Montage->SlotAnimTracks[ResolvedTrack].SlotName.ToString(), AddedIndex));
        return false;
    }

    return true;
}

bool FUnrealMCPAnimationMontageOps::RemoveAnimSegment(UAnimMontage* Montage, int32 TrackIndex, int32 SegmentIndex,
                                                      FUnrealMCPAnimError& OutError)
{
    int32 ResolvedTrack = INDEX_NONE;
    if (!ResolveSlotTrack(Montage, TrackIndex, FString(), ResolvedTrack, OutError))
    {
        return false;
    }

    FAnimTrack& Track = Montage->SlotAnimTracks[ResolvedTrack].AnimTrack;
    if (!Track.AnimSegments.IsValidIndex(SegmentIndex))
    {
        OutError.Set(EUnrealMCPAnimError::SegmentNotFound,
                     FString::Printf(TEXT("segment_index %d is out of range (%d segment(s) in slot '%s')"),
                                     SegmentIndex, Track.AnimSegments.Num(),
                                     *Montage->SlotAnimTracks[ResolvedTrack].SlotName.ToString()));
        for (int32 Index = 0; Index < Track.AnimSegments.Num(); ++Index)
        {
            OutError.Candidates.Add(FString::Printf(TEXT("%d: %s @ %.4fs"), Index,
                                                    *GetNameSafe(Track.AnimSegments[Index].GetAnimReference()),
                                                    Track.AnimSegments[Index].StartPos));
        }
        return false;
    }

    Montage->Modify();
    Track.AnimSegments.RemoveAt(SegmentIndex);
    FinishMontageEdit(Montage, /*bRecomputeLength=*/true);
    return true;
}

const TArray<FString>& FUnrealMCPAnimationMontageOps::SegmentPropertyNames()
{
    static const TArray<FString> Names = {
        TEXT("start_time"), TEXT("play_rate"), TEXT("start_position"), TEXT("end_position"), TEXT("loop_count")};
    return Names;
}

bool FUnrealMCPAnimationMontageOps::SetSegmentProperty(UAnimMontage* Montage, int32 TrackIndex, int32 SegmentIndex,
                                                       const FString& PropertyName,
                                                       const TSharedPtr<FJsonValue>& Value,
                                                       FUnrealMCPAnimError& OutError)
{
    int32 ResolvedTrack = INDEX_NONE;
    if (!ResolveSlotTrack(Montage, TrackIndex, FString(), ResolvedTrack, OutError))
    {
        return false;
    }

    FAnimTrack& Track = Montage->SlotAnimTracks[ResolvedTrack].AnimTrack;
    if (!Track.AnimSegments.IsValidIndex(SegmentIndex))
    {
        OutError.Set(EUnrealMCPAnimError::SegmentNotFound,
                     FString::Printf(TEXT("segment_index %d is out of range (%d segment(s) in slot '%s')"),
                                     SegmentIndex, Track.AnimSegments.Num(),
                                     *Montage->SlotAnimTracks[ResolvedTrack].SlotName.ToString()));
        return false;
    }

    double Number = 0.0;
    if (!Value.IsValid() || !Value->TryGetNumber(Number))
    {
        OutError.Set(EUnrealMCPAnimError::InvalidValue, TEXT("value must be a number"));
        return false;
    }

    const FString Property = PropertyName.ToLower();
    if (!SegmentPropertyNames().Contains(Property))
    {
        OutError.Set(EUnrealMCPAnimError::UnsupportedProperty,
                     FString::Printf(TEXT("'%s' is not a segment property"), *PropertyName));
        OutError.AvailableFields = SegmentPropertyNames();
        return false;
    }

    FAnimSegment& Segment = Track.AnimSegments[SegmentIndex];
    const float Number32 = static_cast<float>(Number);

    if (Property == TEXT("start_time"))
    {
        if (Number32 < 0.0f)
        {
            OutError.Set(EUnrealMCPAnimError::InvalidValue, TEXT("start_time must not be negative"));
            return false;
        }
    }
    else if (Property == TEXT("play_rate"))
    {
        if (FMath::IsNearlyZero(Number32))
        {
            OutError.Set(EUnrealMCPAnimError::InvalidValue, TEXT("play_rate must not be 0"));
            return false;
        }
    }
    else if (Property == TEXT("start_position"))
    {
        if (Number32 < 0.0f || Number32 >= Segment.AnimEndTime)
        {
            OutError.Set(EUnrealMCPAnimError::InvalidValue,
                         FString::Printf(TEXT("start_position %.4f must be between 0 and the end position %.4f"),
                                         Number32, Segment.AnimEndTime));
            return false;
        }
    }
    else if (Property == TEXT("end_position"))
    {
        if (Number32 <= Segment.AnimStartTime || Number32 > (Segment.GetAnimReference() ? Segment.GetAnimReference()->GetPlayLength() : Number32))
        {
            OutError.Set(EUnrealMCPAnimError::InvalidValue,
                         FString::Printf(TEXT("end_position %.4f must be greater than the start position %.4f and within the animation"),
                                         Number32, Segment.AnimStartTime));
            return false;
        }
    }
    else if (Property == TEXT("loop_count"))
    {
        const int32 LoopCount = FMath::RoundToInt32(Number);
        if (LoopCount < 1)
        {
            OutError.Set(EUnrealMCPAnimError::InvalidValue,
                         FString::Printf(TEXT("loop_count %d must be at least 1"), LoopCount));
            return false;
        }
    }

    Montage->Modify();

    if (Property == TEXT("start_time"))
    {
        Segment.StartPos = Number32;
    }
    else if (Property == TEXT("play_rate"))
    {
        Segment.AnimPlayRate = Number32;
    }
    else if (Property == TEXT("start_position"))
    {
        Segment.AnimStartTime = Number32;
    }
    else if (Property == TEXT("end_position"))
    {
        Segment.AnimEndTime = Number32;
    }
    else
    {
        Segment.LoopingCount = FMath::RoundToInt32(Number);
    }

    FinishMontageEdit(Montage, /*bRecomputeLength=*/true);
    return true;
}

// ---------------------------------------------------------------------------
// Notifies and branching points
// ---------------------------------------------------------------------------

void FUnrealMCPAnimationMontageOps::ListMontageNotifies(UAnimMontage* Montage, TArray<FUnrealMCPAnimNotifyInfo>& OutNotifies)
{
    OutNotifies.Reset();
    if (!Montage)
    {
        return;
    }

    OutNotifies.Reserve(Montage->Notifies.Num());
    for (int32 Index = 0; Index < Montage->Notifies.Num(); ++Index)
    {
        const FAnimNotifyEvent& Notify = Montage->Notifies[Index];

        FUnrealMCPAnimNotifyInfo Info;
        Info.NotifyIndex = Index;
        Info.NotifyName = Notify.NotifyName.ToString();
        Info.NotifyClass = NotifyClassName(Notify);
        Info.EntryType = Notify.NotifyStateClass ? TEXT("notify_state") : TEXT("notify");
        Info.TriggerTime = Notify.GetTriggerTime();
        Info.Duration = Notify.GetDuration();
        Info.EndTime = Info.TriggerTime + Info.Duration;
        Info.TrackIndex = Notify.TrackIndex;
        if (Montage->AnimNotifyTracks.IsValidIndex(Notify.TrackIndex))
        {
            const FName TrackName = Montage->AnimNotifyTracks[Notify.TrackIndex].TrackName;
            Info.TrackName = TrackName.IsNone() ? FString() : TrackName.ToString();
        }
        Info.TriggerChance = Notify.NotifyTriggerChance;
        Info.WeightThreshold = Notify.TriggerWeightThreshold;
        Info.bTriggerOnServer = Notify.bTriggerOnDedicatedServer;
        Info.bTriggerOnFollower = Notify.bTriggerOnFollower;
        Info.FilterType = FUnrealMCPAnimationOps::FilterTypeToString(static_cast<int32>(Notify.NotifyFilterType.GetValue()));
        Info.FilterLOD = Notify.NotifyFilterLOD;
        Info.Color = FString::Printf(TEXT("#%s"), *Notify.NotifyColor.ToHex());
        Info.Guid = Notify.Guid.ToString();
        Info.bIsBranchingPoint = IsBranchingPointNotify(Notify);

        const int32 SectionIndex = Montage->GetSectionIndexFromPosition(Info.TriggerTime);
        if (SectionIndex != INDEX_NONE)
        {
            Info.LinkedSectionName = Montage->GetSectionName(SectionIndex).ToString();
        }

        OutNotifies.Add(Info);
    }
}

bool FUnrealMCPAnimationMontageOps::ResolveMontageNotify(UAnimMontage* Montage, int32 NotifyIndex,
                                                         const FString& NotifyName, const FString& Guid, float Time,
                                                         bool bHasTime, int32& OutNotifyIndex,
                                                         FUnrealMCPAnimError& OutError)
{
    OutNotifyIndex = INDEX_NONE;
    if (!Montage)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
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

        for (int32 Index = 0; Index < Montage->Notifies.Num(); ++Index)
        {
            if (Montage->Notifies[Index].Guid == ParsedGuid)
            {
                OutNotifyIndex = Index;
                return true;
            }
        }

        OutError.Set(EUnrealMCPAnimError::NotifyNotFound,
                     FString::Printf(TEXT("no notify with guid '%s' on '%s'"), *Guid, *Montage->GetName()));
        return false;
    }

    if (!NotifyName.IsEmpty())
    {
        TArray<int32> Matches;
        for (int32 Index = 0; Index < Montage->Notifies.Num(); ++Index)
        {
            const FAnimNotifyEvent& Notify = Montage->Notifies[Index];
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

        for (int32 Index = 0; Index < Montage->Notifies.Num(); ++Index)
        {
            OutError.Candidates.Add(FString::Printf(TEXT("%d: %s @ %.4fs"), Index,
                                                    *Montage->Notifies[Index].NotifyName.ToString(),
                                                    Montage->Notifies[Index].GetTriggerTime()));
        }

        OutError.Set(Matches.Num() == 0 ? EUnrealMCPAnimError::NotifyNotFound : EUnrealMCPAnimError::AmbiguousNotify,
                     Matches.Num() == 0
                         ? FString::Printf(TEXT("no notify named '%s' on '%s'"), *NotifyName, *Montage->GetName())
                         : FString::Printf(TEXT("'%s' matches %d notifies on '%s'; pass notify_index or 'time'"),
                                           *NotifyName, Matches.Num(), *Montage->GetName()));
        return false;
    }

    if (!Montage->Notifies.IsValidIndex(NotifyIndex))
    {
        OutError.Set(EUnrealMCPAnimError::NotifyNotFound,
                     FString::Printf(TEXT("notify_index %d is out of range (%d notify(ies) on '%s')"),
                                     NotifyIndex, Montage->Notifies.Num(), *Montage->GetName()));
        for (int32 Index = 0; Index < Montage->Notifies.Num(); ++Index)
        {
            OutError.Candidates.Add(FString::Printf(TEXT("%d: %s @ %.4fs"), Index,
                                                    *Montage->Notifies[Index].NotifyName.ToString(),
                                                    Montage->Notifies[Index].GetTriggerTime()));
        }
        return false;
    }

    OutNotifyIndex = NotifyIndex;
    return true;
}

bool FUnrealMCPAnimationMontageOps::AddMontageNotify(UAnimMontage* Montage, const FString& NotifyName,
                                                     const FString& NotifyClassName, float Time, float Duration,
                                                     int32 TrackIndex, bool bState, int32& OutNotifyIndex,
                                                     FUnrealMCPAnimError& OutError)
{
    OutNotifyIndex = INDEX_NONE;
    if (!Montage)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }
    if (Time < 0.0f || (Montage->GetPlayLength() > 0.0f && Time > Montage->GetPlayLength()))
    {
        OutError.Set(EUnrealMCPAnimError::InvalidValue,
                     FString::Printf(TEXT("trigger_time %.4f is outside [0, %.4f]"),
                                     Time, Montage->GetPlayLength()));
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

        // UAnimNotify / UAnimNotifyState are abstract in 5.5: instancing them is fatal, so an abstract
        // class is reported instead of leaving a null notify behind.
        if (NotifyClass->HasAnyClassFlags(CLASS_Abstract))
        {
            OutError.Set(EUnrealMCPAnimError::InvalidParams,
                         FString::Printf(TEXT("class '%s' is abstract and cannot be instanced; pass a concrete subclass or omit notify_class"),
                                         *NotifyClass->GetName()));
            return false;
        }
    }

    Montage->Modify();
    Montage->InitializeNotifyTrack();

    FAnimNotifyEvent& NewNotify = Montage->Notifies.AddDefaulted_GetRef();
    const FName FinalName = NotifyName.IsEmpty()
        ? (NotifyClass ? FName(*NotifyClass->GetName()) : FName(bState ? TEXT("NotifyState") : TEXT("Notify")))
        : FName(*NotifyName);
    NewNotify.NotifyName = FinalName;
    NewNotify.Link(Montage, Time);
    NewNotify.TriggerTimeOffset = 0.0f;
    NewNotify.TrackIndex = TrackIndex > 0 && Montage->AnimNotifyTracks.IsValidIndex(TrackIndex) ? TrackIndex : 0;
    NewNotify.Guid = FGuid::NewGuid();
    NewNotify.NotifyColor = FColor::White;

    if (NotifyClass)
    {
        if (bState)
        {
            NewNotify.NotifyStateClass = NewObject<UAnimNotifyState>(Montage, NotifyClass, FinalName, RF_Transactional);
        }
        else
        {
            NewNotify.Notify = NewObject<UAnimNotify>(Montage, NotifyClass, FinalName, RF_Transactional);
        }
    }

    if (bState)
    {
        NewNotify.SetDuration(FMath::Max(0.0f, Duration));
    }

    const FGuid AssignedGuid = NewNotify.Guid;
    Montage->RefreshCacheData();
    Montage->MarkPackageDirty();

    for (int32 Index = 0; Index < Montage->Notifies.Num(); ++Index)
    {
        if (Montage->Notifies[Index].Guid == AssignedGuid)
        {
            OutNotifyIndex = Index;
            return true;
        }
    }

    OutError.Set(EUnrealMCPAnimError::WriteFailed,
                 FString::Printf(TEXT("the notify was added but could not be located again on '%s'"), *Montage->GetName()));
    return false;
}

bool FUnrealMCPAnimationMontageOps::RemoveMontageNotify(UAnimMontage* Montage, int32 NotifyIndex,
                                                        FUnrealMCPAnimError& OutError)
{
    if (!Montage || !Montage->Notifies.IsValidIndex(NotifyIndex))
    {
        OutError.Set(EUnrealMCPAnimError::NotifyNotFound,
                     FString::Printf(TEXT("notify_index %d is out of range"), NotifyIndex));
        return false;
    }

    Montage->Modify();
    Montage->Notifies.RemoveAt(NotifyIndex);
    Montage->RefreshCacheData();
    Montage->MarkPackageDirty();
    return true;
}

bool FUnrealMCPAnimationMontageOps::SetMontageNotifyTime(UAnimMontage* Montage, int32 NotifyIndex, float Time,
                                                         bool bHasTime, const FString& SectionName,
                                                         FUnrealMCPAnimError& OutError)
{
    if (!Montage || !Montage->Notifies.IsValidIndex(NotifyIndex))
    {
        OutError.Set(EUnrealMCPAnimError::NotifyNotFound,
                     FString::Printf(TEXT("notify_index %d is out of range"), NotifyIndex));
        return false;
    }

    float NewTime = Time;
    if (!SectionName.IsEmpty())
    {
        if (!FindSectionStartTime(Montage, SectionName, NewTime))
        {
            OutError.Set(EUnrealMCPAnimError::SectionNotFound,
                         FString::Printf(TEXT("no section named '%s' on '%s'"), *SectionName, *Montage->GetName()));
            for (int32 I = 0; I < Montage->CompositeSections.Num(); ++I)
            {
                OutError.Candidates.Add(Montage->CompositeSections[I].SectionName.ToString());
            }
            return false;
        }
    }
    else if (!bHasTime)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("pass 'trigger_time' or 'section'"));
        return false;
    }

    if (NewTime < 0.0f || (Montage->GetPlayLength() > 0.0f && NewTime > Montage->GetPlayLength()))
    {
        OutError.Set(EUnrealMCPAnimError::InvalidValue,
                     FString::Printf(TEXT("trigger_time %.4f is outside [0, %.4f]"),
                                     NewTime, Montage->GetPlayLength()));
        return false;
    }

    Montage->Modify();
    Montage->Notifies[NotifyIndex].Link(Montage, NewTime);
    Montage->Notifies[NotifyIndex].TriggerTimeOffset = 0.0f;
    Montage->RefreshCacheData();
    Montage->MarkPackageDirty();
    return true;
}

void FUnrealMCPAnimationMontageOps::ListBranchingPoints(UAnimMontage* Montage,
                                                        TArray<FUnrealMCPAnimBranchingPointInfo>& OutPoints)
{
    OutPoints.Reset();
    if (!Montage)
    {
        return;
    }

    for (int32 Index = 0; Index < Montage->Notifies.Num(); ++Index)
    {
        const FAnimNotifyEvent& Notify = Montage->Notifies[Index];
        if (!IsBranchingPointNotify(Notify))
        {
            continue;
        }

        FUnrealMCPAnimBranchingPointInfo Info;
        Info.BranchingPointIndex = OutPoints.Num();
        Info.NotifyIndex = Index;
        Info.NotifyName = Notify.NotifyName.ToString();
        Info.NotifyClass = NotifyClassName(Notify);
        Info.TriggerTime = Notify.GetTriggerTime();
        Info.TrackIndex = Notify.TrackIndex;

        const int32 SectionIndex = Montage->GetSectionIndexFromPosition(Info.TriggerTime);
        if (SectionIndex != INDEX_NONE)
        {
            Info.SectionName = Montage->GetSectionName(SectionIndex).ToString();
        }

        OutPoints.Add(Info);
    }
}

bool FUnrealMCPAnimationMontageOps::AddBranchingPoint(UAnimMontage* Montage, const FString& NotifyName, float Time,
                                                      int32 TrackIndex, int32& OutNotifyIndex,
                                                      FUnrealMCPAnimError& OutError)
{
    OutNotifyIndex = INDEX_NONE;
    if (!Montage)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }
    if (NotifyName.IsEmpty())
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("name is required"));
        return false;
    }
    if (Time < 0.0f || (Montage->GetPlayLength() > 0.0f && Time > Montage->GetPlayLength()))
    {
        OutError.Set(EUnrealMCPAnimError::InvalidValue,
                     FString::Printf(TEXT("trigger_time %.4f is outside [0, %.4f]"),
                                     Time, Montage->GetPlayLength()));
        return false;
    }

    Montage->Modify();
    Montage->InitializeNotifyTrack();

    FAnimNotifyEvent& NewNotify = Montage->Notifies.AddDefaulted_GetRef();
    NewNotify.NotifyName = FName(*NotifyName);
    NewNotify.Link(Montage, Time);
    NewNotify.TriggerTimeOffset = 0.0f;
    NewNotify.MontageTickType = EMontageNotifyTickType::BranchingPoint;
    NewNotify.TrackIndex = TrackIndex > 0 && Montage->AnimNotifyTracks.IsValidIndex(TrackIndex) ? TrackIndex : 0;
    NewNotify.Guid = FGuid::NewGuid();
    NewNotify.NotifyColor = FColor::White;

    const FGuid AssignedGuid = NewNotify.Guid;
    Montage->RefreshCacheData();
    Montage->MarkPackageDirty();

    for (int32 Index = 0; Index < Montage->Notifies.Num(); ++Index)
    {
        if (Montage->Notifies[Index].Guid == AssignedGuid)
        {
            OutNotifyIndex = Index;
            return true;
        }
    }

    OutError.Set(EUnrealMCPAnimError::WriteFailed,
                 FString::Printf(TEXT("the branching point was added but could not be located again on '%s'"),
                                 *Montage->GetName()));
    return false;
}

bool FUnrealMCPAnimationMontageOps::RemoveBranchingPoint(UAnimMontage* Montage, int32 BranchingPointIndex,
                                                         FUnrealMCPAnimError& OutError)
{
    if (!Montage)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }

    int32 Seen = 0;
    for (int32 Index = 0; Index < Montage->Notifies.Num(); ++Index)
    {
        if (!IsBranchingPointNotify(Montage->Notifies[Index]))
        {
            continue;
        }

        if (Seen == BranchingPointIndex)
        {
            Montage->Modify();
            Montage->Notifies.RemoveAt(Index);
            Montage->RefreshCacheData();
            Montage->MarkPackageDirty();
            return true;
        }
        ++Seen;
    }

    OutError.Set(EUnrealMCPAnimError::BranchingPointNotFound,
                 FString::Printf(TEXT("branching_point_index %d is out of range (%d branching point(s) on '%s')"),
                                 BranchingPointIndex, Seen, *Montage->GetName()));
    for (int32 Index = 0; Index < Montage->Notifies.Num(); ++Index)
    {
        if (IsBranchingPointNotify(Montage->Notifies[Index]))
        {
            OutError.Candidates.Add(FString::Printf(TEXT("%s @ %.4fs"), *Montage->Notifies[Index].NotifyName.ToString(),
                                                    Montage->Notifies[Index].GetTriggerTime()));
        }
    }
    return false;
}

bool FUnrealMCPAnimationMontageOps::IsBranchingPointAtTime(UAnimMontage* Montage, float Time, FString& OutNotifyName)
{
    OutNotifyName.Reset();
    if (!Montage)
    {
        return false;
    }

    for (const FAnimNotifyEvent& Notify : Montage->Notifies)
    {
        if (IsBranchingPointNotify(Notify) && FMath::IsNearlyEqual(Notify.GetTriggerTime(), Time, TimeTolerance))
        {
            OutNotifyName = Notify.NotifyName.ToString();
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Blend settings and root motion
// ---------------------------------------------------------------------------

void FUnrealMCPAnimationMontageOps::GetBlendInfo(UAnimMontage* Montage, FUnrealMCPAnimBlendInfo& OutInfo)
{
    OutInfo = FUnrealMCPAnimBlendInfo();
    if (!Montage)
    {
        return;
    }

    OutInfo.BlendInTime = Montage->BlendIn.GetBlendTime();
    OutInfo.BlendInOption = BlendOptionToString(static_cast<int32>(Montage->BlendIn.GetBlendOption()));
    OutInfo.BlendInMode = BlendModeToString(static_cast<int32>(Montage->BlendModeIn));
    OutInfo.BlendOutTime = Montage->BlendOut.GetBlendTime();
    OutInfo.BlendOutOption = BlendOptionToString(static_cast<int32>(Montage->BlendOut.GetBlendOption()));
    OutInfo.BlendOutMode = BlendModeToString(static_cast<int32>(Montage->BlendModeOut));
    OutInfo.BlendOutTriggerTime = Montage->BlendOutTriggerTime;
    OutInfo.bEnableAutoBlendOut = Montage->bEnableAutoBlendOut;
}

bool FUnrealMCPAnimationMontageOps::SetBlendSettings(UAnimMontage* Montage, bool bBlendIn, bool bHasTime,
                                                     float BlendTime, bool bHasOption, const FString& BlendOption,
                                                     FUnrealMCPAnimError& OutError)
{
    if (!Montage)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }
    if (!bHasTime && !bHasOption)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("pass blend_time and/or blend_option"));
        return false;
    }

    int32 Option = 0;
    if (bHasOption && !StringToBlendOption(BlendOption, Option))
    {
        OutError.Set(EUnrealMCPAnimError::InvalidBlendOption,
                     FString::Printf(TEXT("'%s' is not an alpha blend option"), *BlendOption));
        OutError.Candidates = BlendOptionNames();
        return false;
    }

    if (bHasTime && BlendTime < 0.0f)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidValue,
                     FString::Printf(TEXT("blend_time %.4f is negative"), BlendTime));
        return false;
    }

    Montage->Modify();
    FAlphaBlend& Blend = bBlendIn ? Montage->BlendIn : Montage->BlendOut;
    if (bHasTime)
    {
        Blend.SetBlendTime(BlendTime);
    }
    if (bHasOption)
    {
        Blend.SetBlendOption(static_cast<EAlphaBlendOption>(Option));
    }

    Montage->MarkPackageDirty();
    return true;
}

bool FUnrealMCPAnimationMontageOps::SetBlendOutTriggerTime(UAnimMontage* Montage, float TriggerTime,
                                                           FUnrealMCPAnimError& OutError)
{
    if (!Montage)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }

    Montage->Modify();
    // A negative value is meaningful (it selects BlendOutTime instead), so nothing is rejected here.
    Montage->BlendOutTriggerTime = TriggerTime;
    Montage->MarkPackageDirty();
    return true;
}

void FUnrealMCPAnimationMontageOps::GetEnableRootMotion(UAnimMontage* Montage, bool& bOutTranslation, bool& bOutRotation)
{
    bOutTranslation = false;
    bOutRotation = false;
    if (!Montage)
    {
        return;
    }

    bOutTranslation = Montage->bEnableRootMotionTranslation;
    bOutRotation = Montage->bEnableRootMotionRotation;
}

bool FUnrealMCPAnimationMontageOps::SetEnableRootMotion(UAnimMontage* Montage, bool bTranslation, bool bEnable,
                                                        FUnrealMCPAnimError& OutError)
{
    if (!Montage)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }

    Montage->Modify();
    if (bTranslation)
    {
        Montage->bEnableRootMotionTranslation = bEnable;
    }
    else
    {
        Montage->bEnableRootMotionRotation = bEnable;
    }
    Montage->MarkPackageDirty();
    return true;
}

bool FUnrealMCPAnimationMontageOps::SampleRootMotion(UAnimMontage* Montage, float Time, FTransform& OutTransform,
                                                     FUnrealMCPAnimError& OutError)
{
    OutTransform = FTransform::Identity;
    if (!Montage)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }

    const float Length = Montage->GetPlayLength();
    if (Time < 0.0f || (Length > 0.0f && Time > Length))
    {
        OutError.Set(EUnrealMCPAnimError::InvalidValue,
                     FString::Printf(TEXT("time %.4f is outside [0, %.4f]"), Time, Length));
        return false;
    }

    // Accumulated from the start of the montage to Time, the same meaning the sequence command has.
    OutTransform = Montage->ExtractRootMotionFromTrackRange(0.0f, Time);
    return true;
}

// ---------------------------------------------------------------------------
// Creation
// ---------------------------------------------------------------------------

bool FUnrealMCPAnimationMontageOps::CreateMontageFromAnimation(const FString& AnimationPath, const FString& Name,
                                                               const FString& Folder, FString& OutAssetPath,
                                                               FUnrealMCPAnimError& OutError)
{
    OutAssetPath.Reset();

    if (Name.IsEmpty())
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("name is required"));
        return false;
    }

    UObject* Asset = FUnrealMCPCommonUtils::FindAsset(AnimationPath);
    if (!Asset)
    {
        OutError.Set(EUnrealMCPAnimError::AssetNotFound,
                     FString::Printf(TEXT("'%s' was not found"), *AnimationPath));
        return false;
    }

    UAnimSequence* Animation = Cast<UAnimSequence>(Asset);
    if (!Animation)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams,
                     FString::Printf(TEXT("'%s' is a %s; a montage can only be built from an AnimSequence"),
                                     *AnimationPath, *Asset->GetClass()->GetName()));
        return false;
    }

    const FString AssetFolder = Folder.IsEmpty() ? TEXT("/Game") : Folder;
    const FString PackagePath = AssetFolder + TEXT("/") + Name;
    if (UObject* Existing = FUnrealMCPCommonUtils::FindAsset(PackagePath))
    {
        // A deleted-but-never-saved asset survives in memory; that leftover must not block a create.
        const bool bLeftover = !IsValid(Existing) || Existing->IsUnreachable();
        if (bLeftover)
        {
            Existing->MarkAsGarbage();
            CollectGarbage(RF_NoFlags);
        }
        else
        {
            OutError.Set(EUnrealMCPAnimError::AssetExists,
                         FString::Printf(TEXT("'%s' already exists; delete it (safe_delete_asset) or pick another name"),
                                         *PackagePath));
            return false;
        }
    }

    UAnimMontageFactory* Factory = NewObject<UAnimMontageFactory>();
    Factory->SourceAnimation = Animation;
    Factory->TargetSkeleton = Animation->GetSkeleton();

    // Not IAssetTools::CreateAsset: its CanCreateAsset step fully loads the target package, which
    // never returns for a path whose file was deleted in this session.
    UAnimMontage* NewMontage = Cast<UAnimMontage>(
        FUnrealMCPCommonUtils::CreateAssetDirect(Name, AssetFolder, UAnimMontage::StaticClass(), Factory));
    if (!NewMontage)
    {
        OutError.Set(EUnrealMCPAnimError::CreateFailed,
                     FString::Printf(TEXT("the asset factory did not produce a montage at '%s'"), *PackagePath));
        return false;
    }

    // A montage without a section has nowhere to put a segment, so the starting section is guaranteed
    // here even if a future factory stops adding it.
    UAnimMontageFactory::EnsureStartingSection(NewMontage);
    NewMontage->MarkPackageDirty();

    OutAssetPath = NewMontage->GetPathName();
    return true;
}

bool FUnrealMCPAnimationMontageOps::CreateEmptyMontage(const FString& Name, const FString& Folder,
                                                       const FString& SkeletonPath, FString& OutAssetPath,
                                                       FUnrealMCPAnimError& OutError)
{
    OutAssetPath.Reset();

    if (Name.IsEmpty())
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("name is required"));
        return false;
    }

    USkeleton* Skeleton = nullptr;
    if (!FUnrealMCPAnimationOps::ResolveSkeleton(SkeletonPath, Skeleton, OutError))
    {
        return false;
    }

    const FString AssetFolder = Folder.IsEmpty() ? TEXT("/Game") : Folder;
    const FString PackagePath = AssetFolder + TEXT("/") + Name;
    if (UObject* Existing = FUnrealMCPCommonUtils::FindAsset(PackagePath))
    {
        const bool bLeftover = !IsValid(Existing) || Existing->IsUnreachable();
        if (bLeftover)
        {
            Existing->MarkAsGarbage();
            CollectGarbage(RF_NoFlags);
        }
        else
        {
            OutError.Set(EUnrealMCPAnimError::AssetExists,
                         FString::Printf(TEXT("'%s' already exists; delete it (safe_delete_asset) or pick another name"),
                                         *PackagePath));
            return false;
        }
    }

    UAnimMontageFactory* Factory = NewObject<UAnimMontageFactory>();
    Factory->TargetSkeleton = Skeleton;

    // Not IAssetTools::CreateAsset: its CanCreateAsset step fully loads the target package, which
    // never returns for a path whose file was deleted in this session.
    UAnimMontage* NewMontage = Cast<UAnimMontage>(
        FUnrealMCPCommonUtils::CreateAssetDirect(Name, AssetFolder, UAnimMontage::StaticClass(), Factory));
    if (!NewMontage)
    {
        OutError.Set(EUnrealMCPAnimError::CreateFailed,
                     FString::Printf(TEXT("the asset factory did not produce a montage at '%s'"), *PackagePath));
        return false;
    }

    UAnimMontageFactory::EnsureStartingSection(NewMontage);
    NewMontage->MarkPackageDirty();

    OutAssetPath = NewMontage->GetPathName();
    return true;
}

bool FUnrealMCPAnimationMontageOps::DuplicateMontage(const FString& SourcePath, const FString& Name,
                                                     const FString& Folder, FString& OutAssetPath,
                                                     FUnrealMCPAnimError& OutError)
{
    OutAssetPath.Reset();

    UAnimMontage* Source = nullptr;
    if (!ResolveMontage(SourcePath, Source, OutError))
    {
        return false;
    }
    if (Name.IsEmpty())
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("name is required"));
        return false;
    }

    const FString AssetFolder = Folder.IsEmpty() ? TEXT("/Game") : Folder;
    const FString PackagePath = AssetFolder + TEXT("/") + Name;
    if (UObject* Existing = FUnrealMCPCommonUtils::FindAsset(PackagePath))
    {
        const bool bLeftover = !IsValid(Existing) || Existing->IsUnreachable();
        if (bLeftover)
        {
            Existing->MarkAsGarbage();
            CollectGarbage(RF_NoFlags);
        }
        else
        {
            OutError.Set(EUnrealMCPAnimError::AssetExists,
                         FString::Printf(TEXT("'%s' already exists; delete it (safe_delete_asset) or pick another name"),
                                         *PackagePath));
            return false;
        }
    }

    // Same reason as CreateAssetDirect: IAssetTools::DuplicateAsset also runs CanCreateAsset, whose
    // fully-load step never returns for a path whose file was deleted in this session. The copy is
    // made into a package this code creates, which is what DuplicateAsset ends up doing as well.
    UPackage* Package = CreatePackage(*PackagePath);
    UAnimMontage* NewMontage = Package
        ? Cast<UAnimMontage>(StaticDuplicateObject(Source, Package, FName(*Name)))
        : nullptr;
    if (NewMontage)
    {
        NewMontage->SetFlags(RF_Public | RF_Standalone);
        FAssetRegistryModule::AssetCreated(NewMontage);
        Package->MarkPackageDirty();
    }
    if (!NewMontage)
    {
        OutError.Set(EUnrealMCPAnimError::CreateFailed,
                     FString::Printf(TEXT("the asset tools did not duplicate '%s' into '%s'"),
                                     *Source->GetName(), *PackagePath));
        return false;
    }

    UAnimMontageFactory::EnsureStartingSection(NewMontage);
    NewMontage->MarkPackageDirty();

    OutAssetPath = NewMontage->GetPathName();
    return true;
}

// ---------------------------------------------------------------------------
// String conversions
// ---------------------------------------------------------------------------

const TArray<FString>& FUnrealMCPAnimationMontageOps::BlendOptionNames()
{
    static const TArray<FString> Names = []()
    {
        TArray<FString> Result;
        for (const FBlendOptionName& Entry : BlendOptionTable())
        {
            Result.Add(Entry.Name);
        }
        return Result;
    }();
    return Names;
}

FString FUnrealMCPAnimationMontageOps::BlendOptionToString(int32 Option)
{
    for (const FBlendOptionName& Entry : BlendOptionTable())
    {
        if (static_cast<int32>(Entry.Option) == Option)
        {
            return Entry.Name;
        }
    }
    return FString::Printf(TEXT("Unknown(%d)"), Option);
}

bool FUnrealMCPAnimationMontageOps::StringToBlendOption(const FString& Text, int32& OutOption)
{
    for (const FBlendOptionName& Entry : BlendOptionTable())
    {
        if (Text.Equals(Entry.Name, ESearchCase::IgnoreCase))
        {
            OutOption = static_cast<int32>(Entry.Option);
            return true;
        }
    }
    return false;
}

FString FUnrealMCPAnimationMontageOps::BlendModeToString(int32 Mode)
{
    return Mode == static_cast<int32>(EMontageBlendMode::Inertialization) ? TEXT("Inertialization")
                                                                         : TEXT("Standard");
}

FString FUnrealMCPAnimationMontageOps::LinkMethodToString(int32 Method)
{
    switch (static_cast<EAnimLinkMethod::Type>(Method))
    {
    case EAnimLinkMethod::Relative:
        return TEXT("relative");
    case EAnimLinkMethod::Proportional:
        return TEXT("proportional");
    default:
        return TEXT("absolute");
    }
}
