#pragma once

#include "CoreMinimal.h"
#include "Json.h"
#include "Core/MCPCommandRegistry.h"

/**
 * Introspection of the engine's *editor python API*.
 *
 * `list_mcp_commands` describes the bridge's own command surface; the engine plugins' python API
 * (IKRigEditor, IKRetargeter, ...) is invisible to it, so callers end up guessing signatures and
 * paying for it in TypeErrors. These two commands read the live python type instead:
 *
 *   - python_api_index(class_name): every documented member of a python-exposed class
 *   - python_api_doc(class_name, function): one member, with class/function candidates on a miss
 *
 * Both are read-only: no asset, object or graph is touched, and no package is marked dirty.
 * The layering is deliberate - this covers the python API layer, `list_mcp_commands` covers the
 * registered command layer (see the command-registry capability).
 */
class UNREALMCP_API FUnrealMCPPythonAPICommands
{
public:
    FUnrealMCPPythonAPICommands();

    // Register this domain's commands with the process-wide command registry.
    void RegisterCommands(FMCPCommandRegistry& Registry);

private:
    TSharedPtr<FJsonObject> HandlePythonAPIIndex(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandlePythonAPIDoc(const TSharedPtr<FJsonObject>& Params);

    // Runs a python snippet that prints "@@MCPJSON@@{...}" and returns that payload as JSON.
    // Returns false + a structured error envelope when python is unavailable, raised, or printed
    // nothing parseable (the traceback is passed through rather than swallowed).
    static bool RunIntrospectionPython(const FString& PyCode, TSharedPtr<FJsonObject>& OutPayload,
                                       TSharedPtr<FJsonObject>& OutError);

    // True for a bare python identifier: it goes straight into the generated source, so anything
    // else (quotes, newlines, dots) is refused instead of being interpolated.
    static bool IsSafeIdentifier(const FString& Text);
};
