#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Static (no compile, no bridge) hazard check for UMaterialExpressionCustom HLSL.
 *
 * Pure functions: given the node code (and optionally its Output Type) it returns
 * the same report shape the Python MCP tool used to produce, so the write path
 * (HandleSetMaterialExpressionProperty) and the validate_custom_hlsl command can
 * share one rule set. Rules are the ones validated in this project, in two
 * severities: errors either crash the editor or are guaranteed compile errors,
 * warnings are reported but written through.
 */
class UNREALMCP_API FUnrealMCPMaterialHlslLint
{
public:
    /** Returns {success, ok, error_count, warning_count, errors, warnings}. */
    static TSharedPtr<FJsonObject> Run(const FString& Code, const FString& OutputType);

    /** The code with // and block comments removed, one entry per source line. */
    static TArray<FString> StripHlslComments(const FString& Code);
};
