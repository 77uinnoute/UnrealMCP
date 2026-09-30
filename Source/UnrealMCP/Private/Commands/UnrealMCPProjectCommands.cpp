#include "Commands/UnrealMCPProjectCommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "GameFramework/InputSettings.h"

FUnrealMCPProjectCommands::FUnrealMCPProjectCommands()
{
}

void FUnrealMCPProjectCommands::RegisterCommands(FMCPCommandRegistry& Registry)
{
    MCP_REGISTER_COMMAND(Registry, "create_input_mapping", "project", "Add an action key mapping to the project's input settings.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("action_name"), TEXT("string"), TEXT("Input action name")),
            MCPParam(TEXT("key"), TEXT("string"), TEXT("Key name (e.g. SpaceBar, LeftMouseButton)")),
            MCPParamOpt(TEXT("shift"), TEXT("bool"), TEXT("Require Shift")),
            MCPParamOpt(TEXT("ctrl"), TEXT("bool"), TEXT("Require Ctrl")),
            MCPParamOpt(TEXT("alt"), TEXT("bool"), TEXT("Require Alt")),
            MCPParamOpt(TEXT("cmd"), TEXT("bool"), TEXT("Require Cmd")),
        }), MCPFlags(),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleCreateInputMapping(Params); });
}

TSharedPtr<FJsonObject> FUnrealMCPProjectCommands::HandleCreateInputMapping(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString ActionName;
    if (!Params->TryGetStringField(TEXT("action_name"), ActionName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'action_name' parameter"));
    }

    FString Key;
    if (!Params->TryGetStringField(TEXT("key"), Key))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'key' parameter"));
    }

    // Get the input settings
    UInputSettings* InputSettings = GetMutableDefault<UInputSettings>();
    if (!InputSettings)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get input settings"));
    }

    // Create the input action mapping
    FInputActionKeyMapping ActionMapping;
    ActionMapping.ActionName = FName(*ActionName);
    ActionMapping.Key = FKey(*Key);

    // Add modifiers if provided
    if (Params->HasField(TEXT("shift")))
    {
        ActionMapping.bShift = Params->GetBoolField(TEXT("shift"));
    }
    if (Params->HasField(TEXT("ctrl")))
    {
        ActionMapping.bCtrl = Params->GetBoolField(TEXT("ctrl"));
    }
    if (Params->HasField(TEXT("alt")))
    {
        ActionMapping.bAlt = Params->GetBoolField(TEXT("alt"));
    }
    if (Params->HasField(TEXT("cmd")))
    {
        ActionMapping.bCmd = Params->GetBoolField(TEXT("cmd"));
    }

    // Add the mapping. bForceRebuildKeymaps stays at its default (true) so the action is usable
    // immediately; TryUpdateDefaultConfigFile is what makes it SURVIVE AN EDITOR RESTART - plain
    // SaveConfig() left no DefaultInput.ini behind, so the mapping vanished on the next launch and
    // every InputAction node referencing it failed to compile.
    InputSettings->AddActionMapping(ActionMapping);
    const bool bPersisted = InputSettings->TryUpdateDefaultConfigFile();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("action_name"), ActionName);
    ResultObj->SetStringField(TEXT("key"), Key);
    ResultObj->SetBoolField(TEXT("persisted"), bPersisted);
    if (!bPersisted)
    {
        ResultObj->SetStringField(TEXT("warning"),
            TEXT("The mapping is live in this session but could not be written to Config/DefaultInput.ini"));
    }
    return ResultObj;
} 