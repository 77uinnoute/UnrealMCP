#pragma once

#include "CoreMinimal.h"
#include "Json.h"
#include "Core/MCPCommandRegistry.h"

// Forward declarations
class UBlueprint;

/**
 * Handler class for Blueprint Node-related MCP commands
 */
class UNREALMCP_API FUnrealMCPBlueprintNodeCommands
{
public:
    FUnrealMCPBlueprintNodeCommands();

    /** Declare this domain's commands (name, category, params, flags) in the process-wide table. */
    void RegisterCommands(FMCPCommandRegistry& Registry);

private:
    /**
     * Run one command's body through the per-command wrapper: one MCP command is one undo step
     * (cancelled when the command fails), and a successful mutation that the registry marks with
     * bPersistAfterSuccess is written to disk immediately.
     */
    TSharedPtr<FJsonObject> RunCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params,
        const TFunction<TSharedPtr<FJsonObject>(const TSharedPtr<FJsonObject>&)>& Body);

    // Specific blueprint node command handlers
    TSharedPtr<FJsonObject> HandleConnectBlueprintNodes(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddBlueprintGetSelfComponentReference(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddBlueprintEvent(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddBlueprintFunctionCall(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddBlueprintVariable(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddBlueprintVariableNode(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddBlueprintLiteralNode(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddBlueprintNodeByClass(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddBlueprintInputActionNode(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddBlueprintSelfReference(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleFindBlueprintNodes(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleVerifyBlueprintGraph(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListBlueprintGraphs(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddBlueprintFunctionGraph(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveBlueprintFunctionGraph(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRenameBlueprintFunctionGraph(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListBlueprintFunctionGraphs(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddBlueprintFunctionParam(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveBlueprintFunctionParam(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRenameBlueprintFunctionParam(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetBlueprintPinDefault(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetBlueprintNodeProperty(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleDeleteBlueprintNodes(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleDisconnectBlueprintPins(const TSharedPtr<FJsonObject>& Params);

    // Structural editing: node position, pin shape, custom events, editor focus
    TSharedPtr<FJsonObject> HandleMoveBlueprintNode(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRefreshBlueprintNode(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSplitBlueprintPin(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRecombineBlueprintPin(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddBlueprintCustomEventNode(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRenameBlueprintCustomEvent(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleOpenBlueprintGraph(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleFocusBlueprintNode(const TSharedPtr<FJsonObject>& Params);

    // --- Helpers for HandleFindBlueprintNodes ---
    // Find any supported asset (Blueprint/Material/MaterialFunction) by name, including __current__
    UObject* FindAssetForNodes(const FString& AssetName);
    // Dispatch to type-specific node extraction
    TSharedPtr<FJsonObject> ProcessAssetNodes(UObject* Asset, const FString& NodeType, const TSharedPtr<FJsonObject>& Params);
    // Extract nodes from a Blueprint graph (read-only: never creates a graph)
    TSharedPtr<FJsonObject> ProcessBlueprintNodes(UBlueprint* Blueprint, const FString& NodeType, const TSharedPtr<FJsonObject>& Params);
};
