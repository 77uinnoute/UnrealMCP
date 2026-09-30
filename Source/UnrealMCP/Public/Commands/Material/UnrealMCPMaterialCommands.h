#pragma once

#include "CoreMinimal.h"
#include "Json.h"
#include "Core/MCPCommandRegistry.h"

/**
 * Handler class for Material-related MCP commands.
 * Mirrors the other domain command handlers (Editor/Blueprint/UMG/...): each command is
 * implemented in a private HandleXxx method returning a result object with a "success"
 * field, and registered with the process-wide command registry in RegisterCommands().
 */
class UNREALMCP_API FUnrealMCPMaterialCommands
{
public:
    FUnrealMCPMaterialCommands();

    // Register this domain's commands with the process-wide command registry.
    void RegisterCommands(FMCPCommandRegistry& Registry);

private:
    // Wraps one command body: stamps the material's mutation time (from the entry's
    // mutates_graph flag) so compile diagnostics can tell the current compile apart from
    // historical log noise.
    TSharedPtr<FJsonObject> RunCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params,
                                       const TFunction<TSharedPtr<FJsonObject>(const TSharedPtr<FJsonObject>&)>& Body);

    TSharedPtr<FJsonObject> HandleConvertStaticSwitchToDynamic(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleScanMaterialCustomNodes(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleConnectMaterialPin(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleConnectMaterialExpressions(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRecompileMaterial(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetMaterialCompileErrors(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListMaterialExpressions(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetMaterialGraph(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetMaterialExpressionProperty(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetMaterialExpressionProperty(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCreateMaterialExpression(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleDeleteMaterialExpression(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleDeleteMaterialExpressions(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleWipeMaterialGraph(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleDisconnectMaterialProperty(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetMaterialParameters(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetMaterialParameters(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleValidateCustomHlsl(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleValidateCustomExpression(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetCustomInputName(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddCustomInput(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveCustomInput(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleBuildMaterialPreview(const TSharedPtr<FJsonObject>& Params);
};
