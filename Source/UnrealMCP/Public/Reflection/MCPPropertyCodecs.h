#pragma once

#include "CoreMinimal.h"
#include "Json.h"
#include "Reflection/MCPPropertyReflector.h"

class FProperty;
class UScriptStruct;

/** Compact-form codec for one struct type. Both directions are required, so one cannot exist alone. */
struct UNREALMCP_API FStructCodec
{
    /** Struct value -> JSON, in this codec's canonical shape. */
    TFunction<TSharedPtr<FJsonValue>(const void* ValuePtr)> ToJson;

    /** JSON -> struct value at ValuePtr. Must leave ValuePtr untouched when it returns false. */
    TFunction<bool(const TSharedPtr<FJsonValue>& Value, void* ValuePtr, FWriteResult& OutResult)> FromJson;

    /** Canonical read shape(s) this codec produces, reported by Describe / reflect_probe. */
    TArray<FString> Shapes;
};

/**
 * Codec for properties the generic reflector cannot express, matched by predicate so the owner (material,
 * particle, ...) does not have to keep an if-branch inside its command handler.
 * Matches wins over struct codecs and over family dispatch.
 */
struct UNREALMCP_API FPropertyCodec
{
    TFunction<bool(const FProperty* Property)> Matches;

    TFunction<TSharedPtr<FJsonValue>(const FProperty* Property, const void* ValuePtr)> ToJson;

    TFunction<bool(const FProperty* Property, void* ValuePtr, const TSharedPtr<FJsonValue>& Value,
                   FWriteResult& OutResult)> FromJson;

    /** Reported with a rejected write, pointing at the tool or spelling the caller should use instead. */
    FString Hint;

    /** Canonical read shape(s) this codec produces, reported by Describe / reflect_probe. */
    TArray<FString> Shapes;

    /**
     * False marks a policy rejection: the codec still reports the shape and the hint, but the property is
     * declared unwritable, so a probe reports it as unsupported instead of pretending it can write.
     */
    bool bWritable = true;
};

/**
 * Registry of struct and property codecs.
 *
 * Registration is append-only and duplicate struct registration is rejected loudly: a silent overwrite
 * would make the effective shape depend on registration order. The backing maps are function-local
 * statics, so a registrar running during static initialization cannot touch an unconstructed map.
 */
namespace FMCPPropertyCodecs
{
    /** Register the compact form of one struct type. Duplicate registration is refused and logged. */
    UNREALMCP_API void RegisterStructCodec(UScriptStruct* StructType, FStructCodec Codec);

    /** Register a property codec (escape hatch for types the generic reflector cannot express). */
    UNREALMCP_API void RegisterPropertyCodec(FPropertyCodec Codec);

    /** The codec owning this property, or nullptr. */
    UNREALMCP_API const FPropertyCodec* FindPropertyCodec(const FProperty* Property);

    /** The codec for this struct type, or nullptr (caller falls back to field recursion). */
    UNREALMCP_API const FStructCodec* FindStructCodec(const UScriptStruct* StructType);

    /** Register the nine built-in struct codecs. Idempotent; also run lazily on first lookup. */
    UNREALMCP_API void RegisterBuiltinStructCodecs();
}
