#include "Commands/PCG/UnrealMCPPCGCommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Commands/PCG/UnrealMCPPCGGraphFactory.h"
#include "Core/MCPCommandRegistry.h"
#include "Reflection/MCPPropertyReflector.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Data/PCGPointData.h"
#include "Data/PCGSpatialData.h"
#include "Editor.h"
#include "Elements/PCGStaticMeshSpawner.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInterface.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttribute.h"
#include "Metadata/PCGMetadataAttributeTraits.h"
#include "Metadata/PCGMetadataAttributeTpl.h"
#include "Metadata/PCGMetadataCommon.h"
#include "MeshSelectors/PCGISMDescriptor.h"
#include "MeshSelectors/PCGMeshSelectorBase.h"
#include "MeshSelectors/PCGMeshSelectorWeighted.h"
#include "PCGComponent.h"
#include "PCGData.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGPoint.h"
#include "PCGSettings.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectIterator.h"

namespace
{
    /** Which JSON shape a reflected value came back as ("null" when it is not representable). */
    const TCHAR* JsonValueShape(EJson Type)
    {
        switch (Type)
        {
        case EJson::None: return TEXT("none");
        case EJson::Null: return TEXT("null");
        case EJson::String: return TEXT("string");
        case EJson::Number: return TEXT("number");
        case EJson::Boolean: return TEXT("bool");
        case EJson::Array: return TEXT("array");
        case EJson::Object: return TEXT("object");
        default: return TEXT("unknown");
        }
    }

    // --- math struct readback -------------------------------------------------------------------
    //
    // Values are read through the shared property reflector (references, containers and arbitrary
    // structs included). The exception is the math structs below: the reflector reports them as
    // positional arrays, and a rotator read back as [360, 0, 0] cannot be told apart from a vector,
    // so those keep named components (x/y/z/w, pitch/yaw/roll, r/g/b/a).
    //
    // History worth keeping: a previous version read only a whitelist of shapes because reflecting
    // real settings objects crashed the editor five times. The actual cause was in this file - the
    // container address had the property offset applied twice (Object + 2 * Offset) - not the
    // property shapes; removing the whitelist afterwards produced zero unreadable properties.

    double NumericPropertyAsDouble(const FNumericProperty* NumericProperty, const void* ValuePtr)
    {
        return NumericProperty->IsInteger()
            ? static_cast<double>(NumericProperty->GetSignedIntPropertyValue(ValuePtr))
            : NumericProperty->GetFloatingPointPropertyValue(ValuePtr);
    }

    /** One component of a math struct, looked up by field name (X / Y / Z / W / R / G / B / A / Pitch / Yaw / Roll). */
    bool ReadStructComponent(const UScriptStruct* StructType, const void* ValuePtr, const FString& FieldName, double& OutValue)
    {
        const FNumericProperty* Field = StructType
            ? CastField<FNumericProperty>(FindFProperty<FProperty>(StructType, FName(*FieldName)))
            : nullptr;
        if (!Field)
        {
            return false;
        }
        OutValue = NumericPropertyAsDouble(Field, Field->ContainerPtrToValuePtr<void>(ValuePtr));
        return true;
    }

    bool ReadMathStructValue(const UScriptStruct* StructType, const void* ValuePtr, TSharedPtr<FJsonValue>& OutValue)
    {
        if (!StructType || !ValuePtr)
        {
            return false;
        }

        static const TMap<FString, TArray<FString>> Layouts = {
            { TEXT("Vector"),      { TEXT("X"), TEXT("Y"), TEXT("Z") } },
            { TEXT("Vector3f"),    { TEXT("X"), TEXT("Y"), TEXT("Z") } },
            { TEXT("Vector3d"),    { TEXT("X"), TEXT("Y"), TEXT("Z") } },
            { TEXT("Vector2D"),    { TEXT("X"), TEXT("Y") } },
            { TEXT("Vector4"),     { TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W") } },
            { TEXT("LinearColor"), { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") } },
            { TEXT("Rotator"),     { TEXT("Pitch"), TEXT("Yaw"), TEXT("Roll") } },
            { TEXT("Quat"),        { TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W") } },
            { TEXT("IntPoint"),    { TEXT("X"), TEXT("Y") } },
            { TEXT("IntVector"),   { TEXT("X"), TEXT("Y"), TEXT("Z") } },
        };

        const TArray<FString>* Components = Layouts.Find(StructType->GetName());
        if (!Components)
        {
            return false;
        }

        TSharedPtr<FJsonObject> Object = MakeShareable(new FJsonObject);
        for (const FString& Component : *Components)
        {
            double ComponentValue = 0.0;
            if (!ReadStructComponent(StructType, ValuePtr, Component, ComponentValue))
            {
                return false;
            }
            // Lower case on purpose: UE python spells struct members lower case (v.x, r.pitch), so
            // the key here matches what a caller would type in python.
            Object->SetNumberField(Component.ToLower(), ComponentValue);
        }
        OutValue = MakeShareable(new FJsonValueObject(Object));
        return true;
    }

    /** Property name as the editor python layer spells it (LowerBound -> lower_bound). */
    FString PythonStyleName(const FString& PropertyName)
    {
        FString Out;
        Out.Reserve(PropertyName.Len() + 4);
        for (int32 Index = 0; Index < PropertyName.Len(); ++Index)
        {
            const TCHAR Char = PropertyName[Index];
            if (FChar::IsUpper(Char) && Index > 0)
            {
                Out.AppendChar(TEXT('_'));
            }
            Out.AppendChar(FChar::ToLower(Char));
        }
        return Out;
    }

    /**
     * Whether the shared property writer can set this property, and why not when it cannot.
     *
     * One implementation for both the reflected table (get_pcg_node) and the write gate
     * (set_pcg_node_property): a caller that was told "writable: false" must get exactly the same
     * answer - and the same hint - when it tries anyway.
     */
    bool PropertyWritability(const FProperty* Property, FString& OutHint)
    {
        const bool bEditable = Property->HasAnyPropertyFlags(CPF_Edit);
        const bool bEditConst = Property->HasAnyPropertyFlags(CPF_EditConst);
        const bool bTransient = Property->HasAnyPropertyFlags(CPF_Transient);
        const bool bInstanced = Property->HasAnyPropertyFlags(CPF_InstancedReference)
            || Property->HasAnyPropertyFlags(CPF_PersistentInstance)
            || Property->HasAnyPropertyFlags(CPF_ExportObject);

        if (bEditable && !bEditConst && !bTransient && !bInstanced)
        {
            OutHint.Reset();
            return true;
        }

        OutHint = bInstanced
            ? TEXT("instanced subobject: read its value, then write the inner object (or use the domain setter, e.g. set_mesh_selector_type) - assigning the property itself fails")
            : (bEditConst
                ? TEXT("edit-const: engine managed, the property writer cannot set it")
                : (bTransient ? TEXT("transient: editor-only state, not writable") : TEXT("not flagged editable: not writable through the property writer")));
        return false;
    }

    /**
     * One reflected property as JSON.
     *
     * Container is the *base* address of the owning object (not an offset address): the property
     * offset is applied exactly once, here. Passing an already-offset pointer made every read point
     * at base + 2 * offset and crashed the editor on every property shape.
     */
    TSharedPtr<FJsonObject> PropertyToJsonEntry(FProperty* Property, const void* Container)
    {
        TSharedPtr<FJsonObject> Entry = MakeShareable(new FJsonObject);
        Entry->SetStringField(TEXT("property_name"), Property->GetName());
        Entry->SetStringField(TEXT("python_name"), PythonStyleName(Property->GetName()));
        Entry->SetStringField(TEXT("type"), Property->GetCPPType());

        // Values come from the shared reflector (references, containers and arbitrary structs
        // included - the whitelist that used to sit here was a reaction to a bug in this file, not
        // to a property-shape problem). The known math structs are the one exception: they keep
        // named components, because the reflector's positional arrays make e.g. a rotator ambiguous.
        FString ValueText;
        const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Container);
        TSharedPtr<FJsonValue> Value;
        if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
        {
            ReadMathStructValue(StructProperty->Struct, ValuePtr, Value);
        }
        if (!Value.IsValid() && ValuePtr)
        {
            Value = FUnrealMCPCommonUtils::PropertyValueToJson(Property, ValuePtr);
        }
        if (Value.IsValid() && Value->Type != EJson::Null)
        {
            Entry->SetField(TEXT("value"), Value);
            Entry->SetStringField(TEXT("value_shape"), JsonValueShape(Value->Type));

            // The text form is derived from the reflected value; arrays/objects keep their structure.
            switch (Value->Type)
            {
            case EJson::String: ValueText = Value->AsString(); break;
            case EJson::Number:
            {
                // Integer properties keep an integer text form: an int32 read back as "2.000000" is
                // indistinguishable from a float in the text field, which matters when the whole
                // point of the type field is catching float-into-int truncation.
                const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property);
                ValueText = (NumericProperty && NumericProperty->IsInteger())
                    ? LexToString(static_cast<int64>(Value->AsNumber()))
                    : LexToString(Value->AsNumber());
                break;
            }
            case EJson::Boolean: ValueText = Value->AsBool() ? TEXT("true") : TEXT("false"); break;
            default: break;
            }
            if (!ValueText.IsEmpty())
            {
                Entry->SetStringField(TEXT("value_text"), ValueText);
            }
        }
        else
        {
            Entry->SetStringField(TEXT("value_shape"), TEXT("omitted"));
            Entry->SetStringField(TEXT("value_note"),
                TEXT("value not read: the shared reflector has no JSON shape for this property type (the property is still reported with its type and flags)"));
        }

        // Enums are reported twice on purpose: the member name is what a caller writes back,
        // the enum name identifies which enum the member belongs to.
        if (ValuePtr && Property->IsA<FEnumProperty>())
        {
            const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property);
            if (const UEnum* Enum = EnumProperty ? EnumProperty->GetEnum() : nullptr)
            {
                const int64 Raw = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
                Entry->SetStringField(TEXT("enum"), Enum->GetName());
                Entry->SetStringField(TEXT("enum_member"), Enum->GetNameStringByValue(Raw));
            }
        }
        else if (ValuePtr)
        {
            const FByteProperty* ByteProperty = CastField<FByteProperty>(Property);
            if (ByteProperty && ByteProperty->Enum)
            {
                const int64 Raw = static_cast<int64>(ByteProperty->GetPropertyValue(ValuePtr));
                Entry->SetStringField(TEXT("enum"), ByteProperty->Enum->GetName());
                Entry->SetStringField(TEXT("enum_member"), ByteProperty->Enum->GetNameStringByValue(Raw));
            }
        }

        const bool bEditable = Property->HasAnyPropertyFlags(CPF_Edit);
        const bool bEditConst = Property->HasAnyPropertyFlags(CPF_EditConst);
        const bool bTransient = Property->HasAnyPropertyFlags(CPF_Transient);
        const bool bInstanced = Property->HasAnyPropertyFlags(CPF_InstancedReference)
            || Property->HasAnyPropertyFlags(CPF_PersistentInstance)
            || Property->HasAnyPropertyFlags(CPF_ExportObject);
        Entry->SetBoolField(TEXT("editable"), bEditable);
        Entry->SetBoolField(TEXT("edit_const"), bEditConst);
        Entry->SetBoolField(TEXT("transient"), bTransient);
        Entry->SetBoolField(TEXT("instanced"), bInstanced);

        // Writability here means "the shared property writer can set it". Instanced subobjects are
        // the trap this command exists for: MeshSelectorParameters reads fine, but assigning it
        // fails ("is read-only and cannot be set") while its inner object IS writable.
        FString NotWritableHint;
        const bool bWritable = PropertyWritability(Property, NotWritableHint);
        Entry->SetBoolField(TEXT("writable"), bWritable);
        if (!bWritable)
        {
            Entry->SetStringField(TEXT("hint"), NotWritableHint);
        }

        if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
        {
            Entry->SetStringField(TEXT("container"), TEXT("array"));
            Entry->SetStringField(TEXT("element_type"), ArrayProperty->Inner->GetCPPType());
        }
        else if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
        {
            Entry->SetStringField(TEXT("container"), TEXT("set"));
            Entry->SetStringField(TEXT("element_type"), SetProperty->ElementProp->GetCPPType());
        }
        else if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
        {
            Entry->SetStringField(TEXT("container"), TEXT("map"));
            Entry->SetStringField(TEXT("key_type"), MapProperty->KeyProp->GetCPPType());
            Entry->SetStringField(TEXT("value_type"), MapProperty->ValueProp->GetCPPType());
        }

        return Entry;
    }

    /** Reflect one object's property table. bTruncated tells the caller the table was cut short. */
    TSharedPtr<FJsonObject> BuildPropertyTable(UObject* Object, int32 MaxProperties)
    {
        TArray<TSharedPtr<FJsonValue>> Properties;
        int32 WritableCount = 0;
        int32 ReadonlyInstancedCount = 0;

        const UClass* Class = Object ? Object->GetClass() : nullptr;
        int32 Seen = 0;
        for (TFieldIterator<FProperty> It(Class); It; ++It)
        {
            FProperty* Property = *It;
            if (!Property)
            {
                continue;
            }
            if (MaxProperties > 0 && Seen >= MaxProperties)
            {
                break;
            }

            // The base object address: PropertyToJsonEntry applies the property offset itself.
            const TSharedPtr<FJsonObject> Entry = PropertyToJsonEntry(Property, Object);
            if (Entry->GetBoolField(TEXT("writable")))
            {
                ++WritableCount;
            }
            if (Entry->GetBoolField(TEXT("instanced")))
            {
                ++ReadonlyInstancedCount;
            }
            Properties.Add(MakeShareable(new FJsonValueObject(Entry)));
            ++Seen;
        }

        TSharedPtr<FJsonObject> Table = MakeShareable(new FJsonObject);
        Table->SetArrayField(TEXT("properties"), Properties);
        Table->SetNumberField(TEXT("property_count"), Properties.Num());
        Table->SetNumberField(TEXT("writable_count"), WritableCount);
        Table->SetNumberField(TEXT("readonly_instanced_count"), ReadonlyInstancedCount);
        Table->SetBoolField(TEXT("truncated"), MaxProperties > 0 && Properties.Num() == MaxProperties);
        return Table;
    }

    // --- attribute statistics -------------------------------------------------------------------

    struct FAttributeStats
    {
        bool bNumeric = false;
        int32 Components = 0;
        bool bRotator = false;
        bool bColor = false;
        int32 Count = 0;
        double Min[4] = { 0.0, 0.0, 0.0, 0.0 };
        double Max[4] = { 0.0, 0.0, 0.0, 0.0 };
        double Sum[4] = { 0.0, 0.0, 0.0, 0.0 };
        TArray<TArray<double>> Samples;

        void Add(const double* Values, int32 Num)
        {
            const int32 Used = FMath::Min(Num, 4);
            for (int32 Index = 0; Index < Used; ++Index)
            {
                if (Count == 0)
                {
                    Min[Index] = Values[Index];
                    Max[Index] = Values[Index];
                }
                else
                {
                    Min[Index] = FMath::Min(Min[Index], Values[Index]);
                    Max[Index] = FMath::Max(Max[Index], Values[Index]);
                }
                Sum[Index] += Values[Index];
            }
            ++Count;
        }
    };

    void ToComponents(float Value, double Out[4], int32& OutCount) { Out[0] = Value; OutCount = 1; }
    void ToComponents(double Value, double Out[4], int32& OutCount) { Out[0] = Value; OutCount = 1; }
    void ToComponents(int32 Value, double Out[4], int32& OutCount) { Out[0] = Value; OutCount = 1; }
    void ToComponents(int64 Value, double Out[4], int32& OutCount) { Out[0] = static_cast<double>(Value); OutCount = 1; }
    void ToComponents(bool Value, double Out[4], int32& OutCount) { Out[0] = Value ? 1.0 : 0.0; OutCount = 1; }
    void ToComponents(const FVector2D& Value, double Out[4], int32& OutCount) { Out[0] = Value.X; Out[1] = Value.Y; OutCount = 2; }
    void ToComponents(const FVector& Value, double Out[4], int32& OutCount) { Out[0] = Value.X; Out[1] = Value.Y; Out[2] = Value.Z; OutCount = 3; }
    void ToComponents(const FVector4& Value, double Out[4], int32& OutCount) { Out[0] = Value.X; Out[1] = Value.Y; Out[2] = Value.Z; Out[3] = Value.W; OutCount = 4; }
    void ToComponents(const FLinearColor& Value, double Out[4], int32& OutCount) { Out[0] = Value.R; Out[1] = Value.G; Out[2] = Value.B; Out[3] = Value.A; OutCount = 4; }
    void ToComponents(const FQuat& Value, double Out[4], int32& OutCount) { Out[0] = Value.X; Out[1] = Value.Y; Out[2] = Value.Z; Out[3] = Value.W; OutCount = 4; }
    void ToComponents(const FRotator& Value, double Out[4], int32& OutCount) { Out[0] = Value.Pitch; Out[1] = Value.Yaw; Out[2] = Value.Roll; OutCount = 3; }

    template <typename T>
    bool AccumulateAttribute(const UPCGMetadata* Metadata, FName AttributeName,
                             const TArray<FPCGPoint>& Points, int32 MaxSamples, FAttributeStats& OutStats)
    {
        const FPCGMetadataAttribute<T>* Attribute = Metadata ? Metadata->GetConstTypedAttribute<T>(AttributeName) : nullptr;
        if (!Attribute)
        {
            return false;
        }

        OutStats.bNumeric = true;
        for (const FPCGPoint& Point : Points)
        {
            const T Value = Attribute->GetValueFromItemKey(Point.MetadataEntry);
            double Components[4] = { 0.0, 0.0, 0.0, 0.0 };
            int32 Num = 0;
            ToComponents(Value, Components, Num);
            if (OutStats.Components == 0)
            {
                OutStats.Components = Num;
                OutStats.bRotator = std::is_same_v<T, FRotator>;
            }
            OutStats.Add(Components, Num);
            if (OutStats.Samples.Num() < MaxSamples)
            {
                OutStats.Samples.Add(TArray<double>(Components, Num));
            }
        }
        return true;
    }

    void AccumulateByType(const UPCGMetadata* Metadata, FName AttributeName, EPCGMetadataTypes Type,
                          const TArray<FPCGPoint>& Points, int32 MaxSamples, FAttributeStats& OutStats)
    {
        switch (Type)
        {
        case EPCGMetadataTypes::Float: AccumulateAttribute<float>(Metadata, AttributeName, Points, MaxSamples, OutStats); break;
        case EPCGMetadataTypes::Double: AccumulateAttribute<double>(Metadata, AttributeName, Points, MaxSamples, OutStats); break;
        case EPCGMetadataTypes::Integer32: AccumulateAttribute<int32>(Metadata, AttributeName, Points, MaxSamples, OutStats); break;
        case EPCGMetadataTypes::Integer64: AccumulateAttribute<int64>(Metadata, AttributeName, Points, MaxSamples, OutStats); break;
        case EPCGMetadataTypes::Boolean: AccumulateAttribute<bool>(Metadata, AttributeName, Points, MaxSamples, OutStats); break;
        case EPCGMetadataTypes::Vector2: AccumulateAttribute<FVector2D>(Metadata, AttributeName, Points, MaxSamples, OutStats); break;
        case EPCGMetadataTypes::Vector: AccumulateAttribute<FVector>(Metadata, AttributeName, Points, MaxSamples, OutStats); break;
        case EPCGMetadataTypes::Vector4: AccumulateAttribute<FVector4>(Metadata, AttributeName, Points, MaxSamples, OutStats); break;
        case EPCGMetadataTypes::Rotator: AccumulateAttribute<FRotator>(Metadata, AttributeName, Points, MaxSamples, OutStats); break;
        case EPCGMetadataTypes::Quaternion: AccumulateAttribute<FQuat>(Metadata, AttributeName, Points, MaxSamples, OutStats); break;
        default: break;   // string / name / soft path / transform: not a numeric distribution
        }
    }

    /** The FPCGPoint fields that can be summarised straight off the point, with no metadata attribute. */
    const TMap<FString, FString>& PointFieldTypes()
    {
        static const TMap<FString, FString> Fields = {
            { TEXT("density"),   TEXT("Float") },
            { TEXT("steepness"), TEXT("Float") },
            { TEXT("seed"),      TEXT("Integer32") },
            { TEXT("color"),     TEXT("Vector4") },
            { TEXT("position"),  TEXT("Vector") },
            { TEXT("scale"),     TEXT("Vector") },
        };
        return Fields;
    }

    /**
     * Point-intrinsic fields (not metadata attributes): Density and Seed are plain members, Color is
     * a vector, Position/Scale come from the point transform. Case insensitive on purpose - the
     * caller writes "Density", the lookup key is lower case.
     */
    bool AccumulatePointField(const FString& AttributeName, const TArray<FPCGPoint>& Points, int32 MaxSamples,
                              FAttributeStats& OutStats)
    {
        const FString Key = AttributeName.ToLower();
        if (!PointFieldTypes().Contains(Key))
        {
            return false;
        }

        OutStats.bNumeric = true;
        for (const FPCGPoint& Point : Points)
        {
            double Components[4] = { 0.0, 0.0, 0.0, 0.0 };
            int32 Num = 0;
            if (Key == TEXT("density"))
            {
                Components[0] = Point.Density;
                Num = 1;
            }
            else if (Key == TEXT("steepness"))
            {
                Components[0] = Point.Steepness;
                Num = 1;
            }
            else if (Key == TEXT("seed"))
            {
                Components[0] = Point.Seed;
                Num = 1;
            }
            else if (Key == TEXT("color"))
            {
                // FPCGPoint::Color is an FVector4 (X/Y/Z/W); the channels are reported as r/g/b/a.
                Components[0] = Point.Color.X;
                Components[1] = Point.Color.Y;
                Components[2] = Point.Color.Z;
                Components[3] = Point.Color.W;
                Num = 4;
            }
            else if (Key == TEXT("position"))
            {
                const FVector Position = Point.Transform.GetLocation();
                Components[0] = Position.X;
                Components[1] = Position.Y;
                Components[2] = Position.Z;
                Num = 3;
            }
            else if (Key == TEXT("scale"))
            {
                const FVector Scale = Point.Transform.GetScale3D();
                Components[0] = Scale.X;
                Components[1] = Scale.Y;
                Components[2] = Scale.Z;
                Num = 3;
            }
            else
            {
                return false;
            }

            if (OutStats.Components == 0)
            {
                OutStats.Components = Num;
                OutStats.bColor = (Key == TEXT("color"));
            }
            OutStats.Add(Components, Num);
            if (OutStats.Samples.Num() < MaxSamples)
            {
                OutStats.Samples.Add(TArray<double>(Components, Num));
            }
        }
        return true;
    }

    FString AttributeTypeName(EPCGMetadataTypes Type)
    {
        return StaticEnum<EPCGMetadataTypes>() ? StaticEnum<EPCGMetadataTypes>()->GetNameStringByValue(static_cast<int64>(Type)) : FString();
    }

    FString GenerationTriggerName(EPCGComponentGenerationTrigger Trigger)
    {
        return StaticEnum<EPCGComponentGenerationTrigger>()
            ? StaticEnum<EPCGComponentGenerationTrigger>()->GetNameStringByValue(static_cast<int64>(Trigger))
            : FString();
    }

    // --- write path -----------------------------------------------------------------------------

    /**
     * Equal within the precision a property write can be expected to survive.
     *
     * A float property legitimately rounds on the way in (0.3 reads back as 0.30000001192092896),
     * so an exact comparison would report a mismatch for every honest float write. Integer
     * properties are the reason the tolerance is not larger: 0.7 written into an int32 lands on 0,
     * which MUST stay visible.
     */
    bool NumbersEquivalent(double Left, double Right)
    {
        const double Scale = FMath::Max(1.0, FMath::Max(FMath::Abs(Left), FMath::Abs(Right)));
        return FMath::Abs(Left - Right) <= 1e-5 * Scale;
    }

    /**
     * Key name with separators and case removed: "bFitToCurve" and "b_fit_to_curve" are the same
     * field, and a write must not be reported as a mismatch over spelling.
     */
    FString NormalizedKeyName(const FString& KeyName)
    {
        FString Out;
        Out.Reserve(KeyName.Len());
        for (const TCHAR Char : KeyName)
        {
            if (Char != TEXT('_'))
            {
                Out.AppendChar(FChar::ToLower(Char));
            }
        }
        return Out;
    }

    /** Structural comparison of "what was asked for" against "what was read back". */
    bool JsonValuesEquivalent(const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
    {
        if (!Left.IsValid() || !Right.IsValid())
        {
            return !Left.IsValid() && !Right.IsValid();
        }
        if (Left->Type == EJson::Null || Right->Type == EJson::Null)
        {
            return Left->Type == Right->Type;
        }
        if (Left->Type != Right->Type)
        {
            return false;
        }

        switch (Left->Type)
        {
        case EJson::Number:
            return NumbersEquivalent(Left->AsNumber(), Right->AsNumber());
        case EJson::Boolean:
            return Left->AsBool() == Right->AsBool();
        case EJson::String:
            return Left->AsString() == Right->AsString();
        case EJson::Array:
        {
            const TArray<TSharedPtr<FJsonValue>>& LeftArray = Left->AsArray();
            const TArray<TSharedPtr<FJsonValue>>& RightArray = Right->AsArray();
            if (LeftArray.Num() != RightArray.Num())
            {
                return false;
            }
            for (int32 Index = 0; Index < LeftArray.Num(); ++Index)
            {
                if (!JsonValuesEquivalent(LeftArray[Index], RightArray[Index]))
                {
                    return false;
                }
            }
            return true;
        }
        case EJson::Object:
        {
            const TSharedPtr<FJsonObject>& LeftObject = Left->AsObject();
            const TSharedPtr<FJsonObject>& RightObject = Right->AsObject();
            if (!LeftObject.IsValid() || !RightObject.IsValid())
            {
                return LeftObject == RightObject;
            }

            // Subset semantics: only the keys the caller actually sent are compared. A partial struct
            // write is legitimate (the writer applies exactly the fields it is given), so the readback
            // carrying more keys is expected - comparing the key counts turned a successful struct
            // write into readback_mismatch and left it unsaved.
            //
            // Names are matched with separators and case removed: a caller writes the spelling the
            // read table reports as python_name ("b_fit_to_curve") while a struct readback uses the
            // reflection name ("bFitToCurve") for the same field.
            TMap<FString, TSharedPtr<FJsonValue>> RightByNormalizedName;
            RightByNormalizedName.Reserve(RightObject->Values.Num());
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : RightObject->Values)
            {
                RightByNormalizedName.Add(NormalizedKeyName(Pair.Key), Pair.Value);
            }
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : LeftObject->Values)
            {
                const TSharedPtr<FJsonValue>* Other = RightByNormalizedName.Find(NormalizedKeyName(Pair.Key));
                if (!Other || !JsonValuesEquivalent(Pair.Value, *Other))
                {
                    return false;
                }
            }
            return true;
        }
        default:
            return false;
        }
    }

    /**
     * Readback disagreed with the request: report it instead of a success nobody can trust, and
     * leave the asset unsaved so the state on disk is not the state the caller was told about.
     */
    TSharedPtr<FJsonObject> ReadbackMismatchError(const FString& What, const TSharedPtr<FJsonValue>& Requested,
                                                 const TSharedPtr<FJsonValue>& Readback)
    {
        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("readback_mismatch"),
            FString::Printf(TEXT("%s did not survive the write: the value read back differs from the value requested "
                                 "(the asset was left unsaved on purpose)"), *What));
        if (Requested.IsValid())
        {
            Error->SetField(TEXT("requested_value"), Requested);
        }
        if (Readback.IsValid())
        {
            Error->SetField(TEXT("readback_value"), Readback);
        }
        Error->SetBoolField(TEXT("saved"), false);
        return Error;
    }

    /** Persist the graph asset a write touched (dirty first, then save) and report it as "saved".
        `persist=false` leaves the edit in memory for a caller-driven flush (batch writes). */
    void SaveGraphAsset(UObject* Asset, TSharedPtr<FJsonObject>& Result, bool bPersist = true)
    {
        if (!Asset)
        {
            Result->SetBoolField(TEXT("saved"), false);
            Result->SetBoolField(TEXT("persist_requested"), bPersist);
            return;
        }

        Asset->MarkPackageDirty();
        if (!bPersist)
        {
            Result->SetBoolField(TEXT("saved"), false);
            Result->SetBoolField(TEXT("persist_requested"), false);
            return;
        }

        Result->SetBoolField(TEXT("saved"), FUnrealMCPCommonUtils::SaveAssetForObject(Asset));
        Result->SetBoolField(TEXT("persist_requested"), true);
    }

    /** The labels of one node's pins in the requested direction - the candidates of pin_not_found. */
    TArray<TSharedPtr<FJsonValue>> PinLabelCandidates(const UPCGNode* Node, bool bOutputPin)
    {
        TArray<TSharedPtr<FJsonValue>> Candidates;
        if (!Node)
        {
            return Candidates;
        }
        for (const TObjectPtr<UPCGPin>& Pin : bOutputPin ? Node->GetOutputPins() : Node->GetInputPins())
        {
            if (Pin)
            {
                Candidates.Add(MakeShareable(new FJsonValueString(Pin->Properties.Label.ToString())));
            }
        }
        return Candidates;
    }

    /**
     * Resolve a mesh selector class from its name, its object path, or one of the shorthands the mesh
     * selector UI offers. Candidates come from the loaded UPCGMeshSelectorBase subclasses, so no name
     * list is maintained here.
     */
    UClass* ResolveMeshSelectorClass(const FString& SelectorClass, TSharedPtr<FJsonObject>& OutError)
    {
        static const TMap<FString, FString> Shorthands = {
            { TEXT("weighted"), TEXT("PCGMeshSelectorWeighted") },
            { TEXT("by_attribute"), TEXT("PCGMeshSelectorByAttribute") },
            { TEXT("byattribute"), TEXT("PCGMeshSelectorByAttribute") },
            { TEXT("weighted_by_category"), TEXT("PCGMeshSelectorWeightedByCategory") },
            { TEXT("weightedbycategory"), TEXT("PCGMeshSelectorWeightedByCategory") },
        };

        FString ClassName = SelectorClass;
        const int32 DotIndex = SelectorClass.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
        if (DotIndex != INDEX_NONE)
        {
            ClassName = SelectorClass.RightChop(DotIndex + 1);
        }
        if (const FString* Mapped = Shorthands.Find(ClassName.ToLower()))
        {
            ClassName = *Mapped;
        }

        TArray<UClass*> SelectorClasses;
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Candidate = *It;
            if (Candidate && Candidate->IsChildOf(UPCGMeshSelectorBase::StaticClass()) && !Candidate->HasAnyClassFlags(CLASS_Abstract))
            {
                SelectorClasses.Add(Candidate);
            }
        }

        for (UClass* Candidate : SelectorClasses)
        {
            if (Candidate->GetName().Equals(ClassName, ESearchCase::IgnoreCase))
            {
                return Candidate;
            }
        }

        TArray<TSharedPtr<FJsonValue>> Candidates;
        for (UClass* Candidate : SelectorClasses)
        {
            if (Candidate->GetName().Contains(ClassName, ESearchCase::IgnoreCase) && Candidates.Num() < 40)
            {
                Candidates.Add(MakeShareable(new FJsonValueString(Candidate->GetName())));
            }
        }
        if (Candidates.Num() == 0)
        {
            // Nothing resembles the request, so the useful answer is the list of selector classes that
            // do exist (three of them in 5.5) rather than a single arbitrary name.
            for (UClass* Candidate : SelectorClasses)
            {
                if (Candidates.Num() >= 40)
                {
                    break;
                }
                Candidates.Add(MakeShareable(new FJsonValueString(Candidate->GetName())));
            }
        }

        OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unknown_selector_class"),
            FString::Printf(TEXT("'%s' is not a loaded UPCGMeshSelectorBase subclass (shorthands: weighted, by_attribute, weighted_by_category)"),
                *SelectorClass));
        if (Candidates.Num() > 0)
        {
            OutError->SetArrayField(TEXT("candidates"), Candidates);
        }
        return nullptr;
    }

    /**
     * The component fields every PCG component readback shares, so list_pcg_components and
     * set_pcg_component_graph cannot drift apart.
     *
     * Returns whether the component already holds generated output. The two callers name that field
     * differently on purpose - the read command reports `generated`, the write commands report
     * `generated_output_available` - but the value is computed here once.
     */
    bool SetComponentStateFields(const UPCGComponent* Component, TSharedPtr<FJsonObject>& Item)
    {
        const UPCGGraph* BoundGraph = Component ? Component->GetGraph() : nullptr;
        Item->SetStringField(TEXT("graph_path"), BoundGraph ? BoundGraph->GetPathName() : FString());
        if (Component)
        {
            if (const UPCGGraphInstance* Instance = Component->GetGraphInstance())
            {
                Item->SetStringField(TEXT("graph_instance_path"), Instance->GetPathName());
            }
        }

        Item->SetStringField(TEXT("generation_trigger"),
            Component ? GenerationTriggerName(Component->GenerationTrigger) : FString());
        Item->SetBoolField(TEXT("active"), Component && Component->IsActive());
        Item->SetBoolField(TEXT("generating"), Component && Component->IsGenerating());
        return Component && Component->GetGeneratedGraphOutput().TaggedData.Num() > 0;
    }
}

FUnrealMCPPCGCommands::FUnrealMCPPCGCommands()
{
}

void FUnrealMCPPCGCommands::RegisterCommands(FMCPCommandRegistry& Registry)
{
    MCP_REGISTER_COMMAND(Registry, "list_pcg_assets", "pcg",
        "List PCG assets (PCGGraph / PCGGraphInstance / PCGDataAsset) under a folder, from the asset registry.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("folder"), TEXT("string"), TEXT("Content folder to search (default /Game)")),
            MCPParamOpt(TEXT("class_filter"), TEXT("string"), TEXT("Only this class: PCGGraph / PCGGraphInstance / PCGDataAsset")),
            MCPParamOpt(TEXT("recursive"), TEXT("bool"), TEXT("Include sub folders (default true)")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleListPCGAssets(Params); });

    MCP_REGISTER_COMMAND(Registry, "get_pcg_graph", "pcg",
        "Dump a PCG graph: nodes, their pin labels/connections, the edge list and the graph input/output nodes.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("PCG graph asset path")),
            MCPParamOpt(TEXT("detail"), TEXT("string"), TEXT("summary (default) or full - full also returns each node's reflected property table")),
            MCPParamOpt(TEXT("max_nodes"), TEXT("int"), TEXT("Return at most this many nodes (node_count stays the real total)")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleGetPCGGraph(Params); });

    MCP_REGISTER_COMMAND(Registry, "get_pcg_node", "pcg",
        "Reflect one PCG node's settings: every property with its display label, python name, type, value and writability.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("PCG graph asset path")),
            MCPParamOpt(TEXT("node_name"), TEXT("string"), TEXT("Node object name (from get_pcg_graph)")),
            MCPParamOpt(TEXT("node_index"), TEXT("int"), TEXT("Node index in the graph, used when node_name is absent")),
            MCPParamOpt(TEXT("max_properties"), TEXT("int"), TEXT("Return at most this many properties")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleGetPCGNode(Params); });

    MCP_REGISTER_COMMAND(Registry, "list_pcg_components", "pcg",
        "List the PCG components in the current editor world: graph, trigger, bounds and the instance batches left by generation.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("actor_label"), TEXT("string"), TEXT("Only components whose actor label contains this text")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleListPCGComponents(Params); });

    MCP_REGISTER_COMMAND(Registry, "get_pcg_generated_output", "pcg",
        "Read a PCG component's existing generation result: tagged data, point counts, available attributes and per-attribute stats.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("actor_label"), TEXT("string"), TEXT("Owning actor label (or a unique substring of it)")),
            MCPParamOpt(TEXT("component_name"), TEXT("string"), TEXT("PCG component object name")),
            MCPParamOpt(TEXT("attribute"), TEXT("string"), TEXT("Attribute to summarise (min/max/mean/channels/samples)")),
            MCPParamOpt(TEXT("max_samples"), TEXT("int"), TEXT("Sample values to return, 1..64 (default 8)")),
            MCPParamOpt(TEXT("include_attributes"), TEXT("bool"), TEXT("List the available attribute names (default true)")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleGetPCGGeneratedOutput(Params); });

    MCP_REGISTER_COMMAND(Registry, "add_pcg_node", "pcg",
        "Add a PCG node to a graph by settings class, and report its name, pins and position (writes the asset).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("PCG graph asset path")),
            MCPParam(TEXT("node_class"), TEXT("string"), TEXT("Settings class short name or object path, e.g. PCGCreatePointsSphereSettings")),
            MCPParamOpt(TEXT("editor_x"), TEXT("int"), TEXT("Node position X in the graph editor")),
            MCPParamOpt(TEXT("editor_y"), TEXT("int"), TEXT("Node position Y in the graph editor")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleAddPCGNode(Params); });

    MCP_REGISTER_COMMAND(Registry, "connect_pcg_pins", "pcg",
        "Connect two PCG pins by display label and read the connection back (a wrong label is silent in the engine).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("PCG graph asset path")),
            MCPParam(TEXT("from_node"), TEXT("string"), TEXT("Upstream node object name")),
            MCPParam(TEXT("from_label"), TEXT("string"), TEXT("Upstream pin display label (from get_pcg_graph)")),
            MCPParam(TEXT("to_node"), TEXT("string"), TEXT("Downstream node object name")),
            MCPParam(TEXT("to_label"), TEXT("string"), TEXT("Downstream pin display label (from get_pcg_graph)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleConnectPCGPins(Params); });

    MCP_REGISTER_COMMAND(Registry, "disconnect_pcg_pins", "pcg",
        "Remove one PCG edge by its two endpoints and read the pin's remaining edges back.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("PCG graph asset path")),
            MCPParam(TEXT("from_node"), TEXT("string"), TEXT("Upstream node object name")),
            MCPParam(TEXT("from_label"), TEXT("string"), TEXT("Upstream pin display label")),
            MCPParam(TEXT("to_node"), TEXT("string"), TEXT("Downstream node object name")),
            MCPParam(TEXT("to_label"), TEXT("string"), TEXT("Downstream pin display label")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleDisconnectPCGPins(Params); });

    MCP_REGISTER_COMMAND(Registry, "remove_pcg_node", "pcg",
        "Remove one PCG node from a graph (its edges go with it - nothing is rewired automatically).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("PCG graph asset path")),
            MCPParam(TEXT("node_name"), TEXT("string"), TEXT("Node object name (from get_pcg_graph)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleRemovePCGNode(Params); });

    MCP_REGISTER_COMMAND(Registry, "set_pcg_node_property", "pcg",
        "Write one PCG settings property (reflection name or snake_case) through the shared property writer, then read it back.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("PCG graph asset path")),
            MCPParamOpt(TEXT("node_name"), TEXT("string"), TEXT("Node object name (from get_pcg_graph)")),
            MCPParamOpt(TEXT("node_index"), TEXT("int"), TEXT("Node index in the graph, used when node_name is absent")),
            MCPParam(TEXT("property_name"), TEXT("string"), TEXT("Property name, reflection name (LowerBound) or snake_case (lower_bound)")),
            MCPParam(TEXT("value"), TEXT("any"), TEXT("Value in the shape get_pcg_node reports (number / bool / string / enum member / array / object)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleSetPCGNodeProperty(Params); });

    MCP_REGISTER_COMMAND(Registry, "generate_pcg_component", "pcg",
        "Dispatch generation for one PCG component and report the component state (generation is asynchronous).",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("actor_label"), TEXT("string"), TEXT("Owning actor label (or a unique substring of it)")),
            MCPParamOpt(TEXT("component_name"), TEXT("string"), TEXT("PCG component object name")),
            MCPParamOpt(TEXT("force"), TEXT("bool"), TEXT("Force a regeneration even when the component is up to date")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleGeneratePCGComponent(Params); });

    MCP_REGISTER_COMMAND(Registry, "cleanup_pcg_component", "pcg",
        "Dispatch cleanup for one PCG component (removing its generated output), without waiting for it.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("actor_label"), TEXT("string"), TEXT("Owning actor label (or a unique substring of it)")),
            MCPParamOpt(TEXT("component_name"), TEXT("string"), TEXT("PCG component object name")),
            MCPParamOpt(TEXT("remove_components"), TEXT("bool"), TEXT("Remove the generated components too (default true)")),
            MCPParamOpt(TEXT("save_generated_components"), TEXT("bool"), TEXT("Save the generated components while cleaning up (default false)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleCleanupPCGComponent(Params); });

    MCP_REGISTER_COMMAND(Registry, "create_pcg_graph", "pcg",
        "Create a PCG graph asset (writes it to disk; never overwrites an existing asset) and read its structure back.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path, folder and name, e.g. /Game/MCP/_PCGProbe/PCG_New")),
            MCPParamOpt(TEXT("folder"), TEXT("string"), TEXT("Folder to use when asset_path carries the name only")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleCreatePCGGraph(Params); });

    MCP_REGISTER_COMMAND(Registry, "set_pcg_component_graph", "pcg",
        "Bind a PCGGraph / PCGGraphInstance asset to a PCG component in the level and read the binding back.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("actor_label"), TEXT("string"), TEXT("Owning actor label (or a unique substring of it)")),
            MCPParamOpt(TEXT("component_name"), TEXT("string"), TEXT("PCG component object name")),
            MCPParam(TEXT("graph_path"), TEXT("string"), TEXT("PCGGraph or PCGGraphInstance asset path")),
            MCPParamOpt(TEXT("active"), TEXT("bool"), TEXT("Also set the component's active state")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleSetPCGComponentGraph(Params); });

    MCP_REGISTER_COMMAND(Registry, "set_pcg_mesh_selector_type", "pcg",
        "Set the mesh selector class on a static mesh spawner node (this is what instantiates the selector).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("PCG graph asset path")),
            MCPParamOpt(TEXT("node_name"), TEXT("string"), TEXT("Node object name (from get_pcg_graph)")),
            MCPParamOpt(TEXT("node_index"), TEXT("int"), TEXT("Node index in the graph, used when node_name is absent")),
            MCPParam(TEXT("selector_class"), TEXT("string"), TEXT("Selector class name/path or shorthand: weighted / by_attribute / weighted_by_category")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleSetPCGMeshSelectorType(Params); });

    MCP_REGISTER_COMMAND(Registry, "set_pcg_mesh_selector_entries", "pcg",
        "Replace the weighted mesh selector's mesh table (integer weights, static meshes, optional material overrides).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("PCG graph asset path")),
            MCPParamOpt(TEXT("node_name"), TEXT("string"), TEXT("Node object name (from get_pcg_graph)")),
            MCPParamOpt(TEXT("node_index"), TEXT("int"), TEXT("Node index in the graph, used when node_name is absent")),
            MCPParam(TEXT("entries"), TEXT("array"), TEXT("Entries: [{static_mesh, weight (int), override_materials?}] - replaces the whole table")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleSetPCGMeshSelectorEntries(Params); });
}

UPCGGraph* FUnrealMCPPCGCommands::ResolveGraph(const FString& AssetPath, TSharedPtr<FJsonObject>& OutError)
{
    UObject* Asset = FUnrealMCPCommonUtils::FindAsset(AssetPath);
    if (!Asset)
    {
        OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("asset_not_found"),
            FString::Printf(TEXT("No asset at '%s' (accepts '/Game/Dir/Asset', '/Game/Dir/Asset.Asset' or a short name)"), *AssetPath));
        return nullptr;
    }

    if (UPCGGraph* Graph = Cast<UPCGGraph>(Asset))
    {
        return Graph;
    }

    // A graph instance is a valid PCG asset but not a graph: say so instead of "asset_not_found".
    OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("not_a_pcg_graph"),
        FString::Printf(TEXT("'%s' is a %s, not a PCGGraph"), *AssetPath, *Asset->GetClass()->GetName()));
    return nullptr;
}

UPCGNode* FUnrealMCPPCGCommands::ResolveNode(UPCGGraph* Graph, const FString& NodeName, int32 NodeIndex,
                                             TSharedPtr<FJsonObject>& OutError)
{
    const TArray<UPCGNode*>& Nodes = Graph->GetNodes();

    if (!NodeName.IsEmpty())
    {
        for (UPCGNode* Node : Nodes)
        {
            if (Node && Node->GetName() == NodeName)
            {
                return Node;
            }
        }

        // The graph's input/output nodes live outside GetNodes(), so they have to be named
        // explicitly - and they must be reachable: connecting the graph output node is not optional
        // (an unconnected output node generates nothing), and removing it has to be refused with a
        // reason rather than "no such node".
        UPCGNode* InputNode = Graph->GetInputNode();
        if (InputNode && InputNode->GetName() == NodeName)
        {
            return InputNode;
        }
        UPCGNode* OutputNode = Graph->GetOutputNode();
        if (OutputNode && OutputNode->GetName() == NodeName)
        {
            return OutputNode;
        }
    }
    else if (Nodes.IsValidIndex(NodeIndex))
    {
        return Nodes[NodeIndex];
    }

    TArray<TSharedPtr<FJsonValue>> Candidates;
    for (UPCGNode* Node : Nodes)
    {
        if (Node)
        {
            Candidates.Add(MakeShareable(new FJsonValueString(Node->GetName())));
        }
    }
    if (Graph->GetInputNode())
    {
        Candidates.Add(MakeShareable(new FJsonValueString(Graph->GetInputNode()->GetName())));
    }
    if (Graph->GetOutputNode())
    {
        Candidates.Add(MakeShareable(new FJsonValueString(Graph->GetOutputNode()->GetName())));
    }

    OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("node_not_found"),
        NodeName.IsEmpty()
            ? FString::Printf(TEXT("node_index %d is out of range (%d nodes)"), NodeIndex, Nodes.Num())
            : FString::Printf(TEXT("No node named '%s' in this graph"), *NodeName));
    OutError->SetArrayField(TEXT("candidates"), Candidates);
    return nullptr;
}

UPCGPin* FUnrealMCPPCGCommands::ResolvePin(UPCGNode* Node, const FString& Label, bool bOutputPin,
                                           TSharedPtr<FJsonObject>& OutError) const
{
    if (Node && !Label.IsEmpty())
    {
        // Both spellings are accepted on purpose: the display label is what the graph shows and what
        // the engine's connect API takes, the snake_case form is what property callers write.
        for (const TObjectPtr<UPCGPin>& Pin : bOutputPin ? Node->GetOutputPins() : Node->GetInputPins())
        {
            if (!Pin)
            {
                continue;
            }
            const FString PinLabel = Pin->Properties.Label.ToString();
            if (PinLabel.Equals(Label, ESearchCase::IgnoreCase) || PythonStyleName(PinLabel).Equals(Label, ESearchCase::IgnoreCase))
            {
                return Pin;
            }
        }
    }

    OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("pin_not_found"),
        FString::Printf(TEXT("Node '%s' has no %s pin labelled '%s'"),
            Node ? *Node->GetName() : TEXT("<unknown>"), bOutputPin ? TEXT("output") : TEXT("input"), *Label));
    OutError->SetArrayField(TEXT("candidates"), PinLabelCandidates(Node, bOutputPin));
    return nullptr;
}

UClass* FUnrealMCPPCGCommands::ResolveSettingsClass(const FString& NodeClass, TSharedPtr<FJsonObject>& OutError) const
{
    // Callers write either the class name ("PCGCreatePointsSphereSettings") or an object path
    // ("/Script/PCG.PCGCreatePointsSphereSettings"). Any of those spellings end in the class name.
    FString ClassName = NodeClass;
    const int32 DotIndex = NodeClass.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    if (DotIndex != INDEX_NONE)
    {
        ClassName = NodeClass.RightChop(DotIndex + 1);
    }
    if (ClassName.EndsWith(TEXT("_C")))
    {
        ClassName.LeftChopInline(2);
    }

    // Every loaded UPCGSettings subclass is a candidate node type. The class objects exist as soon as
    // the PCG module is loaded, so no name list has to be maintained here.
    TArray<UClass*> SettingsClasses;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Candidate = *It;
        if (Candidate && Candidate->IsChildOf(UPCGSettings::StaticClass()) && !Candidate->HasAnyClassFlags(CLASS_Abstract))
        {
            SettingsClasses.Add(Candidate);
        }
    }

    for (UClass* Candidate : SettingsClasses)
    {
        if (Candidate->GetName().Equals(ClassName, ESearchCase::IgnoreCase))
        {
            return Candidate;
        }
    }

    TArray<TSharedPtr<FJsonValue>> Candidates;
    for (UClass* Candidate : SettingsClasses)
    {
        if (Candidate->GetName().Contains(ClassName, ESearchCase::IgnoreCase) && Candidates.Num() < 40)
        {
            Candidates.Add(MakeShareable(new FJsonValueString(Candidate->GetName())));
        }
    }
    if (Candidates.Num() == 0)
    {
        // Nothing resembles the request, so the useful answer is what does exist rather than an
        // empty candidate list.
        for (UClass* Candidate : SettingsClasses)
        {
            if (Candidates.Num() >= 40)
            {
                break;
            }
            Candidates.Add(MakeShareable(new FJsonValueString(Candidate->GetName())));
        }
    }

    OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unknown_node_class"),
        FString::Printf(TEXT("'%s' is not a loaded UPCGSettings subclass (%d settings classes are loaded)"),
            *NodeClass, SettingsClasses.Num()));
    if (Candidates.Num() > 0)
    {
        OutError->SetArrayField(TEXT("candidates"), Candidates);
    }
    return nullptr;
}

UPCGComponent* FUnrealMCPPCGCommands::FindPCGComponent(const FString& ActorLabel, const FString& ComponentName,
                                                       AActor** OutActor, TSharedPtr<FJsonObject>& OutError) const
{
    if (OutActor)
    {
        *OutActor = nullptr;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("no_editor_world"),
            TEXT("No editor world is available (the level editor is not open)"));
        return nullptr;
    }

    UPCGComponent* Found = nullptr;
    AActor* FoundActor = nullptr;
    TArray<TSharedPtr<FJsonValue>> Candidates;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (!Actor)
        {
            continue;
        }

        TArray<UPCGComponent*> Components;
        Actor->GetComponents<UPCGComponent>(Components);
        for (UPCGComponent* Component : Components)
        {
            if (!Component)
            {
                continue;
            }

            Candidates.Add(MakeShareable(new FJsonValueString(
                FString::Printf(TEXT("%s / %s"), *Actor->GetActorLabel(), *Component->GetName()))));

            if (Found)
            {
                continue;
            }

            const bool bNameMatch = !ComponentName.IsEmpty() && Component->GetName() == ComponentName;
            const bool bLabelMatch = !ActorLabel.IsEmpty() && Actor->GetActorLabel().Contains(ActorLabel);
            if (bNameMatch || bLabelMatch)
            {
                Found = Component;
                FoundActor = Actor;
            }
        }
    }

    if (!Found)
    {
        OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("component_not_found"),
            FString::Printf(TEXT("No PCG component matches actor_label '%s' / component_name '%s'"),
                *ActorLabel, *ComponentName));
        OutError->SetArrayField(TEXT("candidates"), Candidates);
        return nullptr;
    }

    if (OutActor)
    {
        *OutActor = FoundActor;
    }
    return Found;
}

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::PinToJson(const UPCGPin* Pin) const
{
    TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject);
    const FString Label = Pin->Properties.Label.ToString();
    // Two spellings on purpose: the label is what AddEdge/RemoveEdge take, the python-style name
    // is what the property layer spells the same pin with.
    Json->SetStringField(TEXT("label"), Label);
    Json->SetStringField(TEXT("python_name"), PythonStyleName(Label));
    Json->SetBoolField(TEXT("connected"), Pin->IsConnected());
    Json->SetBoolField(TEXT("is_output"), Pin->IsOutputPin());
    Json->SetBoolField(TEXT("allow_multiple_data"), Pin->Properties.bAllowMultipleData);
    Json->SetNumberField(TEXT("edge_count"), Pin->Edges.Num());

    TArray<TSharedPtr<FJsonValue>> EdgesTo;
    for (const TObjectPtr<UPCGEdge>& Edge : Pin->Edges)
    {
        const UPCGPin* Other = Edge ? Edge->GetOtherPin(Pin) : nullptr;
        if (Other && Other->Node)
        {
            EdgesTo.Add(MakeShareable(new FJsonValueString(Other->Node->GetName())));
        }
    }
    Json->SetArrayField(TEXT("edges_to"), EdgesTo);
    return Json;
}

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::NodeToJson(const UPCGNode* Node, bool bIncludeProperties,
                                                          int32 MaxProperties) const
{
    TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject);
    Json->SetStringField(TEXT("name"), Node->GetName());
    Json->SetBoolField(TEXT("is_settings_instance"), Node->IsInstance());

    UPCGSettings* Settings = Node->GetSettings();
    Json->SetStringField(TEXT("settings_class"), Settings ? Settings->GetClass()->GetName() : FString());
    Json->SetStringField(TEXT("settings_path"), Settings ? Settings->GetPathName() : FString());

#if WITH_EDITOR
    int32 PositionX = 0;
    int32 PositionY = 0;
    Node->GetNodePosition(PositionX, PositionY);
    Json->SetNumberField(TEXT("editor_x"), PositionX);
    Json->SetNumberField(TEXT("editor_y"), PositionY);
#endif

    TArray<TSharedPtr<FJsonValue>> InputPins;
    for (const TObjectPtr<UPCGPin>& Pin : Node->GetInputPins())
    {
        if (Pin)
        {
            InputPins.Add(MakeShareable(new FJsonValueObject(PinToJson(Pin))));
        }
    }
    // An empty array here is a fact, not a failure: PCGDebugSettings has no output pins at all and
    // the graph input/output nodes use "In" / "Out" as their single label.
    TArray<TSharedPtr<FJsonValue>> OutputPins;
    for (const TObjectPtr<UPCGPin>& Pin : Node->GetOutputPins())
    {
        if (Pin)
        {
            OutputPins.Add(MakeShareable(new FJsonValueObject(PinToJson(Pin))));
        }
    }
    Json->SetArrayField(TEXT("input_pins"), InputPins);
    Json->SetArrayField(TEXT("output_pins"), OutputPins);

    if (bIncludeProperties && Settings)
    {
        const TSharedPtr<FJsonObject> Table = BuildPropertyTable(Settings, MaxProperties);
        Json->SetArrayField(TEXT("properties"), Table->GetArrayField(TEXT("properties")));
        Json->SetNumberField(TEXT("property_count"), Table->GetNumberField(TEXT("property_count")));
        Json->SetNumberField(TEXT("writable_count"), Table->GetNumberField(TEXT("writable_count")));
        Json->SetNumberField(TEXT("readonly_instanced_count"), Table->GetNumberField(TEXT("readonly_instanced_count")));
    }
    return Json;
}

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::HandleListPCGAssets(const TSharedPtr<FJsonObject>& Params)
{
    FString Folder = TEXT("/Game");
    Params->TryGetStringField(TEXT("folder"), Folder);
    FString ClassFilter;
    Params->TryGetStringField(TEXT("class_filter"), ClassFilter);
    bool bRecursive = true;
    Params->TryGetBoolField(TEXT("recursive"), bRecursive);

    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    if (!AssetRegistry.PathExists(FName(*Folder)))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("folder_not_found"),
            FString::Printf(TEXT("No content folder '%s' in the asset registry"), *Folder));
    }

    // Asset registry queries only, no asset loading: listing must stay free of side effects.
    // The map is only a validity list for class_filter here (the actual filtering happens on the
    // returned asset data, see below).
    static const TSet<FString> SupportedClasses = {
        TEXT("PCGGraph"), TEXT("PCGGraphInstance"), TEXT("PCGDataAsset"),
    };

    if (!ClassFilter.IsEmpty() && !SupportedClasses.Contains(ClassFilter))
    {
        TArray<FString> Supported = SupportedClasses.Array();
        Supported.Sort();
        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unknown_class_filter"),
            FString::Printf(TEXT("'%s' is not a supported PCG class"), *ClassFilter));
        TArray<TSharedPtr<FJsonValue>> Candidates;
        for (const FString& Name : Supported)
        {
            Candidates.Add(MakeShareable(new FJsonValueString(Name)));
        }
        Error->SetArrayField(TEXT("candidates"), Candidates);
        return Error;
    }

    // Path query + class filter applied here: an FARFilter built from FTopLevelAssetPath(PCG,
    // <class>) matched nothing for PCG assets on this engine version, while the registry does list
    // them (verified: 2 PCGGraph assets, count 0 returned). Filtering by AssetClassPath name avoids
    // depending on that filter form entirely.
    TArray<FAssetData> Assets;
    AssetRegistry.GetAssetsByPath(FName(*Folder), Assets, bRecursive);

    TArray<TSharedPtr<FJsonValue>> AssetsJson;
    for (const FAssetData& AssetData : Assets)
    {
        const FString ClassName = AssetData.AssetClassPath.GetAssetName().ToString();
        if (ClassFilter.IsEmpty())
        {
            if (!SupportedClasses.Contains(ClassName))
            {
                continue;
            }
        }
        else if (ClassName != ClassFilter)
        {
            continue;
        }

        TSharedPtr<FJsonObject> Item = MakeShareable(new FJsonObject);
        Item->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
        Item->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
        Item->SetStringField(TEXT("class"), ClassName);
        Item->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
        AssetsJson.Add(MakeShareable(new FJsonValueObject(Item)));
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("folder"), Folder);
    Result->SetBoolField(TEXT("recursive"), bRecursive);
    Result->SetArrayField(TEXT("assets"), AssetsJson);
    Result->SetNumberField(TEXT("count"), AssetsJson.Num());
    if (!ClassFilter.IsEmpty())
    {
        Result->SetStringField(TEXT("class_filter"), ClassFilter);
    }
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::HandleGetPCGGraph(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' parameter"));
    }

    TSharedPtr<FJsonObject> Error;
    UPCGGraph* Graph = ResolveGraph(AssetPath, Error);
    if (!Graph)
    {
        return Error;
    }

    FString Detail = TEXT("summary");
    Params->TryGetStringField(TEXT("detail"), Detail);
    const bool bFull = Detail.Equals(TEXT("full"), ESearchCase::IgnoreCase);

    int32 MaxNodes = 0;
    if (double MaxNodesValue = 0.0; Params->TryGetNumberField(TEXT("max_nodes"), MaxNodesValue))
    {
        MaxNodes = static_cast<int32>(MaxNodesValue);
    }

    const TArray<UPCGNode*>& Nodes = Graph->GetNodes();
    TArray<TSharedPtr<FJsonValue>> NodesJson;
    TArray<TSharedPtr<FJsonValue>> EdgesJson;
    int32 Returned = 0;
    for (UPCGNode* Node : Nodes)
    {
        if (!Node)
        {
            continue;
        }
        if (MaxNodes > 0 && Returned >= MaxNodes)
        {
            break;
        }
        NodesJson.Add(MakeShareable(new FJsonValueObject(NodeToJson(Node, bFull, 0))));
        ++Returned;

        // One edge per output pin per connection, emitted from the upstream side so the list has
        // no duplicates and reads in evaluation order.
        for (const TObjectPtr<UPCGPin>& Pin : Node->GetOutputPins())
        {
            if (!Pin)
            {
                continue;
            }
            for (const TObjectPtr<UPCGEdge>& Edge : Pin->Edges)
            {
                const UPCGPin* Other = Edge ? Edge->GetOtherPin(Pin) : nullptr;
                if (!Other || !Other->Node)
                {
                    continue;
                }
                TSharedPtr<FJsonObject> EdgeJson = MakeShareable(new FJsonObject);
                EdgeJson->SetStringField(TEXT("from_node"), Node->GetName());
                EdgeJson->SetStringField(TEXT("from_label"), Pin->Properties.Label.ToString());
                EdgeJson->SetStringField(TEXT("to_node"), Other->Node->GetName());
                EdgeJson->SetStringField(TEXT("to_label"), Other->Properties.Label.ToString());
                EdgesJson.Add(MakeShareable(new FJsonValueObject(EdgeJson)));
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("asset_path"), Graph->GetPathName());
    Result->SetStringField(TEXT("graph_class"), Graph->GetClass()->GetName());
    Result->SetStringField(TEXT("detail"), bFull ? TEXT("full") : TEXT("summary"));
    Result->SetNumberField(TEXT("node_count"), Nodes.Num());
    Result->SetArrayField(TEXT("nodes"), NodesJson);
    Result->SetArrayField(TEXT("edges"), EdgesJson);
    Result->SetBoolField(TEXT("truncated"), Returned < Nodes.Num());

    if (const UPCGNode* InputNode = Graph->GetInputNode())
    {
        Result->SetObjectField(TEXT("input_node"), NodeToJson(InputNode, false, 0));
    }
    if (const UPCGNode* OutputNode = Graph->GetOutputNode())
    {
        Result->SetObjectField(TEXT("output_node"), NodeToJson(OutputNode, false, 0));
    }
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::HandleGetPCGNode(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' parameter"));
    }

    TSharedPtr<FJsonObject> Error;
    UPCGGraph* Graph = ResolveGraph(AssetPath, Error);
    if (!Graph)
    {
        return Error;
    }

    FString NodeName;
    Params->TryGetStringField(TEXT("node_name"), NodeName);
    int32 NodeIndex = 0;
    if (double NodeIndexValue = 0.0; Params->TryGetNumberField(TEXT("node_index"), NodeIndexValue))
    {
        NodeIndex = static_cast<int32>(NodeIndexValue);
    }
    int32 MaxProperties = 0;
    if (double MaxPropertiesValue = 0.0; Params->TryGetNumberField(TEXT("max_properties"), MaxPropertiesValue))
    {
        MaxProperties = static_cast<int32>(MaxPropertiesValue);
    }

    UPCGNode* Node = ResolveNode(Graph, NodeName, NodeIndex, Error);
    if (!Node)
    {
        return Error;
    }

    UPCGSettings* Settings = Node->GetSettings();
    if (!Settings)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("node_has_no_settings"),
            FString::Printf(TEXT("Node '%s' holds no settings object to reflect"), *Node->GetName()));
    }

    const TSharedPtr<FJsonObject> Table = BuildPropertyTable(Settings, MaxProperties);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("asset_path"), Graph->GetPathName());
    Result->SetStringField(TEXT("node_name"), Node->GetName());
    Result->SetStringField(TEXT("settings_class"), Settings->GetClass()->GetName());
    Result->SetStringField(TEXT("settings_path"), Settings->GetPathName());
    Result->SetArrayField(TEXT("properties"), Table->GetArrayField(TEXT("properties")));
    Result->SetNumberField(TEXT("property_count"), Table->GetNumberField(TEXT("property_count")));
    Result->SetNumberField(TEXT("writable_count"), Table->GetNumberField(TEXT("writable_count")));
    Result->SetNumberField(TEXT("readonly_instanced_count"), Table->GetNumberField(TEXT("readonly_instanced_count")));
    Result->SetBoolField(TEXT("truncated"), Table->GetBoolField(TEXT("truncated")));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::HandleListPCGComponents(const TSharedPtr<FJsonObject>& Params)
{
    FString LabelFilter;
    Params->TryGetStringField(TEXT("actor_label"), LabelFilter);

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("no_editor_world"),
            TEXT("No editor world is available (the level editor is not open)"));
    }

    TArray<TSharedPtr<FJsonValue>> ComponentsJson;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (!Actor)
        {
            continue;
        }

        TArray<UPCGComponent*> Components;
        Actor->GetComponents<UPCGComponent>(Components);
        if (Components.Num() == 0)
        {
            continue;
        }

        const FString ActorLabel = Actor->GetActorLabel();
        if (!LabelFilter.IsEmpty() && !ActorLabel.Contains(LabelFilter))
        {
            continue;
        }

        FVector Origin = FVector::ZeroVector;
        FVector Extent = FVector::ZeroVector;
        Actor->GetActorBounds(/*bOnlyCollidingComponents=*/ false, Origin, Extent);

        for (UPCGComponent* Component : Components)
        {
            if (!Component)
            {
                continue;
            }

            TSharedPtr<FJsonObject> Item = MakeShareable(new FJsonObject);
            Item->SetStringField(TEXT("actor_label"), ActorLabel);
            Item->SetStringField(TEXT("actor_class"), Actor->GetClass()->GetName());
            Item->SetStringField(TEXT("component_name"), Component->GetName());

            // Bound graph, graph instance and component state come from the shared readback, so this
            // command and set_pcg_component_graph always agree on them.
            Item->SetBoolField(TEXT("generated"), SetComponentStateFields(Component, Item));

            TSharedPtr<FJsonObject> BoundsMin = MakeShareable(new FJsonObject);
            BoundsMin->SetNumberField(TEXT("x"), Origin.X - Extent.X);
            BoundsMin->SetNumberField(TEXT("y"), Origin.Y - Extent.Y);
            BoundsMin->SetNumberField(TEXT("z"), Origin.Z - Extent.Z);
            TSharedPtr<FJsonObject> BoundsMax = MakeShareable(new FJsonObject);
            BoundsMax->SetNumberField(TEXT("x"), Origin.X + Extent.X);
            BoundsMax->SetNumberField(TEXT("y"), Origin.Y + Extent.Y);
            BoundsMax->SetNumberField(TEXT("z"), Origin.Z + Extent.Z);
            Item->SetObjectField(TEXT("bounds_min"), BoundsMin);
            Item->SetObjectField(TEXT("bounds_max"), BoundsMax);

            // Managed resources are private engine state (UPCGComponent::GetManagedResources is not
            // public). Report what is publicly observable instead: the instance batches the
            // generation left on the owner actor, and how many tagged data it produced.
            TArray<UInstancedStaticMeshComponent*> InstancedComponents;
            Actor->GetComponents<UInstancedStaticMeshComponent>(InstancedComponents);
            TArray<TSharedPtr<FJsonValue>> BatchJson;
            int32 TotalInstances = 0;
            for (UInstancedStaticMeshComponent* InstancedComponent : InstancedComponents)
            {
                if (!InstancedComponent)
                {
                    continue;
                }
                const int32 InstanceCount = InstancedComponent->GetInstanceCount();
                TotalInstances += InstanceCount;

                TSharedPtr<FJsonObject> Batch = MakeShareable(new FJsonObject);
                Batch->SetStringField(TEXT("component_name"), InstancedComponent->GetName());
                const UStaticMesh* Mesh = InstancedComponent->GetStaticMesh();
                Batch->SetStringField(TEXT("mesh"), Mesh ? Mesh->GetName() : FString());
                Batch->SetNumberField(TEXT("instances"), InstanceCount);
                BatchJson.Add(MakeShareable(new FJsonValueObject(Batch)));
            }
            Item->SetNumberField(TEXT("instanced_mesh_components"), InstancedComponents.Num());
            Item->SetNumberField(TEXT("instanced_mesh_instances"), TotalInstances);
            Item->SetArrayField(TEXT("instance_batches"), BatchJson);

            ComponentsJson.Add(MakeShareable(new FJsonValueObject(Item)));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("world_name"), World->GetName());
    Result->SetArrayField(TEXT("components"), ComponentsJson);
    Result->SetNumberField(TEXT("count"), ComponentsJson.Num());
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::HandleGetPCGGeneratedOutput(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorLabel;
    Params->TryGetStringField(TEXT("actor_label"), ActorLabel);
    FString ComponentName;
    Params->TryGetStringField(TEXT("component_name"), ComponentName);
    FString AttributeName;
    Params->TryGetStringField(TEXT("attribute"), AttributeName);
    bool bIncludeAttributes = true;
    Params->TryGetBoolField(TEXT("include_attributes"), bIncludeAttributes);

    int32 MaxSamples = 8;
    if (double MaxSamplesValue = 0.0; Params->TryGetNumberField(TEXT("max_samples"), MaxSamplesValue))
    {
        MaxSamples = FMath::Clamp(static_cast<int32>(MaxSamplesValue), 1, 64);
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("no_editor_world"),
            TEXT("No editor world is available (the level editor is not open)"));
    }

    UPCGComponent* Found = nullptr;
    AActor* FoundActor = nullptr;
    TArray<TSharedPtr<FJsonValue>> Candidates;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (!Actor)
        {
            continue;
        }
        TArray<UPCGComponent*> Components;
        Actor->GetComponents<UPCGComponent>(Components);
        for (UPCGComponent* Component : Components)
        {
            if (!Component)
            {
                continue;
            }
            Candidates.Add(MakeShareable(new FJsonValueString(
                FString::Printf(TEXT("%s / %s"), *Actor->GetActorLabel(), *Component->GetName()))));

            if (Found)
            {
                continue;
            }
            const bool bNameMatch = !ComponentName.IsEmpty() && Component->GetName() == ComponentName;
            const bool bLabelMatch = !ActorLabel.IsEmpty() && Actor->GetActorLabel().Contains(ActorLabel);
            if (bNameMatch || bLabelMatch)
            {
                Found = Component;
                FoundActor = Actor;
            }
        }
    }

    if (!Found)
    {
        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("component_not_found"),
            FString::Printf(TEXT("No PCG component matches actor_label '%s' / component_name '%s'"),
                *ActorLabel, *ComponentName));
        Error->SetArrayField(TEXT("candidates"), Candidates);
        return Error;
    }

    // Read-only by construction: the existing result is inspected, never (re)generated.
    const FPCGDataCollection& Collection = Found->GetGeneratedGraphOutput();
    if (Collection.TaggedData.Num() == 0)
    {
        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("not_generated"),
            FString::Printf(TEXT("'%s' has no generation result yet - generate it first (this command never generates)"),
                *Found->GetName()));
        Error->SetStringField(TEXT("actor_label"), FoundActor ? FoundActor->GetActorLabel() : FString());
        return Error;
    }

    TArray<TSharedPtr<FJsonValue>> TaggedJson;
    TArray<FPCGPoint> AllPoints;
    TMap<FString, EPCGMetadataTypes> AttributeTypes;
    const UPCGMetadata* PointMetadata = nullptr;
    int32 TotalPoints = 0;

    for (const FPCGTaggedData& Tagged : Collection.TaggedData)
    {
        TSharedPtr<FJsonObject> Item = MakeShareable(new FJsonObject);
        Item->SetStringField(TEXT("data_type"), Tagged.Data ? Tagged.Data->GetClass()->GetName() : FString());
        Item->SetStringField(TEXT("pin"), Tagged.Pin.ToString());

        TArray<TSharedPtr<FJsonValue>> Tags;
        for (const FString& Tag : Tagged.Tags)
        {
            Tags.Add(MakeShareable(new FJsonValueString(Tag)));
        }
        Item->SetArrayField(TEXT("tags"), Tags);

        const UPCGPointData* PointData = Cast<UPCGPointData>(Tagged.Data);
        Item->SetBoolField(TEXT("is_point_data"), PointData != nullptr);
        Item->SetNumberField(TEXT("point_count"), PointData ? PointData->GetNumPoints() : 0);

        if (PointData)
        {
            const TArray<FPCGPoint>& Points = PointData->GetPoints();
            AllPoints.Append(Points);
            TotalPoints += Points.Num();

            if (const UPCGMetadata* Metadata = PointData->ConstMetadata())
            {
                // The metadata of the first point data carries every attribute that is worth
                // summarising; later ones only extend the point set.
                if (!PointMetadata)
                {
                    PointMetadata = Metadata;
                }

                TArray<FName> Names;
                TArray<EPCGMetadataTypes> Types;
                Metadata->GetAttributes(Names, Types);
                for (int32 Index = 0; Index < Names.Num() && Index < Types.Num(); ++Index)
                {
                    // PCG keeps an unnamed attribute around; it is not a name a caller can use, and
                    // listing it as "None" only invites a wrong request.
                    if (Names[Index].IsNone())
                    {
                        continue;
                    }
                    AttributeTypes.Add(Names[Index].ToString(), Types[Index]);
                }
            }
        }

        TaggedJson.Add(MakeShareable(new FJsonValueObject(Item)));
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("actor_label"), FoundActor ? FoundActor->GetActorLabel() : FString());
    Result->SetStringField(TEXT("component_name"), Found->GetName());
    Result->SetStringField(TEXT("world_name"), World->GetName());
    Result->SetArrayField(TEXT("tagged_data"), TaggedJson);
    Result->SetNumberField(TEXT("tagged_count"), TaggedJson.Num());
    Result->SetNumberField(TEXT("total_points"), TotalPoints);

    if (bIncludeAttributes || !AttributeName.IsEmpty())
    {
        TArray<TSharedPtr<FJsonValue>> AttributesJson;

        // Point-intrinsic fields first: they are requestable without any metadata attribute (and
        // they are what a generator actually changed), so the caller sees them before the extras.
        if (TotalPoints > 0)
        {
            for (const TPair<FString, FString>& Pair : PointFieldTypes())
            {
                TSharedPtr<FJsonObject> Item = MakeShareable(new FJsonObject);
                Item->SetStringField(TEXT("name"), Pair.Key);
                Item->SetStringField(TEXT("type"), Pair.Value);
                Item->SetStringField(TEXT("source"), TEXT("point"));
                AttributesJson.Add(MakeShareable(new FJsonValueObject(Item)));
            }
        }
        for (const TPair<FString, EPCGMetadataTypes>& Pair : AttributeTypes)
        {
            TSharedPtr<FJsonObject> Item = MakeShareable(new FJsonObject);
            Item->SetStringField(TEXT("name"), Pair.Key);
            Item->SetStringField(TEXT("type"), AttributeTypeName(Pair.Value));
            Item->SetStringField(TEXT("source"), TEXT("metadata"));
            AttributesJson.Add(MakeShareable(new FJsonValueObject(Item)));
        }
        Result->SetArrayField(TEXT("attributes"), AttributesJson);
    }

    if (!AttributeName.IsEmpty())
    {
        const EPCGMetadataTypes* FoundType = AttributeTypes.Find(AttributeName);
        const FString* PointFieldType = PointFieldTypes().Find(AttributeName.ToLower());

        FAttributeStats Stats;
        const bool bPointField = AccumulatePointField(AttributeName, AllPoints, MaxSamples, Stats);
        if (!bPointField && FoundType)
        {
            AccumulateByType(PointMetadata, FName(*AttributeName), *FoundType, AllPoints, MaxSamples, Stats);
        }
        else if (!bPointField)
        {
            TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_attribute"),
                FString::Printf(TEXT("'%s' is not an attribute of this result"), *AttributeName));
            TArray<TSharedPtr<FJsonValue>> AttributeCandidates;
            for (const TPair<FString, FString>& Pair : PointFieldTypes())
            {
                AttributeCandidates.Add(MakeShareable(new FJsonValueString(Pair.Key)));
            }
            for (const TPair<FString, EPCGMetadataTypes>& Pair : AttributeTypes)
            {
                AttributeCandidates.Add(MakeShareable(new FJsonValueString(Pair.Key)));
            }
            Error->SetArrayField(TEXT("candidates"), AttributeCandidates);
            return Error;
        }

        TSharedPtr<FJsonObject> StatsJson = MakeShareable(new FJsonObject);
        StatsJson->SetStringField(TEXT("name"), AttributeName);
        StatsJson->SetStringField(TEXT("type"), bPointField
            ? (PointFieldType ? *PointFieldType : FString())
            : AttributeTypeName(*FoundType));
        StatsJson->SetNumberField(TEXT("sample_count"), Stats.Count);
        StatsJson->SetBoolField(TEXT("numeric"), Stats.bNumeric);

        if (Stats.bNumeric && Stats.Count > 0)
        {
            static const TCHAR* ScalarChannel[1] = { TEXT("value") };
            static const TCHAR* Vector2Channels[2] = { TEXT("x"), TEXT("y") };
            static const TCHAR* Vector3Channels[3] = { TEXT("x"), TEXT("y"), TEXT("z") };
            static const TCHAR* Vector4Channels[4] = { TEXT("x"), TEXT("y"), TEXT("z"), TEXT("w") };
            static const TCHAR* RotatorChannels[3] = { TEXT("pitch"), TEXT("yaw"), TEXT("roll") };
            static const TCHAR* ColorChannels[4] = { TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a") };

            const TCHAR** ChannelNames = ScalarChannel;
            if (Stats.bRotator) { ChannelNames = RotatorChannels; }
            else if (Stats.bColor) { ChannelNames = ColorChannels; }
            else if (Stats.Components == 2) { ChannelNames = Vector2Channels; }
            else if (Stats.Components == 3) { ChannelNames = Vector3Channels; }
            else if (Stats.Components >= 4) { ChannelNames = Vector4Channels; }

            const int32 ChannelCount = Stats.Components > 0 ? Stats.Components : 1;
            StatsJson->SetNumberField(TEXT("min"), Stats.Min[0]);
            StatsJson->SetNumberField(TEXT("max"), Stats.Max[0]);
            StatsJson->SetNumberField(TEXT("mean"), Stats.Sum[0] / Stats.Count);

            TArray<TSharedPtr<FJsonValue>> Channels;
            for (int32 Index = 0; Index < ChannelCount; ++Index)
            {
                TSharedPtr<FJsonObject> Channel = MakeShareable(new FJsonObject);
                Channel->SetStringField(TEXT("name"), ChannelNames[Index]);
                Channel->SetNumberField(TEXT("min"), Stats.Min[Index]);
                Channel->SetNumberField(TEXT("max"), Stats.Max[Index]);
                Channel->SetNumberField(TEXT("mean"), Stats.Sum[Index] / Stats.Count);
                Channels.Add(MakeShareable(new FJsonValueObject(Channel)));
            }
            StatsJson->SetArrayField(TEXT("channels"), Channels);

            TArray<TSharedPtr<FJsonValue>> Samples;
            for (const TArray<double>& Sample : Stats.Samples)
            {
                TArray<TSharedPtr<FJsonValue>> ComponentsJson;
                for (const double Component : Sample)
                {
                    ComponentsJson.Add(MakeShareable(new FJsonValueNumber(Component)));
                }
                Samples.Add(MakeShareable(new FJsonValueArray(ComponentsJson)));
            }
            StatsJson->SetArrayField(TEXT("samples"), Samples);
        }
        else
        {
            // Non-numeric attributes (string / name / soft path / transform) have no distribution:
            // the type is reported and the numeric fields stay null rather than a fake zero.
            StatsJson->SetField(TEXT("min"), MakeShareable(new FJsonValueNull()));
            StatsJson->SetField(TEXT("max"), MakeShareable(new FJsonValueNull()));
            StatsJson->SetField(TEXT("mean"), MakeShareable(new FJsonValueNull()));
        }

        Result->SetObjectField(TEXT("attribute_stats"), StatsJson);
    }

    return Result;
}

namespace
{
    /** Plain string list as a JSON array (for the reflector's rejection fields). */
    TArray<TSharedPtr<FJsonValue>> ToJsonStringArray(const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> Json;
        Json.Reserve(Values.Num());
        for (const FString& Value : Values)
        {
            Json.Add(MakeShareable(new FJsonValueString(Value)));
        }
        return Json;
    }

    /** Copy the position and pin arrays out of a node dump, so a write response carries the same shape. */
    void CopyNodeReadback(const TSharedPtr<FJsonObject>& NodeJson, TSharedPtr<FJsonObject>& Result)
    {
        if (!NodeJson.IsValid())
        {
            return;
        }

        double PositionX = 0.0;
        double PositionY = 0.0;
        NodeJson->TryGetNumberField(TEXT("editor_x"), PositionX);
        NodeJson->TryGetNumberField(TEXT("editor_y"), PositionY);
        Result->SetNumberField(TEXT("editor_x"), PositionX);
        Result->SetNumberField(TEXT("editor_y"), PositionY);

        const TArray<TSharedPtr<FJsonValue>>* InputPins = nullptr;
        if (NodeJson->TryGetArrayField(TEXT("input_pins"), InputPins))
        {
            Result->SetArrayField(TEXT("input_pins"), *InputPins);
        }
        const TArray<TSharedPtr<FJsonValue>>* OutputPins = nullptr;
        if (NodeJson->TryGetArrayField(TEXT("output_pins"), OutputPins))
        {
            Result->SetArrayField(TEXT("output_pins"), *OutputPins);
        }
    }
}

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::HandleAddPCGNode(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' parameter"));
    }
    FString NodeClass;
    if (!Params->TryGetStringField(TEXT("node_class"), NodeClass))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'node_class' parameter"));
    }

    TSharedPtr<FJsonObject> Error;
    UPCGGraph* Graph = ResolveGraph(AssetPath, Error);
    if (!Graph)
    {
        return Error;
    }

    UClass* SettingsClass = ResolveSettingsClass(NodeClass, Error);
    if (!SettingsClass)
    {
        return Error;
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP add_pcg_node")));
    Graph->Modify();

    UPCGSettings* DefaultSettings = nullptr;
    UPCGNode* Node = Graph->AddNodeOfType(SettingsClass, DefaultSettings);
    if (!Node || !Node->GetSettings())
    {
        Transaction.Cancel();
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("node_create_failed"),
            FString::Printf(TEXT("The graph API did not create a node for '%s'"), *SettingsClass->GetName()));
    }

    // Only move the node when the caller asked for a position: the graph's own placement beats a
    // fabricated 0/0 when nobody cares, and a partial request keeps the other axis untouched.
    double RequestedX = 0.0;
    double RequestedY = 0.0;
    const bool bHasX = Params->TryGetNumberField(TEXT("editor_x"), RequestedX);
    const bool bHasY = Params->TryGetNumberField(TEXT("editor_y"), RequestedY);
    if (bHasX || bHasY)
    {
        int32 CurrentX = 0;
        int32 CurrentY = 0;
        Node->GetNodePosition(CurrentX, CurrentY);
        Node->SetNodePosition(bHasX ? static_cast<int32>(RequestedX) : CurrentX,
                              bHasY ? static_cast<int32>(RequestedY) : CurrentY);
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("asset_path"), Graph->GetPathName());
    Result->SetStringField(TEXT("node_name"), Node->GetName());
    Result->SetStringField(TEXT("settings_class"), Node->GetSettings()->GetClass()->GetName());
    Result->SetBoolField(TEXT("in_graph"), Graph->Contains(Node));
    CopyNodeReadback(NodeToJson(Node, /*bIncludeProperties=*/false, 0), Result);
    Result->SetNumberField(TEXT("node_count"), Graph->GetNodes().Num());
    SaveGraphAsset(Graph, Result, FUnrealMCPCommonUtils::IsPersistRequested(Params));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::HandleConnectPCGPins(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    FString FromNodeName;
    FString FromLabel;
    FString ToNodeName;
    FString ToLabel;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath)
        || !Params->TryGetStringField(TEXT("from_node"), FromNodeName)
        || !Params->TryGetStringField(TEXT("from_label"), FromLabel)
        || !Params->TryGetStringField(TEXT("to_node"), ToNodeName)
        || !Params->TryGetStringField(TEXT("to_label"), ToLabel))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing one of 'asset_path' / 'from_node' / 'from_label' / 'to_node' / 'to_label'"));
    }

    TSharedPtr<FJsonObject> Error;
    UPCGGraph* Graph = ResolveGraph(AssetPath, Error);
    if (!Graph)
    {
        return Error;
    }
    UPCGNode* FromNode = ResolveNode(Graph, FromNodeName, INDEX_NONE, Error);
    if (!FromNode)
    {
        return Error;
    }
    UPCGNode* ToNode = ResolveNode(Graph, ToNodeName, INDEX_NONE, Error);
    if (!ToNode)
    {
        return Error;
    }
    UPCGPin* FromPin = ResolvePin(FromNode, FromLabel, /*bOutputPin=*/true, Error);
    if (!FromPin)
    {
        return Error;
    }
    UPCGPin* ToPin = ResolvePin(ToNode, ToLabel, /*bOutputPin=*/false, Error);
    if (!ToPin)
    {
        return Error;
    }

    const int32 EdgesBefore = ToPin->Edges.Num();

    FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP connect_pcg_pins")));
    Graph->Modify();

    // The engine takes pin labels, so the resolved labels are passed on - a caller that wrote the
    // snake_case spelling still connects the pin it meant.
    Graph->AddEdge(FromNode, FromPin->Properties.Label, ToNode, ToPin->Properties.Label);

    // Readback, because AddEdge returns the "to" node even when the label never matched anything:
    // the connection (or its absence) is the only honest answer.
    bool bConnected = false;
    for (const TObjectPtr<UPCGEdge>& Edge : ToPin->Edges)
    {
        const UPCGPin* Other = Edge ? Edge->GetOtherPin(ToPin) : nullptr;
        if (Other && Other->Node == FromNode && Other->Properties.Label == FromPin->Properties.Label)
        {
            bConnected = true;
            break;
        }
    }

    if (!bConnected)
    {
        Transaction.Cancel();

        TSharedPtr<FJsonObject> Failure = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("pin_not_connected"),
            FString::Printf(TEXT("The graph did not connect %s.%s -> %s.%s (a wrong pin label is a silent no-op in the engine)"),
                *FromNode->GetName(), *FromPin->Properties.Label.ToString(),
                *ToNode->GetName(), *ToPin->Properties.Label.ToString()));
        Failure->SetArrayField(TEXT("from_candidates"), PinLabelCandidates(FromNode, /*bOutputPin=*/true));
        Failure->SetArrayField(TEXT("to_candidates"), PinLabelCandidates(ToNode, /*bOutputPin=*/false));
        return Failure;
    }

    TSharedPtr<FJsonObject> EdgeJson = MakeShareable(new FJsonObject);
    EdgeJson->SetStringField(TEXT("from_node"), FromNode->GetName());
    EdgeJson->SetStringField(TEXT("from_label"), FromPin->Properties.Label.ToString());
    EdgeJson->SetStringField(TEXT("to_node"), ToNode->GetName());
    EdgeJson->SetStringField(TEXT("to_label"), ToPin->Properties.Label.ToString());

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("asset_path"), Graph->GetPathName());
    Result->SetBoolField(TEXT("connected"), true);
    Result->SetObjectField(TEXT("edge"), EdgeJson);
    Result->SetNumberField(TEXT("edges_before"), EdgesBefore);
    Result->SetNumberField(TEXT("edges_after"), ToPin->Edges.Num());
    SaveGraphAsset(Graph, Result, FUnrealMCPCommonUtils::IsPersistRequested(Params));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::HandleDisconnectPCGPins(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    FString FromNodeName;
    FString FromLabel;
    FString ToNodeName;
    FString ToLabel;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath)
        || !Params->TryGetStringField(TEXT("from_node"), FromNodeName)
        || !Params->TryGetStringField(TEXT("from_label"), FromLabel)
        || !Params->TryGetStringField(TEXT("to_node"), ToNodeName)
        || !Params->TryGetStringField(TEXT("to_label"), ToLabel))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing one of 'asset_path' / 'from_node' / 'from_label' / 'to_node' / 'to_label'"));
    }

    TSharedPtr<FJsonObject> Error;
    UPCGGraph* Graph = ResolveGraph(AssetPath, Error);
    if (!Graph)
    {
        return Error;
    }
    UPCGNode* FromNode = ResolveNode(Graph, FromNodeName, INDEX_NONE, Error);
    if (!FromNode)
    {
        return Error;
    }
    UPCGNode* ToNode = ResolveNode(Graph, ToNodeName, INDEX_NONE, Error);
    if (!ToNode)
    {
        return Error;
    }
    UPCGPin* FromPin = ResolvePin(FromNode, FromLabel, /*bOutputPin=*/true, Error);
    if (!FromPin)
    {
        return Error;
    }
    UPCGPin* ToPin = ResolvePin(ToNode, ToLabel, /*bOutputPin=*/false, Error);
    if (!ToPin)
    {
        return Error;
    }

    const int32 EdgesBefore = ToPin->Edges.Num();

    FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP disconnect_pcg_pins")));
    Graph->Modify();

    if (!Graph->RemoveEdge(FromNode, FromPin->Properties.Label, ToNode, ToPin->Properties.Label))
    {
        Transaction.Cancel();

        // Say which edges do exist on that pin instead of only "not found".
        TArray<TSharedPtr<FJsonValue>> Candidates;
        for (const TObjectPtr<UPCGEdge>& Edge : ToPin->Edges)
        {
            const UPCGPin* Other = Edge ? Edge->GetOtherPin(ToPin) : nullptr;
            if (Other && Other->Node)
            {
                Candidates.Add(MakeShareable(new FJsonValueString(FString::Printf(TEXT("%s.%s -> %s.%s"),
                    *Other->Node->GetName(), *Other->Properties.Label.ToString(),
                    *ToNode->GetName(), *ToPin->Properties.Label.ToString()))));
            }
        }
        for (const TObjectPtr<UPCGEdge>& Edge : FromPin->Edges)
        {
            const UPCGPin* Other = Edge ? Edge->GetOtherPin(FromPin) : nullptr;
            if (Other && Other->Node)
            {
                Candidates.Add(MakeShareable(new FJsonValueString(FString::Printf(TEXT("%s.%s -> %s.%s"),
                    *FromNode->GetName(), *FromPin->Properties.Label.ToString(),
                    *Other->Node->GetName(), *Other->Properties.Label.ToString()))));
            }
        }

        TSharedPtr<FJsonObject> Failure = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("edge_not_found"),
            FString::Printf(TEXT("No edge %s.%s -> %s.%s in this graph"),
                *FromNode->GetName(), *FromPin->Properties.Label.ToString(),
                *ToNode->GetName(), *ToPin->Properties.Label.ToString()));
        Failure->SetArrayField(TEXT("candidates"), Candidates);
        return Failure;
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("asset_path"), Graph->GetPathName());
    Result->SetBoolField(TEXT("removed"), true);
    Result->SetNumberField(TEXT("edges_before"), EdgesBefore);
    Result->SetNumberField(TEXT("edges_after"), ToPin->Edges.Num());
    SaveGraphAsset(Graph, Result, FUnrealMCPCommonUtils::IsPersistRequested(Params));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::HandleRemovePCGNode(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    FString NodeName;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath)
        || !Params->TryGetStringField(TEXT("node_name"), NodeName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing one of 'asset_path' / 'node_name'"));
    }

    TSharedPtr<FJsonObject> Error;
    UPCGGraph* Graph = ResolveGraph(AssetPath, Error);
    if (!Graph)
    {
        return Error;
    }
    UPCGNode* Node = ResolveNode(Graph, NodeName, INDEX_NONE, Error);
    if (!Node)
    {
        return Error;
    }

    if (Node == Graph->GetInputNode() || Node == Graph->GetOutputNode())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("cannot_remove_graph_io_nodes"),
            FString::Printf(TEXT("'%s' is the graph's input or output node and cannot be removed"), *Node->GetName()));
    }

    // Read the name before the removal: the object is gone afterwards.
    const FString RemovedName = Node->GetName();
    const int32 NodeCountBefore = Graph->GetNodes().Num();

    FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP remove_pcg_node")));
    Graph->Modify();
    Graph->RemoveNode(Node);

    // Readback: removal is only reported when the graph really lost exactly that one node.
    const int32 NodeCountAfter = Graph->GetNodes().Num();
    if (NodeCountAfter != NodeCountBefore - 1)
    {
        Transaction.Cancel();
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("remove_failed"),
            FString::Printf(TEXT("'%s' was not removed: the graph went from %d to %d nodes"),
                *RemovedName, NodeCountBefore, NodeCountAfter));
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("asset_path"), Graph->GetPathName());
    Result->SetStringField(TEXT("removed_name"), RemovedName);
    Result->SetNumberField(TEXT("node_count"), NodeCountAfter);
    SaveGraphAsset(Graph, Result, FUnrealMCPCommonUtils::IsPersistRequested(Params));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::HandleSetPCGNodeProperty(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    FString PropertyName;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath)
        || !Params->TryGetStringField(TEXT("property_name"), PropertyName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing one of 'asset_path' / 'property_name'"));
    }
    const TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
    if (!Value.IsValid())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'value' parameter"));
    }

    FString NodeName;
    Params->TryGetStringField(TEXT("node_name"), NodeName);
    int32 NodeIndex = INDEX_NONE;
    if (double NodeIndexValue = 0.0; Params->TryGetNumberField(TEXT("node_index"), NodeIndexValue))
    {
        NodeIndex = static_cast<int32>(NodeIndexValue);
    }

    TSharedPtr<FJsonObject> Error;
    UPCGGraph* Graph = ResolveGraph(AssetPath, Error);
    if (!Graph)
    {
        return Error;
    }
    UPCGNode* Node = ResolveNode(Graph, NodeName, NodeIndex, Error);
    if (!Node)
    {
        return Error;
    }
    UPCGSettings* Settings = Node->GetSettings();
    if (!Settings)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("node_has_no_settings"),
            FString::Printf(TEXT("'%s' carries no settings object, so it has no properties to write"), *Node->GetName()));
    }

    // Reflection name first (that is what a read table reports as property_name), then the
    // snake_case spelling (that is what it reports as python_name). Both are documented as writable.
    FProperty* Property = Settings->GetClass()->FindPropertyByName(*PropertyName);
    if (!Property)
    {
        for (TFieldIterator<FProperty> It(Settings->GetClass()); It; ++It)
        {
            if (PythonStyleName(It->GetName()).Equals(PropertyName, ESearchCase::IgnoreCase))
            {
                Property = *It;
                break;
            }
        }
    }
    if (!Property)
    {
        TArray<TSharedPtr<FJsonValue>> Candidates;
        for (TFieldIterator<FProperty> It(Settings->GetClass()); It; ++It)
        {
            Candidates.Add(MakeShareable(new FJsonValueString(It->GetName())));
        }
        TSharedPtr<FJsonObject> Failure = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unknown_property"),
            FString::Printf(TEXT("'%s' has no property '%s' (neither the reflection name nor its snake_case spelling)"),
                *Settings->GetClass()->GetName(), *PropertyName));
        Failure->SetArrayField(TEXT("candidates"), Candidates);
        return Failure;
    }

    // The same gate the read table applies, so "writable: false" and a rejected write agree.
    FString NotWritableHint;
    if (!PropertyWritability(Property, NotWritableHint))
    {
        TSharedPtr<FJsonObject> Failure = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("property_not_writable"),
            FString::Printf(TEXT("'%s' cannot be written through the property writer"), *Property->GetName()));
        Failure->SetStringField(TEXT("hint"), NotWritableHint);
        Failure->SetStringField(TEXT("property_name"), Property->GetName());
        Failure->SetStringField(TEXT("python_name"), PythonStyleName(Property->GetName()));
        Failure->SetBoolField(TEXT("saved"), false);
        return Failure;
    }

    const TSharedPtr<FJsonObject> BeforeEntry = PropertyToJsonEntry(Property, Settings);
    const TSharedPtr<FJsonValue> ValueBefore = BeforeEntry.IsValid() ? BeforeEntry->TryGetField(TEXT("value")) : nullptr;

    FString WriteError;
    FString ErrorCode;
    TArray<FString> AvailableFields;
    FWriteResult WriteResult;
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP set_pcg_node_property")));
        Settings->Modify();

        if (!FUnrealMCPCommonUtils::SetObjectProperty(Settings, Property->GetName(), Value, WriteError,
                                                      &AvailableFields, &ErrorCode, &WriteResult))
        {
            Transaction.Cancel();

            TSharedPtr<FJsonObject> Failure = FUnrealMCPCommonUtils::CreateErrorResponse(
                ErrorCode.IsEmpty() ? TEXT("write_failed") : ErrorCode, WriteError);
            Failure->SetStringField(TEXT("property_name"), Property->GetName());
            Failure->SetStringField(TEXT("property_type"), Property->GetCPPType());
            Failure->SetBoolField(TEXT("unchanged"), WriteResult.bUnchanged);
            Failure->SetBoolField(TEXT("saved"), false);
            if (AvailableFields.Num() > 0)
            {
                Failure->SetArrayField(TEXT("available_fields"), ToJsonStringArray(AvailableFields));
            }
            if (WriteResult.SupportedShapes.Num() > 0)
            {
                Failure->SetArrayField(TEXT("supported_shapes"), ToJsonStringArray(WriteResult.SupportedShapes));
            }
            if (!WriteResult.Hint.IsEmpty())
            {
                Failure->SetStringField(TEXT("hint"), WriteResult.Hint);
            }
            return Failure;
        }

        // Tell PCG the settings changed. A raw property write never reaches PostEditChangeProperty,
        // and that is where PCG refreshes UPCGSettings::CachedCrc and broadcasts
        // OnSettingsChangedDelegate -> UPCGNode -> graph. The graph compiler caches the compiled
        // element keyed by that CRC, so without this the next generation re-runs the graph and still
        // uses the OLD value. Measured: writing LowerBound 0.5 -> 0.99 and regenerating (force) left
        // the point count and the ISM batches untouched, while structural edits (add/remove/connect)
        // were picked up fine - the difference is exactly this notification.
        Settings->PostEditChange();

        // Readback inside the transaction: a value that did not survive the write is reported as a
        // mismatch and the transaction is rolled back, so nothing unsaved is left behind either.
        const TSharedPtr<FJsonObject> AfterEntry = PropertyToJsonEntry(Property, Settings);
        const TSharedPtr<FJsonValue> ValueAfter = AfterEntry.IsValid() ? AfterEntry->TryGetField(TEXT("value")) : nullptr;

        const bool bVerified = ValueAfter.IsValid();
        if (bVerified && !JsonValuesEquivalent(Value, ValueAfter))
        {
            Transaction.Cancel();

            // Put the old value back explicitly: rolling back the transaction is not enough on its
            // own (measured: the truncated value survived a Cancel), and "the asset is left unsaved"
            // is only honest when the in-memory graph is back where it was too.
            bool bReverted = false;
            FString RevertError;
            if (ValueBefore.IsValid())
            {
                FString RestoreError;
                FString RestoreCode;
                bReverted = FUnrealMCPCommonUtils::SetObjectProperty(Settings, Property->GetName(), ValueBefore,
                                                                    RestoreError, nullptr, &RestoreCode);
                if (!bReverted)
                {
                    // Worth reporting: it means the in-memory graph still holds the rejected value, and
                    // the caller has to know that "left unsaved" is not the same as "left unchanged".
                    RevertError = RestoreCode.IsEmpty() ? RestoreError
                                                        : FString::Printf(TEXT("%s: %s"), *RestoreCode, *RestoreError);
                }
            }
            else
            {
                RevertError = TEXT("the property had no readable value before the write, so there was nothing to restore");
            }

            TSharedPtr<FJsonObject> Failure = ReadbackMismatchError(Property->GetName(), Value, ValueAfter);
            Failure->SetBoolField(TEXT("reverted"), bReverted);
            if (!bReverted && !RevertError.IsEmpty())
            {
                Failure->SetStringField(TEXT("revert_error"), RevertError);
            }
            Failure->SetStringField(TEXT("property_name"), Property->GetName());
            Failure->SetStringField(TEXT("python_name"), PythonStyleName(Property->GetName()));
            Failure->SetStringField(TEXT("property_type"), Property->GetCPPType());
            return Failure;
        }

        TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
        Result->SetBoolField(TEXT("success"), true);
        Result->SetStringField(TEXT("asset_path"), Graph->GetPathName());
        Result->SetStringField(TEXT("node_name"), Node->GetName());
        Result->SetStringField(TEXT("settings_class"), Settings->GetClass()->GetName());
        Result->SetStringField(TEXT("property_name"), Property->GetName());
        Result->SetStringField(TEXT("python_name"), PythonStyleName(Property->GetName()));
        Result->SetStringField(TEXT("property_type"), Property->GetCPPType());
        if (ValueBefore.IsValid())
        {
            Result->SetField(TEXT("value_before"), ValueBefore);
        }
        if (ValueAfter.IsValid())
        {
            Result->SetField(TEXT("value_after"), ValueAfter);
            Result->SetStringField(TEXT("value_shape"), JsonValueShape(ValueAfter->Type));
        }
        else
        {
            Result->SetStringField(TEXT("value_shape"), TEXT("omitted"));
            Result->SetStringField(TEXT("readback_note"),
                TEXT("the shared reflector has no JSON shape for this property type, so the write could not be compared - it is reported as written, not as verified"));
        }
        Result->SetBoolField(TEXT("verified"), bVerified);
        Result->SetBoolField(TEXT("changed"), !ValueBefore.IsValid() || !JsonValuesEquivalent(ValueBefore, ValueAfter));
        Result->SetBoolField(TEXT("notified"), true);
        SaveGraphAsset(Graph, Result, FUnrealMCPCommonUtils::IsPersistRequested(Params));
        return Result;
    }
}

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::HandleGeneratePCGComponent(const TSharedPtr<FJsonObject>& Params)
{
    bool bForce = false;
    Params->TryGetBoolField(TEXT("force"), bForce);

    FString ActorLabel;
    FString ComponentName;
    Params->TryGetStringField(TEXT("actor_label"), ActorLabel);
    Params->TryGetStringField(TEXT("component_name"), ComponentName);

    AActor* Actor = nullptr;
    TSharedPtr<FJsonObject> Error;
    UPCGComponent* Component = FindPCGComponent(ActorLabel, ComponentName, &Actor, Error);
    if (!Component)
    {
        return Error;
    }

    if (Component->IsManagedByRuntimeGenSystem())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("runtime_generation_component"),
            FString::Printf(TEXT("'%s' is managed by the runtime generation system, which owns its generation - "
                                "a manual generate would be overwritten by it"), *Component->GetName()));
    }

    // Dispatch and report. Generation runs through a task graph, so the honest answer here is the
    // component state at this instant; the result is read afterwards with get_pcg_generated_output.
    Component->Generate(bForce);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("actor_label"), Actor ? Actor->GetActorLabel() : FString());
    Result->SetStringField(TEXT("component_name"), Component->GetName());
    const UPCGGraph* BoundGraph = Component->GetGraph();
    Result->SetStringField(TEXT("graph_path"), BoundGraph ? BoundGraph->GetPathName() : FString());
    Result->SetStringField(TEXT("generation_trigger"), GenerationTriggerName(Component->GenerationTrigger));
    Result->SetBoolField(TEXT("dispatched"), true);
    Result->SetBoolField(TEXT("active"), Component->IsActive());
    Result->SetBoolField(TEXT("generating"), Component->IsGenerating());
    Result->SetBoolField(TEXT("generated_output_available"), Component->GetGeneratedGraphOutput().TaggedData.Num() > 0);
    Result->SetStringField(TEXT("note"),
        TEXT("generation is asynchronous: read the result with get_pcg_generated_output / list_pcg_components once it lands"));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::HandleCleanupPCGComponent(const TSharedPtr<FJsonObject>& Params)
{
    bool bRemoveComponents = true;
    Params->TryGetBoolField(TEXT("remove_components"), bRemoveComponents);
    bool bSaveGeneratedComponents = false;
    Params->TryGetBoolField(TEXT("save_generated_components"), bSaveGeneratedComponents);

    FString ActorLabel;
    FString ComponentName;
    Params->TryGetStringField(TEXT("actor_label"), ActorLabel);
    Params->TryGetStringField(TEXT("component_name"), ComponentName);

    AActor* Actor = nullptr;
    TSharedPtr<FJsonObject> Error;
    UPCGComponent* Component = FindPCGComponent(ActorLabel, ComponentName, &Actor, Error);
    if (!Component)
    {
        return Error;
    }

    if (Component->IsManagedByRuntimeGenSystem())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("runtime_generation_component"),
            FString::Printf(TEXT("'%s' is managed by the runtime generation system, which owns its cleanup"), *Component->GetName()));
    }

    Component->Cleanup(bRemoveComponents, bSaveGeneratedComponents);

    // Cleanup never touches the graph asset, so there is no "saved" field here: reporting one would
    // suggest the graph was rewritten.
    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("actor_label"), Actor ? Actor->GetActorLabel() : FString());
    Result->SetStringField(TEXT("component_name"), Component->GetName());
    const UPCGGraph* BoundGraph = Component->GetGraph();
    Result->SetStringField(TEXT("graph_path"), BoundGraph ? BoundGraph->GetPathName() : FString());
    Result->SetBoolField(TEXT("cleanup_dispatched"), true);
    Result->SetBoolField(TEXT("remove_components"), bRemoveComponents);
    Result->SetBoolField(TEXT("generating"), Component->IsGenerating());
    Result->SetBoolField(TEXT("generated_output_available"), Component->GetGeneratedGraphOutput().TaggedData.Num() > 0);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::HandleCreatePCGGraph(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' parameter"));
    }
    FString Folder;
    Params->TryGetStringField(TEXT("folder"), Folder);

    // asset_path is the full path (folder + name). A caller that only knows the name passes `folder`.
    FString AssetName = AssetPath;
    const int32 LastSlash = AssetPath.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    if (LastSlash != INDEX_NONE)
    {
        AssetName = AssetPath.Mid(LastSlash + 1);
        Folder = AssetPath.Left(LastSlash);
    }

    // Object paths get pasted in too ("/Game/X/Y.Y") - keep the asset name.
    const int32 LastDot = AssetName.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    if (LastDot != INDEX_NONE)
    {
        AssetName = AssetName.Left(LastDot);
    }

    if (AssetName.IsEmpty() || Folder.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_asset_path"),
            FString::Printf(TEXT("'%s' does not carry both a folder and an asset name (expected e.g. /Game/MCP/_PCGProbe/PCG_New)"),
                *AssetPath));
    }

    // Registry query rather than a load: this path must not pull an existing asset into memory just
    // to find out that it is already there.
    const FString PackageName = Folder + TEXT("/") + AssetName;
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    TArray<FAssetData> Existing;
    AssetRegistryModule.Get().GetAssetsByPackageName(FName(*PackageName), Existing);
    if (Existing.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> ExistingJson;
        for (const FAssetData& AssetData : Existing)
        {
            ExistingJson.Add(MakeShareable(new FJsonValueString(AssetData.GetObjectPathString())));
        }

        TSharedPtr<FJsonObject> Failure = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("asset_exists"),
            FString::Printf(TEXT("'%s' already exists"), *PackageName));
        Failure->SetStringField(TEXT("asset_path"), Existing[0].GetObjectPathString());
        Failure->SetArrayField(TEXT("existing_assets"), ExistingJson);
        Failure->SetStringField(TEXT("hint"),
            TEXT("delete it (safe_delete_asset) or pick another name - this command never overwrites an asset"));
        return Failure;
    }

    // The engine's PCGGraphFactory is in PCGEditor/Private and cannot be included from here, so this
    // plugin carries its own minimal UPCGGraph factory (see UnrealMCPPCGGraphFactory.h).
    UFactory* Factory = NewObject<UUnrealMCPPCGGraphFactory>(GetTransientPackage(), UUnrealMCPPCGGraphFactory::StaticClass());
    UPCGGraph* Graph = Cast<UPCGGraph>(FUnrealMCPCommonUtils::CreateAssetDirect(
        AssetName, Folder, UPCGGraph::StaticClass(), Factory));
    if (!Graph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("create_failed"),
            FString::Printf(TEXT("the asset factory did not produce a PCG graph at '%s'"), *PackageName));
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("asset_path"), Graph->GetPathName());
    Result->SetStringField(TEXT("graph_class"), Graph->GetClass()->GetName());
    Result->SetNumberField(TEXT("node_count"), Graph->GetNodes().Num());

    // Read the graph back through the same accessors the read commands use. The input/output nodes
    // are default subobjects of every UPCGGraph, so their names are available immediately - and the
    // caller needs them: a chain that never reaches the output node generates an empty result.
    if (const UPCGNode* InputNode = Graph->GetInputNode())
    {
        Result->SetStringField(TEXT("input_node"), InputNode->GetName());
    }
    if (const UPCGNode* OutputNode = Graph->GetOutputNode())
    {
        Result->SetStringField(TEXT("output_node"), OutputNode->GetName());
    }
    Result->SetStringField(TEXT("note"),
        TEXT("a fresh graph has no chain: add nodes and connect them to output_node, otherwise generation succeeds with an empty result"));
    SaveGraphAsset(Graph, Result, FUnrealMCPCommonUtils::IsPersistRequested(Params));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::HandleSetPCGComponentGraph(const TSharedPtr<FJsonObject>& Params)
{
    FString GraphPath;
    if (!Params->TryGetStringField(TEXT("graph_path"), GraphPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'graph_path' parameter"));
    }

    FString ActorLabel;
    FString ComponentName;
    Params->TryGetStringField(TEXT("actor_label"), ActorLabel);
    Params->TryGetStringField(TEXT("component_name"), ComponentName);

    UObject* Asset = FUnrealMCPCommonUtils::FindAsset(GraphPath);
    if (!Asset)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("asset_not_found"),
            FString::Printf(TEXT("No asset at '%s' (list_pcg_assets shows the PCG assets that exist)"), *GraphPath));
    }
    UPCGGraphInterface* GraphInterface = Cast<UPCGGraphInterface>(Asset);
    if (!GraphInterface)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("not_a_pcg_graph_interface"),
            FString::Printf(TEXT("'%s' is a %s, not a PCGGraph or PCGGraphInstance"),
                *Asset->GetPathName(), *Asset->GetClass()->GetName()));
    }

    AActor* Actor = nullptr;
    TSharedPtr<FJsonObject> Error;
    UPCGComponent* Component = FindPCGComponent(ActorLabel, ComponentName, &Actor, Error);
    if (!Component)
    {
        return Error;
    }

    bool bActive = false;
    const bool bHasActive = Params->TryGetBoolField(TEXT("active"), bActive);

    FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP set_pcg_component_graph")));
    Component->Modify();

    if (bHasActive)
    {
        // A component that is not active generates and cleans up nothing (measured: the fixture
        // component reported active=false until this was set, and generation produced no points).
        Component->SetActive(bActive);
    }

    // SetGraphLocal is the public editor-side rebinding entry (SetGraphInterfaceLocal is private;
    // SetGraph is the NetMulticast runtime path). It stores what it is handed in the component's own
    // graph instance and refreshes the component after the change.
    Component->SetGraphLocal(GraphInterface);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("actor_label"), Actor ? Actor->GetActorLabel() : FString());
    Result->SetStringField(TEXT("component_name"), Component->GetName());
    Result->SetStringField(TEXT("requested_graph_path"), Asset->GetPathName());

    // Shared readback with list_pcg_components, then the write-side name for "has output".
    const bool bHasOutput = SetComponentStateFields(Component, Result);
    Result->SetBoolField(TEXT("generated_output_available"), bHasOutput);
    if (bHasActive)
    {
        Result->SetBoolField(TEXT("active_set_to"), bActive);
    }

    // Readback of the binding itself. The component holds what it was handed in its own graph
    // instance (`GraphInstance->Graph`), and resolves the effective graph through it - so the honest
    // check is "what does the component hold", reported separately from "which graph is effective".
    // A PCGGraphInstance asset makes those two different, which is exactly why both are reported.
    const UPCGGraphInstance* ComponentInstance = Component->GetGraphInstance();
    const UPCGGraphInterface* HeldInterface = ComponentInstance ? ComponentInstance->Graph.Get() : nullptr;
    const UPCGGraph* EffectiveGraph = Component->GetGraph();

    Result->SetStringField(TEXT("graph_interface_path"), HeldInterface ? HeldInterface->GetPathName() : FString());

    const bool bHeldRequestedInterface = HeldInterface && HeldInterface->GetPathName() == Asset->GetPathName();
    if (!bHeldRequestedInterface || !EffectiveGraph)
    {
        Transaction.Cancel();

        TSharedPtr<FJsonObject> Failure = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("readback_mismatch"),
            EffectiveGraph
                ? TEXT("the component does not hold the graph interface that was requested")
                : TEXT("the component holds the requested interface but it does not resolve to a graph"));
        Failure->SetStringField(TEXT("requested_graph_path"), Asset->GetPathName());
        Failure->SetStringField(TEXT("graph_interface_path"),
            HeldInterface ? HeldInterface->GetPathName() : FString());
        Failure->SetBoolField(TEXT("saved"), false);
        return Failure;
    }

    // The level package is dirty now. This command deliberately does not save it: writing the user's
    // level is their decision, unlike the graph asset writes which persist immediately.
    if (UPackage* LevelPackage = Actor ? Actor->GetOutermost() : nullptr)
    {
        Result->SetBoolField(TEXT("level_package_dirty"), LevelPackage->IsDirty());
    }
    Result->SetStringField(TEXT("note"),
        TEXT("binding only: trigger generate_pcg_component to produce output, and save the level yourself if you want the binding to survive"));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::HandleSetPCGMeshSelectorType(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    FString SelectorClass;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath)
        || !Params->TryGetStringField(TEXT("selector_class"), SelectorClass))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing one of 'asset_path' / 'selector_class'"));
    }

    FString NodeName;
    Params->TryGetStringField(TEXT("node_name"), NodeName);
    int32 NodeIndex = INDEX_NONE;
    if (double NodeIndexValue = 0.0; Params->TryGetNumberField(TEXT("node_index"), NodeIndexValue))
    {
        NodeIndex = static_cast<int32>(NodeIndexValue);
    }

    TSharedPtr<FJsonObject> Error;
    UPCGGraph* Graph = ResolveGraph(AssetPath, Error);
    if (!Graph)
    {
        return Error;
    }
    UPCGNode* Node = ResolveNode(Graph, NodeName, NodeIndex, Error);
    if (!Node)
    {
        return Error;
    }

    UObject* SettingsObject = Node->GetSettings();
    UPCGStaticMeshSpawnerSettings* SpawnerSettings = Cast<UPCGStaticMeshSpawnerSettings>(SettingsObject);
    if (!SpawnerSettings)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("not_a_static_mesh_spawner"),
            FString::Printf(TEXT("'%s' holds a %s, not a PCGStaticMeshSpawnerSettings"),
                *Node->GetName(), SettingsObject ? *SettingsObject->GetClass()->GetName() : TEXT("<none>")));
    }

    UClass* ResolvedClass = ResolveMeshSelectorClass(SelectorClass, Error);
    if (!ResolvedClass)
    {
        return Error;
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP set_pcg_mesh_selector_type")));
    SpawnerSettings->Modify();

    // The engine entry point: it assigns the class and instantiates the selector, which is the only
    // way to make the instanced read-only MeshSelectorParameters writable at all.
    SpawnerSettings->SetMeshSelectorType(ResolvedClass);
    SpawnerSettings->PostEditChange();

    // Readback: the class AND the instance have to be there. A type that was assigned without an
    // instance would leave the entries command with nothing to write into.
    const UClass* ActualClass = SpawnerSettings->MeshSelectorType.Get();
    UPCGMeshSelectorBase* SelectorInstance = SpawnerSettings->MeshSelectorParameters;
    if (!ActualClass || !ActualClass->GetName().Equals(ResolvedClass->GetName(), ESearchCase::IgnoreCase) || !SelectorInstance)
    {
        Transaction.Cancel();

        TSharedPtr<FJsonObject> Failure = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("readback_mismatch"),
            TEXT("the mesh selector type or its instance did not materialize"));
        Failure->SetStringField(TEXT("requested_selector_class"), ResolvedClass->GetName());
        Failure->SetStringField(TEXT("selector_class"), ActualClass ? ActualClass->GetName() : FString());
        Failure->SetBoolField(TEXT("selector_instantiated"), SelectorInstance != nullptr);
        Failure->SetBoolField(TEXT("saved"), false);
        return Failure;
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("asset_path"), Graph->GetPathName());
    Result->SetStringField(TEXT("node_name"), Node->GetName());
    Result->SetStringField(TEXT("selector_class"), ActualClass->GetName());
    Result->SetStringField(TEXT("selector_parameters_path"), SelectorInstance->GetPathName());
    Result->SetStringField(TEXT("selector_parameters_class"), SelectorInstance->GetClass()->GetName());
    Result->SetBoolField(TEXT("selector_instantiated"), true);
    Result->SetBoolField(TEXT("notified"), true);
    SaveGraphAsset(Graph, Result, FUnrealMCPCommonUtils::IsPersistRequested(Params));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPPCGCommands::HandleSetPCGMeshSelectorEntries(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' parameter"));
    }

    FString NodeName;
    Params->TryGetStringField(TEXT("node_name"), NodeName);
    int32 NodeIndex = INDEX_NONE;
    if (double NodeIndexValue = 0.0; Params->TryGetNumberField(TEXT("node_index"), NodeIndexValue))
    {
        NodeIndex = static_cast<int32>(NodeIndexValue);
    }

    const TArray<TSharedPtr<FJsonValue>>* EntriesJson = nullptr;
    if (!Params->TryGetArrayField(TEXT("entries"), EntriesJson) || !EntriesJson)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'entries' parameter (an array)"));
    }

    TSharedPtr<FJsonObject> Error;
    UPCGGraph* Graph = ResolveGraph(AssetPath, Error);
    if (!Graph)
    {
        return Error;
    }
    UPCGNode* Node = ResolveNode(Graph, NodeName, NodeIndex, Error);
    if (!Node)
    {
        return Error;
    }

    UObject* SettingsObject = Node->GetSettings();
    UPCGStaticMeshSpawnerSettings* SpawnerSettings = Cast<UPCGStaticMeshSpawnerSettings>(SettingsObject);
    if (!SpawnerSettings)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("not_a_static_mesh_spawner"),
            FString::Printf(TEXT("'%s' holds a %s, not a PCGStaticMeshSpawnerSettings"),
                *Node->GetName(), SettingsObject ? *SettingsObject->GetClass()->GetName() : TEXT("<none>")));
    }

    UPCGMeshSelectorBase* SelectorInstance = SpawnerSettings->MeshSelectorParameters;
    if (!SelectorInstance)
    {
        TSharedPtr<FJsonObject> Failure = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("selector_not_instantiated"),
            TEXT("this spawner has no mesh selector instance yet - call set_pcg_mesh_selector_type first (the selector is an instanced property, the engine only creates it through that entry)"));
        Failure->SetStringField(TEXT("selector_class"),
            SpawnerSettings->MeshSelectorType ? SpawnerSettings->MeshSelectorType->GetName() : FString());
        Failure->SetBoolField(TEXT("saved"), false);
        return Failure;
    }

    UPCGMeshSelectorWeighted* WeightedSelector = Cast<UPCGMeshSelectorWeighted>(SelectorInstance);
    if (!WeightedSelector)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unsupported_selector"),
            FString::Printf(TEXT("'%s' entries are not supported here: only PCGMeshSelectorWeighted has a mesh table"),
                *SelectorInstance->GetClass()->GetName()));
    }

    // Validate everything before touching the graph: a partially written table would look configured
    // while silently selecting the wrong meshes, and the whole point of this command is that the
    // mesh-selector traps stop being silent.
    struct FValidatedEntry
    {
        UStaticMesh* Mesh = nullptr;
        int32 Weight = 1;
        TArray<UMaterialInterface*> OverrideMaterials;
    };

    TArray<FValidatedEntry> ValidatedEntries;
    ValidatedEntries.Reserve(EntriesJson->Num());
    for (int32 Index = 0; Index < EntriesJson->Num(); ++Index)
    {
        // FJsonValue::TryGetObject hands back a pointer to the shared pointer, not a reference.
        const TSharedPtr<FJsonObject>* EntryObject = nullptr;
        if (!(*EntriesJson)[Index].IsValid() || !(*EntriesJson)[Index]->TryGetObject(EntryObject) || !EntryObject || !EntryObject->IsValid())
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_entry"),
                FString::Printf(TEXT("entries[%d] is not an object with 'static_mesh' / 'weight'"), Index));
        }
        const TSharedPtr<FJsonObject>& EntryJson = *EntryObject;

        FValidatedEntry Entry;

        FString MeshReference;
        if (!EntryJson->TryGetStringField(TEXT("static_mesh"), MeshReference) || MeshReference.IsEmpty())
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_entry"),
                FString::Printf(TEXT("entries[%d] has no 'static_mesh'"), Index));
        }

        UObject* MeshAsset = FUnrealMCPCommonUtils::FindAsset(MeshReference);
        if (!MeshAsset)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unknown_mesh"),
                FString::Printf(TEXT("entries[%d].static_mesh '%s' does not resolve to an asset"), Index, *MeshReference));
        }
        Entry.Mesh = Cast<UStaticMesh>(MeshAsset);
        if (!Entry.Mesh)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unknown_mesh"),
                FString::Printf(TEXT("entries[%d].static_mesh '%s' is a %s, not a StaticMesh"), Index,
                    *MeshAsset->GetPathName(), *MeshAsset->GetClass()->GetName()));
        }

        double WeightValue = 1.0;
        EntryJson->TryGetNumberField(TEXT("weight"), WeightValue);
        if (FMath::Frac(WeightValue) != 0.0)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_weight"),
                FString::Printf(TEXT("entries[%d].weight is %f: the engine field is an int32, so a fraction would be truncated (0.7 -> 0) and that entry would never be selected - the spawner then emits nothing without a single log line. Use an integer weight."),
                    Index, WeightValue));
        }
        if (WeightValue < 0.0)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_weight"),
                FString::Printf(TEXT("entries[%d].weight is negative (%f); the engine expects a weight of 0 or more"),
                    Index, WeightValue));
        }
        Entry.Weight = static_cast<int32>(WeightValue);

        const TArray<TSharedPtr<FJsonValue>>* MaterialsJson = nullptr;
        if (EntryJson->TryGetArrayField(TEXT("override_materials"), MaterialsJson) && MaterialsJson)
        {
            for (const TSharedPtr<FJsonValue>& MaterialValue : *MaterialsJson)
            {
                FString MaterialReference;
                if (!MaterialValue.IsValid() || !MaterialValue->TryGetString(MaterialReference))
                {
                    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_entry"),
                        FString::Printf(TEXT("entries[%d].override_materials contains a non-string entry"), Index));
                }

                UObject* MaterialAsset = FUnrealMCPCommonUtils::FindAsset(MaterialReference);
                UMaterialInterface* Material = Cast<UMaterialInterface>(MaterialAsset);
                if (!Material)
                {
                    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unknown_material"),
                        FString::Printf(TEXT("entries[%d].override_materials '%s' is %s"), Index, *MaterialReference,
                            MaterialAsset ? *FString::Printf(TEXT("a %s, not a MaterialInterface"), *MaterialAsset->GetClass()->GetName())
                                          : TEXT("not an asset")));
                }
                Entry.OverrideMaterials.Add(Material);
            }
        }

        ValidatedEntries.Add(Entry);
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP set_pcg_mesh_selector_entries")));
    WeightedSelector->Modify();

    WeightedSelector->MeshEntries.Reset();
    for (const FValidatedEntry& Entry : ValidatedEntries)
    {
        FPCGMeshSelectorWeightedEntry& NewEntry = WeightedSelector->MeshEntries.AddDefaulted_GetRef();
        NewEntry.Descriptor.StaticMesh = Entry.Mesh;
        NewEntry.Weight = Entry.Weight;
        for (UMaterialInterface* Material : Entry.OverrideMaterials)
        {
            NewEntry.Descriptor.OverrideMaterials.Add(Material);
        }
    }
    WeightedSelector->RefreshDisplayNames();

    // Tell the engine. The settings' compiled-graph cache key is a CRC that walks into instanced
    // subobjects, and that CRC (plus the OnSettingsChangedDelegate chain) only refreshes in
    // PostEditChangeProperty - without this the next generation re-runs the graph with the OLD table.
    WeightedSelector->PostEditChange();
    SpawnerSettings->PostEditChange();

    // Readback, in the same shape the caller wrote.
    TArray<TSharedPtr<FJsonValue>> ReadbackEntries;
    for (const FPCGMeshSelectorWeightedEntry& Entry : WeightedSelector->MeshEntries)
    {
        TSharedPtr<FJsonObject> Item = MakeShareable(new FJsonObject);
        Item->SetStringField(TEXT("static_mesh"), Entry.Descriptor.StaticMesh.ToSoftObjectPath().ToString());
        Item->SetNumberField(TEXT("weight"), Entry.Weight);
        Item->SetStringField(TEXT("display_name"), Entry.DisplayName.ToString());

        TArray<TSharedPtr<FJsonValue>> MaterialsJson;
        for (const TSoftObjectPtr<UMaterialInterface>& Material : Entry.Descriptor.OverrideMaterials)
        {
            MaterialsJson.Add(MakeShareable(new FJsonValueString(Material.ToSoftObjectPath().ToString())));
        }
        Item->SetArrayField(TEXT("override_materials"), MaterialsJson);
        ReadbackEntries.Add(MakeShareable(new FJsonValueObject(Item)));
    }

    if (ReadbackEntries.Num() != ValidatedEntries.Num())
    {
        Transaction.Cancel();

        TSharedPtr<FJsonObject> Failure = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("readback_mismatch"),
            FString::Printf(TEXT("wrote %d entries but the selector reports %d"), ValidatedEntries.Num(), ReadbackEntries.Num()));
        Failure->SetBoolField(TEXT("saved"), false);
        return Failure;
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("asset_path"), Graph->GetPathName());
    Result->SetStringField(TEXT("node_name"), Node->GetName());
    Result->SetStringField(TEXT("selector_class"), WeightedSelector->GetClass()->GetName());
    Result->SetStringField(TEXT("selector_parameters_path"), WeightedSelector->GetPathName());
    Result->SetNumberField(TEXT("entry_count"), ReadbackEntries.Num());
    Result->SetArrayField(TEXT("entries"), ReadbackEntries);
    Result->SetBoolField(TEXT("notified"), true);
    SaveGraphAsset(Graph, Result, FUnrealMCPCommonUtils::IsPersistRequested(Params));
    return Result;
}
