#include "Commands/Niagara/UnrealMCPNiagaraCommands.h"

#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Commands/Niagara/UnrealMCPNiagaraOps.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"
#include "UObject/Object.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealMCPNiagaraCommands, Log, All);

namespace
{
    // -----------------------------------------------------------------------
    // Parameter helpers
    // -----------------------------------------------------------------------

    FString GetStringParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName,
                           const FString& DefaultValue = FString())
    {
        FString Value;
        if (Params.IsValid() && Params->TryGetStringField(FieldName, Value))
        {
            return Value;
        }
        return DefaultValue;
    }

    bool GetBoolParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, bool DefaultValue)
    {
        bool Value = DefaultValue;
        if (Params.IsValid() && Params->TryGetBoolField(FieldName, Value))
        {
            return Value;
        }
        return DefaultValue;
    }

    int32 GetIntParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, int32 DefaultValue)
    {
        int32 Value = DefaultValue;
        if (Params.IsValid() && Params->TryGetNumberField(FieldName, Value))
        {
            return Value;
        }
        return DefaultValue;
    }

    // -----------------------------------------------------------------------
    // Response shaping
    // -----------------------------------------------------------------------

    TSharedPtr<FJsonObject> MakeErrorJson(const FString& ErrorCode, const FString& ErrorMessage,
                                          const TArray<FString>& Candidates = TArray<FString>())
    {
        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(ErrorCode, ErrorMessage);
        if (Candidates.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> CandidateArray;
            for (const FString& Candidate : Candidates)
            {
                CandidateArray.Add(MakeShared<FJsonValueString>(Candidate));
            }
            Error->SetArrayField(TEXT("candidates"), CandidateArray);
        }
        return Error;
    }

    TSharedPtr<FJsonObject> MakeOpErrorJson(const FUnrealMCPNiagaraResult& Result)
    {
        return MakeErrorJson(Result.ErrorCode.IsEmpty() ? TEXT("niagara_op_failed") : Result.ErrorCode,
                             Result.ErrorMessage, Result.Candidates);
    }

    TSharedPtr<FJsonObject> MakeSuccessJson(const FString& AssetPath)
    {
        TSharedPtr<FJsonObject> Success = FUnrealMCPCommonUtils::CreateSuccessResponse();
        if (!AssetPath.IsEmpty())
        {
            Success->SetStringField(TEXT("asset_path"), AssetPath);
        }
        return Success;
    }

    TSharedPtr<FJsonObject> EmitterToJson(const FUnrealMCPNiagaraEmitterInfo& Emitter)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("id"), Emitter.Id);
        Object->SetStringField(TEXT("name"), Emitter.Name);
        Object->SetBoolField(TEXT("enabled"), Emitter.bEnabled);
        Object->SetStringField(TEXT("emitter_path"), Emitter.EmitterPath);
        Object->SetStringField(TEXT("sim_target"), Emitter.SimTarget);
        Object->SetNumberField(TEXT("simulation_stage_count"), Emitter.SimulationStageCount);
        TArray<TSharedPtr<FJsonValue>> StageNames;
        for (const FString& StageName : Emitter.SimulationStageNames)
        {
            StageNames.Add(MakeShared<FJsonValueString>(StageName));
        }
        Object->SetArrayField(TEXT("simulation_stage_names"), StageNames);
        Object->SetBoolField(TEXT("local_space"), Emitter.bLocalSpace);
        Object->SetBoolField(TEXT("determinism"), Emitter.bDeterminism);
        Object->SetNumberField(TEXT("random_seed"), Emitter.RandomSeed);
        Object->SetBoolField(TEXT("interpolated_spawning"), Emitter.bInterpolatedSpawning);
        Object->SetNumberField(TEXT("pre_allocation_count"), Emitter.PreAllocationCount);
        TArray<TSharedPtr<FJsonValue>> Renderers;
        for (const FString& Class : Emitter.RendererClasses)
        {
            Renderers.Add(MakeShared<FJsonValueString>(Class));
        }
        Object->SetArrayField(TEXT("renderers"), Renderers);
        return Object;
    }

    TArray<TSharedPtr<FJsonValue>> EmittersToJson(const TArray<FUnrealMCPNiagaraEmitterInfo>& Emitters)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        for (const FUnrealMCPNiagaraEmitterInfo& Emitter : Emitters)
        {
            Array.Add(MakeShared<FJsonValueObject>(EmitterToJson(Emitter)));
        }
        return Array;
    }

    TArray<TSharedPtr<FJsonValue>> ParametersToJson(const TArray<FUnrealMCPNiagaraParameterInfo>& Parameters)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        for (const FUnrealMCPNiagaraParameterInfo& Parameter : Parameters)
        {
            TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
            Object->SetStringField(TEXT("name"), Parameter.Name);
            Object->SetStringField(TEXT("type"), Parameter.Type);
            Object->SetStringField(TEXT("value"), Parameter.Value);
            Array.Add(MakeShared<FJsonValueObject>(Object));
        }
        return Array;
    }

    TArray<TSharedPtr<FJsonValue>> ModulesToJson(const TArray<FUnrealMCPNiagaraModuleInfo>& Modules)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        for (const FUnrealMCPNiagaraModuleInfo& Module : Modules)
        {
            TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
            Object->SetStringField(TEXT("guid"), Module.Guid);
            Object->SetStringField(TEXT("name"), Module.Name);
            Object->SetStringField(TEXT("stage"), Module.Stage);
            Object->SetBoolField(TEXT("enabled"), Module.bEnabled);
            Object->SetStringField(TEXT("script_path"), Module.ScriptPath);
            Object->SetNumberField(TEXT("editor_x"), Module.PosX);
            Object->SetNumberField(TEXT("editor_y"), Module.PosY);
            Object->SetNumberField(TEXT("order"), Module.Order);
            Array.Add(MakeShared<FJsonValueObject>(Object));
        }
        return Array;
    }

    TArray<TSharedPtr<FJsonValue>> MessagesToJson(const TArray<FUnrealMCPNiagaraCompileMessage>& Messages,
                                                  const FString& WantedSeverity)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        for (const FUnrealMCPNiagaraCompileMessage& Message : Messages)
        {
            if (!WantedSeverity.IsEmpty() && !Message.Severity.Equals(WantedSeverity, ESearchCase::IgnoreCase))
            {
                continue;
            }
            TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
            Object->SetStringField(TEXT("severity"), Message.Severity);
            Object->SetStringField(TEXT("script_path"), Message.ScriptPath);
            Object->SetStringField(TEXT("message"), Message.Message);
            if (!Message.NodeGuid.IsEmpty())
            {
                Object->SetStringField(TEXT("node_guid"), Message.NodeGuid);
            }
            Array.Add(MakeShared<FJsonValueObject>(Object));
        }
        return Array;
    }

    TArray<TSharedPtr<FJsonValue>> ScriptStatusesToJson(const TArray<FUnrealMCPNiagaraScriptStatus>& Statuses)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        for (const FUnrealMCPNiagaraScriptStatus& Status : Statuses)
        {
            TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
            Object->SetStringField(TEXT("script_path"), Status.ScriptPath);
            Object->SetStringField(TEXT("usage"), Status.Usage);
            Object->SetStringField(TEXT("status"), Status.Status);
            Array.Add(MakeShared<FJsonValueObject>(Object));
        }
        return Array;
    }

    TArray<TSharedPtr<FJsonValue>> NodesToJson(const TArray<FUnrealMCPNiagaraNodeInfo>& Nodes)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        for (const FUnrealMCPNiagaraNodeInfo& Node : Nodes)
        {
            TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
            Object->SetStringField(TEXT("guid"), Node.Guid);
            Object->SetStringField(TEXT("type"), Node.Type);
            Object->SetStringField(TEXT("title"), Node.Name);
            Object->SetNumberField(TEXT("editor_x"), Node.PosX);
            Object->SetNumberField(TEXT("editor_y"), Node.PosY);
            Array.Add(MakeShared<FJsonValueObject>(Object));
        }
        return Array;
    }

    TArray<TSharedPtr<FJsonValue>> PinsToJson(const TArray<FUnrealMCPNiagaraPinInfo>& Pins)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        for (const FUnrealMCPNiagaraPinInfo& Pin : Pins)
        {
            TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
            Object->SetStringField(TEXT("name"), Pin.Name);
            Object->SetStringField(TEXT("direction"), Pin.Direction);
            Object->SetStringField(TEXT("type"), Pin.Type);
            Object->SetStringField(TEXT("node_guid"), Pin.NodeGuid);
            Object->SetBoolField(TEXT("connected"), Pin.bConnected);
            Array.Add(MakeShared<FJsonValueObject>(Object));
        }
        return Array;
    }

    /**
     * The Grid2D attribute table. `first_slice` is the tile a RenderTarget copy writes that attribute
     * to, so the list doubles as the tile index <-> name map.
     */
    TArray<TSharedPtr<FJsonValue>> GridAttributesToJson(const TArray<FUnrealMCPNiagaraGridAttributeInfo>& Attributes)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        for (const FUnrealMCPNiagaraGridAttributeInfo& Attribute : Attributes)
        {
            TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
            Object->SetStringField(TEXT("name"), Attribute.Name);
            Object->SetStringField(TEXT("type"), Attribute.Type);
            Object->SetNumberField(TEXT("first_slice"), Attribute.FirstSlice);
            Object->SetNumberField(TEXT("channels"), Attribute.Channels);
            Array.Add(MakeShared<FJsonValueObject>(Object));
        }
        return Array;
    }

    /**
     * Pin specs from the "input_pins" / "output_pins" arrays: [{"name": "Grid", "type": "Grid2D"}].
     * A pin whose name is missing is rejected here rather than becoming a nameless pin in the graph.
     */
    bool ReadPinSpecs(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, bool bOutput,
                      TArray<FUnrealMCPNiagaraPinSpec>& OutPins, FString& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
        if (!Params.IsValid() || !Params->TryGetArrayField(FieldName, Array) || Array == nullptr)
        {
            return true;
        }

        for (const TSharedPtr<FJsonValue>& Value : *Array)
        {
            const TSharedPtr<FJsonObject>* Object = nullptr;
            if (!Value.IsValid() || !Value->TryGetObject(Object) || Object == nullptr)
            {
                OutError = FString::Printf(TEXT("'%s' takes objects like {\"name\": \"X\", \"type\": \"vec3\"}"), *FieldName);
                return false;
            }

            FUnrealMCPNiagaraPinSpec Spec;
            Spec.Direction = bOutput ? TEXT("output") : TEXT("input");
            if (!(*Object)->TryGetStringField(TEXT("name"), Spec.Name) || Spec.Name.IsEmpty())
            {
                OutError = FString::Printf(TEXT("A pin in '%s' has no 'name'"), *FieldName);
                return false;
            }
            if (!(*Object)->TryGetStringField(TEXT("type"), Spec.Type) || Spec.Type.IsEmpty())
            {
                OutError = FString::Printf(TEXT("Pin '%s' in '%s' has no 'type'"), *Spec.Name, *FieldName);
                return false;
            }
            OutPins.Add(Spec);
        }
        return true;
    }
}

FUnrealMCPNiagaraCommands::FUnrealMCPNiagaraCommands()
{
}

void FUnrealMCPNiagaraCommands::RegisterCommands(FMCPCommandRegistry& Registry)
{
    // ------------------------------------------------------------------
    // Assets
    // ------------------------------------------------------------------
    MCP_REGISTER_COMMAND(Registry, "create_niagara_system", "niagara",
        "Create a Niagara system asset (empty, with the engine's default system scripts and graphs).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("name"), TEXT("string"), TEXT("Asset name")),
            MCPParamOpt(TEXT("folder"), TEXT("string"), TEXT("Content folder; default /Game/VFX")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("create_niagara_system"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleCreateNiagaraSystem(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "duplicate_niagara_system", "niagara",
        "Duplicate a Niagara system to another content path.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("source_path"), TEXT("string"), TEXT("System asset to copy")),
            MCPParamOpt(TEXT("destination_path"), TEXT("string"), TEXT("Full target path; default <source>_Copy")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("duplicate_niagara_system"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleDuplicateNiagaraSystem(P); }); }));

    // ------------------------------------------------------------------
    // Emitters
    // ------------------------------------------------------------------
    MCP_REGISTER_COMMAND(Registry, "list_niagara_emitters", "niagara",
        "List the emitter handles of a Niagara system (handle id, name, enabled, sim target, stage count).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("list_niagara_emitters"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleListNiagaraEmitters(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "add_niagara_emitter", "niagara",
        "Add an emitter to a Niagara system, from a template emitter asset or as a fresh emitter.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path")),
            MCPParamOpt(TEXT("emitter_name"), TEXT("string"), TEXT("Name for the new emitter handle; default \"Emitter\"")),
            MCPParamOpt(TEXT("source_emitter_path"), TEXT("string"), TEXT("Emitter asset to copy; omitted creates an empty emitter")),
            MCPParamOpt(TEXT("add_default_modules"), TEXT("bool"), TEXT("When creating an empty emitter, add the engine's default modules and renderers; default false")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_niagara_emitter"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddNiagaraEmitter(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "remove_niagara_emitter", "niagara",
        "Remove an emitter from a Niagara system by handle id or name.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path")),
            MCPParam(TEXT("emitter_name"), TEXT("string"), TEXT("Emitter handle id or emitter name")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("remove_niagara_emitter"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRemoveNiagaraEmitter(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_niagara_emitter_properties", "niagara",
        "Change simulator properties of an emitter (sim target, local space, determinism, interpolated spawning, pool size).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path")),
            MCPParam(TEXT("emitter_name"), TEXT("string"), TEXT("Emitter handle id or name")),
            MCPParamOpt(TEXT("sim_target"), TEXT("string"), TEXT("CPUSim / GPUComputeSim; a Grid2D or RenderTarget effect needs GPUComputeSim")),
            MCPParamOpt(TEXT("local_space"), TEXT("bool"), TEXT("Simulate in emitter space instead of world space")),
            MCPParamOpt(TEXT("determinism"), TEXT("bool"), TEXT("Deterministic simulation")),
            MCPParamOpt(TEXT("random_seed"), TEXT("int"), TEXT("Determinism seed")),
            MCPParamOpt(TEXT("interpolated_spawning"), TEXT("bool"), TEXT("Spawn with the interpolated spawning script")),
            MCPParamOpt(TEXT("pre_allocation_count"), TEXT("int"), TEXT("Particles allocated up front")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_niagara_emitter_properties"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetNiagaraEmitterProperties(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_niagara_renderer_material", "niagara",
        "Bind a material to one of an emitter's renderers (the material property is found by shape, not by a name table).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path")),
            MCPParam(TEXT("emitter_name"), TEXT("string"), TEXT("Emitter handle id or name")),
            MCPParamOpt(TEXT("material_path"), TEXT("string"), TEXT("Material or material instance; omitted clears it")),
            MCPParamOpt(TEXT("renderer_index"), TEXT("int"), TEXT("Renderer to change; default 0")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_niagara_renderer_material"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetNiagaraRendererMaterial(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "add_niagara_renderer", "niagara",
        "Add a renderer to an emitter (Sprite / Mesh / Ribbon / Light / Decal / Component), optionally binding a material - without a renderer nothing a solver computes is ever drawn.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path")),
            MCPParam(TEXT("emitter_name"), TEXT("string"), TEXT("Emitter handle id or name")),
            MCPParam(TEXT("renderer_type"), TEXT("string"), TEXT("Sprite / Mesh / Ribbon / Light / Decal / Component, or a full renderer class name")),
            MCPParamOpt(TEXT("material_path"), TEXT("string"), TEXT("Material or material instance to bind; omitted leaves it unset")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_niagara_renderer"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddNiagaraRenderer(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "remove_niagara_renderer", "niagara",
        "Remove one of an emitter's renderers by index (the order list_niagara_emitters reports).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path")),
            MCPParam(TEXT("emitter_name"), TEXT("string"), TEXT("Emitter handle id or name")),
            MCPParamOpt(TEXT("renderer_index"), TEXT("int"), TEXT("Renderer to remove; default 0")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("remove_niagara_renderer"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRemoveNiagaraRenderer(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "add_niagara_simulation_stage", "niagara",
        "Add a simulation stage (GPU iteration) to an emitter: the stage, its script and the stage's graph output.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path")),
            MCPParam(TEXT("emitter_name"), TEXT("string"), TEXT("Emitter handle id or name")),
            MCPParamOpt(TEXT("stage_name"), TEXT("string"), TEXT("Stage name; default \"Simulation Stage\"")),
            MCPParamOpt(TEXT("iteration_source"), TEXT("string"), TEXT("Particles / DataInterface / DirectSet; default Particles")),
            MCPParamOpt(TEXT("enabled"), TEXT("bool"), TEXT("Enable the stage; default true")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_niagara_simulation_stage"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddNiagaraSimulationStage(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_niagara_simulation_stage", "niagara",
        "Configure an existing simulation stage: name, enabled, iteration source, iteration count, bound DataInterface parameter.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path")),
            MCPParam(TEXT("emitter_name"), TEXT("string"), TEXT("Emitter handle id or name")),
            MCPParam(TEXT("stage"), TEXT("string"), TEXT("Stage name as list_niagara_emitters reports it, or its index")),
            MCPParamOpt(TEXT("stage_name"), TEXT("string"), TEXT("Rename the stage")),
            MCPParamOpt(TEXT("enabled"), TEXT("bool"), TEXT("Enable or disable the stage (a bitfield the generic property writer cannot reach)")),
            MCPParamOpt(TEXT("iteration_source"), TEXT("string"), TEXT("Particles / DataInterface / DirectSet")),
            MCPParamOpt(TEXT("num_iterations"), TEXT("int"), TEXT("Iteration count; only meaningful for the DirectSet source")),
            MCPParamOpt(TEXT("data_interface_parameter"), TEXT("string"), TEXT("A DataInterface user parameter to iterate over, e.g. \"User.WaterGrid\"")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_niagara_simulation_stage"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetNiagaraSimulationStage(P); }); }));

    // ------------------------------------------------------------------
    // Compile
    // ------------------------------------------------------------------
    MCP_REGISTER_COMMAND(Registry, "compile_niagara_system", "niagara",
        "Compile a Niagara system and read the per-script diagnostics back.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path")),
            MCPParamOpt(TEXT("wait"), TEXT("bool"), TEXT("Block until the compile finished; default true")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("compile_niagara_system"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleCompileNiagaraSystem(P); }); }));

    // ------------------------------------------------------------------
    // Parameters
    // ------------------------------------------------------------------
    MCP_REGISTER_COMMAND(Registry, "get_niagara_parameters", "niagara",
        "Read a system's user parameters (name, type, current value).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_niagara_parameters"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetNiagaraParameters(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_niagara_parameter", "niagara",
        "Write one user parameter of a system and read it back.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path")),
            MCPParam(TEXT("param_name"), TEXT("string"), TEXT("Parameter name; \"Gravity\" and \"User.Gravity\" are the same parameter")),
            MCPParam(TEXT("value"), TEXT("string"), TEXT("Value as text: \"12.5\", \"(X=0,Y=0,Z=-980)\", \"true\"")),
            MCPParamOpt(TEXT("param_type"), TEXT("string"), TEXT("float / int / bool / vec2 / vec3 / vec4 / color / position / quat; taken from the existing parameter when omitted")),
            MCPParamOpt(TEXT("add_if_missing"), TEXT("bool"), TEXT("Create the user parameter when it does not exist yet; default true")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_niagara_parameter"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetNiagaraParameter(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_niagara_data_interface_properties", "niagara",
        "Create a DataInterface user parameter (Grid2D collection, render target, array...) or configure one; "
        "with no properties it reports the DataInterface's editable property names.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path")),
            MCPParam(TEXT("param_name"), TEXT("string"), TEXT("User parameter name; \"Grid\" and \"User.Grid\" are the same")),
            MCPParamOpt(TEXT("data_interface_type"), TEXT("string"), TEXT("DataInterface class for a parameter that does not exist yet, e.g. \"Grid2DCollection\", \"Grid3DCollection\", \"RenderTarget2D\", \"ArrayFloat3\"")),
            MCPParamOpt(TEXT("properties"), TEXT("object"), TEXT("Properties to write, e.g. {\"ResolutionMaxAxis\": 256, \"CellSize\": 12.5}; omit to read the property list")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_niagara_data_interface_properties"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetNiagaraDataInterfaceProperties(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "list_niagara_grid_attributes", "niagara",
        "Read a Grid2DCollection user parameter's attribute table (name, type, first slice, channels) - "
        "the index <-> name map for the RenderTarget tiles a grid is copied into.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path")),
            MCPParam(TEXT("param_name"), TEXT("string"), TEXT("Grid2DCollection user parameter; \"WaterGrid\" and \"User.WaterGrid\" are the same")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("list_niagara_grid_attributes"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleListNiagaraGridAttributes(P); }); }));

    // ------------------------------------------------------------------
    // Stack modules
    // ------------------------------------------------------------------
    MCP_REGISTER_COMMAND(Registry, "list_niagara_modules", "niagara",
        "List the stack modules of one emitter (or of the system, when emitter_name is omitted).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path")),
            MCPParamOpt(TEXT("emitter_name"), TEXT("string"), TEXT("Emitter handle id or name; omitted lists the system stack")),
            MCPParamOpt(TEXT("stage"), TEXT("string"), TEXT("SystemSpawn / SystemUpdate / EmitterSpawn / EmitterUpdate / ParticleSpawn / ParticleUpdate, or a simulation stage of the emitter by name (\"SWE Sweep\") or index (\"0\"); omitted lists every stage")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("list_niagara_modules"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleListNiagaraModules(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "add_niagara_module", "niagara",
        "Add a module script to one stage of an emitter (or of the system) stack.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path")),
            MCPParam(TEXT("module_asset_path"), TEXT("string"), TEXT("Module script asset, e.g. /Niagara/Modules/Emitter/SpawnRate.SpawnRate")),
            MCPParam(TEXT("stage"), TEXT("string"), TEXT("SystemSpawn / SystemUpdate / EmitterSpawn / EmitterUpdate / ParticleSpawn / ParticleUpdate, or a simulation stage of the emitter by name or index")),
            MCPParamOpt(TEXT("emitter_name"), TEXT("string"), TEXT("Emitter handle id or name; omitted targets the system stack")),
            MCPParamOpt(TEXT("insert_index"), TEXT("int"), TEXT("Position within the stage; -1 appends")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_niagara_module"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddNiagaraModule(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_niagara_module_enabled", "niagara",
        "Enable or disable one stack module.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path")),
            MCPParam(TEXT("module_guid"), TEXT("string"), TEXT("Node guid of the module (or its display name)")),
            MCPParam(TEXT("enabled"), TEXT("bool"), TEXT("New enabled state")),
            MCPParamOpt(TEXT("emitter_name"), TEXT("string"), TEXT("Emitter handle id or name; omitted targets the system stack")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_niagara_module_enabled"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetNiagaraModuleEnabled(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "remove_niagara_module", "niagara",
        "Remove a module from a stage, together with the parameter-map override nodes the stack created for it.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path")),
            MCPParam(TEXT("module"), TEXT("string"), TEXT("Module: node guid, or its display name")),
            MCPParamOpt(TEXT("emitter_name"), TEXT("string"), TEXT("Emitter handle id or name; omitted targets the system stack")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("remove_niagara_module"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRemoveNiagaraModule(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_niagara_module_input", "niagara",
        "Write one input of a stack module (the same rapid-iteration value the stack UI writes) and read it back.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path")),
            MCPParam(TEXT("module"), TEXT("string"), TEXT("Module: node guid, or its display name")),
            MCPParam(TEXT("input_name"), TEXT("string"), TEXT("Input name as the stack shows it (\"SpawnRate\"), or its full \"Module.SpawnRate\" form")),
            MCPParam(TEXT("value"), TEXT("string"), TEXT("Value as text: \"25\", \"(X=0,Y=0,Z=-980)\", \"true\"")),
            MCPParamOpt(TEXT("emitter_name"), TEXT("string"), TEXT("Emitter handle id or name; omitted targets the system stack")),
            MCPParamOpt(TEXT("param_type"), TEXT("string"), TEXT("Expected type; a mismatch with the input's real type is an error")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_niagara_module_input"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetNiagaraModuleInput(P); }); }));

    // ------------------------------------------------------------------
    // Scratch-pad graphs
    // ------------------------------------------------------------------
    MCP_REGISTER_COMMAND(Registry, "create_niagara_scratch_pad_module", "niagara",
        "Create an empty scratch-pad module (custom HLSL or node graph) on one emitter stage.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path")),
            MCPParam(TEXT("emitter_name"), TEXT("string"), TEXT("Emitter handle id or name")),
            MCPParam(TEXT("stage"), TEXT("string"), TEXT("EmitterSpawn / EmitterUpdate / ParticleSpawn / ParticleUpdate, or a simulation stage of the emitter by name or index")),
            MCPParamOpt(TEXT("module_name"), TEXT("string"), TEXT("Module name; default \"ScratchModule\"")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("create_niagara_scratch_pad_module"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleCreateNiagaraScratchPadModule(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "list_niagara_scratch_pad_nodes", "niagara",
        "Dump a scratch-pad module's graph: nodes and the pins of every node.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path")),
            MCPParam(TEXT("emitter_name"), TEXT("string"), TEXT("Emitter handle id or name")),
            MCPParam(TEXT("module"), TEXT("string"), TEXT("Scratch-pad module: node guid of the stack module, or its name")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("list_niagara_scratch_pad_nodes"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleListNiagaraScratchPadNodes(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_niagara_custom_hlsl", "niagara",
        "Write custom HLSL into a scratch-pad module: creates a Custom HLSL node, or rewrites an existing one.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path")),
            MCPParam(TEXT("emitter_name"), TEXT("string"), TEXT("Emitter handle id or name")),
            MCPParam(TEXT("module"), TEXT("string"), TEXT("Scratch-pad module: node guid of the stack module, or its name")),
            MCPParam(TEXT("custom_hlsl_code"), TEXT("string"), TEXT("HLSL source; pin names are the variables the code reads and writes")),
            MCPParamOpt(TEXT("node_guid"), TEXT("string"), TEXT("Existing Custom HLSL node to rewrite; omitted adds a new node")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_niagara_custom_hlsl"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetNiagaraCustomHlsl(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_niagara_scratch_pad_pins", "niagara",
        "Declare scratch-pad pins: module-level inputs/outputs, or typed pins on one node.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path")),
            MCPParam(TEXT("emitter_name"), TEXT("string"), TEXT("Emitter handle id or name")),
            MCPParam(TEXT("module"), TEXT("string"), TEXT("Scratch-pad module: node guid of the stack module, or its name")),
            MCPParamOpt(TEXT("node_guid"), TEXT("string"), TEXT("Target node for node-level pins; omitted declares module inputs/outputs instead")),
            MCPParamOpt(TEXT("input_pins"), TEXT("array"), TEXT("Pins the module reads, e.g. [{\"name\": \"Radius\", \"type\": \"float\"}]")),
            MCPParamOpt(TEXT("output_pins"), TEXT("array"), TEXT("Pins the module writes, e.g. [{\"name\": \"Height\", \"type\": \"float\"}]")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_niagara_scratch_pad_pins"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetNiagaraScratchPadPins(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "remove_niagara_scratch_pad_node", "niagara",
        "Remove one node from a scratch-pad graph (its links go with it); the module's input/output nodes are protected.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path")),
            MCPParam(TEXT("emitter_name"), TEXT("string"), TEXT("Emitter handle id or name")),
            MCPParam(TEXT("module"), TEXT("string"), TEXT("Scratch-pad module: node guid of the stack module, or its name")),
            MCPParam(TEXT("node_guid"), TEXT("string"), TEXT("Node to remove")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("remove_niagara_scratch_pad_node"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRemoveNiagaraScratchPadNode(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "remove_niagara_scratch_pad_pin", "niagara",
        "Remove one pin from a scratch-pad node (a Custom HLSL node's signature is rebuilt, so the pin really leaves the compile).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path")),
            MCPParam(TEXT("emitter_name"), TEXT("string"), TEXT("Emitter handle id or name")),
            MCPParam(TEXT("module"), TEXT("string"), TEXT("Scratch-pad module: node guid of the stack module, or its name")),
            MCPParam(TEXT("node_guid"), TEXT("string"), TEXT("Node that owns the pin")),
            MCPParam(TEXT("pin_name"), TEXT("string"), TEXT("Pin name, as listed by list_niagara_scratch_pad_nodes")),
            MCPParamOpt(TEXT("direction"), TEXT("string"), TEXT("input / output; omit to match either")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("remove_niagara_scratch_pad_pin"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRemoveNiagaraScratchPadPin(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "connect_niagara_scratch_pad_pins", "niagara",
        "Connect two pins inside a scratch-pad graph (through the graph schema, so type checking is the engine's).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path")),
            MCPParam(TEXT("emitter_name"), TEXT("string"), TEXT("Emitter handle id or name")),
            MCPParam(TEXT("module"), TEXT("string"), TEXT("Scratch-pad module: node guid of the stack module, or its name")),
            MCPParam(TEXT("from_node"), TEXT("string"), TEXT("Source node guid")),
            MCPParam(TEXT("from_pin"), TEXT("string"), TEXT("Source pin name")),
            MCPParam(TEXT("to_node"), TEXT("string"), TEXT("Target node guid")),
            MCPParam(TEXT("to_pin"), TEXT("string"), TEXT("Target pin name")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("connect_niagara_scratch_pad_pins"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleConnectNiagaraScratchPadPins(P); }); }));

    // ------------------------------------------------------------------
    // Level actor + component parameters
    // ------------------------------------------------------------------
    MCP_REGISTER_COMMAND(Registry, "spawn_niagara_actor", "niagara",
        "Spawn an ANiagaraActor with a system into the editor world (the level is left unsaved).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("system_path"), TEXT("string"), TEXT("Niagara system asset to play")),
            MCPParam(TEXT("actor_name"), TEXT("string"), TEXT("Actor name; a taken name is rejected")),
            MCPParamOpt(TEXT("location"), TEXT("object"), TEXT("Spawn location {x,y,z}; default 0,0,0")),
            MCPParamOpt(TEXT("rotation"), TEXT("object"), TEXT("Spawn rotation {pitch,yaw,roll}; default 0,0,0")),
            MCPParamOpt(TEXT("auto_activate"), TEXT("bool"), TEXT("Activate the component on spawn; default true")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("spawn_niagara_actor"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSpawnNiagaraActor(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_niagara_component_parameter", "niagara",
        "Override one user parameter on a spawned Niagara actor's component, and read it back.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("actor_name"), TEXT("string"), TEXT("Niagara actor name in the editor world")),
            MCPParam(TEXT("param_name"), TEXT("string"), TEXT("Parameter name; \"Gravity\" and \"User.Gravity\" are the same parameter")),
            MCPParam(TEXT("value"), TEXT("string"), TEXT("Value as text: \"12.5\", \"(X=0,Y=0,Z=-980)\", \"true\"")),
            MCPParam(TEXT("param_type"), TEXT("string"), TEXT("float / int / bool / vec2 / vec3 / vec4 / color / quat")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_niagara_component_parameter"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetNiagaraComponentParameter(P); }); }));
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::RunCommand(
    const FString& CommandType, const TSharedPtr<FJsonObject>& Params,
    const TFunction<TSharedPtr<FJsonObject>(const TSharedPtr<FJsonObject>&)>& Body)
{
    const FMCPCommandEntry* Entry = FMCPCommandRegistry::Get().Find(CommandType);
    const bool bMutating = Entry && Entry->Flags.bMutatesGraph;
    const bool bPersist = GetBoolParam(Params, TEXT("persist"), true);

    TUniquePtr<FScopedTransaction> Transaction;
    if (bMutating)
    {
        Transaction = MakeUnique<FScopedTransaction>(FText::FromString(FString::Printf(TEXT("UnrealMCP %s"), *CommandType)));
    }

    TSharedPtr<FJsonObject> Result = Body(Params);

    const bool bSuccess = FUnrealMCPCommonUtils::ResponseIndicatesSuccess(Result);
    if (bMutating && !bSuccess && Transaction.IsValid())
    {
        Transaction->Cancel();
    }
    Transaction.Reset();

    // Persist immediately on success: the editor is routinely killed by the build script, so an
    // unsaved system edit would be lost. `persist=false` batches writes instead.
    bool bSaved = false;
    if (bMutating && bSuccess && bPersist && Result.IsValid())
    {
        FString AssetPath = GetStringParam(Params, TEXT("asset_path"));
        if (AssetPath.IsEmpty())
        {
            AssetPath = GetStringParam(Params, TEXT("system_path"));
        }
        if (!AssetPath.IsEmpty())
        {
            if (UObject* Asset = FUnrealMCPCommonUtils::FindAsset(AssetPath))
            {
                bSaved = FUnrealMCPCommonUtils::SaveAssetForObject(Asset);
            }
            Result->SetBoolField(TEXT("saved"), bSaved);
            Result->SetBoolField(TEXT("persist_requested"), bPersist);
        }
    }

    return Result;
}

// =====================================================================
// Assets
// =====================================================================

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleCreateNiagaraSystem(const TSharedPtr<FJsonObject>& Params)
{
    if (TSharedPtr<FJsonObject> Busy = FUnrealMCPCommonUtils::RefuseAssetLifecycleInPlayMode(TEXT("create_niagara_system")))
    {
        return Busy;
    }

    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::CreateSystem(GetStringParam(Params, TEXT("name")),
                                            GetStringParam(Params, TEXT("folder")), Result))
    {
        return MakeOpErrorJson(Result);
    }

    // Persistence reads the asset to save off the parameters, and this command has no such parameter
    // (the asset is named by name + folder). Hand the created package to the wrapper instead, so the
    // single save path stays in one place.
    Params->SetStringField(TEXT("asset_path"), Result.AssetPath);

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(Result.AssetPath);
    Response->SetStringField(TEXT("system_path"), Result.AssetPath);
    return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleDuplicateNiagaraSystem(const TSharedPtr<FJsonObject>& Params)
{
    if (TSharedPtr<FJsonObject> Busy = FUnrealMCPCommonUtils::RefuseAssetLifecycleInPlayMode(TEXT("duplicate_niagara_system")))
    {
        return Busy;
    }

    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::DuplicateSystem(GetStringParam(Params, TEXT("source_path")),
                                               GetStringParam(Params, TEXT("destination_path")), Result))
    {
        return MakeOpErrorJson(Result);
    }

    // The wrapper saves the asset named by "asset_path"; this command names the copy by
    // destination_path, so the new package is handed over here.
    Params->SetStringField(TEXT("asset_path"), Result.AssetPath);

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(Result.AssetPath);
    Response->SetStringField(TEXT("source_path"), GetStringParam(Params, TEXT("source_path")));
    return Response;
}

// =====================================================================
// Emitters
// =====================================================================

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleListNiagaraEmitters(const TSharedPtr<FJsonObject>& Params)
{
    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::ListEmitters(GetStringParam(Params, TEXT("asset_path")), Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(Result.AssetPath);
    Response->SetNumberField(TEXT("emitter_count"), Result.Emitters.Num());
    Response->SetArrayField(TEXT("emitters"), EmittersToJson(Result.Emitters));
    return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleAddNiagaraEmitter(const TSharedPtr<FJsonObject>& Params)
{
    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::AddEmitter(GetStringParam(Params, TEXT("asset_path")),
                                          GetStringParam(Params, TEXT("emitter_name")),
                                          GetStringParam(Params, TEXT("source_emitter_path")),
                                          GetBoolParam(Params, TEXT("add_default_modules"), false), Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(Result.AssetPath);
    Response->SetStringField(TEXT("emitter_id"), Result.EmitterId);
    Response->SetStringField(TEXT("emitter_name"), Result.EmitterName);
    return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleRemoveNiagaraEmitter(const TSharedPtr<FJsonObject>& Params)
{
    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::RemoveEmitter(GetStringParam(Params, TEXT("asset_path")),
                                             GetStringParam(Params, TEXT("emitter_name")), Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(Result.AssetPath);
    Response->SetStringField(TEXT("emitter_id"), Result.EmitterId);
    Response->SetStringField(TEXT("emitter_name"), Result.EmitterName);
    return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleSetNiagaraEmitterProperties(
    const TSharedPtr<FJsonObject>& Params)
{
    FUnrealMCPNiagaraEmitterEdits Edits;
    Edits.bSimTarget = Params.IsValid() && Params->HasField(TEXT("sim_target"));
    Edits.SimTarget = GetStringParam(Params, TEXT("sim_target"));
    Edits.bLocalSpace = Params.IsValid() && Params->HasField(TEXT("local_space"));
    Edits.LocalSpace = GetBoolParam(Params, TEXT("local_space"), false);
    Edits.bDeterminism = Params.IsValid() && Params->HasField(TEXT("determinism"));
    Edits.Determinism = GetBoolParam(Params, TEXT("determinism"), false);
    Edits.bRandomSeed = Params.IsValid() && Params->HasField(TEXT("random_seed"));
    Edits.RandomSeed = GetIntParam(Params, TEXT("random_seed"), 0);
    Edits.bInterpolatedSpawning = Params.IsValid() && Params->HasField(TEXT("interpolated_spawning"));
    Edits.InterpolatedSpawning = GetBoolParam(Params, TEXT("interpolated_spawning"), false);
    Edits.bPreAllocationCount = Params.IsValid() && Params->HasField(TEXT("pre_allocation_count"));
    Edits.PreAllocationCount = GetIntParam(Params, TEXT("pre_allocation_count"), 0);

    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::SetEmitterProperties(GetStringParam(Params, TEXT("asset_path")),
                                                    GetStringParam(Params, TEXT("emitter_name")), Edits, Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(Result.AssetPath);
    if (Result.Emitters.Num() > 0)
    {
        Response->SetObjectField(TEXT("emitter"), EmitterToJson(Result.Emitters[0]));
    }
    Response->SetStringField(TEXT("emitter_name"), Result.EmitterName);
    return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleAddNiagaraRenderer(
    const TSharedPtr<FJsonObject>& Params)
{
    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::AddRenderer(GetStringParam(Params, TEXT("asset_path")),
                                           GetStringParam(Params, TEXT("emitter_name")),
                                           GetStringParam(Params, TEXT("renderer_type")),
                                           GetStringParam(Params, TEXT("material_path")), Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(GetStringParam(Params, TEXT("asset_path")));
    Response->SetStringField(TEXT("emitter_name"), Result.EmitterName);
    Response->SetStringField(TEXT("renderer_class"), Result.ModuleName);
    Response->SetStringField(TEXT("renderer_index"), Result.ParamName);
    Response->SetStringField(TEXT("material_path"), Result.Value);
    FUnrealMCPCommonUtils::AddStringArrayField(Response, TEXT("renderers"), Result.Candidates);
    return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleRemoveNiagaraRenderer(
    const TSharedPtr<FJsonObject>& Params)
{
    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::RemoveRenderer(GetStringParam(Params, TEXT("asset_path")),
                                              GetStringParam(Params, TEXT("emitter_name")),
                                              GetIntParam(Params, TEXT("renderer_index"), 0), Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(GetStringParam(Params, TEXT("asset_path")));
    Response->SetStringField(TEXT("emitter_name"), Result.EmitterName);
    Response->SetStringField(TEXT("removed_renderer_class"), Result.ModuleName);
    FUnrealMCPCommonUtils::AddStringArrayField(Response, TEXT("renderers"), Result.Candidates);
    return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleSetNiagaraRendererMaterial(
    const TSharedPtr<FJsonObject>& Params)
{
    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::SetRendererMaterial(GetStringParam(Params, TEXT("asset_path")),
                                                   GetStringParam(Params, TEXT("emitter_name")),
                                                   GetStringParam(Params, TEXT("material_path")),
                                                   GetIntParam(Params, TEXT("renderer_index"), 0), Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(Result.AssetPath);
    Response->SetStringField(TEXT("emitter_name"), Result.EmitterName);
    Response->SetStringField(TEXT("material_property"), Result.ParamName);
    Response->SetStringField(TEXT("material"), Result.Value);
    if (Result.Emitters.Num() > 0)
    {
        Response->SetObjectField(TEXT("emitter"), EmitterToJson(Result.Emitters[0]));
    }
    return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleAddNiagaraSimulationStage(
    const TSharedPtr<FJsonObject>& Params)
{
    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::AddSimulationStage(GetStringParam(Params, TEXT("asset_path")),
                                                  GetStringParam(Params, TEXT("emitter_name")),
                                                  GetStringParam(Params, TEXT("stage_name")),
                                                  GetStringParam(Params, TEXT("iteration_source")),
                                                  GetBoolParam(Params, TEXT("enabled"), true), Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(Result.AssetPath);
    Response->SetStringField(TEXT("emitter_name"), Result.EmitterName);
    Response->SetStringField(TEXT("stage_name"), Result.ParamName);
    Response->SetStringField(TEXT("iteration_source"), Result.ValueType);
    Response->SetBoolField(TEXT("enabled"), Result.bEnabled);
    Response->SetStringField(TEXT("script_path"), Result.ScriptPath);
    if (Result.Emitters.Num() > 0)
    {
        Response->SetObjectField(TEXT("emitter"), EmitterToJson(Result.Emitters[0]));
    }
    return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleSetNiagaraSimulationStage(
    const TSharedPtr<FJsonObject>& Params)
{
    const bool bSetEnabled = Params.IsValid() && Params->HasField(TEXT("enabled"));
    const bool bSetNumIterations = Params.IsValid() && Params->HasField(TEXT("num_iterations"));

    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::SetSimulationStage(GetStringParam(Params, TEXT("asset_path")),
                                                  GetStringParam(Params, TEXT("emitter_name")),
                                                  GetStringParam(Params, TEXT("stage")),
                                                  GetStringParam(Params, TEXT("stage_name")),
                                                  bSetEnabled, GetBoolParam(Params, TEXT("enabled"), true),
                                                  GetStringParam(Params, TEXT("iteration_source")),
                                                  bSetNumIterations, GetIntParam(Params, TEXT("num_iterations"), 1),
                                                  GetStringParam(Params, TEXT("data_interface_parameter")),
                                                  Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(Result.AssetPath);
    Response->SetStringField(TEXT("emitter_name"), Result.EmitterName);
    Response->SetStringField(TEXT("stage_name"), Result.ParamName);
    Response->SetBoolField(TEXT("enabled"), Result.bEnabled);
    Response->SetStringField(TEXT("iteration_source"), Result.ValueType);
    Response->SetStringField(TEXT("num_iterations"), Result.Value);
    Response->SetStringField(TEXT("data_interface_parameter"), Result.ScriptPath);
    if (Result.Emitters.Num() > 0)
    {
        Response->SetObjectField(TEXT("emitter"), EmitterToJson(Result.Emitters[0]));
    }
    return Response;
}

// =====================================================================
// Compile
// =====================================================================

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleCompileNiagaraSystem(const TSharedPtr<FJsonObject>& Params)
{
    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::CompileSystem(GetStringParam(Params, TEXT("asset_path")),
                                             GetBoolParam(Params, TEXT("wait"), true), Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(Result.AssetPath);
    Response->SetStringField(TEXT("compile_status"), Result.CompileStatus);
    Response->SetNumberField(TEXT("script_count"), Result.ScriptStatuses.Num());
    Response->SetArrayField(TEXT("scripts"), ScriptStatusesToJson(Result.ScriptStatuses));
    Response->SetArrayField(TEXT("errors"), MessagesToJson(Result.CompileMessages, TEXT("error")));
    Response->SetArrayField(TEXT("warnings"), MessagesToJson(Result.CompileMessages, TEXT("warning")));
    return Response;
}

// =====================================================================
// Parameters
// =====================================================================

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleGetNiagaraParameters(const TSharedPtr<FJsonObject>& Params)
{
    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::GetParameters(GetStringParam(Params, TEXT("asset_path")), Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(Result.AssetPath);
    Response->SetNumberField(TEXT("parameter_count"), Result.Parameters.Num());
    Response->SetArrayField(TEXT("parameters"), ParametersToJson(Result.Parameters));
    return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleSetNiagaraParameter(const TSharedPtr<FJsonObject>& Params)
{
    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::SetParameter(GetStringParam(Params, TEXT("asset_path")),
                                            GetStringParam(Params, TEXT("param_name")),
                                            GetStringParam(Params, TEXT("param_type")),
                                            GetStringParam(Params, TEXT("value")),
                                            GetBoolParam(Params, TEXT("add_if_missing"), true), Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(Result.AssetPath);
    Response->SetStringField(TEXT("param_name"), Result.ParamName);
    Response->SetStringField(TEXT("param_type"), Result.ValueType);
    Response->SetStringField(TEXT("value"), Result.Value);
    return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleSetNiagaraDataInterfaceProperties(
    const TSharedPtr<FJsonObject>& Params)
{
    TArray<TPair<FString, FString>> Properties;
    const TSharedPtr<FJsonObject>* PropertyObject = nullptr;
    if (Params.IsValid() && Params->TryGetObjectField(TEXT("properties"), PropertyObject)
        && PropertyObject && (*PropertyObject).IsValid())
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PropertyObject)->Values)
        {
            FString ValueText;
            if (Pair.Value.IsValid() && Pair.Value->Type == EJson::String)
            {
                ValueText = Pair.Value->AsString();
            }
            else if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Boolean)
            {
                ValueText = Pair.Value->AsBool() ? TEXT("true") : TEXT("false");
            }
            else if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Number)
            {
                ValueText = FString::SanitizeFloat(Pair.Value->AsNumber());
            }
            else
            {
                return MakeErrorJson(TEXT("invalid_params"),
                    FString::Printf(TEXT("Property '%s' must be a number, a string or a boolean"), *Pair.Key));
            }
            Properties.Add(TPair<FString, FString>(Pair.Key, ValueText));
        }
    }

    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::SetDataInterfaceProperties(GetStringParam(Params, TEXT("asset_path")),
                                                          GetStringParam(Params, TEXT("param_name")),
                                                          GetStringParam(Params, TEXT("data_interface_type")),
                                                          Properties, Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(Result.AssetPath);
    Response->SetStringField(TEXT("param_name"), Result.ParamName);
    Response->SetStringField(TEXT("data_interface_class"), Result.ValueType);
    Response->SetStringField(TEXT("data_interface_path"), Result.ScriptPath);
    Response->SetArrayField(TEXT("properties"), ParametersToJson(Result.Parameters));
    TArray<TSharedPtr<FJsonValue>> Editable;
    for (const FString& Property : Result.Candidates)
    {
        Editable.Add(MakeShared<FJsonValueString>(Property));
    }
    Response->SetArrayField(TEXT("editable_properties"), Editable);
    return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleListNiagaraGridAttributes(const TSharedPtr<FJsonObject>& Params)
{
    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::ListGridAttributes(GetStringParam(Params, TEXT("asset_path")),
                                                  GetStringParam(Params, TEXT("param_name")), Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(Result.AssetPath);
    Response->SetStringField(TEXT("param_name"), Result.ParamName);
    Response->SetStringField(TEXT("data_interface_class"), Result.ValueType);
    Response->SetNumberField(TEXT("attribute_count"), Result.GridAttributeCount);
    Response->SetNumberField(TEXT("slot_count"), Result.GridSlotCount);
    Response->SetNumberField(TEXT("channel_count"), Result.GridChannelCount);
    Response->SetStringField(TEXT("preview_attribute"), Result.PreviewAttribute);
    Response->SetBoolField(TEXT("preview_grid_enabled"), Result.bPreviewGridEnabled);
    Response->SetArrayField(TEXT("attributes"), GridAttributesToJson(Result.GridAttributes));
    TArray<TSharedPtr<FJsonValue>> Warnings;
    for (const FString& Warning : Result.GridAttributeWarnings)
    {
        Warnings.Add(MakeShared<FJsonValueString>(Warning));
    }
    Response->SetArrayField(TEXT("warnings"), Warnings);
    return Response;
}

// =====================================================================
// Stack modules
// =====================================================================

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleListNiagaraModules(const TSharedPtr<FJsonObject>& Params)
{
    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::ListModules(GetStringParam(Params, TEXT("asset_path")),
                                           GetStringParam(Params, TEXT("emitter_name")),
                                           GetStringParam(Params, TEXT("stage")), Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(Result.AssetPath);
    Response->SetNumberField(TEXT("module_count"), Result.Modules.Num());
    Response->SetArrayField(TEXT("modules"), ModulesToJson(Result.Modules));
    return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleAddNiagaraModule(const TSharedPtr<FJsonObject>& Params)
{
    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::AddModule(GetStringParam(Params, TEXT("asset_path")),
                                         GetStringParam(Params, TEXT("emitter_name")),
                                         GetStringParam(Params, TEXT("stage")),
                                         GetStringParam(Params, TEXT("module_asset_path")),
                                         GetIntParam(Params, TEXT("insert_index"), INDEX_NONE), Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(Result.AssetPath);
    Response->SetStringField(TEXT("module_guid"), Result.ModuleGuid);
    Response->SetStringField(TEXT("module_name"), Result.ModuleName);
    Response->SetStringField(TEXT("script_path"), Result.ScriptPath);
    return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleSetNiagaraModuleEnabled(const TSharedPtr<FJsonObject>& Params)
{
    FUnrealMCPNiagaraResult Result;
    const FString EmitterName = GetStringParam(Params, TEXT("emitter_name"));
    if (!FUnrealMCPNiagaraOps::SetModuleEnabled(GetStringParam(Params, TEXT("asset_path")), EmitterName,
                                                GetStringParam(Params, TEXT("module_guid")),
                                                GetBoolParam(Params, TEXT("enabled"), true),
                                                EmitterName.IsEmpty(), Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(Result.AssetPath);
    Response->SetStringField(TEXT("module_guid"), Result.ModuleGuid);
    Response->SetStringField(TEXT("module_name"), Result.ModuleName);
    Response->SetBoolField(TEXT("enabled"), Result.bEnabled);
    return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleSetNiagaraModuleInput(
    const TSharedPtr<FJsonObject>& Params)
{
    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::SetModuleInput(GetStringParam(Params, TEXT("asset_path")),
                                              GetStringParam(Params, TEXT("emitter_name")),
                                              GetStringParam(Params, TEXT("module")),
                                              GetStringParam(Params, TEXT("input_name")),
                                              GetStringParam(Params, TEXT("value")),
                                              GetStringParam(Params, TEXT("param_type")), Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(Result.AssetPath);
    Response->SetStringField(TEXT("module_guid"), Result.ModuleGuid);
    Response->SetStringField(TEXT("module_name"), Result.ModuleName);
    Response->SetStringField(TEXT("input_name"), Result.ParamName);
    Response->SetStringField(TEXT("input_type"), Result.ValueType);
    Response->SetStringField(TEXT("value"), Result.Value);
    Response->SetStringField(TEXT("rapid_iteration_parameter"), Result.RapidIterationParameter);
    if (!Result.EmitterName.IsEmpty())
    {
        Response->SetStringField(TEXT("emitter_instance"), Result.EmitterName);
    }
    return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleRemoveNiagaraModule(const TSharedPtr<FJsonObject>& Params)
{
    const FString EmitterName = GetStringParam(Params, TEXT("emitter_name"));
    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::RemoveModule(GetStringParam(Params, TEXT("asset_path")), EmitterName,
                                            GetStringParam(Params, TEXT("module")), EmitterName.IsEmpty(), Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(Result.AssetPath);
    Response->SetStringField(TEXT("removed_module_guid"), Result.ModuleGuid);
    Response->SetStringField(TEXT("removed_module_name"), Result.ModuleName);
    TArray<TSharedPtr<FJsonValue>> Removed;
    for (const FString& Node : Result.Candidates)
    {
        Removed.Add(MakeShared<FJsonValueString>(Node));
    }
    Response->SetArrayField(TEXT("removed_nodes"), Removed);
    Response->SetArrayField(TEXT("modules"), ModulesToJson(Result.Modules));
    Response->SetArrayField(TEXT("errors"), MessagesToJson(Result.CompileMessages, TEXT("error")));
    return Response;
}

// =====================================================================
// Scratch-pad graphs
// =====================================================================

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleCreateNiagaraScratchPadModule(
    const TSharedPtr<FJsonObject>& Params)
{
    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::CreateScratchPadModule(GetStringParam(Params, TEXT("asset_path")),
                                                      GetStringParam(Params, TEXT("emitter_name")),
                                                      GetStringParam(Params, TEXT("stage")),
                                                      GetStringParam(Params, TEXT("module_name")), Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(Result.AssetPath);
    Response->SetStringField(TEXT("module_guid"), Result.ModuleGuid);
    Response->SetStringField(TEXT("module_name"), Result.ModuleName);
    Response->SetStringField(TEXT("script_path"), Result.ScriptPath);
    return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleListNiagaraScratchPadNodes(
    const TSharedPtr<FJsonObject>& Params)
{
    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::ListScratchPadNodes(GetStringParam(Params, TEXT("asset_path")),
                                                   GetStringParam(Params, TEXT("emitter_name")),
                                                   GetStringParam(Params, TEXT("module")), Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(Result.AssetPath);
    Response->SetStringField(TEXT("module_guid"), Result.ModuleGuid);
    Response->SetStringField(TEXT("module_name"), Result.ModuleName);
    Response->SetNumberField(TEXT("node_count"), Result.Nodes.Num());
    Response->SetArrayField(TEXT("nodes"), NodesToJson(Result.Nodes));
    Response->SetArrayField(TEXT("pins"), PinsToJson(Result.Pins));
    return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleSetNiagaraCustomHlsl(const TSharedPtr<FJsonObject>& Params)
{
    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::SetCustomHlsl(GetStringParam(Params, TEXT("asset_path")),
                                             GetStringParam(Params, TEXT("emitter_name")),
                                             GetStringParam(Params, TEXT("module")),
                                             GetStringParam(Params, TEXT("node_guid")),
                                             GetStringParam(Params, TEXT("custom_hlsl_code")), Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(Result.AssetPath);
    Response->SetStringField(TEXT("module_guid"), Result.ModuleGuid);
    Response->SetStringField(TEXT("module_name"), Result.ModuleName);
    Response->SetStringField(TEXT("node_guid"), Result.NodeGuid);
    Response->SetStringField(TEXT("custom_hlsl_code"), Result.HlslCode);
    return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleSetNiagaraScratchPadPins(
    const TSharedPtr<FJsonObject>& Params)
{
    TArray<FUnrealMCPNiagaraPinSpec> Pins;
    FString ReadError;
    if (!ReadPinSpecs(Params, TEXT("input_pins"), /*bOutput=*/false, Pins, ReadError)
        || !ReadPinSpecs(Params, TEXT("output_pins"), /*bOutput=*/true, Pins, ReadError))
    {
        return MakeErrorJson(TEXT("invalid_params"), ReadError);
    }
    if (Pins.Num() == 0)
    {
        return MakeErrorJson(TEXT("invalid_params"),
                             TEXT("Pass at least one pin in 'input_pins' or 'output_pins'"));
    }

    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::SetScratchPadPins(GetStringParam(Params, TEXT("asset_path")),
                                                 GetStringParam(Params, TEXT("emitter_name")),
                                                 GetStringParam(Params, TEXT("module")),
                                                 GetStringParam(Params, TEXT("node_guid")), Pins, Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(Result.AssetPath);
    Response->SetStringField(TEXT("module_guid"), Result.ModuleGuid);
    Response->SetStringField(TEXT("module_name"), Result.ModuleName);
    Response->SetStringField(TEXT("node_guid"), Result.NodeGuid);
    Response->SetNumberField(TEXT("pin_count"), Result.Pins.Num());
    Response->SetArrayField(TEXT("pins"), PinsToJson(Result.Pins));
    return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleRemoveNiagaraScratchPadNode(
    const TSharedPtr<FJsonObject>& Params)
{
    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::RemoveScratchPadNode(GetStringParam(Params, TEXT("asset_path")),
                                                    GetStringParam(Params, TEXT("emitter_name")),
                                                    GetStringParam(Params, TEXT("module")),
                                                    GetStringParam(Params, TEXT("node_guid")), Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(Result.AssetPath);
    Response->SetStringField(TEXT("removed_node_guid"), Result.NodeGuid);
    Response->SetStringField(TEXT("removed_node_type"), Result.Value);
    Response->SetStringField(TEXT("module_guid"), Result.ModuleGuid);
    Response->SetNumberField(TEXT("node_count"), Result.RemainingNodes);
    Response->SetArrayField(TEXT("nodes"), NodesToJson(Result.Nodes));
    Response->SetArrayField(TEXT("pins"), PinsToJson(Result.Pins));
    return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleRemoveNiagaraScratchPadPin(
    const TSharedPtr<FJsonObject>& Params)
{
    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::RemoveScratchPadPin(GetStringParam(Params, TEXT("asset_path")),
                                                   GetStringParam(Params, TEXT("emitter_name")),
                                                   GetStringParam(Params, TEXT("module")),
                                                   GetStringParam(Params, TEXT("node_guid")),
                                                   GetStringParam(Params, TEXT("pin_name")),
                                                   GetStringParam(Params, TEXT("direction")), Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(Result.AssetPath);
    Response->SetStringField(TEXT("node_guid"), Result.NodeGuid);
    Response->SetStringField(TEXT("removed_pin_name"), Result.Value);
    Response->SetStringField(TEXT("module_guid"), Result.ModuleGuid);
    Response->SetNumberField(TEXT("node_count"), Result.RemainingNodes);
    Response->SetArrayField(TEXT("pins"), PinsToJson(Result.Pins));
    return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleConnectNiagaraScratchPadPins(
    const TSharedPtr<FJsonObject>& Params)
{
    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::ConnectScratchPadPins(GetStringParam(Params, TEXT("asset_path")),
                                                     GetStringParam(Params, TEXT("emitter_name")),
                                                     GetStringParam(Params, TEXT("module")),
                                                     GetStringParam(Params, TEXT("from_node")),
                                                     GetStringParam(Params, TEXT("from_pin")),
                                                     GetStringParam(Params, TEXT("to_node")),
                                                     GetStringParam(Params, TEXT("to_pin")), Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = MakeSuccessJson(Result.AssetPath);
    Response->SetBoolField(TEXT("connected"), Result.bConnected);
    TArray<TSharedPtr<FJsonValue>> Converted;
    for (const FString& NodeGuid : Result.InsertedConversionNodes)
    {
        Converted.Add(MakeShared<FJsonValueString>(NodeGuid));
    }
    Response->SetArrayField(TEXT("inserted_conversion_nodes"), Converted);
    if (Result.InsertedConversionNodes.Num() > 0)
    {
        Response->SetStringField(TEXT("conversion_warning"),
            TEXT("The schema inserted a conversion node for this connection. If it converts to or from a "
                 "Position (LWC) value, compile_niagara_system will fail with 'Component ... of variable "
                 "selction_result has no valid offset'; drop the pin (remove_niagara_scratch_pad_pin) and "
                 "write a plain float/vec3 attribute instead"));
    }
    return Response;
}

// =====================================================================
// Level actor + component parameters
// =====================================================================

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleSpawnNiagaraActor(const TSharedPtr<FJsonObject>& Params)
{
    FVector Location = FVector::ZeroVector;
    FRotator Rotation = FRotator::ZeroRotator;
    if (Params.IsValid() && Params->HasField(TEXT("location")))
    {
        Location = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location"));
    }
    if (Params.IsValid() && Params->HasField(TEXT("rotation")))
    {
        Rotation = FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation"));
    }

    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::SpawnNiagaraActor(GetStringParam(Params, TEXT("system_path")),
                                                 GetStringParam(Params, TEXT("actor_name")), Location, Rotation,
                                                 GetBoolParam(Params, TEXT("auto_activate"), true), Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = FUnrealMCPCommonUtils::CreateSuccessResponse();
    Response->SetStringField(TEXT("actor_name"), Result.ActorName);
    Response->SetStringField(TEXT("actor_path"), Result.ActorPath);
    Response->SetStringField(TEXT("system_path"), Result.AssetPath);
    Response->SetStringField(TEXT("component_path"), Result.Value);
    return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleSetNiagaraComponentParameter(
    const TSharedPtr<FJsonObject>& Params)
{
    FUnrealMCPNiagaraResult Result;
    if (!FUnrealMCPNiagaraOps::SetComponentParameter(GetStringParam(Params, TEXT("actor_name")),
                                                     GetStringParam(Params, TEXT("param_name")),
                                                     GetStringParam(Params, TEXT("param_type")),
                                                     GetStringParam(Params, TEXT("value")), Result))
    {
        return MakeOpErrorJson(Result);
    }

    TSharedPtr<FJsonObject> Response = FUnrealMCPCommonUtils::CreateSuccessResponse();
    Response->SetStringField(TEXT("actor_name"), Result.ActorName);
    Response->SetStringField(TEXT("param_name"), Result.ParamName);
    Response->SetStringField(TEXT("param_type"), Result.ValueType);
    Response->SetStringField(TEXT("value"), Result.Value);
    return Response;
}
