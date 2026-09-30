#include "Reflection/MCPObjectPathResolver.h"

#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Commands/Blueprint/UnrealMCPBlueprintGraphOps.h"
#include "UObject/UObjectGlobals.h"

UObject* FMCPObjectPathResolver::ResolveObject(const FString& Path, TArray<FString>& OutTried)
{
    if (Path.IsEmpty())
    {
        return nullptr;
    }
    OutTried.AddUnique(Path);

    // "<outer>:<name>[.<name>...]" - load the outer object, then look each trailing segment up inside
    // the one before it (that is the whole spelling of a nested sub-object path).
    FString OuterPath;
    FString SubPath;
    if (Path.Split(TEXT(":"), &OuterPath, &SubPath, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
    {
        UObject* Outer = ResolveObject(OuterPath, OutTried);
        if (!Outer)
        {
            return nullptr;
        }

        UObject* Current = Outer;
        TArray<FString> Segments;
        SubPath.ParseIntoArray(Segments, TEXT("."), /*InCullEmpty=*/true);
        for (const FString& Segment : Segments)
        {
            Current = StaticFindObject(UObject::StaticClass(), Current, *Segment);
            if (!Current)
            {
                return nullptr;
            }
        }
        return Current;
    }

    if (UObject* Asset = FUnrealMCPCommonUtils::FindAsset(Path))
    {
        return Asset;
    }
    return LoadObject<UObject>(nullptr, *Path);
}

bool FMCPObjectPathResolver::ResolveFromParams(const TSharedPtr<FJsonObject>& Params, const TCHAR* ParamName,
                                               UObject*& OutObject, FString& OutErrorCode, FString& OutErrorMessage,
                                               TArray<FString>& OutTried)
{
    UObject* Object = nullptr;

    FString Target;
    if (Params.IsValid())
    {
        Params->TryGetStringField(FString(ParamName), Target);
    }

    if (Target.IsEmpty())
    {
        OutErrorCode = TEXT("invalid_params");
        OutErrorMessage = FString::Printf(TEXT("Missing '%s' parameter"), ParamName);
        return false;
    }

    Object = ResolveObject(Target, OutTried);

    if (!Object)
    {
        // Bare class names ("StaticMeshComponent") are not assets, so a class is the last resort and
        // its default object is what gets addressed - same rule reflect_probe applies.
        UClass* Class = nullptr;
        TArray<FString> ClassCandidates;
        if (FUnrealMCPBlueprintGraphOps::ResolveClass(Target, Class, ClassCandidates) && Class)
        {
            Object = Class->GetDefaultObject();
        }
    }

    if (UClass* AsClass = Cast<UClass>(Object))
    {
        Object = AsClass->GetDefaultObject();
    }

    if (!Object)
    {
        OutErrorCode = TEXT("load_failed");
        OutErrorMessage = FString::Printf(
            TEXT("Could not resolve %s '%s' as an asset, a class, or a sub-object (tried: %s)"),
            ParamName, *Target, *FString::Join(OutTried, TEXT(", ")));
        return false;
    }

    OutObject = Object;
    return true;
}
