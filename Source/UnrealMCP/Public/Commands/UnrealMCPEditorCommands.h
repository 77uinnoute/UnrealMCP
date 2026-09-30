#pragma once

#include "CoreMinimal.h"
#include "Json.h"
#include "Core/MCPCommandRegistry.h"

/**
 * Handler class for Editor-related MCP commands
 * Handles viewport control, actor manipulation, and level management
 */
class UNREALMCP_API FUnrealMCPEditorCommands
{
public:
    FUnrealMCPEditorCommands();

    /** Declare this domain's commands (name, category, params, flags) in the process-wide table. */
    void RegisterCommands(FMCPCommandRegistry& Registry);

    /**
     * Close every editor tab showing this asset and put the engine's "where does this asset open
     * next time" record back the way it was (an MCP-driven close must not move the user's tab).
     * Returns the number of closed editors (0 when none was open). Shared by close_asset_editors
     * and by the particle commands' auto_close escape hatch.
     */
    static int32 CloseAssetEditorsFor(UObject* Asset);

private:
    // Actor manipulation commands
    TSharedPtr<FJsonObject> HandleGetActorsInLevel(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleFindActorsByName(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSpawnActor(const TSharedPtr<FJsonObject>& Params);

    /** Spawn an AEmitter for a particle system asset, optionally attached to another actor. */
    TSharedPtr<FJsonObject> HandleSpawnParticleActor(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleDeleteActor(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetActorTransform(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetActorLocationSafe(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetActorCustomDepthSafe(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetActorProperties(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetActorProperty(const TSharedPtr<FJsonObject>& Params);

    // Blueprint actor spawning
    TSharedPtr<FJsonObject> HandleSpawnBlueprintActor(const TSharedPtr<FJsonObject>& Params);

    // Editor viewport commands
    TSharedPtr<FJsonObject> HandleFocusViewport(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleTakeScreenshot(const TSharedPtr<FJsonObject>& Params);

    // Console variable commands (python has no API for reading cvar values)
    TSharedPtr<FJsonObject> HandleGetConsoleVariable(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetConsoleVariable(const TSharedPtr<FJsonObject>& Params);

    // Asset editor session commands
    TSharedPtr<FJsonObject> HandleCloseAssetEditors(const TSharedPtr<FJsonObject>& Params);
}; 