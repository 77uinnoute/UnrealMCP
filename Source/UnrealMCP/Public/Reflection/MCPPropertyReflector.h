#pragma once

#include "CoreMinimal.h"
#include "Json.h"

class FProperty;
class UScriptStruct;
class UObject;

/**
 * Outcome of one property (or struct field) write.
 *
 * The error code is produced here rather than inferred from the message text: callers used to guess it
 * with substring matching on the message, so editing a message silently changed the code.
 * `bUnchanged` states the atomicity promise explicitly instead of leaving the caller to assume it.
 */
struct UNREALMCP_API FWriteResult
{
    bool bSuccess = false;

    FString ErrorCode;
    FString ErrorMessage;

    /** Index of the first unusable container element, INDEX_NONE when the write was not element-wise. */
    int32 FailedIndex = INDEX_NONE;

    /** Writable fields of the struct whose field name could not be resolved. */
    TArray<FString> AvailableFields;

    /** Legal members of the enum whose member name could not be resolved. */
    TArray<FString> Candidates;

    /** Shapes this property accepts, so a rejected write tells the caller what would have worked. */
    TArray<FString> SupportedShapes;

    /** Alternative tool or spelling to use when this property cannot be written this way. */
    FString Hint;

    /** Read-back of the property as it stands when the write was rejected. */
    FString CurrentValueJson;

    /** True when the target property still holds its pre-write value (the failure was atomic). */
    bool bUnchanged = true;

    static FWriteResult Success();
    static FWriteResult Failure(const TCHAR* InErrorCode, const FString& InMessage);

    FWriteResult& WithIndex(int32 InFailedIndex);
    FWriteResult& WithFields(TArray<FString> InAvailableFields);
    FWriteResult& WithCandidates(TArray<FString> InCandidates);
    FWriteResult& WithShapes(TArray<FString> InSupportedShapes);
    FWriteResult& WithHint(const FString& InHint);
    FWriteResult& WithCurrentValue(const FString& InCurrentValueJson);
};

/**
 * What a property can accept, derived from the FProperty itself plus any registered codec.
 * Consumed by `reflect_probe` and by the error path, so neither has to hard-code a type list.
 */
struct UNREALMCP_API FPropertyDescriptor
{
    /** Engine type name, e.g. "TArray<FName>" (same string the tools already report as property_type). */
    FString CppType;

    /** Property class name, e.g. "FArrayProperty" / "FStructProperty". */
    FString PropertyClass;

    /** "" for scalars, otherwise "Array" / "Set" / "Map". */
    FString Container;

    /** Inner property's CPP type for containers. */
    FString ElementType;

    /** Value property's CPP type for maps. */
    FString ValueType;

    /** True when FromJson can write it. */
    bool bSupported = false;

    /** True when a struct or property codec owns it (its shape is codec-defined). */
    bool bHasCodec = false;

    /** "" for non-containers, otherwise "replace" (element count == final element count). */
    FString Semantics;

    /** True when the container's element count is what the caller passes (replace semantics). */
    bool bReplacesValue = true;

    TArray<FString> SupportedShapes;

    FString Hint;
};

/**
 * The single property read/write implementation.
 *
 * Every property command must go through here: handlers must not dispatch on property type themselves
 * and must not compute a property value address themselves (address arithmetic belongs next to the
 * FProperty that owns it, which is why a handler passing a container pointer as the value address
 * silently corrupted unrelated memory).
 *
 * Dispatch is organized by property *family* rather than by concrete property class, so covering a new
 * scalar type needs no code change here.
 */
class UNREALMCP_API FMCPPropertyReflector
{
public:
    /** Property -> JSON. Handles structs and containers recursively. */
    static TSharedPtr<FJsonValue> ToJson(FProperty* Property, const void* ValuePtr);

    /**
     * JSON -> property, written in place at PropertyAddr.
     * Rejected writes leave the property untouched (bUnchanged) and carry the reason in the result.
     */
    static FWriteResult FromJson(FProperty* Property, void* PropertyAddr, const FString& Context,
                                 const TSharedPtr<FJsonValue>& Value);

    /** Type / shape / support description of a property, without touching any value. */
    static FPropertyDescriptor Describe(const FProperty* Property);

    /** True when FromJson can write this property. */
    static bool IsSupported(const FProperty* Property);

    /**
     * Every value shape the reflector understands, as a flat vocabulary. This is what a rejection
     * quotes as "what you may write instead", so no caller keeps its own hand-written type list.
     */
    static TArray<FString> SupportedShapeVocabulary();

    /** Recursive struct writer (field object, or array in declaration order). Shared with struct codecs. */
    static FWriteResult WriteStructFields(UScriptStruct* StructType, void* StructAddr, const FString& Context,
                                          const TSharedPtr<FJsonValue>& Value);

    /**
     * Numbers from either an array value or a struct-shaped object carrying the given field names.
     * Owned by this layer; FUnrealMCPCommonUtils::ReadNumbersFromJson forwards here.
     */
    static bool ReadNumbersFromJson(const TSharedPtr<FJsonValue>& Value, const TArray<FString>& ObjectKeys,
                                    TArray<double>& OutNumbers, FString& OutErrorMessage);
};
