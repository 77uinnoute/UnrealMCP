#pragma once

#include "CoreMinimal.h"
#include "Json.h"
#include "Core/MCPCommandRegistry.h"

/**
 * Handler class for Blueprint-related MCP commands
 */
class UNREALMCP_API FUnrealMCPBlueprintCommands
{
public:
    FUnrealMCPBlueprintCommands();

    /** Declare this domain's commands (name, category, params, flags) in the process-wide table. */
    void RegisterCommands(FMCPCommandRegistry& Registry);

private:
    /**
     * Run one command's body through the per-command wrapper: a successful mutation that the registry
     * marks with bPersistAfterSuccess is written to disk immediately (the editor is routinely killed
     * by the build script, so unsaved edits would be lost).
     */
    TSharedPtr<FJsonObject> RunCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params,
        const TFunction<TSharedPtr<FJsonObject>(const TSharedPtr<FJsonObject>&)>& Body);

    // Specific blueprint command handlers
    TSharedPtr<FJsonObject> HandleCreateBlueprint(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddComponentToBlueprint(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAttachComponentToComponent(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetComponentProperty(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetPhysicsProperties(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCompileBlueprint(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSpawnBlueprintActor(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetBlueprintProperty(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetStaticMeshProperties(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetPawnProperties(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetAssetProperties(const TSharedPtr<FJsonObject>& Params);

    // Structural editing: member variables and local variables
    TSharedPtr<FJsonObject> HandleListBlueprintVariables(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetBlueprintVariableInfo(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveBlueprintVariable(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRenameBlueprintVariable(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetBlueprintVariableType(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetBlueprintVariableDefaultValue(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetBlueprintVariableFlags(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListBlueprintLocalVariables(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddBlueprintLocalVariable(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveBlueprintLocalVariable(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRenameBlueprintLocalVariable(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetBlueprintLocalVariableDefault(const TSharedPtr<FJsonObject>& Params);

    // Structural editing: component hierarchy
    TSharedPtr<FJsonObject> HandleGetBlueprintComponentHierarchy(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveComponentFromBlueprint(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetBlueprintRootComponent(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleDetachComponent(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetComponentCollision(const TSharedPtr<FJsonObject>& Params);

    // Structural editing: interfaces, existence checks and blueprint comparison
    TSharedPtr<FJsonObject> HandleImplementBlueprintInterface(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleUnimplementBlueprintInterface(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListBlueprintInterfaces(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListOverridableFunctions(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCheckBlueprintElement(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCompareBlueprints(const TSharedPtr<FJsonObject>& Params);

    // Helper functions
    TSharedPtr<FJsonObject> AddComponentToBlueprint(const FString& BlueprintName, const FString& ComponentType, 
                                                   const FString& ComponentName, const FString& MeshType,
                                                   const TArray<float>& Location, const TArray<float>& Rotation,
                                                   const TArray<float>& Scale, const TSharedPtr<FJsonObject>& ComponentProperties);
}; 