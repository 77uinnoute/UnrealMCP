#include "Commands/Material/UnrealMCPMaterialCommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Commands/Material/UnrealMCPMaterialHlslLint.h"
#include "Compat/UnrealMCPVersionCompat.h"
#include "Core/MCPCommandRegistry.h"
#include "Reflection/MCPPropertyCodecs.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Materials/Material.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialExpression.h"
#include "MaterialExpressionIO.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Factories/MaterialFactoryNew.h"
#include "MaterialShared.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "HAL/FileManager.h"
#include "Templates/Function.h"
#include "Internationalization/Regex.h"
#include "Editor.h"

//==============================================================================
// Property codecs for the material expression types the generic reflector cannot express.
// Registered from a file-scope object; the codec registry is backed by function-local statics, so this
// cannot race another translation unit's registration during static initialization.
//==============================================================================

namespace
{
    TArray<FString> MaterialCodecShapes(std::initializer_list<const TCHAR*> InShapes)
    {
        TArray<FString> Out;
        Out.Reserve(static_cast<int32>(InShapes.size()));
        for (const TCHAR* Shape : InShapes)
        {
            Out.Add(Shape);
        }
        return Out;
    }

    bool MatchesReflectedStruct(const FProperty* Property, const TCHAR* StructName)
    {
        const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
        return StructProperty && StructProperty->Struct &&
               StructProperty->Struct->GetFName() == FName(StructName);
    }

    struct FMaterialPropertyCodecRegistrar
    {
        FMaterialPropertyCodecRegistrar()
        {
            // FExpressionInput (Add.A, Multiply.B, ...): reading keeps the three-field connection
            // diagnostic the tools already document. Writing it is a policy rejection: an input is
            // wiring, and only connect_material_pin knows how to rewire it.
            {
                const TCHAR* const Shape = TEXT("{expression, output_index, input_name}");
                FPropertyCodec Codec;
                Codec.Matches = [](const FProperty* Property)
                {
                    return MatchesReflectedStruct(Property, TEXT("ExpressionInput"));
                };
                Codec.Shapes = MaterialCodecShapes({ Shape });
                Codec.ToJson = [](const FProperty* /*Property*/, const void* ValuePtr) -> TSharedPtr<FJsonValue>
                {
                    const FExpressionInput* Input = static_cast<const FExpressionInput*>(ValuePtr);
                    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
                    Object->SetStringField(TEXT("expression"),
                        Input->Expression ? Input->Expression->GetName() : TEXT(""));
                    Object->SetNumberField(TEXT("output_index"), Input->OutputIndex);
                    Object->SetStringField(TEXT("input_name"), Input->InputName.ToString());
                    return MakeShared<FJsonValueObject>(Object);
                };
                Codec.FromJson = [Shape](const FProperty*, void*, const TSharedPtr<FJsonValue>&,
                                         FWriteResult& Out) -> bool
                {
                    Out = FWriteResult::Failure(TEXT("unsupported_property_type"),
                        TEXT("FExpressionInput is wiring, not a value: use connect_material_pin"))
                        .WithShapes(MaterialCodecShapes({ Shape }))
                        .WithHint(TEXT("改用 connect_material_pin 连接该输入引脚"));
                    return false;
                };
                Codec.Hint = TEXT("改用 connect_material_pin 连接该输入引脚");
                Codec.bWritable = false;
                FMCPPropertyCodecs::RegisterPropertyCodec(MoveTemp(Codec));
            }

            // FCustomInput: the Custom node's Inputs array is edited one element at a time through
            // add_custom_input / remove_custom_input / set_custom_input_name. Those keep every
            // upstream FExpressionInput alive (the wiring lives in the element the new insert copies
            // around, never in a fresh blank one); assigning the whole array from a JSON payload
            // would rebuild the elements from scratch and drop the wiring they carry.
            {
                FPropertyCodec Codec;
                Codec.Matches = [](const FProperty* Property)
                {
                    return MatchesReflectedStruct(Property, TEXT("CustomInput"));
                };
                Codec.Shapes = MaterialCodecShapes({ TEXT("{input_name}") });
                Codec.ToJson = [](const FProperty*, const void* ValuePtr) -> TSharedPtr<FJsonValue>
                {
                    const FCustomInput* Input = static_cast<const FCustomInput*>(ValuePtr);
                    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
                    Object->SetStringField(TEXT("input_name"), Input->InputName.ToString());
                    return MakeShared<FJsonValueObject>(Object);
                };
                Codec.FromJson = [](const FProperty*, void*, const TSharedPtr<FJsonValue>&,
                                    FWriteResult& Out) -> bool
                {
                    Out = FWriteResult::Failure(TEXT("unsupported_property_type"),
                        TEXT("Custom inputs are added, removed or renamed through their own commands, not assigned"))
                        .WithHint(TEXT("改用 add_custom_input / remove_custom_input / set_custom_input_name 增删或重命名 Custom 输入引脚"));
                    return false;
                };
                Codec.Hint = TEXT("改用 add_custom_input / remove_custom_input / set_custom_input_name 增删或重命名 Custom 输入引脚");
                Codec.bWritable = false;
                FMCPPropertyCodecs::RegisterPropertyCodec(MoveTemp(Codec));
            }
        }
    };

    static FMaterialPropertyCodecRegistrar GMaterialPropertyCodecRegistrar;
}

// Material property pins addressable by name (shared by connect / disconnect /
// graph query so the supported set can never drift apart).
namespace
{
    // The three addressing keys the resolver below reads, plus the spellings that show up in
    // practice. Only a failure to resolve reaches the hint, so a caller who passes a stray key
    // alongside a working one is not nagged.
    struct FAddressKeyAlias
    {
        const TCHAR* Wrong;
        const TCHAR* Right;
    };
    const FAddressKeyAlias GAddressKeyAliases[] = {
        { TEXT("name"), TEXT("expression_name") },
        { TEXT("desc"), TEXT("expression_desc") },
        { TEXT("type"), TEXT("expression_type") },
        { TEXT("expression"), TEXT("expression_name") },
        { TEXT("node_name"), TEXT("expression_name") },
        { TEXT("node_desc"), TEXT("expression_desc") },
        { TEXT("node_type"), TEXT("expression_type") },
        // create_material_expression names its class that way; every resolve-based command does not.
        { TEXT("expression_class"), TEXT("expression_type") },
    };

    // A misspelled addressing key used to come back as a bare expression_not_found, which reads as
    // "that node is gone" instead of "that key is not one I look at".
    void AddAddressKeyHints(const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonObject>& Error)
    {
        if (!Params.IsValid() || !Error.IsValid())
        {
            return;
        }
        TArray<TSharedPtr<FJsonValue>> UnknownKeys;
        TSharedPtr<FJsonObject> DidYouMean = MakeShared<FJsonObject>();
        for (const FAddressKeyAlias& Alias : GAddressKeyAliases)
        {
            if (!Params->HasField(Alias.Wrong))
            {
                continue;
            }
            UnknownKeys.Add(MakeShared<FJsonValueString>(Alias.Wrong));
            DidYouMean->SetStringField(Alias.Wrong, Alias.Right);
        }
        if (UnknownKeys.Num() == 0)
        {
            return;
        }
        Error->SetArrayField(TEXT("unknown_keys"), UnknownKeys);
        Error->SetObjectField(TEXT("did_you_mean"), DidYouMean);
    }

    bool ResolveExpression(const FMaterialExpressionCollection& Collection, const TSharedPtr<FJsonObject>& Params, UMaterialExpression*& OutExpression, TSharedPtr<FJsonObject>& OutError)
    {
        FString Name, Desc, Type;
        const bool bName = Params->TryGetStringField(TEXT("expression_name"), Name);
        const bool bDesc = Params->TryGetStringField(TEXT("expression_desc"), Desc);
        const bool bType = Params->TryGetStringField(TEXT("expression_type"), Type);
        TArray<int32> Matches;
        for (int32 Index = 0; Index < Collection.Expressions.Num(); ++Index)
        {
            const UMaterialExpression* Expr = Collection.Expressions[Index];
            if (!Expr) continue;
            const bool bMatchesName = bName && Expr->GetName() == Name;
            const bool bMatchesDesc = bDesc && Expr->Desc == Desc;
            const bool bMatchesType = bType && Expr->GetClass()->GetName() == Type;
            const bool bMatches = (bName || bDesc || bType)
                ? ((!bName || bMatchesName) && (!bDesc || bMatchesDesc) && (!bType || bMatchesType))
                : false;
            if (bMatches)
            {
                Matches.Add(Index);
            }
        }
        auto Candidates = [&Collection]()
        {
            TArray<TSharedPtr<FJsonValue>> Values;
            for (int32 Index = 0; Index < Collection.Expressions.Num(); ++Index)
            {
                const UMaterialExpression* Expr = Collection.Expressions[Index];
                TSharedPtr<FJsonObject> Candidate = MakeShared<FJsonObject>();
                Candidate->SetNumberField(TEXT("index"), Index);
                Candidate->SetStringField(TEXT("type"), Expr ? Expr->GetClass()->GetName() : TEXT(""));
                Candidate->SetStringField(TEXT("name"), Expr ? Expr->GetName() : TEXT(""));
                Candidate->SetStringField(TEXT("desc"), Expr ? Expr->Desc : TEXT(""));
                Values.Add(MakeShared<FJsonValueObject>(Candidate));
            }
            return Values;
        };
        if (Matches.Num() != 1)
        {
            OutError = MakeShared<FJsonObject>();
            OutError->SetBoolField(TEXT("success"), false);
            OutError->SetStringField(TEXT("error"), Matches.Num() == 0 ? TEXT("expression_not_found") : TEXT("ambiguous_expression"));
            OutError->SetNumberField(TEXT("match_count"), Matches.Num());
            OutError->SetArrayField(TEXT("candidates"), Candidates());
            TArray<TSharedPtr<FJsonValue>> MatchedCandidates;
            for (int32 Match : Matches)
            {
                const UMaterialExpression* Expr = Collection.Expressions[Match];
                TSharedPtr<FJsonObject> Candidate = MakeShared<FJsonObject>();
                Candidate->SetNumberField(TEXT("index"), Match);
                Candidate->SetStringField(TEXT("type"), Expr->GetClass()->GetName());
                Candidate->SetStringField(TEXT("name"), Expr->GetName());
                Candidate->SetStringField(TEXT("desc"), Expr->Desc);
                MatchedCandidates.Add(MakeShared<FJsonValueObject>(Candidate));
            }
            OutError->SetArrayField(TEXT("matched_candidates"), MatchedCandidates);
            AddAddressKeyHints(Params, OutError);
            return false;
        }
        OutExpression = Collection.Expressions[Matches[0]];
        return OutExpression != nullptr;
    }
    const TMap<FString, EMaterialProperty>& GetMaterialPropertyMap()
    {
        static const TMap<FString, EMaterialProperty> Map = {
            {TEXT("BaseColor"), MP_BaseColor},
            {TEXT("EmissiveColor"), MP_EmissiveColor},
            {TEXT("Metallic"), MP_Metallic},
            {TEXT("Specular"), MP_Specular},
            {TEXT("Roughness"), MP_Roughness},
            {TEXT("Anisotropy"), MP_Anisotropy},
            {TEXT("Normal"), MP_Normal},
            {TEXT("Tangent"), MP_Tangent},
            {TEXT("Opacity"), MP_Opacity},
            {TEXT("OpacityMask"), MP_OpacityMask},
            {TEXT("SubsurfaceColor"), MP_SubsurfaceColor},
            {TEXT("AmbientOcclusion"), MP_AmbientOcclusion},
            {TEXT("Refraction"), MP_Refraction},
            {TEXT("WorldPositionOffset"), MP_WorldPositionOffset},
            {TEXT("PixelDepthOffset"), MP_PixelDepthOffset},
            {TEXT("SurfaceThickness"), MP_SurfaceThickness},
            {TEXT("Displacement"), MP_Displacement}
        };
        return Map;
    }

    // Output Type of a Custom node as the lint rule set spells it (the trailing
    // component of the enum name, e.g. CMOT_Float3 -> Float3).
    FString CustomOutputTypeName(ECustomMaterialOutputType OutputType)
    {
        switch (OutputType)
        {
        case CMOT_Float1: return TEXT("Float1");
        case CMOT_Float2: return TEXT("Float2");
        case CMOT_Float3: return TEXT("Float3");
        case CMOT_Float4: return TEXT("Float4");
        case CMOT_MaterialAttributes: return TEXT("MaterialAttributes");
        default: return TEXT("");
        }
    }

    // Structured input list shared by list_material_expressions and
    // get_material_expression_property(property="inputs"): the pin name plus the
    // upstream connection the python API cannot read (FCustomInput.Input and
    // FExpressionInput members are protected there).
    TArray<TSharedPtr<FJsonValue>> BuildInputList(UMaterialExpression* Expr)
    {
        TArray<TSharedPtr<FJsonValue>> Inputs;
        const int32 InputCount = Expr->CountInputs();
        for (int32 Pin = 0; Pin < InputCount; ++Pin)
        {
            const FExpressionInput* In = Expr->GetInput(Pin);
            TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("input_name"), Expr->GetInputName(Pin).ToString());
            Item->SetStringField(TEXT("expression"), (In && In->Expression) ? In->Expression->GetName() : TEXT(""));
            Item->SetNumberField(TEXT("output_index"), In ? In->OutputIndex : 0);
            Item->SetBoolField(TEXT("connected"), In && In->Expression != nullptr);
            Inputs.Add(MakeShared<FJsonValueObject>(Item));
        }
        return Inputs;
    }

    // Legal output names of an expression, resolved through the same GetOutputs()
    // virtual the connect path uses, so callers never have to guess the spelling
    // (TextureSample exposes RGB/R/G/B/A, single-output nodes expose "").
    TArray<TSharedPtr<FJsonValue>> BuildOutputList(UMaterialExpression* Expr)
    {
        TArray<TSharedPtr<FJsonValue>> Outputs;
        if (!Expr)
        {
            return Outputs;
        }
        if (UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expr))
        {
            if (Custom->Outputs.Num() == 0)
            {
                Custom->PostEditChange();
            }
        }
        const TArray<FExpressionOutput>& ExprOutputs = Expr->GetOutputs();
        for (int32 Index = 0; Index < ExprOutputs.Num(); ++Index)
        {
            TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetNumberField(TEXT("output_index"), Index);
            Item->SetStringField(TEXT("name"),
                ExprOutputs[Index].OutputName.IsNone() ? TEXT("") : ExprOutputs[Index].OutputName.ToString());
            Outputs.Add(MakeShared<FJsonValueObject>(Item));
        }
        return Outputs;
    }

    // The material's own mutation clock: every command that changes the graph
    // stamps the asset here. get_material_compile_errors uses it to separate the
    // current compile's log lines from the intermediate-state failures of earlier
    // edits, which the log tail alone cannot tell apart.
    //
    // UTC, not local: the editor writes log stamps in UTC ("[2026.09.11-06.01.31]"
    // while the wall clock read 14:01), and FDateTime is timezone-naive, so mixing
    // FDateTime::Now() in here would shift the whole boundary by the zone offset
    // and file the current compile's failures under history.
    TMap<FString, FDateTime>& GetMaterialMutationTimes()
    {
        static TMap<FString, FDateTime> Times;
        return Times;
    }

    void RecordMaterialMutation(const FString& AssetPath, const FDateTime& MutationTime)
    {
        GetMaterialMutationTimes().Add(AssetPath, MutationTime);
    }

    // "[2026.09.11-06.01.31:502]" -> the UTC FDateTime the log meant. FDateTime
    // holds ticks without a zone, and UE writes log stamps from UtcNow(), so the
    // parsed value is directly comparable to FDateTime::UtcNow().
    bool ParseLogLineTimestamp(const FString& Line, FDateTime& OutTime)
    {
        if (!Line.StartsWith(TEXT("[")))
        {
            return false;
        }
        const int32 CloseIndex = Line.Find(TEXT("]"));
        if (CloseIndex < 2)
        {
            return false;
        }
        const FString Stamp = Line.Mid(1, CloseIndex - 1);
        FString DatePart, TimePart;
        if (!Stamp.Split(TEXT("-"), &DatePart, &TimePart))
        {
            return false;
        }
        FString ClockPart = TimePart;
        FString MillisPart = TEXT("0");
        TimePart.Split(TEXT(":"), &ClockPart, &MillisPart);

        TArray<FString> DateParts;
        DatePart.ParseIntoArray(DateParts, TEXT("."), true);
        TArray<FString> ClockParts;
        ClockPart.ParseIntoArray(ClockParts, TEXT("."), true);
        if (DateParts.Num() < 3 || ClockParts.Num() < 3)
        {
            return false;
        }
        const int32 Year = FCString::Atoi(*DateParts[0]);
        const int32 Month = FCString::Atoi(*DateParts[1]);
        const int32 Day = FCString::Atoi(*DateParts[2]);
        const int32 Hour = FCString::Atoi(*ClockParts[0]);
        const int32 Minute = FCString::Atoi(*ClockParts[1]);
        const int32 Second = FCString::Atoi(*ClockParts[2]);
        // FDateTime validates its arguments, so screen the ranges first.
        if (Year < 1970 || Month < 1 || Month > 12 || Day < 1 || Day > 31
            || Hour > 23 || Minute > 59 || Second > 59)
        {
            return false;
        }
        OutTime = FDateTime(Year, Month, Day, Hour, Minute, Second, FCString::Atoi(*MillisPart));
        return true;
    }

    // `since` accepts ISO-8601 or an epoch-seconds number (numeric strings too).
    bool TryParseSince(const TSharedPtr<FJsonValue>& Value, FDateTime& OutTime)
    {
        if (!Value.IsValid())
        {
            return false;
        }
        if (Value->Type == EJson::Number)
        {
            OutTime = FDateTime::FromUnixTimestamp((int64)Value->AsNumber());
            return true;
        }
        if (Value->Type == EJson::String)
        {
            const FString Text = Value->AsString();
            if (Text.IsNumeric())
            {
                OutTime = FDateTime::FromUnixTimestamp(FCString::Atoi64(*Text));
                return true;
            }
            return FDateTime::ParseIso8601(*Text, OutTime);
        }
        return false;
    }

    // Expressions reachable from the material's property inputs, walked with the
    // same input iterator the engine uses when breaking links, so every input
    // array is covered (Custom pins, material attributes, ...).
    void CollectReachableExpressions(UMaterial* Mat, TSet<UMaterialExpression*>& OutReachable)
    {
        TArray<UMaterialExpression*> Pending;
        for (const TPair<FString, EMaterialProperty>& KV : GetMaterialPropertyMap())
        {
            if (FExpressionInput* RootInput = Mat->GetExpressionInputForProperty(KV.Value))
            {
                if (RootInput->Expression)
                {
                    Pending.Add(RootInput->Expression);
                }
            }
        }
        while (Pending.Num() > 0)
        {
            UMaterialExpression* Expr = Pending.Pop();
            if (!Expr || OutReachable.Contains(Expr))
            {
                continue;
            }
            OutReachable.Add(Expr);
            for (FExpressionInputIterator It{ Expr }; It; ++It)
            {
                if (It->Expression)
                {
                    Pending.Add(It->Expression);
                }
            }
        }
    }

    // Finds a UPROPERTY by name; accepts C++ names ("DefaultValue") as well as
    // the snake_case form used by the Python editor-property surface
    // ("default_value" -> "DefaultValue").
    const FProperty* FindExpressionProperty(UObject* Expr, const FString& InName)
    {
        if (const FProperty* P = Expr->GetClass()->FindPropertyByName(FName(*InName)))
        {
            return P;
        }
        FString Pascal;
        bool bCapitalize = true;
        for (const TCHAR Ch : InName)
        {
            if (Ch == TEXT('_'))
            {
                bCapitalize = true;
                continue;
            }
            Pascal.AppendChar(bCapitalize ? FChar::ToUpper(Ch) : Ch);
            bCapitalize = false;
        }
        if (Pascal != InName)
        {
            return Expr->GetClass()->FindPropertyByName(FName(*Pascal));
        }
        return nullptr;
    }
}

FUnrealMCPMaterialCommands::FUnrealMCPMaterialCommands()
{
}

void FUnrealMCPMaterialCommands::RegisterCommands(FMCPCommandRegistry& Registry)
{
    MCP_REGISTER_COMMAND(Registry, "convert_static_switch_to_dynamic", "material",
        "Replace every static switch parameter with a scalar parameter and rewire its links.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_name"), TEXT("string"), TEXT("Material asset path or name")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("convert_static_switch_to_dynamic"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleConvertStaticSwitchToDynamic(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "scan_material_custom_nodes", "material",
        "Find Custom expression nodes across the materials under a folder.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("folder"), TEXT("string"), TEXT("Content folder to scan recursively")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("scan_material_custom_nodes"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleScanMaterialCustomNodes(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "connect_material_pin", "material",
        "Connect an expression's output to a material property input.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path")),
            MCPParam(TEXT("property"), TEXT("string"), TEXT("Material property to connect (e.g. BaseColor)")),
            MCPParamOpt(TEXT("expression_name"), TEXT("string"), TEXT("Source expression name")),
            MCPParamOpt(TEXT("expression_desc"), TEXT("string"), TEXT("Source expression description")),
            MCPParamOpt(TEXT("expression_type"), TEXT("string"), TEXT("Source expression class name")),
            MCPParamOpt(TEXT("source_output_name"), TEXT("string"), TEXT("Source output pin name; defaults to first")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("connect_material_pin"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleConnectMaterialPin(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "connect_material_expression", "material",
        "Connect one expression's output to another expression's input.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path")),
            MCPParamOpt(TEXT("source_name"), TEXT("string"), TEXT("Source expression name")),
            MCPParamOpt(TEXT("source_desc"), TEXT("string"), TEXT("Source expression description")),
            MCPParamOpt(TEXT("source_type"), TEXT("string"), TEXT("Source expression class name")),
            MCPParamOpt(TEXT("target_name"), TEXT("string"), TEXT("Target expression name")),
            MCPParamOpt(TEXT("target_desc"), TEXT("string"), TEXT("Target expression description")),
            MCPParamOpt(TEXT("target_type"), TEXT("string"), TEXT("Target expression class name")),
            MCPParamOpt(TEXT("output_name"), TEXT("string"), TEXT("Source output pin name; defaults to first")),
            MCPParamOpt(TEXT("input_name"), TEXT("string"), TEXT("Target input pin name; defaults to first")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("connect_material_expression"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleConnectMaterialExpressions(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "get_material_compile_errors", "material",
        "Report a material's compile errors, splitting current ones from log history.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path")),
            MCPParamOpt(TEXT("since"), TEXT("string"), TEXT("ISO-8601 time or epoch seconds to treat as the compile boundary")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_material_compile_errors"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetMaterialCompileErrors(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "list_material_expressions", "material",
        "List a material's expressions with their live/orphan state.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("list_material_expressions"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleListMaterialExpressions(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "get_material_graph", "material",
        "Dump a material's expression graph: nodes, connections and material property inputs.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_material_graph"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetMaterialGraph(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "get_material_expression_property", "material",
        "Read one property of an expression, using the reflector's value shapes.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path")),
            MCPParam(TEXT("property"), TEXT("string"), TEXT("Property name to read")),
            MCPParamOpt(TEXT("expression_name"), TEXT("string"), TEXT("Expression name to resolve")),
            MCPParamOpt(TEXT("expression_desc"), TEXT("string"), TEXT("Expression description to resolve")),
            MCPParamOpt(TEXT("expression_type"), TEXT("string"), TEXT("Expression class name to resolve")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_material_expression_property"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetMaterialExpressionProperty(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_material_expression_property", "material",
        "Write one expression property, linting Custom HLSL before the write.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path")),
            MCPParam(TEXT("property"), TEXT("string"), TEXT("Property name to write")),
            MCPParam(TEXT("value"), TEXT("object"), TEXT("JSON value to assign")),
            MCPParamOpt(TEXT("expression_name"), TEXT("string"), TEXT("Expression name to resolve")),
            MCPParamOpt(TEXT("expression_desc"), TEXT("string"), TEXT("Expression description to resolve")),
            MCPParamOpt(TEXT("expression_type"), TEXT("string"), TEXT("Expression class name to resolve")),
            MCPParamOpt(TEXT("recompile"), TEXT("bool"), TEXT("Recompile the material after the write")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_material_expression_property"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetMaterialExpressionProperty(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "create_material_expression", "material",
        "Create an expression node in a material graph.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path")),
            MCPParam(TEXT("expression_class"), TEXT("string"), TEXT("Expression class name to create")),
            MCPParamOpt(TEXT("editor_x"), TEXT("int"), TEXT("Node X position in the graph")),
            MCPParamOpt(TEXT("editor_y"), TEXT("int"), TEXT("Node Y position in the graph")),
            MCPParamOpt(TEXT("desc"), TEXT("string"), TEXT("Node description")),
            MCPParamOpt(TEXT("code"), TEXT("string"), TEXT("Custom HLSL code for a Custom node")),
            MCPParamOpt(TEXT("inputs"), TEXT("array"), TEXT("Custom node input pins")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("create_material_expression"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleCreateMaterialExpression(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "delete_material_expression", "material",
        "Delete one expression node from a material graph.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path")),
            MCPParamOpt(TEXT("expression_name"), TEXT("string"), TEXT("Expression name to delete")),
            MCPParamOpt(TEXT("expression_desc"), TEXT("string"), TEXT("Expression description to delete")),
            MCPParamOpt(TEXT("expression_type"), TEXT("string"), TEXT("Expression class name to delete")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("delete_material_expression"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleDeleteMaterialExpression(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "delete_material_expressions", "material",
        "Delete several expressions from a material graph in one call.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path")),
            MCPParam(TEXT("expressions"), TEXT("array"), TEXT("Expression selectors to delete")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("delete_material_expressions"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleDeleteMaterialExpressions(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "wipe_material_graph", "material",
        "Delete every expression in a material graph.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path")),
            MCPParamOpt(TEXT("max_iterations"), TEXT("int"), TEXT("Iteration cap for the deletion loop")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("wipe_material_graph"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleWipeMaterialGraph(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "disconnect_material_property", "material",
        "Break the connection feeding a material property input.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path")),
            MCPParam(TEXT("property"), TEXT("string"), TEXT("Material property to disconnect")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("disconnect_material_property"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleDisconnectMaterialProperty(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "get_material_parameters", "material",
        "List the scalar and vector parameters of a material or material instance.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Material or material instance asset path")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_material_parameters"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetMaterialParameters(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_material_parameters", "material",
        "Write material instance parameter overrides from a name -> value object.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Material instance asset path")),
            MCPParam(TEXT("values"), TEXT("object"), TEXT("Object of parameter name -> value")),
            MCPParamOpt(TEXT("recompile"), TEXT("bool"), TEXT("Recompile after the write")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_material_parameters"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetMaterialParameters(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "validate_custom_hlsl", "material",
        "Lint a Custom node HLSL snippet without touching an asset.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("code"), TEXT("string"), TEXT("HLSL code to lint")),
            MCPParamOpt(TEXT("output_type"), TEXT("string"), TEXT("Output type name; defaults to float")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("validate_custom_hlsl"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleValidateCustomHlsl(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "validate_custom_expression", "material",
        "Cross-check a Custom node's code against its input pins.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path")),
            MCPParamOpt(TEXT("expression_name"), TEXT("string"), TEXT("Expression name to resolve")),
            MCPParamOpt(TEXT("expression_desc"), TEXT("string"), TEXT("Expression description to resolve")),
            MCPParamOpt(TEXT("expression_type"), TEXT("string"), TEXT("Expression class name to resolve")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("validate_custom_expression"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleValidateCustomExpression(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_custom_input_name", "material",
        "Rename a Custom node's input pin, keeping its existing connections.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path")),
            MCPParam(TEXT("old_name"), TEXT("string"), TEXT("Current input pin name")),
            MCPParam(TEXT("new_name"), TEXT("string"), TEXT("New input pin name")),
            MCPParamOpt(TEXT("expression_name"), TEXT("string"), TEXT("Expression name to resolve")),
            MCPParamOpt(TEXT("expression_desc"), TEXT("string"), TEXT("Expression description to resolve")),
            MCPParamOpt(TEXT("expression_type"), TEXT("string"), TEXT("Expression class name to resolve")),
            MCPParamOpt(TEXT("recompile"), TEXT("bool"), TEXT("Recompile the material after the rename")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_custom_input_name"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetCustomInputName(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "add_custom_input", "material",
        "Add one input pin to a Custom expression node, keeping every existing connection.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path")),
            MCPParam(TEXT("input_name"), TEXT("string"), TEXT("Name of the pin to add")),
            MCPParamOpt(TEXT("index"), TEXT("int"), TEXT("Insert position; defaults to appending (which is the HLSL argument order)")),
            MCPParamOpt(TEXT("expression_name"), TEXT("string"), TEXT("Expression name to resolve")),
            MCPParamOpt(TEXT("expression_desc"), TEXT("string"), TEXT("Expression description to resolve")),
            MCPParamOpt(TEXT("expression_type"), TEXT("string"), TEXT("Expression class name to resolve")),
            MCPParamOpt(TEXT("recompile"), TEXT("bool"), TEXT("Recompile the material after the insert")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_custom_input"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddCustomInput(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "remove_custom_input", "material",
        "Remove one input pin from a Custom expression node, keeping the other connections.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path")),
            MCPParam(TEXT("input_name"), TEXT("string"), TEXT("Name of the pin to remove")),
            MCPParamOpt(TEXT("expression_name"), TEXT("string"), TEXT("Expression name to resolve")),
            MCPParamOpt(TEXT("expression_desc"), TEXT("string"), TEXT("Expression description to resolve")),
            MCPParamOpt(TEXT("expression_type"), TEXT("string"), TEXT("Expression class name to resolve")),
            MCPParamOpt(TEXT("recompile"), TEXT("bool"), TEXT("Recompile the material after the removal")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("remove_custom_input"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRemoveCustomInput(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "recompile_material", "material",
        "Material-level refresh + one recompile (+ optional save): finish a graph edit with this.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path")),
            MCPParamOpt(TEXT("refresh"), TEXT("bool"), TEXT("Force the material-level PostEditChange that makes the live render pick expression-only edits up (default true)")),
            MCPParamOpt(TEXT("save"), TEXT("bool"), TEXT("Save the asset package after recompiling (default false)")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("recompile_material"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRecompileMaterial(P); }); }));

    // Deliberately not a mutating-graph command: it only READS the source material. The graph it
    // builds lives in a throw-away material, so the source must not be stamped as freshly mutated
    // (that stamp is what get_material_compile_errors uses to split current compiles from history).
    MCP_REGISTER_COMMAND(Registry, "build_material_preview", "material",
        "Build a throw-away single-node preview material for one expression of a material.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Source material asset path")),
            MCPParamOpt(TEXT("expression_name"), TEXT("string"), TEXT("Expression name to preview")),
            MCPParamOpt(TEXT("expression_desc"), TEXT("string"), TEXT("Expression description to preview")),
            MCPParamOpt(TEXT("expression_type"), TEXT("string"), TEXT("Expression class name to preview")),
            MCPParamOpt(TEXT("output_index"), TEXT("int"), TEXT("Output slot to preview (default 0)")),
            MCPParamOpt(TEXT("channel"), TEXT("string"), TEXT("rgba (default) or one of r/g/b/a")),
            MCPParamOpt(TEXT("temp_name"), TEXT("string"), TEXT("Name of the preview material asset")),
            MCPParamOpt(TEXT("temp_folder"), TEXT("string"), TEXT("Folder of the preview material asset")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("build_material_preview"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleBuildMaterialPreview(P); }); }));
}

// get_material_compile_errors stamps the most recent successful mutation so it can tell the current
// compile apart from history. Which commands mutate now comes from their registry flag.
TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::RunCommand(const FString& CommandType,
                                                               const TSharedPtr<FJsonObject>& Params,
                                                               const TFunction<TSharedPtr<FJsonObject>(const TSharedPtr<FJsonObject>&)>& Body)
{
    const FMCPCommandEntry* Entry = FMCPCommandRegistry::Get().Find(CommandType);
    const bool bMutatesGraph = Entry && Entry->Flags.bMutatesGraph;

    // Stamped BEFORE the change, not after: the compile a write triggers starts
    // inside the handler, so a boundary taken on return postdates the very log
    // lines it is meant to cover (observed: failure line at ...29.948 vs boundary
    // ...29.952, which filed the current compile under history).
    const FDateTime MutationStart = bMutatesGraph ? FDateTime::UtcNow() : FDateTime();

    TSharedPtr<FJsonObject> ResultJson = Body(Params);
    if (ResultJson.IsValid() && bMutatesGraph)
    {
        bool bSuccess = false;
        // A batch that failed some of its items still changed the graph, so its compile has to be
        // dated too; handlers that report per-item outcomes set `partial` next to the failing
        // top-level `success`.
        bool bPartial = false;
        FString AssetPath;
        if ((ResultJson->TryGetBoolField(TEXT("success"), bSuccess) && bSuccess)
            || (ResultJson->TryGetBoolField(TEXT("partial"), bPartial) && bPartial))
        {
            if (Params.IsValid() && Params->TryGetStringField(TEXT("asset_path"), AssetPath))
            {
                RecordMaterialMutation(AssetPath, MutationStart);
            }
        }
    }
    return ResultJson;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleConvertStaticSwitchToDynamic(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> ResultJson;
    FString AssetName;
    if (!Params->TryGetStringField(TEXT("asset_name"), AssetName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_name' parameter"));
    }

    UObject* Asset = FUnrealMCPCommonUtils::FindAsset(AssetName);
    UMaterial* Material = Cast<UMaterial>(Asset);
    if (!Material)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Material not found: %s"), *AssetName));
    }

    Material->Modify();
    int32 Converted = 0;
    TArray<FName> ConvertedNames;

    // Collect static switch parameters first; never mutate
    // FMaterialExpressionCollection::Expressions while iterating it.
    FMaterialExpressionCollection& ExprCollection = Material->GetExpressionCollection();
    TArray<UMaterialExpressionStaticSwitchParameter*> Switches;
    for (UMaterialExpression* Expr : ExprCollection.Expressions)
    {
        if (UMaterialExpressionStaticSwitchParameter* Sw = Cast<UMaterialExpressionStaticSwitchParameter>(Expr))
        {
            Switches.Add(Sw);
        }
    }

    for (UMaterialExpressionStaticSwitchParameter* Sw : Switches)
    {
        if (!Sw)
        {
            continue;
        }

        const FName ParamName = Sw->ParameterName;
        const float NewDefault = Sw->DefaultValue ? 1.0f : 0.0f;

        // Create the replacement through the engine API so it is registered with
        // the expression collection and Material->EditorParameters (ownership/GC).
        UMaterialExpressionScalarParameter* Scalar = Cast<UMaterialExpressionScalarParameter>(
            UMaterialEditingLibrary::CreateMaterialExpressionEx(
                Material,
                nullptr,
                UMaterialExpressionScalarParameter::StaticClass(),
                nullptr,
                Sw->MaterialExpressionEditorX,
                Sw->MaterialExpressionEditorY));
        if (!Scalar)
        {
            continue;
        }
        Scalar->ParameterName = ParamName;
        Scalar->DefaultValue = NewDefault;

        // Redirect expression-to-expression connections to the new scalar,
        // iterating over a snapshot of the collection.
        TArray<UMaterialExpression*> ExpressionSnapshot = ExprCollection.Expressions;
        for (UMaterialExpression* Other : ExpressionSnapshot)
        {
            if (!Other || Other == Sw)
            {
                continue;
            }
            const int32 NumInputs = Other->CountInputs();
            for (int32 i = 0; i < NumInputs; ++i)
            {
                FExpressionInput* In = Other->GetInput(i);
                if (In && In->Expression == Sw)
                {
                    In->Expression = Scalar;
                    In->OutputIndex = 0;
                }
            }
        }

        // Redirect material property inputs that point to this switch.
        // NOTE: use UMaterial::GetExpressionInputForProperty directly instead of
        // UMaterialEditingLibrary::GetMaterialPropertyInputNode - the latter
        // dereferences the input without a null check and crashes for
        // properties that have no input slot.
        for (int32 PropIdx = 0; PropIdx < MP_MAX; ++PropIdx)
        {
            const EMaterialProperty Prop = (EMaterialProperty)PropIdx;
            FExpressionInput* PropInput = Material->GetExpressionInputForProperty(Prop);
            if (PropInput && PropInput->Expression == Sw)
            {
                PropInput->Expression = Scalar;
                PropInput->OutputIndex = 0;
            }
        }

        // Delete the switch through the engine API: breaks remaining links,
        // unregisters the parameter (EditorParameters), removes the expression
        // and marks it garbage safely.
        UMaterialEditingLibrary::DeleteMaterialExpression(Material, Sw);

        ConvertedNames.Add(ParamName);
        ++Converted;
    }

    if (Converted > 0)
    {
        UMaterialEditingLibrary::RecompileMaterial(Material);
        Material->MarkPackageDirty();
    }

    ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetNumberField(TEXT("converted"), Converted);
    if (ConvertedNames.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> NamesArray;
        for (const FName& N : ConvertedNames)
        {
            NamesArray.Add(MakeShareable(new FJsonValueString(N.ToString())));
        }
        ResultJson->SetArrayField(TEXT("converted_names"), NamesArray);
    }
    return ResultJson;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleScanMaterialCustomNodes(const TSharedPtr<FJsonObject>& Params)
{
    FString Folder;
    if (!Params->TryGetStringField(TEXT("folder"), Folder))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'folder' parameter"));
    }

    IAssetRegistry& AssetRegistry = IAssetRegistry::GetChecked();
    TArray<FAssetData> AssetDataList;
    AssetRegistry.GetAssetsByPath(FName(*Folder), AssetDataList, /*bRecursive=*/true, /*bIncludeOnlyOnDiskAssets=*/false);

    TArray<TSharedPtr<FJsonValue>> MaterialsArray;
    for (const FAssetData& AssetData : AssetDataList)
    {
        if (AssetData.GetClass() != UMaterial::StaticClass())
        {
            continue;
        }
        UMaterial* Material = Cast<UMaterial>(AssetData.GetAsset());
        if (!Material)
        {
            continue;
        }

        TArray<TSharedPtr<FJsonValue>> CustomNodes;
        const FMaterialExpressionCollection& ExprCollection = Material->GetExpressionCollection();
        for (UMaterialExpression* Expr : ExprCollection.Expressions)
        {
            if (UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expr))
            {
                TSharedPtr<FJsonObject> NodeObj = MakeShareable(new FJsonObject);
                NodeObj->SetStringField(TEXT("name"), Custom->GetName());
                FString FirstLine = Custom->Code;
                int32 NewlineIdx = INDEX_NONE;
                if (FirstLine.FindChar(TEXT('\n'), NewlineIdx))
                {
                    FirstLine = FirstLine.Left(NewlineIdx);
                }
                NodeObj->SetStringField(TEXT("code_first_line"), FirstLine);
                CustomNodes.Add(MakeShareable(new FJsonValueObject(NodeObj)));
            }
        }

        if (CustomNodes.Num() > 0)
        {
            TSharedPtr<FJsonObject> MatObj = MakeShareable(new FJsonObject);
            MatObj->SetStringField(TEXT("material"), AssetData.GetObjectPathString());
            MatObj->SetArrayField(TEXT("custom_nodes"), CustomNodes);
            MaterialsArray.Add(MakeShareable(new FJsonValueObject(MatObj)));
        }
    }

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetArrayField(TEXT("materials"), MaterialsArray);
    ResultJson->SetNumberField(TEXT("material_count_with_custom"), MaterialsArray.Num());
    return ResultJson;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleConnectMaterialPin(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    FString PropertyName;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) ||
        !Params->TryGetStringField(TEXT("property"), PropertyName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' or 'property' parameter"));
    }

    UMaterial* Mat = LoadObject<UMaterial>(nullptr, *AssetPath);
    if (!Mat)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Material not found: %s"), *AssetPath));
    }

    const FMaterialExpressionCollection& ExprCollection = Mat->GetExpressionCollection();
    UMaterialExpression* FromExpression = nullptr;
    TSharedPtr<FJsonObject> ResolveError;
    if (!ResolveExpression(ExprCollection, Params, FromExpression, ResolveError))
    {
        return ResolveError;
    }

    if (!FromExpression)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Source expression is null"));
    }

    const TMap<FString, EMaterialProperty>& PropertyMap = GetMaterialPropertyMap();
    const EMaterialProperty* Prop = PropertyMap.Find(PropertyName);
    if (!Prop)
    {
        TSharedPtr<FJsonObject> ErrJson = MakeShareable(new FJsonObject);
        ErrJson->SetBoolField(TEXT("success"), false);
        ErrJson->SetStringField(TEXT("error"), FString::Printf(
            TEXT("Unknown material property: %s"), *PropertyName));
        TArray<TSharedPtr<FJsonValue>> Supported;
        for (const TPair<FString, EMaterialProperty>& KV : PropertyMap)
        {
            Supported.Add(MakeShareable(new FJsonValueString(KV.Key)));
        }
        ErrJson->SetArrayField(TEXT("supported_properties"), Supported);
        if (FromExpression)
        {
            TArray<TSharedPtr<FJsonValue>> Pins;
            const TArray<FExpressionOutput>& Outputs = FromExpression->GetOutputs();
            for (const FExpressionOutput& Out : Outputs)
            {
                Pins.Add(MakeShareable(new FJsonValueString(Out.OutputName.ToString())));
            }
            ErrJson->SetArrayField(TEXT("available_pins"), Pins);
        }
        return ErrJson;
    }

    // ---- Source output: resolve via GetOutputs() virtual ----
    // Same engine limitation as connect_material_expression: the engine
    // UMaterialEditingLibrary::ConnectMaterialProperty resolves the source
    // output through GetExpressionOutputIndexByName which reads the member
    // `Outputs` array (empty for dynamic-output node classes), so python-
    // created Custom / SceneTexture / Add nodes always fail. Resolve through
    // the GetOutputs() virtual instead and write the property input directly.
    if (UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(FromExpression))
    {
        if (Custom->Outputs.Num() == 0)
        {
            Custom->PostEditChange();
        }
    }
    const TArray<FExpressionOutput>& Outs = FromExpression->GetOutputs();

    FString SourceOutputNameStr;
    Params->TryGetStringField(TEXT("source_output_name"), SourceOutputNameStr);
    const FName SourceOutName(*SourceOutputNameStr);

    int32 OutIndex = INDEX_NONE;
    if (Outs.Num() > 0)
    {
        if (SourceOutName.IsNone())
        {
            OutIndex = 0;
        }
        else
        {
            for (int32 i = 0; i < Outs.Num(); ++i)
            {
                const FExpressionOutput& Out = Outs[i];
                bool bMatch = false;
                if (!Out.OutputName.IsNone())
                {
                    bMatch = (Out.OutputName == SourceOutName);
                }
                else
                {
                    // Unnamed masked outputs match R/G/B/A like the engine resolver
                    bMatch = (Out.MaskR && !Out.MaskG && !Out.MaskB && !Out.MaskA && SourceOutName == TEXT("R")) ||
                             (!Out.MaskR && Out.MaskG && !Out.MaskB && !Out.MaskA && SourceOutName == TEXT("G")) ||
                             (!Out.MaskR && !Out.MaskG && Out.MaskB && !Out.MaskA && SourceOutName == TEXT("B")) ||
                             (!Out.MaskR && !Out.MaskG && !Out.MaskB && Out.MaskA && SourceOutName == TEXT("A"));
                }
                if (bMatch)
                {
                    OutIndex = i;
                    break;
                }
            }
        }
    }
    if (OutIndex == INDEX_NONE)
    {
        TSharedPtr<FJsonObject> ErrJson = MakeShareable(new FJsonObject);
        ErrJson->SetBoolField(TEXT("success"), false);
        ErrJson->SetStringField(TEXT("error"), TEXT("output_not_found"));
        ErrJson->SetStringField(TEXT("source"), FromExpression->GetName());
        if (!SourceOutName.IsNone())
        {
            ErrJson->SetStringField(TEXT("requested_output"), SourceOutName.ToString());
        }
        TArray<TSharedPtr<FJsonValue>> Pins;
        for (const FExpressionOutput& Out : Outs)
        {
            Pins.Add(MakeShareable(new FJsonValueString(Out.OutputName.IsNone() ? TEXT("") : Out.OutputName.ToString())));
        }
        ErrJson->SetArrayField(TEXT("available_pins"), Pins);
        return ErrJson;
    }

    FExpressionInput* PropertyInput = Mat->GetExpressionInputForProperty(*Prop);
    if (!PropertyInput)
    {
        TSharedPtr<FJsonObject> ErrJson = MakeShareable(new FJsonObject);
        ErrJson->SetBoolField(TEXT("success"), false);
        ErrJson->SetStringField(TEXT("error"), FString::Printf(
            TEXT("Property '%s' has no connectable input (hidden or unsupported property)"), *PropertyName));
        return ErrJson;
    }

    PropertyInput->Connect(OutIndex, FromExpression);
    // Deliberately NOT recompiling here. A recompile rebuilds every shader permutation of the
    // material, and wiring commands are issued in loops (a 10-node rebuild = 10 connects), which
    // measured ~250ms per connect => ~2.4s per material. Finishing the graph is the caller's job:
    // recompile_material does the material-level refresh + one recompile.
    Mat->MarkPackageDirty();

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetBoolField(TEXT("connected"), true);
    ResultJson->SetStringField(TEXT("property"), PropertyName);
    ResultJson->SetStringField(TEXT("source_name"), FromExpression->GetName());
    ResultJson->SetStringField(TEXT("source_desc"), FromExpression->Desc);
    return ResultJson;
}

// Expression-to-expression connect that bypasses the engine python limitation:
// UMaterialEditingLibrary::ConnectMaterialExpressions resolves the SOURCE output
// through the member `Outputs` array only, which stays empty for dynamic-output
// node classes (Add/Subtract, SceneTexture, python-created Custom...). Here we
// use the GetOutputs() virtual (same as the compiler/graph UI) so every node
// class works as a source.
TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleConnectMaterialExpressions(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' parameter"));
    }

    UMaterial* Mat = LoadObject<UMaterial>(nullptr, *AssetPath);
    if (!Mat)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Material not found: %s"), *AssetPath));
    }

    const FMaterialExpressionCollection& ExprCollection = Mat->GetExpressionCollection();

    // Resolve source/target via the shared resolver: wrap the per-side fields
    // (source_name / source_desc / source_type) into expression_* keys.
    auto MakeSideParams = [&Params](const FString& Prefix)
    {
        TSharedPtr<FJsonObject> Side = MakeShareable(new FJsonObject);
        FString Value;
        if (Params->TryGetStringField(Prefix + TEXT("_name"), Value))
        {
            Side->SetStringField(TEXT("expression_name"), Value);
        }
        if (Params->TryGetStringField(Prefix + TEXT("_desc"), Value))
        {
            Side->SetStringField(TEXT("expression_desc"), Value);
        }
        if (Params->TryGetStringField(Prefix + TEXT("_type"), Value))
        {
            Side->SetStringField(TEXT("expression_type"), Value);
        }
        return Side;
    };

    UMaterialExpression* Src = nullptr;
    TSharedPtr<FJsonObject> ResolveError;
    if (!ResolveExpression(ExprCollection, MakeSideParams(TEXT("source")), Src, ResolveError))
    {
        return ResolveError;
    }
    UMaterialExpression* Dst = nullptr;
    if (!ResolveExpression(ExprCollection, MakeSideParams(TEXT("target")), Dst, ResolveError))
    {
        return ResolveError;
    }

    // ---- Source output: resolve via GetOutputs() virtual ----
    // Python-created Custom nodes never ran PostEditChangeProperty, so their
    // member Outputs array is still empty; refresh it on demand. RebuildOutputs()
    // itself is not exported from the engine DLL (MinimalAPI), but the
    // PostEditChangeProperty override dispatches to it virtually.
    if (UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Src))
    {
        if (Custom->Outputs.Num() == 0)
        {
            Custom->PostEditChange();
        }
    }
    const TArray<FExpressionOutput>& Outs = Src->GetOutputs();

    FString OutputNameStr;
    Params->TryGetStringField(TEXT("output_name"), OutputNameStr);
    const FName OutName(*OutputNameStr);

    int32 OutIndex = INDEX_NONE;
    if (Outs.Num() > 0)
    {
        if (OutName.IsNone())
        {
            OutIndex = 0;
        }
        else
        {
            for (int32 i = 0; i < Outs.Num(); ++i)
            {
                const FExpressionOutput& Out = Outs[i];
                bool bMatch = false;
                if (!Out.OutputName.IsNone())
                {
                    bMatch = (Out.OutputName == OutName);
                }
                else
                {
                    // Unnamed masked outputs match R/G/B/A like the engine resolver
                    bMatch = (Out.MaskR && !Out.MaskG && !Out.MaskB && !Out.MaskA && OutName == TEXT("R")) ||
                             (!Out.MaskR && Out.MaskG && !Out.MaskB && !Out.MaskA && OutName == TEXT("G")) ||
                             (!Out.MaskR && !Out.MaskG && Out.MaskB && !Out.MaskA && OutName == TEXT("B")) ||
                             (!Out.MaskR && !Out.MaskG && !Out.MaskB && Out.MaskA && OutName == TEXT("A"));
                }
                if (bMatch)
                {
                    OutIndex = i;
                    break;
                }
            }
        }
    }
    if (OutIndex == INDEX_NONE)
    {
        TSharedPtr<FJsonObject> ErrJson = MakeShareable(new FJsonObject);
        ErrJson->SetBoolField(TEXT("success"), false);
        ErrJson->SetStringField(TEXT("error"), TEXT("output_not_found"));
        ErrJson->SetStringField(TEXT("source"), Src->GetName());
        if (!OutName.IsNone())
        {
            ErrJson->SetStringField(TEXT("requested_output"), OutName.ToString());
        }
        TArray<TSharedPtr<FJsonValue>> Available;
        for (const FExpressionOutput& Out : Outs)
        {
            Available.Add(MakeShareable(new FJsonValueString(Out.OutputName.IsNone() ? TEXT("") : Out.OutputName.ToString())));
        }
        ErrJson->SetArrayField(TEXT("available_outputs"), Available);
        if (Outs.Num() == 0)
        {
            ErrJson->SetStringField(TEXT("detail"), TEXT("source expression exposes no outputs (GetOutputs empty)"));
        }
        return ErrJson;
    }

    // ---- Target input: resolve by name via GetInput/GetInputName ----
    FString InputNameStr;
    Params->TryGetStringField(TEXT("input_name"), InputNameStr);
    const FName InName(*InputNameStr);

    FExpressionInput* In = nullptr;
    int32 InIndex = INDEX_NONE;
    const int32 NumInputs = Dst->CountInputs();
    if (InName.IsNone())
    {
        if (NumInputs > 0)
        {
            InIndex = 0;
            In = Dst->GetInput(0);
        }
    }
    else
    {
        for (int32 i = 0; i < NumInputs; ++i)
        {
            if (Dst->GetInputName(i) == InName)
            {
                InIndex = i;
                In = Dst->GetInput(i);
                break;
            }
        }
    }
    if (!In)
    {
        TSharedPtr<FJsonObject> ErrJson = MakeShareable(new FJsonObject);
        ErrJson->SetBoolField(TEXT("success"), false);
        ErrJson->SetStringField(TEXT("error"), TEXT("input_not_found"));
        ErrJson->SetStringField(TEXT("target"), Dst->GetName());
        if (!InName.IsNone())
        {
            ErrJson->SetStringField(TEXT("requested_input"), InName.ToString());
        }
        TArray<TSharedPtr<FJsonValue>> Available;
        for (int32 i = 0; i < NumInputs; ++i)
        {
            Available.Add(MakeShareable(new FJsonValueString(Dst->GetInputName(i).ToString())));
        }
        ErrJson->SetArrayField(TEXT("available_inputs"), Available);
        if (NumInputs == 0)
        {
            ErrJson->SetStringField(TEXT("detail"), TEXT("target expression exposes no inputs"));
        }
        return ErrJson;
    }

    Mat->Modify();
    In->Connect(OutIndex, Src);
    // Let the target react to the new link (e.g. Custom input bookkeeping). Cheap and needed.
    Dst->PostEditChange();
    // Deliberately NOT recompiling here (it used to, ~250ms per connect, i.e. ~2.4s per 10-node
    // material because the wiring commands run in loops). The material-level refresh + the single
    // recompile belong to recompile_material, called once the graph is finished.
    Mat->MarkPackageDirty();

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("source_name"), Src->GetName());
    ResultJson->SetStringField(TEXT("source_desc"), Src->Desc);
    ResultJson->SetStringField(TEXT("target_name"), Dst->GetName());
    ResultJson->SetStringField(TEXT("target_desc"), Dst->Desc);
    ResultJson->SetStringField(TEXT("output_name"), Outs[OutIndex].OutputName.ToString());
    ResultJson->SetNumberField(TEXT("output_index"), OutIndex);
    ResultJson->SetStringField(TEXT("input_name"), Dst->GetInputName(InIndex).ToString());
    ResultJson->SetNumberField(TEXT("input_index"), InIndex);
    return ResultJson;
}

namespace
{
    // The engine's wording for the failure that stops a Custom node's compile at the FIRST
    // unconnected pin (MaterialExpressions.cpp:15536-15538). Read on its own, "missing input 2
    // (In_B)" looks like "the pin was never created", so it is turned into a named finding.
    struct FCompileErrorFinding
    {
        FString NodeDescription;
        FString PinName;
        int32 PinIndex1Based = 0;
    };

    bool ParseCustomMissingInput(const FString& Error, FCompileErrorFinding& Out)
    {
        static const FRegexPattern Pattern(TEXT("Custom material (.*?) missing input ([0-9]+) \\(([^)]*)\\)"));
        FRegexMatcher Matcher(Pattern, Error);
        if (!Matcher.FindNext())
        {
            return false;
        }
        Out.NodeDescription = Matcher.GetCaptureGroup(1).TrimStartAndEnd();
        Out.PinIndex1Based = FCString::Atoi(*Matcher.GetCaptureGroup(2));
        Out.PinName = Matcher.GetCaptureGroup(3).TrimStartAndEnd();
        return true;
    }
}

//==============================================================================
// The one command that makes a material current again.
//
// Every wiring command (connect_material_expression / connect_material_pin /
// disconnect_material_property) deliberately does NOT recompile any more: a recompile rebuilds every
// shader permutation of the material, and wiring runs in loops (10-node rebuild = 10 connects), which
// measured ~250ms per connect, i.e. ~2.4s per material, on a graph that was not even finished yet.
// The caller finishes with this command once, after the graph is in its final shape.
//==============================================================================

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleRecompileMaterial(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' parameter"));
    }
    bool bRefresh = true;
    bool bSave = false;
    Params->TryGetBoolField(TEXT("refresh"), bRefresh);
    Params->TryGetBoolField(TEXT("save"), bSave);

    UMaterial* Mat = LoadObject<UMaterial>(nullptr, *AssetPath);
    if (!Mat)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Material not found: %s"), *AssetPath));
    }

    // refresh: a MATERIAL-level property change is what makes the live render pick expression-only
    // edits up (a Custom node's `code`, a parameter's default). Without it the asset reads back
    // correct while the viewport keeps drawing the previous shader/uniforms - the reason
    // Saved/MCPScripts/toon_ganyu_materials.py has a poke_material() from the python side.
    if (bRefresh)
    {
        const uint8 bWasTwoSided = Mat->TwoSided;
        Mat->Modify();
        Mat->TwoSided = bWasTwoSided ? 0 : 1;
        Mat->PostEditChange();
        Mat->TwoSided = bWasTwoSided;
        Mat->PostEditChange();
    }

    UMaterialEditingLibrary::RecompileMaterial(Mat);
    Mat->MarkPackageDirty();

    const bool bSaved = bSave ? FUnrealMCPCommonUtils::SaveAssetForObject(Mat) : false;

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
    ResultJson->SetBoolField(TEXT("refreshed"), bRefresh);
    ResultJson->SetBoolField(TEXT("recompiled"), true);
    ResultJson->SetBoolField(TEXT("saved"), bSaved);
    ResultJson->SetStringField(TEXT("hint"),
        TEXT("Shader compilation is asynchronous: read get_material_compile_errors once more after a moment."));
    return ResultJson;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleGetMaterialCompileErrors(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' parameter"));
    }

    UMaterial* Mat = LoadObject<UMaterial>(nullptr, *AssetPath);
    if (!Mat)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Material not found: %s"), *AssetPath));
    }

    // Primary path: the engine stores per-feature-level compile errors on
    // the material resource. Shader compilation is asynchronous, so these
    // may be empty until the compile finishes (hence the hint below).
    TSharedPtr<FJsonObject> ErrorsByFeatureLevel = MakeShareable(new FJsonObject);
    int32 TotalErrorCount = 0;

    // One entry per distinct raw message, so the same failure reported for both feature levels
    // stays a single finding that lists the levels it appeared under.
    struct FParsedFinding
    {
        FString Raw;
        FCompileErrorFinding Finding;
        TArray<FString> FeatureLevels;
    };
    TArray<FParsedFinding> Findings;

    static const TPair<const TCHAR*, ERHIFeatureLevel::Type> FeatureLevels[] = {
        { TEXT("SM5"), ERHIFeatureLevel::SM5 },
        { TEXT("SM6"), ERHIFeatureLevel::SM6 },
    };

    for (const TPair<const TCHAR*, ERHIFeatureLevel::Type>& FeatureLevel : FeatureLevels)
    {
        TArray<TSharedPtr<FJsonValue>> LevelErrors;
        if (const FMaterialResource* Resource = UNREALMCP_MATERIAL_RESOURCE_FOR_FEATURE_LEVEL(Mat, FeatureLevel.Value))
        {
            for (const FString& Error : Resource->GetCompileErrors())
            {
                LevelErrors.Add(MakeShareable(new FJsonValueString(Error)));
                FCompileErrorFinding Finding;
                if (!ParseCustomMissingInput(Error, Finding))
                {
                    continue;
                }
                FParsedFinding* Existing = Findings.FindByPredicate(
                    [&Error](const FParsedFinding& Other) { return Other.Raw == Error; });
                if (!Existing)
                {
                    FParsedFinding Added;
                    Added.Raw = Error;
                    Added.Finding = Finding;
                    Existing = &Findings[Findings.Add(MoveTemp(Added))];
                }
                Existing->FeatureLevels.AddUnique(FeatureLevel.Key);
            }
        }
        TotalErrorCount += LevelErrors.Num();
        ErrorsByFeatureLevel->SetArrayField(FeatureLevel.Key, LevelErrors);
    }

    // The engine names the node by its own Description, which every Custom node defaults to
    // ("Custom", MaterialExpressions.cpp:15494), so the raw text usually does not identify the
    // expression. Resolve it against the graph instead of guessing: one match is the answer,
    // several are reported as candidates.
    TArray<TSharedPtr<FJsonValue>> ErrorDetails;
    for (const FParsedFinding& Parsed : Findings)
    {
        TArray<TSharedPtr<FJsonValue>> LevelValues;
        for (const FString& Level : Parsed.FeatureLevels)
        {
            LevelValues.Add(MakeShareable(new FJsonValueString(Level)));
        }
        TArray<FString> NameCandidates;
        for (UMaterialExpression* Expr : Mat->GetExpressionCollection().Expressions)
        {
            const UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expr);
            if (Custom && Custom->Description == Parsed.Finding.NodeDescription)
            {
                NameCandidates.Add(Custom->GetName());
            }
        }

        TSharedPtr<FJsonObject> Detail = MakeShareable(new FJsonObject);
        Detail->SetStringField(TEXT("raw"), Parsed.Raw);
        Detail->SetStringField(TEXT("kind"), TEXT("custom_input_unconnected"));
        Detail->SetStringField(TEXT("node_description"), Parsed.Finding.NodeDescription);
        Detail->SetStringField(TEXT("pin_name"), Parsed.Finding.PinName);
        Detail->SetNumberField(TEXT("pin_index_1based"), Parsed.Finding.PinIndex1Based);
        Detail->SetStringField(TEXT("expression_name"), NameCandidates.Num() == 1 ? NameCandidates[0] : TEXT(""));
        if (NameCandidates.Num() != 1)
        {
            TArray<TSharedPtr<FJsonValue>> CandidateValues;
            for (const FString& Name : NameCandidates)
            {
                CandidateValues.Add(MakeShared<FJsonValueString>(Name));
            }
            Detail->SetArrayField(TEXT("expression_name_candidates"), CandidateValues);
        }
        Detail->SetArrayField(TEXT("feature_levels"), LevelValues);
        Detail->SetStringField(TEXT("hint"),
            TEXT("该输入未接线：对应引脚在 inputs 里的 connected 为 false。编译在第一个未接线的引脚处即中止，接好这一个之后可能再报下一个。"));
        ErrorDetails.Add(MakeShared<FJsonValueObject>(Detail));
    }

    // Fallback path: pass-level failures (e.g. LumenCardPS/VS) only appear
    // in the editor log. Scan the last 256KB of the project log for compile
    // markers only - matching the material name pulls in SavePackage /
    // thumbnail / AssetCheck noise that mentions the asset.
    // Boundary between "this compile" and history: the last time an MCP command
    // changed this material. An explicit `since` overrides it.
    FDateTime SinceBoundary;
    FString BoundarySource = TEXT("unknown");
    const TSharedPtr<FJsonValue> SinceValue = Params->TryGetField(TEXT("since"));
    if (SinceValue.IsValid() && TryParseSince(SinceValue, SinceBoundary))
    {
        BoundarySource = TEXT("explicit_since");
    }
    if (BoundarySource == TEXT("unknown"))
    {
        if (const FDateTime* RecordedMutation = GetMaterialMutationTimes().Find(AssetPath))
        {
            SinceBoundary = *RecordedMutation;
            BoundarySource = TEXT("material_mutation");
        }
    }

    const FDateTime QueryTime = FDateTime::UtcNow();
    TArray<TSharedPtr<FJsonValue>> ThisCompileErrors;
    TArray<TSharedPtr<FJsonValue>> HistoricalErrors;
    int32 ScannedLines = 0;
    int32 MatchedLines = 0;
    {
        const FString LogFilePath = FPaths::ProjectLogDir() / (FString(FApp::GetProjectName()) + TEXT(".log"));
        const int64 LogFileSize = IFileManager::Get().FileSize(*LogFilePath);
        if (LogFileSize > 0)
        {
            const int64 ReadSize = FMath::Min(LogFileSize, (int64)256 * 1024);
            const int64 ReadOffset = LogFileSize - ReadSize;
            // The editor holds the log open for writing, so a share-read-only
            // handle is refused with a sharing violation (err 32) and the whole
            // fallback would silently report nothing.
            TUniquePtr<FArchive> LogReader(IFileManager::Get().CreateFileReader(*LogFilePath, FILEREAD_AllowWrite));
            if (LogReader)
            {
                const int32 ReadSize32 = (int32)ReadSize;
                TArray<ANSICHAR> LogTail;
                LogTail.SetNumUninitialized(ReadSize32 + 1);
                LogReader->Seek(ReadOffset);
                LogReader->Serialize(LogTail.GetData(), ReadSize);
                LogReader->Close();
                LogTail[ReadSize32] = 0;

                const FUTF8ToTCHAR Converted(LogTail.GetData(), ReadSize32);
                const FString Tail(Converted.Get(), Converted.Length());

                TArray<FString> TailLines;
                Tail.ParseIntoArrayLines(TailLines);
                // When we started mid-line, the first fragment is truncated garbage.
                for (int32 LineIdx = (ReadOffset > 0) ? 1 : 0; LineIdx < TailLines.Num(); ++LineIdx)
                {
                    const FString& Line = TailLines[LineIdx];
                    ++ScannedLines;
                    const bool bCompileMarker =
                        Line.Contains(TEXT("Failed to compile Material")) ||
                        Line.Contains(TEXT("[Material] Error:")) ||
                        Line.Contains(TEXT("Compiler error"));
                    if (bCompileMarker)
                    {
                        ++MatchedLines;
                        FDateTime LineTime;
                        const bool bHasTime = ParseLogLineTimestamp(Line, LineTime);
                        TSharedPtr<FJsonObject> Entry = MakeShareable(new FJsonObject);
                        Entry->SetStringField(TEXT("line"), Line);
                        Entry->SetStringField(TEXT("timestamp"), bHasTime ? LineTime.ToIso8601() : TEXT(""));
                        Entry->SetNumberField(TEXT("age_seconds"),
                            bHasTime ? FMath::RoundToDouble((QueryTime - LineTime).GetTotalSeconds()) : -1.0);
                        // Without a boundary everything counts as history: better
                        // to look stale than to call an old failure current.
                        const bool bThisCompile = bHasTime
                            && BoundarySource != TEXT("unknown")
                            && LineTime >= SinceBoundary;
                        if (bThisCompile)
                        {
                            ThisCompileErrors.Add(MakeShareable(new FJsonValueObject(Entry)));
                        }
                        else
                        {
                            HistoricalErrors.Add(MakeShareable(new FJsonValueObject(Entry)));
                        }
                        if (MatchedLines >= 100)
                        {
                            break;
                        }
                    }
                }
            }
        }
    }

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
    ResultJson->SetObjectField(TEXT("errors_by_feature_level"), ErrorsByFeatureLevel);
    ResultJson->SetArrayField(TEXT("error_details"), ErrorDetails);
    ResultJson->SetArrayField(TEXT("log_errors_this_compile"), ThisCompileErrors);
    ResultJson->SetArrayField(TEXT("log_errors_historical"), HistoricalErrors);
    ResultJson->SetNumberField(TEXT("scanned_lines"), ScannedLines);
    ResultJson->SetNumberField(TEXT("matched_lines"), MatchedLines);
    ResultJson->SetNumberField(TEXT("total_error_count"), TotalErrorCount);
    ResultJson->SetStringField(TEXT("since_boundary"),
        BoundarySource == TEXT("unknown") ? TEXT("") : SinceBoundary.ToIso8601());
    ResultJson->SetStringField(TEXT("boundary_source"), BoundarySource);
    ResultJson->SetStringField(TEXT("hint"), TEXT("errors_by_feature_level is the authority on the CURRENT compile state; log_errors_this_compile / log_errors_historical are log-tail references only. error_details decodes known engine wordings. Shader compilation is asynchronous, so retry once it finishes if they are empty."));
    return ResultJson;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleListMaterialExpressions(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' parameter"));
    }

    UMaterial* Mat = LoadObject<UMaterial>(nullptr, *AssetPath);
    if (!Mat)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Material not found: %s"), *AssetPath));
    }

    const FMaterialExpressionCollection& ExprCollection = Mat->GetExpressionCollection();

    // Live nodes vs orphans: reachability from the material's property inputs.
    // A traversal failure would mean over-reporting orphans, so it is not treated
    // as "everything is unreferenced" - the reachable set only grows.
    TSet<UMaterialExpression*> ReachableExpressions;
    CollectReachableExpressions(Mat, ReachableExpressions);

    TArray<TSharedPtr<FJsonValue>> Expressions;
    int32 UnreferencedCount = 0;
    for (int32 ExprIdx = 0; ExprIdx < ExprCollection.Expressions.Num(); ++ExprIdx)
    {
        UMaterialExpression* Expr = ExprCollection.Expressions[ExprIdx];
        const bool bReferenced = Expr && ReachableExpressions.Contains(Expr);
        if (!bReferenced)
        {
            ++UnreferencedCount;
        }
        TSharedPtr<FJsonObject> ExprObj = MakeShareable(new FJsonObject);
        ExprObj->SetNumberField(TEXT("index"), ExprIdx);
        ExprObj->SetBoolField(TEXT("referenced"), bReferenced);
        ExprObj->SetStringField(TEXT("type"), Expr ? Expr->GetClass()->GetName() : TEXT(""));
        ExprObj->SetStringField(TEXT("name"), Expr ? Expr->GetName() : TEXT(""));
        ExprObj->SetStringField(TEXT("desc"), Expr ? Expr->Desc : TEXT(""));
        ExprObj->SetStringField(TEXT("object_path"), Expr ? Expr->GetPathName() : TEXT(""));
        if (Expr)
        {
            ExprObj->SetNumberField(TEXT("editor_x"), Expr->MaterialExpressionEditorX);
            ExprObj->SetNumberField(TEXT("editor_y"), Expr->MaterialExpressionEditorY);
            ExprObj->SetArrayField(TEXT("inputs"), BuildInputList(Expr));

            if (const UMaterialExpressionScalarParameter* SP = Cast<UMaterialExpressionScalarParameter>(Expr))
            {
                ExprObj->SetStringField(TEXT("parameter_name"), SP->ParameterName.ToString());
                ExprObj->SetStringField(TEXT("parameter_type"), TEXT("scalar"));
                ExprObj->SetNumberField(TEXT("default_value"), SP->DefaultValue);
            }
            else if (const UMaterialExpressionVectorParameter* VP = Cast<UMaterialExpressionVectorParameter>(Expr))
            {
                ExprObj->SetStringField(TEXT("parameter_name"), VP->ParameterName.ToString());
                ExprObj->SetStringField(TEXT("parameter_type"), TEXT("vector"));
                TSharedPtr<FJsonObject> Color = MakeShareable(new FJsonObject);
                Color->SetNumberField(TEXT("r"), VP->DefaultValue.R);
                Color->SetNumberField(TEXT("g"), VP->DefaultValue.G);
                Color->SetNumberField(TEXT("b"), VP->DefaultValue.B);
                Color->SetNumberField(TEXT("a"), VP->DefaultValue.A);
                ExprObj->SetObjectField(TEXT("default_value"), Color);
            }
        }
        Expressions.Add(MakeShareable(new FJsonValueObject(ExprObj)));
    }

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
    ResultJson->SetArrayField(TEXT("expressions"), Expressions);
    ResultJson->SetNumberField(TEXT("count"), Expressions.Num());
    ResultJson->SetNumberField(TEXT("unreferenced_count"), UnreferencedCount);
    return ResultJson;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleGetMaterialGraph(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' parameter"));
    }

    UMaterial* Mat = LoadObject<UMaterial>(nullptr, *AssetPath);
    if (!Mat)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Material not found: %s"), *AssetPath));
    }

    const FMaterialExpressionCollection& ExprCollection = Mat->GetExpressionCollection();
    auto& Exprs = ExprCollection.Expressions;
    const int32 N = Exprs.Num();

    TMap<UMaterialExpression*, int32> Idx;
    for (int32 i = 0; i < N; ++i)
    {
        if (Exprs[i]) { Idx.Add(Exprs[i], i); }
    }

    TArray<TSharedPtr<FJsonValue>> Edges;
    for (int32 i = 0; i < N; ++i)
    {
        UMaterialExpression* Expr = Exprs[i];
        if (!Expr) { continue; }
        const int32 InputCount = Expr->CountInputs();
        for (int32 Pin = 0; Pin < InputCount; ++Pin)
        {
            FExpressionInput* In = Expr->GetInput(Pin);
            if (!In || !In->Expression) { continue; }
            const int32* FromIdx = Idx.Find(In->Expression);
            if (!FromIdx) { continue; } // references outside the material graph

            TSharedPtr<FJsonObject> Edge = MakeShareable(new FJsonObject);
            Edge->SetNumberField(TEXT("from_index"), *FromIdx);
            Edge->SetNumberField(TEXT("to_index"), i);
            Edge->SetStringField(TEXT("input_name"), Expr->GetInputName(Pin).ToString());
            FString OutputName;
            const TArray<FExpressionOutput>& Outputs = In->Expression->GetOutputs();
            if (Outputs.IsValidIndex(In->OutputIndex))
            {
                OutputName = Outputs[In->OutputIndex].OutputName.ToString();
            }
            Edge->SetStringField(TEXT("output_name"), OutputName);
            Edges.Add(MakeShareable(new FJsonValueObject(Edge)));
        }
    }

    TArray<TSharedPtr<FJsonValue>> PropertyConnections;
    const TMap<FString, EMaterialProperty>& PropertyMap = GetMaterialPropertyMap();
    for (const TPair<FString, EMaterialProperty>& KV : PropertyMap)
    {
        FExpressionInput* In = Mat->GetExpressionInputForProperty(KV.Value);
        if (!In || !In->Expression) { continue; }
        const int32* FromIdx = Idx.Find(In->Expression);
        if (!FromIdx) { continue; }

        TSharedPtr<FJsonObject> Conn = MakeShareable(new FJsonObject);
        Conn->SetStringField(TEXT("property"), KV.Key);
        Conn->SetNumberField(TEXT("expression_index"), *FromIdx);
        FString OutputName;
        const TArray<FExpressionOutput>& Outputs = In->Expression->GetOutputs();
        if (Outputs.IsValidIndex(In->OutputIndex))
        {
            OutputName = Outputs[In->OutputIndex].OutputName.ToString();
        }
        Conn->SetStringField(TEXT("output_name"), OutputName);
        PropertyConnections.Add(MakeShareable(new FJsonValueObject(Conn)));
    }

    TArray<TSharedPtr<FJsonValue>> Parameters;
    for (int32 i = 0; i < N; ++i)
    {
        UMaterialExpression* Expr = Exprs[i];
        if (!Expr) { continue; }
        if (const UMaterialExpressionScalarParameter* SP = Cast<UMaterialExpressionScalarParameter>(Expr))
        {
            TSharedPtr<FJsonObject> P = MakeShareable(new FJsonObject);
            P->SetNumberField(TEXT("expression_index"), i);
            P->SetStringField(TEXT("parameter_name"), SP->ParameterName.ToString());
            P->SetStringField(TEXT("parameter_type"), TEXT("scalar"));
            P->SetNumberField(TEXT("default_value"), SP->DefaultValue);
            Parameters.Add(MakeShareable(new FJsonValueObject(P)));
        }
        else if (const UMaterialExpressionVectorParameter* VP = Cast<UMaterialExpressionVectorParameter>(Expr))
        {
            TSharedPtr<FJsonObject> P = MakeShareable(new FJsonObject);
            P->SetNumberField(TEXT("expression_index"), i);
            P->SetStringField(TEXT("parameter_name"), VP->ParameterName.ToString());
            P->SetStringField(TEXT("parameter_type"), TEXT("vector"));
            TSharedPtr<FJsonObject> Color = MakeShareable(new FJsonObject);
            Color->SetNumberField(TEXT("r"), VP->DefaultValue.R);
            Color->SetNumberField(TEXT("g"), VP->DefaultValue.G);
            Color->SetNumberField(TEXT("b"), VP->DefaultValue.B);
            Color->SetNumberField(TEXT("a"), VP->DefaultValue.A);
            P->SetObjectField(TEXT("default_value"), Color);
            Parameters.Add(MakeShareable(new FJsonValueObject(P)));
        }
    }

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
    ResultJson->SetNumberField(TEXT("expression_count"), N);
    ResultJson->SetArrayField(TEXT("edges"), Edges);
    ResultJson->SetArrayField(TEXT("property_connections"), PropertyConnections);
    ResultJson->SetArrayField(TEXT("parameters"), Parameters);
    return ResultJson;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleGetMaterialExpressionProperty(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath, PropertyName;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) ||
        !Params->TryGetStringField(TEXT("property"), PropertyName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path', expression_name/expression_desc/expression_type or 'property' parameter"));
    }

    UMaterial* Mat = LoadObject<UMaterial>(nullptr, *AssetPath);
    if (!Mat)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Material not found: %s"), *AssetPath));
    }

    auto& Exprs = Mat->GetExpressionCollection().Expressions;
    UMaterialExpression* Expr = nullptr;
    TSharedPtr<FJsonObject> ResolveError;
    if (!ResolveExpression(Mat->GetExpressionCollection(), Params, Expr, ResolveError)) return ResolveError;
    const int32 ExprIndex = Exprs.IndexOfByKey(Expr);

    // "inputs" is a virtual read: the shared structured pin list, not the raw
    // FArrayProperty (whose elements are not addressable through the setter).
    if (PropertyName.Equals(TEXT("inputs"), ESearchCase::IgnoreCase))
    {
        TSharedPtr<FJsonObject> InputsJson = MakeShareable(new FJsonObject);
        InputsJson->SetBoolField(TEXT("success"), true);
        InputsJson->SetStringField(TEXT("asset_path"), AssetPath);
        InputsJson->SetNumberField(TEXT("expression_index"), ExprIndex);
        InputsJson->SetStringField(TEXT("expression_name"), Expr->GetName());
        InputsJson->SetStringField(TEXT("expression_desc"), Expr->Desc);
        InputsJson->SetStringField(TEXT("property"), PropertyName);
        InputsJson->SetArrayField(TEXT("value"), BuildInputList(Expr));
        return InputsJson;
    }

    const FProperty* Prop = FindExpressionProperty(Expr, PropertyName);
    if (!Prop)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Property '%s' not found on expression type %s"), *PropertyName, *Expr->GetClass()->GetName()));
    }

    // One read implementation: the reflector owns the type dispatch and the value shapes, so a
    // material expression property reads back in exactly the shape the write path accepts.
    const TSharedPtr<FJsonValue> Value = FMCPPropertyReflector::ToJson(
        const_cast<FProperty*>(Prop), Prop->ContainerPtrToValuePtr<void>(Expr));
    if (!Value.IsValid() || Value->Type == EJson::Null)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Property '%s' (type %s) is not supported for reading"), *PropertyName, *Prop->GetClass()->GetName()));
    }

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
    ResultJson->SetNumberField(TEXT("expression_index"), ExprIndex);
    ResultJson->SetStringField(TEXT("property"), PropertyName);
    ResultJson->SetField(TEXT("value"), Value);
    return ResultJson;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleSetMaterialExpressionProperty(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath, PropertyName;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) ||
        !Params->TryGetStringField(TEXT("property"), PropertyName) ||
        !Params->HasField(TEXT("value")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path', expression_name/expression_desc/expression_type, 'property' or 'value' parameter"));
    }
    const TSharedPtr<FJsonValue>* JsonPtr = Params->Values.Find(TEXT("value"));
    const TSharedPtr<FJsonValue> JsonValue = (JsonPtr && JsonPtr->IsValid()) ? *JsonPtr : nullptr;
    bool bRecompile = false;
    Params->TryGetBoolField(TEXT("recompile"), bRecompile);

    UMaterial* Mat = LoadObject<UMaterial>(nullptr, *AssetPath);
    if (!Mat)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Material not found: %s"), *AssetPath));
    }

    auto& Exprs = Mat->GetExpressionCollection().Expressions;
    UMaterialExpression* Expr = nullptr;
    TSharedPtr<FJsonObject> ResolveError;
    if (!ResolveExpression(Mat->GetExpressionCollection(), Params, Expr, ResolveError)) return ResolveError;
    const int32 ExprIndex = Exprs.IndexOfByKey(Expr);

    const FProperty* Prop = FindExpressionProperty(Expr, PropertyName);
    if (!Prop)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Property '%s' not found on expression type %s"), *PropertyName, *Expr->GetClass()->GetName()));
    }
    // The reflector decides support and shape. A policy rejection (the Custom `inputs` array, a material
    // input) is refused here - before any Modify() - so the rejection has zero side effects.
    const FPropertyDescriptor PropDescriptor = FMCPPropertyReflector::Describe(Prop);
    const auto MakeStringArray = [](const TArray<FString>& In)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        Out.Reserve(In.Num());
        for (const FString& Item : In)
        {
            Out.Add(MakeShared<FJsonValueString>(Item));
        }
        return Out;
    };

    if (!PropDescriptor.bSupported)
    {
        TSharedPtr<FJsonObject> Rejection = MakeShareable(new FJsonObject);
        Rejection->SetBoolField(TEXT("success"), false);
        Rejection->SetStringField(TEXT("error"), TEXT("unsupported_property_type"));
        Rejection->SetStringField(TEXT("property"), PropertyName);
        Rejection->SetStringField(TEXT("property_type"), PropDescriptor.CppType);
        Rejection->SetStringField(TEXT("message"), FString::Printf(
            TEXT("Cannot set '%s': %s is not writable through set_material_expression_property"),
            *PropertyName, *PropDescriptor.CppType));
        Rejection->SetArrayField(TEXT("supported_types"),
            MakeStringArray(FMCPPropertyReflector::SupportedShapeVocabulary()));
        Rejection->SetArrayField(TEXT("supported_shapes"), MakeStringArray(PropDescriptor.SupportedShapes));
        if (!PropDescriptor.Hint.IsEmpty())
        {
            Rejection->SetStringField(TEXT("hint"), PropDescriptor.Hint);
        }
        return Rejection;
    }

    // Custom HLSL is linted here, not in the MCP tool layer, so every write path
    // (MCP tool call, python loopback) gets the same guard.
    TArray<TSharedPtr<FJsonValue>> HlslWarnings;
    if (PropertyName.Equals(TEXT("code"), ESearchCase::IgnoreCase))
    {
        if (const UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expr))
        {
            const TSharedPtr<FJsonObject> LintReport = FUnrealMCPMaterialHlslLint::Run(JsonValue->AsString(), CustomOutputTypeName(Custom->OutputType));
            const int32 LintErrorCount = LintReport->GetIntegerField(TEXT("error_count"));
            if (LintErrorCount > 0)
            {
                TSharedPtr<FJsonObject> Rejection = MakeShareable(new FJsonObject);
                Rejection->SetBoolField(TEXT("success"), false);
                Rejection->SetStringField(TEXT("error"), TEXT("invalid_custom_hlsl"));
                Rejection->SetStringField(TEXT("message"), TEXT("Custom HLSL rejected before the write; the material was not modified."));
                Rejection->SetArrayField(TEXT("errors"), LintReport->GetArrayField(TEXT("errors")));
                Rejection->SetArrayField(TEXT("warnings"), LintReport->GetArrayField(TEXT("warnings")));
                return Rejection;
            }
            HlslWarnings = LintReport->GetArrayField(TEXT("warnings"));
        }
    }

    Mat->Modify();
    Expr->Modify();
    const FWriteResult WriteResult = FMCPPropertyReflector::FromJson(
        const_cast<FProperty*>(Prop), Prop->ContainerPtrToValuePtr<void>(Expr), PropertyName, JsonValue);
    if (!WriteResult.bSuccess)
    {
        // Value-level rejection: Modify() stamped an undo step, but the property value itself is
        // untouched - the reflector never commits a partially written value.
        TSharedPtr<FJsonObject> Rejection = MakeShareable(new FJsonObject);
        Rejection->SetBoolField(TEXT("success"), false);
        Rejection->SetStringField(TEXT("error"), WriteResult.ErrorCode);
        Rejection->SetStringField(TEXT("property"), PropertyName);
        Rejection->SetStringField(TEXT("property_type"), PropDescriptor.CppType);
        Rejection->SetStringField(TEXT("message"), FString::Printf(
            TEXT("Cannot set '%s': %s"), *PropertyName, *WriteResult.ErrorMessage));
        Rejection->SetBoolField(TEXT("unchanged"), WriteResult.bUnchanged);
        if (WriteResult.SupportedShapes.Num() > 0)
        {
            Rejection->SetArrayField(TEXT("supported_shapes"), MakeStringArray(WriteResult.SupportedShapes));
        }
        if (WriteResult.AvailableFields.Num() > 0)
        {
            Rejection->SetArrayField(TEXT("available_fields"), MakeStringArray(WriteResult.AvailableFields));
        }
        if (WriteResult.FailedIndex != INDEX_NONE)
        {
            Rejection->SetNumberField(TEXT("failed_index"), WriteResult.FailedIndex);
        }
        if (!WriteResult.Hint.IsEmpty())
        {
            Rejection->SetStringField(TEXT("hint"), WriteResult.Hint);
        }
        return Rejection;
    }
    Mat->MarkPackageDirty();

    bool bRecompiled = false;
    if (bRecompile)
    {
        UMaterialEditingLibrary::RecompileMaterial(Mat);
        bRecompiled = true;
    }

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
    ResultJson->SetNumberField(TEXT("expression_index"), ExprIndex);
    ResultJson->SetStringField(TEXT("property"), PropertyName);
    ResultJson->SetBoolField(TEXT("recompiled"), bRecompiled);
    if (HlslWarnings.Num() > 0)
    {
        ResultJson->SetArrayField(TEXT("warnings"), HlslWarnings);
    }
    return ResultJson;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleCreateMaterialExpression(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath, ClassName;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) ||
        !Params->TryGetStringField(TEXT("expression_class"), ClassName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' or 'expression_class' parameter"));
    }
    double XD = 0, YD = 0;
    Params->TryGetNumberField(TEXT("editor_x"), XD);
    Params->TryGetNumberField(TEXT("editor_y"), YD);
    FString Desc;
    const bool bHasDesc = Params->TryGetStringField(TEXT("desc"), Desc);

    UMaterial* Mat = LoadObject<UMaterial>(nullptr, *AssetPath);
    if (!Mat)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Material not found: %s"), *AssetPath));
    }

    UClass* ExpressionClass = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *ClassName));
    if (!ExpressionClass)
    {
        ExpressionClass = FindFirstObject<UClass>(*ClassName);
    }
    if (!ExpressionClass || !ExpressionClass->IsChildOf(UMaterialExpression::StaticClass()))
    {
        TSharedPtr<FJsonObject> ErrJson = MakeShareable(new FJsonObject);
        ErrJson->SetBoolField(TEXT("success"), false);
        ErrJson->SetStringField(TEXT("error"), FString::Printf(TEXT("Unknown expression class: %s"), *ClassName));
        TArray<TSharedPtr<FJsonValue>> Hints;
        for (const TCHAR* Hint : { TEXT("MaterialExpressionScalarParameter"), TEXT("MaterialExpressionVectorParameter"),
             TEXT("MaterialExpressionConstant"), TEXT("MaterialExpressionConstant2Vector"), TEXT("MaterialExpressionConstant3Vector"),
             TEXT("MaterialExpressionMultiply"), TEXT("MaterialExpressionAdd"), TEXT("MaterialExpressionLinearInterpolate"),
             TEXT("MaterialExpressionTextureSample"), TEXT("MaterialExpressionCustom"), TEXT("MaterialExpressionTime"),
             TEXT("MaterialExpressionWorldPosition"), TEXT("MaterialExpressionFresnel") })
        {
            Hints.Add(MakeShareable(new FJsonValueString(Hint)));
        }
        ErrJson->SetArrayField(TEXT("hint_classes"), Hints);
        return ErrJson;
    }

    // Custom-only payload: HLSL and pins. Other classes get a hard error instead
    // of silently ignoring the shader they asked for.
    FString Code;
    const bool bHasCode = Params->TryGetStringField(TEXT("code"), Code);
    const TArray<TSharedPtr<FJsonValue>>* InputsArray = nullptr;
    const bool bHasInputs = Params->TryGetArrayField(TEXT("inputs"), InputsArray);

    if ((bHasCode || bHasInputs) && !ExpressionClass->IsChildOf(UMaterialExpressionCustom::StaticClass()))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("code/inputs are only supported for MaterialExpressionCustom, not %s"), *ClassName));
    }

    TArray<TSharedPtr<FJsonValue>> HlslWarnings;
    if (bHasCode)
    {
        // Lint before the node exists: a rejected shader must leave the graph untouched.
        const UMaterialExpressionCustom* CustomDefaults = GetDefault<UMaterialExpressionCustom>();
        const TSharedPtr<FJsonObject> LintReport =
            FUnrealMCPMaterialHlslLint::Run(Code, CustomOutputTypeName(CustomDefaults->OutputType));
        if (LintReport->GetIntegerField(TEXT("error_count")) > 0)
        {
            TSharedPtr<FJsonObject> Rejection = MakeShareable(new FJsonObject);
            Rejection->SetBoolField(TEXT("success"), false);
            Rejection->SetStringField(TEXT("error"), TEXT("invalid_custom_hlsl"));
            Rejection->SetStringField(TEXT("message"), TEXT("Custom HLSL rejected before the create; no expression was added."));
            Rejection->SetArrayField(TEXT("errors"), LintReport->GetArrayField(TEXT("errors")));
            Rejection->SetArrayField(TEXT("warnings"), LintReport->GetArrayField(TEXT("warnings")));
            return Rejection;
        }
        HlslWarnings = LintReport->GetArrayField(TEXT("warnings"));
    }

    Mat->Modify();
    UMaterialExpression* NewExpr = UMaterialEditingLibrary::CreateMaterialExpressionEx(
        Mat, nullptr, ExpressionClass, nullptr, FMath::RoundToInt32(XD), FMath::RoundToInt32(YD));
    if (!NewExpr)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create expression"));
    }
    if (bHasDesc)
    {
        NewExpr->Desc = Desc;
    }

    if (UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(NewExpr))
    {
        if (bHasCode)
        {
            Custom->Code = Code;
        }
        if (bHasInputs)
        {
            Custom->Inputs.Empty();
            for (const TSharedPtr<FJsonValue>& PinValue : *InputsArray)
            {
                FString PinName;
                if (PinValue.IsValid() && PinValue->Type == EJson::String)
                {
                    PinName = PinValue->AsString();
                }
                else if (PinValue.IsValid() && PinValue->Type == EJson::Object)
                {
                    PinValue->AsObject()->TryGetStringField(TEXT("input_name"), PinName);
                }
                if (PinName.IsEmpty())
                {
                    continue;
                }
                Custom->Inputs.AddDefaulted_GetRef().InputName = FName(*PinName);
            }
            // Rebuild the output metadata so the pins and the legal output names
            // are visible to the connect path immediately after creation.
            Custom->PostEditChange();
        }
    }
    Mat->MarkPackageDirty();

    auto& Exprs = Mat->GetExpressionCollection().Expressions;
    int32 NewIndex = INDEX_NONE;
    for (int32 i = 0; i < Exprs.Num(); ++i)
    {
        if (Exprs[i] == NewExpr) { NewIndex = i; break; }
    }

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
    ResultJson->SetStringField(TEXT("name"), NewExpr->GetName());
    ResultJson->SetStringField(TEXT("desc"), NewExpr->Desc);
    ResultJson->SetStringField(TEXT("type"), NewExpr->GetClass()->GetName());
    ResultJson->SetArrayField(TEXT("inputs"), BuildInputList(NewExpr));
    ResultJson->SetArrayField(TEXT("outputs"), BuildOutputList(NewExpr));
    if (HlslWarnings.Num() > 0)
    {
        ResultJson->SetArrayField(TEXT("warnings"), HlslWarnings);
    }
    if (NewIndex != INDEX_NONE)
    {
        ResultJson->SetNumberField(TEXT("expression_index"), NewIndex);
    }
    return ResultJson;
}

// UMaterialEditingLibrary::DeleteAllMaterialExpressions erases from the very
// array it iterates, so a single pass only removes part of the graph (observed:
// 102 -> 50 -> 24 -> 11). Delete from a snapshot and GC between passes until the
// collection is genuinely empty, then report what is actually left.
TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleWipeMaterialGraph(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' parameter"));
    }

    UMaterial* Mat = LoadObject<UMaterial>(nullptr, *AssetPath);
    if (!Mat)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Material not found: %s"), *AssetPath));
    }

    int32 MaxIterations = 32;
    double RequestedIterations = 0.0;
    if (Params->TryGetNumberField(TEXT("max_iterations"), RequestedIterations))
    {
        MaxIterations = FMath::Max(1, FMath::RoundToInt32(RequestedIterations));
    }

    Mat->Modify();

    auto& Exprs = Mat->GetExpressionCollection().Expressions;
    int32 Iterations = 0;
    int32 DeletedTotal = 0;
    while (Exprs.Num() > 0 && Iterations < MaxIterations)
    {
        ++Iterations;
        const int32 Before = Exprs.Num();
        TArray<UMaterialExpression*> Snapshot;
        Snapshot.Reserve(Before);
        for (const TObjectPtr<UMaterialExpression>& Expr : Exprs)
        {
            if (Expr)
            {
                Snapshot.Add(Expr);
            }
        }
        for (UMaterialExpression* Expr : Snapshot)
        {
            UMaterialEditingLibrary::DeleteMaterialExpression(Mat, Expr);
        }
        CollectGarbage(RF_NoFlags);
        DeletedTotal += FMath::Max(0, Before - Exprs.Num());
    }

    const int32 Remaining = Exprs.Num();
    Mat->MarkPackageDirty();

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
    ResultJson->SetNumberField(TEXT("iterations"), Iterations);
    ResultJson->SetNumberField(TEXT("deleted_total"), DeletedTotal);
    ResultJson->SetNumberField(TEXT("remaining"), Remaining);
    if (Remaining != 0)
    {
        ResultJson->SetBoolField(TEXT("success"), false);
        ResultJson->SetStringField(TEXT("error"), TEXT("wipe_incomplete"));
        ResultJson->SetStringField(TEXT("message"), FString::Printf(
            TEXT("wipe_material_graph stopped with %d expression(s) still in the graph after %d iteration(s)"),
            Remaining, Iterations));
        return ResultJson;
    }
    ResultJson->SetBoolField(TEXT("success"), true);
    return ResultJson;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleDeleteMaterialExpression(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' or expression_name/expression_desc/expression_type parameter"));
    }

    UMaterial* Mat = LoadObject<UMaterial>(nullptr, *AssetPath);
    if (!Mat)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Material not found: %s"), *AssetPath));
    }

    auto& Exprs = Mat->GetExpressionCollection().Expressions;
    UMaterialExpression* Victim = nullptr;
    TSharedPtr<FJsonObject> ResolveError;
    if (!ResolveExpression(Mat->GetExpressionCollection(), Params, Victim, ResolveError)) return ResolveError;
    Mat->Modify();
    UMaterialEditingLibrary::DeleteMaterialExpression(Mat, Victim);
    Mat->MarkPackageDirty();
    const int32 Remaining = Mat->GetExpressionCollection().Expressions.Num();

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
    ResultJson->SetStringField(TEXT("deleted_name"), Victim->GetName());
    ResultJson->SetNumberField(TEXT("remaining_count"), Remaining);
    return ResultJson;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleDeleteMaterialExpressions(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || !Params->TryGetArrayField(TEXT("expressions"), Items))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' or 'expressions' parameter"));
    }
    TArray<TSharedPtr<FJsonValue>> Results;
    int32 FailedCount = 0;
    for (const TSharedPtr<FJsonValue>& Item : *Items)
    {
        TSharedPtr<FJsonObject> ItemParams = Item.IsValid() ? Item->AsObject() : nullptr;
        if (!ItemParams.IsValid())
        {
            Results.Add(MakeShared<FJsonValueObject>(FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Each expression item must be an object"))));
            ++FailedCount;
            continue;
        }
        TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
        Request->SetStringField(TEXT("asset_path"), AssetPath);
        for (const auto& Pair : ItemParams->Values) Request->SetField(Pair.Key, Pair.Value);
        TSharedPtr<FJsonObject> ItemResult = HandleDeleteMaterialExpression(Request);
        bool bItemSuccess = false;
        if (!ItemResult->TryGetBoolField(TEXT("success"), bItemSuccess) || !bItemSuccess)
        {
            ++FailedCount;
        }
        Results.Add(MakeShared<FJsonValueObject>(ItemResult));
    }
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    // The top level used to be `true` no matter what the items did, so a batch where every item
    // failed still read as a success. It now reports what happened, and `partial` names the middle
    // case the way the PIE commands do.
    Result->SetBoolField(TEXT("success"), FailedCount == 0);
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetNumberField(TEXT("requested_count"), Items->Num());
    Result->SetNumberField(TEXT("deleted_count"), Items->Num() - FailedCount);
    Result->SetNumberField(TEXT("failed_count"), FailedCount);
    if (FailedCount > 0 && FailedCount < Items->Num())
    {
        Result->SetBoolField(TEXT("partial"), true);
    }
    Result->SetArrayField(TEXT("results"), Results);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleDisconnectMaterialProperty(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath, PropertyName;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) ||
        !Params->TryGetStringField(TEXT("property"), PropertyName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' or 'property' parameter"));
    }

    UMaterial* Mat = LoadObject<UMaterial>(nullptr, *AssetPath);
    if (!Mat)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Material not found: %s"), *AssetPath));
    }

    const TMap<FString, EMaterialProperty>& PropertyMap = GetMaterialPropertyMap();
    const EMaterialProperty* Prop = PropertyMap.Find(PropertyName);
    if (!Prop)
    {
        TSharedPtr<FJsonObject> ErrJson = MakeShareable(new FJsonObject);
        ErrJson->SetBoolField(TEXT("success"), false);
        ErrJson->SetStringField(TEXT("error"), FString::Printf(TEXT("Unknown material property: %s"), *PropertyName));
        TArray<TSharedPtr<FJsonValue>> Supported;
        for (const TPair<FString, EMaterialProperty>& KV : PropertyMap)
        {
            Supported.Add(MakeShareable(new FJsonValueString(KV.Key)));
        }
        ErrJson->SetArrayField(TEXT("supported_properties"), Supported);
        return ErrJson;
    }

    FExpressionInput* In = Mat->GetExpressionInputForProperty(*Prop);
    const bool bWasConnected = In && In->Expression != nullptr;

    Mat->Modify();
    if (In)
    {
        In->Expression = nullptr;
        In->OutputIndex = 0;
    }
    // Same wiring rule as the connect commands: no recompile here (a recompile rebuilds every shader
    // permutation; the caller finishes with recompile_material once the graph is in its final shape).
    Mat->MarkPackageDirty();

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
    ResultJson->SetStringField(TEXT("property"), PropertyName);
    ResultJson->SetBoolField(TEXT("was_connected"), bWasConnected);
    return ResultJson;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleGetMaterialParameters(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' parameter"));
    }

    UObject* Obj = LoadObject<UMaterialInterface>(nullptr, *AssetPath);
    if (!Obj)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Material not found: %s"), *AssetPath));
    }

    TArray<TSharedPtr<FJsonValue>> Parameters;

    if (UMaterial* Mat = Cast<UMaterial>(Obj))
    {
        const FMaterialExpressionCollection& ExprCollection = Mat->GetExpressionCollection();
        for (int32 i = 0; i < ExprCollection.Expressions.Num(); ++i)
        {
            UMaterialExpression* Expr = ExprCollection.Expressions[i];
            if (!Expr) { continue; }
            if (const UMaterialExpressionScalarParameter* SP = Cast<UMaterialExpressionScalarParameter>(Expr))
            {
                TSharedPtr<FJsonObject> P = MakeShareable(new FJsonObject);
                P->SetNumberField(TEXT("expression_index"), i);
                P->SetStringField(TEXT("parameter_name"), SP->ParameterName.ToString());
                P->SetStringField(TEXT("parameter_type"), TEXT("scalar"));
                P->SetNumberField(TEXT("value"), SP->DefaultValue);
                P->SetBoolField(TEXT("overridden"), true);
                Parameters.Add(MakeShareable(new FJsonValueObject(P)));
            }
            else if (const UMaterialExpressionVectorParameter* VP = Cast<UMaterialExpressionVectorParameter>(Expr))
            {
                TSharedPtr<FJsonObject> P = MakeShareable(new FJsonObject);
                P->SetNumberField(TEXT("expression_index"), i);
                P->SetStringField(TEXT("parameter_name"), VP->ParameterName.ToString());
                P->SetStringField(TEXT("parameter_type"), TEXT("vector"));
                TSharedPtr<FJsonObject> Color = MakeShareable(new FJsonObject);
                Color->SetNumberField(TEXT("r"), VP->DefaultValue.R);
                Color->SetNumberField(TEXT("g"), VP->DefaultValue.G);
                Color->SetNumberField(TEXT("b"), VP->DefaultValue.B);
                Color->SetNumberField(TEXT("a"), VP->DefaultValue.A);
                P->SetObjectField(TEXT("value"), Color);
                P->SetBoolField(TEXT("overridden"), true);
                Parameters.Add(MakeShareable(new FJsonValueObject(P)));
            }
        }

        TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
        ResultJson->SetBoolField(TEXT("success"), true);
        ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
        ResultJson->SetStringField(TEXT("asset_kind"), TEXT("material"));
        ResultJson->SetArrayField(TEXT("parameters"), Parameters);
        return ResultJson;
    }

    if (UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(Obj))
    {
        // Base defaults from the parent material's parameter expressions.
        const UMaterial* Base = MIC->GetMaterial();
        TMap<FName, double> ScalarDefaults;
        TMap<FName, FLinearColor> VectorDefaults;
        if (Base)
        {
            for (UMaterialExpression* Expr : Base->GetExpressionCollection().Expressions)
            {
                if (!Expr) { continue; }
                if (const UMaterialExpressionScalarParameter* SP = Cast<UMaterialExpressionScalarParameter>(Expr))
                {
                    ScalarDefaults.Add(SP->ParameterName, SP->DefaultValue);
                }
                else if (const UMaterialExpressionVectorParameter* VP = Cast<UMaterialExpressionVectorParameter>(Expr))
                {
                    VectorDefaults.Add(VP->ParameterName, VP->DefaultValue);
                }
            }
        }

        TMap<FName, float> ScalarOverrides;
        for (const FScalarParameterValue& V : MIC->ScalarParameterValues)
        {
            ScalarOverrides.Add(V.ParameterInfo.Name, V.ParameterValue);
        }
        TMap<FName, FLinearColor> VectorOverrides;
        for (const FVectorParameterValue& V : MIC->VectorParameterValues)
        {
            VectorOverrides.Add(V.ParameterInfo.Name, V.ParameterValue);
        }

        auto EmitScalar = [&](const FName& Name, float Value, bool bOverridden, int32 BaseIndex)
        {
            TSharedPtr<FJsonObject> P = MakeShareable(new FJsonObject);
            if (BaseIndex >= 0) { P->SetNumberField(TEXT("expression_index"), BaseIndex); }
            P->SetStringField(TEXT("parameter_name"), Name.ToString());
            P->SetStringField(TEXT("parameter_type"), TEXT("scalar"));
            P->SetNumberField(TEXT("value"), Value);
            P->SetBoolField(TEXT("overridden"), bOverridden);
            Parameters.Add(MakeShareable(new FJsonValueObject(P)));
        };
        auto EmitVector = [&](const FName& Name, const FLinearColor& Value, bool bOverridden, int32 BaseIndex)
        {
            TSharedPtr<FJsonObject> P = MakeShareable(new FJsonObject);
            if (BaseIndex >= 0) { P->SetNumberField(TEXT("expression_index"), BaseIndex); }
            P->SetStringField(TEXT("parameter_name"), Name.ToString());
            P->SetStringField(TEXT("parameter_type"), TEXT("vector"));
            TSharedPtr<FJsonObject> Color = MakeShareable(new FJsonObject);
            Color->SetNumberField(TEXT("r"), Value.R);
            Color->SetNumberField(TEXT("g"), Value.G);
            Color->SetNumberField(TEXT("b"), Value.B);
            Color->SetNumberField(TEXT("a"), Value.A);
            P->SetObjectField(TEXT("value"), Color);
            P->SetBoolField(TEXT("overridden"), bOverridden);
            Parameters.Add(MakeShareable(new FJsonValueObject(P)));
        };

        // Base parameters first (stable order), then instance-only overrides.
        if (Base)
        {
            int32 BaseIdx = 0;
            for (UMaterialExpression* Expr : Base->GetExpressionCollection().Expressions)
            {
                if (!Expr) { continue; }
                if (const UMaterialExpressionScalarParameter* SP = Cast<UMaterialExpressionScalarParameter>(Expr))
                {
                    const FName Name = SP->ParameterName;
                    const float* Ov = ScalarOverrides.Find(Name);
                    EmitScalar(Name, Ov ? *Ov : SP->DefaultValue, Ov != nullptr, BaseIdx);
                }
                else if (const UMaterialExpressionVectorParameter* VP = Cast<UMaterialExpressionVectorParameter>(Expr))
                {
                    const FName Name = VP->ParameterName;
                    const FLinearColor* Ov = VectorOverrides.Find(Name);
                    EmitVector(Name, Ov ? *Ov : VP->DefaultValue, Ov != nullptr, BaseIdx);
                }
                ++BaseIdx;
            }
        }
        for (const TPair<FName, float>& KV : ScalarOverrides)
        {
            if (!ScalarDefaults.Contains(KV.Key))
            {
                EmitScalar(KV.Key, KV.Value, true, -1);
            }
        }
        for (const TPair<FName, FLinearColor>& KV : VectorOverrides)
        {
            if (!VectorDefaults.Contains(KV.Key))
            {
                EmitVector(KV.Key, KV.Value, true, -1);
            }
        }

        TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
        ResultJson->SetBoolField(TEXT("success"), true);
        ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
        ResultJson->SetStringField(TEXT("asset_kind"), TEXT("material_instance"));
        ResultJson->SetArrayField(TEXT("parameters"), Parameters);
        return ResultJson;
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
        TEXT("Asset is neither a material nor a material instance: %s"), *AssetPath));
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleSetMaterialParameters(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    const TSharedPtr<FJsonObject>* ValuesObj = nullptr;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) ||
        !Params->TryGetObjectField(TEXT("values"), ValuesObj))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' or 'values' parameter"));
    }
    bool bRecompile = false;
    Params->TryGetBoolField(TEXT("recompile"), bRecompile);

    if ((*ValuesObj)->Values.Num() == 0)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'values' must contain at least one parameter"));
    }

    UObject* Obj = LoadObject<UMaterialInterface>(nullptr, *AssetPath);
    if (!Obj)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Material not found: %s"), *AssetPath));
    }

    TArray<TSharedPtr<FJsonValue>> Updated;
    TArray<TSharedPtr<FJsonValue>> Missing;

    if (UMaterial* Mat = Cast<UMaterial>(Obj))
    {
        auto& Exprs = Mat->GetExpressionCollection().Expressions;
        for (const TPair<FString, TSharedPtr<FJsonValue>>& KV : (*ValuesObj)->Values)
        {
            bool bFound = false;
            for (int32 i = 0; i < Exprs.Num() && !bFound; ++i)
            {
                UMaterialExpression* Expr = Exprs[i];
                if (!Expr) { continue; }
                if (const UMaterialExpressionScalarParameter* SP = Cast<UMaterialExpressionScalarParameter>(Expr))
                {
                    if (SP->ParameterName.ToString().Equals(KV.Key, ESearchCase::CaseSensitive))
                    {
                        const FProperty* Prop = Expr->GetClass()->FindPropertyByName(TEXT("DefaultValue"));
                        FString Err;
                        if (Prop)
                        {
                            Mat->Modify();
                            Expr->Modify();
                            const FWriteResult WriteResult = FMCPPropertyReflector::FromJson(
                                const_cast<FProperty*>(Prop), Prop->ContainerPtrToValuePtr<void>(Expr), KV.Key, KV.Value);
                            if (!WriteResult.bSuccess)
                            {
                                Err = WriteResult.ErrorMessage;
                            }
                        }
                        else
                        {
                            Err = TEXT("DefaultValue property not found");
                        }
                        if (Err.IsEmpty())
                        {
                            TSharedPtr<FJsonObject> U = MakeShareable(new FJsonObject);
                            U->SetStringField(TEXT("parameter_name"), KV.Key);
                            U->SetStringField(TEXT("parameter_type"), TEXT("scalar"));
                            Updated.Add(MakeShareable(new FJsonValueObject(U)));
                        }
                        else
                        {
                            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
                                TEXT("Cannot set '%s': %s"), *KV.Key, *Err));
                        }
                        bFound = true;
                    }
                }
                else if (const UMaterialExpressionVectorParameter* VP = Cast<UMaterialExpressionVectorParameter>(Expr))
                {
                    if (VP->ParameterName.ToString().Equals(KV.Key, ESearchCase::CaseSensitive))
                    {
                        const FProperty* Prop = Expr->GetClass()->FindPropertyByName(TEXT("DefaultValue"));
                        FString Err;
                        if (Prop)
                        {
                            Mat->Modify();
                            Expr->Modify();
                            const FWriteResult WriteResult = FMCPPropertyReflector::FromJson(
                                const_cast<FProperty*>(Prop), Prop->ContainerPtrToValuePtr<void>(Expr), KV.Key, KV.Value);
                            if (!WriteResult.bSuccess)
                            {
                                Err = WriteResult.ErrorMessage;
                            }
                        }
                        else
                        {
                            Err = TEXT("DefaultValue property not found");
                        }
                        if (Err.IsEmpty())
                        {
                            TSharedPtr<FJsonObject> U = MakeShareable(new FJsonObject);
                            U->SetStringField(TEXT("parameter_name"), KV.Key);
                            U->SetStringField(TEXT("parameter_type"), TEXT("vector"));
                            Updated.Add(MakeShareable(new FJsonValueObject(U)));
                        }
                        else
                        {
                            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
                                TEXT("Cannot set '%s': %s"), *KV.Key, *Err));
                        }
                        bFound = true;
                    }
                }
            }
            if (!bFound)
            {
                Missing.Add(MakeShareable(new FJsonValueString(KV.Key)));
            }
        }

        bool bRecompiled = false;
        if (bRecompile)
        {
            UMaterialEditingLibrary::RecompileMaterial(Mat);
            bRecompiled = true;
        }
        Mat->MarkPackageDirty();

        TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
        ResultJson->SetBoolField(TEXT("success"), true);
        ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
        ResultJson->SetStringField(TEXT("asset_kind"), TEXT("material"));
        ResultJson->SetArrayField(TEXT("updated"), Updated);
        ResultJson->SetArrayField(TEXT("missing"), Missing);
        ResultJson->SetBoolField(TEXT("recompiled"), bRecompiled);
        return ResultJson;
    }

    if (UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(Obj))
    {
        // Valid names = parameters that exist on the base material or are
        // already overridden on this instance.
        TSet<FName> KnownNames;
        if (const UMaterial* Base = MIC->GetMaterial())
        {
            for (UMaterialExpression* Expr : Base->GetExpressionCollection().Expressions)
            {
                if (!Expr) { continue; }
                if (const UMaterialExpressionScalarParameter* SP = Cast<UMaterialExpressionScalarParameter>(Expr))
                {
                    KnownNames.Add(SP->ParameterName);
                }
                else if (const UMaterialExpressionVectorParameter* VP = Cast<UMaterialExpressionVectorParameter>(Expr))
                {
                    KnownNames.Add(VP->ParameterName);
                }
            }
        }
        for (const FScalarParameterValue& V : MIC->ScalarParameterValues) { KnownNames.Add(V.ParameterInfo.Name); }
        for (const FVectorParameterValue& V : MIC->VectorParameterValues) { KnownNames.Add(V.ParameterInfo.Name); }

        MIC->Modify();
        for (const TPair<FString, TSharedPtr<FJsonValue>>& KV : (*ValuesObj)->Values)
        {
            const FName ParamName(*KV.Key);
            if (!KnownNames.Contains(ParamName))
            {
                Missing.Add(MakeShareable(new FJsonValueString(KV.Key)));
                continue;
            }
            const TSharedPtr<FJsonObject>* ColorObj = nullptr;
            if (KV.Value->Type == EJson::Object && KV.Value->TryGetObject(ColorObj) && ColorObj)
            {
                FLinearColor C;
                C.R = (*ColorObj)->GetNumberField(TEXT("r"));
                C.G = (*ColorObj)->GetNumberField(TEXT("g"));
                C.B = (*ColorObj)->GetNumberField(TEXT("b"));
                C.A = (*ColorObj)->HasField(TEXT("a")) ? (*ColorObj)->GetNumberField(TEXT("a")) : 1.0;
                MIC->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(ParamName), C);
                TSharedPtr<FJsonObject> U = MakeShareable(new FJsonObject);
                U->SetStringField(TEXT("parameter_name"), KV.Key);
                U->SetStringField(TEXT("parameter_type"), TEXT("vector"));
                Updated.Add(MakeShareable(new FJsonValueObject(U)));
            }
            else if (KV.Value->Type == EJson::Number)
            {
                MIC->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(ParamName), (float)KV.Value->AsNumber());
                TSharedPtr<FJsonObject> U = MakeShareable(new FJsonObject);
                U->SetStringField(TEXT("parameter_name"), KV.Key);
                U->SetStringField(TEXT("parameter_type"), TEXT("scalar"));
                Updated.Add(MakeShareable(new FJsonValueObject(U)));
            }
            else
            {
                return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
                    TEXT("Cannot set '%s': value must be a number (scalar) or an {r,g,b,a} object (vector)"), *KV.Key));
            }
        }
        MIC->MarkPackageDirty();

        TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
        ResultJson->SetBoolField(TEXT("success"), true);
        ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
        ResultJson->SetStringField(TEXT("asset_kind"), TEXT("material_instance"));
        ResultJson->SetArrayField(TEXT("updated"), Updated);
        ResultJson->SetArrayField(TEXT("missing"), Missing);
        ResultJson->SetBoolField(TEXT("recompiled"), false);
        ResultJson->SetStringField(TEXT("note"), TEXT("Material instances update deferred; no explicit recompile required"));
        return ResultJson;
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
        TEXT("Asset is neither a material nor a material instance: %s"), *AssetPath));
}

namespace
{
    // Types that introduce a declaration, used to recognise `float3 foo` and
    // `float fireNoise(...)` as code-defined names rather than pin references.
    const TSet<FString>& HlslTypeNames()
    {
        static const TSet<FString> Names = {
            TEXT("float"), TEXT("float2"), TEXT("float3"), TEXT("float4"), TEXT("float2x2"),
            TEXT("float3x3"), TEXT("float4x4"), TEXT("half"), TEXT("half2"), TEXT("half3"),
            TEXT("half4"), TEXT("int"), TEXT("int2"), TEXT("int3"), TEXT("int4"), TEXT("uint"),
            TEXT("uint2"), TEXT("uint3"), TEXT("uint4"), TEXT("bool"), TEXT("bool2"),
            TEXT("bool3"), TEXT("bool4"), TEXT("matrix"), TEXT("double"), TEXT("void"),
            TEXT("texture2d"), TEXT("texture2darray"), TEXT("texture3d"), TEXT("texturecube"),
            TEXT("samplerstate"), TEXT("string"), TEXT("const"), TEXT("static"), TEXT("struct"),
        };
        return Names;
    }

    // Vocabulary that is not a Custom input pin: HLSL keywords/types/builtins plus
    // the engine helpers and View members the material translator exposes. A hit
    // here is never a "code references a pin that does not exist" signal.
    const TSet<FString>& HlslVocabulary()
    {
        static const TSet<FString> Words = {
            TEXT("if"), TEXT("else"), TEXT("for"), TEXT("while"), TEXT("do"), TEXT("return"),
            TEXT("break"), TEXT("continue"), TEXT("switch"), TEXT("case"), TEXT("default"),
            TEXT("const"), TEXT("static"), TEXT("struct"), TEXT("typedef"), TEXT("void"),
            TEXT("in"), TEXT("out"), TEXT("inout"), TEXT("true"), TEXT("false"), TEXT("define"),
            TEXT("float"), TEXT("float2"), TEXT("float3"), TEXT("float4"), TEXT("float2x2"),
            TEXT("float3x3"), TEXT("float4x4"), TEXT("half"), TEXT("half2"), TEXT("half3"),
            TEXT("half4"), TEXT("int"), TEXT("int2"), TEXT("int3"), TEXT("int4"), TEXT("uint"),
            TEXT("uint2"), TEXT("uint3"), TEXT("uint4"), TEXT("bool"), TEXT("matrix"), TEXT("double"),
            TEXT("sampler"), TEXT("samplerstate"), TEXT("texture2d"), TEXT("texture2darray"),
            TEXT("texture3d"), TEXT("texturecube"), TEXT("texturecubearray"),
            TEXT("abs"), TEXT("acos"), TEXT("all"), TEXT("any"), TEXT("asin"), TEXT("atan"),
            TEXT("atan2"), TEXT("ceil"), TEXT("clamp"), TEXT("cos"), TEXT("cosh"), TEXT("cross"),
            TEXT("ddx"), TEXT("ddy"), TEXT("degrees"), TEXT("determinant"), TEXT("distance"),
            TEXT("dot"), TEXT("exp"), TEXT("exp2"), TEXT("floor"), TEXT("fmod"), TEXT("frac"),
            TEXT("fwidth"), TEXT("isfinite"), TEXT("isinf"), TEXT("isnan"), TEXT("length"),
            TEXT("lerp"), TEXT("log"), TEXT("log10"), TEXT("log2"), TEXT("mad"), TEXT("max"),
            TEXT("min"), TEXT("modf"), TEXT("mul"), TEXT("normalize"), TEXT("pow"), TEXT("radians"),
            TEXT("reflect"), TEXT("refract"), TEXT("round"), TEXT("rsqrt"), TEXT("saturate"),
            TEXT("sign"), TEXT("sin"), TEXT("sincos"), TEXT("sinh"), TEXT("smoothstep"),
            TEXT("sqrt"), TEXT("step"), TEXT("tan"), TEXT("tanh"), TEXT("transpose"), TEXT("trunc"),
            TEXT("asfloat"), TEXT("asint"), TEXT("asuint"),
            TEXT("texture2dsample"), TEXT("texture2dsamplelevel"), TEXT("texture2darraysample"),
            TEXT("samplelevel"), TEXT("load"), TEXT("store"), TEXT("getdimensions"),
            TEXT("scenetexturelookup"), TEXT("scenetexturefetch"), TEXT("scenetexturefetchfunc"),
            TEXT("calcscenedepth"), TEXT("calcscenecustomdepth"), TEXT("getscenetextureviewsize"),
            TEXT("viewportuvtoscenetextureuv"), TEXT("viewportuvtobufferuv"),
            TEXT("svpositiontoworld"), TEXT("svpositiontotranslatedworld"), TEXT("svpositiontoclip"),
            TEXT("gettanhalffieldofview"), TEXT("getpreviewtranslation"), TEXT("getworldvieworigin"),
            TEXT("getscreenposition"), TEXT("getworldposition"), TEXT("converttodevicez"),
            TEXT("unroll"), TEXT("loop"), TEXT("branch"), TEXT("flatten"), TEXT("forceinline"),
            TEXT("dfhacktofloat"),
            TEXT("unpacknormal"), TEXT("view"), TEXT("realtime"), TEXT("buffersizeandinvsize"),
            TEXT("viewsizeandinvsize"), TEXT("uv"), TEXT("position"), TEXT("worldposition"),
        };
        return Words;
    }

    TSharedPtr<FJsonValue> MakeFinding(const TCHAR* Kind, const FString& Detail)
    {
        TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("kind"), Kind);
        Item->SetStringField(TEXT("detail"), Detail);
        return MakeShared<FJsonValueObject>(Item);
    }

    TArray<TSharedPtr<FJsonValue>> PinNameList(const UMaterialExpressionCustom* Custom)
    {
        TArray<TSharedPtr<FJsonValue>> Names;
        for (const FCustomInput& Input : Custom->Inputs)
        {
            Names.Add(MakeShared<FJsonValueString>(Input.InputName.ToString()));
        }
        return Names;
    }

    // Every pin carrying that name. A Custom node can hold duplicates (nothing in the engine
    // forbids it), so the callers below decide whether 0, 1 or many is usable.
    TArray<int32> FindCustomPins(const UMaterialExpressionCustom* Custom, const FString& PinName)
    {
        TArray<int32> Matches;
        for (int32 Index = 0; Index < Custom->Inputs.Num(); ++Index)
        {
            if (Custom->Inputs[Index].InputName.ToString() == PinName)
            {
                Matches.Add(Index);
            }
        }
        return Matches;
    }

    TSharedPtr<FJsonObject> CreateCodeError(const TCHAR* Code, const FString& Message)
    {
        TSharedPtr<FJsonObject> ErrJson = MakeShared<FJsonObject>();
        ErrJson->SetBoolField(TEXT("success"), false);
        ErrJson->SetStringField(TEXT("error"), Code);
        ErrJson->SetStringField(TEXT("message"), Message);
        return ErrJson;
    }
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleValidateCustomHlsl(const TSharedPtr<FJsonObject>& Params)
{
    FString Code, OutputType;
    if (!Params->TryGetStringField(TEXT("code"), Code))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'code' parameter"));
    }
    Params->TryGetStringField(TEXT("output_type"), OutputType);
    return FUnrealMCPMaterialHlslLint::Run(Code, OutputType);
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleValidateCustomExpression(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' parameter"));
    }

    UMaterial* Mat = LoadObject<UMaterial>(nullptr, *AssetPath);
    if (!Mat)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Material not found: %s"), *AssetPath));
    }

    UMaterialExpression* Expr = nullptr;
    TSharedPtr<FJsonObject> ResolveError;
    if (!ResolveExpression(Mat->GetExpressionCollection(), Params, Expr, ResolveError))
    {
        return ResolveError;
    }

    UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expr);
    if (!Custom)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("'%s' is a %s; only Custom expressions carry input pins to cross-check"), *Expr->GetName(), *Expr->GetClass()->GetName()));
    }

    TArray<FString> Pins;
    for (const FCustomInput& Input : Custom->Inputs)
    {
        Pins.Add(Input.InputName.ToString());
    }

    static const FRegexPattern InputLabelPattern(TEXT("^\\s*Input\\s*:\\s*(.*)$"), ERegexPatternFlags::CaseInsensitive);
    static const FRegexPattern DefinePattern(TEXT("^\\s*#\\s*define\\s+([A-Za-z_]\\w*)"));
    static const FRegexPattern IdentifierPattern(TEXT("([A-Za-z_]\\w*)"));
    static const FRegexPattern LabelNamePattern(TEXT("^([A-Za-z_]\\w*)"));
    const TArray<FString> Lines = FUnrealMCPMaterialHlslLint::StripHlslComments(Custom->Code);

    auto IsAssignmentTarget = [](const FString& Line, int32 End)
    {
        int32 Probe = End;
        while (Probe < Line.Len() && FChar::IsWhitespace(Line[Probe]))
        {
            ++Probe;
        }
        return Probe < Line.Len() && Line[Probe] == TEXT('=')
            && (Probe + 1 >= Line.Len() || Line[Probe + 1] != TEXT('='));
    };
    auto IsTypeSeparator = [](const FString& Gap)
    {
        for (const TCHAR Char : Gap)
        {
            if (!FChar::IsWhitespace(Char) && Char != TEXT('*'))
            {
                return false;
            }
        }
        return true;
    };

    // Pass 1: every name the code defines for itself. The engine binds pins by
    // plain name substitution, so a local variable, a local helper function, a
    // macro or a node Additional Define must never be read as a reference to a
    // pin that does not exist.
    TSet<FString> DefinedNames;
    for (const FCustomDefine& Define : Custom->AdditionalDefines)
    {
        DefinedNames.Add(Define.DefineName);
    }
    for (const FString& Line : Lines)
    {
        if (Line.TrimStartAndEnd().StartsWith(TEXT("#")))
        {
            FRegexMatcher DefineMatcher(DefinePattern, Line);
            while (DefineMatcher.FindNext())
            {
                DefinedNames.Add(DefineMatcher.GetCaptureGroup(1));
            }
            continue;
        }

        FString PreviousIdentifier;
        int32 PreviousEnd = INDEX_NONE;
        FRegexMatcher Matcher(IdentifierPattern, Line);
        while (Matcher.FindNext())
        {
            const FString Identifier = Matcher.GetCaptureGroup(1);
            const int32 Begin = Matcher.GetMatchBeginning();
            const int32 End = Matcher.GetMatchEnding();
            // Numbers swallow letters in HLSL literals (1.0e7 -> 'e7', 0x1F -> 'x1F').
            if (Begin > 0 && FChar::IsDigit(Line[Begin - 1]))
            {
                PreviousIdentifier = Identifier;
                PreviousEnd = End;
                continue;
            }
            const FString Gap = (PreviousEnd != INDEX_NONE) ? Line.Mid(PreviousEnd, Begin - PreviousEnd) : FString();
            const bool bTypeDefinesIt = !PreviousIdentifier.IsEmpty()
                && IsTypeSeparator(Gap)
                && HlslTypeNames().Contains(PreviousIdentifier.ToLower());
            if (IsAssignmentTarget(Line, End) || bTypeDefinesIt)
            {
                DefinedNames.Add(Identifier);
            }
            PreviousIdentifier = Identifier;
            PreviousEnd = End;
        }
    }

    TArray<TSharedPtr<FJsonValue>> Errors;
    TArray<TSharedPtr<FJsonValue>> Warnings;
    TSet<FString> UsedPins;
    TSet<FString> ReferencedIdentifiers;
    int32 ReferenceCount = 0;

    for (const FString& Line : Lines)
    {
        if (Line.TrimStartAndEnd().StartsWith(TEXT("#")))
        {
            continue;
        }

        // 'Input: X' lines are labels, not references; the engine does not parse
        // them, so a label no pin matches means code and node disagree.
        FRegexMatcher LabelMatcher(InputLabelPattern, Line);
        if (LabelMatcher.FindNext())
        {
            const FString Label = LabelMatcher.GetCaptureGroup(1).TrimStartAndEnd();
            FRegexMatcher NameMatcher(LabelNamePattern, Label);
            if (NameMatcher.FindNext())
            {
                const FString LabelName = NameMatcher.GetCaptureGroup(1);
                if (!Pins.Contains(LabelName))
                {
                    Errors.Add(MakeFinding(TEXT("input_decl_not_pin"),
                        FString::Printf(TEXT("'Input: %s' is a label only; no pin is named '%s'"), *LabelName, *LabelName)));
                }
                else
                {
                    UsedPins.Add(LabelName);
                }
            }
            continue;
        }

        FRegexMatcher Matcher(IdentifierPattern, Line);
        while (Matcher.FindNext())
        {
            const FString Identifier = Matcher.GetCaptureGroup(1);
            const int32 Begin = Matcher.GetMatchBeginning();
            const int32 End = Matcher.GetMatchEnding();
            // Member access (View.RealTime, color.rgb) is not a pin reference.
            if (Begin > 0 && Line[Begin - 1] == TEXT('.'))
            {
                continue;
            }
            // Letters inside a numeric literal (1.0e7, 0x1F) are not references.
            if (Begin > 0 && FChar::IsDigit(Line[Begin - 1]))
            {
                continue;
            }
            // 'foo:' labels and 'foo::' scopes.
            if (End < Line.Len() && Line[End] == TEXT(':'))
            {
                continue;
            }
            if (IsAssignmentTarget(Line, End))
            {
                continue;
            }
            if (HlslVocabulary().Contains(Identifier.ToLower()) || DefinedNames.Contains(Identifier))
            {
                continue;
            }
            ++ReferenceCount;
            if (Pins.Contains(Identifier))
            {
                UsedPins.Add(Identifier);
                continue;
            }
            // The engine passes '<pin>Sampler' alongside a Texture2D pin.
            if (Identifier.EndsWith(TEXT("Sampler")) && Pins.Contains(Identifier.LeftChop(7)))
            {
                UsedPins.Add(Identifier.LeftChop(7));
                continue;
            }
            if (!ReferencedIdentifiers.Contains(Identifier))
            {
                ReferencedIdentifiers.Add(Identifier);
                Errors.Add(MakeFinding(TEXT("code_reference_not_in_pins"),
                    FString::Printf(TEXT("Code references '%s' but no input pin or code-defined name has it (pins: %s)"), *Identifier, *FString::Join(Pins, TEXT(", ")))));
            }
        }
    }

    for (const FString& Pin : Pins)
    {
        if (!UsedPins.Contains(Pin))
        {
            Warnings.Add(MakeFinding(TEXT("pin_not_in_code"),
                FString::Printf(TEXT("Pin '%s' is not referenced in Code; renaming the pin alone leaves the code reading the old name"), *Pin)));
        }
    }

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
    ResultJson->SetStringField(TEXT("expression_name"), Custom->GetName());
    ResultJson->SetNumberField(TEXT("pin_count"), Pins.Num());
    ResultJson->SetNumberField(TEXT("code_reference_count"), ReferenceCount);
    ResultJson->SetArrayField(TEXT("errors"), Errors);
    ResultJson->SetArrayField(TEXT("warnings"), Warnings);
    return ResultJson;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleSetCustomInputName(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath, OldName, NewName;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) ||
        !Params->TryGetStringField(TEXT("old_name"), OldName) ||
        !Params->TryGetStringField(TEXT("new_name"), NewName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path', 'old_name' or 'new_name' parameter"));
    }
    bool bRecompile = false;
    Params->TryGetBoolField(TEXT("recompile"), bRecompile);

    UMaterial* Mat = LoadObject<UMaterial>(nullptr, *AssetPath);
    if (!Mat)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Material not found: %s"), *AssetPath));
    }

    UMaterialExpression* Expr = nullptr;
    TSharedPtr<FJsonObject> ResolveError;
    if (!ResolveExpression(Mat->GetExpressionCollection(), Params, Expr, ResolveError))
    {
        return ResolveError;
    }

    UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expr);
    if (!Custom)
    {
        TSharedPtr<FJsonObject> ErrJson = CreateCodeError(TEXT("not_a_custom_expression"),
            FString::Printf(TEXT("'%s' is a %s; only Custom expressions expose input pins"), *Expr->GetName(), *Expr->GetClass()->GetName()));
        ErrJson->SetStringField(TEXT("expression_name"), Expr->GetName());
        return ErrJson;
    }

    TArray<int32> Matches = FindCustomPins(Custom, OldName);

    if (Matches.Num() != 1)
    {
        TSharedPtr<FJsonObject> ErrJson = CreateCodeError(
            Matches.Num() == 0 ? TEXT("input_not_found") : TEXT("ambiguous_input"),
            Matches.Num() == 0
                ? FString::Printf(TEXT("No input pin named '%s' on %s"), *OldName, *Custom->GetName())
                : FString::Printf(TEXT("'%s' matches %d input pins on %s; the rename is ambiguous"), *OldName, Matches.Num(), *Custom->GetName()));
        ErrJson->SetStringField(TEXT("expression_name"), Custom->GetName());
        ErrJson->SetStringField(TEXT("requested_input"), OldName);
        ErrJson->SetArrayField(TEXT("available_inputs"), PinNameList(Custom));
        return ErrJson;
    }

    for (const FCustomInput& Input : Custom->Inputs)
    {
        if (Input.InputName.ToString() == NewName)
        {
            TSharedPtr<FJsonObject> ErrJson = CreateCodeError(TEXT("input_name_conflict"),
                FString::Printf(TEXT("'%s' already names an input pin on %s"), *NewName, *Custom->GetName()));
            ErrJson->SetStringField(TEXT("expression_name"), Custom->GetName());
            ErrJson->SetArrayField(TEXT("available_inputs"), PinNameList(Custom));
            return ErrJson;
        }
    }

    Mat->Modify();
    Custom->Modify();
    // Only the pin name changes: rebuilding Inputs[] would risk dropping the
    // FExpressionInput pointers the python API cannot read back (FCustomInput.Input
    // is protected there), so the upstream wiring is left untouched.
    Custom->Inputs[Matches[0]].InputName = FName(*NewName);
    Custom->PostEditChange();
    Mat->MarkPackageDirty();

    bool bRecompiled = false;
    if (bRecompile)
    {
        UMaterialEditingLibrary::RecompileMaterial(Mat);
        bRecompiled = true;
    }

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
    ResultJson->SetStringField(TEXT("expression_name"), Custom->GetName());
    ResultJson->SetStringField(TEXT("old_name"), OldName);
    ResultJson->SetStringField(TEXT("new_name"), NewName);
    ResultJson->SetBoolField(TEXT("recompiled"), bRecompiled);
    ResultJson->SetArrayField(TEXT("available_inputs"), PinNameList(Custom));
    return ResultJson;
}

//==============================================================================
// Custom input pins: added, removed and renamed one element at a time.
//
// The wiring of a pin lives in its own FCustomInput element (Materials/MaterialExpressionCustom.h:34-35),
// never in the upstream node, so editing the array element-wise keeps every other connection intact:
// appending a blank element, or shifting the tail along for an insert, only moves elements around.
// Rewriting the whole array from a JSON payload is what loses the wiring (the blank elements it
// builds have no upstream), which is why the generic property writer refuses FArrayProperty here.
//==============================================================================

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleAddCustomInput(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath, InputName;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) ||
        !Params->TryGetStringField(TEXT("input_name"), InputName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' or 'input_name' parameter"));
    }
    bool bRecompile = false;
    Params->TryGetBoolField(TEXT("recompile"), bRecompile);

    UMaterial* Mat = LoadObject<UMaterial>(nullptr, *AssetPath);
    if (!Mat)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Material not found: %s"), *AssetPath));
    }

    UMaterialExpression* Expr = nullptr;
    TSharedPtr<FJsonObject> ResolveError;
    if (!ResolveExpression(Mat->GetExpressionCollection(), Params, Expr, ResolveError))
    {
        return ResolveError;
    }

    UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expr);
    if (!Custom)
    {
        TSharedPtr<FJsonObject> ErrJson = CreateCodeError(TEXT("not_a_custom_expression"),
            FString::Printf(TEXT("'%s' is a %s; only Custom expressions expose input pins"), *Expr->GetName(), *Expr->GetClass()->GetName()));
        ErrJson->SetStringField(TEXT("expression_name"), Expr->GetName());
        return ErrJson;
    }

    if (FindCustomPins(Custom, InputName).Num() > 0)
    {
        TSharedPtr<FJsonObject> ErrJson = CreateCodeError(TEXT("input_name_conflict"),
            FString::Printf(TEXT("'%s' already names an input pin on %s"), *InputName, *Custom->GetName()));
        ErrJson->SetStringField(TEXT("expression_name"), Custom->GetName());
        ErrJson->SetArrayField(TEXT("available_inputs"), PinNameList(Custom));
        return ErrJson;
    }

    // Appending is the default: the array order IS the HLSL argument order, so inserting in the
    // middle renumbers every later argument of the existing code.
    int32 InsertIndex = Custom->Inputs.Num();
    double RequestedIndex = 0;
    if (Params->TryGetNumberField(TEXT("index"), RequestedIndex))
    {
        InsertIndex = FMath::RoundToInt32(RequestedIndex);
        if (InsertIndex < 0 || InsertIndex > Custom->Inputs.Num())
        {
            TSharedPtr<FJsonObject> ErrJson = CreateCodeError(TEXT("index_out_of_range"),
                FString::Printf(TEXT("index %d is outside 0..%d on %s"), InsertIndex, Custom->Inputs.Num(), *Custom->GetName()));
            ErrJson->SetStringField(TEXT("expression_name"), Custom->GetName());
            ErrJson->SetNumberField(TEXT("input_count"), Custom->Inputs.Num());
            ErrJson->SetArrayField(TEXT("available_inputs"), PinNameList(Custom));
            return ErrJson;
        }
    }

    Mat->Modify();
    Custom->Modify();
    Custom->Inputs.Insert(FCustomInput(), InsertIndex);
    Custom->Inputs[InsertIndex].InputName = FName(*InputName);
    // Rebuilds Outputs (MaterialExpressions.cpp:15617) so the new pin is visible to the connect path
    // and to a read of `inputs` right after this call.
    Custom->PostEditChange();
    Mat->MarkPackageDirty();

    bool bRecompiled = false;
    if (bRecompile)
    {
        UMaterialEditingLibrary::RecompileMaterial(Mat);
        bRecompiled = true;
    }

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
    ResultJson->SetStringField(TEXT("expression_name"), Custom->GetName());
    ResultJson->SetStringField(TEXT("input_name"), InputName);
    ResultJson->SetNumberField(TEXT("inserted_index"), InsertIndex);
    ResultJson->SetNumberField(TEXT("input_count"), Custom->Inputs.Num());
    ResultJson->SetBoolField(TEXT("recompiled"), bRecompiled);
    ResultJson->SetArrayField(TEXT("inputs"), BuildInputList(Custom));
    return ResultJson;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleRemoveCustomInput(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath, InputName;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) ||
        !Params->TryGetStringField(TEXT("input_name"), InputName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' or 'input_name' parameter"));
    }
    bool bRecompile = false;
    Params->TryGetBoolField(TEXT("recompile"), bRecompile);

    UMaterial* Mat = LoadObject<UMaterial>(nullptr, *AssetPath);
    if (!Mat)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Material not found: %s"), *AssetPath));
    }

    UMaterialExpression* Expr = nullptr;
    TSharedPtr<FJsonObject> ResolveError;
    if (!ResolveExpression(Mat->GetExpressionCollection(), Params, Expr, ResolveError))
    {
        return ResolveError;
    }

    UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expr);
    if (!Custom)
    {
        TSharedPtr<FJsonObject> ErrJson = CreateCodeError(TEXT("not_a_custom_expression"),
            FString::Printf(TEXT("'%s' is a %s; only Custom expressions expose input pins"), *Expr->GetName(), *Expr->GetClass()->GetName()));
        ErrJson->SetStringField(TEXT("expression_name"), Expr->GetName());
        return ErrJson;
    }

    const TArray<int32> Matches = FindCustomPins(Custom, InputName);
    if (Matches.Num() != 1)
    {
        TSharedPtr<FJsonObject> ErrJson = CreateCodeError(
            Matches.Num() == 0 ? TEXT("input_not_found") : TEXT("ambiguous_input"),
            Matches.Num() == 0
                ? FString::Printf(TEXT("No input pin named '%s' on %s"), *InputName, *Custom->GetName())
                : FString::Printf(TEXT("'%s' matches %d input pins on %s; the removal is ambiguous"), *InputName, Matches.Num(), *Custom->GetName()));
        ErrJson->SetStringField(TEXT("expression_name"), Custom->GetName());
        ErrJson->SetStringField(TEXT("requested_input"), InputName);
        ErrJson->SetArrayField(TEXT("available_inputs"), PinNameList(Custom));
        return ErrJson;
    }

    // The pin's FExpressionInput leaves with its element, so report what stopped being connected:
    // the upstream node keeps existing and simply feeds one less input.
    const FExpressionInput& Removed = Custom->Inputs[Matches[0]].Input;
    const FString DisconnectedFrom = Removed.Expression ? Removed.Expression->GetName() : FString();
    const int32 RemovedIndex = Matches[0];

    Mat->Modify();
    Custom->Modify();
    Custom->Inputs.RemoveAt(RemovedIndex);
    Custom->PostEditChange();
    Mat->MarkPackageDirty();

    bool bRecompiled = false;
    if (bRecompile)
    {
        UMaterialEditingLibrary::RecompileMaterial(Mat);
        bRecompiled = true;
    }

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
    ResultJson->SetStringField(TEXT("expression_name"), Custom->GetName());
    ResultJson->SetStringField(TEXT("removed_name"), InputName);
    ResultJson->SetNumberField(TEXT("removed_index"), RemovedIndex);
    ResultJson->SetBoolField(TEXT("was_connected"), !DisconnectedFrom.IsEmpty());
    ResultJson->SetStringField(TEXT("disconnected_from"), DisconnectedFrom);
    ResultJson->SetNumberField(TEXT("input_count"), Custom->Inputs.Num());
    ResultJson->SetBoolField(TEXT("recompiled"), bRecompiled);
    ResultJson->SetArrayField(TEXT("inputs"), BuildInputList(Custom));
    return ResultJson;
}

//==============================================================================
// Single-node preview material.
//
// draw_material_to_render_target() evaluates a whole MATERIAL, so looking at one node means
// routing that node into EmissiveColor somewhere. Duplicating the material and rewiring it does
// NOT work: the copy inherits the original's compiled shader map and no python-side call
// (RecompileMaterial, save, reload, rename, usage flags, material instance parent) makes the
// canvas draw pick up the edit - the copy keeps rendering the source's shader. A material created
// inside the session and left unwired (the shape the engine's own node preview uses) does render
// correctly, so this command builds one from scratch: a new material that receives a clone of the
// target expression plus everything upstream of it, wired into EmissiveColor, forced through a
// synchronous recompile. Nothing of the source material is modified.
//==============================================================================

namespace
{
    /** True when the object is an expression of the given material (its outer chain starts there). */
    bool BelongsToMaterial(const UObject* Object, const UMaterial* Material)
    {
        for (const UObject* Outer = Object ? Object->GetOuter() : nullptr; Outer; Outer = Outer->GetOuter())
        {
            if (Outer == Material)
            {
                return true;
            }
        }
        return false;
    }

    /** Object references that count as graph dependencies: input pins plus graph-only links
        (a NamedRerouteUsage points at its declaration outside its Inputs array). */
    void CollectExpressionDependencies(UMaterialExpression* Expression, const UMaterial* Material,
                                       TArray<UMaterialExpression*>& OutDependencies)
    {
        for (FExpressionInputIterator It{ Expression }; It; ++It)
        {
            if (It->Expression && BelongsToMaterial(It->Expression, Material))
            {
                OutDependencies.Add(It->Expression);
            }
        }

        for (TFieldIterator<FObjectPropertyBase> PropIt(Expression->GetClass()); PropIt; ++PropIt)
        {
            UObject* Value = PropIt->GetObjectPropertyValue_InContainer(Expression);
            UMaterialExpression* Referenced = Cast<UMaterialExpression>(Value);
            if (Referenced && BelongsToMaterial(Referenced, Material))
            {
                OutDependencies.Add(Referenced);
            }
        }

        // Custom node pins are FExpressionInput inside a struct array the iterator above does cover,
        // but their TextureObject/other struct members are not graph edges - nothing else to walk.
    }

    /** The target expression plus every expression upstream of it, parents first ordering not required. */
    void CollectPreviewClosure(UMaterialExpression* Target, const UMaterial* Material,
                              TArray<UMaterialExpression*>& OutExpressions)
    {
        TSet<UMaterialExpression*> Seen;
        TArray<UMaterialExpression*> Pending;
        Pending.Add(Target);
        while (Pending.Num() > 0)
        {
            UMaterialExpression* Current = Pending.Pop();
            if (!Current || Seen.Contains(Current))
            {
                continue;
            }
            Seen.Add(Current);
            OutExpressions.Add(Current);

            TArray<UMaterialExpression*> Dependencies;
            CollectExpressionDependencies(Current, Material, Dependencies);
            for (UMaterialExpression* Dependency : Dependencies)
            {
                Pending.Add(Dependency);
            }
        }
    }

    /** Point the clone's input pins and graph links at the clones instead of the source expressions. */
    void RemapPreviewLinks(UMaterialExpression* Clone, const TMap<UMaterialExpression*, UMaterialExpression*>& CloneMap)
    {
        for (FExpressionInputIterator It{ Clone }; It; ++It)
        {
            if (!It->Expression)
            {
                continue;
            }
            if (UMaterialExpression* const* Mapped = CloneMap.Find(It->Expression))
            {
                It->Expression = *Mapped;
            }
        }

        for (TFieldIterator<FObjectPropertyBase> PropIt(Clone->GetClass()); PropIt; ++PropIt)
        {
            UMaterialExpression* Referenced = Cast<UMaterialExpression>(PropIt->GetObjectPropertyValue_InContainer(Clone));
            if (!Referenced)
            {
                continue;
            }
            if (UMaterialExpression* const* Mapped = CloneMap.Find(Referenced))
            {
                PropIt->SetObjectPropertyValue_InContainer(Clone, *Mapped);
            }
        }
    }
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleBuildMaterialPreview(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' parameter"));
    }

    FString Channel = TEXT("rgba");
    Params->TryGetStringField(TEXT("channel"), Channel);
    Channel = Channel.ToLower();
    if (Channel != TEXT("rgba") && Channel != TEXT("r") && Channel != TEXT("g")
        && Channel != TEXT("b") && Channel != TEXT("a"))
    {
        TSharedPtr<FJsonObject> ErrJson = CreateCodeError(TEXT("invalid_channel"),
            FString::Printf(TEXT("channel must be 'rgba' or one of r/g/b/a, got '%s'"), *Channel));
        TArray<TSharedPtr<FJsonValue>> Available;
        for (const TCHAR* Candidate : { TEXT("rgba"), TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a") })
        {
            Available.Add(MakeShareable(new FJsonValueString(Candidate)));
        }
        ErrJson->SetArrayField(TEXT("available_channels"), Available);
        return ErrJson;
    }

    double OutputIndexValue = 0.0;
    Params->TryGetNumberField(TEXT("output_index"), OutputIndexValue);
    const int32 OutputIndex = FMath::Max(0, FMath::RoundToInt32(OutputIndexValue));

    UMaterial* Source = LoadObject<UMaterial>(nullptr, *AssetPath);
    if (!Source)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Material not found: %s"), *AssetPath));
    }

    UMaterialExpression* Target = nullptr;
    TSharedPtr<FJsonObject> ResolveError;
    if (!ResolveExpression(Source->GetExpressionCollection(), Params, Target, ResolveError))
    {
        return ResolveError;
    }
    if (!Target)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Source expression is null"));
    }

    const TArray<FExpressionOutput>& Outputs = Target->GetOutputs();
    if (Outputs.Num() > 0 && OutputIndex >= Outputs.Num())
    {
        TSharedPtr<FJsonObject> ErrJson = CreateCodeError(TEXT("output_index_not_found"),
            FString::Printf(TEXT("%s has %d outputs, output_index %d does not exist"),
                *Target->GetName(), Outputs.Num(), OutputIndex));
        ErrJson->SetStringField(TEXT("expression_name"), Target->GetName());
        ErrJson->SetArrayField(TEXT("available_outputs"), BuildOutputList(Target));
        return ErrJson;
    }

    FString TempFolder = TEXT("/Game/MCP/_Preview");
    Params->TryGetStringField(TEXT("temp_folder"), TempFolder);
    FString TempName;
    if (!Params->TryGetStringField(TEXT("temp_name"), TempName) || TempName.IsEmpty())
    {
        TempName = FString::Printf(TEXT("PM_%s"), *Source->GetName());
    }

    const FString TempAssetPath = FString::Printf(TEXT("%s/%s"), *TempFolder, *TempName);
    const FString TempObjectPath = FString::Printf(TEXT("%s.%s"), *TempAssetPath, *TempName);

    // The caller owns the temp asset's lifetime (it previews, then deletes it). Refuse to write into
    // an existing one instead of silently creating a differently named sibling.
    if (FindObject<UMaterial>(nullptr, *TempObjectPath) != nullptr)
    {
        TSharedPtr<FJsonObject> ErrJson = CreateCodeError(TEXT("temp_asset_exists"),
            FString::Printf(TEXT("Preview material %s already exists; delete it before building a new preview"), *TempAssetPath));
        ErrJson->SetStringField(TEXT("temp_asset_path"), TempAssetPath);
        return ErrJson;
    }

    UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
    UMaterial* Preview = Cast<UMaterial>(FUnrealMCPCommonUtils::CreateAssetDirect(
        TempName, TempFolder, UMaterial::StaticClass(), Factory));
    if (!Preview)
    {
        TSharedPtr<FJsonObject> ErrJson = CreateCodeError(TEXT("temp_create_failed"),
            FString::Printf(TEXT("Could not create the preview material %s"), *TempAssetPath));
        ErrJson->SetStringField(TEXT("temp_asset_path"), TempAssetPath);
        return ErrJson;
    }

    // From here on every failure reports temp_asset_path so the caller can clean the asset up.
    TArray<UMaterialExpression*> Closure;
    CollectPreviewClosure(Target, Source, Closure);

    Preview->Modify();
    FMaterialExpressionCollection& PreviewCollection = Preview->GetExpressionCollection();
    TMap<UMaterialExpression*, UMaterialExpression*> CloneMap;
    for (UMaterialExpression* SourceExpression : Closure)
    {
        UMaterialExpression* Clone = DuplicateObject<UMaterialExpression>(
            SourceExpression, Preview, SourceExpression->GetFName());
        if (!Clone)
        {
            TSharedPtr<FJsonObject> ErrJson = CreateCodeError(TEXT("clone_failed"),
                FString::Printf(TEXT("Could not clone expression %s"), *SourceExpression->GetName()));
            ErrJson->SetStringField(TEXT("temp_asset_path"), TempAssetPath);
            return ErrJson;
        }
        CloneMap.Add(SourceExpression, Clone);
        PreviewCollection.AddExpression(Clone);
    }

    for (const TPair<UMaterialExpression*, UMaterialExpression*>& Pair : CloneMap)
    {
        RemapPreviewLinks(Pair.Value, CloneMap);
        // Output metadata (Custom additional outputs, masks) is rebuilt by the expression itself.
        Pair.Value->PostEditChange();
    }

    UMaterialExpression* const* TargetClonePtr = CloneMap.Find(Target);
    UMaterialExpression* TargetClone = TargetClonePtr ? *TargetClonePtr : nullptr;
    if (!TargetClone)
    {
        TSharedPtr<FJsonObject> ErrJson = CreateCodeError(TEXT("clone_failed"),
            FString::Printf(TEXT("The cloned graph does not contain %s"), *Target->GetName()));
        ErrJson->SetStringField(TEXT("temp_asset_path"), TempAssetPath);
        return ErrJson;
    }

    // Unlit, so the preview is the node's value and nothing else: no lighting, no base colour chain
    // (the copy has no other pins connected at all).
    Preview->SetShadingModel(MSM_Unlit);

    UMaterialExpression* EmissiveSource = TargetClone;
    bool bChannelMaskCreated = false;
    if (Channel != TEXT("rgba"))
    {
        UMaterialExpressionComponentMask* Mask = Cast<UMaterialExpressionComponentMask>(
            UMaterialEditingLibrary::CreateMaterialExpressionEx(
                Preview, nullptr, UMaterialExpressionComponentMask::StaticClass(), nullptr, 0, 0));
        if (!Mask)
        {
            TSharedPtr<FJsonObject> ErrJson = CreateCodeError(TEXT("channel_mask_failed"),
                FString::Printf(TEXT("Could not create the channel mask for channel '%s'"), *Channel));
            ErrJson->SetStringField(TEXT("temp_asset_path"), TempAssetPath);
            return ErrJson;
        }
        Mask->R = Channel == TEXT("r");
        Mask->G = Channel == TEXT("g");
        Mask->B = Channel == TEXT("b");
        Mask->A = Channel == TEXT("a");

        FExpressionInputIterator MaskInputs{ Mask };
        if (MaskInputs)
        {
            MaskInputs->Expression = TargetClone;
            MaskInputs->OutputIndex = OutputIndex;
        }
        Mask->PostEditChange();
        EmissiveSource = Mask;
        bChannelMaskCreated = true;
    }

    FExpressionInput* EmissiveInput = Preview->GetExpressionInputForProperty(MP_EmissiveColor);
    if (!EmissiveInput)
    {
        TSharedPtr<FJsonObject> ErrJson = CreateCodeError(TEXT("emissive_input_missing"),
            TEXT("The preview material exposes no EmissiveColor input"));
        ErrJson->SetStringField(TEXT("temp_asset_path"), TempAssetPath);
        return ErrJson;
    }
    EmissiveInput->Expression = EmissiveSource;
    EmissiveInput->OutputIndex = bChannelMaskCreated ? 0 : OutputIndex;
    // A stale channel mask on the input would silently zero channels out.
    EmissiveInput->Mask = 0;
    EmissiveInput->MaskR = 0;
    EmissiveInput->MaskG = 0;
    EmissiveInput->MaskB = 0;
    EmissiveInput->MaskA = 0;

    Preview->MarkPackageDirty();

    // RecompileMaterial first (it is the engine's own material update path: PostEditChange inside an
    // FMaterialUpdateContext), then force the compile to be synchronous - the caller draws the moment
    // this returns, and a queued compile would make that draw render the material before this wiring.
    UMaterialEditingLibrary::RecompileMaterial(Preview);
    Preview->ForceRecompileForRendering(EMaterialShaderPrecompileMode::Synchronous);

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("source_asset_path"), Source->GetPathName());
    ResultJson->SetStringField(TEXT("temp_asset_path"), TempAssetPath);
    ResultJson->SetStringField(TEXT("temp_object_path"), TempObjectPath);
    ResultJson->SetStringField(TEXT("expression_name"), Target->GetName());
    ResultJson->SetStringField(TEXT("expression_desc"), Target->Desc);
    ResultJson->SetStringField(TEXT("expression_type"), Target->GetClass()->GetName());
    ResultJson->SetStringField(TEXT("preview_expression_name"), TargetClone->GetName());
    ResultJson->SetNumberField(TEXT("output_index"), OutputIndex);
    ResultJson->SetStringField(TEXT("channel"), Channel);
    ResultJson->SetBoolField(TEXT("channel_mask_created"), bChannelMaskCreated);
    ResultJson->SetStringField(TEXT("shading_model"), TEXT("MSM_UNLIT"));
    ResultJson->SetNumberField(TEXT("copied_nodes"), CloneMap.Num());
    ResultJson->SetArrayField(TEXT("outputs"), BuildOutputList(Target));
    return ResultJson;
}




