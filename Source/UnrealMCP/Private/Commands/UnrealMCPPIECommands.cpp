#include "Commands/UnrealMCPPIECommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Core/MCPCommandRegistry.h"

#include "CoreGlobals.h"
#include "Editor.h"
#include "EditorSubsystem.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/SkeletalMeshComponent.h"
#include "LevelEditorSubsystem.h"
#include "ReferenceSkeleton.h"

UWorld* FUnrealMCPPIECommands::GetPlayWorld()
{
    return GEditor ? GEditor->PlayWorld : nullptr;
}

FUnrealMCPPIECommands::FUnrealMCPPIECommands()
{
}

TSharedPtr<FJsonObject> FUnrealMCPPIECommands::HandleStartPIE(const TSharedPtr<FJsonObject>& Params)
{
    if (!GEditor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("editor_unavailable: GEditor is null"));
    }

    const bool bAlreadyRunning = GetPlayWorld() != nullptr;
    bool bRequested = false;
    if (!bAlreadyRunning)
    {
        if (ULevelEditorSubsystem* LevelEditor = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>())
        {
            // The editor's own Play entry point (ULevelEditorSubsystem::EditorRequestBeginPlay: InProcess +
            // the active viewport, i.e. exactly what the Play button does - a player pawn gets spawned).
            // NOT EditorPlaySimulate(), which requests a Simulate session (no player pawn at all).
            // It returns immediately: the session is built on a later frame, which is exactly why this
            // command must not try to wait - waiting would mean sleeping on the GameThread.
            LevelEditor->EditorRequestBeginPlay();
            bRequested = true;
        }
    }

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetBoolField(TEXT("requested"), bRequested);
    ResultJson->SetBoolField(TEXT("was_already_running"), bAlreadyRunning);
    ResultJson->SetBoolField(TEXT("pie_running"), GetPlayWorld() != nullptr);
    ResultJson->SetStringField(TEXT("hint"),
        TEXT("the session comes up on a later frame; poll pie_running with short calls (this command never waits)"));
    return ResultJson;
}

TSharedPtr<FJsonObject> FUnrealMCPPIECommands::HandleStopPIE(const TSharedPtr<FJsonObject>& Params)
{
    if (!GEditor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("editor_unavailable: GEditor is null"));
    }

    const bool bWasRunning = GetPlayWorld() != nullptr;
    if (bWasRunning)
    {
        if (ULevelEditorSubsystem* LevelEditor = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>())
        {
            LevelEditor->EditorRequestEndPlay();
        }
    }

    // Not waiting for the tear-down to finish either: the caller polls pie_running, and any runtime
    // read taken before it goes false still answers from the live session (get_actor_pose returns
    // not_in_pie only once the play world is actually gone).
    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetBoolField(TEXT("was_running"), bWasRunning);
    ResultJson->SetBoolField(TEXT("pie_running"), GetPlayWorld() != nullptr);
    ResultJson->SetStringField(TEXT("hint"),
        TEXT("poll pie_running until false; runtime reads before that still answer from the live session"));
    return ResultJson;
}

TSharedPtr<FJsonObject> FUnrealMCPPIECommands::HandleGetActorPose(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorLabel;
    if (!Params->TryGetStringField(TEXT("actor_label"), ActorLabel) || ActorLabel.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'actor_label' parameter"));
    }

    const TArray<TSharedPtr<FJsonValue>>* BonesJson = nullptr;
    if (!Params->TryGetArrayField(TEXT("bones"), BonesJson) || !BonesJson)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'bones' array parameter"));
    }

    UWorld* PlayWorld = GetPlayWorld();
    if (!PlayWorld)
    {
        // A stale answer here is worse than no answer: the caller would compare an editor-world pose
        // against a PIE-time expectation and conclude the rig froze.
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("not_in_pie"),
            TEXT("no PIE session is running; call start_pie and poll pie_running before reading a pose"));
    }

    AActor* TargetActor = nullptr;
    for (TActorIterator<AActor> It(PlayWorld); It; ++It)
    {
        AActor* Actor = *It;
#if WITH_EDITOR
        const FString Candidate = Actor->GetActorLabel();
#else
        const FString Candidate = Actor->GetName();
#endif
        if (Candidate == ActorLabel)
        {
            TargetActor = Actor;
            break;
        }
    }

    if (!TargetActor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("actor_not_found"),
            FString::Printf(TEXT("no actor labelled '%s' in the PIE world (the label is the editor label, which a spawned clone keeps)"), *ActorLabel));
    }

    USkeletalMeshComponent* SkeletalComponent = TargetActor->FindComponentByClass<USkeletalMeshComponent>();
    if (!SkeletalComponent)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("no_skeletal_mesh_component"),
            FString::Printf(TEXT("actor '%s' has no SkeletalMeshComponent"), *ActorLabel));
    }

    const USkeletalMesh* SkeletalMesh = SkeletalComponent->GetSkeletalMeshAsset();
    const FReferenceSkeleton* RefSkeleton = SkeletalMesh ? &SkeletalMesh->GetRefSkeleton() : nullptr;

    TArray<TSharedPtr<FJsonValue>> FoundJson;
    TArray<TSharedPtr<FJsonValue>> MissingJson;
    TSharedPtr<FJsonObject> PosesJson = MakeShareable(new FJsonObject);

    for (const TSharedPtr<FJsonValue>& BoneValue : *BonesJson)
    {
        const FString BoneNameText = BoneValue.IsValid() ? BoneValue->AsString() : FString();
        if (BoneNameText.IsEmpty())
        {
            continue;
        }

        // Validate against the actual reference skeleton BEFORE asking for a transform:
        // GetBoneTransform() falls back to the component transform for an unknown name, which is how a
        // typo turns into a plausible constant pose. Unknown names only ever reach `missing[]`.
        const bool bKnownBone = RefSkeleton && RefSkeleton->FindBoneIndex(FName(*BoneNameText)) != INDEX_NONE;
        if (!bKnownBone)
        {
            MissingJson.Add(MakeShared<FJsonValueString>(BoneNameText));
            continue;
        }

        const FVector Location = SkeletalComponent->GetBoneTransform(FName(*BoneNameText)).GetLocation();
        TArray<TSharedPtr<FJsonValue>> VectorJson;
        VectorJson.Add(MakeShared<FJsonValueNumber>(Location.X));
        VectorJson.Add(MakeShared<FJsonValueNumber>(Location.Y));
        VectorJson.Add(MakeShared<FJsonValueNumber>(Location.Z));
        PosesJson->SetArrayField(BoneNameText, VectorJson);
        FoundJson.Add(MakeShared<FJsonValueString>(BoneNameText));
    }

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    // The read itself succeeded even when some names were unknown: those are already carried by
    // missing[], and turning a partial hit into a failed call would make callers either ignore the
    // data or treat a typo as a broken rig. `partial` is the flag for that case.
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetBoolField(TEXT("partial"), MissingJson.Num() > 0);
    ResultJson->SetStringField(TEXT("actor_label"), ActorLabel);
    ResultJson->SetStringField(TEXT("actor_name"), TargetActor->GetName());
    ResultJson->SetStringField(TEXT("skeletal_mesh"),
        SkeletalMesh ? SkeletalMesh->GetPathName() : FString());
    // Which frame this sample belongs to: two calls returning the same counter means nothing moved,
    // which is the "is it frozen" question this command exists to answer.
    ResultJson->SetNumberField(TEXT("frame"), (double)GFrameCounter);
    ResultJson->SetNumberField(TEXT("world_time_seconds"), PlayWorld->GetTimeSeconds());
    ResultJson->SetArrayField(TEXT("found"), FoundJson);
    ResultJson->SetArrayField(TEXT("missing"), MissingJson);
    ResultJson->SetField(TEXT("poses"), MakeShared<FJsonValueObject>(PosesJson));
    if (MissingJson.Num() > 0)
    {
        ResultJson->SetStringField(TEXT("hint"),
            TEXT("names in missing[] are not bones of this actor's skeleton; they are NOT reported with a fallback transform"));
    }
    return ResultJson;
}

void FUnrealMCPPIECommands::RegisterCommands(FMCPCommandRegistry& Registry)
{
    MCP_REGISTER_COMMAND(Registry, "start_pie", "pie",
        "Request a real Play-in-Editor session (same as pressing Play: the game mode spawns its default "
        "pawn, so the player pawn exists - unlike Simulate) and return immediately (never waits: PIE comes "
        "up on a later frame, polling here would freeze the GameThread). Poll pie_running to know when it is up.",
        (TArray<FMCPParamSpec>{}), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleStartPIE(Params); });

    MCP_REGISTER_COMMAND(Registry, "stop_pie", "pie",
        "Request the end of the PIE session and return immediately; poll pie_running until false.",
        (TArray<FMCPParamSpec>{}), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleStopPIE(Params); });

    MCP_REGISTER_COMMAND(Registry, "get_actor_pose", "pie",
        "Read world-space bone locations of an actor in the PIE world. Names that are not bones of its "
        "skeleton are reported in missing[] - never substituted with the actor transform. Outside PIE it "
        "fails with not_in_pie instead of answering from the editor world.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("actor_label"), TEXT("string"), TEXT("Actor label in the PIE world")),
            MCPParam(TEXT("bones"), TEXT("array"), TEXT("Bone names to sample, e.g. [\"pelvis\", \"head\"]")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleGetActorPose(Params); });
}
