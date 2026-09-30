#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UnrealMCPPythonAPI.generated.h"

/**
 * Editor python loopback entry into the MCP bridge.
 *
 * Editor python scripts (run via execute_python_command / execute_python_file)
 * already execute on the GameThread, so they MUST NOT send commands to the
 * bridge over TCP: the bridge would queue the command back onto the
 * GameThread, the script would block waiting for the response, and the
 * command would block waiting for the script to finish -> guaranteed
 * deadlock. Instead, scripts call this reflected static function which
 * dispatches synchronously through the same command handlers the TCP path
 * uses.
 *
 * Usage (editor python):
 *   import unreal, json
 *   resp = unreal.UnrealMCPPythonAPI.execute_mcp_command(
 *       "connect_material_expression",
 *       json.dumps({"params": {...}}))
 *   # resp is a JSON string with the same shape as the TCP response.
 */
UCLASS(BlueprintType)
class UNREALMCP_API UUnrealMCPPythonAPI : public UObject
{
    GENERATED_BODY()

public:
    /**
     * Execute an MCP bridge command directly (no TCP, no GameThread queuing).
     * Must be called on the GameThread (editor python satisfies this).
     *
     * @param CommandType   Bridge command type, e.g. "connect_material_expression".
     * @param ParamsJson    JSON object string with the same schema as the TCP
     *                      protocol body ({"type": ..., "params": {...}}).
     * @return              JSON response string, same shape as the TCP response.
     */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP")
    static FString ExecuteMCPCommand(const FString& CommandType, const FString& ParamsJson);
};
