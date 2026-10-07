#pragma once

#include "CoreMinimal.h"
#include "Json.h"
#include "Core/MCPCommandRegistry.h"

/**
 * Live Coding (hot reload) commands.
 *
 * Why this domain exists: a C++ edit had no tool surface at all. The only route was a console
 * command plus a log grep, and the two failure modes looked identical from the outside -
 * "the patch did not apply" and "the channel never answered".
 *
 * The two commands split along the same line PIE uses:
 *   - live_coding_compile: `wait=true` (default) blocks until the patch finishes and returns its
 *     outcome, which is what the engine's own Ctrl+Alt+F11 does (the same compile loop, on the
 *     same thread). `wait=false` only starts it and returns immediately, so a caller that cannot
 *     afford a long call can poll.
 *   - live_coding_status: the poll target for `wait=false`. It also finalises an outstanding
 *     request: when the compile has stopped, the outcome is read from the captured LogLiveCoding
 *     output, because the engine only exposes the result of a synchronous Compile() call.
 *
 * Windows only: ILiveCodingModule lives in a Windows-only engine module.
 */
class UNREALMCP_API FUnrealMCPLiveCodingCommands
{
public:
    FUnrealMCPLiveCodingCommands();
    ~FUnrealMCPLiveCodingCommands();

    /** Declare this domain's commands (name, category, params, flags) in the process-wide table. */
    void RegisterCommands(FMCPCommandRegistry& Registry);

private:
    TSharedPtr<FJsonObject> HandleLiveCodingCompile(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleLiveCodingStatus(const TSharedPtr<FJsonObject>& Params);

    /** True between a request that did not wait and the status call that sees it stop. */
    bool bRequestPending = false;
    double RequestStartTime = 0.0;

    /** Outcome of the last finished request: success / no_changes / failure / cancelled / unknown. */
    FString LastOutcome = TEXT("none");
};
