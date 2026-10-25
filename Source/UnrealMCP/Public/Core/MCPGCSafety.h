#pragma once

#include "CoreMinimal.h"

namespace UnrealMCP
{
	/**
	 * Scope flag for "a python script is executing right now".
	 *
	 * Forcing a collection while python runs hits UObjectBaseUtility::MarkAsGarbage()'s
	 * check(!IsRooted()) and kills the editor outright (2026-10-12: wipe_material_graph called
	 * over the loopback; assert + AV, no structured error). Every GC site in the plugin therefore
	 * goes through CollectGarbageUnlessPythonIsRunning instead of calling CollectGarbage directly.
	 */
	UNREALMCP_API bool IsPythonExecutionActive();

	UNREALMCP_API void SetPythonExecutionActive(bool bActive);

	/** RAII form used by ExecutePythonCode. */
	struct FMCPPythonExecutionScope
	{
		FMCPPythonExecutionScope() { SetPythonExecutionActive(true); }
		~FMCPPythonExecutionScope() { SetPythonExecutionActive(false); }
	};

	/**
	 * Collect garbage, unless a python script is mid-execution - then skip it and log why.
	 *
	 * Skipping is the safe direction: the callers use GC to reclaim an in-memory leftover so a
	 * later create does not report asset_exists. A skipped collection leaves the leftover in place,
	 * so the follow-up reports a name conflict instead of taking the editor down; call the command
	 * from the MCP tool face (no in-flight python) when the reclaim is actually needed.
	 */
	UNREALMCP_API void CollectGarbageUnlessPythonIsRunning(const TCHAR* Reason);
}
