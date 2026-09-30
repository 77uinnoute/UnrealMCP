#pragma once

#include "CoreMinimal.h"
#include "Json.h"
#include "Core/MCPCommandRegistry.h"

class UAnimSequence;
class UAnimMontage;
class USkeleton;
struct FAnimSegment;

/**
 * Structured error codes of the animation commands. One set of codes so the same failure
 * reads the same way from every command (mirrors the particle domain's convention).
 */
namespace EUnrealMCPAnimError
{
    inline const TCHAR* AssetNotFound        = TEXT("asset_not_found");
    inline const TCHAR* AssetNotAnimSequence = TEXT("asset_not_anim_sequence");
    inline const TCHAR* AssetNotSkeleton     = TEXT("asset_not_skeleton");
    inline const TCHAR* AssetExists          = TEXT("asset_exists");
    inline const TCHAR* CreateFailed         = TEXT("create_failed");
    inline const TCHAR* InvalidParams        = TEXT("invalid_params");
    inline const TCHAR* InvalidValue         = TEXT("invalid_value");
    inline const TCHAR* UnknownValue         = TEXT("unknown_value");
    inline const TCHAR* BoneNotFound         = TEXT("bone_not_found");
    inline const TCHAR* BoneTrackNotFound    = TEXT("bone_track_not_found");
    inline const TCHAR* BoneTrackExists      = TEXT("bone_track_exists");
    inline const TCHAR* CurveNotFound        = TEXT("curve_not_found");
    inline const TCHAR* CurveExists          = TEXT("curve_exists");
    inline const TCHAR* NotifyNotFound       = TEXT("notify_not_found");
    inline const TCHAR* AmbiguousNotify      = TEXT("ambiguous_notify");
    inline const TCHAR* NotifyTrackNotFound  = TEXT("notify_track_not_found");
    inline const TCHAR* LastNotifyTrack      = TEXT("last_notify_track");
    inline const TCHAR* NotifyTrackNameTaken = TEXT("notify_track_name_taken");
    inline const TCHAR* NotANotifyState      = TEXT("not_a_notify_state");
    inline const TCHAR* SyncMarkerNotFound   = TEXT("sync_marker_not_found");
    inline const TCHAR* CompressionFailed    = TEXT("compression_failed");
    inline const TCHAR* IncompatibleFrameRate = TEXT("incompatible_frame_rate");
    inline const TCHAR* UnsupportedProperty  = TEXT("unsupported_property");
    inline const TCHAR* WriteFailed          = TEXT("write_failed");

    // montage
    inline const TCHAR* AssetNotAnimMontage  = TEXT("asset_not_anim_montage");
    inline const TCHAR* SectionNotFound      = TEXT("section_not_found");
    inline const TCHAR* SectionNameTaken     = TEXT("section_name_taken");
    inline const TCHAR* LastSection          = TEXT("last_section");
    inline const TCHAR* SlotTrackNotFound    = TEXT("slot_track_not_found");
    inline const TCHAR* SlotNameTaken        = TEXT("slot_name_taken");
    inline const TCHAR* LastSlotTrack        = TEXT("last_slot_track");
    inline const TCHAR* SegmentNotFound      = TEXT("segment_not_found");
    inline const TCHAR* BranchingPointNotFound = TEXT("branching_point_not_found");
    inline const TCHAR* InvalidBlendOption   = TEXT("invalid_blend_option");

    // editor session
    inline const TCHAR* EditorNotOpen        = TEXT("editor_not_open");
}

/** One failure, with the candidates that let a caller retry without guessing. */
struct FUnrealMCPAnimError
{
    FString Code;
    FString Message;
    TArray<FString> Candidates;
    TArray<FString> AvailableFields;

    bool IsError() const { return !Code.IsEmpty(); }

    void Set(const TCHAR* InCode, const FString& InMessage)
    {
        Code = InCode;
        Message = InMessage;
    }
};

/** Summary of one animation sequence, filled from the asset registry or from the asset. */
struct FUnrealMCPAnimSequenceInfo
{
    FString AssetPath;
    FString AssetName;
    FString SkeletonPath;
    float Length = 0.0f;
    float FrameRate = 30.0f;
    int32 FrameCount = 0;
    int32 BoneTrackCount = 0;
    int32 CurveCount = 0;
    int32 NotifyCount = 0;
    int32 NotifyTrackCount = 0;
    int32 SyncMarkerCount = 0;
    float RateScale = 1.0f;
    bool bEnableRootMotion = false;
    bool bForceRootLock = false;
    FString RootMotionRootLock;
    FString AdditiveAnimType;
    FString AdditiveBasePose;
    int64 RawSize = 0;
    int32 CompressedSize = 0;
    FString CompressionScheme;
    /** "asset" when every field was read from the loaded asset, "registry" for tag-only data. */
    FString InfoSource = TEXT("asset");
};

/** One float curve of a sequence. */
struct FUnrealMCPAnimCurveInfo
{
    FString CurveName;
    int32 KeyCount = 0;
    float DefaultValue = 0.0f;
    int32 CurveTypeFlags = 0;
    bool bEditable = false;
    bool bMetadata = false;
    bool bDisabled = false;
};

/** One key of a float curve. */
struct FUnrealMCPAnimCurveKey
{
    float Time = 0.0f;
    float Value = 0.0f;
    float ArriveTangent = 0.0f;
    float LeaveTangent = 0.0f;
    /** "constant" / "linear" / "cubic" / "none" */
    FString InterpMode;
    /** "auto" / "user" / "break" / "none" */
    FString TangentMode;
};

/** One notify (instant or state) of a sequence. */
struct FUnrealMCPAnimNotifyInfo
{
    int32 NotifyIndex = INDEX_NONE;
    FString NotifyName;
    FString NotifyClass;
    /** "notify" for an instant notify, "notify_state" for a state. */
    FString EntryType;
    float TriggerTime = 0.0f;
    float EndTime = 0.0f;
    float Duration = 0.0f;
    int32 TrackIndex = 0;
    FString TrackName;
    float TriggerChance = 1.0f;
    float WeightThreshold = 0.0f;
    bool bTriggerOnServer = true;
    bool bTriggerOnFollower = false;
    FString FilterType;
    int32 FilterLOD = 0;
    FString Color;
    FString Guid;
    /** Montage only: name of the section the notify's trigger time falls into (derived, not stored). */
    FString LinkedSectionName;
    /** Montage only: true when the notify is a branching point (`MontageTickType == BranchingPoint`). */
    bool bIsBranchingPoint = false;
};

/** One notify track of a sequence. */
struct FUnrealMCPAnimNotifyTrackInfo
{
    int32 TrackIndex = 0;
    FString TrackName;
    FString Color;
    int32 NotifyCount = 0;
    int32 SyncMarkerCount = 0;
};

/** One sync marker of a sequence. */
struct FUnrealMCPAnimSyncMarkerInfo
{
    FString MarkerName;
    float Time = 0.0f;
    int32 TrackIndex = 0;
};

/** One bone's local transform in a sampled pose. */
struct FUnrealMCPAnimBonePose
{
    FString BoneName;
    int32 BoneIndex = INDEX_NONE;
    FTransform Transform;
};

/** One authored key of a bone track. */
struct FUnrealMCPAnimKeyframe
{
    float Time = 0.0f;
    FVector Position = FVector::ZeroVector;
    FQuat Rotation = FQuat::Identity;
    FVector Scale = FVector::OneVector;
};

/** Compression state of a sequence. */
struct FUnrealMCPAnimCompressionInfo
{
    FString CompressionScheme;
    int64 RawSize = 0;
    int32 CompressedSize = 0;
    double Ratio = 0.0;
    FString CurveCompressionScheme;
};

/** Summary of one animation montage. */
struct FUnrealMCPAnimMontageInfo
{
    FString AssetPath;
    FString AssetName;
    FString SkeletonPath;
    float Length = 0.0f;
    int32 SectionCount = 0;
    int32 SlotTrackCount = 0;
    int32 SegmentCount = 0;
    int32 NotifyCount = 0;
    int32 BranchingPointCount = 0;
    float RateScale = 1.0f;
    float BlendInTime = 0.0f;
    FString BlendInOption;
    FString BlendInMode;
    float BlendOutTime = 0.0f;
    FString BlendOutOption;
    FString BlendOutMode;
    float BlendOutTriggerTime = 0.0f;
    bool bEnableAutoBlendOut = true;
    bool bEnableRootMotionTranslation = false;
    bool bEnableRootMotionRotation = false;
    FString SyncGroup;
    /** "asset" when every field was read from the loaded asset, "registry" for tag-only data. */
    FString InfoSource = TEXT("asset");
};

/** One composite section of a montage. */
struct FUnrealMCPAnimSectionInfo
{
    int32 SectionIndex = INDEX_NONE;
    FString SectionName;
    float StartTime = 0.0f;
    float EndTime = 0.0f;
    float Length = 0.0f;
    FString NextSectionName;
    /** True when the section points at itself (the montage editor's "loop this section"). */
    bool bIsLooping = false;
    int32 SlotIndex = 0;
    int32 SegmentIndex = INDEX_NONE;
    FString LinkMethod;
};

/** One slot track of a montage. */
struct FUnrealMCPAnimSlotTrackInfo
{
    int32 TrackIndex = INDEX_NONE;
    FString SlotName;
    FString GroupName;
    int32 SegmentCount = 0;
    float TrackLength = 0.0f;
};

/** One animation segment inside a slot track. */
struct FUnrealMCPAnimSegmentInfo
{
    int32 TrackIndex = INDEX_NONE;
    int32 SegmentIndex = INDEX_NONE;
    FString AnimPath;
    FString AnimName;
    float StartPos = 0.0f;
    float EndPos = 0.0f;
    float Length = 0.0f;
    float AnimStartTime = 0.0f;
    float AnimEndTime = 0.0f;
    float PlayRate = 1.0f;
    int32 LoopCount = 1;
    bool bValid = true;
};

/** Blend settings of a montage. */
struct FUnrealMCPAnimBlendInfo
{
    float BlendInTime = 0.0f;
    FString BlendInOption;
    FString BlendInMode;
    float BlendOutTime = 0.0f;
    FString BlendOutOption;
    FString BlendOutMode;
    float BlendOutTriggerTime = 0.0f;
    bool bEnableAutoBlendOut = true;
};

/** One branching point of a montage (a notify whose montage tick type is BranchingPoint). */
struct FUnrealMCPAnimBranchingPointInfo
{
    /** Ordinal among the branching points, the index `remove_branching_point` expects. */
    int32 BranchingPointIndex = INDEX_NONE;
    /** Index in the montage's notify array (the index `remove_notify` expects). */
    int32 NotifyIndex = INDEX_NONE;
    FString NotifyName;
    FString NotifyClass;
    float TriggerTime = 0.0f;
    int32 TrackIndex = 0;
    FString SectionName;
};

/** State of the Persona preview session of one animation asset. */
struct FUnrealMCPAnimPreviewState
{
    bool bEditorOpen = false;
    /** True when a preview instance bound to this asset was found (the session can be driven). */
    bool bPreviewAvailable = false;
    bool bIsMontage = false;
    /** Name the asset editor reports for itself ("AnimationEditor", "MontageEditor", ...). */
    FString EditorName;
    FString AssetPath;
    float PreviewTime = 0.0f;
    float PlayLength = 0.0f;
    bool bIsPlaying = false;
    bool bIsLooping = false;
    float PlayRate = 1.0f;
    /** Montage only: the section the preview time currently falls into. */
    FString CurrentSectionName;
    int32 CurrentSectionIndex = INDEX_NONE;
    /** How many preview components were asked to preview the asset (0 when one was already bound). */
    int32 PreviewComponentsBound = 0;
};

/**
 * The animation editor session: opening the Persona tab of a sequence or a montage and driving its
 * preview (time, play/stop, section jumps).
 *
 * Persona does not expose the preview through an asset API, so the session is found the way the
 * editor itself finds it: the asset editor instance of the asset (UAssetEditorSubsystem) plus the
 * `UAnimPreviewInstance` of a preview component whose current asset is this asset. Nothing here
 * writes the asset - these commands change what the editor shows, not what the asset holds, which is
 * why they never save.
 */
class UNREALMCP_API FUnrealMCPAnimationEditorOps
{
public:
    /** Open the Persona tab of a sequence or montage; reports whether it is open afterwards. */
    static bool OpenEditor(UAnimSequenceBase* Host, FUnrealMCPAnimPreviewState& OutState,
                           FUnrealMCPAnimError& OutError);
    /** Close and reopen the editor of the asset, so the tab re-reads the asset from disk. */
    static bool RefreshEditor(UAnimSequenceBase* Host, FUnrealMCPAnimPreviewState& OutState,
                              FUnrealMCPAnimError& OutError);
    /** Read the preview state of the asset; fails with `editor_not_open` when there is no session. */
    static bool GetPreviewState(UAnimSequenceBase* Host, FUnrealMCPAnimPreviewState& OutState,
                                FUnrealMCPAnimError& OutError);
    static bool SetPreviewTime(UAnimSequenceBase* Host, float Time, FUnrealMCPAnimPreviewState& OutState,
                               FUnrealMCPAnimError& OutError);
    /** bPlay drives the preview; with bHasLoop/bHasPlayRate the preview settings follow too. */
    static bool SetPreviewPlaying(UAnimSequenceBase* Host, bool bPlay, bool bHasLoop, bool bLoop,
                                  bool bHasPlayRate, float PlayRate, FUnrealMCPAnimPreviewState& OutState,
                                  FUnrealMCPAnimError& OutError);
    /** Jump the montage preview to a section (starts the preview, jumps, and restores the play state). */
    static bool JumpToSection(UAnimMontage* Montage, const FString& SectionName,
                              FUnrealMCPAnimPreviewState& OutState, FUnrealMCPAnimError& OutError);
};

/**
 * The animation sequence kernel: every read and write the `anim_sequence` commands expose.
 *
 * The (VibeUE, UE 5.8) edits these assets through 5.6+ APIs such as
 * `UAnimSequenceBase::AddNotify` / `IAnimationDataController::AddSyncMarker`; UE 5.5 has neither,
 * so the operations here are written against what 5.5 really offers: the data controller
 * (`IAnimationDataController`) for curves and bone tracks, `Notifies` + `AnimNotifyTracks` +
 * `RefreshCacheData()` for notifies, `UAnimSequence::AuthoredSyncMarkers` for sync markers.
 *
 * All functions must be called on the GameThread. Writers are called inside the command layer's
 * transaction; they mark the package dirty, never save.
 */
class UNREALMCP_API FUnrealMCPAnimationOps
{
public:
    // --- resolution ----------------------------------------------------------

    /**
     * Sampling frame rate of one sequence, taken from its data model.
     *
     * MUST NOT come from UAnimSequenceBase::GetSamplingFrameRate(): that returns the PROJECT default
     * rate for every asset, so it would report 30 fps for a sequence whose own rate is 60 and would
     * convert frames to seconds with the wrong rate.
     */
    static FFrameRate GetSequenceFrameRate(const UAnimSequence* Sequence);

    /** Data model frame count of one sequence (the sampled key count is one more: the T0 key). */
    static int32 GetSequenceFrameCount(const UAnimSequence* Sequence);

    static bool ResolveSequence(const FString& AssetPath, UAnimSequence*& OutSequence, FUnrealMCPAnimError& OutError);
    static bool ResolveSkeleton(const FString& AssetPath, USkeleton*& OutSkeleton, FUnrealMCPAnimError& OutError);
    /** Reference-skeleton bone index for a bone name; candidates list matching suggestions. */
    static bool ResolveBoneIndex(UAnimSequence* Sequence, const FString& BoneName, int32& OutBoneIndex, FUnrealMCPAnimError& OutError);

    // --- discovery -----------------------------------------------------------

    /**
     * List sequences below SearchPath. Registry metadata only: the assets are NEVER loaded, which
     * is what keeps a broad search from pulling the whole library into memory.
     */
    static bool ListSequences(const FString& SearchPath, const FString& SkeletonFilter, int32 MaxResults,
                              TArray<FUnrealMCPAnimSequenceInfo>& OutInfos, int32& OutFoundCount, FUnrealMCPAnimError& OutError);
    /** Same registry path as ListSequences, matched by substring / wildcard on the asset name. */
    static bool SearchSequences(const FString& SearchPath, const FString& Query, int32 MaxResults,
                                TArray<FUnrealMCPAnimSequenceInfo>& OutInfos, int32& OutFoundCount, FUnrealMCPAnimError& OutError);
    /** Fill the full info of one loaded sequence. */
    static void FillSequenceInfo(UAnimSequence* Sequence, FUnrealMCPAnimSequenceInfo& OutInfo);
    /** Bone names of the sequence's skeleton reference pose. */
    static bool GetBoneNames(UAnimSequence* Sequence, TArray<FString>& OutBoneNames, FUnrealMCPAnimError& OutError);

    // --- sampling ------------------------------------------------------------

    static bool SampleBoneTransform(UAnimSequence* Sequence, int32 BoneIndex, float Time, FTransform& OutTransform,
                                    FUnrealMCPAnimError& OutError);
    static bool SamplePose(UAnimSequence* Sequence, float Time, TArray<FUnrealMCPAnimBonePose>& OutPose,
                           FUnrealMCPAnimError& OutError);
    /** bTotal samples the whole play length instead of the elapsed time. */
    static bool SampleRootMotion(UAnimSequence* Sequence, float Time, bool bTotal, FTransform& OutTransform,
                                 FUnrealMCPAnimError& OutError);

    // --- curves --------------------------------------------------------------

    static bool ListCurves(UAnimSequence* Sequence, TArray<FUnrealMCPAnimCurveInfo>& OutCurves,
                           int32& OutTransformCurveCount, FUnrealMCPAnimError& OutError);
    static bool GetCurveKeys(UAnimSequence* Sequence, const FString& CurveName,
                             TArray<FUnrealMCPAnimCurveKey>& OutKeys, FUnrealMCPAnimError& OutError);
    static bool GetCurveValue(UAnimSequence* Sequence, const FString& CurveName, float Time, float& OutValue,
                              FUnrealMCPAnimError& OutError);
    static bool AddCurve(UAnimSequence* Sequence, const FString& CurveName, FUnrealMCPAnimError& OutError);
    static bool RemoveCurve(UAnimSequence* Sequence, const FString& CurveName, FUnrealMCPAnimError& OutError);
    static bool SetCurveKeys(UAnimSequence* Sequence, const FString& CurveName,
                             const TArray<FUnrealMCPAnimCurveKey>& Keys, bool bReplaceAll, FUnrealMCPAnimError& OutError);

    // --- notifies ------------------------------------------------------------

    static void ListNotifies(UAnimSequence* Sequence, TArray<FUnrealMCPAnimNotifyInfo>& OutNotifies);
    /** Locate one notify by index, by name (optionally narrowed by time) or by editor guid. */
    static bool ResolveNotify(UAnimSequence* Sequence, int32 NotifyIndex, const FString& NotifyName,
                              const FString& Guid, float Time, bool bHasTime, int32& OutNotifyIndex,
                              FUnrealMCPAnimError& OutError);
    static bool AddNotify(UAnimSequence* Sequence, const FString& NotifyName, const FString& NotifyClassName,
                          float Time, float Duration, int32 TrackIndex, bool bState, int32& OutNotifyIndex,
                          FUnrealMCPAnimError& OutError);
    static bool RemoveNotify(UAnimSequence* Sequence, int32 NotifyIndex, FUnrealMCPAnimError& OutError);
    /** Write one field of a resolved notify. PropertyName is the snake_case alias of the field. */
    static bool SetNotifyProperty(UAnimSequence* Sequence, int32 NotifyIndex, const FString& PropertyName,
                                  const TSharedPtr<FJsonValue>& Value, FUnrealMCPAnimError& OutError);
    static const TArray<FString>& NotifyPropertyNames();

    static void ListNotifyTracks(UAnimSequence* Sequence, TArray<FUnrealMCPAnimNotifyTrackInfo>& OutTracks);
    static bool ResolveNotifyTrack(UAnimSequence* Sequence, int32 TrackIndex, const FString& TrackName,
                                   int32& OutTrackIndex, FUnrealMCPAnimError& OutError);
    static bool AddNotifyTrack(UAnimSequence* Sequence, const FString& TrackName, const FLinearColor& Color,
                               int32& OutTrackIndex, FUnrealMCPAnimError& OutError);
    static bool RenameNotifyTrack(UAnimSequence* Sequence, int32 TrackIndex, const FString& NewName,
                                  FUnrealMCPAnimError& OutError);
    static bool RemoveNotifyTrack(UAnimSequence* Sequence, int32 TrackIndex, FUnrealMCPAnimError& OutError);

    // --- sync markers --------------------------------------------------------

    static void ListSyncMarkers(UAnimSequence* Sequence, TArray<FUnrealMCPAnimSyncMarkerInfo>& OutMarkers);
    static bool AddSyncMarker(UAnimSequence* Sequence, const FString& MarkerName, float Time, int32& OutMarkerIndex,
                              FUnrealMCPAnimError& OutError);
    static bool ResolveSyncMarker(UAnimSequence* Sequence, const FString& MarkerName, float Time, bool bHasTime,
                                  int32 MarkerIndex, int32& OutMarkerIndex, FUnrealMCPAnimError& OutError);
    static bool RemoveSyncMarker(UAnimSequence* Sequence, int32 MarkerIndex, FUnrealMCPAnimError& OutError);
    static bool SetSyncMarkerTime(UAnimSequence* Sequence, int32 MarkerIndex, float NewTime, FUnrealMCPAnimError& OutError);

    // --- creation and configuration ------------------------------------------

    /**
     * Create a sequence asset. When bFromReferencePose is set every bone track of the skeleton is
     * baked with the reference pose (a usable pose), otherwise the asset is created empty.
     */
    static bool CreateSequence(const FString& Name, const FString& Folder, const FString& SkeletonPath,
                               float FrameRate, float Duration, bool bFromReferencePose,
                               FString& OutAssetPath, FUnrealMCPAnimError& OutError);

    static bool SetFrameRate(UAnimSequence* Sequence, float FrameRate, float& OutReadback, FUnrealMCPAnimError& OutError);
    static bool SetRateScale(UAnimSequence* Sequence, float RateScale, float& OutReadback, FUnrealMCPAnimError& OutError);
    static bool AddBoneTrack(UAnimSequence* Sequence, const FString& BoneName, FUnrealMCPAnimError& OutError);
    static bool RemoveBoneTrack(UAnimSequence* Sequence, const FString& BoneName, FUnrealMCPAnimError& OutError);
    /**
     * Write the keyframes of one bone. bBakeEveryFrame resamples the given keys onto every frame of
     * the sequence (what the sequence editor stores), otherwise the keys are written sparsely.
     */
    static bool SetBoneTrackKeys(UAnimSequence* Sequence, const FString& BoneName,
                                 const TArray<FUnrealMCPAnimKeyframe>& Keys, bool bBakeEveryFrame,
                                 FUnrealMCPAnimError& OutError);

    static bool SetAdditiveAnimType(UAnimSequence* Sequence, const FString& Type, FUnrealMCPAnimError& OutError);
    static bool SetAdditiveBasePose(UAnimSequence* Sequence, const FString& BasePoseSequencePath, FUnrealMCPAnimError& OutError);
    static bool SetEnableRootMotion(UAnimSequence* Sequence, bool bEnable, FUnrealMCPAnimError& OutError);
    static bool SetRootMotionRootLock(UAnimSequence* Sequence, const FString& LockType, FUnrealMCPAnimError& OutError);
    static bool SetForceRootLock(UAnimSequence* Sequence, bool bForce, FUnrealMCPAnimError& OutError);

    static bool SetCompressionScheme(UAnimSequence* Sequence, const FString& SettingsPath, FString& OutReadback,
                                     FUnrealMCPAnimError& OutError);
    /** Synchronous recompression; reports the size it ended up with (never a "started" signal). */
    static bool CompressSequence(UAnimSequence* Sequence, int32& OutCompressedSize, bool& bOutDataValid,
                                 FUnrealMCPAnimError& OutError);
    static void FillCompressionInfo(UAnimSequence* Sequence, FUnrealMCPAnimCompressionInfo& OutInfo);

    static bool GetSourceFiles(UAnimSequence* Sequence, TArray<FString>& OutFiles, FUnrealMCPAnimError& OutError);
    static FString ExportToJson(UAnimSequence* Sequence);

    // --- string conversions (shared with the command layer's readbacks) -------

    static FString InterpModeToString(int32 Mode);
    static FString TangentModeToString(int32 Mode);
    static bool StringToInterpMode(const FString& Text, int32& OutMode);
    static bool StringToTangentMode(const FString& Text, int32& OutMode);
    static FString AdditiveTypeToString(int32 Type);
    static bool StringToAdditiveType(const FString& Text, int32& OutType);
    static FString RootLockToString(int32 LockType);
    static bool StringToRootLock(const FString& Text, int32& OutLockType);
    static FString FilterTypeToString(int32 FilterType);
    static bool StringToFilterType(const FString& Text, int32& OutFilterType);
};

/**
 * The animation montage kernel: every read and write the `anim_montage` commands expose.
 *
 * Montages are not data-model assets: their content lives in `CompositeSections` / `SlotAnimTracks`
 * (`FAnimTrack::AnimSegments`) / `Notifies`, all of which the montage itself keeps sorted and linked
 * through `UpdateLinkableElements` + `RefreshCacheData`. The writers here mark the package dirty and
 * never save - the command layer owns the transaction and the save.
 */
class UNREALMCP_API FUnrealMCPAnimationMontageOps
{
public:
    // --- resolution ----------------------------------------------------------

    /** Accepts an object path, a package path or a short asset name. */
    static bool ResolveMontage(const FString& AssetPath, UAnimMontage*& OutMontage, FUnrealMCPAnimError& OutError);
    static bool ResolveSection(UAnimMontage* Montage, const FString& SectionName, int32 SectionIndex,
                               int32& OutSectionIndex, FUnrealMCPAnimError& OutError);
    static bool ResolveSlotTrack(UAnimMontage* Montage, int32 TrackIndex, const FString& SlotName,
                                 int32& OutTrackIndex, FUnrealMCPAnimError& OutError);

    // --- discovery -----------------------------------------------------------

    /** Registry metadata only; the assets are never loaded. */
    static bool ListMontages(const FString& SearchPath, const FString& SkeletonFilter, int32 MaxResults,
                             TArray<FUnrealMCPAnimMontageInfo>& OutInfos, int32& OutFoundCount,
                             FUnrealMCPAnimError& OutError);
    /**
     * Montages that reference one animation. Registry referencers first (no load); when the registry
     * has no dependency data for the asset yet - which is normal right after a create - it falls back
     * to a bounded scan that loads at most MaxScan montages.
     */
    static bool FindMontagesUsingAnimation(const FString& AnimationPath, int32 MaxResults, int32 MaxScan,
                                           TArray<FUnrealMCPAnimMontageInfo>& OutInfos, int32& OutFoundCount,
                                           FString& OutStrategy, FUnrealMCPAnimError& OutError);
    static void FillMontageInfo(UAnimMontage* Montage, FUnrealMCPAnimMontageInfo& OutInfo);

    // --- sections ------------------------------------------------------------

    static void ListSections(UAnimMontage* Montage, TArray<FUnrealMCPAnimSectionInfo>& OutSections);
    static void FillSectionInfo(UAnimMontage* Montage, int32 SectionIndex, FUnrealMCPAnimSectionInfo& OutInfo);
    static int32 GetSectionIndexAtTime(UAnimMontage* Montage, float Time);
    /** Every (section -> next section) pair the montage currently has. */
    static void ListSectionLinks(UAnimMontage* Montage, TArray<TPair<FString, FString>>& OutLinks);
    /** Adds a section at StartTime and re-sorts the sections by time (the montage editor's behaviour). */
    static bool AddSection(UAnimMontage* Montage, const FString& SectionName, float StartTime, int32& OutSectionIndex,
                           FUnrealMCPAnimError& OutError);
    static bool RemoveSection(UAnimMontage* Montage, int32 SectionIndex, FUnrealMCPAnimError& OutError);
    static bool RenameSection(UAnimMontage* Montage, int32 SectionIndex, const FString& NewName,
                              FUnrealMCPAnimError& OutError);
    static bool SetSectionStartTime(UAnimMontage* Montage, int32 SectionIndex, float Time,
                                    FUnrealMCPAnimError& OutError);
    static bool SetNextSection(UAnimMontage* Montage, int32 SectionIndex, const FString& NextSectionName,
                               FUnrealMCPAnimError& OutError);

    // --- slot tracks ---------------------------------------------------------

    static void ListSlotTracks(UAnimMontage* Montage, TArray<FUnrealMCPAnimSlotTrackInfo>& OutTracks);
    static bool AddSlotTrack(UAnimMontage* Montage, const FString& SlotName, int32& OutTrackIndex,
                             FUnrealMCPAnimError& OutError);
    static bool RemoveSlotTrack(UAnimMontage* Montage, int32 TrackIndex, FUnrealMCPAnimError& OutError);
    static bool SetSlotName(UAnimMontage* Montage, int32 TrackIndex, const FString& NewName,
                            FUnrealMCPAnimError& OutError);
    /** Slot names used by the montage plus the slot names the skeleton defines. */
    static void ListUsedSlotNames(UAnimMontage* Montage, TArray<FString>& OutUsed, TArray<FString>& OutSkeletonSlots);

    // --- segments ------------------------------------------------------------

    static void ListAnimSegments(UAnimMontage* Montage, int32 TrackIndex,
                                 TArray<FUnrealMCPAnimSegmentInfo>& OutSegments, FUnrealMCPAnimError& OutError);
    static void FillSegmentInfo(const FAnimSegment& Segment, int32 TrackIndex, int32 SegmentIndex,
                                FUnrealMCPAnimSegmentInfo& OutInfo);
    static bool AddAnimSegment(UAnimMontage* Montage, int32 TrackIndex, const FString& AnimPath, float StartTime,
                               float PlayRate, int32 LoopCount, bool bHasStartTime, bool bHasPlayRate,
                               bool bHasLoopCount, int32& OutSegmentIndex, FUnrealMCPAnimError& OutError);
    static bool RemoveAnimSegment(UAnimMontage* Montage, int32 TrackIndex, int32 SegmentIndex,
                                  FUnrealMCPAnimError& OutError);
    /** Writes one field of a segment. PropertyName is the snake_case alias of the field. */
    static bool SetSegmentProperty(UAnimMontage* Montage, int32 TrackIndex, int32 SegmentIndex,
                                   const FString& PropertyName, const TSharedPtr<FJsonValue>& Value,
                                   FUnrealMCPAnimError& OutError);
    static const TArray<FString>& SegmentPropertyNames();

    // --- notifies and branching points ---------------------------------------

    static void ListMontageNotifies(UAnimMontage* Montage, TArray<FUnrealMCPAnimNotifyInfo>& OutNotifies);
    /** Locate one notify by index, by name (optionally narrowed by time) or by editor guid. */
    static bool ResolveMontageNotify(UAnimMontage* Montage, int32 NotifyIndex, const FString& NotifyName,
                                     const FString& Guid, float Time, bool bHasTime, int32& OutNotifyIndex,
                                     FUnrealMCPAnimError& OutError);
    static bool AddMontageNotify(UAnimMontage* Montage, const FString& NotifyName, const FString& NotifyClassName,
                                 float Time, float Duration, int32 TrackIndex, bool bState, int32& OutNotifyIndex,
                                 FUnrealMCPAnimError& OutError);
    static bool RemoveMontageNotify(UAnimMontage* Montage, int32 NotifyIndex, FUnrealMCPAnimError& OutError);
    /**
     * Move a notify. With a section name the notify is placed at that section's start time (what
     * "link a notify to a section" means: the montage stores no section link, the position is it).
     */
    static bool SetMontageNotifyTime(UAnimMontage* Montage, int32 NotifyIndex, float Time, bool bHasTime,
                                     const FString& SectionName, FUnrealMCPAnimError& OutError);

    static void ListBranchingPoints(UAnimMontage* Montage, TArray<FUnrealMCPAnimBranchingPointInfo>& OutPoints);
    static bool AddBranchingPoint(UAnimMontage* Montage, const FString& NotifyName, float Time, int32 TrackIndex,
                                  int32& OutNotifyIndex, FUnrealMCPAnimError& OutError);
    /** Removes one branching point; a plain notify at the same index is refused, not removed. */
    static bool RemoveBranchingPoint(UAnimMontage* Montage, int32 BranchingPointIndex, FUnrealMCPAnimError& OutError);
    static bool IsBranchingPointAtTime(UAnimMontage* Montage, float Time, FString& OutNotifyName);

    // --- blend settings and root motion --------------------------------------

    static void GetBlendInfo(UAnimMontage* Montage, FUnrealMCPAnimBlendInfo& OutInfo);
    static bool SetBlendSettings(UAnimMontage* Montage, bool bBlendIn, bool bHasTime, float BlendTime,
                                 bool bHasOption, const FString& BlendOption, FUnrealMCPAnimError& OutError);
    static bool SetBlendOutTriggerTime(UAnimMontage* Montage, float TriggerTime, FUnrealMCPAnimError& OutError);
    static void GetEnableRootMotion(UAnimMontage* Montage, bool& bOutTranslation, bool& bOutRotation);
    static bool SetEnableRootMotion(UAnimMontage* Montage, bool bTranslation, bool bEnable,
                                    FUnrealMCPAnimError& OutError);
    static bool SampleRootMotion(UAnimMontage* Montage, float Time, FTransform& OutTransform,
                                 FUnrealMCPAnimError& OutError);

    // --- creation ------------------------------------------------------------

    static bool CreateMontageFromAnimation(const FString& AnimationPath, const FString& Name, const FString& Folder,
                                           FString& OutAssetPath, FUnrealMCPAnimError& OutError);
    static bool CreateEmptyMontage(const FString& Name, const FString& Folder, const FString& SkeletonPath,
                                   FString& OutAssetPath, FUnrealMCPAnimError& OutError);
    static bool DuplicateMontage(const FString& SourcePath, const FString& Name, const FString& Folder,
                                 FString& OutAssetPath, FUnrealMCPAnimError& OutError);

    // --- string conversions --------------------------------------------------

    static FString BlendOptionToString(int32 Option);
    static bool StringToBlendOption(const FString& Text, int32& OutOption);
    /** The valid blend option names, for error `candidates`. */
    static const TArray<FString>& BlendOptionNames();
    static FString BlendModeToString(int32 Mode);
    static FString LinkMethodToString(int32 Method);
};

/**
 * Handler class for the AnimSequence MCP commands (`anim_sequence` category).
 *
 * Mirrors the other domains: each command is a private HandleXxx method returning a JSON result,
 * registered in RegisterCommands() with its parameters and policy flags. Writes go through
 * RunCommand, which wraps the body in one undo step and saves the asset on success; readbacks are
 * read back from the asset, never echoed from the request.
 */
class UNREALMCP_API FUnrealMCPAnimationCommands
{
public:
    FUnrealMCPAnimationCommands();

    void RegisterCommands(FMCPCommandRegistry& Registry);

private:
    TSharedPtr<FJsonObject> RunCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params,
                                       const TFunction<TSharedPtr<FJsonObject>(const TSharedPtr<FJsonObject>&)>& Body);

    // --- reads: discovery and properties
    TSharedPtr<FJsonObject> HandleAnimSelfCheck(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListAnimSequences(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetAnimSequenceInfo(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleFindAnimationsForSkeleton(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSearchAnimations(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetAnimationLength(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetAnimationFrameRate(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetAnimationFrameCount(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetAnimationSkeleton(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetRateScale(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetAnimatedBones(const TSharedPtr<FJsonObject>& Params);

    // --- reads: sampling
    TSharedPtr<FJsonObject> HandleGetBoneTransform(const TSharedPtr<FJsonObject>& Params, bool bByFrame);
    TSharedPtr<FJsonObject> HandleGetPose(const TSharedPtr<FJsonObject>& Params, bool bByFrame);
    TSharedPtr<FJsonObject> HandleGetRootMotionAtTime(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetTotalRootMotion(const TSharedPtr<FJsonObject>& Params);

    // --- reads: curves, notifies, sync markers
    TSharedPtr<FJsonObject> HandleListCurves(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetCurveInfo(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetCurveValueAtTime(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetCurveKeyframes(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListNotifies(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetNotifyInfo(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListNotifyTracks(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetNotifyTrackCount(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListSyncMarkers(const TSharedPtr<FJsonObject>& Params);

    // --- reads: configuration
    TSharedPtr<FJsonObject> HandleGetSetting(const TSharedPtr<FJsonObject>& Params, const FString& Which);
    TSharedPtr<FJsonObject> HandleGetCompressionInfo(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetSourceFiles(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleExportAnimationToJson(const TSharedPtr<FJsonObject>& Params);

    // --- writes: creation and properties
    TSharedPtr<FJsonObject> HandleCreateAnimSequence(const TSharedPtr<FJsonObject>& Params, bool bFromReferencePose);
    TSharedPtr<FJsonObject> HandleSetAnimationFrameRate(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetRateScale(const TSharedPtr<FJsonObject>& Params);

    // --- writes: bone tracks
    TSharedPtr<FJsonObject> HandleAddBoneTrack(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveBoneTrack(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetBoneTrackKeys(const TSharedPtr<FJsonObject>& Params);

    // --- writes: curves
    TSharedPtr<FJsonObject> HandleAddCurve(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveCurve(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetCurveKeys(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddCurveKey(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetCurveMetadataFlags(const TSharedPtr<FJsonObject>& Params);

    // --- writes: notifies
    TSharedPtr<FJsonObject> HandleAddNotify(const TSharedPtr<FJsonObject>& Params, bool bState);
    TSharedPtr<FJsonObject> HandleRemoveNotify(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetNotifyProperty(const TSharedPtr<FJsonObject>& Params, const FString& PropertyName);

    // --- writes: notify tracks and sync markers
    TSharedPtr<FJsonObject> HandleAddNotifyTrack(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRenameNotifyTrack(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveNotifyTrack(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddSyncMarker(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveSyncMarker(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetSyncMarkerTime(const TSharedPtr<FJsonObject>& Params);

    // --- writes: additive, root motion, compression
    TSharedPtr<FJsonObject> HandleSetAdditiveAnimType(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetAdditiveBasePose(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetEnableRootMotion(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetRootMotionRootLock(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetForceRootLock(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetCompressionScheme(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCompressAnimation(const TSharedPtr<FJsonObject>& Params);

    // --- montage: reads
    TSharedPtr<FJsonObject> HandleListMontages(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetMontageInfo(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleFindMontagesForSkeleton(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleFindMontagesUsingAnimation(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetMontageLength(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetMontageSkeleton(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListSections(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetSectionInfo(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetSectionAtTime(const TSharedPtr<FJsonObject>& Params, bool bByName);
    TSharedPtr<FJsonObject> HandleGetSectionLength(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetNextSection(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetAllSectionLinks(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListSlotTracks(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetSlotTrackInfo(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetAllUsedSlotNames(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListAnimSegments(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetAnimSegmentInfo(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListMontageNotifies(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListBranchingPoints(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleIsBranchingPointAtTime(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetBlendSettings(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetMontageRootMotionSetting(const TSharedPtr<FJsonObject>& Params, bool bTranslation);

    // --- montage: writes
    TSharedPtr<FJsonObject> HandleCreateMontageFromAnimation(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCreateEmptyMontage(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleDuplicateMontage(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddSection(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveSection(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRenameSection(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetSectionStartTime(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetNextSection(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetSectionLoop(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleClearSectionLink(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddSlotTrack(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveSlotTrack(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetSlotName(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddAnimSegment(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveAnimSegment(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetSegmentProperty(const TSharedPtr<FJsonObject>& Params, const FString& PropertyName);
    TSharedPtr<FJsonObject> HandleAddMontageNotify(const TSharedPtr<FJsonObject>& Params, bool bState);
    TSharedPtr<FJsonObject> HandleRemoveMontageNotify(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetMontageNotifyProperty(const TSharedPtr<FJsonObject>& Params, const FString& PropertyName);
    TSharedPtr<FJsonObject> HandleAddBranchingPoint(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveBranchingPoint(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetBlend(const TSharedPtr<FJsonObject>& Params, bool bBlendIn);
    TSharedPtr<FJsonObject> HandleSetBlendOutTriggerTime(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetMontageRootMotion(const TSharedPtr<FJsonObject>& Params, bool bTranslation);

    // --- editor session / preview
    TSharedPtr<FJsonObject> HandleOpenAnimationEditor(const TSharedPtr<FJsonObject>& Params, bool bMontageOnly);
    TSharedPtr<FJsonObject> HandleRefreshMontageEditor(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetPreviewTime(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandlePlayPreview(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleStopPreview(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleJumpToSection(const TSharedPtr<FJsonObject>& Params);
};
