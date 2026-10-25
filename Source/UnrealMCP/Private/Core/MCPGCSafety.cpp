#include "Core/MCPGCSafety.h"

#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealMCPGCSafety, Log, All);

namespace UnrealMCP
{
	namespace
	{
		bool bPythonExecutionActive = false;
	}

	bool IsPythonExecutionActive()
	{
		return bPythonExecutionActive;
	}

	void SetPythonExecutionActive(bool bActive)
	{
		bPythonExecutionActive = bActive;
	}

	void CollectGarbageUnlessPythonIsRunning(const TCHAR* Reason)
	{
		if (bPythonExecutionActive)
		{
			// Do NOT CollectGarbage here: see the header. The caller gets a leftover instead of a
			// crashed editor, and the caller's own error path reports the name conflict.
			UE_LOG(LogUnrealMCPGCSafety, Warning,
				TEXT("Skipped garbage collection (reason: %s) because a python script is executing. "
					 "Call this command from the MCP tool face when the reclaim is required."),
				Reason ? Reason : TEXT("unspecified"));
			return;
		}

		CollectGarbage(RF_NoFlags);
	}
}
