#pragma once

#include "CoreMinimal.h"
#include "Json.h"
#include "Core/MCPCommandRegistry.h"

/**
 * PIE (play-in-editor) lifecycle commands.
 *
 * These exist because the lifecycle had no tool surface at all: callers reached for
 * EditorLevelLibrary side doors, and the two readings that mattered lied -
 *
 *   - start: MUST NOT wait. PIE comes up on a later frame, so a command that "waits for ready"
 *     can only wait by sleeping/polling on the GameThread, which freezes the whole MCP channel.
 *     start_pie therefore only requests the session and reports `pie_running` for the caller to poll.
 *   - get_actor_pose: a bone name that is not in the skeleton MUST be reported in `missing[]`.
 *     GetBoneTransform would otherwise fall back to the component's own transform and hand back a
 *     plausible-looking constant (2700, 2000, 95) that reads like a frozen rig.
 */
class UNREALMCP_API FUnrealMCPPIECommands
{
public:
    FUnrealMCPPIECommands();

    // Register this domain's commands with the process-wide command registry.
    void RegisterCommands(FMCPCommandRegistry& Registry);

private:
    TSharedPtr<FJsonObject> HandleStartPIE(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleStopPIE(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetActorPose(const TSharedPtr<FJsonObject>& Params);

    /**
     * Push a key event through the game viewport, the same door a physical key uses.
     *
     * This is the only route that exercises the whole input chain (viewport -> player input ->
     * mappings -> bindings), including the SetIgnoreInput gate that UPlayerInput::InputKey sits
     * behind. The event is processed later in the same frame, so nothing read during the call can
     * say whether it landed: the viewport's return value is reported for the record but reads
     * FALSE for a key that feeds an axis mapping, and a pressed-state read returns the state from
     * before the event (both measured on a DefaultPawn that then flew). Proving the effect is the
     * caller's read-back of the game's own state.
     */
    TSharedPtr<FJsonObject> HandleInjectKey(const TSharedPtr<FJsonObject>& Params);

    /** The PIE world, or null when the editor is not playing. Never the editor world. */
    static UWorld* GetPlayWorld();

    /**
     * Blueprints sitting in BS_Error, each with the messages its graph nodes carry.
     *
     * PIE refuses to start while one exists and asks with a modal dialog. Nothing suppresses that
     * dialog plugin-wide (the unattended scopes live in the asset commands), so from the outside the
     * session simply never appears. This preflight is what turns that silence into a named blocker.
     */
    static void CollectCompileErrorBlueprints(TArray<TSharedPtr<FJsonValue>>& OutBlueprints);

    /**
     * The most recent session-log lines mentioning any of the given asset names.
     *
     * The RigVM / Kismet compiler writes its real messages to LogBlueprint (prefixed [AssetLog])
     * and only some of them also land on a graph node, so the log is the only place the text is
     * complete. Lines from earlier failed compiles can still be in range - this is a tail, not a
     * per-request slice.
     */
    static void CollectBlueprintLogErrors(const TArray<FString>& AssetNames, TArray<FString>& OutLines);
};
