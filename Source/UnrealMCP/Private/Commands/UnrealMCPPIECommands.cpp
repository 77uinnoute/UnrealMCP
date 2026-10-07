#include "Commands/UnrealMCPPIECommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Core/MCPCommandRegistry.h"

#include "CoreGlobals.h"
#include "Editor.h"
#include "EditorSubsystem.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "Components/SkeletalMeshComponent.h"
#include "InputCoreTypes.h"
#include "InputKeyEventArgs.h"
#include "Misc/CoreMiscDefines.h"
#include "LevelEditorSubsystem.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ReferenceSkeleton.h"
#include "UObject/UObjectIterator.h"

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

    // PIE refuses to start while a Blueprint has compile errors, and asks that question with a modal
    // dialog. Nothing suppresses modals plugin-wide (the only GIsRunningUnattendedScript scopes live
    // in the asset commands), so the request used to go nowhere visible and the caller polled
    // pie_running forever. Name the blockers instead, before requesting anything.
    bool bForce = false;
    Params->TryGetBoolField(TEXT("force"), bForce);

    TArray<TSharedPtr<FJsonValue>> Blockers;
    CollectCompileErrorBlueprints(Blockers);
    if (Blockers.Num() > 0 && !bForce)
    {
        TArray<FString> Names;
        for (const TSharedPtr<FJsonValue>& Value : Blockers)
        {
            const TSharedPtr<FJsonObject>* ObjectJson = nullptr;
            if (!Value.IsValid() || !Value->TryGetObject(ObjectJson) || !ObjectJson)
            {
                continue;
            }
            FString AssetPath;
            if (!(*ObjectJson)->TryGetStringField(TEXT("asset_path"), AssetPath))
            {
                continue;
            }
            int32 LastDot = INDEX_NONE;
            Names.Add(AssetPath.FindLastChar(TEXT('.'), LastDot) ? AssetPath.RightChop(LastDot + 1) : AssetPath);
        }

        TArray<FString> LogLines;
        CollectBlueprintLogErrors(Names, LogLines);

        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("pie_blocked_by_compile_errors"),
            FString::Printf(TEXT("%d Blueprint(s) have compile errors, so PIE will not start (the engine asks "
                                 "that question with a modal dialog). Compile them first, or pass force=true."),
                            Blockers.Num()));
        Error->SetArrayField(TEXT("compile_error_blueprints"), Blockers);

        TArray<TSharedPtr<FJsonValue>> LogValues;
        for (const FString& Line : LogLines)
        {
            LogValues.Add(MakeShared<FJsonValueString>(Line));
        }
        Error->SetArrayField(TEXT("log_errors_tail"), LogValues);
        return Error;
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

TSharedPtr<FJsonObject> FUnrealMCPPIECommands::HandleInjectKey(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* PlayWorld = GetPlayWorld();
    if (!PlayWorld)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("not_in_pie"),
            TEXT("no Play-in-Editor session is running; inject_key drives the game viewport only"));
    }

    FString KeyName;
    if (!Params->TryGetStringField(TEXT("key"), KeyName) || KeyName.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_key"),
            TEXT("pass 'key': the FKey name a binding names, e.g. \"W\", \"SpaceBar\", \"LeftMouseButton\""));
    }

    // The key name, not the mapped action: a binding maps a key to an action, and this command sits
    // on the key side of that mapping on purpose (see the description).
    const FKey Key(*KeyName);
    if (!Key.IsValid())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unknown_key"),
            FString::Printf(TEXT("'%s' is not a known FKey name"), *KeyName));
    }

    FString EventName = TEXT("tap");
    Params->TryGetStringField(TEXT("event"), EventName);
    EventName = EventName.ToLower();
    if (EventName != TEXT("press") && EventName != TEXT("release") && EventName != TEXT("tap"))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_event"),
            FString::Printf(TEXT("event must be press / release / tap, got '%s'"), *EventName));
    }

    UGameViewportClient* ViewportClient = PlayWorld->GetGameViewport();
    if (!ViewportClient || !ViewportClient->Viewport)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("no_viewport"),
            TEXT("the play world has no game viewport: the input chain has no door to open"));
    }

    // The same shape a physical key produces: FInputKeyEventArgs carries the viewport that received
    // it, and the device id is the primary one (a keyboard reports as internal id 0).
    auto Push = [ViewportClient, &Key](EInputEvent Event)
    {
        return ViewportClient->InputKey(FInputKeyEventArgs(ViewportClient->Viewport,
            FInputDeviceId::CreateFromInternalId(0), Key, Event, /*EventTimestamp=*/uint64(0)));
    };

    TArray<TSharedPtr<FJsonValue>> EventJson;
    auto Record = [&EventJson](const TCHAR* Name, bool bHandled)
    {
        TSharedPtr<FJsonObject> Entry = MakeShareable(new FJsonObject);
        Entry->SetStringField(TEXT("event"), Name);
        Entry->SetBoolField(TEXT("viewport_handled"), bHandled);
        EventJson.Add(MakeShareable(new FJsonValueObject(Entry)));
    };

    bool bViewportHandled = false;
    if (EventName == TEXT("press") || EventName == TEXT("tap"))
    {
        const bool bHandled = Push(IE_Pressed);
        Record(TEXT("pressed"), bHandled);
        bViewportHandled |= bHandled;
    }
    if (EventName == TEXT("release") || EventName == TEXT("tap"))
    {
        const bool bHandled = Push(IE_Released);
        Record(TEXT("released"), bHandled);
        bViewportHandled |= bHandled;
    }

    APlayerController* PlayerController = PlayWorld->GetFirstPlayerController();
    UPlayerInput* PlayerInput = PlayerController ? PlayerController->PlayerInput : nullptr;

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("key"), Key.GetFName().ToString());
    ResultJson->SetStringField(TEXT("event"), EventName);
    ResultJson->SetBoolField(TEXT("viewport_handled"), bViewportHandled);
    if (PlayerInput)
    {
        // Which input layer the event fed - the useful, stable part of the input side. The pressed
        // state is deliberately NOT reported: see the note.
        ResultJson->SetStringField(TEXT("player_input_class"), PlayerInput->GetClass()->GetName());
    }
    ResultJson->SetArrayField(TEXT("events"), EventJson);
    ResultJson->SetNumberField(TEXT("frame"), (double)GFrameCounter);
    ResultJson->SetNumberField(TEXT("world_time_seconds"), PlayWorld->GetTimeSeconds());
    ResultJson->SetStringField(TEXT("note"),
        TEXT("pushed through UGameViewportClient::InputKey - the door a physical key uses, SetIgnoreInput gate "
             "included. `viewport_handled` is only the viewport's return value: it is FALSE for a key that feeds "
             "an axis mapping even though the input is used (measured: W returned false and the pawn then flew), "
             "so never read it as success. There is also no same-frame delivery readback - the event is processed "
             "later in the frame, so a pressed-state read taken during this call returns the state from BEFORE it "
             "(measured: false on the press frame, true on the release frame, while the input worked both times). "
             "Which binding reacted is not knowable from here: read the game's own state back (get_actor_pose / "
             "the pawn's location / a gameplay readout) - that is the only proof."));
    return ResultJson;
}

void FUnrealMCPPIECommands::RegisterCommands(FMCPCommandRegistry& Registry)
{
    MCP_REGISTER_COMMAND(Registry, "start_pie", "pie",
        "Request a real Play-in-Editor session (same as pressing Play: the game mode spawns its default "
        "pawn, so the player pawn exists - unlike Simulate) and return immediately (never waits: PIE comes "
        "up on a later frame, polling here would freeze the GameThread). Poll pie_running to know when it is up. "
        "Preflight: a Blueprint in BS_Error makes PIE refuse to start and ask with a modal dialog, "
        "so the request is refused here with pie_blocked_by_compile_errors plus the offending assets (and the "
        "matching LogBlueprint lines, where the RigVM compiler writes its real text). force=true requests anyway.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("force"), TEXT("bool"), TEXT("Request the session even when blueprints have compile errors (default false)")),
        }), MCPFlags(false, false, false),
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

    MCP_REGISTER_COMMAND(Registry, "inject_key", "pie",
        "Press / release / tap a KEY in the running PIE session, pushed through the game viewport - the same "
        "door a physical key uses (SetIgnoreInput gate included), which is what a UI-focused or input-ignoring "
        "state is supposed to block. This is the key side of a key->action binding, so it is what proves the "
        "MAPPING; an action-level inject only proves the action reaches logic. `viewport_handled` is only the "
        "viewport's return value and is FALSE for a key that feeds an axis mapping even though the input is used "
        "(measured on a DefaultPawn), so never read it as success. There is no same-frame delivery readback "
        "either: the event is processed later in the frame, so a pressed-state read taken during the call "
        "reflects the state from before it. Which binding reacted is not knowable from here: read the game's own "
        "state back (get_actor_pose / the pawn's location) - that is the only proof. Fails with not_in_pie "
        "outside a session and never blocks.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("key"), TEXT("string"), TEXT("FKey name, e.g. \"W\", \"SpaceBar\", \"LeftMouseButton\" (the key, not the mapped action)")),
            MCPParamOpt(TEXT("event"), TEXT("string"), TEXT("press / release / tap (default tap = press then release)")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleInjectKey(Params); });
}

void FUnrealMCPPIECommands::CollectCompileErrorBlueprints(TArray<TSharedPtr<FJsonValue>>& OutBlueprints)
{
    for (TObjectIterator<UBlueprint> It; It; ++It)
    {
        UBlueprint* Blueprint = *It;
        if (!Blueprint || Blueprint->Status != BS_Error)
        {
            continue;
        }

        TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
        Item->SetStringField(TEXT("blueprint_class"), Blueprint->GetClass()->GetName());

        TArray<UEdGraph*> Graphs;
        Graphs.Append(Blueprint->UbergraphPages);
        Graphs.Append(Blueprint->FunctionGraphs);
        Graphs.Append(Blueprint->MacroGraphs);

        TArray<TSharedPtr<FJsonValue>> Messages;
        for (UEdGraph* Graph : Graphs)
        {
            if (!Graph)
            {
                continue;
            }
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node && !Node->ErrorMsg.IsEmpty())
                {
                    Messages.Add(MakeShared<FJsonValueString>(FString::Printf(
                        TEXT("%s: %s"), *Node->GetNodeTitle(ENodeTitleType::ListView).ToString(), *Node->ErrorMsg)));
                }
            }
        }
        Item->SetArrayField(TEXT("node_errors"), Messages);
        OutBlueprints.Add(MakeShared<FJsonValueObject>(Item));
    }
}

void FUnrealMCPPIECommands::CollectBlueprintLogErrors(const TArray<FString>& AssetNames, TArray<FString>& OutLines)
{
    if (AssetNames.Num() == 0)
    {
        return;
    }

    const FString LogFile = FPaths::ProjectLogDir() / (FString(FApp::GetProjectName()) + TEXT(".log"));
    FString Contents;
    if (!FFileHelper::LoadFileToString(Contents, *LogFile))
    {
        return;
    }

    TArray<FString> Lines;
    Contents.ParseIntoArrayLines(Lines);
    const int32 First = FMath::Max(0, Lines.Num() - 500);
    for (int32 Index = First; Index < Lines.Num() && OutLines.Num() < 40; ++Index)
    {
        const FString& Line = Lines[Index];
        if (!Line.Contains(TEXT("LogBlueprint: Error")) && !Line.Contains(TEXT("[AssetLog]")))
        {
            continue;
        }
        for (const FString& Name : AssetNames)
        {
            if (Line.Contains(Name))
            {
                OutLines.Add(Line);
                break;
            }
        }
    }
}
