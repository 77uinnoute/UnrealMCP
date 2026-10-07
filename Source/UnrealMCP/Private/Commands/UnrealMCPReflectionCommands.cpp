#include "Commands/UnrealMCPReflectionCommands.h"

#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Commands/Blueprint/UnrealMCPBlueprintGraphOps.h"
#include "Core/MCPCommandRegistry.h"
#include "Reflection/MCPPropertyReflector.h"
#include "Reflection/MCPObjectPathResolver.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    /** Exact C++ name first, then a case/underscore insensitive match (same rule the write path uses). */
    FString NormalizePropertyToken(const FString& In)
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

    /** Exact C++ name first, then a case/underscore insensitive match (one rule for classes and structs). */
    FProperty* ResolveStructField(UStruct* Struct, const FString& PropertyName, TArray<FString>& OutCandidates)
    {
        OutCandidates.Reset();
        if (!Struct)
        {
            return nullptr;
        }

        FProperty* NormalizedMatch = nullptr;
        const FString Wanted = NormalizePropertyToken(PropertyName);
        for (TFieldIterator<FProperty> PropIt(Struct); PropIt; ++PropIt)
        {
            FProperty* Property = *PropIt;
            if (!Property)
            {
                continue;
            }
            OutCandidates.Add(Property->GetName());
            if (Property->GetName().Equals(PropertyName, ESearchCase::CaseSensitive))
            {
                return Property;
            }
            if (!NormalizedMatch && NormalizePropertyToken(Property->GetName()) == Wanted)
            {
                NormalizedMatch = Property;
            }
        }
        return NormalizedMatch;
    }

    FProperty* ResolveProperty(UClass* Class, const FString& PropertyName, TArray<FString>& OutCandidates)
    {
        return ResolveStructField(Class, PropertyName, OutCandidates);
    }

    /** One step of a property path: a field name plus an optional container subscript ("LodData[0]"). */
    struct FPathStep
    {
        FString Name;
        bool bHasSubscript = false;
        FString Subscript;
    };

    /** Split "LodData[0].PhysicalMeshData.WeightMaps[1].Values" into its steps. */
    bool ParsePropertyPath(const FString& Path, TArray<FPathStep>& OutSteps, FString& OutErrorMessage)
    {
        OutSteps.Reset();

        FString Remaining = Path.TrimStartAndEnd();
        while (!Remaining.IsEmpty())
        {
            int32 Bracket = INDEX_NONE;
            int32 Dot = INDEX_NONE;
            Remaining.FindChar(TEXT('['), Bracket);
            Remaining.FindChar(TEXT('.'), Dot);
            const bool bHasSubscript = Bracket != INDEX_NONE && (Dot == INDEX_NONE || Bracket < Dot);

            FPathStep Step;
            Step.Name = (bHasSubscript ? Remaining.Left(Bracket)
                                       : (Dot == INDEX_NONE ? Remaining : Remaining.Left(Dot))).TrimStartAndEnd();
            if (Step.Name.IsEmpty())
            {
                OutErrorMessage = FString::Printf(TEXT("Property path '%s' has an empty segment"), *Path);
                return false;
            }

            if (bHasSubscript)
            {
                const int32 Close = Remaining.Find(TEXT("]"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Bracket);
                if (Close == INDEX_NONE)
                {
                    OutErrorMessage = FString::Printf(TEXT("Property path '%s' has an unclosed subscript"), *Path);
                    return false;
                }
                Step.bHasSubscript = true;
                Step.Subscript = Remaining.Mid(Bracket + 1, Close - Bracket - 1).TrimStartAndEnd();
                if (Step.Subscript.IsEmpty())
                {
                    OutErrorMessage = FString::Printf(TEXT("Property path '%s' has an empty subscript"), *Path);
                    return false;
                }
                Remaining = Remaining.Mid(Close + 1);
                Remaining.RemoveFromStart(TEXT("."));
            }
            else if (Dot != INDEX_NONE)
            {
                Remaining = Remaining.Mid(Dot + 1);
            }
            else
            {
                Remaining.Empty();
            }

            OutSteps.Add(Step);
        }

        if (OutSteps.Num() == 0)
        {
            OutErrorMessage = FString::Printf(TEXT("Property path '%s' is empty"), *Path);
            return false;
        }
        return true;
    }

    /** FNumericProperty does not expose signedness, so ask the concrete property class. */
    bool IsSignedIntegerProperty(const FProperty* Property)
    {
        return Property->IsA<FIntProperty>() || Property->IsA<FInt64Property>()
            || Property->IsA<FInt16Property>() || Property->IsA<FInt8Property>();
    }

    /** Text form of a map key, used both to match a subscript and to list the legal keys on a miss. */
    FString MapKeyToText(FProperty* KeyProperty, const void* KeyAddress)
    {
        if (const FNumericProperty* Numeric = CastField<FNumericProperty>(KeyProperty))
        {
            if (Numeric->IsInteger())
            {
                return IsSignedIntegerProperty(KeyProperty)
                    ? FString::Printf(TEXT("%lld"), Numeric->GetSignedIntPropertyValue(KeyAddress))
                    : FString::Printf(TEXT("%llu"), Numeric->GetUnsignedIntPropertyValue(KeyAddress));
            }
        }
        if (const FNameProperty* NameProperty = CastField<FNameProperty>(KeyProperty))
        {
            return NameProperty->GetPropertyValue(KeyAddress).ToString();
        }
        if (const FStrProperty* StringProperty = CastField<FStrProperty>(KeyProperty))
        {
            return StringProperty->GetPropertyValue(KeyAddress);
        }
        if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(KeyProperty))
        {
            const FNumericProperty* Underlying = EnumProperty->GetUnderlyingProperty();
            if (Underlying && EnumProperty->GetEnum())
            {
                return EnumProperty->GetEnum()->GetNameStringByValue(Underlying->GetSignedIntPropertyValue(KeyAddress));
            }
        }
        return FString();
    }

    bool MapKeyMatchesSubscript(FProperty* KeyProperty, const void* KeyAddress, const FString& Subscript)
    {
        const FString KeyText = MapKeyToText(KeyProperty, KeyAddress);
        if (!KeyText.IsEmpty() && KeyText.Equals(Subscript, ESearchCase::IgnoreCase))
        {
            return true;
        }
        // "1" and "01" address the same entry, and a target enum key accepts its numeric value too.
        if (const FNumericProperty* Numeric = CastField<FNumericProperty>(KeyProperty))
        {
            if (Numeric->IsInteger() && Subscript.IsNumeric())
            {
                return IsSignedIntegerProperty(KeyProperty)
                    ? Numeric->GetSignedIntPropertyValue(KeyAddress) == FCString::Atoi64(*Subscript)
                    : Numeric->GetUnsignedIntPropertyValue(KeyAddress) == FCString::Strtoui64(*Subscript, nullptr, 10);
            }
        }
        if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(KeyProperty))
        {
            const FNumericProperty* Underlying = EnumProperty->GetUnderlyingProperty();
            if (Underlying && Subscript.IsNumeric())
            {
                return Underlying->GetSignedIntPropertyValue(KeyAddress) == FCString::Atoi64(*Subscript);
            }
        }
        return false;
    }

    /** Apply "[i]" (array index) or "[key]" (map key) to a container property. */
    bool ResolveSubscript(FProperty* Property, void* Address, const FString& Subscript, FProperty*& OutProperty,
                          void*& OutAddress, FString& OutErrorMessage)
    {
        if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
        {
            if (!Subscript.IsNumeric())
            {
                OutErrorMessage = FString::Printf(TEXT("'%s' is an array; the subscript must be an index"),
                    *Property->GetName());
                return false;
            }

            const int32 Index = FCString::Atoi(*Subscript);
            FScriptArrayHelper Helper(ArrayProperty, Address);
            if (!Helper.IsValidIndex(Index))
            {
                OutErrorMessage = FString::Printf(TEXT("Index %d is out of range for '%s' (%d elements)"),
                    Index, *Property->GetName(), Helper.Num());
                return false;
            }

            OutProperty = ArrayProperty->Inner;
            OutAddress = Helper.GetRawPtr(Index);
            return true;
        }

        // C-style fixed-size arrays (`FBlendParameter BlendParameters[3]`) are ONE property with ArrayDim
        // slots - there is no FArrayProperty to unwrap, the slots sit at a fixed stride inside the
        // property's own memory. Without this branch every such property answered "is not an array or a
        // map" and the subscript did not apply, which is how UBlendSpace::BlendParameters stayed out of
        // reach of the reflector.
        if (Property->ArrayDim > 1)
        {
            if (!Subscript.IsNumeric())
            {
                OutErrorMessage = FString::Printf(TEXT("'%s' is a fixed-size array; the subscript must be an index"),
                    *Property->GetName());
                return false;
            }

            const int32 Index = FCString::Atoi(*Subscript);
            if (Index < 0 || Index >= Property->ArrayDim)
            {
                OutErrorMessage = FString::Printf(TEXT("Index %d is out of range for '%s' (%d elements)"),
                    Index, *Property->GetName(), Property->ArrayDim);
                return false;
            }

            OutProperty = Property;
            OutAddress = static_cast<uint8*>(Address) + static_cast<int64>(Index) * Property->ElementSize;
            return true;
        }

        if (FMapProperty* MapProperty = CastField<FMapProperty>(Property))
        {
            FScriptMapHelper Helper(MapProperty, Address);
            TArray<FString> Keys;
            for (int32 Index = 0; Index < Helper.Num(); ++Index)
            {
                if (!Helper.IsValidIndex(Index))
                {
                    continue;
                }
                const void* KeyAddress = Helper.GetKeyPtr(Index);
                Keys.Add(MapKeyToText(MapProperty->KeyProp, KeyAddress));
                if (MapKeyMatchesSubscript(MapProperty->KeyProp, KeyAddress, Subscript))
                {
                    OutProperty = MapProperty->ValueProp;
                    OutAddress = Helper.GetValuePtr(Index);
                    return true;
                }
            }

            OutErrorMessage = FString::Printf(TEXT("'%s' has no key '%s' (keys: %s)"),
                *Property->GetName(), *Subscript, *FString::Join(Keys, TEXT(", ")));
            return false;
        }

        OutErrorMessage = FString::Printf(TEXT("'%s' is not an array or a map; the subscript [%s] does not apply"),
            *Property->GetName(), *Subscript);
        return false;
    }

    /**
     * Resolve a dotted/indexed property path to its leaf property and value address.
     *
     * A path is the only way to reach data that is not a property of the target object itself - the cloth
     * mask at "LodData[0].PhysicalMeshData.WeightMaps[1].Values" being the case that forced it: python cannot
     * see the clothing asset's LOD data at all, and no amount of object-path resolution reaches a leaf
     * inside it. Intermediate segments may be structs, objects, arrays or maps.
     */
    bool ResolvePropertyPath(UObject* RootObject, const FString& Path, FProperty*& OutProperty,
                             void*& OutAddress, FString& OutErrorCode, FString& OutErrorMessage,
                             TArray<FString>& OutCandidates)
    {
        OutProperty = nullptr;
        OutAddress = nullptr;
        OutCandidates.Reset();

        TArray<FPathStep> Steps;
        if (!ParsePropertyPath(Path, Steps, OutErrorMessage))
        {
            OutErrorCode = TEXT("invalid_params");
            return false;
        }

        // Walk from the object's own class down: Base is the instance the current segment lives in.
        UStruct* Scope = RootObject->GetClass();
        void* Base = RootObject;

        for (int32 StepIndex = 0; StepIndex < Steps.Num(); ++StepIndex)
        {
            const FPathStep& Step = Steps[StepIndex];

            TArray<FString> Candidates;
            FProperty* Property = ResolveStructField(Scope, Step.Name, Candidates);
            if (!Property)
            {
                OutErrorCode = TEXT("unknown_property");
                OutErrorMessage = FString::Printf(TEXT("'%s' not found in %s along path '%s'"),
                    *Step.Name, *Scope->GetName(), *Path);
                OutCandidates = Candidates;  // The legal names at the step that failed.
                return false;
            }

            void* Address = Property->ContainerPtrToValuePtr<void>(Base);

            if (Step.bHasSubscript)
            {
                FProperty* ElementProperty = nullptr;
                void* ElementAddress = nullptr;
                if (!ResolveSubscript(Property, Address, Step.Subscript, ElementProperty, ElementAddress, OutErrorMessage))
                {
                    OutErrorCode = TEXT("unknown_field");
                    OutErrorMessage = FString::Printf(TEXT("%s (path '%s')"), *OutErrorMessage, *Path);
                    return false;
                }
                Property = ElementProperty;
                Address = ElementAddress;
            }

            if (StepIndex == Steps.Num() - 1)
            {
                OutProperty = Property;
                OutAddress = Address;
                return true;
            }

            // Descend into the value so the next segment resolves inside it.
            if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
            {
                Scope = StructProperty->Struct;
                Base = Address;
                continue;
            }
            if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
            {
                UObject* Inner = ObjectProperty->GetObjectPropertyValue(Address);
                if (!Inner)
                {
                    OutErrorCode = TEXT("load_failed");
                    OutErrorMessage = FString::Printf(TEXT("'%s' is null, so path '%s' cannot continue"),
                        *Step.Name, *Path);
                    return false;
                }
                Scope = Inner->GetClass();
                Base = Inner;
                continue;
            }

            OutErrorCode = TEXT("unknown_field");
            OutErrorMessage = CastField<FArrayProperty>(Property) || CastField<FMapProperty>(Property)
                ? FString::Printf(TEXT("'%s' is a container: path '%s' needs a subscript like [0] before the next segment"),
                    *Step.Name, *Path)
                : FString::Printf(TEXT("'%s' (%s) cannot be descended into (path '%s')"),
                    *Step.Name, *Property->GetClass()->GetName(), *Path);
            return false;
        }

        OutErrorCode = TEXT("invalid_params");
        OutErrorMessage = FString::Printf(TEXT("Property path '%s' resolved to nothing"), *Path);
        return false;
    }

    /** Compact JSON text of a value, used only to tell "changed" from "written but clamped back". */
    FString JsonValueToComparableString(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid())
        {
            return FString();
        }
        FString Out;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
        FJsonSerializer::Serialize(Value, TEXT(""), Writer);
        return Out;
    }

    /**
     * Follow-up command a write needs before it means anything at runtime.
     *
     * A write that lands in the asset while the runtime data stays stale is the hardest failure to
     * attribute: the editor looks right, the read-back is right, and only PIE shows the symptom (an
     * empty blend = a T-pose). These pairs are stated in the reply instead of being folklore.
     */
    FString DependentCommandHint(const FString& RequestedProperty)
    {
        if (RequestedProperty.StartsWith(TEXT("sample_data"), ESearchCase::IgnoreCase))
        {
            return TEXT("BlendSpace samples written: the runtime segment/triangle table stays stale until "
                        "finalize_blend_space runs, and the blend output is empty (a T-pose in PIE) until it does. "
                        "set_blend_space_samples does the write and the finalize in one call.");
        }
        if (RequestedProperty.StartsWith(TEXT("blend_parameters"), ESearchCase::IgnoreCase))
        {
            return TEXT("BlendSpace grid changed: the samples have to be realigned with the new axes and "
                        "finalize_blend_space has to run, or the runtime data stays empty.");
        }
        return FString();
    }

    /** The UObject a probe addresses: a graph node, a component template, a material expression, or an asset/CDO. */
    bool ResolveProbeTarget(const TSharedPtr<FJsonObject>& Params, UObject*& OutObject,
                            FString& OutErrorCode, FString& OutErrorMessage, TArray<FString>& OutCandidates)
    {
        OutObject = nullptr;
        OutCandidates.Reset();

        FString Target;
        Params->TryGetStringField(TEXT("target"), Target);

        FString NodeId;
        const bool bHasNodeId = Params->TryGetStringField(TEXT("node_id"), NodeId) && !NodeId.IsEmpty();

        FString ComponentName;
        const bool bHasComponent = Params->TryGetStringField(TEXT("component_name"), ComponentName) && !ComponentName.IsEmpty();

        FString ExpressionName;
        const bool bHasExpression = Params->TryGetStringField(TEXT("expression_name"), ExpressionName) && !ExpressionName.IsEmpty();

        // --- node / component: the target names a blueprint -------------------------------------
        if (bHasNodeId || bHasComponent)
        {
            UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(Target);
            if (!Blueprint)
            {
                OutErrorCode = TEXT("blueprint_not_found");
                OutErrorMessage = FString::Printf(TEXT("Blueprint '%s' not found"), *Target);
                return false;
            }

            if (bHasComponent)
            {
                UActorComponent* ComponentTemplate = FUnrealMCPCommonUtils::FindWritableComponentTemplate(
                    Blueprint, ComponentName, OutErrorCode, OutErrorMessage);
                if (!ComponentTemplate)
                {
                    if (OutErrorCode.IsEmpty())
                    {
                        OutErrorCode = TEXT("component_not_found");
                    }
                    return false;
                }
                OutObject = ComponentTemplate;
                return true;
            }

            FString GraphName;
            Params->TryGetStringField(TEXT("graph_name"), GraphName);
            UEdGraph* Graph = nullptr;
            // Read-only: never create the event graph while probing.
            if (!FUnrealMCPBlueprintGraphOps::ResolveGraph(Blueprint, GraphName, /*bCreateEventGraphIfMissing=*/false,
                    Graph, OutErrorCode, OutErrorMessage, OutCandidates))
            {
                return false;
            }

            UEdGraphNode* Node = FUnrealMCPBlueprintGraphOps::FindNodeByGuid(Graph, NodeId);
            if (!Node)
            {
                OutErrorCode = EUnrealMCPGraphError::NodeNotFound;
                OutErrorMessage = FString::Printf(TEXT("Node '%s' not found in graph '%s'"), *NodeId, *Graph->GetName());
                return false;
            }
            OutObject = Node;
            return true;
        }

        // --- material expression: the target names a material ------------------------------------
        if (bHasExpression)
        {
            UObject* Asset = FUnrealMCPCommonUtils::FindAsset(Target);
            UMaterial* Material = Cast<UMaterial>(Asset);
            if (!Material)
            {
                OutErrorCode = TEXT("material_not_found");
                OutErrorMessage = FString::Printf(TEXT("Material '%s' not found"), *Target);
                return false;
            }

            for (UMaterialExpression* Expression : Material->GetExpressions())
            {
                if (!Expression)
                {
                    continue;
                }
                OutCandidates.Add(Expression->GetName());
                if (Expression->GetName() == ExpressionName || Expression->GetDescription() == ExpressionName)
                {
                    OutObject = Expression;
                    return true;
                }
            }
            OutErrorCode = TEXT("expression_not_found");
            OutErrorMessage = FString::Printf(TEXT("Expression '%s' not found in material '%s'"), *ExpressionName, *Target);
            return false;
        }

        // --- plain asset ------------------------------------------------------------------------
        // A sub-object path ("/Game/X.X:Sub.Name") must not go through FindAsset: it stops at the outermost
        // asset, which is the degradation that kept a cloth config object out of reach of the probe. Both
        // shapes are resolved by the same walker the writer uses, so probe and write always agree.
        TArray<FString> TriedPaths;
        if (UObject* Asset = FMCPObjectPathResolver::ResolveObject(Target, TriedPaths))
        {
            // LoadObject resolves "/Script/Engine.Actor" to the UClass object itself, so a class target has
            // to fall through to its default object rather than being probed as a UClass instance.
            if (UClass* AssetClass = Cast<UClass>(Asset))
            {
                OutObject = AssetClass->GetDefaultObject();
                if (!OutObject)
                {
                    OutErrorCode = TEXT("load_failed");
                    OutErrorMessage = FString::Printf(TEXT("Class '%s' has no default object"), *Target);
                    return false;
                }
                return true;
            }
            OutObject = Asset;
            return true;
        }

        // --- class: probe the CDO so a type can be inspected without an instance -----------------
        UClass* Class = nullptr;
        if (FUnrealMCPBlueprintGraphOps::ResolveClass(Target, Class, OutCandidates) && Class)
        {
            OutObject = Class->GetDefaultObject();
            if (!OutObject)
            {
                OutErrorCode = TEXT("load_failed");
                OutErrorMessage = FString::Printf(TEXT("Class '%s' has no default object"), *Target);
                return false;
            }
            return true;
        }

        const FString PieCode = FUnrealMCPCommonUtils::ClassifyAssetLoadFailure(Target);
        OutErrorCode = Target.IsEmpty() ? TEXT("invalid_params") : (PieCode.IsEmpty() ? FString(TEXT("load_failed")) : PieCode);
        OutErrorMessage = Target.IsEmpty()
            ? TEXT("Missing 'target' parameter")
            : FString::Printf(TEXT("Could not resolve target '%s' as an asset, a class, or a sub-object (tried: %s)"),
                *Target, *FString::Join(TriedPaths, TEXT(", ")));
        return false;
    }
}

FUnrealMCPReflectionCommands::FUnrealMCPReflectionCommands()
{
}

void FUnrealMCPReflectionCommands::RegisterCommands(FMCPCommandRegistry& Registry)
{
    MCP_REGISTER_COMMAND(Registry, "reflect_probe", "reflection",
        "Describe a property on a target object: type, supported shapes, edit flags and current value.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("target"), TEXT("string"), TEXT("Asset, blueprint, material, class, or sub-object path/name to probe")),
            MCPParam(TEXT("property"), TEXT("string"), TEXT("Property name (C++ name; snake_case resolves) or a path like \"LodData[0].PhysicalMeshData.WeightMaps[1].Values\"")),
            MCPParamOpt(TEXT("node_id"), TEXT("string"), TEXT("Graph node GUID, for a blueprint node target")),
            MCPParamOpt(TEXT("component_name"), TEXT("string"), TEXT("Component name, for a blueprint component template target")),
            MCPParamOpt(TEXT("graph_name"), TEXT("string"), TEXT("Graph name for a node probe; defaults to the event graph")),
            MCPParamOpt(TEXT("expression_name"), TEXT("string"), TEXT("Expression name, for a material expression target")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleReflectProbe(Params); });

    MCP_REGISTER_COMMAND(Registry, "set_object_property", "reflection",
        "Write properties on any object (asset, class or sub-object) with one optional save. The object selector "
        "is 'target' - the same word reflect_probe uses, because the two are used as a pair. 'object_path' still "
        "works as a deprecated alias, and using it is reported back in renamed_params[].",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("target"), TEXT("string"), TEXT("Asset, class, or sub-object path to write, e.g. \"/Game/X.X:A.Config\"")),
            MCPParamOpt(TEXT("object_path"), TEXT("string"), TEXT("Deprecated alias of 'target' (same meaning); the response echoes it in renamed_params[]")),
            MCPParamOpt(TEXT("property_name"), TEXT("string"), TEXT("Single-property form: property name (snake_case resolves) or a path like \"LodData[0].PhysicalMeshData.WeightMaps[1].Values\"")),
            MCPParamOpt(TEXT("property_value"), TEXT("any"), TEXT("Single-property form: value in the shape reflect_probe reports")),
            MCPParamOpt(TEXT("properties"), TEXT("object"), TEXT("Batch form: object of property name (or path) -> value, written with ONE save")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save the owning package once after the writes; default true")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleSetObjectProperty(Params); });

    MCP_REGISTER_COMMAND(Registry, "list_enum_values", "reflection",
        "List a UENUM's values through its own reflection data (name / display name / numeric value). "
        "A short name is resolved against the common script modules.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("enum_name"), TEXT("string"), TEXT("Enum short name or full object path")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleListEnumValues(Params); });
}

TSharedPtr<FJsonObject> FUnrealMCPReflectionCommands::HandleReflectProbe(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing command parameters"));
    }

    FString PropertyName;
    if (!Params->TryGetStringField(TEXT("property"), PropertyName) || PropertyName.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'property' parameter"));
    }

    UObject* TargetObject = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    TArray<FString> Candidates;
    if (!ResolveProbeTarget(Params, TargetObject, ErrorCode, ErrorMessage, Candidates))
    {
        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(ErrorCode, ErrorMessage);
        if (Candidates.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> CandidateValues;
            for (const FString& Candidate : Candidates)
            {
                CandidateValues.Add(MakeShared<FJsonValueString>(Candidate));
            }
            Error->SetArrayField(TEXT("candidates"), CandidateValues);
        }
        return Error;
    }

    TArray<FString> PropertyCandidates;
    FProperty* Property = nullptr;
    void* PropertyAddress = nullptr;
    FString PropertyErrorCode;
    FString PropertyErrorMessage;
    if (!ResolvePropertyPath(TargetObject, PropertyName, Property, PropertyAddress, PropertyErrorCode,
            PropertyErrorMessage, PropertyCandidates))
    {
        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(PropertyErrorCode, PropertyErrorMessage);
        TArray<TSharedPtr<FJsonValue>> CandidateValues;
        for (const FString& Candidate : PropertyCandidates)
        {
            CandidateValues.Add(MakeShared<FJsonValueString>(Candidate));
        }
        Error->SetArrayField(TEXT("candidates"), CandidateValues);
        return Error;
    }

    const FPropertyDescriptor Descriptor = FMCPPropertyReflector::Describe(Property);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("target"), TargetObject->GetPathName());
    Data->SetStringField(TEXT("target_class"), TargetObject->GetClass()->GetName());
    Data->SetStringField(TEXT("property"), Property->GetName());
    if (!Property->GetName().Equals(PropertyName, ESearchCase::IgnoreCase))
    {
        Data->SetStringField(TEXT("property_path"), PropertyName);  // Echo the path that reached this leaf.
    }
    Data->SetStringField(TEXT("property_type"), Descriptor.CppType);
    Data->SetStringField(TEXT("property_class"), Descriptor.PropertyClass);

    if (!Descriptor.Container.IsEmpty())
    {
        Data->SetStringField(TEXT("container"), Descriptor.Container);
    }
    if (!Descriptor.ElementType.IsEmpty())
    {
        Data->SetStringField(TEXT("element_type"), Descriptor.ElementType);
    }
    if (!Descriptor.ValueType.IsEmpty())
    {
        Data->SetStringField(TEXT("value_type"), Descriptor.ValueType);
    }
    if (!Descriptor.Semantics.IsEmpty())
    {
        Data->SetStringField(TEXT("semantics"), Descriptor.Semantics);
    }

    Data->SetBoolField(TEXT("supported"), Descriptor.bSupported);
    Data->SetBoolField(TEXT("has_codec"), Descriptor.bHasCodec);
    Data->SetBoolField(TEXT("editable"), Property->HasAnyPropertyFlags(CPF_Edit));
    Data->SetBoolField(TEXT("edit_const"), Property->HasAnyPropertyFlags(CPF_EditConst));
    Data->SetBoolField(TEXT("transient"), Property->HasAnyPropertyFlags(CPF_Transient));

    TArray<TSharedPtr<FJsonValue>> Shapes;
    for (const FString& Shape : Descriptor.SupportedShapes)
    {
        Shapes.Add(MakeShared<FJsonValueString>(Shape));
    }
    Data->SetArrayField(TEXT("supported_shapes"), Shapes);

    // The verdict a caller needs BEFORE writing: whether the writer will accept this property, phrased
    // in the same rule the writer uses (nested-path form when the probe itself was addressed by path).
    // `supported_shapes` alone does not answer it - a property can be type-supported and still refused
    // for its edit flags, which is what made writing SampleData a two-attempt guess.
    const bool bProbedByPath = PropertyName.Contains(TEXT(".")) || PropertyName.Contains(TEXT("["));
    FString WritableDetail;
    const bool bWritable = FMCPPropertyReflector::IsWritableAndSupported(Property, bProbedByPath, WritableDetail);
    Data->SetBoolField(TEXT("writable"), bWritable);
    if (!WritableDetail.IsEmpty())
    {
        Data->SetStringField(TEXT("writable_detail"), WritableDetail);
    }
    if (!Descriptor.Shape.IsEmpty())
    {
        Data->SetStringField(TEXT("shape"), Descriptor.Shape);
    }
    FUnrealMCPCommonUtils::AddStringArrayField(Data, TEXT("element_fields"), Descriptor.ElementFields);

    if (!Descriptor.Hint.IsEmpty())
    {
        Data->SetStringField(TEXT("hint"), Descriptor.Hint);
    }

    // A pin rebuild can only be judged after a write, so the probe never promises true/false here.
    Data->SetStringField(TEXT("triggers_pin_rebuild"), TEXT("unknown"));

    const TSharedPtr<FJsonValue> CurrentValue = FMCPPropertyReflector::ToJson(Property, PropertyAddress);
    if (CurrentValue.IsValid())
    {
        Data->SetField(TEXT("current_value"), CurrentValue);
    }

    return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
}

TSharedPtr<FJsonObject> FUnrealMCPReflectionCommands::HandleSetObjectProperty(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing command parameters"));
    }

    // --- the write list: the single-property form and the batch form may both be given -----------------
    struct FPendingWrite
    {
        FString RequestedName;
        TSharedPtr<FJsonValue> Value;
    };
    TArray<FPendingWrite> Writes;

    FString SingleName;
    if (Params->TryGetStringField(TEXT("property_name"), SingleName) && !SingleName.IsEmpty())
    {
        const TSharedPtr<FJsonValue>* SingleValue = Params->Values.Find(TEXT("property_value"));
        if (!SingleValue || !SingleValue->IsValid())
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"),
                TEXT("'property_value' is required when 'property_name' is given"));
        }
        Writes.Add({ SingleName, *SingleValue });
    }

    const TSharedPtr<FJsonObject>* PropsObject = nullptr;
    if (Params->TryGetObjectField(TEXT("properties"), PropsObject) && PropsObject->IsValid())
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PropsObject)->Values)
        {
            Writes.Add({ Pair.Key, Pair.Value });
        }
    }

    if (Writes.Num() == 0)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"),
            TEXT("Nothing to write: pass 'property_name' + 'property_value', or a non-empty 'properties' object"));
    }

    // The object selector is `target` - the same word reflect_probe uses, because these two are used as a
    // pair (probe, then write). `object_path` was the old spelling and still works: one concept, one name,
    // with a compatibility window that reports itself back so callers migrate.
    FString TargetKey = TEXT("target");
    bool bUsedRenamedParam = false;
    {
        FString NewValue;
        FString OldValue;
        const bool bHasNew = Params->TryGetStringField(TEXT("target"), NewValue);
        const bool bHasOld = Params->TryGetStringField(TEXT("object_path"), OldValue);
        if (bHasNew && bHasOld && !NewValue.Equals(OldValue))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"),
                TEXT("'target' and its deprecated alias 'object_path' were both given with different values; "
                     "pass only one of them"));
        }
        if (!bHasNew && bHasOld)
        {
            TargetKey = TEXT("object_path");
            bUsedRenamedParam = true;
        }
    }

    UObject* TargetObject = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    TArray<FString> TriedPaths;
    if (!FMCPObjectPathResolver::ResolveFromParams(Params, *TargetKey, TargetObject, ErrorCode,
                                                   ErrorMessage, TriedPaths))
    {
        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(ErrorCode, ErrorMessage);
        FUnrealMCPCommonUtils::AddStringArrayField(Error, TEXT("tried"), TriedPaths);
        return Error;
    }

    bool bPersist = true;
    Params->TryGetBoolField(TEXT("persist"), bPersist);

    TargetObject->Modify();

    TArray<TSharedPtr<FJsonValue>> WritesJson;
    TArray<TSharedPtr<FJsonValue>> FailedJson;
    FString ResponseHint;

    for (const FPendingWrite& Pending : Writes)
    {
        // A name may be a plain property or a path ("LodData[0].PhysicalMeshData.WeightMaps[1].Values");
        // both go through the same resolver so the accepted spelling cannot drift between them.
        TArray<FString> PropertyCandidates;
        FProperty* Property = nullptr;
        void* PropertyAddr = nullptr;
        FString ResolveErrorCode;
        FString ResolveErrorMessage;
        const bool bNestedPath = Pending.RequestedName.Contains(TEXT(".")) || Pending.RequestedName.Contains(TEXT("["));
        if (!ResolvePropertyPath(TargetObject, Pending.RequestedName, Property, PropertyAddr, ResolveErrorCode,
                ResolveErrorMessage, PropertyCandidates))
        {
            TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
            Failure->SetStringField(TEXT("property_name"), Pending.RequestedName);
            Failure->SetStringField(TEXT("error_code"),
                ResolveErrorCode.IsEmpty() ? TEXT("unknown_property") : ResolveErrorCode);
            Failure->SetStringField(TEXT("error"), ResolveErrorMessage);
            FUnrealMCPCommonUtils::AddStringArrayField(Failure, TEXT("candidates"), PropertyCandidates);
            FailedJson.Add(MakeShared<FJsonValueObject>(Failure));
            continue;
        }

        // Same gate set_asset_properties applies: reflection reaches engine-managed data, and a write to it
        // looks like it worked while nothing ever consumes the value. A nested path is exempt from the
        // "editable" half because baked data (FPointWeightMap.Values) carries no Edit specifier at all -
        // addressing it down to the leaf IS the explicit opt-in - but EditConst stays refused.
        // The rule itself lives in the reflector so `reflect_probe` reports the same verdict.
        FString WritableDetail;
        const bool bWritable = FMCPPropertyReflector::IsWritable(Property, bNestedPath, WritableDetail);
        if (!bWritable)
        {
            TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
            Failure->SetStringField(TEXT("property_name"), Property->GetName());
            Failure->SetStringField(TEXT("error_code"), TEXT("property_not_writable"));
            Failure->SetStringField(TEXT("writable_detail"), WritableDetail);
            Failure->SetStringField(TEXT("error"), FString::Printf(TEXT("'%s' is not editable (%s); it is engine-managed data"),
                *Property->GetName(), *Property->GetClass()->GetName()));
            FailedJson.Add(MakeShared<FJsonValueObject>(Failure));
            continue;
        }

        const TSharedPtr<FJsonValue> ValueBefore = FUnrealMCPCommonUtils::PropertyValueToJson(Property, PropertyAddr);

        // The reflector owns the type dispatch and the address arithmetic, so a leaf inside a container
        // takes exactly the shapes the same property would take on an object of its own.
        const FWriteResult WriteResult = FMCPPropertyReflector::FromJson(
            Property, PropertyAddr, Pending.RequestedName, Pending.Value);
        FString WriteErrorCode = WriteResult.ErrorCode;
        FString WriteError = WriteResult.ErrorMessage;
        if (!WriteResult.bSuccess)
        {
            if (WriteErrorCode.IsEmpty())
            {
                WriteErrorCode = TEXT("write_failed");
            }
            if (WriteError.IsEmpty())
            {
                WriteError = FString::Printf(TEXT("Write to '%s' was refused"), *Pending.RequestedName);
            }
            TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
            Failure->SetStringField(TEXT("property_name"), Property->GetName());
            if (bNestedPath)
            {
                Failure->SetStringField(TEXT("property_path"), Pending.RequestedName);
            }
            Failure->SetStringField(TEXT("error_code"), WriteErrorCode);
            Failure->SetStringField(TEXT("error"), WriteError);
            FUnrealMCPCommonUtils::AddStringArrayField(Failure, TEXT("candidates"), WriteResult.Candidates);
            FUnrealMCPCommonUtils::AddStringArrayField(Failure, TEXT("available_fields"), WriteResult.AvailableFields);
            FUnrealMCPCommonUtils::AddStringArrayField(Failure, TEXT("supported_shapes"), WriteResult.SupportedShapes);
            if (WriteResult.FailedIndex != INDEX_NONE)
            {
                Failure->SetNumberField(TEXT("failed_index"), WriteResult.FailedIndex);
            }
            Failure->SetBoolField(TEXT("unchanged"), WriteResult.bUnchanged);
            if (ValueBefore.IsValid())
            {
                Failure->SetField(TEXT("property_value_before"), ValueBefore);
            }
            FailedJson.Add(MakeShared<FJsonValueObject>(Failure));
            continue;
        }

        // UPROPERTY(Setter = ...) fields only mirror the state the engine reads, and the details-panel seam
        // is what a cloth/physics style consumer re-reads, so both notifications are part of the write.
        FUnrealMCPCommonUtils::InvokePropertySetter(TargetObject, Property, PropertyAddr);
        FPropertyChangedEvent ChangeEvent(Property);
        TargetObject->PostEditChangeProperty(ChangeEvent);

        const TSharedPtr<FJsonValue> ValueAfter = FUnrealMCPCommonUtils::PropertyValueToJson(Property, PropertyAddr);

        TSharedPtr<FJsonObject> Written = MakeShared<FJsonObject>();
        Written->SetStringField(TEXT("property_name"), Property->GetName());
        if (bNestedPath)
        {
            Written->SetStringField(TEXT("property_path"), Pending.RequestedName);
        }
        Written->SetStringField(TEXT("property_type"), FMCPPropertyReflector::Describe(Property).CppType);
        if (ValueBefore.IsValid())
        {
            Written->SetField(TEXT("property_value_before"), ValueBefore);
        }
        if (ValueAfter.IsValid())
        {
            Written->SetField(TEXT("property_value_after"), ValueAfter);
        }
        // A setter may clamp or refuse silently, so "changed" compares the read-backs rather than the requests.
        Written->SetBoolField(TEXT("changed"),
            JsonValueToComparableString(ValueBefore) != JsonValueToComparableString(ValueAfter));

        // Read-only information: it never alters the write, it only says what else has to happen.
        if (const FString DependentHint = DependentCommandHint(Pending.RequestedName); !DependentHint.IsEmpty())
        {
            Written->SetStringField(TEXT("hint"), DependentHint);
            ResponseHint = DependentHint;
        }

        WritesJson.Add(MakeShared<FJsonValueObject>(Written));
    }

    // Exactly one save for the whole call: that is the point of the batch form (the editor is routinely
    // killed by the build script, and one-save-per-property turned a 5-property edit into 5 disk writes).
    bool bSaved = false;
    if (WritesJson.Num() > 0)
    {
        TargetObject->MarkPackageDirty();
        if (bPersist)
        {
            bSaved = FUnrealMCPCommonUtils::SaveAssetForObject(TargetObject);
        }
    }

    FString RequestedPath;
    Params->TryGetStringField(TargetKey, RequestedPath);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("requested_object_path"), RequestedPath);
    Data->SetStringField(TEXT("object_path"), TargetObject->GetPathName());
    Data->SetStringField(TEXT("object_class"), TargetObject->GetClass()->GetName());
    Data->SetStringField(TEXT("object_class_path"), TargetObject->GetClass()->GetPathName());
    Data->SetArrayField(TEXT("writes"), WritesJson);
    Data->SetNumberField(TEXT("written_count"), WritesJson.Num());
    Data->SetArrayField(TEXT("failed"), FailedJson);
    Data->SetNumberField(TEXT("failed_count"), FailedJson.Num());
    Data->SetBoolField(TEXT("persist_requested"), bPersist);
    Data->SetBoolField(TEXT("saved"), bSaved);
    if (!ResponseHint.IsEmpty())
    {
        Data->SetStringField(TEXT("hint"), ResponseHint);
    }
    if (bUsedRenamedParam)
    {
        TSharedPtr<FJsonObject> Renamed = MakeShared<FJsonObject>();
        Renamed->SetStringField(TEXT("old"), TEXT("object_path"));
        Renamed->SetStringField(TEXT("new"), TEXT("target"));
        TArray<TSharedPtr<FJsonValue>> RenamedArray;
        RenamedArray.Add(MakeShared<FJsonValueObject>(Renamed));
        Data->SetArrayField(TEXT("renamed_params"), RenamedArray);
    }

    return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
}


// list_enum_values: reflection over a UENUM. python could only do this with dir(unreal.<Enum>),
// which is unavailable inside a script that has no unreal python binding for the enum (cloth and
// plugin enums are the common case) - here the enum is found through its own reflection data, so
// the same command serves the tool surface and execute_python_* scripts.
TSharedPtr<FJsonObject> FUnrealMCPReflectionCommands::HandleListEnumValues(const TSharedPtr<FJsonObject>& Params)
{
    FString EnumName;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("enum_name"), EnumName) || EnumName.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"),
            TEXT("Missing 'enum_name' parameter"));
    }

    UEnum* Enum = nullptr;
    TArray<FString> Tried;
    Tried.Add(EnumName);
    // FindObject only sees already-loaded objects: an enum that nothing has touched yet (e.g.
    // EBlendableLocation) is not in memory, so the lookup loads it as a fallback.
    auto ResolveEnumCandidate = [](const FString& Candidate) -> UEnum*
    {
        if (UEnum* Found = FindObject<UEnum>(nullptr, *Candidate))
        {
            return Found;
        }
        return LoadObject<UEnum>(nullptr, *Candidate);
    };
    if (EnumName.Contains(TEXT(".")))
    {
        Enum = ResolveEnumCandidate(EnumName);
    }
    if (!Enum)
    {
        static const TCHAR* Prefixes[] = {
            TEXT("/Script/Engine."), TEXT("/Script/CoreUObject."), TEXT("/Script/UnrealEd."),
            TEXT("/Script/UMG."), TEXT("/Script/ClothingSystemRuntimeCommon.")
        };
        for (const TCHAR* Prefix : Prefixes)
        {
            const FString Candidate = FString(Prefix) + EnumName;
            Tried.Add(Candidate);
            Enum = ResolveEnumCandidate(Candidate);
            if (Enum)
            {
                break;
            }
        }
    }
    if (!Enum)
    {
        // Python spells these without the C++ E prefix ("BlendableLocation" for EBlendableLocation),
        // so a failed first pass retries with the prefix rather than making the caller guess.
        static const TCHAR* EPrefixes[] = {
            TEXT("/Script/Engine.E"), TEXT("/Script/CoreUObject.E"), TEXT("/Script/UnrealEd.E"),
            TEXT("/Script/UMG.E")
        };
        for (const TCHAR* Prefix : EPrefixes)
        {
            const FString Candidate = FString(Prefix) + EnumName;
            Tried.Add(Candidate);
            Enum = ResolveEnumCandidate(Candidate);
            if (Enum)
            {
                break;
            }
        }
    }
    if (!Enum)
    {
        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unknown_enum"),
            FString::Printf(TEXT("'%s' did not resolve to a UEnum; pass a full path like '/Script/Engine.TextureGroup'"),
                *EnumName));
        FUnrealMCPCommonUtils::AddStringArrayField(Error, TEXT("tried"), Tried);
        return Error;
    }

    TArray<TSharedPtr<FJsonValue>> Values;
    TArray<FString> Names;
    for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
    {
        const FString ShortName = Enum->GetNameStringByIndex(Index);
        // Skip the auto-generated count sentinel the UHT emits for every enum.
        if (ShortName.EndsWith(TEXT("_MAX")))
        {
            continue;
        }

        TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
        Value->SetStringField(TEXT("name"), ShortName);
        Value->SetStringField(TEXT("display_name"), Enum->GetDisplayNameTextByIndex(Index).ToString());
        Value->SetNumberField(TEXT("value"), static_cast<double>(Enum->GetValueByIndex(Index)));
        Values.Add(MakeShared<FJsonValueObject>(Value));
        Names.Add(ShortName);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("enum_name"), EnumName);
    Data->SetStringField(TEXT("enum_path"), Enum->GetPathName());
    Data->SetStringField(TEXT("cpp_name"), Enum->GetName());
    Data->SetNumberField(TEXT("count"), Values.Num());
    Data->SetArrayField(TEXT("values"), Values);
    // A flat name list keeps the old python contract (dir() returned exactly this).
    TArray<TSharedPtr<FJsonValue>> NameValues;
    for (const FString& Name : Names)
    {
        NameValues.Add(MakeShared<FJsonValueString>(Name));
    }
    Data->SetArrayField(TEXT("names"), NameValues);
    return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
}
