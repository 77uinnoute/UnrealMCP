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

    /** The PIE world, or null when the editor is not playing. Never the editor world. */
    static UWorld* GetPlayWorld();
};
