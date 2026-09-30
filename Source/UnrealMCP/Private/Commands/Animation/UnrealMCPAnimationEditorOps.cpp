#include "Commands/Animation/UnrealMCPAnimationCommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Animation/DebugSkelMeshComponent.h"
#include "AnimPreviewInstance.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/IToolkitHost.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealMCPAnimationEditorOps, Log, All);

namespace
{
    UAssetEditorSubsystem* GetAssetEditorSubsystem()
    {
        return GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
    }

    /** The preview instance of one preview component, when it is showing this asset. */
    UAnimPreviewInstance* GetInstanceBoundTo(UDebugSkelMeshComponent* Component, UAnimSequenceBase* Host)
    {
        if (!IsValid(Component))
        {
            return nullptr;
        }

        UAnimPreviewInstance* Instance = Cast<UAnimPreviewInstance>(Component->PreviewInstance);
        return (Instance && Instance->GetCurrentAsset() == Host) ? Instance : nullptr;
    }

    /** The preview instance Persona currently drives for this asset, if any. */
    UAnimPreviewInstance* FindPreviewInstance(UAnimSequenceBase* Host)
    {
        for (TObjectIterator<UDebugSkelMeshComponent> It; It; ++It)
        {
            UDebugSkelMeshComponent* Component = *It;
            if (Component->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
            {
                continue;
            }

            if (UAnimPreviewInstance* Instance = GetInstanceBoundTo(Component, Host))
            {
                return Instance;
            }
        }

        return nullptr;
    }

    /**
     * Bind the asset to a preview component, for the case where Persona has just been opened and its
     * component has no asset yet. Components that already preview something else are left alone, so
     * opening one editor cannot hijack another one's preview.
     */
    bool BindPreviewInstance(UAnimSequenceBase* Host, int32& OutBoundCount)
    {
        OutBoundCount = 0;
        for (TObjectIterator<UDebugSkelMeshComponent> It; It; ++It)
        {
            UDebugSkelMeshComponent* Component = *It;
            if (!IsValid(Component) || Component->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
            {
                continue;
            }

            UAnimPreviewInstance* Existing = Cast<UAnimPreviewInstance>(Component->PreviewInstance);
            if (Existing && Existing->GetCurrentAsset() != nullptr)
            {
                continue;
            }

            Component->EnablePreview(true, Host);

            UAnimPreviewInstance* Instance = Cast<UAnimPreviewInstance>(Component->PreviewInstance);
            if (!Instance)
            {
                continue;
            }

            // bIsLooping=false: an opened asset is loaded paused at the start, and play_preview starts it.
            Instance->SetAnimationAsset(Host, /*bIsLooping=*/false, 1.0f);
            ++OutBoundCount;
        }

        return OutBoundCount > 0;
    }

    /**
     * EditorName is taken by value on purpose: callers pass the string field of the very state this
     * function resets first, and a reference into that state would be cleared before it is read.
     */
    void FillState(UAnimSequenceBase* Host, UAnimPreviewInstance* Instance, FString EditorName,
                   FUnrealMCPAnimPreviewState& OutState)
    {
        OutState = FUnrealMCPAnimPreviewState();
        OutState.bEditorOpen = true;
        OutState.EditorName = EditorName;
        OutState.AssetPath = Host->GetPathName();
        OutState.bIsMontage = Cast<UAnimMontage>(Host) != nullptr;
        OutState.PlayLength = Host->GetPlayLength();

        if (!Instance)
        {
            return;
        }

        OutState.bPreviewAvailable = true;
        OutState.PreviewTime = Instance->GetCurrentTime();
        OutState.bIsPlaying = Instance->IsPlaying();
        OutState.bIsLooping = Instance->IsLooping();
        OutState.PlayRate = Instance->GetPlayRate();

        if (UAnimMontage* Montage = Cast<UAnimMontage>(Host))
        {
            const int32 SectionIndex = Montage->GetSectionIndexFromPosition(OutState.PreviewTime);
            if (SectionIndex != INDEX_NONE)
            {
                OutState.CurrentSectionIndex = SectionIndex;
                OutState.CurrentSectionName = Montage->GetSectionName(SectionIndex).ToString();
            }
        }
    }

    /**
     * The session of one asset: its editor instance plus the preview instance it drives. Every
     * failure here is `editor_not_open` - the asset exists, but nothing is holding a preview of it.
     */
    bool ResolveSession(UAnimSequenceBase* Host, UAnimPreviewInstance*& OutInstance,
                        FUnrealMCPAnimPreviewState& OutState, FUnrealMCPAnimError& OutError)
    {
        OutInstance = nullptr;

        if (!Host)
        {
            OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
            return false;
        }

        UAssetEditorSubsystem* Subsystem = GetAssetEditorSubsystem();
        if (!Subsystem)
        {
            OutError.Set(EUnrealMCPAnimError::EditorNotOpen,
                         TEXT("no asset editor subsystem: these commands only work inside the editor"));
            return false;
        }

        IAssetEditorInstance* EditorInstance = Subsystem->FindEditorForAsset(Host, /*bFocusIfOpen=*/false);
        if (!EditorInstance)
        {
            OutError.Set(EUnrealMCPAnimError::EditorNotOpen,
                         FString::Printf(TEXT("'%s' has no open editor; call open_animation_editor (or open_montage_editor) first"),
                                         *Host->GetName()));
            return false;
        }

        const FString EditorName = EditorInstance->GetEditorName().ToString();

        UAnimPreviewInstance* Instance = FindPreviewInstance(Host);
        int32 BoundCount = 0;
        if (!Instance)
        {
            // The tab is open but nothing previews the asset yet (Persona creates its preview while
            // the tab is being set up); ask the free preview components to take it.
            BindPreviewInstance(Host, BoundCount);
            Instance = FindPreviewInstance(Host);
        }

        if (!Instance)
        {
            OutError.Set(EUnrealMCPAnimError::EditorNotOpen,
                         FString::Printf(TEXT("the '%s' editor of '%s' has no preview instance bound to the asset"),
                                         *EditorName, *Host->GetName()));
            return false;
        }

        OutInstance = Instance;
        FillState(Host, Instance, EditorName, OutState);
        OutState.PreviewComponentsBound = BoundCount;
        return true;
    }

    /** Refresh = close the tab and open it again, so it re-reads the asset. */
    bool OpenEditorFor(UAnimSequenceBase* Host, UAssetEditorSubsystem* Subsystem, FString& OutEditorName,
                       FUnrealMCPAnimError& OutError)
    {
        // No progress window: an MCP call must not block on a modal dialog.
        Subsystem->OpenEditorForAsset(Host, EToolkitMode::Standalone, TSharedPtr<IToolkitHost>(), /*bShowProgressWindow=*/false);

        IAssetEditorInstance* EditorInstance = Subsystem->FindEditorForAsset(Host, /*bFocusIfOpen=*/false);
        if (!EditorInstance)
        {
            OutError.Set(EUnrealMCPAnimError::EditorNotOpen,
                         FString::Printf(TEXT("opening the asset editor of '%s' did not produce an editor instance"),
                                         *Host->GetName()));
            return false;
        }

        OutEditorName = EditorInstance->GetEditorName().ToString();
        return true;
    }
}

bool FUnrealMCPAnimationEditorOps::OpenEditor(UAnimSequenceBase* Host, FUnrealMCPAnimPreviewState& OutState,
                                              FUnrealMCPAnimError& OutError)
{
    if (!Host)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }

    UAssetEditorSubsystem* Subsystem = GetAssetEditorSubsystem();
    if (!Subsystem)
    {
        OutError.Set(EUnrealMCPAnimError::EditorNotOpen,
                     TEXT("no asset editor subsystem: these commands only work inside the editor"));
        return false;
    }

    FString EditorName;
    if (!OpenEditorFor(Host, Subsystem, EditorName, OutError))
    {
        return false;
    }

    // The preview may need one more push right after the tab appears; a failure here is not a failure
    // of the open itself, the state reports `preview_available` and the caller can retry.
    UAnimPreviewInstance* Instance = FindPreviewInstance(Host);
    int32 BoundCount = 0;
    if (!Instance)
    {
        BindPreviewInstance(Host, BoundCount);
        Instance = FindPreviewInstance(Host);
    }

    FillState(Host, Instance, EditorName, OutState);
    OutState.PreviewComponentsBound = BoundCount;
    return true;
}

bool FUnrealMCPAnimationEditorOps::RefreshEditor(UAnimSequenceBase* Host, FUnrealMCPAnimPreviewState& OutState,
                                                 FUnrealMCPAnimError& OutError)
{
    if (!Host)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }

    UAssetEditorSubsystem* Subsystem = GetAssetEditorSubsystem();
    if (!Subsystem)
    {
        OutError.Set(EUnrealMCPAnimError::EditorNotOpen,
                     TEXT("no asset editor subsystem: these commands only work inside the editor"));
        return false;
    }

    Subsystem->CloseAllEditorsForAsset(Host);

    FString EditorName;
    if (!OpenEditorFor(Host, Subsystem, EditorName, OutError))
    {
        return false;
    }

    UAnimPreviewInstance* Instance = FindPreviewInstance(Host);
    int32 BoundCount = 0;
    if (!Instance)
    {
        BindPreviewInstance(Host, BoundCount);
        Instance = FindPreviewInstance(Host);
    }

    FillState(Host, Instance, EditorName, OutState);
    OutState.PreviewComponentsBound = BoundCount;
    return true;
}

bool FUnrealMCPAnimationEditorOps::GetPreviewState(UAnimSequenceBase* Host, FUnrealMCPAnimPreviewState& OutState,
                                                   FUnrealMCPAnimError& OutError)
{
    UAnimPreviewInstance* Instance = nullptr;
    return ResolveSession(Host, Instance, OutState, OutError);
}

bool FUnrealMCPAnimationEditorOps::SetPreviewTime(UAnimSequenceBase* Host, float Time,
                                                  FUnrealMCPAnimPreviewState& OutState, FUnrealMCPAnimError& OutError)
{
    UAnimPreviewInstance* Instance = nullptr;
    if (!ResolveSession(Host, Instance, OutState, OutError))
    {
        return false;
    }

    const float Length = Host->GetPlayLength();
    if (Time < 0.0f || (Length > 0.0f && Time > Length))
    {
        OutError.Set(EUnrealMCPAnimError::InvalidValue,
                     FString::Printf(TEXT("time %.4f is outside [0, %.4f]"), Time, Length));
        return false;
    }

    if (Cast<UAnimMontage>(Host))
    {
        // The montage path also moves its preview start section, which SetPosition alone would not.
        Instance->MontagePreview_JumpToPosition(Time);
    }
    else
    {
        Instance->SetPosition(Time, /*bFireNotifies=*/false);
    }

    FillState(Host, Instance, OutState.EditorName, OutState);
    return true;
}

bool FUnrealMCPAnimationEditorOps::SetPreviewPlaying(UAnimSequenceBase* Host, bool bPlay, bool bHasLoop, bool bLoop,
                                                     bool bHasPlayRate, float PlayRate,
                                                     FUnrealMCPAnimPreviewState& OutState,
                                                     FUnrealMCPAnimError& OutError)
{
    UAnimPreviewInstance* Instance = nullptr;
    if (!ResolveSession(Host, Instance, OutState, OutError))
    {
        return false;
    }

    if (bHasPlayRate && !FMath::IsNearlyZero(PlayRate) && Cast<UAnimMontage>(Host) == nullptr)
    {
        Instance->SetPlayRate(PlayRate);
    }

    if (bHasLoop && Cast<UAnimMontage>(Host) == nullptr)
    {
        Instance->SetLooping(bLoop);
    }

    if (Cast<UAnimMontage>(Host))
    {
        // For a montage this also (re)starts the montage preview when nothing is playing.
        Instance->MontagePreview_SetPlaying(bPlay);
    }
    else
    {
        Instance->SetPlaying(bPlay);
    }

    FillState(Host, Instance, OutState.EditorName, OutState);
    return true;
}

bool FUnrealMCPAnimationEditorOps::JumpToSection(UAnimMontage* Montage, const FString& SectionName,
                                                 FUnrealMCPAnimPreviewState& OutState, FUnrealMCPAnimError& OutError)
{
    if (!Montage)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
        return false;
    }
    if (SectionName.IsEmpty())
    {
        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("section is required"));
        return false;
    }

    if (Montage->GetSectionIndex(FName(*SectionName)) == INDEX_NONE)
    {
        OutError.Set(EUnrealMCPAnimError::SectionNotFound,
                     FString::Printf(TEXT("no section named '%s' on '%s'"), *SectionName, *Montage->GetName()));
        for (int32 Index = 0; Index < Montage->CompositeSections.Num(); ++Index)
        {
            OutError.Candidates.Add(Montage->CompositeSections[Index].SectionName.ToString());
        }
        return false;
    }

    UAnimPreviewInstance* Instance = nullptr;
    if (!ResolveSession(Montage, Instance, OutState, OutError))
    {
        return false;
    }

    const int32 SectionIndex = Montage->GetSectionIndex(FName(*SectionName));
    float SectionStartTime = 0.0f;
    float SectionEndTime = 0.0f;
    Montage->GetSectionStartAndEndTime(SectionIndex, SectionStartTime, SectionEndTime);

    // Montage_JumpToSection needs a running montage instance - the same dance the montage editor's own
    // "jump to section" does: start it if it is not playing, jump, then put the play state back.
    const bool bWasPlaying = Instance->IsPlayingMontage() && Instance->IsPlaying();
    if (!Instance->IsPlayingMontage())
    {
        Instance->MontagePreview_SetPlaying(true);
    }

    Instance->Montage_JumpToSection(FName(*SectionName), Montage);

    // The montage's own position moves right away, but the preview proxy time only follows on the next
    // tick, so it is placed here as well - otherwise an immediate readback would still name the section
    // the preview came from. This is the montage editor's own "move the preview" entry point.
    Instance->MontagePreview_JumpToPosition(SectionStartTime);

    if (!bWasPlaying)
    {
        Instance->MontagePreview_SetPlaying(false);
    }

    FillState(Montage, Instance, OutState.EditorName, OutState);
    return true;
}
