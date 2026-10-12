#pragma once

#include "CoreMinimal.h"
#include "Json.h"
#include "Core/MCPCommandRegistry.h"

/**
 * Handler class for Niagara (UNiagaraSystem) MCP commands.
 *
 * Mirrors the other domains: the JSON layer lives here, every operation is implemented in
 * FUnrealMCPNiagaraOps, and each write is a single command = single undo step = one save on
 * success. The Niagara graph itself is only ever edited through the engine's own stack utilities
 * (see UnrealMCPNiagaraOps.h for what NiagaraEditor does and does not export).
 */
class UNREALMCP_API FUnrealMCPNiagaraCommands
{
public:
    FUnrealMCPNiagaraCommands();

    void RegisterCommands(FMCPCommandRegistry& Registry);

private:
    /** One undo step per mutating command, plus the immediate save on success. */
    TSharedPtr<FJsonObject> RunCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params,
                                       const TFunction<TSharedPtr<FJsonObject>(const TSharedPtr<FJsonObject>&)>& Body);

    // Assets
    TSharedPtr<FJsonObject> HandleCreateNiagaraSystem(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleDuplicateNiagaraSystem(const TSharedPtr<FJsonObject>& Params);

    // Emitters
    TSharedPtr<FJsonObject> HandleListNiagaraEmitters(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddNiagaraEmitter(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveNiagaraEmitter(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetNiagaraEmitterProperties(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddNiagaraRenderer(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveNiagaraRenderer(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetNiagaraRendererMaterial(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddNiagaraSimulationStage(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetNiagaraSimulationStage(const TSharedPtr<FJsonObject>& Params);

    // Compile
    TSharedPtr<FJsonObject> HandleCompileNiagaraSystem(const TSharedPtr<FJsonObject>& Params);

    // Parameters
    TSharedPtr<FJsonObject> HandleGetNiagaraParameters(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetNiagaraParameter(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetNiagaraDataInterfaceProperties(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListNiagaraGridAttributes(const TSharedPtr<FJsonObject>& Params);

    // Stack modules
    TSharedPtr<FJsonObject> HandleListNiagaraModules(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddNiagaraModule(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetNiagaraModuleEnabled(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetNiagaraModuleInput(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveNiagaraModule(const TSharedPtr<FJsonObject>& Params);

    // Scratch-pad graphs
    TSharedPtr<FJsonObject> HandleCreateNiagaraScratchPadModule(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListNiagaraScratchPadNodes(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetNiagaraCustomHlsl(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetNiagaraScratchPadPins(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveNiagaraScratchPadNode(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveNiagaraScratchPadPin(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleConnectNiagaraScratchPadPins(const TSharedPtr<FJsonObject>& Params);

    // Level actor + component parameters
    TSharedPtr<FJsonObject> HandleSpawnNiagaraActor(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetNiagaraComponentParameter(const TSharedPtr<FJsonObject>& Params);
};
