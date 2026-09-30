#include "Reflection/MCPPropertyReflector.h"
#include "Reflection/MCPPropertyCodecs.h"

#include "Compat/UnrealMCPVersionCompat.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"

#include "UObject/UnrealType.h"
#include "UObject/TextProperty.h"

//==============================================================================
// FWriteResult
//==============================================================================

FWriteResult FWriteResult::Success()
{
    FWriteResult Result;
    Result.bSuccess = true;
    Result.bUnchanged = false;
    return Result;
}

FWriteResult FWriteResult::Failure(const TCHAR* InErrorCode, const FString& InMessage)
{
    FWriteResult Result;
    Result.bSuccess = false;
    Result.bUnchanged = true;
    Result.ErrorCode = InErrorCode;
    Result.ErrorMessage = InMessage;
    return Result;
}

FWriteResult& FWriteResult::WithIndex(int32 InFailedIndex)           { FailedIndex = InFailedIndex; return *this; }
FWriteResult& FWriteResult::WithFields(TArray<FString> InFields)     { AvailableFields = MoveTemp(InFields); return *this; }
FWriteResult& FWriteResult::WithCandidates(TArray<FString> InCands)  { Candidates = MoveTemp(InCands); return *this; }
FWriteResult& FWriteResult::WithShapes(TArray<FString> InShapes)     { SupportedShapes = MoveTemp(InShapes); return *this; }
FWriteResult& FWriteResult::WithHint(const FString& InHint)          { Hint = InHint; return *this; }
FWriteResult& FWriteResult::WithCurrentValue(const FString& InValue) { CurrentValueJson = InValue; return *this; }

//==============================================================================
// Shape vocabulary: the single source for both Describe and the error messages.
//==============================================================================

namespace
{
    const TCHAR* const ShapeNumber         = TEXT("number");
    const TCHAR* const ShapeBoolean        = TEXT("boolean");
    const TCHAR* const ShapeString         = TEXT("string");
    const TCHAR* const ShapeNumberArray    = TEXT("[v0, v1, ...]");
    const TCHAR* const ShapeFieldObject    = TEXT("{FieldName: value}");
    const TCHAR* const ShapeAssetPath      = TEXT("asset path string");
    const TCHAR* const ShapeEnumMember     = TEXT("enum member name or number");
    const TCHAR* const ShapeArray          = TEXT("array");
    const TCHAR* const ShapeKeyValueObject = TEXT("{Key: value}");
    const TCHAR* const ShapeKeyValuePairs  = TEXT("[[key, value], ...]");
    const TCHAR* const ShapeStructText     = TEXT("struct text string");

    TArray<FString> MakeShapes(std::initializer_list<const TCHAR*> InShapes)
    {
        TArray<FString> Out;
        Out.Reserve(static_cast<int32>(InShapes.size()));
        for (const TCHAR* Shape : InShapes)
        {
            Out.Add(Shape);
        }
        return Out;
    }
}

//==============================================================================
// Value readers. A JSON number is the normal form, but the MCP tool schemas declare several value
// parameters as strings, so a numeric/bool string is accepted too. Anything else is a real type
// mismatch and must not be silently coerced to 0 / false.
//==============================================================================

namespace
{
    bool ReadNumberValue(const TSharedPtr<FJsonValue>& Value, double& OutNumber)
    {
        if (!Value.IsValid())
        {
            return false;
        }
        if (Value->Type == EJson::Number)
        {
            OutNumber = Value->AsNumber();
            return true;
        }
        if (Value->Type == EJson::String && Value->AsString().IsNumeric())
        {
            OutNumber = FCString::Atod(*Value->AsString());
            return true;
        }
        return false;
    }

    bool ReadBoolValue(const TSharedPtr<FJsonValue>& Value, bool& bOutValue)
    {
        if (!Value.IsValid())
        {
            return false;
        }
        if (Value->Type == EJson::Boolean)
        {
            bOutValue = Value->AsBool();
            return true;
        }
        if (Value->Type == EJson::Number)
        {
            bOutValue = Value->AsNumber() != 0.0;
            return true;
        }
        if (Value->Type == EJson::String)
        {
            const FString Text = Value->AsString();
            if (Text.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Text == TEXT("1"))
            {
                bOutValue = true;
                return true;
            }
            if (Text.Equals(TEXT("false"), ESearchCase::IgnoreCase) || Text == TEXT("0"))
            {
                bOutValue = false;
                return true;
            }
        }
        return false;
    }

    bool ReadTextValue(const TSharedPtr<FJsonValue>& Value, FString& OutText)
    {
        if (!Value.IsValid() || Value->Type == EJson::Array || Value->Type == EJson::Object)
        {
            return false;
        }
        OutText = Value->AsString();
        return true;
    }

    /** Lowercase with underscores removed, so "offset_options" and "OffsetOptions" collapse together. */
    FString NormalizeToken(const FString& In)
    {
        FString Out;
        Out.Reserve(In.Len());
        for (const TCHAR Char : In)
        {
            if (Char != TEXT('_'))
            {
                Out.AppendChar(FChar::ToLower(Char));
            }
        }
        return Out;
    }

    /** Namespaced so it cannot collide with the engine's own global EscapeJsonString (JsonWriter.h:57). */
    FString MCPEscapeJsonString(const FString& In)
    {
        FString Out;
        Out.Reserve(In.Len() + 8);
        for (const TCHAR Char : In)
        {
            switch (Char)
            {
            case TEXT('\\'): Out += TEXT("\\\\"); break;
            case TEXT('"'):  Out += TEXT("\\\""); break;
            case TEXT('\n'): Out += TEXT("\\n"); break;
            case TEXT('\r'): Out += TEXT("\\r"); break;
            case TEXT('\t'): Out += TEXT("\\t"); break;
            default: Out.AppendChar(Char); break;
            }
        }
        return Out;
    }

    /**
     * Compact strict-JSON text for any value shape. Written by hand rather than through
     * FJsonSerializer because the 5.5 overload set needs an identifier/raw-writer combination that
     * would wrap the payload in a synthetic key.
     */
    FString JsonValueToJsonText(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid())
        {
            return TEXT("null");
        }
        switch (Value->Type)
        {
        case EJson::String:
            return FString::Printf(TEXT("\"%s\""), *MCPEscapeJsonString(Value->AsString()));
        case EJson::Number:
        {
            const double Number = Value->AsNumber();
            if (Number == FMath::TruncToDouble(Number))
            {
                return FString::Printf(TEXT("%lld"), static_cast<int64>(Number));
            }
            return FString::Printf(TEXT("%g"), Number);
        }
        case EJson::Boolean:
            return Value->AsBool() ? TEXT("true") : TEXT("false");
        case EJson::Array:
        {
            const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
            if (!Value->TryGetArray(Items) || !Items)
            {
                return TEXT("[]");
            }
            TArray<FString> Parts;
            Parts.Reserve(Items->Num());
            for (const TSharedPtr<FJsonValue>& Item : *Items)
            {
                Parts.Add(JsonValueToJsonText(Item));
            }
            return FString::Printf(TEXT("[%s]"), *FString::Join(Parts, TEXT(",")));
        }
        case EJson::Object:
        {
            const TSharedPtr<FJsonObject>* Object = nullptr;
            if (!Value->TryGetObject(Object) || !Object || !(*Object).IsValid())
            {
                return TEXT("{}");
            }
            TArray<FString> Parts;
            Parts.Reserve((*Object)->Values.Num());
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Object)->Values)
            {
                Parts.Add(FString::Printf(TEXT("\"%s\":%s"),
                    *MCPEscapeJsonString(Pair.Key), *JsonValueToJsonText(Pair.Value)));
            }
            return FString::Printf(TEXT("{%s}"), *FString::Join(Parts, TEXT(",")));
        }
        default:
            return TEXT("null");
        }
    }

    /** JSON value rendered as a plain string: used for map keys and for error read-backs. */
    FString JsonValueToString(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid())
        {
            return TEXT("null");
        }
        switch (Value->Type)
        {
        case EJson::String:
            return Value->AsString();
        case EJson::Number:
        {
            const double Number = Value->AsNumber();
            if (Number == FMath::TruncToDouble(Number))
            {
                return FString::Printf(TEXT("%lld"), static_cast<int64>(Number));
            }
            return FString::Printf(TEXT("%g"), Number);
        }
        case EJson::Boolean:
            return Value->AsBool() ? TEXT("true") : TEXT("false");
        default:
            return JsonValueToJsonText(Value);
        }
    }
}

//==============================================================================
// Enum member resolution: number / numeric string / "Prefix::Member" / bare member name, with a
// case- and underscore-insensitive fallback and the legal members reported when nothing matches.
//==============================================================================

namespace
{
    TArray<FString> CollectEnumCandidates(const UEnum* EnumDef)
    {
        TArray<FString> Names;
        if (!EnumDef)
        {
            return Names;
        }
        const int32 Num = EnumDef->NumEnums();
        for (int32 Index = 0; Index < Num; ++Index)
        {
            const FString Name = EnumDef->GetAuthoredNameStringByIndex(Index);
            if (!Name.EndsWith(TEXT("_MAX")))
            {
                Names.Add(Name);
            }
        }
        return Names;
    }

    bool ResolveEnumValue(const UEnum* EnumDef, const TSharedPtr<FJsonValue>& Value, const FString& Context,
                          int64& OutEnumValue, FWriteResult& OutResult)
    {
        if (!EnumDef)
        {
            OutResult = FWriteResult::Failure(TEXT("unsupported_property_type"),
                FString::Printf(TEXT("%s has no resolvable enum type"), *Context));
            return false;
        }

        if (Value.IsValid() && Value->Type == EJson::Number)
        {
            OutEnumValue = static_cast<int64>(Value->AsNumber());
            return true;
        }

        FString Requested;
        if (!ReadTextValue(Value, Requested) || Requested.IsEmpty())
        {
            OutResult = FWriteResult::Failure(TEXT("type_mismatch"),
                FString::Printf(TEXT("%s requires an enum member name or number"), *Context))
                .WithShapes(MakeShapes({ ShapeEnumMember }))
                .WithCandidates(CollectEnumCandidates(EnumDef));
            return false;
        }

        if (Requested.IsNumeric())
        {
            OutEnumValue = FCString::Atoi64(*Requested);
            return true;
        }

        const FString FullRequested = Requested;
        FString ShortRequested = Requested;
        if (ShortRequested.Contains(TEXT("::")))
        {
            ShortRequested.Split(TEXT("::"), nullptr, &ShortRequested);
        }

        int64 EnumValue = EnumDef->GetValueByNameString(ShortRequested);
        if (EnumValue == INDEX_NONE && ShortRequested != FullRequested)
        {
            EnumValue = EnumDef->GetValueByNameString(FullRequested);
        }

        // Case/underscore-insensitive fallback, over both the qualified and the authored member name.
        if (EnumValue == INDEX_NONE)
        {
            const FString Wanted = NormalizeToken(ShortRequested);
            const int32 Num = EnumDef->NumEnums();
            for (int32 Index = 0; Index < Num; ++Index)
            {
                const FString MemberName = EnumDef->GetNameStringByIndex(Index);
                if (MemberName.EndsWith(TEXT("_MAX")))
                {
                    continue;
                }
                if (NormalizeToken(MemberName) == Wanted ||
                    NormalizeToken(EnumDef->GetAuthoredNameStringByIndex(Index)) == Wanted)
                {
                    EnumValue = EnumDef->GetValueByIndex(Index);
                    break;
                }
            }
        }

        if (EnumValue == INDEX_NONE)
        {
            const TArray<FString> Candidates = CollectEnumCandidates(EnumDef);
            OutResult = FWriteResult::Failure(TEXT("unknown_enum_member"),
                FString::Printf(TEXT("'%s' is not a member of %s. Available members: %s"),
                    *Requested, *EnumDef->GetName(), *FString::Join(Candidates, TEXT(", "))))
                .WithShapes(MakeShapes({ ShapeEnumMember }))
                .WithCandidates(Candidates);
            return false;
        }

        OutEnumValue = EnumValue;
        return true;
    }
}

//==============================================================================
// Struct field helpers. Field lookup mirrors the top-level rule: exact C++ name first, then a
// case/underscore insensitive match, so python-style snake_case names resolve to the reflected field.
//==============================================================================

namespace
{
    bool IsWritableStructField(const FProperty* Field)
    {
        return Field
            && Field->HasAnyPropertyFlags(CPF_Edit)
            && !Field->HasAnyPropertyFlags(CPF_Transient | CPF_EditConst);
    }

    void CollectStructFieldNames(const UScriptStruct* StructType, TArray<FString>& OutNames)
    {
        OutNames.Reset();
        if (!StructType)
        {
            return;
        }
        for (TFieldIterator<FProperty> FieldIt(StructType); FieldIt; ++FieldIt)
        {
            if (IsWritableStructField(*FieldIt))
            {
                OutNames.Add(FieldIt->GetName());
            }
        }
    }

    FProperty* ResolveStructField(const UScriptStruct* StructType, const FString& FieldName)
    {
        if (!StructType)
        {
            return nullptr;
        }
        for (TFieldIterator<FProperty> FieldIt(StructType); FieldIt; ++FieldIt)
        {
            if (IsWritableStructField(*FieldIt) && FieldIt->GetName().Equals(FieldName, ESearchCase::CaseSensitive))
            {
                return *FieldIt;
            }
        }

        const FString Normalized = NormalizeToken(FieldName);
        for (TFieldIterator<FProperty> FieldIt(StructType); FieldIt; ++FieldIt)
        {
            if (IsWritableStructField(*FieldIt) && NormalizeToken(FieldIt->GetName()) == Normalized)
            {
                return *FieldIt;
            }
        }
        return nullptr;
    }
}

//==============================================================================
// Codec registry
//==============================================================================

namespace FMCPPropertyCodecs
{
    static TMap<UScriptStruct*, FStructCodec>& GetStructCodecs()
    {
        // Function-local static: a registrar running during static initialization constructs the map on
        // first touch instead of racing module startup.
        static TMap<UScriptStruct*, FStructCodec> Codecs;
        static bool bBuiltinsRegistered = false;
        if (!bBuiltinsRegistered)
        {
            // Set first: the built-in registrars re-enter this function.
            bBuiltinsRegistered = true;
            RegisterBuiltinStructCodecs();
        }
        return Codecs;
    }

    static TArray<FPropertyCodec>& GetPropertyCodecs()
    {
        static TArray<FPropertyCodec> Codecs;
        return Codecs;
    }

    void RegisterStructCodec(UScriptStruct* StructType, FStructCodec Codec)
    {
        if (!StructType)
        {
            UE_LOG(LogTemp, Error, TEXT("RegisterStructCodec: null struct type"));
            return;
        }
        if (!Codec.ToJson || !Codec.FromJson)
        {
            UE_LOG(LogTemp, Error, TEXT("RegisterStructCodec: %s must supply both ToJson and FromJson"),
                *StructType->GetName());
            return;
        }

        TMap<UScriptStruct*, FStructCodec>& Codecs = GetStructCodecs();
        if (Codecs.Contains(StructType))
        {
            // Refused rather than overwritten: a silent overwrite would make the effective shape depend on
            // registration order.
            UE_LOG(LogTemp, Error,
                TEXT("RegisterStructCodec: %s already has a codec; the second registration was refused"),
                *StructType->GetName());
            return;
        }
        Codecs.Add(StructType, MoveTemp(Codec));
    }

    void RegisterPropertyCodec(FPropertyCodec Codec)
    {
        if (!Codec.Matches || !Codec.ToJson || !Codec.FromJson)
        {
            UE_LOG(LogTemp, Error, TEXT("RegisterPropertyCodec: Matches, ToJson and FromJson are all required"));
            return;
        }
        GetPropertyCodecs().Add(MoveTemp(Codec));
    }

    const FPropertyCodec* FindPropertyCodec(const FProperty* Property)
    {
        if (!Property)
        {
            return nullptr;
        }
        for (const FPropertyCodec& Codec : GetPropertyCodecs())
        {
            if (Codec.Matches(Property))
            {
                return &Codec;
            }
        }
        return nullptr;
    }

    const FStructCodec* FindStructCodec(const UScriptStruct* StructType)
    {
        if (!StructType)
        {
            return nullptr;
        }
        return GetStructCodecs().Find(StructType);
    }
}

//==============================================================================
// Built-in struct codecs. Reading always uses one canonical shape (a number array; a string for FGuid;
// an object of component arrays for FTransform); writing accepts that shape plus a named-field object,
// so whatever the read path emits can be written straight back.
//==============================================================================

namespace
{
    /** Number-array codec: reads as n numbers, writes from n numbers or a named-field object. */
    FStructCodec MakeNumberArrayCodec(std::initializer_list<const TCHAR*> FieldKeys, int32 ExpectedCount,
                                      TFunction<void(const TArray<double>&, void*)> Writer)
    {
        TArray<FString> Keys;
        Keys.Reserve(static_cast<int32>(FieldKeys.size()));
        for (const TCHAR* Key : FieldKeys)
        {
            Keys.Add(Key);
        }

        FStructCodec Codec;
        Codec.Shapes = MakeShapes({ ShapeNumberArray, ShapeFieldObject });
        Codec.FromJson = [Keys, ExpectedCount, Writer](const TSharedPtr<FJsonValue>& Value, void* ValuePtr,
                                                       FWriteResult& OutResult) -> bool
        {
            TArray<double> Numbers;
            FString ReadError;
            if (!FMCPPropertyReflector::ReadNumbersFromJson(Value, Keys, Numbers, ReadError) ||
                Numbers.Num() != ExpectedCount)
            {
                if (ReadError.IsEmpty())
                {
                    ReadError = FString::Printf(TEXT("expected %d numbers"), ExpectedCount);
                }
                OutResult = FWriteResult::Failure(TEXT("type_mismatch"), ReadError)
                    .WithShapes(MakeShapes({ ShapeNumberArray, ShapeFieldObject }));
                return false;
            }
            Writer(Numbers, ValuePtr);
            return true;
        };
        return Codec;
    }

    TSharedPtr<FJsonValue> NumbersToJson(std::initializer_list<double> Numbers)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Reserve(static_cast<int32>(Numbers.size()));
        for (const double Number : Numbers)
        {
            Arr.Add(MakeShared<FJsonValueNumber>(Number));
        }
        return MakeShared<FJsonValueArray>(Arr);
    }

    /** Named-number lookup that tolerates case/underscore spelling; leaves OutNumber untouched if absent. */
    bool ReadOptionalNumberField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, double& OutNumber)
    {
        if (!Object.IsValid())
        {
            return false;
        }
        if (Object->TryGetNumberField(Key, OutNumber))
        {
            return true;
        }
        const FString Wanted = NormalizeToken(Key);
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
        {
            if (NormalizeToken(Pair.Key) == Wanted && Pair.Value.IsValid() &&
                Pair.Value->Type == EJson::Number)
            {
                OutNumber = Pair.Value->AsNumber();
                return true;
            }
        }
        return false;
    }

    /**
     * Colour codec. Reads [r, g, b, a]. Writes from 3 or 4 numbers, or from a named object whose alpha is
     * optional - the material tools used to imply alpha, and that has to keep working.
     */
    FStructCodec MakeColorCodec(double ImpliedAlpha, TFunction<void(const TArray<double>&, void*)> Writer)
    {
        FStructCodec Codec;
        Codec.Shapes = MakeShapes({ ShapeNumberArray, ShapeFieldObject });
        Codec.FromJson = [ImpliedAlpha, Writer](const TSharedPtr<FJsonValue>& Value, void* ValuePtr,
                                                FWriteResult& OutResult) -> bool
        {
            const TArray<FString> ShapeList = MakeShapes({ ShapeNumberArray, ShapeFieldObject });
            TArray<double> Numbers;

            if (Value.IsValid() && Value->Type == EJson::Array)
            {
                FString ReadError;
                if (!FMCPPropertyReflector::ReadNumbersFromJson(Value, TArray<FString>(), Numbers, ReadError))
                {
                    OutResult = FWriteResult::Failure(TEXT("type_mismatch"), ReadError).WithShapes(ShapeList);
                    return false;
                }
            }
            else
            {
                const TSharedPtr<FJsonObject>* Object = nullptr;
                if (!Value.IsValid() || Value->Type != EJson::Object || !Value->TryGetObject(Object) ||
                    !Object || !(*Object).IsValid())
                {
                    OutResult = FWriteResult::Failure(TEXT("type_mismatch"),
                        TEXT("a colour requires [r, g, b, (a)] or {r, g, b, (a)}")).WithShapes(ShapeList);
                    return false;
                }

                double Red = 0.0;
                double Green = 0.0;
                double Blue = 0.0;
                double Alpha = ImpliedAlpha;
                if (!ReadOptionalNumberField(*Object, TEXT("R"), Red) ||
                    !ReadOptionalNumberField(*Object, TEXT("G"), Green) ||
                    !ReadOptionalNumberField(*Object, TEXT("B"), Blue))
                {
                    OutResult = FWriteResult::Failure(TEXT("type_mismatch"),
                        TEXT("a colour object requires r, g and b numbers")).WithShapes(ShapeList);
                    return false;
                }
                ReadOptionalNumberField(*Object, TEXT("A"), Alpha);
                Numbers.Add(Red);
                Numbers.Add(Green);
                Numbers.Add(Blue);
                Numbers.Add(Alpha);
            }

            if (Numbers.Num() == 3)
            {
                Numbers.Add(ImpliedAlpha);
            }
            if (Numbers.Num() != 4)
            {
                OutResult = FWriteResult::Failure(TEXT("type_mismatch"),
                    FString::Printf(TEXT("a colour requires 3 or 4 numbers, got %d"), Numbers.Num()))
                    .WithShapes(ShapeList);
                return false;
            }
            Writer(Numbers, ValuePtr);
            return true;
        };
        return Codec;
    }
}

void FMCPPropertyCodecs::RegisterBuiltinStructCodecs()
{
    {
        FStructCodec Codec = MakeNumberArrayCodec({ TEXT("X"), TEXT("Y") }, 2,
            [](const TArray<double>& N, void* Ptr) { *static_cast<FVector2D*>(Ptr) = FVector2D(N[0], N[1]); });
        Codec.ToJson = [](const void* ValuePtr)
        {
            const FVector2D& Vec = *static_cast<const FVector2D*>(ValuePtr);
            return NumbersToJson({ Vec.X, Vec.Y });
        };
        RegisterStructCodec(TBaseStructure<FVector2D>::Get(), MoveTemp(Codec));
    }
    {
        FStructCodec Codec = MakeNumberArrayCodec({ TEXT("X"), TEXT("Y"), TEXT("Z") }, 3,
            [](const TArray<double>& N, void* Ptr) { *static_cast<FVector*>(Ptr) = FVector(N[0], N[1], N[2]); });
        Codec.ToJson = [](const void* ValuePtr)
        {
            const FVector& Vec = *static_cast<const FVector*>(ValuePtr);
            return NumbersToJson({ Vec.X, Vec.Y, Vec.Z });
        };
        RegisterStructCodec(TBaseStructure<FVector>::Get(), MoveTemp(Codec));
    }
    {
        FStructCodec Codec = MakeNumberArrayCodec({ TEXT("Pitch"), TEXT("Yaw"), TEXT("Roll") }, 3,
            [](const TArray<double>& N, void* Ptr) { *static_cast<FRotator*>(Ptr) = FRotator(N[0], N[1], N[2]); });
        Codec.ToJson = [](const void* ValuePtr)
        {
            const FRotator& Rot = *static_cast<const FRotator*>(ValuePtr);
            return NumbersToJson({ Rot.Pitch, Rot.Yaw, Rot.Roll });
        };
        RegisterStructCodec(TBaseStructure<FRotator>::Get(), MoveTemp(Codec));
    }
    {
        FStructCodec Codec = MakeNumberArrayCodec({ TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W") }, 4,
            [](const TArray<double>& N, void* Ptr) { *static_cast<FQuat*>(Ptr) = FQuat(N[0], N[1], N[2], N[3]); });
        Codec.ToJson = [](const void* ValuePtr)
        {
            const FQuat& Quat = *static_cast<const FQuat*>(ValuePtr);
            return NumbersToJson({ Quat.X, Quat.Y, Quat.Z, Quat.W });
        };
        RegisterStructCodec(TBaseStructure<FQuat>::Get(), MoveTemp(Codec));
    }
    {
        // FTransform reads as {Rotation, Translation, Scale3D}; the flattened 10-number array is accepted
        // on write but never emitted, so there is exactly one read shape.
        FStructCodec Codec;
        Codec.Shapes = MakeShapes({ ShapeFieldObject, ShapeNumberArray });
        Codec.ToJson = [](const void* ValuePtr)
        {
            const FTransform& Transform = *static_cast<const FTransform*>(ValuePtr);
            const FQuat Rotation = Transform.GetRotation();
            const FVector Translation = Transform.GetTranslation();
            const FVector Scale = Transform.GetScale3D();

            TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
            Object->SetField(TEXT("Rotation"), NumbersToJson({ Rotation.X, Rotation.Y, Rotation.Z, Rotation.W }));
            Object->SetField(TEXT("Translation"), NumbersToJson({ Translation.X, Translation.Y, Translation.Z }));
            Object->SetField(TEXT("Scale3D"), NumbersToJson({ Scale.X, Scale.Y, Scale.Z }));
            return MakeShared<FJsonValueObject>(Object);
        };
        Codec.FromJson = [](const TSharedPtr<FJsonValue>& Value, void* ValuePtr, FWriteResult& OutResult) -> bool
        {
            const TArray<FString> ShapeList = MakeShapes({ ShapeFieldObject, ShapeNumberArray });
            TArray<FString> NumberKeys;
            NumberKeys.Add(TEXT("X"));
            NumberKeys.Add(TEXT("Y"));
            NumberKeys.Add(TEXT("Z"));
            NumberKeys.Add(TEXT("W"));
            const TArray<FString> VectorKeys = { TEXT("X"), TEXT("Y"), TEXT("Z") };

            // Convenience: the flattened quat + translation + scale form (10 numbers).
            if (Value.IsValid() && Value->Type == EJson::Array)
            {
                TArray<double> Numbers;
                FString ReadError;
                if (!FMCPPropertyReflector::ReadNumbersFromJson(Value, NumberKeys, Numbers, ReadError) ||
                    Numbers.Num() != 10)
                {
                    OutResult = FWriteResult::Failure(TEXT("type_mismatch"),
                        TEXT("FTransform requires {Rotation, Translation, Scale3D} or 10 numbers"))
                        .WithShapes(ShapeList);
                    return false;
                }
                FTransform& Transform = *static_cast<FTransform*>(ValuePtr);
                Transform.SetComponents(FQuat(Numbers[0], Numbers[1], Numbers[2], Numbers[3]),
                                        FVector(Numbers[4], Numbers[5], Numbers[6]),
                                        FVector(Numbers[7], Numbers[8], Numbers[9]));
                return true;
            }

            const TSharedPtr<FJsonObject>* Object = nullptr;
            if (!Value.IsValid() || Value->Type != EJson::Object || !Value->TryGetObject(Object) ||
                !Object || !(*Object).IsValid())
            {
                OutResult = FWriteResult::Failure(TEXT("type_mismatch"),
                    TEXT("FTransform requires {Rotation, Translation, Scale3D} or 10 numbers"))
                    .WithShapes(ShapeList);
                return false;
            }

            FTransform& Transform = *static_cast<FTransform*>(ValuePtr);
            FQuat Rotation = Transform.GetRotation();
            FVector Translation = Transform.GetTranslation();
            FVector Scale = Transform.GetScale3D();
            bool bOk = true;

            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Object)->Values)
            {
                const FString Token = NormalizeToken(Pair.Key);
                TArray<double> Numbers;
                FString ReadError;

                if (Token == NormalizeToken(TEXT("Rotation")))
                {
                    if (!FMCPPropertyReflector::ReadNumbersFromJson(Pair.Value, NumberKeys, Numbers, ReadError) ||
                        Numbers.Num() != 4)
                    {
                        bOk = false;
                    }
                    else
                    {
                        Rotation = FQuat(Numbers[0], Numbers[1], Numbers[2], Numbers[3]);
                    }
                }
                else if (Token == NormalizeToken(TEXT("Translation")))
                {
                    if (!FMCPPropertyReflector::ReadNumbersFromJson(Pair.Value, VectorKeys, Numbers, ReadError) ||
                        Numbers.Num() != 3)
                    {
                        bOk = false;
                    }
                    else
                    {
                        Translation = FVector(Numbers[0], Numbers[1], Numbers[2]);
                    }
                }
                else if (Token == NormalizeToken(TEXT("Scale3D")) || Token == NormalizeToken(TEXT("Scale")))
                {
                    if (!FMCPPropertyReflector::ReadNumbersFromJson(Pair.Value, VectorKeys, Numbers, ReadError) ||
                        Numbers.Num() != 3)
                    {
                        bOk = false;
                    }
                    else
                    {
                        Scale = FVector(Numbers[0], Numbers[1], Numbers[2]);
                    }
                }
                else
                {
                    bOk = false;
                }

                if (!bOk)
                {
                    break;
                }
            }

            if (!bOk)
            {
                OutResult = FWriteResult::Failure(TEXT("type_mismatch"),
                    TEXT("FTransform components must be Rotation [x,y,z,w], Translation [x,y,z], Scale3D [x,y,z]"))
                    .WithShapes(ShapeList);
                return false;
            }

            Transform.SetComponents(Rotation, Translation, Scale);
            return true;
        };
        RegisterStructCodec(TBaseStructure<FTransform>::Get(), MoveTemp(Codec));
    }
    {
        FStructCodec Codec = MakeColorCodec(1.0, [](const TArray<double>& N, void* Ptr)
            {
                *static_cast<FLinearColor*>(Ptr) = FLinearColor(N[0], N[1], N[2], N[3]);
            });
        Codec.ToJson = [](const void* ValuePtr)
        {
            const FLinearColor& Color = *static_cast<const FLinearColor*>(ValuePtr);
            return NumbersToJson({ Color.R, Color.G, Color.B, Color.A });
        };
        RegisterStructCodec(TBaseStructure<FLinearColor>::Get(), MoveTemp(Codec));
    }
    {
        FStructCodec Codec = MakeColorCodec(255.0, [](const TArray<double>& N, void* Ptr)
            {
                *static_cast<FColor*>(Ptr) = FColor(static_cast<uint8>(N[0]), static_cast<uint8>(N[1]),
                                                   static_cast<uint8>(N[2]), static_cast<uint8>(N[3]));
            });
        Codec.ToJson = [](const void* ValuePtr)
        {
            const FColor& Color = *static_cast<const FColor*>(ValuePtr);
            return NumbersToJson({ static_cast<double>(Color.R), static_cast<double>(Color.G),
                                   static_cast<double>(Color.B), static_cast<double>(Color.A) });
        };
        RegisterStructCodec(TBaseStructure<FColor>::Get(), MoveTemp(Codec));
    }
    {
        FStructCodec Codec;
        Codec.Shapes = MakeShapes({ ShapeString });
        Codec.ToJson = [](const void* ValuePtr)
        {
            return MakeShared<FJsonValueString>(static_cast<const FGuid*>(ValuePtr)->ToString());
        };
        Codec.FromJson = [](const TSharedPtr<FJsonValue>& Value, void* ValuePtr, FWriteResult& OutResult) -> bool
        {
            FString Text;
            if (!ReadTextValue(Value, Text) || !FGuid::Parse(Text, *static_cast<FGuid*>(ValuePtr)))
            {
                OutResult = FWriteResult::Failure(TEXT("type_mismatch"), TEXT("FGuid requires a GUID string"))
                    .WithShapes(MakeShapes({ ShapeString }));
                return false;
            }
            return true;
        };
        RegisterStructCodec(TBaseStructure<FGuid>::Get(), MoveTemp(Codec));
    }
    {
        FStructCodec Codec = MakeNumberArrayCodec({ TEXT("X"), TEXT("Y") }, 2,
            [](const TArray<double>& N, void* Ptr)
            {
                *static_cast<FIntPoint*>(Ptr) = FIntPoint(static_cast<int32>(N[0]), static_cast<int32>(N[1]));
            });
        Codec.ToJson = [](const void* ValuePtr)
        {
            const FIntPoint& Point = *static_cast<const FIntPoint*>(ValuePtr);
            return NumbersToJson({ static_cast<double>(Point.X), static_cast<double>(Point.Y) });
        };
        RegisterStructCodec(TBaseStructure<FIntPoint>::Get(), MoveTemp(Codec));
    }
}

//==============================================================================
// Container handle: the only place that touches the FScript*Helper classes or raw element memory, so
// command handlers never deal with a void* address.
//==============================================================================

namespace
{
    class FContainerHandle
    {
    public:
        FContainerHandle(FProperty* InProperty, void* InAddr)
        {
            if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(InProperty))
            {
                Kind = EKind::Array;
                ArrayHelper = MakeUnique<FScriptArrayHelper>(ArrayProperty, InAddr);
                ElementProperty = ArrayProperty->Inner;
            }
            else if (FSetProperty* SetProperty = CastField<FSetProperty>(InProperty))
            {
                Kind = EKind::Set;
                SetHelper = MakeUnique<FScriptSetHelper>(SetProperty, InAddr);
                ElementProperty = SetProperty->ElementProp;
            }
            else if (FMapProperty* MapProperty = CastField<FMapProperty>(InProperty))
            {
                Kind = EKind::Map;
                MapHelper = MakeUnique<FScriptMapHelper>(MapProperty, InAddr);
                ElementProperty = MapProperty->KeyProp;
                ValueProperty = MapProperty->ValueProp;
            }
        }

        bool IsValid() const { return Kind != EKind::None && ElementProperty != nullptr; }
        bool IsMap() const { return Kind == EKind::Map; }
        FProperty* GetElementProperty() const { return ElementProperty; }
        FProperty* GetValueProperty() const { return ValueProperty; }

        void EmptyValues() const
        {
            switch (Kind)
            {
            case EKind::Array: ArrayHelper->EmptyValues(); break;
            case EKind::Set:   SetHelper->EmptyElements(); break;
            case EKind::Map:   MapHelper->EmptyValues(); break;
            default: break;
            }
        }

        /** Appends one element (or one key/value pair) by copying from caller-owned scratch memory. */
        void Append(const void* ElementPtr, const void* InValuePtr) const
        {
            switch (Kind)
            {
            case EKind::Array:
            {
                const int32 NewIndex = ArrayHelper->AddValue();
                ElementProperty->CopyCompleteValue(ArrayHelper->GetRawPtr(NewIndex), ElementPtr);
                break;
            }
            case EKind::Set:
                SetHelper->AddElement(ElementPtr);
                break;
            case EKind::Map:
                MapHelper->AddPair(ElementPtr, InValuePtr);
                break;
            default:
                break;
            }
        }

    private:
        enum class EKind { None, Array, Set, Map };

        EKind Kind = EKind::None;
        FProperty* ElementProperty = nullptr;
        FProperty* ValueProperty = nullptr;
        TUniquePtr<FScriptArrayHelper> ArrayHelper;
        TUniquePtr<FScriptSetHelper> SetHelper;
        TUniquePtr<FScriptMapHelper> MapHelper;
    };

    /** Scratch element storage: allocated and initialized here, always destroyed before leaving scope. */
    struct FScratchElement
    {
        FScratchElement(FProperty* InProperty, void* InPtr)
            : Property(InProperty), Ptr(InPtr)
        {
        }

        ~FScratchElement()
        {
            if (Property && Ptr)
            {
                Property->DestroyValue(Ptr);
                FMemory::Free(Ptr);
            }
        }

        FScratchElement(const FScratchElement&) = delete;
        FScratchElement& operator=(const FScratchElement&) = delete;

        FProperty* Property = nullptr;
        void* Ptr = nullptr;
    };

    TUniquePtr<FScratchElement> AllocScratchElement(FProperty* ElementProperty)
    {
        if (!ElementProperty)
        {
            return nullptr;
        }
        void* Memory = FMemory::Malloc(static_cast<SIZE_T>(ElementProperty->GetSize()),
                                       static_cast<uint32>(ElementProperty->GetMinAlignment()));
        ElementProperty->InitializeValue(Memory);
        return MakeUnique<FScratchElement>(ElementProperty, Memory);
    }

    /**
     * The container write contract, shared by arrays, sets and maps: validate every element on scratch
     * memory first, then replace the whole container. A rejected element leaves the container untouched.
     */
    FWriteResult WriteContainer(FProperty* Property, void* PropertyAddr, const FString& Context,
                                const TSharedPtr<FJsonValue>& Value)
    {
        FContainerHandle Handle(Property, PropertyAddr);
        if (!Handle.IsValid())
        {
            return FWriteResult::Failure(TEXT("unsupported_property_type"),
                FString::Printf(TEXT("%s has no resolvable container element type"), *Context));
        }

        FProperty* const ElementProperty = Handle.GetElementProperty();
        FProperty* const ValueProperty = Handle.GetValueProperty();

        // Build the (key, value) pair list, accepting both spellings of a map.
        TArray<TPair<TSharedPtr<FJsonValue>, TSharedPtr<FJsonValue>>> PairList;

        if (Handle.IsMap())
        {
            if (Value.IsValid() && Value->Type == EJson::Object)
            {
                const TSharedPtr<FJsonObject>* Object = nullptr;
                if (!Value->TryGetObject(Object) || !Object || !(*Object).IsValid())
                {
                    return FWriteResult::Failure(TEXT("type_mismatch"),
                        FString::Printf(TEXT("Map property %s requires an object value"), *Context));
                }
                for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Object)->Values)
                {
                    PairList.Emplace(MakeShared<FJsonValueString>(Pair.Key), Pair.Value);
                }
            }
            else if (Value.IsValid() && Value->Type == EJson::Array)
            {
                const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
                if (!Value->TryGetArray(Items) || !Items)
                {
                    return FWriteResult::Failure(TEXT("type_mismatch"),
                        FString::Printf(TEXT("Map property %s requires key/value pairs"), *Context));
                }
                for (const TSharedPtr<FJsonValue>& Item : *Items)
                {
                    const TArray<TSharedPtr<FJsonValue>>* PairItems = nullptr;
                    if (!Item.IsValid() || Item->Type != EJson::Array || !Item->TryGetArray(PairItems) ||
                        !PairItems || PairItems->Num() != 2)
                    {
                        return FWriteResult::Failure(TEXT("type_mismatch"),
                            FString::Printf(TEXT("Map property %s requires [[key, value], ...] entries"), *Context))
                            .WithShapes(MakeShapes({ ShapeKeyValueObject, ShapeKeyValuePairs }));
                    }
                    PairList.Emplace((*PairItems)[0], (*PairItems)[1]);
                }
            }
            else
            {
                return FWriteResult::Failure(TEXT("type_mismatch"),
                    FString::Printf(TEXT("Map property %s requires an object or key/value pair array"), *Context))
                    .WithShapes(MakeShapes({ ShapeKeyValueObject, ShapeKeyValuePairs }));
            }
        }
        else
        {
            const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
            if (!Value.IsValid() || Value->Type != EJson::Array || !Value->TryGetArray(Items) || !Items)
            {
                return FWriteResult::Failure(TEXT("type_mismatch"),
                    FString::Printf(TEXT("Container property %s requires an array value"), *Context))
                    .WithShapes(MakeShapes({ ShapeArray }));
            }
            for (const TSharedPtr<FJsonValue>& Item : *Items)
            {
                PairList.Emplace(Item, TSharedPtr<FJsonValue>());
            }
        }

        // Pass 1: validate every element on scratch memory.
        TArray<TUniquePtr<FScratchElement>> ScratchKeys;
        TArray<TUniquePtr<FScratchElement>> ScratchValues;
        ScratchKeys.Reserve(PairList.Num());
        ScratchValues.Reserve(PairList.Num());

        for (int32 Index = 0; Index < PairList.Num(); ++Index)
        {
            const FString ItemContext = FString::Printf(TEXT("%s[%d]"), *Context, Index);

            TUniquePtr<FScratchElement> Key = AllocScratchElement(ElementProperty);
            if (!Key)
            {
                return FWriteResult::Failure(TEXT("unsupported_property_type"),
                    FString::Printf(TEXT("%s: element memory could not be allocated"), *ItemContext));
            }

            FWriteResult ElementResult = FMCPPropertyReflector::FromJson(
                ElementProperty, Key->Ptr, ItemContext, PairList[Index].Key);
            if (!ElementResult.bSuccess)
            {
                return ElementResult.WithIndex(Index);
            }

            if (Handle.IsMap())
            {
                TUniquePtr<FScratchElement> MapValue = AllocScratchElement(ValueProperty);
                if (!MapValue)
                {
                    return FWriteResult::Failure(TEXT("unsupported_property_type"),
                        FString::Printf(TEXT("%s: value memory could not be allocated"), *ItemContext));
                }
                FWriteResult ValueResult = FMCPPropertyReflector::FromJson(
                    ValueProperty, MapValue->Ptr, ItemContext, PairList[Index].Value);
                if (!ValueResult.bSuccess)
                {
                    return ValueResult.WithIndex(Index);
                }
                ScratchValues.Add(MoveTemp(MapValue));
            }

            ScratchKeys.Add(MoveTemp(Key));
        }

        // Pass 2: commit. Element count equals the number of validated elements (replace semantics).
        // A set collapses duplicates, which is the only case where fewer elements can end up stored.
        Handle.EmptyValues();
        for (int32 Index = 0; Index < ScratchKeys.Num(); ++Index)
        {
            Handle.Append(ScratchKeys[Index]->Ptr, Handle.IsMap() ? ScratchValues[Index]->Ptr : nullptr);
        }

        return FWriteResult::Success();
    }
}

//==============================================================================
// ToJson
//==============================================================================

TSharedPtr<FJsonValue> FMCPPropertyReflector::ToJson(FProperty* Property, const void* ValuePtr)
{
    if (!Property || !ValuePtr)
    {
        return MakeShared<FJsonValueNull>();
    }

    if (const FPropertyCodec* Codec = FMCPPropertyCodecs::FindPropertyCodec(Property))
    {
        return Codec->ToJson(Property, ValuePtr);
    }

    // A TEnumAsByte member reads as its numeric value, matching the shape its write path accepts.
    if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
    {
        return MakeShared<FJsonValueNumber>(ByteProperty->GetPropertyValue(ValuePtr));
    }

    if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
    {
        if (const UEnum* EnumDef = EnumProperty->GetEnum())
        {
            const int64 EnumValue = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
            return MakeShared<FJsonValueString>(EnumDef->GetNameStringByValue(EnumValue));
        }
        return MakeShared<FJsonValueNull>();
    }

    if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
    {
        return MakeShared<FJsonValueBoolean>(BoolProperty->GetPropertyValue(ValuePtr));
    }

    if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
    {
        if (NumericProperty->IsInteger())
        {
            // Unsigned integer properties would read back as negative through the signed getter.
            const bool bUnsigned = Property->IsA<FUInt16Property>() || Property->IsA<FUInt32Property>() ||
                                   Property->IsA<FUInt64Property>();
            if (bUnsigned)
            {
                return MakeShared<FJsonValueNumber>(
                    static_cast<double>(NumericProperty->GetUnsignedIntPropertyValue(ValuePtr)));
            }
            return MakeShared<FJsonValueNumber>(
                static_cast<double>(NumericProperty->GetSignedIntPropertyValue(ValuePtr)));
        }
        return MakeShared<FJsonValueNumber>(NumericProperty->GetFloatingPointPropertyValue(ValuePtr));
    }

    if (const FStrProperty* StrProperty = CastField<FStrProperty>(Property))
    {
        return MakeShared<FJsonValueString>(StrProperty->GetPropertyValue(ValuePtr));
    }
    if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
    {
        return MakeShared<FJsonValueString>(NameProperty->GetPropertyValue(ValuePtr).ToString());
    }
    if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
    {
        return MakeShared<FJsonValueString>(TextProperty->GetPropertyValue(ValuePtr).ToString());
    }

    // Class flavors are checked before their object base classes.
    if (const FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
    {
        const UObject* ClassObject = ClassProperty->GetObjectPropertyValue(ValuePtr);
        return MakeShared<FJsonValueString>(ClassObject ? ClassObject->GetPathName() : TEXT("None"));
    }
    if (const FSoftClassProperty* SoftClassProperty = CastField<FSoftClassProperty>(Property))
    {
        const FSoftObjectPtr SoftPtr = SoftClassProperty->GetPropertyValue(ValuePtr);
        return MakeShared<FJsonValueString>(SoftPtr.IsNull() ? TEXT("None") : SoftPtr.ToString());
    }
    if (const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
    {
        const UObject* RefObject = ObjectProperty->GetObjectPropertyValue(ValuePtr);
        return MakeShared<FJsonValueString>(RefObject ? RefObject->GetPathName() : TEXT("None"));
    }
    if (const FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
    {
        const FSoftObjectPtr SoftPtr = SoftObjectProperty->GetPropertyValue(ValuePtr);
        return MakeShared<FJsonValueString>(SoftPtr.IsNull() ? TEXT("None") : SoftPtr.ToString());
    }

    if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
    {
        FScriptSetHelper SetHelper(SetProperty, ValuePtr);
        TArray<TSharedPtr<FJsonValue>> JsonArray;
        const int32 MaxIndex = SetHelper.GetMaxIndex();
        for (int32 Index = 0; Index < MaxIndex; ++Index)
        {
            if (SetHelper.IsValidIndex(Index))
            {
                JsonArray.Add(ToJson(SetProperty->ElementProp, SetHelper.GetElementPtr(Index)));
            }
        }
        return MakeShared<FJsonValueArray>(JsonArray);
    }

    if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
    {
        FScriptMapHelper MapHelper(MapProperty, ValuePtr);
        TSharedPtr<FJsonObject> MapObject = MakeShared<FJsonObject>();
        const int32 MaxIndex = MapHelper.GetMaxIndex();
        for (int32 Index = 0; Index < MaxIndex; ++Index)
        {
            if (MapHelper.IsValidIndex(Index))
            {
                const FString KeyText = JsonValueToString(ToJson(MapProperty->KeyProp, MapHelper.GetKeyPtr(Index)));
                MapObject->SetField(KeyText, ToJson(MapProperty->ValueProp, MapHelper.GetValuePtr(Index)));
            }
        }
        return MakeShared<FJsonValueObject>(MapObject);
    }

    if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
    {
        FScriptArrayHelper ArrayHelper(ArrayProperty, ValuePtr);
        TArray<TSharedPtr<FJsonValue>> JsonArray;
        for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
        {
            const void* ElementPtr = ArrayHelper.GetRawPtr(Index);
            if (ElementPtr)
            {
                JsonArray.Add(ToJson(ArrayProperty->Inner, ElementPtr));
            }
        }
        return MakeShared<FJsonValueArray>(JsonArray);
    }

    if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
    {
        UScriptStruct* StructType = StructProperty->Struct;
        if (!StructType)
        {
            return MakeShared<FJsonValueNull>();
        }
        if (const FStructCodec* Codec = FMCPPropertyCodecs::FindStructCodec(StructType))
        {
            return Codec->ToJson(ValuePtr);
        }

        TSharedPtr<FJsonObject> StructObject = MakeShared<FJsonObject>();
        for (TFieldIterator<FProperty> FieldIt(StructType); FieldIt; ++FieldIt)
        {
            FProperty* FieldProperty = *FieldIt;
            const void* FieldPtr = FieldProperty->ContainerPtrToValuePtr<void>(ValuePtr);
            if (!FieldPtr)
            {
                continue;
            }
            StructObject->SetField(FieldProperty->GetName(), ToJson(FieldProperty, FieldPtr));
        }
        return MakeShared<FJsonValueObject>(StructObject);
    }

    return MakeShared<FJsonValueNull>();
}

//==============================================================================
// FromJson
//==============================================================================

namespace
{
    FWriteResult WritePropertyInternal(FProperty* Property, void* PropertyAddr, const FString& Context,
                                       const TSharedPtr<FJsonValue>& Value);
}

FWriteResult FMCPPropertyReflector::FromJson(FProperty* Property, void* PropertyAddr, const FString& Context,
                                             const TSharedPtr<FJsonValue>& Value)
{
    FWriteResult Result = WritePropertyInternal(Property, PropertyAddr, Context, Value);

    // A rejected write reports what the property holds now, so the caller does not have to ask whether
    // the value had already moved before the rejection.
    if (!Result.bSuccess && Result.CurrentValueJson.IsEmpty() && Property && PropertyAddr)
    {
        Result.CurrentValueJson = JsonValueToJsonText(ToJson(Property, PropertyAddr));
    }
    return Result;
}

namespace
{
FWriteResult WritePropertyInternal(FProperty* Property, void* PropertyAddr, const FString& Context,
                                   const TSharedPtr<FJsonValue>& Value)
{
    if (!Property)
    {
        return FWriteResult::Failure(TEXT("unknown_property"), TEXT("No reflected property was resolved"));
    }
    if (!PropertyAddr)
    {
        return FWriteResult::Failure(TEXT("unsupported_property_type"),
            FString::Printf(TEXT("%s has no writable address"), *Context));
    }

    const TArray<FString> ShapesForProperty = FMCPPropertyReflector::Describe(Property).SupportedShapes;

    auto RejectWithShapes = [&ShapesForProperty](const TCHAR* ErrorCode, const FString& Message)
    {
        return FWriteResult::Failure(ErrorCode, Message).WithShapes(ShapesForProperty);
    };

    if (!Value.IsValid())
    {
        return RejectWithShapes(TEXT("type_mismatch"), FString::Printf(TEXT("%s requires a value"), *Context));
    }

    // Escape hatch first: a registered property codec owns its shape and its policy.
    if (const FPropertyCodec* Codec = FMCPPropertyCodecs::FindPropertyCodec(Property))
    {
        FWriteResult CodecResult;
        if (Codec->FromJson(Property, PropertyAddr, Value, CodecResult))
        {
            CodecResult.bSuccess = true;
            CodecResult.bUnchanged = false;
            return CodecResult;
        }
        CodecResult.bSuccess = false;
        CodecResult.bUnchanged = true;
        if (CodecResult.ErrorCode.IsEmpty())
        {
            CodecResult.ErrorCode = TEXT("unsupported_property_type");
        }
        if (CodecResult.ErrorMessage.IsEmpty())
        {
            CodecResult.ErrorMessage = FString::Printf(
                TEXT("%s cannot be written through the property reflector"), *Context);
        }
        if (CodecResult.Hint.IsEmpty())
        {
            CodecResult.Hint = Codec->Hint;
        }
        if (CodecResult.SupportedShapes.Num() == 0)
        {
            CodecResult.SupportedShapes = Codec->Shapes;
        }
        return CodecResult;
    }

    if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
    {
        bool bBoolValue = false;
        if (!ReadBoolValue(Value, bBoolValue))
        {
            return RejectWithShapes(TEXT("type_mismatch"),
                FString::Printf(TEXT("Bool property %s requires a boolean value"), *Context));
        }
        BoolProperty->SetPropertyValue(PropertyAddr, bBoolValue);
        return FWriteResult::Success();
    }

    // TEnumAsByte<> byte properties go through the shared enum resolver; plain bytes are numeric.
    if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
    {
        if (UEnum* EnumDef = ByteProperty->GetIntPropertyEnum())
        {
            int64 EnumValue = 0;
            FWriteResult EnumResult;
            if (!ResolveEnumValue(EnumDef, Value, Context, EnumValue, EnumResult))
            {
                return EnumResult;
            }
            ByteProperty->SetPropertyValue(PropertyAddr, static_cast<uint8>(EnumValue));
            return FWriteResult::Success();
        }

        double Number = 0.0;
        if (!ReadNumberValue(Value, Number))
        {
            return RejectWithShapes(TEXT("type_mismatch"),
                FString::Printf(TEXT("Byte property %s requires a number value"), *Context));
        }
        ByteProperty->SetPropertyValue(PropertyAddr, static_cast<uint8>(Number));
        return FWriteResult::Success();
    }

    if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
    {
        UEnum* EnumDef = EnumProperty->GetEnum();
        FNumericProperty* UnderlyingProperty = EnumProperty->GetUnderlyingProperty();
        if (!EnumDef || !UnderlyingProperty)
        {
            return RejectWithShapes(TEXT("unsupported_property_type"),
                FString::Printf(TEXT("Enum property %s has no resolvable enum type"), *Context));
        }
        int64 EnumValue = 0;
        FWriteResult EnumResult;
        if (!ResolveEnumValue(EnumDef, Value, Context, EnumValue, EnumResult))
        {
            return EnumResult;
        }
        UnderlyingProperty->SetIntPropertyValue(PropertyAddr, EnumValue);
        return FWriteResult::Success();
    }

    // Numeric family: one branch covers Int8/Int16/Int32/Int64, UInt8/16/32/64, Float and Double.
    if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
    {
        double Number = 0.0;
        if (!ReadNumberValue(Value, Number))
        {
            return RejectWithShapes(TEXT("type_mismatch"),
                FString::Printf(TEXT("Numeric property %s requires a number value"), *Context));
        }
        if (NumericProperty->IsInteger())
        {
            NumericProperty->SetIntPropertyValue(PropertyAddr, static_cast<int64>(Number));
        }
        else
        {
            NumericProperty->SetFloatingPointPropertyValue(PropertyAddr, Number);
        }
        return FWriteResult::Success();
    }

    // String family: one text reader feeds all three, so none of them can end up write-missing.
    if (FStrProperty* StrProperty = CastField<FStrProperty>(Property))
    {
        FString Text;
        if (!ReadTextValue(Value, Text))
        {
            return RejectWithShapes(TEXT("type_mismatch"),
                FString::Printf(TEXT("String property %s requires a string value"), *Context));
        }
        StrProperty->SetPropertyValue(PropertyAddr, Text);
        return FWriteResult::Success();
    }
    if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
    {
        FString Text;
        if (!ReadTextValue(Value, Text))
        {
            return RejectWithShapes(TEXT("type_mismatch"),
                FString::Printf(TEXT("Name property %s requires a string value"), *Context));
        }
        NameProperty->SetPropertyValue(PropertyAddr, FName(*Text));
        return FWriteResult::Success();
    }
    if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
    {
        FString Text;
        if (!ReadTextValue(Value, Text))
        {
            return RejectWithShapes(TEXT("type_mismatch"),
                FString::Printf(TEXT("Text property %s requires a string value"), *Context));
        }
        TextProperty->SetPropertyValue(PropertyAddr, FText::FromString(Text));
        return FWriteResult::Success();
    }

    // Object family. Class flavors first, because FClassProperty derives from FObjectProperty and
    // FSoftClassProperty derives from FSoftObjectProperty.
    if (FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
    {
        if (Value->Type != EJson::String)
        {
            return RejectWithShapes(TEXT("type_mismatch"),
                FString::Printf(TEXT("Class property %s requires a class path string"), *Context));
        }
        const FString ClassPath = Value->AsString();
        UClass* RequiredBase = ClassProperty->MetaClass.Get();
        UClass* ResolvedClass = FindObject<UClass>(nullptr, *ClassPath);
        if (!ResolvedClass)
        {
            ResolvedClass = LoadObject<UClass>(nullptr, *ClassPath);
        }
        if (!ResolvedClass)
        {
            ResolvedClass = LoadClass<UObject>(nullptr, *ClassPath);
        }
        if (!ResolvedClass)
        {
            return RejectWithShapes(TEXT("load_failed"),
                FString::Printf(TEXT("Could not load class '%s' for property %s"), *ClassPath, *Context));
        }
        if (RequiredBase && !ResolvedClass->IsChildOf(RequiredBase))
        {
            return RejectWithShapes(TEXT("type_mismatch"),
                FString::Printf(TEXT("Class '%s' is not a subclass of '%s' required by property %s"),
                    *ResolvedClass->GetName(), *RequiredBase->GetName(), *Context));
        }
        ClassProperty->SetPropertyValue(PropertyAddr, ResolvedClass);
        return FWriteResult::Success();
    }

    if (FSoftClassProperty* SoftClassProperty = CastField<FSoftClassProperty>(Property))
    {
        if (Value->Type != EJson::String)
        {
            return RejectWithShapes(TEXT("type_mismatch"),
                FString::Printf(TEXT("Soft class property %s requires a class path string"), *Context));
        }
        const FString ClassPath = Value->AsString();
        UClass* ResolvedClass = FindObject<UClass>(nullptr, *ClassPath);
        if (!ResolvedClass)
        {
            ResolvedClass = LoadObject<UClass>(nullptr, *ClassPath);
        }
        if (!ResolvedClass)
        {
            ResolvedClass = LoadClass<UObject>(nullptr, *ClassPath);
        }
        if (!ResolvedClass)
        {
            return RejectWithShapes(TEXT("load_failed"),
                FString::Printf(TEXT("Could not load class '%s' for property %s"), *ClassPath, *Context));
        }
        UClass* RequiredBase = SoftClassProperty->MetaClass.Get();
        if (RequiredBase && !ResolvedClass->IsChildOf(RequiredBase))
        {
            return RejectWithShapes(TEXT("type_mismatch"),
                FString::Printf(TEXT("Class '%s' is not a subclass of '%s' required by property %s"),
                    *ResolvedClass->GetName(), *RequiredBase->GetName(), *Context));
        }
        SoftClassProperty->SetPropertyValue(PropertyAddr, FSoftObjectPtr(ResolvedClass));
        return FWriteResult::Success();
    }

    if (FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
    {
        if (Value->Type != EJson::String)
        {
            return RejectWithShapes(TEXT("type_mismatch"),
                FString::Printf(TEXT("Object property %s requires an asset path string"), *Context));
        }
        const FString AssetPath = Value->AsString();
        UObject* Asset = FUnrealMCPCommonUtils::FindAsset(AssetPath);
        if (!Asset)
        {
            return RejectWithShapes(TEXT("load_failed"),
                FString::Printf(TEXT("Could not load asset '%s' for property %s"), *AssetPath, *Context));
        }
        if (ObjectProperty->PropertyClass && !Asset->IsA(ObjectProperty->PropertyClass))
        {
            return RejectWithShapes(TEXT("type_mismatch"),
                FString::Printf(TEXT("Asset '%s' (%s) is not assignable to %s required by property %s"),
                    *Asset->GetName(), *Asset->GetClass()->GetName(),
                    *ObjectProperty->PropertyClass->GetName(), *Context));
        }
        ObjectProperty->SetObjectPropertyValue(PropertyAddr, Asset);
        return FWriteResult::Success();
    }

    if (FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
    {
        if (Value->Type != EJson::String)
        {
            return RejectWithShapes(TEXT("type_mismatch"),
                FString::Printf(TEXT("Soft object property %s requires an asset path string"), *Context));
        }
        const FString AssetPath = Value->AsString();
        UObject* Asset = FUnrealMCPCommonUtils::FindAsset(AssetPath);
        if (!Asset)
        {
            return RejectWithShapes(TEXT("load_failed"),
                FString::Printf(TEXT("Could not load asset '%s' for property %s"), *AssetPath, *Context));
        }
        if (SoftObjectProperty->PropertyClass && !Asset->IsA(SoftObjectProperty->PropertyClass))
        {
            return RejectWithShapes(TEXT("type_mismatch"),
                FString::Printf(TEXT("Asset '%s' (%s) is not assignable to %s required by property %s"),
                    *Asset->GetName(), *Asset->GetClass()->GetName(),
                    *SoftObjectProperty->PropertyClass->GetName(), *Context));
        }
        SoftObjectProperty->SetPropertyValue(PropertyAddr, FSoftObjectPtr(Asset));
        return FWriteResult::Success();
    }

    if (CastField<FObjectPropertyBase>(Property))
    {
        // Weak / lazy / interface object properties carry reference semantics this layer does not model.
        return RejectWithShapes(TEXT("unsupported_property_type"),
            FString::Printf(TEXT("Unsupported object property type: %s for property %s"),
                *Property->GetClass()->GetName(), *Context));
    }

    if (CastField<FArrayProperty>(Property) || CastField<FSetProperty>(Property) ||
        CastField<FMapProperty>(Property))
    {
        return WriteContainer(Property, PropertyAddr, Context, Value);
    }

    if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
    {
        UScriptStruct* StructType = StructProperty->Struct;
        if (!StructType)
        {
            return RejectWithShapes(TEXT("unsupported_property_type"),
                FString::Printf(TEXT("Struct property %s has no resolvable struct type"), *Context));
        }

        if (const FStructCodec* Codec = FMCPPropertyCodecs::FindStructCodec(StructType))
        {
            FWriteResult CodecResult;
            if (Codec->FromJson(Value, PropertyAddr, CodecResult))
            {
                CodecResult.bSuccess = true;
                CodecResult.bUnchanged = false;
                return CodecResult;
            }
            CodecResult.bSuccess = false;
            CodecResult.bUnchanged = true;
            if (CodecResult.ErrorCode.IsEmpty())
            {
                CodecResult.ErrorCode = TEXT("type_mismatch");
            }
            if (CodecResult.ErrorMessage.IsEmpty())
            {
                CodecResult.ErrorMessage = FString::Printf(TEXT("%s: value does not match %s"),
                    *Context, *StructType->GetName());
            }
            if (CodecResult.SupportedShapes.Num() == 0)
            {
                CodecResult.SupportedShapes = Codec->Shapes;
            }
            return CodecResult;
        }

        return FMCPPropertyReflector::WriteStructFields(StructType, PropertyAddr, Context, Value);
    }

    return RejectWithShapes(TEXT("unsupported_property_type"),
        FString::Printf(TEXT("Unsupported property type: %s for property %s"),
            *Property->GetClass()->GetName(), *Context));
}
} // namespace

//==============================================================================
// Describe / IsSupported
//==============================================================================

FPropertyDescriptor FMCPPropertyReflector::Describe(const FProperty* Property)
{
    FPropertyDescriptor Descriptor;
    if (!Property)
    {
        return Descriptor;
    }

    // GetCPPType splits a container's inner text into ExtendedTypeText, so concatenate to report the
    // real type ("TArray<FName>") instead of just the outer shape ("TArray").
    FString ExtendedType;
    Descriptor.CppType = Property->GetCPPType(&ExtendedType) + ExtendedType;
    Descriptor.PropertyClass = Property->GetClass()->GetName();

    if (const FPropertyCodec* Codec = FMCPPropertyCodecs::FindPropertyCodec(Property))
    {
        Descriptor.bHasCodec = true;
        Descriptor.bSupported = Codec->bWritable;
        Descriptor.SupportedShapes = Codec->Shapes;
        Descriptor.Hint = Codec->Hint;
        return Descriptor;
    }

    if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
    {
        Descriptor.Container = TEXT("Array");
        Descriptor.ElementType = ArrayProperty->Inner ? ArrayProperty->Inner->GetCPPType() : FString();
        Descriptor.Semantics = TEXT("replace");
        Descriptor.bSupported = IsSupported(ArrayProperty->Inner);
        Descriptor.SupportedShapes = MakeShapes({ ShapeArray });
        if (!Descriptor.bSupported)
        {
            // A container is unusable because of its element type, so the element's own hint is what
            // the caller needs (e.g. Custom input arrays -> set_custom_input_name).
            if (const FPropertyCodec* InnerCodec = FMCPPropertyCodecs::FindPropertyCodec(ArrayProperty->Inner))
            {
                Descriptor.Hint = InnerCodec->Hint;
            }
        }
        return Descriptor;
    }

    if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
    {
        Descriptor.Container = TEXT("Set");
        Descriptor.ElementType = SetProperty->ElementProp ? SetProperty->ElementProp->GetCPPType() : FString();
        Descriptor.Semantics = TEXT("replace");
        Descriptor.bSupported = IsSupported(SetProperty->ElementProp);
        Descriptor.SupportedShapes = MakeShapes({ ShapeArray });
        return Descriptor;
    }

    if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
    {
        Descriptor.Container = TEXT("Map");
        Descriptor.ElementType = MapProperty->KeyProp ? MapProperty->KeyProp->GetCPPType() : FString();
        Descriptor.ValueType = MapProperty->ValueProp ? MapProperty->ValueProp->GetCPPType() : FString();
        Descriptor.Semantics = TEXT("replace");
        Descriptor.bSupported = IsSupported(MapProperty->KeyProp) && IsSupported(MapProperty->ValueProp);
        Descriptor.SupportedShapes = MakeShapes({ ShapeKeyValueObject, ShapeKeyValuePairs });
        return Descriptor;
    }

    if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
    {
        if (const FStructCodec* Codec = FMCPPropertyCodecs::FindStructCodec(StructProperty->Struct))
        {
            Descriptor.bHasCodec = true;
            Descriptor.bSupported = true;
            Descriptor.SupportedShapes = Codec->Shapes;
            return Descriptor;
        }
        Descriptor.bSupported = true;
        Descriptor.SupportedShapes = MakeShapes({ ShapeFieldObject, ShapeNumberArray, ShapeStructText });
        return Descriptor;
    }

    if (CastField<FByteProperty>(Property) || CastField<FEnumProperty>(Property))
    {
        Descriptor.bSupported = true;
        Descriptor.SupportedShapes = MakeShapes({ ShapeEnumMember });
        return Descriptor;
    }

    if (CastField<FBoolProperty>(Property))
    {
        Descriptor.bSupported = true;
        Descriptor.SupportedShapes = MakeShapes({ ShapeBoolean, ShapeNumber, ShapeString });
        return Descriptor;
    }

    if (CastField<FNumericProperty>(Property))
    {
        Descriptor.bSupported = true;
        Descriptor.SupportedShapes = MakeShapes({ ShapeNumber, ShapeString });
        return Descriptor;
    }

    if (CastField<FStrProperty>(Property) || CastField<FNameProperty>(Property) ||
        CastField<FTextProperty>(Property))
    {
        Descriptor.bSupported = true;
        Descriptor.SupportedShapes = MakeShapes({ ShapeString });
        return Descriptor;
    }

    if (CastField<FClassProperty>(Property) || CastField<FSoftClassProperty>(Property) ||
        CastField<FObjectProperty>(Property) || CastField<FSoftObjectProperty>(Property))
    {
        Descriptor.bSupported = true;
        Descriptor.SupportedShapes = MakeShapes({ ShapeAssetPath });
        return Descriptor;
    }

    if (CastField<FObjectPropertyBase>(Property))
    {
        Descriptor.bSupported = false;
        Descriptor.SupportedShapes = MakeShapes({ ShapeAssetPath });
        Descriptor.Hint = TEXT("Weak / lazy / interface object properties are not writable through the reflector; use execute_python_command");
        return Descriptor;
    }

    Descriptor.bSupported = false;
    return Descriptor;
}

bool FMCPPropertyReflector::IsSupported(const FProperty* Property)
{
    return Describe(Property).bSupported;
}

TArray<FString> FMCPPropertyReflector::SupportedShapeVocabulary()
{
    return MakeShapes({ ShapeNumber, ShapeBoolean, ShapeString, ShapeAssetPath, ShapeEnumMember,
                        ShapeArray, ShapeKeyValueObject, ShapeNumberArray, ShapeFieldObject, ShapeStructText });
}

//==============================================================================
// Struct field recursion: a field object (by name, case/underscore insensitive) or an array in
// declaration order, validated on a scratch copy and only committed when every field is usable.
//==============================================================================

FWriteResult FMCPPropertyReflector::WriteStructFields(UScriptStruct* StructType, void* StructAddr,
                                                      const FString& Context, const TSharedPtr<FJsonValue>& Value)
{
    const TArray<FString> StructShapes = MakeShapes({ ShapeFieldObject, ShapeNumberArray, ShapeStructText });

    if (!StructType || !StructAddr || !Value.IsValid())
    {
        return FWriteResult::Failure(TEXT("type_mismatch"),
            FString::Printf(TEXT("Struct property %s requires an object, array or struct text value"), *Context))
            .WithShapes(StructShapes);
    }

    // A string is the struct's own text form: route it through the struct's import (which honours
    // custom ImportTextItem, e.g. FKey takes a bare key name "I"), on a scratch copy so a bad text
    // leaves the value untouched.
    if (Value->Type == EJson::String)
    {
        const FString Text = Value->AsString();
        uint8* TextScratch = static_cast<uint8*>(FMemory::Malloc(
            static_cast<SIZE_T>(StructType->GetStructureSize()),
            static_cast<uint32>(StructType->GetMinAlignment())));
        StructType->InitializeStruct(TextScratch);
        StructType->CopyScriptStruct(TextScratch, StructAddr);

        FString Before;
        StructType->ExportText(Before, StructAddr, nullptr, nullptr, PPF_None, nullptr);

        FStringOutputDevice ImportErrors;
        const FString StructName = StructType->GetName();
        const TCHAR* End = StructType->ImportText(*Text, TextScratch, nullptr, PPF_None, &ImportErrors, StructName);
        FString After;
        if (End)
        {
            StructType->ExportText(After, TextScratch, nullptr, nullptr, PPF_None, nullptr);
        }
        // An import that "succeeds" without changing anything, for a text that is not already the
        // current value, parsed nothing: report it rather than claim a write.
        const bool bImported = End != nullptr && ImportErrors.Len() == 0 && !(After == Before && Text != Before);

        FWriteResult TextResult = FWriteResult::Success();
        if (bImported)
        {
            StructType->CopyScriptStruct(StructAddr, TextScratch);
        }
        else
        {
            TextResult = FWriteResult::Failure(TEXT("type_mismatch"),
                FString::Printf(TEXT("Struct property %s: '%s' does not import as %s%s%s"),
                    *Context, *Text, *StructType->GetName(),
                    ImportErrors.Len() > 0 ? TEXT(": ") : TEXT(""), *ImportErrors))
                .WithShapes(StructShapes);
        }
        StructType->DestroyStruct(TextScratch);
        FMemory::Free(TextScratch);
        return TextResult;
    }

    TArray<FProperty*> Fields;
    TArray<TSharedPtr<FJsonValue>> FieldValues;

    if (Value->Type == EJson::Object)
    {
        const TSharedPtr<FJsonObject>* ObjectValue = nullptr;
        if (!Value->TryGetObject(ObjectValue) || !ObjectValue || !(*ObjectValue).IsValid())
        {
            return FWriteResult::Failure(TEXT("type_mismatch"),
                FString::Printf(TEXT("Struct property %s requires an object value"), *Context))
                .WithShapes(StructShapes);
        }

        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*ObjectValue)->Values)
        {
            FProperty* Field = ResolveStructField(StructType, Pair.Key);
            if (!Field)
            {
                TArray<FString> AvailableFields;
                CollectStructFieldNames(StructType, AvailableFields);
                return FWriteResult::Failure(TEXT("unknown_field"),
                    FString::Printf(TEXT("Struct field not found: %s on %s. Available fields: %s"),
                        *Pair.Key, *StructType->GetName(), *FString::Join(AvailableFields, TEXT(", "))))
                    .WithFields(AvailableFields)
                    .WithShapes(StructShapes);
            }
            Fields.Add(Field);
            FieldValues.Add(Pair.Value);
        }
    }
    else if (Value->Type == EJson::Array)
    {
        const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
        if (!Value->TryGetArray(Items) || !Items)
        {
            return FWriteResult::Failure(TEXT("type_mismatch"),
                FString::Printf(TEXT("Struct property %s requires an array value"), *Context))
                .WithShapes(StructShapes);
        }

        TArray<FProperty*> OrderedFields;
        for (TFieldIterator<FProperty> FieldIt(StructType); FieldIt; ++FieldIt)
        {
            if (IsWritableStructField(*FieldIt))
            {
                OrderedFields.Add(*FieldIt);
            }
        }
        if (OrderedFields.Num() != Items->Num())
        {
            return FWriteResult::Failure(TEXT("invalid_value"),
                FString::Printf(TEXT("Struct %s expects %d values in declaration order, got %d"),
                    *StructType->GetName(), OrderedFields.Num(), Items->Num()))
                .WithShapes(StructShapes);
        }
        for (int32 Index = 0; Index < OrderedFields.Num(); ++Index)
        {
            Fields.Add(OrderedFields[Index]);
            FieldValues.Add((*Items)[Index]);
        }
    }
    else
    {
        return FWriteResult::Failure(TEXT("type_mismatch"),
            FString::Printf(TEXT("Struct property %s requires an object or array value"), *Context))
            .WithShapes(StructShapes);
    }

    uint8* Scratch = static_cast<uint8*>(FMemory::Malloc(
        static_cast<SIZE_T>(StructType->GetStructureSize()),
        static_cast<uint32>(StructType->GetMinAlignment())));
    StructType->InitializeStruct(Scratch);
    StructType->CopyScriptStruct(Scratch, StructAddr);

    FWriteResult Result = FWriteResult::Success();
    for (int32 Index = 0; Index < Fields.Num(); ++Index)
    {
        FProperty* Field = Fields[Index];
        void* FieldAddr = Field->ContainerPtrToValuePtr<void>(Scratch);

        FWriteResult FieldResult = FromJson(Field, FieldAddr, Field->GetName(), FieldValues[Index]);
        if (!FieldResult.bSuccess)
        {
            Result = FieldResult;
            Result.ErrorMessage = FString::Printf(TEXT("%s.%s: %s"),
                *Context, *Field->GetName(), *FieldResult.ErrorMessage);
            break;
        }
    }

    if (Result.bSuccess)
    {
        StructType->CopyScriptStruct(StructAddr, Scratch);
    }

    StructType->DestroyStruct(Scratch);
    FMemory::Free(Scratch);
    return Result;
}

//==============================================================================
// Numbers from an array, or from a struct-shaped object carrying the named fields. Object keys are
// matched case- and underscore-insensitively, so {"x": 1} writes the same struct as {"X": 1}.
//==============================================================================

bool FMCPPropertyReflector::ReadNumbersFromJson(const TSharedPtr<FJsonValue>& Value,
                                                const TArray<FString>& ObjectKeys,
                                                TArray<double>& OutNumbers, FString& OutErrorMessage)
{
    OutNumbers.Reset();
    OutErrorMessage.Reset();

    if (!Value.IsValid())
    {
        OutErrorMessage = TEXT("missing value");
        return false;
    }

    if (Value->Type == EJson::Array)
    {
        const TArray<TSharedPtr<FJsonValue>>* ArrayValue = nullptr;
        if (Value->TryGetArray(ArrayValue) && ArrayValue)
        {
            for (const TSharedPtr<FJsonValue>& Item : *ArrayValue)
            {
                if (!Item.IsValid() || Item->Type != EJson::Number)
                {
                    OutErrorMessage = TEXT("expected an array of numbers");
                    return false;
                }
                OutNumbers.Add(Item->AsNumber());
            }
        }
        return OutNumbers.Num() > 0;
    }

    if (Value->Type == EJson::Object)
    {
        const TSharedPtr<FJsonObject>* ObjectValue = nullptr;
        if (Value->TryGetObject(ObjectValue) && ObjectValue && (*ObjectValue).IsValid())
        {
            for (const FString& Key : ObjectKeys)
            {
                double Number = 0.0;
                bool bFound = (*ObjectValue)->TryGetNumberField(Key, Number);
                if (!bFound)
                {
                    const FString Wanted = NormalizeToken(Key);
                    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*ObjectValue)->Values)
                    {
                        if (NormalizeToken(Pair.Key) == Wanted && Pair.Value.IsValid() &&
                            Pair.Value->Type == EJson::Number)
                        {
                            Number = Pair.Value->AsNumber();
                            bFound = true;
                            break;
                        }
                    }
                }
                if (!bFound)
                {
                    OutErrorMessage = FString::Printf(TEXT("expected a '%s' number field"), *Key);
                    return false;
                }
                OutNumbers.Add(Number);
            }
            return true;
        }
    }

    OutErrorMessage = TEXT("expected an array of numbers or a struct object");
    return false;
}
