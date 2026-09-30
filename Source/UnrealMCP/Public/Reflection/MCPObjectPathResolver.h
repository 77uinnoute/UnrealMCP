#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Object-path resolution shared by every command that addresses an object by path.
 *
 * A path is an asset path, a class path/name, or a sub-object of either
 * ("/Game/X.X:Sub.SubSub"). Two rules are deliberately part of the contract:
 *   - the outer half of a sub-object path still goes through FindAsset, so "no disk file, no load"
 *     is preserved and a deleted package cannot poison the loader;
 *   - a bare class name resolves to that class's default object, because a class is not an asset.
 *
 * Every command family that writes or reads by path (reflect_probe, set_object_property, the clothing
 * commands) resolves through here, so the spelling of a path cannot drift between them.
 */
class UNREALMCP_API FMCPObjectPathResolver
{
public:
    /**
     * Resolve "<outer>:<name>[.<name>...]": load the outer object and look each trailing segment up
     * inside the one before it. OutTried collects every spelling attempted, for error reporting.
     */
    static UObject* ResolveObject(const FString& Path, TArray<FString>& OutTried);

    /**
     * Read a string path parameter from the command params and resolve it as an asset, a class (its
     * default object) or a sub-object of either. On failure OutErrorCode is "invalid_params" (missing
     * or empty path) or "load_failed", with OutTried listing what was attempted.
     */
    static bool ResolveFromParams(const TSharedPtr<FJsonObject>& Params, const TCHAR* ParamName,
                                  UObject*& OutObject, FString& OutErrorCode, FString& OutErrorMessage,
                                  TArray<FString>& OutTried);
};
