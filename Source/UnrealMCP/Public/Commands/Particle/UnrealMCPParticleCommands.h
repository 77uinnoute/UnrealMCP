#pragma once

#include "CoreMinimal.h"
#include "Json.h"
#include "Core/MCPCommandRegistry.h"

/**
 * Handler class for Cascade (UParticleSystem) MCP commands.
 * Mirrors the other domain handlers: the bridge routes the particle command types here,
 * each command is implemented in a private HandleXxx method returning a result object with
 * a "success" field, and RunCommand wraps the dispatch with one undo step per command
 * plus an immediate save on success.
 */
class UNREALMCP_API FUnrealMCPParticleCommands
{
public:
    FUnrealMCPParticleCommands();

    // Register this domain's commands with the process-wide command registry.
    void RegisterCommands(FMCPCommandRegistry& Registry);

private:
    // Wraps one command body with the response shaping (detail / fields / include_modules),
    // auto_close, one undo step per command (from the entry's mutates_graph flag) and the
    // immediate save on success.
    TSharedPtr<FJsonObject> RunCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params,
                                       const TFunction<TSharedPtr<FJsonObject>(const TSharedPtr<FJsonObject>&)>& Body);

    // Reads
    TSharedPtr<FJsonObject> HandleListParticleEmitters(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListParticleModules(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetParticleModule(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleValidateParticleSystem(const TSharedPtr<FJsonObject>& Params);

    // Writes
    TSharedPtr<FJsonObject> HandleCreateParticleSystem(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddParticleEmitter(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveParticleEmitter(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddParticleModule(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleMoveParticleModule(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleDuplicateParticleEmitter(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetParticleEmitterName(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetParticleLODEnabled(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveParticleModule(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetParticleModuleProperty(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetParticleBursts(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetParticleDistribution(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetParticleLODCount(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCopyParticleLOD(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetParticleLODDistance(const TSharedPtr<FJsonObject>& Params);
};
