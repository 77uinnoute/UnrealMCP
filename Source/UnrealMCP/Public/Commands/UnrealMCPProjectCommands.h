#pragma once

#include "CoreMinimal.h"
#include "Json.h"
#include "Core/MCPCommandRegistry.h"

/**
 * Handler class for Project-wide MCP commands
 */
class UNREALMCP_API FUnrealMCPProjectCommands
{
public:
    FUnrealMCPProjectCommands();

    /** Declare this domain's commands (name, category, params, flags) in the process-wide table. */
    void RegisterCommands(FMCPCommandRegistry& Registry);

private:
    // Specific project command handlers
    TSharedPtr<FJsonObject> HandleCreateInputMapping(const TSharedPtr<FJsonObject>& Params);
}; 