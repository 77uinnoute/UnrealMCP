#include "Commands/Blueprint/UnrealMCPBlueprintNodeCommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Commands/Blueprint/UnrealMCPBlueprintGraphOps.h"
#include "Commands/Blueprint/UnrealMCPBlueprintCommandHelpers.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Timeline.h"
#include "K2Node_VariableGet.h"
#include "K2Node_Variable.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_StateMachine.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "BlueprintEditor.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Commands/Material/UnrealMCPMaterialOps.h"

// Declare the log category
DEFINE_LOG_CATEGORY_STATIC(LogUnrealMCP, Log, All);

// The response shapers every blueprint command file shares (error envelope with candidates,
// compile read-back, local variable names).
using namespace UnrealMCPBlueprintHelpers;

namespace
{
    // -----------------------------------------------------------------------
    // JSON shaping: the command layer is the only place that knows about JSON,
    // the graph logic lives in FUnrealMCPBlueprintGraphOps.
    // -----------------------------------------------------------------------

    TSharedPtr<FJsonObject> MakeFunctionReferenceJson(const FMemberReference& Reference)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("member_name"), Reference.GetMemberName().ToString());
        if (UClass* Parent = Reference.GetMemberParentClass())
        {
            Result->SetStringField(TEXT("member_parent"), Parent->GetPathName());
        }
        else
        {
            Result->SetStringField(TEXT("member_parent"), TEXT(""));
        }
        const FGuid Guid = Reference.GetMemberGuid();
        if (Guid.IsValid())
        {
            Result->SetStringField(TEXT("member_guid"), Guid.ToString());
        }
        return Result;
    }

    /** Stable, non-localized node identity. All read and create responses use this function. */
    void AppendNodeSemantics(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& NodeObj)
    {
        if (!Node || !NodeObj.IsValid())
        {
            return;
        }

        if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
        {
            NodeObj->SetObjectField(TEXT("function_reference"), MakeFunctionReferenceJson(Call->FunctionReference));
            // Read the reference's own verdict instead of inferring it from the pins: the engine always
            // creates a PSC_Self pin for functions defined in this blueprint (BlueprintNodeStatics.cpp:48-52),
            // so "there is a self pin" / "member_parent is empty" say nothing about the reference.
            NodeObj->SetBoolField(TEXT("self_context"), Call->FunctionReference.IsSelfContext());
        }
        else if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
        {
            NodeObj->SetObjectField(TEXT("function_reference"), MakeFunctionReferenceJson(Event->EventReference));
        }
        else if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
        {
            // Local variable nodes are not self context either - they are scoped to the function graph
            // and carry no Target pin at all.
            NodeObj->SetBoolField(TEXT("self_context"), Variable->VariableReference.IsSelfContext());
        }

        if (const UK2Node_DynamicCast* DynamicCast = Cast<UK2Node_DynamicCast>(Node))
        {
            NodeObj->SetStringField(TEXT("target_type"),
                DynamicCast->TargetType ? DynamicCast->TargetType->GetPathName() : FString());
        }

        // UE 5.5 AnimGraph nodes expose their runtime struct through public UPROPERTY "Node".
        // Read a strict field whitelist through reflection: no private layout assumptions or ExportText.
        if (Node->GetClass()->GetName().StartsWith(TEXT("AnimGraphNode_")))
        {
            const FStructProperty* InnerProperty = FindFProperty<FStructProperty>(Node->GetClass(), TEXT("Node"));
            if (InnerProperty)
            {
                const void* Inner = InnerProperty->ContainerPtrToValuePtr<void>(Node);
                TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
                const TPair<const TCHAR*, const TCHAR*> Fields[] = {
                    { TEXT("Sequence"), TEXT("sequence") },
                    { TEXT("bLoopAnimation"), TEXT("loop_animation") },
                    { TEXT("PlayRate"), TEXT("play_rate") },
                    { TEXT("BlendTime"), TEXT("blend_time") },
                    { TEXT("ActiveValue"), TEXT("active_value") }
                };
                for (const TPair<const TCHAR*, const TCHAR*>& Field : Fields)
                {
                    if (FProperty* Property = FindFProperty<FProperty>(InnerProperty->Struct, Field.Key))
                    {
                        Summary->SetField(Field.Value, FUnrealMCPCommonUtils::PropertyValueToJson(
                            Property, Property->ContainerPtrToValuePtr<void>(Inner)));
                    }
                }
                if (Summary->Values.Num() > 0)
                {
                    NodeObj->SetObjectField(TEXT("inner_node"), Summary);
                }
            }
        }
    }

    /** A self pin the engine binds on its own, so an unlinked pin with no default is still valid:
        hidden self pins (static calls, meta HideSelfPin) and calls/variables on the node's own
        class, where the reference is self-context and "self" is implied. Reporting these as a
        missing required input would flag every member call inside its own blueprint. */
    bool IsImplicitlyBoundSelfPin(const UEdGraphNode* Node, const UEdGraphPin* Pin)
    {
        if (!Node || !Pin || Pin->PinName != UEdGraphSchema_K2::PN_Self)
        {
            return false;
        }
        if (Pin->bHidden)
        {
            return true;
        }
        if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
        {
            return Call->FunctionReference.IsSelfContext();
        }
        if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
        {
            return Variable->VariableReference.IsSelfContext();
        }
        return false;
    }

    TSharedPtr<FJsonObject> PinInfoToJson(const FUnrealMCPPinInfo& Pin)
    {
        TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
        PinObj->SetStringField(TEXT("pin_name"), Pin.PinName);
        PinObj->SetStringField(TEXT("direction"), Pin.Direction);
        PinObj->SetStringField(TEXT("category"), Pin.Category);
        PinObj->SetStringField(TEXT("default_value"), Pin.DefaultValue);
        if (!Pin.DefaultObject.IsEmpty())
        {
            PinObj->SetStringField(TEXT("default_object"), Pin.DefaultObject);
        }
        // A text pin's literal lives in DefaultTextValue (that is what the compiler materialises);
        // bHidden/bNotConnectable say whether an unconnected pin is merely not drawn or genuinely
        // filled in by the engine. Without these, "hidden but unbound" and "FText literal written or
        // not" are unreadable from the MCP side.
        if (!Pin.DefaultTextValue.IsEmpty())
        {
            PinObj->SetStringField(TEXT("default_text_value"), Pin.DefaultTextValue);
        }
        PinObj->SetBoolField(TEXT("b_hidden"), Pin.bHidden);
        PinObj->SetBoolField(TEXT("b_not_connectable"), Pin.bNotConnectable);
        PinObj->SetBoolField(TEXT("connected"), Pin.Connected);

        TArray<TSharedPtr<FJsonValue>> LinkedToArray;
        for (const FString& Link : Pin.LinkedTo)
        {
            LinkedToArray.Add(MakeShared<FJsonValueString>(Link));
        }
        PinObj->SetArrayField(TEXT("linked_to"), LinkedToArray);
        return PinObj;
    }

    TSharedPtr<FJsonObject> NodeInfoToJson(const FUnrealMCPNodeInfo& Node, bool bVerbose = true)
    {
        TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
        NodeObj->SetStringField(TEXT("node_id"), Node.NodeId);
        NodeObj->SetStringField(TEXT("name"), Node.Name);
        NodeObj->SetStringField(TEXT("type"), Node.Type);
        NodeObj->SetStringField(TEXT("graph_name"), Node.GraphName);
        NodeObj->SetNumberField(TEXT("pos_x"), Node.PosX);
        NodeObj->SetNumberField(TEXT("pos_y"), Node.PosY);
        AppendNodeSemantics(Node.Node, NodeObj);

        if (!bVerbose)
        {
            NodeObj->SetNumberField(TEXT("pin_count"), Node.Pins.Num());
            return NodeObj;
        }

        // Editable UPROPERTYs of this node class, typed (numbers stay numbers, structs become arrays)
        if (Node.Node)
        {
            TSharedPtr<FJsonObject> PropertiesObject =
                FUnrealMCPBlueprintGraphOps::MakeNodePropertiesJson(Node.Node);
            if (PropertiesObject.IsValid())
            {
                NodeObj->SetObjectField(TEXT("properties"), PropertiesObject);
            }
        }

        TArray<TSharedPtr<FJsonValue>> PinsArray;
        TArray<TSharedPtr<FJsonValue>> ConnectionsArray;
        for (const FUnrealMCPPinInfo& Pin : Node.Pins)
        {
            PinsArray.Add(MakeShared<FJsonValueObject>(PinInfoToJson(Pin)));

            if (Pin.Direction != TEXT("output"))
            {
                continue;
            }
            for (const FString& Link : Pin.LinkedTo)
            {
                TSharedPtr<FJsonObject> ConnObj = MakeShared<FJsonObject>();
                ConnObj->SetStringField(TEXT("from"), FString::Printf(TEXT("%s.%s"), *Node.NodeId, *Pin.PinName));
                ConnObj->SetStringField(TEXT("to"), Link);
                ConnectionsArray.Add(MakeShared<FJsonValueObject>(ConnObj));
            }
        }
        NodeObj->SetArrayField(TEXT("pins"), PinsArray);
        NodeObj->SetArrayField(TEXT("connections"), ConnectionsArray);
        return NodeObj;
    }

    TSharedPtr<FJsonObject> MakeFunctionConsistencyJson(UBlueprint* Blueprint, UEdGraph* Graph)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        UK2Node_FunctionEntry* Entry = nullptr;
        UK2Node_FunctionResult* ResultNode = nullptr;
        if (Graph)
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                Entry = Entry ? Entry : Cast<UK2Node_FunctionEntry>(Node);
                ResultNode = ResultNode ? ResultNode : Cast<UK2Node_FunctionResult>(Node);
            }
        }

        TSet<FName> EntryParams;
        TSet<FName> ResultParams;
        auto GatherDataPins = [](const UEdGraphNode* Node, TSet<FName>& Names)
        {
            if (!Node) return;
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
                {
                    Names.Add(Pin->PinName);
                }
            }
        };
        GatherDataPins(Entry, EntryParams);
        GatherDataPins(ResultNode, ResultParams);

        TSet<FName> SignatureInputs;
        TSet<FName> SignatureOutputs;
        UFunction* Function = Blueprint && Blueprint->GeneratedClass && Graph
            ? Blueprint->GeneratedClass->FindFunctionByName(Graph->GetFName()) : nullptr;
        if (Function)
        {
            for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
            {
                const bool bOutput = It->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm);
                (bOutput ? SignatureOutputs : SignatureInputs).Add(It->GetFName());
            }
        }

        TArray<FString> Reasons;
        if (!Entry)
        {
            Reasons.Add(TEXT("missing_entry"));
        }
        if (ResultNode && SignatureOutputs.Num() == 0)
        {
            Reasons.Add(TEXT("unexpected_result"));
        }
        if (!ResultNode && SignatureOutputs.Num() > 0)
        {
            Reasons.Add(TEXT("missing_result"));
        }
        auto SetsEqual = [](const TSet<FName>& A, const TSet<FName>& B)
        {
            if (A.Num() != B.Num()) return false;
            for (const FName Name : A) if (!B.Contains(Name)) return false;
            return true;
        };
        if (Function && (!SetsEqual(EntryParams, SignatureInputs) || !SetsEqual(ResultParams, SignatureOutputs)))
        {
            Reasons.Add(TEXT("signature_param_mismatch"));
        }

        Result->SetBoolField(TEXT("entry_present"), Entry != nullptr);
        Result->SetBoolField(TEXT("result_node_present"), ResultNode != nullptr);
        Result->SetBoolField(TEXT("inconsistent"), Reasons.Num() > 0);
        Result->SetArrayField(TEXT("inconsistency_reasons"), MakeStringArray(Reasons));
        return Result;
    }

    TSharedPtr<FJsonObject> GraphInfoToJson(const FUnrealMCPGraphInfo& Graph)
    {
        TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
        GraphObj->SetStringField(TEXT("graph_name"), Graph.GraphName);
        GraphObj->SetStringField(TEXT("graph_class"), Graph.GraphClass);
        GraphObj->SetNumberField(TEXT("node_count"), Graph.NodeCount);
        GraphObj->SetBoolField(TEXT("is_editable"), Graph.IsEditable);
        return GraphObj;
    }

    TArray<TSharedPtr<FJsonValue>> BuildReadback(const TArray<UEdGraphPin*>& Pins)
    {
        TArray<TSharedPtr<FJsonValue>> ReadbackArray;
        for (const UEdGraphPin* Pin : Pins)
        {
            if (!Pin)
            {
                continue;
            }
            FUnrealMCPPinInfo Info;
            FUnrealMCPBlueprintGraphOps::MakePinInfo(Pin, Info);
            ReadbackArray.Add(MakeShared<FJsonValueObject>(PinInfoToJson(Info)));
        }
        return ReadbackArray;
    }

    /** readback of a freshly created node: everything it exposes. */
    TArray<TSharedPtr<FJsonValue>> BuildReadbackOfNode(UEdGraphNode* Node)
    {
        TArray<UEdGraphPin*> Pins;
        if (Node)
        {
            for (UEdGraphPin* Pin : Node->Pins)
            {
                Pins.Add(Pin);
            }
        }
        return BuildReadback(Pins);
    }

    /** Every write command answers in the same shape: node_id + graph_name + node_count + readback. */
    TSharedPtr<FJsonObject> MakeWriteResult(UEdGraph* Graph, UEdGraphNode* Node,
                                            const TArray<TSharedPtr<FJsonValue>>& Readback)
    {
        // Graph writes persist immediately, the same contract the reflection library keeps (GuardPersist
        // in UnrealMCPBlueprintGraphLibrary): the plugin's Python layer documents "success means written
        // to disk", and without this the editor closing/being killed by the build script silently drops
        // the edit - observed on BP_TPSDemoGameMode, whose Array_Length pin types (set by
        // connect_blueprint_nodes) were gone after the next editor start.
        //
        // Gated on the dispatch's persist decision: a batch that passes persist=false must not be saved
        // here, or "one flush per batch" is a lie for every node command.
        if (Graph && FUnrealMCPCommonUtils::IsPersistEnabled())
        {
            FUnrealMCPBlueprintGraphOps::PersistGraphOwner(Graph);
        }

        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        // Explicit success, the same field every other write command (set_object_property, the asset and
        // component writers) returns. Without it the only way to tell a write from a no-op was the
        // absence of an "error" field, which reads as a convention rather than an answer.
        ResultObj->SetBoolField(TEXT("success"), true);
        if (Node)
        {
            ResultObj->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
            FUnrealMCPNodeInfo NodeInfo;
            FUnrealMCPBlueprintGraphOps::MakeNodeInfo(Node, NodeInfo);
            ResultObj->SetObjectField(TEXT("node"), NodeInfoToJson(NodeInfo));
        }
        if (Graph)
        {
            ResultObj->SetStringField(TEXT("graph_name"), Graph->GetName());
            ResultObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
        }
        if (Readback.Num() > 0)
        {
            ResultObj->SetArrayField(TEXT("readback"), Readback);
        }
        return ResultObj;
    }

    // -----------------------------------------------------------------------
    // Parameter helpers
    // -----------------------------------------------------------------------

    UBlueprint* ResolveBlueprintForCommand(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutError)
    {
        FString BlueprintName;
        if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
        {
            OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"),
                TEXT("Missing 'blueprint_name' parameter"));
            return nullptr;
        }

        UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
        if (!Blueprint)
        {
            OutError = FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::BlueprintNotFound,
                FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
            return nullptr;
        }
        return Blueprint;
    }

    bool ResolveCommandGraph(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params, bool bCreateIfMissing,
                             UEdGraph*& OutGraph, TSharedPtr<FJsonObject>& OutError)
    {
        FString GraphName;
        Params->TryGetStringField(TEXT("graph_name"), GraphName);

        FString ErrorCode;
        FString ErrorMessage;
        TArray<FString> Candidates;
        if (!FUnrealMCPBlueprintGraphOps::ResolveGraph(Blueprint, GraphName, bCreateIfMissing, OutGraph,
                                                      ErrorCode, ErrorMessage, Candidates))
        {
            TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(ErrorCode, ErrorMessage);

            TArray<TSharedPtr<FJsonValue>> CandidateArray;
            for (const FString& Candidate : Candidates)
            {
                CandidateArray.Add(MakeShared<FJsonValueString>(Candidate));
            }
            Error->SetArrayField(TEXT("available_graphs"), CandidateArray);

            OutError = Error;
            return false;
        }
        return true;
    }

    FVector2D ReadNodePosition(const TSharedPtr<FJsonObject>& Params)
    {
        if (Params->HasField(TEXT("node_position")))
        {
            return FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
        }
        return FVector2D(0.0f, 0.0f);
    }

}

FUnrealMCPBlueprintNodeCommands::FUnrealMCPBlueprintNodeCommands()
{
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::RunCommand(const FString& CommandType,
    const TSharedPtr<FJsonObject>& Params,
    const TFunction<TSharedPtr<FJsonObject>(const TSharedPtr<FJsonObject>&)>& Body)
{
    const FMCPCommandEntry* Entry = FMCPCommandRegistry::Get().Find(CommandType);
    const bool bMutating = Entry && Entry->Flags.bMutatesGraph;
    // The registry flag is the DEFAULT; an explicit `persist` parameter overrides it so a batch of
    // graph writes can be flushed once by the caller instead of writing the package per command.
    bool bPersist = Entry && Entry->Flags.bPersistAfterSuccess;
    Params->TryGetBoolField(TEXT("persist"), bPersist);

    // One MCP command = one undo step, so a wrong graph edit can be taken back with Ctrl+Z.
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

    bool bSaved = false;
    if (bMutating && bSuccess && bPersist)
    {
        bSaved = FUnrealMCPCommonUtils::SaveBlueprintFromParams(Params);
    }

    if (Result.IsValid())
    {
        // A handler that saved on its own already reported `saved`; do not overwrite it.
        if (!Result->HasField(TEXT("saved")))
        {
            Result->SetBoolField(TEXT("saved"), bSaved);
        }
        if (!Result->HasField(TEXT("persist_requested")))
        {
            Result->SetBoolField(TEXT("persist_requested"), bPersist);
        }
    }
    return Result;
}

void FUnrealMCPBlueprintNodeCommands::RegisterCommands(FMCPCommandRegistry& Registry)
{
    MCP_REGISTER_COMMAND(Registry, "connect_blueprint_nodes", "blueprint_node",
        "Connect an output pin to an input pin in a Blueprint graph. A pin that can hold only one wire "
        "(every exec pin) loses its previous link when a second one is connected - the engine does that "
        "silently, so the response lists what was dropped in displaced_links[] (with a hint: branch with a "
        "Sequence node instead of re-wiring the same output). `verified` means the new wire is in AND "
        "nothing was displaced; a false there with success=true is a completed connection that cost an "
        "existing one.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("source_node_id"), TEXT("string"), TEXT("Node guid of the source node")),
            MCPParam(TEXT("source_pin"), TEXT("string"), TEXT("Output pin name on the source node")),
            MCPParam(TEXT("target_node_id"), TEXT("string"), TEXT("Node guid of the target node")),
            MCPParam(TEXT("target_pin"), TEXT("string"), TEXT("Input pin name on the target node")),
            MCPParamOpt(TEXT("graph_name"), TEXT("string"), TEXT("Target graph (defaults to the event graph)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("connect_blueprint_nodes"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleConnectBlueprintNodes(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "add_blueprint_get_self_component_reference", "blueprint_node", "Add a Self Get node for one of the Blueprint's components.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("component_name"), TEXT("string"), TEXT("Component variable to reference")),
            MCPParamOpt(TEXT("graph_name"), TEXT("string"), TEXT("Target graph (defaults to the event graph)")),
            MCPParamOpt(TEXT("node_position"), TEXT("array"), TEXT("[X, Y] node position in the graph")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_blueprint_get_self_component_reference"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddBlueprintGetSelfComponentReference(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "add_blueprint_event_node", "blueprint_node", "Add an event node to a Blueprint graph.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("event_name"), TEXT("string"), TEXT("Event name (e.g. ReceiveBeginPlay)")),
            MCPParamOpt(TEXT("graph_name"), TEXT("string"), TEXT("Target graph (defaults to the event graph)")),
            MCPParamOpt(TEXT("node_position"), TEXT("array"), TEXT("[X, Y] node position in the graph")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_blueprint_event_node"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddBlueprintEvent(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "add_blueprint_function_node", "blueprint_node",
        "Add a function call node, optionally applying input pin defaults. The defaults go through the same "
        "setter set_blueprint_pin_default uses, so a LIST handed to a single-valued pin is refused and that "
        "entry's `hint` under failed[] carries the MakeArray recipe.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("function_name"), TEXT("string"), TEXT("Function to call")),
            MCPParamOpt(TEXT("target"), TEXT("string"), TEXT("Class the function is called on (defaults to self)")),
            MCPParamOpt(TEXT("params"), TEXT("object"), TEXT("Pin name to default value map applied after creating the node")),
            MCPParamOpt(TEXT("graph_name"), TEXT("string"), TEXT("Target graph (defaults to the event graph)")),
            MCPParamOpt(TEXT("node_position"), TEXT("array"), TEXT("[X, Y] node position in the graph")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_blueprint_function_node"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddBlueprintFunctionCall(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "add_blueprint_variable", "blueprint_node", "Add a member variable to a Blueprint.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("variable_name"), TEXT("string"), TEXT("Name of the new variable")),
            MCPParam(TEXT("variable_type"), TEXT("string"), TEXT("Pin category of the variable (e.g. bool, int, object)")),
            MCPParamOpt(TEXT("sub_class"), TEXT("string"), TEXT("Class / struct / enum path for object and struct types")),
            MCPParamOpt(TEXT("default_value"), TEXT("string"), TEXT("Default value as text")),
            MCPParamOpt(TEXT("is_exposed"), TEXT("bool"), TEXT("Expose the variable on the Blueprint's instance details")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_blueprint_variable"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddBlueprintVariable(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "add_blueprint_variable_node", "blueprint_node", "Add a variable Get or Set node, creating a local variable when needed.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("variable_name"), TEXT("string"), TEXT("Variable to read or write")),
            MCPParamOpt(TEXT("node_kind"), TEXT("string"), TEXT("'get' (default) or 'set'")),
            MCPParamOpt(TEXT("is_local"), TEXT("bool"), TEXT("Use a local variable of the target function graph")),
            MCPParamOpt(TEXT("owner_class"), TEXT("string"), TEXT("Build a non-self Get/Set of a property on this class (e.g. PlayerController); the node gets a Target pin you must wire. Not combinable with is_local")),
            MCPParamOpt(TEXT("variable_type"), TEXT("string"), TEXT("Pin category for a local variable that does not exist yet")),
            MCPParamOpt(TEXT("sub_class"), TEXT("string"), TEXT("Class / struct / enum path for a new local variable")),
            MCPParamOpt(TEXT("default_value"), TEXT("string"), TEXT("Default value for a new local variable")),
            MCPParamOpt(TEXT("graph_name"), TEXT("string"), TEXT("Target graph (defaults to the event graph)")),
            MCPParamOpt(TEXT("node_position"), TEXT("array"), TEXT("[X, Y] node position in the graph")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_blueprint_variable_node"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddBlueprintVariableNode(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "add_blueprint_literal_node", "blueprint_node", "Add a literal node of the given type and value.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("literal_type"), TEXT("string"), TEXT("Literal type (e.g. int, float, string, bool, vector)")),
            MCPParam(TEXT("value"), TEXT("string"), TEXT("Literal value as text")),
            MCPParamOpt(TEXT("graph_name"), TEXT("string"), TEXT("Target graph (defaults to the event graph)")),
            MCPParamOpt(TEXT("node_position"), TEXT("array"), TEXT("[X, Y] node position in the graph")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_blueprint_literal_node"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddBlueprintLiteralNode(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "add_blueprint_node_by_class", "blueprint_node", "Add a bare K2 node of the given class.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("node_class"), TEXT("string"), TEXT("K2 node class name to instantiate")),
            MCPParamOpt(TEXT("graph_name"), TEXT("string"), TEXT("Target graph (defaults to the event graph)")),
            MCPParamOpt(TEXT("node_position"), TEXT("array"), TEXT("[X, Y] node position in the graph")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_blueprint_node_by_class"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddBlueprintNodeByClass(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "add_blueprint_input_action_node", "blueprint_node", "Add an input action event node to a Blueprint graph.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("action_name"), TEXT("string"), TEXT("Input action name")),
            MCPParamOpt(TEXT("graph_name"), TEXT("string"), TEXT("Target graph (defaults to the event graph)")),
            MCPParamOpt(TEXT("node_position"), TEXT("array"), TEXT("[X, Y] node position in the graph")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_blueprint_input_action_node"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddBlueprintInputActionNode(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "add_blueprint_self_reference", "blueprint_node", "Add a Self reference node to a Blueprint graph.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParamOpt(TEXT("graph_name"), TEXT("string"), TEXT("Target graph (defaults to the event graph)")),
            MCPParamOpt(TEXT("node_position"), TEXT("array"), TEXT("[X, Y] node position in the graph")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_blueprint_self_reference"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddBlueprintSelfReference(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "find_blueprint_nodes", "blueprint_node",
        "List nodes and their pins from a Blueprint, Material or MaterialFunction graph. `truncated` is true "
        "only when a node that should have been listed was dropped by max_nodes (raise it and re-ask); it is "
        "NOT set by node_type filtering - read `filtered` for that, since re-asking a filtered query gains "
        "nothing. verbose=false drops "
        "properties, pins and connections - the response declares that in omitted_fields[] and keeps pin_count "
        "per node - so such a payload cannot answer any pin-name question: do not filter it by pin name (that "
        "silently matches nothing). Call again with verbose=true (the default) for pin-level work.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Asset name, or __current__ for the focused editor asset")),
            MCPParamOpt(TEXT("node_type"), TEXT("string"), TEXT("Filter: All (default) or Event")),
            MCPParamOpt(TEXT("graph_name"), TEXT("string"), TEXT("Graph to read (defaults to the event graph)")),
            MCPParamOpt(TEXT("event_name"), TEXT("string"), TEXT("Event to match (required when node_type is Event)")),
            MCPParamOpt(TEXT("event_type"), TEXT("string"), TEXT("Alias of event_name")),
            MCPParamOpt(TEXT("max_nodes"), TEXT("int"), TEXT("Maximum nodes returned, 1-1000 (default 200)")),
            MCPParamOpt(TEXT("verbose"), TEXT("bool"), TEXT("Include properties, pins and connections (default true). false omits them and reports that in omitted_fields[]; the result is not usable for pin-name lookups")),
        }), MCPFlags(false, false, false, false) /* read-only */,
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("find_blueprint_nodes"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleFindBlueprintNodes(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "verify_blueprint_graph", "blueprint_node", "Run read-only structural checks on a Blueprint graph.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParamOpt(TEXT("graph_name"), TEXT("string"), TEXT("Graph to verify (defaults to event graph)")),
            MCPParamOpt(TEXT("rules"), TEXT("array"), TEXT("Optional rule-name whitelist")),
            MCPParamOpt(TEXT("max_issues"), TEXT("int"), TEXT("Maximum issues returned, 1-1000 (default 200)")),
        }), MCPFlags(false, false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("verify_blueprint_graph"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleVerifyBlueprintGraph(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "list_blueprint_graphs", "blueprint_node", "List the graphs of a Blueprint.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
        }), MCPFlags(false, false, false, false) /* read-only */,
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("list_blueprint_graphs"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleListBlueprintGraphs(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "add_blueprint_function_graph", "blueprint_node", "Create a function graph (a user function, or an override of a parent's BlueprintImplementableEvent via signature_class).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("function_name"), TEXT("string"), TEXT("Function / event name (becomes the graph name)")),
            MCPParamOpt(TEXT("signature_class"), TEXT("string"), TEXT("Class declaring the event to implement (omit for a user function)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_blueprint_function_graph"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddBlueprintFunctionGraph(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "remove_blueprint_function_graph", "blueprint_node", "Remove a function graph (engine-owned graphs are refused).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("function_name"), TEXT("string"), TEXT("Name of the function graph to remove")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("remove_blueprint_function_graph"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRemoveBlueprintFunctionGraph(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "list_blueprint_function_graphs", "blueprint_node", "List a Blueprint's function graphs with their signature source and parameters.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
        }), MCPFlags(false, false, false, false) /* read-only */,
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("list_blueprint_function_graphs"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleListBlueprintFunctionGraphs(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "rename_blueprint_function_graph", "blueprint_node", "Rename a function graph (renames the function it defines).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("old_name"), TEXT("string"), TEXT("Current function / graph name")),
            MCPParam(TEXT("new_name"), TEXT("string"), TEXT("New function / graph name")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("rename_blueprint_function_graph"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRenameBlueprintFunctionGraph(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "add_blueprint_function_param", "blueprint_node", "Add an input parameter or an output (return value) to a function graph.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("function_name"), TEXT("string"), TEXT("Function graph to edit")),
            MCPParam(TEXT("param_name"), TEXT("string"), TEXT("Parameter name")),
            MCPParam(TEXT("param_type"), TEXT("string"), TEXT("Pin category, e.g. bool / int / float / object / struct (containers: \"int[]\", \"struct{}\", \"int{,}\")")),
            MCPParamOpt(TEXT("sub_class"), TEXT("string"), TEXT("Class / struct / enum path for object and struct types")),
            MCPParamOpt(TEXT("is_output"), TEXT("bool"), TEXT("true adds a return value (output) instead of an input parameter")),
            MCPParamOpt(TEXT("default_value"), TEXT("string"), TEXT("Optional default value for an input parameter")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_blueprint_function_param"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddBlueprintFunctionParam(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "remove_blueprint_function_param", "blueprint_node", "Remove an input parameter or an output from a function graph.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("function_name"), TEXT("string"), TEXT("Function graph to edit")),
            MCPParam(TEXT("param_name"), TEXT("string"), TEXT("Parameter / output name to remove")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("remove_blueprint_function_param"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRemoveBlueprintFunctionParam(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "rename_blueprint_function_param", "blueprint_node", "Rename an input parameter or an output of a function graph.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("function_name"), TEXT("string"), TEXT("Function graph to edit")),
            MCPParam(TEXT("old_name"), TEXT("string"), TEXT("Current parameter name")),
            MCPParam(TEXT("new_name"), TEXT("string"), TEXT("New parameter name")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("rename_blueprint_function_param"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRenameBlueprintFunctionParam(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_blueprint_pin_default", "blueprint_node",
        "Write one input pin's default value on an existing node. A pin holds ONE value, so a LIST is refused: "
        "to feed several values, put a MakeArray node beside it, connect its element pins and write those - the "
        "refusal carries that recipe in `hint`. (A scalar written to even an Array[Name] pin is accepted and "
        "lands in the pin's DefaultValue.)",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("node_id"), TEXT("string"), TEXT("Node guid of the node to change")),
            MCPParam(TEXT("pin_name"), TEXT("string"), TEXT("Input pin name")),
            MCPParam(TEXT("value"), TEXT("string"), TEXT("Default value (number / bool / string / enum member / array)")),
            MCPParamOpt(TEXT("graph_name"), TEXT("string"), TEXT("Target graph (defaults to the event graph)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_blueprint_pin_default"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetBlueprintPinDefault(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_blueprint_node_property", "blueprint_node", "Write one of a graph node's own properties. A dotted property_name walks into an in-memory struct/object property held by the node (e.g. \"node.chain_end\" for an AnimGraph node's FAnimNode); the response echoes the resolved property_path, and a bad segment returns available_path_segments.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("node_id"), TEXT("string"), TEXT("Node guid of the node to change")),
            MCPParam(TEXT("property_name"), TEXT("string"), TEXT("Property name on the node class")),
            MCPParam(TEXT("property_value"), TEXT("string"), TEXT("Value in the shape reflect_probe reports for the property")),
            MCPParamOpt(TEXT("graph_name"), TEXT("string"), TEXT("Target graph (defaults to the event graph)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_blueprint_node_property"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetBlueprintNodeProperty(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "delete_blueprint_nodes", "blueprint_node", "Delete nodes by guid from a Blueprint graph.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("node_ids"), TEXT("array"), TEXT("Node guids to delete")),
            MCPParamOpt(TEXT("graph_name"), TEXT("string"), TEXT("Target graph (defaults to the event graph)")),
            MCPParamOpt(TEXT("force"), TEXT("bool"), TEXT("Allow deletion of protected structural nodes (default false)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("delete_blueprint_nodes"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleDeleteBlueprintNodes(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "disconnect_blueprint_pins", "blueprint_node", "Break a pin's links, optionally only towards one linked node or pin.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("node_id"), TEXT("string"), TEXT("Node guid holding the pin")),
            MCPParam(TEXT("pin_name"), TEXT("string"), TEXT("Pin name to disconnect")),
            MCPParamOpt(TEXT("linked_node_id"), TEXT("string"), TEXT("Only break the link to this node")),
            MCPParamOpt(TEXT("linked_pin_name"), TEXT("string"), TEXT("Only break the link to this pin")),
            MCPParamOpt(TEXT("graph_name"), TEXT("string"), TEXT("Target graph (defaults to the event graph)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("disconnect_blueprint_pins"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleDisconnectBlueprintPins(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "move_blueprint_node", "blueprint_node", "Move a node to a new position and read the position back.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("node_id"), TEXT("string"), TEXT("Node guid of the node to move")),
            MCPParam(TEXT("position"), TEXT("array"), TEXT("[X, Y] new node position in the graph")),
            MCPParamOpt(TEXT("graph_name"), TEXT("string"), TEXT("Target graph (defaults to the event graph)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("move_blueprint_node"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleMoveBlueprintNode(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "refresh_blueprint_node", "blueprint_node", "Rebuild a node's pins and read the refreshed pin list back.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("node_id"), TEXT("string"), TEXT("Node guid of the node to refresh")),
            MCPParamOpt(TEXT("graph_name"), TEXT("string"), TEXT("Target graph (defaults to the event graph)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("refresh_blueprint_node"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRefreshBlueprintNode(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "split_blueprint_pin", "blueprint_node", "Split a struct pin into its sub-pins (UEdGraphSchema_K2::SplitPin).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("node_id"), TEXT("string"), TEXT("Node guid holding the pin")),
            MCPParam(TEXT("pin_name"), TEXT("string"), TEXT("Struct pin to split")),
            MCPParamOpt(TEXT("graph_name"), TEXT("string"), TEXT("Target graph (defaults to the event graph)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("split_blueprint_pin"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSplitBlueprintPin(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "recombine_blueprint_pin", "blueprint_node", "Recombine a split struct pin back into one pin (UEdGraphSchema_K2::RecombinePin).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("node_id"), TEXT("string"), TEXT("Node guid holding the pin")),
            MCPParam(TEXT("pin_name"), TEXT("string"), TEXT("Split pin to recombine")),
            MCPParamOpt(TEXT("graph_name"), TEXT("string"), TEXT("Target graph (defaults to the event graph)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("recombine_blueprint_pin"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRecombineBlueprintPin(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "add_blueprint_custom_event_node", "blueprint_node", "Add a Custom Event node, optionally with parameters.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("event_name"), TEXT("string"), TEXT("Name of the custom event")),
            MCPParamOpt(TEXT("params"), TEXT("array"), TEXT("Parameters to add: [{name, type, sub_class?}]")),
            MCPParamOpt(TEXT("graph_name"), TEXT("string"), TEXT("Target graph (defaults to the event graph)")),
            MCPParamOpt(TEXT("node_position"), TEXT("array"), TEXT("[X, Y] node position in the graph")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_blueprint_custom_event_node"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddBlueprintCustomEventNode(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "rename_blueprint_custom_event", "blueprint_node", "Rename a Custom Event node and read the new name and node id back.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("node_id"), TEXT("string"), TEXT("Node guid of the custom event")),
            MCPParam(TEXT("new_name"), TEXT("string"), TEXT("New event name")),
            MCPParamOpt(TEXT("graph_name"), TEXT("string"), TEXT("Target graph (defaults to the event graph)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("rename_blueprint_custom_event"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRenameBlueprintCustomEvent(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "open_blueprint_graph", "blueprint_node", "Open a Blueprint's graph in the blueprint editor.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParamOpt(TEXT("graph_name"), TEXT("string"), TEXT("Graph to open (defaults to the event graph)")),
        }), MCPFlags(false, false, false, false) /* editor session only */,
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("open_blueprint_graph"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleOpenBlueprintGraph(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "focus_blueprint_node", "blueprint_node", "Bring an open Blueprint's graph editor to a node (jump to / focus it).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("node_id"), TEXT("string"), TEXT("Node guid to focus")),
            MCPParamOpt(TEXT("graph_name"), TEXT("string"), TEXT("Graph holding the node (defaults to the event graph)")),
        }), MCPFlags(false, false, false, false) /* editor session only */,
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("focus_blueprint_node"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleFocusBlueprintNode(P); }); }));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleConnectBlueprintNodes(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString SourceNodeId;
    FString TargetNodeId;
    FString SourcePinName;
    FString TargetPinName;
    if (!Params->TryGetStringField(TEXT("source_node_id"), SourceNodeId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'source_node_id' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("source_pin"), SourcePinName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'source_pin' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("target_node_id"), TargetNodeId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'target_node_id' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("target_pin"), TargetPinName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'target_pin' parameter"));
    }

    UEdGraph* Graph = nullptr;
    if (!ResolveCommandGraph(Blueprint, Params, /*bCreateIfMissing=*/true, Graph, Error))
    {
        return Error;
    }

    UEdGraphNode* SourceNode = FUnrealMCPBlueprintGraphOps::FindNodeByGuid(Graph, SourceNodeId);
    UEdGraphNode* TargetNode = FUnrealMCPBlueprintGraphOps::FindNodeByGuid(Graph, TargetNodeId);
    if (!SourceNode || !TargetNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::NodeNotFound,
            FString::Printf(TEXT("Source or target node not found. source=%s target=%s"),
                SourceNode ? TEXT("found") : TEXT("missing"),
                TargetNode ? TEXT("found") : TEXT("missing")));
    }

    // Capture the target's old source before the schema may break/replace it.
    UEdGraphPin* TargetPinBefore = FUnrealMCPBlueprintGraphOps::FindPin(TargetNode, TargetPinName, EGPD_Input);
    TArray<TSharedPtr<FJsonValue>> PreviousSources;
    if (TargetPinBefore)
    {
        for (const UEdGraphPin* Linked : TargetPinBefore->LinkedTo)
        {
            if (Linked && Linked->GetOwningNode())
            {
                PreviousSources.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s.%s"),
                    *Linked->GetOwningNode()->NodeGuid.ToString(), *Linked->PinName.ToString())));
            }
        }
    }

    FString ConnectErrorCode;
    FString ConnectErrorMessage;
    TArray<FString> Candidates;
    TArray<FUnrealMCPBlueprintGraphOps::FDisplacedLink> DisplacedLinks;
    if (!FUnrealMCPBlueprintGraphOps::ConnectNodes(Graph, SourceNode, SourcePinName, TargetNode, TargetPinName,
                                                  ConnectErrorCode, ConnectErrorMessage, Candidates, &DisplacedLinks))
    {
        return MakeCandidatesError(ConnectErrorCode, ConnectErrorMessage, Candidates);
    }

    // Read the link back so the caller can assert what actually landed in the graph.
    const UEdGraphPin* ResolvedSourcePin = FUnrealMCPBlueprintGraphOps::FindPin(SourceNode, SourcePinName, EGPD_Output);
    const UEdGraphPin* ResolvedTargetPin = FUnrealMCPBlueprintGraphOps::FindPin(TargetNode, TargetPinName, EGPD_Input);

    TArray<UEdGraphPin*> TouchedPins;
    TouchedPins.Add(const_cast<UEdGraphPin*>(ResolvedSourcePin));
    TouchedPins.Add(const_cast<UEdGraphPin*>(ResolvedTargetPin));

    TSharedPtr<FJsonObject> ResultObj = MakeWriteResult(Graph, nullptr, BuildReadback(TouchedPins));
    ResultObj->SetArrayField(TEXT("tgt_pin_prev_source"), PreviousSources);

    // displaced_links is the authoritative reading: tgt_pin_prev_source only ever looked at the target,
    // while the schema drops the SOURCE side when a single-connection pin is re-wired.
    TArray<TSharedPtr<FJsonValue>> DisplacedJson;
    for (const FUnrealMCPBlueprintGraphOps::FDisplacedLink& Link : DisplacedLinks)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("node_id"), Link.NodeId);
        Entry->SetStringField(TEXT("node"), Link.NodeName);
        Entry->SetStringField(TEXT("pin"), Link.PinName);
        Entry->SetStringField(TEXT("direction"), Link.Direction);
        Entry->SetStringField(TEXT("lost_node_id"), Link.LostNodeId);
        Entry->SetStringField(TEXT("lost_pin"), Link.LostPinName);
        DisplacedJson.Add(MakeShared<FJsonValueObject>(Entry));
    }
    ResultObj->SetArrayField(TEXT("displaced_links"), DisplacedJson);
    if (DisplacedJson.Num() > 0)
    {
        ResultObj->SetStringField(TEXT("hint"),
            TEXT("the engine dropped these connections to make room: a pin that holds only one wire "
                 "(every exec pin) loses its previous link when a second one is connected - branch with a "
                 "Sequence node instead of re-wiring the same output"));
    }
    ResultObj->SetStringField(TEXT("source_node_id"), SourceNodeId);
    ResultObj->SetStringField(TEXT("target_node_id"), TargetNodeId);
    if (ResolvedSourcePin)
    {
        ResultObj->SetStringField(TEXT("resolved_source_pin"), ResolvedSourcePin->PinName.ToString());
        TArray<TSharedPtr<FJsonValue>> LinkedToArray;
        for (const UEdGraphPin* LinkedPin : ResolvedSourcePin->LinkedTo)
        {
            if (LinkedPin && LinkedPin->GetOwningNode())
            {
                LinkedToArray.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s.%s"),
                    *LinkedPin->GetOwningNode()->NodeGuid.ToString(), *LinkedPin->PinName.ToString())));
            }
        }
        ResultObj->SetArrayField(TEXT("source_linked_to"), LinkedToArray);
        ResultObj->SetArrayField(TEXT("src_pin_consumers"), LinkedToArray);
    }
    if (ResolvedTargetPin)
    {
        ResultObj->SetStringField(TEXT("resolved_target_pin"), ResolvedTargetPin->PinName.ToString());
        TArray<TSharedPtr<FJsonValue>> LinkedFromArray;
        for (const UEdGraphPin* LinkedPin : ResolvedTargetPin->LinkedTo)
        {
            if (LinkedPin && LinkedPin->GetOwningNode())
            {
                LinkedFromArray.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s.%s"),
                    *LinkedPin->GetOwningNode()->NodeGuid.ToString(), *LinkedPin->PinName.ToString())));
            }
        }
        ResultObj->SetArrayField(TEXT("target_linked_from"), LinkedFromArray);
    }

    bool bForward = false;
    bool bReverse = false;
    if (ResolvedSourcePin && ResolvedTargetPin)
    {
        bForward = ResolvedSourcePin->LinkedTo.Contains(const_cast<UEdGraphPin*>(ResolvedTargetPin));
        bReverse = ResolvedTargetPin->LinkedTo.Contains(const_cast<UEdGraphPin*>(ResolvedSourcePin));
    }
    // verified now means "the new wire is in AND nothing else moved" - the platformer round showed a
    // bare "true" here while a previously wired exec target had been silently orphaned.
    ResultObj->SetBoolField(TEXT("verified"), bForward && bReverse && DisplacedLinks.Num() == 0);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintGetSelfComponentReference(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString ComponentName;
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'component_name' parameter"));
    }

    UEdGraph* Graph = nullptr;
    if (!ResolveCommandGraph(Blueprint, Params, /*bCreateIfMissing=*/true, Graph, Error))
    {
        return Error;
    }

    UEdGraphNode* Node = nullptr;
    TArray<FString> Candidates;
    FString ErrorCode;
    FString ErrorMessage;
    if (!FUnrealMCPBlueprintGraphOps::CreateVariableNode(Graph, ComponentName, /*bSet=*/false, ReadNodePosition(Params),
                                                        Node, ErrorCode, ErrorMessage, Candidates))
    {
        return MakeCandidatesError(ErrorCode, ErrorMessage, Candidates);
    }

    return MakeWriteResult(Graph, Node, BuildReadbackOfNode(Node));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintEvent(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString EventName;
    if (!Params->TryGetStringField(TEXT("event_name"), EventName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'event_name' parameter"));
    }

    UEdGraph* Graph = nullptr;
    if (!ResolveCommandGraph(Blueprint, Params, /*bCreateIfMissing=*/true, Graph, Error))
    {
        return Error;
    }

    UK2Node_Event* EventNode = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!FUnrealMCPBlueprintGraphOps::CreateEventNode(Graph, EventName, ReadNodePosition(Params),
                                                     EventNode, ErrorCode, ErrorMessage))
    {
        return MakeCandidatesError(ErrorCode, ErrorMessage,
            ErrorCode == EUnrealMCPGraphError::FunctionNotFound
                ? FUnrealMCPBlueprintGraphOps::ListImplementableEventNames(Blueprint) : TArray<FString>());
    }

    return MakeWriteResult(Graph, EventNode, BuildReadbackOfNode(EventNode));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintFunctionCall(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString FunctionName;
    if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'function_name' parameter"));
    }

    FString Target;
    Params->TryGetStringField(TEXT("target"), Target);

    UEdGraph* Graph = nullptr;
    if (!ResolveCommandGraph(Blueprint, Params, /*bCreateIfMissing=*/true, Graph, Error))
    {
        return Error;
    }

    UK2Node_CallFunction* FunctionNode = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    TArray<FString> Candidates;
    if (!FUnrealMCPBlueprintGraphOps::CreateFunctionCallNode(Graph, Target, FunctionName, ReadNodePosition(Params),
                                                            FunctionNode, ErrorCode, ErrorMessage, Candidates))
    {
        return MakeCandidatesError(ErrorCode, ErrorMessage, Candidates);
    }

    // Apply pin defaults through the shared setter (same dispatch as set_blueprint_pin_default)
    TArray<TSharedPtr<FJsonValue>> AppliedArray;
    TArray<TSharedPtr<FJsonValue>> FailedArray;
    TArray<UEdGraphPin*> TouchedPins;

    if (Params->HasField(TEXT("params")))
    {
        const TSharedPtr<FJsonObject>* ParamsObject = nullptr;
        if (Params->TryGetObjectField(TEXT("params"), ParamsObject) && ParamsObject && (*ParamsObject).IsValid())
        {
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Param : (*ParamsObject)->Values)
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("pin"), Param.Key);

                FString PinErrorCode;
                FString PinErrorMessage;
                TArray<FString> PinCandidates;
                if (FUnrealMCPBlueprintGraphOps::SetPinDefaultValue(FunctionNode, Param.Key, Param.Value,
                                                                   PinErrorCode, PinErrorMessage, PinCandidates))
                {
                    UEdGraphPin* AppliedPin = FUnrealMCPBlueprintGraphOps::FindPin(FunctionNode, Param.Key, EGPD_Input);
                    // Object/class pins keep the reference in DefaultObject and leave DefaultValue
                    // empty, so report the resolved path.
                    FString AppliedValue;
                    if (AppliedPin)
                    {
                        AppliedValue = AppliedPin->DefaultValue;
                        if (AppliedValue.IsEmpty() && AppliedPin->DefaultObject)
                        {
                            AppliedValue = AppliedPin->DefaultObject->GetPathName();
                        }
                    }
                    Entry->SetStringField(TEXT("value"), AppliedValue);
                    AppliedArray.Add(MakeShared<FJsonValueObject>(Entry));
                    if (AppliedPin)
                    {
                        TouchedPins.Add(AppliedPin);
                    }
                }
                else
                {
                    Entry->SetStringField(TEXT("error_code"), PinErrorCode);
                    Entry->SetStringField(TEXT("error"), PinErrorMessage);

                    // A list handed to a single-valued pin; hand over the recipe instead of stopping at the
                    // type mismatch (same helper the standalone pin write uses).
                    FString MultiValueHint;
                    if (FUnrealMCPBlueprintGraphOps::TryBuildMultiValuePinHint(Param.Value, MultiValueHint))
                    {
                        Entry->SetStringField(TEXT("hint"), MultiValueHint);
                    }

                    TArray<TSharedPtr<FJsonValue>> CandidateArray;
                    for (const FString& Candidate : PinCandidates)
                    {
                        CandidateArray.Add(MakeShared<FJsonValueString>(Candidate));
                    }
                    if (CandidateArray.Num() > 0)
                    {
                        Entry->SetArrayField(TEXT("candidates"), CandidateArray);
                    }
                    FailedArray.Add(MakeShared<FJsonValueObject>(Entry));
                }
            }
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeWriteResult(Graph, FunctionNode,
        TouchedPins.Num() > 0 ? BuildReadback(TouchedPins) : BuildReadbackOfNode(FunctionNode));
    ResultObj->SetArrayField(TEXT("applied"), AppliedArray);
    ResultObj->SetArrayField(TEXT("failed"), FailedArray);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintVariable(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString VariableName;
    if (!Params->TryGetStringField(TEXT("variable_name"), VariableName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'variable_name' parameter"));
    }

    // AddMemberVariable answers an empty name with a bare false, so reject it before the engine sees
    // it rather than letting that refusal masquerade as a successful add.
    if (VariableName.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("'variable_name' must not be empty"));
    }

    FString VariableType;
    if (!Params->TryGetStringField(TEXT("variable_type"), VariableType))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'variable_type' parameter"));
    }

    FString SubClass;
    Params->TryGetStringField(TEXT("sub_class"), SubClass);
    FString DefaultValue;
    Params->TryGetStringField(TEXT("default_value"), DefaultValue);

    bool bIsExposed = false;
    if (Params->HasField(TEXT("is_exposed")))
    {
        bIsExposed = Params->GetBoolField(TEXT("is_exposed"));
    }

    FEdGraphPinType PinType;
    FString TypeErrorMessage;
    TArray<FString> SupportedTypes;
    if (!FUnrealMCPBlueprintGraphOps::BuildVariablePinType(VariableType, SubClass, PinType,
                                                          TypeErrorMessage, SupportedTypes))
    {
        TSharedPtr<FJsonObject> TypeError = FUnrealMCPCommonUtils::CreateErrorResponse(
            EUnrealMCPGraphError::UnsupportedVariableType, TypeErrorMessage);

        TArray<TSharedPtr<FJsonValue>> SupportedArray;
        for (const FString& Supported : SupportedTypes)
        {
            SupportedArray.Add(MakeShared<FJsonValueString>(Supported));
        }
        TypeError->SetArrayField(TEXT("supported_types"), SupportedArray);
        return TypeError;
    }

    // AddMemberVariable reports "name already taken" and "name empty" as the same bare false, which
    // left the caller with a success response for a variable that was never added. Split the cases
    // out here, using the very list AddMemberVariable itself consults (GetClassVariableList covers
    // this blueprint's own variables *and* every inherited property) so the two cannot disagree.
    // A name that is this blueprint's own stays idempotent: the variable exists, so the call only
    // re-writes its default value / exposure.
    const FName VarName(*VariableName);
    int32 VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName);
    if (VariableIndex == INDEX_NONE)
    {
        TSet<FName> ClassVariables;
        FBlueprintEditorUtils::GetClassVariableList(Blueprint, ClassVariables);
        if (ClassVariables.Contains(VarName))
        {
            TArray<FString> Candidates;
            for (const FName& ClassVariable : ClassVariables)
            {
                Candidates.Add(ClassVariable.ToString());
            }
            Candidates.Sort();
            return MakeCandidatesError(EUnrealMCPGraphError::VariableNameInUse,
                FString::Printf(TEXT("'%s' is already a variable of blueprint '%s' or of one of its parent classes"),
                    *VariableName, *Blueprint->GetName()),
                Candidates);
        }

        if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, VarName, PinType, DefaultValue))
        {
            return MakeCandidatesError(EUnrealMCPGraphError::InvalidValue,
                FString::Printf(TEXT("Could not add variable '%s' to blueprint '%s'"),
                    *VariableName, *Blueprint->GetName()),
                TArray<FString>());
        }

        VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName);
    }

    if (VariableIndex == INDEX_NONE)
    {
        return MakeCandidatesError(EUnrealMCPGraphError::InvalidValue,
            FString::Printf(TEXT("Variable '%s' is not on blueprint '%s' after adding it"),
                *VariableName, *Blueprint->GetName()),
            TArray<FString>());
    }

    // is_exposed is the editor's "instance editable" eye (CPF_DisableEditOnInstance) - the same bit
    // set_blueprint_variable_flags(instance_editable=...) writes and flags.instance_editable reports.
    // Only touch it when the caller actually asked: an absent parameter must not flip the flag.
    // (The old code OR'ed CPF_Edit only when true, so false was a silent no-op that still read back true.)
    if (Params->HasField(TEXT("is_exposed")))
    {
        FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, VarName, /*bBlueprintOnlyEditable=*/!bIsExposed);
    }

    // AddMemberVariable only applies the default when it creates the variable, so an existing
    // variable has to be updated explicitly to keep the command idempotent. Every response field is
    // read back from the stored description afterwards.
    FBPVariableDescription& Variable = Blueprint->NewVariables[VariableIndex];
    if (!DefaultValue.IsEmpty() && Variable.DefaultValue != DefaultValue)
    {
        Variable.DefaultValue = DefaultValue;
    }

    FString AppliedContainer;
    switch (Variable.VarType.ContainerType)
    {
    case EPinContainerType::Array: AppliedContainer = TEXT("array"); break;
    case EPinContainerType::Set:   AppliedContainer = TEXT("set"); break;
    case EPinContainerType::Map:   AppliedContainer = TEXT("map"); break;
    default: break;
    }
    const FString AppliedCategory = Variable.VarType.PinCategory.ToString();
    const bool bExposed = (Variable.PropertyFlags & CPF_DisableEditOnInstance) == 0;
    const bool bClassEditable = (Variable.PropertyFlags & CPF_Edit) != 0;

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("variable_name"), VariableName);
    ResultObj->SetStringField(TEXT("variable_type"), VariableType);
    ResultObj->SetStringField(TEXT("pin_category"), AppliedCategory);
    if (!AppliedContainer.IsEmpty())
    {
        ResultObj->SetStringField(TEXT("container_type"), AppliedContainer);
    }
    ResultObj->SetBoolField(TEXT("is_exposed"), bExposed);
    ResultObj->SetBoolField(TEXT("class_editable"), bClassEditable);

    if (DefaultValue.IsEmpty())
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        ResultObj->SetStringField(TEXT("default_value"), Variable.DefaultValue);
        ResultObj->SetStringField(TEXT("default_value_source"), TEXT("description"));
        return ResultObj;
    }

    // A requested default only counts once it is on the class default object: compile (the
    // compiler moves DefaultValue into the CDO) and compare against the request parsed through
    // the property's own import path.
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    AppendCompileResult(Blueprint, ResultObj);

    FString CdoText;
    FString RequestedText;
    const bool bHaveCdo = ReadCdoDefaultText(Blueprint, VarName, CdoText);
    const bool bRequestParses = NormalizeDefaultText(FindGeneratedMemberProperty(Blueprint, VarName), DefaultValue, RequestedText);
    if (!bHaveCdo || !bRequestParses || CdoText != RequestedText)
    {
        TSharedPtr<FJsonObject> Failure = FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::DefaultValueNotApplied,
            FString::Printf(TEXT("Variable '%s' was created but its default did not reach the class default object%s"),
                *VariableName,
                !bRequestParses ? TEXT(" (the value does not parse for this type)") : TEXT("")));
        Failure->SetStringField(TEXT("requested"), DefaultValue);
        Failure->SetStringField(TEXT("cdo_value"), CdoText);
        Failure->SetBoolField(TEXT("variable_created"), true);
        Failure->SetStringField(TEXT("hint"), TEXT("Fix the value and retry with set_blueprint_variable_default_value"));
        if (ResultObj->HasField(TEXT("errors")))
        {
            Failure->SetField(TEXT("compile_errors"), ResultObj->TryGetField(TEXT("errors")));
        }
        return Failure;
    }

    ResultObj->SetStringField(TEXT("default_value"), CdoText);
    ResultObj->SetStringField(TEXT("default_value_source"), TEXT("cdo"));
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintVariableNode(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString VariableName;
    if (!Params->TryGetStringField(TEXT("variable_name"), VariableName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'variable_name' parameter"));
    }

    FString NodeKind = TEXT("get");
    Params->TryGetStringField(TEXT("node_kind"), NodeKind);
    const bool bSet = NodeKind.Equals(TEXT("set"), ESearchCase::IgnoreCase);

    bool bIsLocal = false;
    if (Params->HasField(TEXT("is_local")))
    {
        bIsLocal = Params->GetBoolField(TEXT("is_local"));
    }

    FString OwnerClass;
    Params->TryGetStringField(TEXT("owner_class"), OwnerClass);
    OwnerClass.TrimStartAndEndInline();
    if (!OwnerClass.IsEmpty() && bIsLocal)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("'owner_class' addresses another class's property and cannot be combined with 'is_local'"));
    }

    UEdGraph* Graph = nullptr;
    if (!ResolveCommandGraph(Blueprint, Params, /*bCreateIfMissing=*/!bIsLocal, Graph, Error))
    {
        return Error;
    }

    UEdGraphNode* Node = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    TArray<FString> Candidates;
    const FVector2D NodePosition = ReadNodePosition(Params);

    if (bIsLocal)
    {
        // A local variable lives in the scope of a function graph (on its entry node) and never
        // becomes an inherited property of the generated class, so it is addressed through the
        // graph-scoped lookup rather than through a property search.
        if (!Graph->GetSchema() || Graph->GetSchema()->GetGraphType(Graph) != GT_Function)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
                FString::Printf(TEXT("'%s' is not a function graph; local variables only exist in function graphs"),
                    *Graph->GetName()));
        }

        UEdGraph* FunctionGraph = FBlueprintEditorUtils::GetTopLevelGraph(Graph);
        const FName VarName(*VariableName);
        UK2Node_FunctionEntry* Entry = nullptr;
        FBPVariableDescription* Description =
            FBlueprintEditorUtils::FindLocalVariable(Blueprint, FunctionGraph, VarName, &Entry);
        if (!Entry)
        {
            // The engine overload only fills OutFunctionEntry on a hit; resolve it here so a miss can
            // still list the variables the graph does have.
            Entry = FUnrealMCPBlueprintGraphOps::FindFunctionEntryNode(FunctionGraph);
        }

        if (!Description)
        {
            FString VariableType;
            FString SubClass;
            Params->TryGetStringField(TEXT("variable_type"), VariableType);
            Params->TryGetStringField(TEXT("sub_class"), SubClass);

            if (VariableType.IsEmpty())
            {
                return MakeCandidatesError(EUnrealMCPGraphError::VariableNotFound,
                    FString::Printf(TEXT("Local variable '%s' does not exist in '%s'; pass 'variable_type' to create it"),
                        *VariableName, *FunctionGraph->GetName()),
                    MakeLocalVariableNames(Entry));
            }

            FEdGraphPinType PinType;
            FString TypeErrorMessage;
            TArray<FString> SupportedTypes;
            if (!FUnrealMCPBlueprintGraphOps::BuildVariablePinType(VariableType, SubClass, PinType,
                                                                  TypeErrorMessage, SupportedTypes))
            {
                TSharedPtr<FJsonObject> TypeError = FUnrealMCPCommonUtils::CreateErrorResponse(
                    EUnrealMCPGraphError::UnsupportedVariableType, TypeErrorMessage);

                TArray<TSharedPtr<FJsonValue>> SupportedArray;
                for (const FString& Supported : SupportedTypes)
                {
                    SupportedArray.Add(MakeShared<FJsonValueString>(Supported));
                }
                TypeError->SetArrayField(TEXT("supported_types"), SupportedArray);
                return TypeError;
            }

            FString DefaultValue;
            Params->TryGetStringField(TEXT("default_value"), DefaultValue);

            // AddLocalVariable does not de-duplicate, which is why the lookup above runs first: the
            // same name must never be added to the entry node twice.
            if (!FBlueprintEditorUtils::AddLocalVariable(Blueprint, FunctionGraph, VarName, PinType, DefaultValue))
            {
                return MakeCandidatesError(EUnrealMCPGraphError::InvalidValue,
                    FString::Printf(TEXT("Could not add local variable '%s' to '%s'"),
                        *VariableName, *FunctionGraph->GetName()),
                    MakeLocalVariableNames(Entry));
            }

            // The reference needs the VarGuid the engine just minted, so read the description back.
            Description = FBlueprintEditorUtils::FindLocalVariable(Blueprint, FunctionGraph, VarName, &Entry);
            if (!Description)
            {
                return MakeCandidatesError(EUnrealMCPGraphError::InvalidValue,
                    FString::Printf(TEXT("Local variable '%s' is not on '%s' after adding it"),
                        *VariableName, *FunctionGraph->GetName()),
                    MakeLocalVariableNames(Entry));
            }

            // The variable only becomes addressable (and so able to produce pins) once the blueprint
            // has been compiled into its function.
            FKismetEditorUtilities::CompileBlueprint(Blueprint);
        }

        if (!FUnrealMCPBlueprintGraphOps::CreateLocalVariableNode(FunctionGraph, VariableName, Description->VarGuid,
                                                                 bSet, NodePosition, Node, ErrorCode, ErrorMessage,
                                                                 Candidates))
        {
            return MakeCandidatesError(ErrorCode, ErrorMessage, Candidates);
        }
    }
    else if (!OwnerClass.IsEmpty())
    {
        if (!FUnrealMCPBlueprintGraphOps::CreateExternalVariableNode(Graph, OwnerClass, VariableName, bSet, NodePosition,
                                                                     Node, ErrorCode, ErrorMessage, Candidates))
        {
            return MakeCandidatesError(ErrorCode, ErrorMessage, Candidates);
        }
    }
    else if (!FUnrealMCPBlueprintGraphOps::CreateVariableNode(Graph, VariableName, bSet, NodePosition,
                                                             Node, ErrorCode, ErrorMessage, Candidates))
    {
        return MakeCandidatesError(ErrorCode, ErrorMessage, Candidates);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeWriteResult(Graph, Node, BuildReadbackOfNode(Node));
    ResultObj->SetStringField(TEXT("variable_name"), VariableName);
    ResultObj->SetStringField(TEXT("node_kind"), bSet ? TEXT("set") : TEXT("get"));
    ResultObj->SetBoolField(TEXT("is_local"), bIsLocal);
    ResultObj->SetStringField(TEXT("owner_class"), OwnerClass);
    const UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node);
    ResultObj->SetBoolField(TEXT("self_context"), VariableNode ? VariableNode->VariableReference.IsSelfContext() : true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintLiteralNode(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString LiteralType;
    if (!Params->TryGetStringField(TEXT("literal_type"), LiteralType))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'literal_type' parameter"));
    }

    FString Value;
    if (!Params->TryGetStringField(TEXT("value"), Value))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'value' parameter"));
    }

    UEdGraph* Graph = nullptr;
    if (!ResolveCommandGraph(Blueprint, Params, /*bCreateIfMissing=*/true, Graph, Error))
    {
        return Error;
    }

    UEdGraphNode* Node = nullptr;
    FString ValueAfter;
    FString ErrorCode;
    FString ErrorMessage;
    TArray<FString> Candidates;
    if (!FUnrealMCPBlueprintGraphOps::CreateLiteralNode(Graph, LiteralType, Value, ReadNodePosition(Params),
                                                       Node, ValueAfter, ErrorCode, ErrorMessage, Candidates))
    {
        return MakeCandidatesError(ErrorCode, ErrorMessage, Candidates);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeWriteResult(Graph, Node, BuildReadbackOfNode(Node));
    ResultObj->SetStringField(TEXT("literal_type"), LiteralType);
    ResultObj->SetStringField(TEXT("value"), ValueAfter);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintNodeByClass(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString NodeClass;
    if (!Params->TryGetStringField(TEXT("node_class"), NodeClass))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'node_class' parameter"));
    }

    UEdGraph* Graph = nullptr;
    if (!ResolveCommandGraph(Blueprint, Params, /*bCreateIfMissing=*/true, Graph, Error))
    {
        return Error;
    }

    UEdGraphNode* Node = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    TArray<FString> Candidates;
    if (!FUnrealMCPBlueprintGraphOps::CreateNodeByClass(Graph, NodeClass, ReadNodePosition(Params),
                                                       Node, ErrorCode, ErrorMessage, Candidates))
    {
        return MakeCandidatesError(ErrorCode, ErrorMessage, Candidates);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeWriteResult(Graph, Node, BuildReadbackOfNode(Node));
    ResultObj->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
    ResultObj->SetStringField(TEXT("comment"), TEXT("Skeleton node: configure its pins with set_blueprint_pin_default / connect_blueprint_nodes"));
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintInputActionNode(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString ActionName;
    if (!Params->TryGetStringField(TEXT("action_name"), ActionName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'action_name' parameter"));
    }

    UEdGraph* Graph = nullptr;
    if (!ResolveCommandGraph(Blueprint, Params, /*bCreateIfMissing=*/true, Graph, Error))
    {
        return Error;
    }

    UEdGraphNode* Node = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!FUnrealMCPBlueprintGraphOps::CreateInputActionNode(Graph, ActionName, ReadNodePosition(Params),
                                                           Node, ErrorCode, ErrorMessage))
    {
        return MakeCandidatesError(ErrorCode, ErrorMessage, {});
    }

    return MakeWriteResult(Graph, Node, BuildReadbackOfNode(Node));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintSelfReference(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    UEdGraph* Graph = nullptr;
    if (!ResolveCommandGraph(Blueprint, Params, /*bCreateIfMissing=*/true, Graph, Error))
    {
        return Error;
    }

    UEdGraphNode* Node = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!FUnrealMCPBlueprintGraphOps::CreateSelfReferenceNode(Graph, ReadNodePosition(Params),
                                                             Node, ErrorCode, ErrorMessage))
    {
        return MakeCandidatesError(ErrorCode, ErrorMessage, {});
    }

    return MakeWriteResult(Graph, Node, BuildReadbackOfNode(Node));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleFindBlueprintNodes(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString NodeType;
    if (!Params->TryGetStringField(TEXT("node_type"), NodeType))
    {
        NodeType = TEXT("All");
    }

    // Step 1: Find the asset
    UObject* Asset = FindAssetForNodes(BlueprintName);
    if (!Asset)
    {
        // For __current__, give a more specific error
        if (BlueprintName == TEXT("__current__"))
        {
            FString Reason;
            if (!GEditor)
            {
                Reason = TEXT("Editor not available");
            }
            else
            {
                UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
                if (AssetEditorSubsystem)
                {
                    TArray<UObject*> EditedAssets = AssetEditorSubsystem->GetAllEditedAssets();
                    Reason = FString::Printf(TEXT("EditedAssets count: %d"), EditedAssets.Num());
                    TSharedPtr<SDockTab> ActiveTab = FGlobalTabmanager::Get()->GetActiveTab();
                    if (ActiveTab.IsValid())
                    {
                        Reason += FString::Printf(TEXT(", ActiveTab: '%s'"), *ActiveTab->GetTabLabel().ToString());
                    }
                    else
                    {
                        Reason += TEXT(", No active tab");
                    }
                }
                else
                {
                    Reason = TEXT("AssetEditorSubsystem not available");
                }
            }
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("No supported asset focused. %s"), *Reason));
        }
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Asset not found: %s"), *BlueprintName));
    }

    // Step 2: Dispatch to type-specific handler
    return ProcessAssetNodes(Asset, NodeType, Params);
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleListBlueprintGraphs(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    TArray<FUnrealMCPGraphInfo> Graphs;
    FString ErrorCode;
    FString ErrorMessage;
    if (!FUnrealMCPBlueprintGraphOps::ListGraphs(Blueprint, Graphs, ErrorCode, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorCode, ErrorMessage);
    }

    TArray<TSharedPtr<FJsonValue>> GraphsArray;
    for (const FUnrealMCPGraphInfo& Graph : Graphs)
    {
        GraphsArray.Add(MakeShared<FJsonValueObject>(GraphInfoToJson(Graph)));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetArrayField(TEXT("graphs"), GraphsArray);
    ResultObj->SetNumberField(TEXT("count"), GraphsArray.Num());
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintFunctionGraph(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString FunctionName;
    if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("Missing 'function_name' parameter"));
    }

    // signature_class is optional: given, this creates the override graph for that class' event.
    UClass* SignatureClass = nullptr;
    FString SignatureClassName;
    if (Params->TryGetStringField(TEXT("signature_class"), SignatureClassName) && !SignatureClassName.IsEmpty())
    {
        TArray<FString> ClassCandidates;
        if (!FUnrealMCPBlueprintGraphOps::ResolveClass(SignatureClassName, SignatureClass, ClassCandidates))
        {
            TSharedPtr<FJsonObject> Response = FUnrealMCPCommonUtils::CreateErrorResponse(
                EUnrealMCPGraphError::NodeClassNotFound,
                FString::Printf(TEXT("signature_class '%s' could not be resolved"), *SignatureClassName));
            TArray<TSharedPtr<FJsonValue>> CandidatesJson;
            for (const FString& Candidate : ClassCandidates)
            {
                CandidatesJson.Add(MakeShared<FJsonValueString>(Candidate));
            }
            Response->SetArrayField(TEXT("candidates"), CandidatesJson);
            return Response;
        }
    }

    UEdGraph* Graph = nullptr;
    bool bCreated = false;
    UClass* SignatureOwner = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    TArray<FString> Candidates;
    if (!FUnrealMCPBlueprintGraphOps::AddFunctionGraph(Blueprint, FunctionName, SignatureClass, Graph, bCreated,
                                                       SignatureOwner, ErrorCode, ErrorMessage, Candidates))
    {
        TSharedPtr<FJsonObject> Response = FUnrealMCPCommonUtils::CreateErrorResponse(ErrorCode, ErrorMessage);
        TArray<TSharedPtr<FJsonValue>> CandidatesJson;
        for (const FString& Candidate : Candidates)
        {
            CandidatesJson.Add(MakeShared<FJsonValueString>(Candidate));
        }
        Response->SetArrayField(TEXT("candidates"), CandidatesJson);
        return Response;
    }

    // A structural change (a new function on the class) only becomes visible to the compiler after the
    // blueprint is marked modified; the compile result is the readback the caller asserts on.
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FString CompileStatus;
    bool bCompiled = false;
    TArray<FString> CompileErrors;
    TArray<FString> CompileWarnings;
    FUnrealMCPBlueprintGraphOps::CompileChecked(Blueprint, CompileStatus, bCompiled, CompileErrors, CompileWarnings);

    // Entry / result nodes are what the caller connects into; without their guids the next call has to guess.
    TArray<TSharedPtr<FJsonValue>> EntryNodesJson;
    TArray<TSharedPtr<FJsonValue>> ResultNodesJson;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node->IsA<UK2Node_FunctionEntry>())
        {
            EntryNodesJson.Add(MakeShared<FJsonValueString>(Node->NodeGuid.ToString()));
        }
        else if (Node->IsA<UK2Node_FunctionResult>())
        {
            ResultNodesJson.Add(MakeShared<FJsonValueString>(Node->NodeGuid.ToString()));
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetStringField(TEXT("function_name"), FunctionName);
    ResultObj->SetStringField(TEXT("graph_name"), Graph->GetName());
    ResultObj->SetBoolField(TEXT("created"), bCreated);
    ResultObj->SetStringField(TEXT("signature_class"), SignatureOwner ? SignatureOwner->GetName() : TEXT(""));
    ResultObj->SetBoolField(TEXT("is_override"), SignatureOwner != nullptr);
    ResultObj->SetArrayField(TEXT("entry_node_ids"), EntryNodesJson);
    ResultObj->SetArrayField(TEXT("result_node_ids"), ResultNodesJson);
    ResultObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
    ResultObj->SetStringField(TEXT("status"), CompileStatus);
    ResultObj->SetBoolField(TEXT("compiled"), bCompiled);
    TArray<TSharedPtr<FJsonValue>> ErrorsJson;
    for (const FString& Message : CompileErrors)
    {
        ErrorsJson.Add(MakeShared<FJsonValueString>(Message));
    }
    ResultObj->SetArrayField(TEXT("errors"), ErrorsJson);
    TArray<TSharedPtr<FJsonValue>> WarningsJson;
    for (const FString& Message : CompileWarnings)
    {
        WarningsJson.Add(MakeShared<FJsonValueString>(Message));
    }
    ResultObj->SetArrayField(TEXT("warnings"), WarningsJson);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleRemoveBlueprintFunctionGraph(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString FunctionName;
    if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("Missing 'function_name' parameter"));
    }

    FString ErrorCode;
    FString ErrorMessage;
    TArray<FString> Candidates;
    if (!FUnrealMCPBlueprintGraphOps::RemoveFunctionGraph(Blueprint, FunctionName, ErrorCode, ErrorMessage, Candidates))
    {
        TSharedPtr<FJsonObject> Response = FUnrealMCPCommonUtils::CreateErrorResponse(ErrorCode, ErrorMessage);
        TArray<TSharedPtr<FJsonValue>> CandidatesJson;
        for (const FString& Candidate : Candidates)
        {
            CandidatesJson.Add(MakeShared<FJsonValueString>(Candidate));
        }
        Response->SetArrayField(TEXT("candidates"), CandidatesJson);
        return Response;
    }

    FString CompileStatus;
    bool bCompiled = false;
    TArray<FString> CompileErrors;
    TArray<FString> CompileWarnings;
    FUnrealMCPBlueprintGraphOps::CompileChecked(Blueprint, CompileStatus, bCompiled, CompileErrors, CompileWarnings);

    // Read back what is left: the removal is only real when the function is gone from the graph list.
    TArray<UEdGraph*> Remaining;
    FUnrealMCPBlueprintGraphOps::ListFunctionGraphs(Blueprint, Remaining);
    TArray<TSharedPtr<FJsonValue>> RemainingJson;
    for (UEdGraph* Graph : Remaining)
    {
        RemainingJson.Add(MakeShared<FJsonValueString>(Graph->GetName()));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetStringField(TEXT("function_name"), FunctionName);
    ResultObj->SetBoolField(TEXT("removed"), true);
    ResultObj->SetArrayField(TEXT("function_graphs"), RemainingJson);
    ResultObj->SetStringField(TEXT("status"), CompileStatus);
    ResultObj->SetBoolField(TEXT("compiled"), bCompiled);
    TArray<TSharedPtr<FJsonValue>> ErrorsJson;
    for (const FString& Message : CompileErrors)
    {
        ErrorsJson.Add(MakeShared<FJsonValueString>(Message));
    }
    ResultObj->SetArrayField(TEXT("errors"), ErrorsJson);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleRenameBlueprintFunctionGraph(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString OldName;
    FString NewName;
    if (!Params->TryGetStringField(TEXT("old_name"), OldName) || !Params->TryGetStringField(TEXT("new_name"), NewName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("Both 'old_name' and 'new_name' are required"));
    }

    FString ErrorCode;
    FString ErrorMessage;
    if (!FUnrealMCPBlueprintGraphOps::RenameFunctionGraph(Blueprint, OldName, NewName, ErrorCode, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorCode, ErrorMessage);
    }

    FString CompileStatus;
    bool bCompiled = false;
    TArray<FString> CompileErrors;
    TArray<FString> CompileWarnings;
    FUnrealMCPBlueprintGraphOps::CompileChecked(Blueprint, CompileStatus, bCompiled, CompileErrors, CompileWarnings);

    TArray<UEdGraph*> Remaining;
    FUnrealMCPBlueprintGraphOps::ListFunctionGraphs(Blueprint, Remaining);
    TArray<TSharedPtr<FJsonValue>> NamesJson;
    for (UEdGraph* Graph : Remaining)
    {
        NamesJson.Add(MakeShared<FJsonValueString>(Graph->GetName()));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetStringField(TEXT("old_name"), OldName);
    ResultObj->SetStringField(TEXT("new_name"), NewName);
    ResultObj->SetBoolField(TEXT("renamed"), true);
    ResultObj->SetArrayField(TEXT("function_graphs"), NamesJson);
    ResultObj->SetStringField(TEXT("status"), CompileStatus);
    ResultObj->SetBoolField(TEXT("compiled"), bCompiled);
    TArray<TSharedPtr<FJsonValue>> ErrorsJson;
    for (const FString& Message : CompileErrors)
    {
        ErrorsJson.Add(MakeShared<FJsonValueString>(Message));
    }
    ResultObj->SetArrayField(TEXT("errors"), ErrorsJson);
    return ResultObj;
}

// Shared tail for the three parameter commands: read the signature back and compile, so the caller asserts on
// the resulting signature instead of on the command's own echo.
static TSharedPtr<FJsonObject> MakeFunctionParamResult(UBlueprint* Blueprint, UEdGraph* Graph, const FString& FunctionName)
{
    TArray<TSharedPtr<FJsonObject>> Params;
    FUnrealMCPBlueprintGraphOps::DescribeFunctionParams(Graph, Params);
    TArray<TSharedPtr<FJsonValue>> ParamsJson;
    for (const TSharedPtr<FJsonObject>& Param : Params)
    {
        ParamsJson.Add(MakeShared<FJsonValueObject>(Param));
    }

    FString CompileStatus;
    bool bCompiled = false;
    TArray<FString> CompileErrors;
    TArray<FString> CompileWarnings;
    FUnrealMCPBlueprintGraphOps::CompileChecked(Blueprint, CompileStatus, bCompiled, CompileErrors, CompileWarnings);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetStringField(TEXT("function_name"), FunctionName);
    ResultObj->SetArrayField(TEXT("params"), ParamsJson);
    ResultObj->SetNumberField(TEXT("param_count"), ParamsJson.Num());
    ResultObj->SetStringField(TEXT("graph_name"), Graph->GetName());
    ResultObj->SetStringField(TEXT("status"), CompileStatus);
    ResultObj->SetBoolField(TEXT("compiled"), bCompiled);
    TArray<TSharedPtr<FJsonValue>> ErrorsJson;
    for (const FString& Message : CompileErrors)
    {
        ErrorsJson.Add(MakeShared<FJsonValueString>(Message));
    }
    ResultObj->SetArrayField(TEXT("errors"), ErrorsJson);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintFunctionParam(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString FunctionName;
    FString ParamName;
    FString ParamType;
    if (!Params->TryGetStringField(TEXT("function_name"), FunctionName) ||
        !Params->TryGetStringField(TEXT("param_name"), ParamName) ||
        !Params->TryGetStringField(TEXT("param_type"), ParamType))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("'function_name', 'param_name' and 'param_type' are required"));
    }

    FUnrealMCPBlueprintGraphOps::FFunctionParamRequest Request;
    Request.Name = ParamName;
    Request.Type = ParamType;
    Params->TryGetStringField(TEXT("sub_class"), Request.SubClass);
    Params->TryGetStringField(TEXT("default_value"), Request.DefaultValue);
    Params->TryGetBoolField(TEXT("is_output"), Request.bIsOutput);

    TArray<UEdGraph*> FunctionGraphs;
    FUnrealMCPBlueprintGraphOps::ListFunctionGraphs(Blueprint, FunctionGraphs);
    UEdGraph* Graph = nullptr;
    for (UEdGraph* Candidate : FunctionGraphs)
    {
        if (Candidate->GetName() == FunctionName)
        {
            Graph = Candidate;
            break;
        }
    }
    if (!Graph)
    {
        TSharedPtr<FJsonObject> Response = FUnrealMCPCommonUtils::CreateErrorResponse(
            EUnrealMCPGraphError::FunctionGraphNotFound,
            FString::Printf(TEXT("No function graph named '%s'"), *FunctionName));
        TArray<TSharedPtr<FJsonValue>> CandidatesJson;
        for (UEdGraph* Candidate : FunctionGraphs)
        {
            CandidatesJson.Add(MakeShared<FJsonValueString>(Candidate->GetName()));
        }
        Response->SetArrayField(TEXT("candidates"), CandidatesJson);
        return Response;
    }

    FString ErrorCode;
    FString ErrorMessage;
    TArray<FString> Candidates;
    if (!FUnrealMCPBlueprintGraphOps::AddFunctionParam(Blueprint, Graph, Request, ErrorCode, ErrorMessage, Candidates))
    {
        TSharedPtr<FJsonObject> Response = FUnrealMCPCommonUtils::CreateErrorResponse(ErrorCode, ErrorMessage);
        TArray<TSharedPtr<FJsonValue>> CandidatesJson;
        for (const FString& Candidate : Candidates)
        {
            CandidatesJson.Add(MakeShared<FJsonValueString>(Candidate));
        }
        Response->SetArrayField(TEXT("candidates"), CandidatesJson);
        return Response;
    }

    return MakeFunctionParamResult(Blueprint, Graph, FunctionName);
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleRemoveBlueprintFunctionParam(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString FunctionName;
    FString ParamName;
    if (!Params->TryGetStringField(TEXT("function_name"), FunctionName) ||
        !Params->TryGetStringField(TEXT("param_name"), ParamName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("Both 'function_name' and 'param_name' are required"));
    }

    TArray<UEdGraph*> FunctionGraphs;
    FUnrealMCPBlueprintGraphOps::ListFunctionGraphs(Blueprint, FunctionGraphs);
    UEdGraph* Graph = nullptr;
    for (UEdGraph* Candidate : FunctionGraphs)
    {
        if (Candidate->GetName() == FunctionName)
        {
            Graph = Candidate;
            break;
        }
    }
    if (!Graph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::FunctionGraphNotFound,
            FString::Printf(TEXT("No function graph named '%s'"), *FunctionName));
    }

    FString ErrorCode;
    FString ErrorMessage;
    bool bWasOutput = false;
    if (!FUnrealMCPBlueprintGraphOps::RemoveFunctionParam(Blueprint, Graph, ParamName, bWasOutput, ErrorCode, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorCode, ErrorMessage);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeFunctionParamResult(Blueprint, Graph, FunctionName);
    ResultObj->SetStringField(TEXT("removed"), ParamName);
    ResultObj->SetBoolField(TEXT("was_output"), bWasOutput);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleRenameBlueprintFunctionParam(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString FunctionName;
    FString OldName;
    FString NewName;
    if (!Params->TryGetStringField(TEXT("function_name"), FunctionName) ||
        !Params->TryGetStringField(TEXT("old_name"), OldName) ||
        !Params->TryGetStringField(TEXT("new_name"), NewName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("'function_name', 'old_name' and 'new_name' are required"));
    }

    TArray<UEdGraph*> FunctionGraphs;
    FUnrealMCPBlueprintGraphOps::ListFunctionGraphs(Blueprint, FunctionGraphs);
    UEdGraph* Graph = nullptr;
    for (UEdGraph* Candidate : FunctionGraphs)
    {
        if (Candidate->GetName() == FunctionName)
        {
            Graph = Candidate;
            break;
        }
    }
    if (!Graph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::FunctionGraphNotFound,
            FString::Printf(TEXT("No function graph named '%s'"), *FunctionName));
    }

    FString ErrorCode;
    FString ErrorMessage;
    if (!FUnrealMCPBlueprintGraphOps::RenameFunctionParam(Blueprint, Graph, OldName, NewName, ErrorCode, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorCode, ErrorMessage);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeFunctionParamResult(Blueprint, Graph, FunctionName);
    ResultObj->SetStringField(TEXT("old_name"), OldName);
    ResultObj->SetStringField(TEXT("new_name"), NewName);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleListBlueprintFunctionGraphs(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    TArray<UEdGraph*> FunctionGraphs;
    FUnrealMCPBlueprintGraphOps::ListFunctionGraphs(Blueprint, FunctionGraphs);

    TArray<TSharedPtr<FJsonValue>> GraphsArray;
    for (UEdGraph* Graph : FunctionGraphs)
    {
        FUnrealMCPGraphInfo Info;
        FUnrealMCPBlueprintGraphOps::MakeGraphInfo(Blueprint, Graph, Info);
        TSharedPtr<FJsonObject> Entry = GraphInfoToJson(Info);

        // Signature source: a user function's entry points at the blueprint's own class, an override's at
        // the parent that declares the event.
        UClass* Owner = nullptr;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (const UK2Node_FunctionEntry* FunctionEntry = Cast<UK2Node_FunctionEntry>(Node))
            {
                Owner = FunctionEntry->FunctionReference.GetMemberParentClass();
            }
        }
        const bool bIsOverride = Owner != nullptr && Owner != Blueprint->GeneratedClass;
        Entry->SetStringField(TEXT("signature_class"), Owner ? Owner->GetName() : TEXT(""));
        Entry->SetBoolField(TEXT("is_override"), bIsOverride);

        // The signature is part of what the caller needs to see: parameters (inputs) and return values
        // (outputs) as they exist now.
        TArray<TSharedPtr<FJsonObject>> Params;
        FUnrealMCPBlueprintGraphOps::DescribeFunctionParams(Graph, Params);
        TArray<TSharedPtr<FJsonValue>> ParamsJson;
        for (const TSharedPtr<FJsonObject>& Param : Params)
        {
            ParamsJson.Add(MakeShared<FJsonValueObject>(Param));
        }
        Entry->SetArrayField(TEXT("params"), ParamsJson);
        const TSharedPtr<FJsonObject> Consistency = MakeFunctionConsistencyJson(Blueprint, Graph);
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Consistency->Values)
        {
            Entry->SetField(Field.Key, Field.Value);
        }
        GraphsArray.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetArrayField(TEXT("function_graphs"), GraphsArray);
    ResultObj->SetNumberField(TEXT("count"), GraphsArray.Num());
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleSetBlueprintPinDefault(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString NodeId;
    if (!Params->TryGetStringField(TEXT("node_id"), NodeId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'node_id' parameter"));
    }

    FString PinName;
    if (!Params->TryGetStringField(TEXT("pin_name"), PinName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'pin_name' parameter"));
    }

    if (!Params->HasField(TEXT("value")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'value' parameter"));
    }
    const TSharedPtr<FJsonValue> Value = Params->Values.FindRef(TEXT("value"));

    UEdGraph* Graph = nullptr;
    if (!ResolveCommandGraph(Blueprint, Params, /*bCreateIfMissing=*/true, Graph, Error))
    {
        return Error;
    }

    UEdGraphNode* Node = FUnrealMCPBlueprintGraphOps::FindNodeByGuid(Graph, NodeId);
    if (!Node)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::NodeNotFound,
            FString::Printf(TEXT("Node '%s' not found in graph '%s' of blueprint '%s'"),
                *NodeId, *Graph->GetName(), *Blueprint->GetName()));
    }

    FString PinErrorCode;
    FString PinErrorMessage;
    TArray<FString> Candidates;
    if (!FUnrealMCPBlueprintGraphOps::SetPinDefaultValue(Node, PinName, Value, PinErrorCode, PinErrorMessage, Candidates))
    {
        TSharedPtr<FJsonObject> Failure = MakeCandidatesError(PinErrorCode, PinErrorMessage, Candidates);
        // A list handed to a single-valued pin: give the MakeArray recipe instead of only the mismatch.
        FString MultiValueHint;
        if (Failure.IsValid() && FUnrealMCPBlueprintGraphOps::TryBuildMultiValuePinHint(Value, MultiValueHint))
        {
            Failure->SetStringField(TEXT("hint"), MultiValueHint);
        }
        return Failure;
    }

    UEdGraphPin* AppliedPin = FUnrealMCPBlueprintGraphOps::FindPin(Node, PinName, EGPD_Input);

    // An object/class pin legally carries an empty DefaultValue (the reference lives in
    // DefaultObject), so report the resolved path instead of an empty string.
    FString ValueAfter;
    if (AppliedPin)
    {
        ValueAfter = AppliedPin->DefaultValue;
        if (ValueAfter.IsEmpty() && AppliedPin->DefaultObject)
        {
            ValueAfter = AppliedPin->DefaultObject->GetPathName();
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeWriteResult(Graph, Node, BuildReadback({ AppliedPin }));
    ResultObj->SetStringField(TEXT("pin_name"), AppliedPin ? AppliedPin->PinName.ToString() : PinName);
    ResultObj->SetStringField(TEXT("default_value_after"), ValueAfter);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleSetBlueprintNodeProperty(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString NodeId;
    if (!Params->TryGetStringField(TEXT("node_id"), NodeId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'node_id' parameter"));
    }

    FString PropertyName;
    if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'property_name' parameter"));
    }

    if (!Params->HasField(TEXT("property_value")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'property_value' parameter"));
    }
    const TSharedPtr<FJsonValue> Value = Params->Values.FindRef(TEXT("property_value"));

    UEdGraph* Graph = nullptr;
    if (!ResolveCommandGraph(Blueprint, Params, /*bCreateIfMissing=*/true, Graph, Error))
    {
        return Error;
    }

    UEdGraphNode* Node = FUnrealMCPBlueprintGraphOps::FindNodeByGuid(Graph, NodeId);
    if (!Node)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::NodeNotFound,
            FString::Printf(TEXT("Node '%s' not found in graph '%s' of blueprint '%s'"),
                *NodeId, *Graph->GetName(), *Blueprint->GetName()));
    }

    FUnrealMCPNodePropertyWriteResult WriteResult;
    if (!FUnrealMCPBlueprintGraphOps::SetNodeProperty(Node, PropertyName, Value, WriteResult))
    {
        TSharedPtr<FJsonObject> Failure = MakeCandidatesError(WriteResult.Write, WriteResult.Candidates);
        if (WriteResult.ValueBefore.IsValid()) Failure->SetField(TEXT("property_value_before"), WriteResult.ValueBefore);
        if (WriteResult.ValueAfter.IsValid()) Failure->SetField(TEXT("property_value_after"), WriteResult.ValueAfter);
        // A dotted path that stopped half way names the fields the last reached level has: the old
        // flat "unknown_property" told the caller nothing about the struct sitting in the way.
        if (!WriteResult.PropertyPath.IsEmpty())
        {
            Failure->SetStringField(TEXT("property_path"), WriteResult.PropertyPath);
        }
        if (WriteResult.AvailablePathSegments.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> SegmentsJson;
            for (const FString& Segment : WriteResult.AvailablePathSegments)
            {
                SegmentsJson.Add(MakeShared<FJsonValueString>(Segment));
            }
            Failure->SetArrayField(TEXT("available_path_segments"), SegmentsJson);
        }
        return Failure;
    }

    // A rebuilt node has new pins, so hand them back for the caller to assert on.
    TSharedPtr<FJsonObject> ResultObj = MakeWriteResult(Graph, Node,
        WriteResult.bPinsRebuilt ? BuildReadbackOfNode(Node) : TArray<TSharedPtr<FJsonValue>>());
    ResultObj->SetStringField(TEXT("property_name"), PropertyName);
    // Read back the path that was actually resolved ("Node.ChainEnd"): without it a caller reading one
    // level too high cannot tell a success from a no-op.
    ResultObj->SetStringField(TEXT("property_path"), WriteResult.PropertyPath);
    ResultObj->SetStringField(TEXT("property_type"), WriteResult.PropertyType);
    if (WriteResult.PropertyTypeDetail.IsValid())
    {
        ResultObj->SetObjectField(TEXT("property_type_detail"), WriteResult.PropertyTypeDetail);
    }
    // JSON values, not encoded text: a caller that reads "[\"A\",\"B\"]" as a string has to parse it back
    // before it can use it, and the shape then disagrees with every other read path.
    if (WriteResult.ValueBefore.IsValid())
    {
        ResultObj->SetField(TEXT("property_value_before"), WriteResult.ValueBefore);
    }
    if (WriteResult.ValueAfter.IsValid())
    {
        ResultObj->SetField(TEXT("property_value_after"), WriteResult.ValueAfter);
    }
    ResultObj->SetBoolField(TEXT("pins_rebuilt"), WriteResult.bPinsRebuilt);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleDeleteBlueprintNodes(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    const TArray<TSharedPtr<FJsonValue>>* NodeIdsArray = nullptr;
    if (!Params->TryGetArrayField(TEXT("node_ids"), NodeIdsArray) || !NodeIdsArray)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"),
            TEXT("Missing 'node_ids' parameter (array of node guids)"));
    }

    UEdGraph* Graph = nullptr;
    if (!ResolveCommandGraph(Blueprint, Params, /*bCreateIfMissing=*/true, Graph, Error))
    {
        return Error;
    }

    bool bForce = false;
    Params->TryGetBoolField(TEXT("force"), bForce);

    TArray<TSharedPtr<FJsonValue>> DeletedArray;
    TArray<TSharedPtr<FJsonValue>> FailedArray;

    // Each guid is handled independently: one missing node must not abort the rest
    for (const TSharedPtr<FJsonValue>& NodeIdValue : *NodeIdsArray)
    {
        const FString NodeId = NodeIdValue.IsValid() ? NodeIdValue->AsString() : FString();

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("node_id"), NodeId);

        UEdGraphNode* Node = FUnrealMCPBlueprintGraphOps::FindNodeByGuid(Graph, NodeId);
        if (!Node)
        {
            Entry->SetStringField(TEXT("error_code"), EUnrealMCPGraphError::NodeNotFound);
            Entry->SetStringField(TEXT("error"), FString::Printf(TEXT("Node '%s' not found"), *NodeId));
            FailedArray.Add(MakeShared<FJsonValueObject>(Entry));
            continue;
        }

        Entry->SetStringField(TEXT("name"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
        Entry->SetStringField(TEXT("type"), Node->GetClass()->GetName());

        const bool bProtected = Node->IsA<UK2Node_FunctionEntry>() || Node->IsA<UK2Node_FunctionResult>() ||
            Node->IsA<UAnimGraphNode_Root>() || Node->IsA<UAnimGraphNode_StateMachine>();
        if (bProtected && !bForce)
        {
            Entry->SetStringField(TEXT("error_code"), TEXT("protected_node"));
            Entry->SetStringField(TEXT("error"), TEXT("Structural node deletion requires force=true"));
            Entry->SetStringField(TEXT("hint"), TEXT("Retry with force=true only when graph structure damage is intended"));
            FailedArray.Add(MakeShared<FJsonValueObject>(Entry));
            continue;
        }

        FString DeleteErrorCode;
        FString DeleteErrorMessage;
        if (!FUnrealMCPBlueprintGraphOps::DeleteNode(Graph, Node, DeleteErrorCode, DeleteErrorMessage))
        {
            Entry->SetStringField(TEXT("error_code"), DeleteErrorCode);
            Entry->SetStringField(TEXT("error"), DeleteErrorMessage);
            FailedArray.Add(MakeShared<FJsonValueObject>(Entry));
            continue;
        }
        DeletedArray.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeWriteResult(Graph, nullptr, {});
    ResultObj->SetArrayField(TEXT("deleted"), DeletedArray);
    ResultObj->SetArrayField(TEXT("failed"), FailedArray);
    ResultObj->SetNumberField(TEXT("deleted_count"), DeletedArray.Num());
    ResultObj->SetNumberField(TEXT("failed_count"), FailedArray.Num());
    ResultObj->SetNumberField(TEXT("remaining_nodes"), Graph->Nodes.Num());
    ResultObj->SetBoolField(TEXT("force"), bForce);
    if (bForce)
    {
        const bool bFunctionGraph = Graph->GetSchema() && Graph->GetSchema()->GetGraphType(Graph) == GT_Function;
        TSharedPtr<FJsonObject> Structure = bFunctionGraph
            ? MakeFunctionConsistencyJson(Blueprint, Graph) : MakeShared<FJsonObject>();
        bool bAnimRoot = false;
        bool bStateMachine = false;
        for (const UEdGraphNode* Remaining : Graph->Nodes)
        {
            bAnimRoot |= Remaining && Remaining->IsA<UAnimGraphNode_Root>();
            bStateMachine |= Remaining && Remaining->IsA<UAnimGraphNode_StateMachine>();
        }
        Structure->SetBoolField(TEXT("anim_root_present"), bAnimRoot);
        Structure->SetBoolField(TEXT("state_machine_present"), bStateMachine);
        ResultObj->SetObjectField(TEXT("structure_after"), Structure);
    }
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleDisconnectBlueprintPins(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString NodeId;
    if (!Params->TryGetStringField(TEXT("node_id"), NodeId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'node_id' parameter"));
    }

    FString PinName;
    if (!Params->TryGetStringField(TEXT("pin_name"), PinName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'pin_name' parameter"));
    }

    FString LinkedNodeId;
    Params->TryGetStringField(TEXT("linked_node_id"), LinkedNodeId);
    FString LinkedPinName;
    Params->TryGetStringField(TEXT("linked_pin_name"), LinkedPinName);

    UEdGraph* Graph = nullptr;
    if (!ResolveCommandGraph(Blueprint, Params, /*bCreateIfMissing=*/true, Graph, Error))
    {
        return Error;
    }

    UEdGraphNode* Node = FUnrealMCPBlueprintGraphOps::FindNodeByGuid(Graph, NodeId);
    if (!Node)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::NodeNotFound,
            FString::Printf(TEXT("Node '%s' not found in graph '%s' of blueprint '%s'"),
                *NodeId, *Graph->GetName(), *Blueprint->GetName()));
    }

    UEdGraphPin* PinBefore = FUnrealMCPBlueprintGraphOps::FindPin(Node, PinName, EGPD_MAX);
    const int32 LinksBefore = PinBefore ? PinBefore->LinkedTo.Num() : 0;

    int32 DisconnectedCount = 0;
    int32 RemainingLinks = 0;
    FString ErrorCode;
    FString ErrorMessage;
    TArray<FString> Candidates;
    if (!FUnrealMCPBlueprintGraphOps::DisconnectPin(Node, PinName, LinkedNodeId, LinkedPinName,
                                                   DisconnectedCount, RemainingLinks, ErrorCode, ErrorMessage, Candidates))
    {
        return MakeCandidatesError(ErrorCode, ErrorMessage, Candidates);
    }

    UEdGraphPin* Pin = FUnrealMCPBlueprintGraphOps::FindPin(Node, PinName, EGPD_MAX);

    TSharedPtr<FJsonObject> ResultObj = MakeWriteResult(Graph, Node, BuildReadback({ Pin }));
    ResultObj->SetStringField(TEXT("pin_name"), Pin ? Pin->PinName.ToString() : PinName);
    ResultObj->SetNumberField(TEXT("links_before"), LinksBefore);
    ResultObj->SetNumberField(TEXT("disconnected_count"), DisconnectedCount);
    ResultObj->SetNumberField(TEXT("remaining_links"), RemainingLinks);
    return ResultObj;
}

UObject* FUnrealMCPBlueprintNodeCommands::FindAssetForNodes(const FString& AssetName)
{
    // __current__: find the currently focused asset in the editor
    if (AssetName == TEXT("__current__"))
    {
        if (!GEditor)
        {
            return nullptr;
        }

        UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
        if (!AssetEditorSubsystem)
        {
            return nullptr;
        }

        TArray<UObject*> EditedAssets = AssetEditorSubsystem->GetAllEditedAssets();
        if (EditedAssets.Num() == 0)
        {
            return nullptr;
        }

        // Collect supported assets
        TArray<UObject*> Supported;
        for (UObject* Asset : EditedAssets)
        {
            if (Asset && (Asset->IsA<UBlueprint>() || FUnrealMCPMaterialOps::IsMaterialGraphAsset(Asset)))
            {
                Supported.Add(Asset);
            }
        }

        if (Supported.Num() == 0)
        {
            return nullptr;
        }
        if (Supported.Num() == 1)
        {
            return Supported[0];
        }

        // Multiple supported assets: use active tab to find which editor is focused.
        // ActiveTab may be a sub-tab (e.g. "EventGraph" inside a Blueprint editor).
        TSharedPtr<SDockTab> ActiveTab = FGlobalTabmanager::Get()->GetActiveTab();
        if (!ActiveTab.IsValid())
        {
            UE_LOG(LogUnrealMCP, Warning, TEXT("FindAssetForNodes: No active tab, Supported=%d, fallback=%s"),
                Supported.Num(), *Supported[0]->GetName());
            return Supported[0]; // fallback
        }

        UE_LOG(LogUnrealMCP, Display, TEXT("FindAssetForNodes: ActiveTab='%s', Supported=%d"),
            *ActiveTab->GetTabLabel().ToString(), Supported.Num());

        // Try ActiveTab label directly
        FString TabLabel = ActiveTab->GetTabLabel().ToString();
        for (UObject* Asset : Supported)
        {
            if (TabLabel.Equals(Asset->GetName(), ESearchCase::IgnoreCase) ||
                TabLabel.Contains(Asset->GetName()))
            {
                UE_LOG(LogUnrealMCP, Display, TEXT("FindAssetForNodes: Match tab label -> %s"), *Asset->GetName());
                return Asset;
            }
        }

        // Walk up tab hierarchy
        TSharedRef<FTabManager> TabManager = ActiveTab->GetTabManager();
        for (int32 Level = 0; Level < 3; ++Level)
        {
            TSharedPtr<SDockTab> OwnerTab = TabManager->GetOwnerTab();
            if (OwnerTab.IsValid())
            {
                FString OwnerLabel = OwnerTab->GetTabLabel().ToString();
                UE_LOG(LogUnrealMCP, Display, TEXT("FindAssetForNodes: OwnerTab[%d]='%s'"), Level, *OwnerLabel);
                for (UObject* Asset : Supported)
                {
                    if (OwnerLabel.Equals(Asset->GetName(), ESearchCase::IgnoreCase) ||
                        OwnerLabel.Contains(Asset->GetName()))
                    {
                        UE_LOG(LogUnrealMCP, Display, TEXT("FindAssetForNodes: Match owner tab -> %s"), *Asset->GetName());
                        return Asset;
                    }
                }
                TabManager = OwnerTab->GetTabManager();
            }
            else
            {
                break;
            }
        }

        // No supported asset has focus — return nullptr to let HandleFindBlueprintNodes report error
        UE_LOG(LogUnrealMCP, Warning, TEXT("FindAssetForNodes: No supported asset focused among %d candidates"), Supported.Num());
        return nullptr;
    }

    // Named asset: use FindAsset (LoadObject + AssetRegistry fallback)
    return FUnrealMCPCommonUtils::FindAsset(AssetName);
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::ProcessAssetNodes(UObject* Asset, const FString& NodeType, const TSharedPtr<FJsonObject>& Params)
{
    if (!Asset)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Invalid asset"));
    }

    // Blueprint
    if (UBlueprint* BP = Cast<UBlueprint>(Asset))
    {
        return ProcessBlueprintNodes(BP, NodeType, Params);
    }

   // Material / MaterialFunction expression graphs are serialized by the material domain.
    TSharedPtr<FJsonObject> MaterialNodes;
    if (FUnrealMCPMaterialOps::TrySerializeGraphNodes(Asset, NodeType, Params, MaterialNodes))
    {
        return MaterialNodes;
    }

    // Unsupported type
    return FUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Asset '%s' is a %s, not a Blueprint/Material/MaterialFunction"),
            *Asset->GetName(), *Asset->GetClass()->GetName()));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleVerifyBlueprintGraph(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint) return Error;
    UEdGraph* Graph = nullptr;
    if (!ResolveCommandGraph(Blueprint, Params, false, Graph, Error)) return Error;

    const TArray<FString> Available = { TEXT("dangling_producer"), TEXT("required_input_unconnected"),
        TEXT("exec_multi_source"), TEXT("unreachable_node"), TEXT("anim_sequence_missing"),
        TEXT("anim_root_unconnected") };
    TSet<FString> Rules;
    const TArray<TSharedPtr<FJsonValue>>* Requested = nullptr;
    if (Params->TryGetArrayField(TEXT("rules"), Requested) && Requested)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Requested)
        {
            const FString Rule = Value.IsValid() ? Value->AsString() : FString();
            if (!Available.Contains(Rule))
            {
                TSharedPtr<FJsonObject> Invalid = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_rule"),
                    FString::Printf(TEXT("Unknown graph verification rule '%s'"), *Rule));
                Invalid->SetArrayField(TEXT("available_rules"), MakeStringArray(Available));
                return Invalid;
            }
            Rules.Add(Rule);
        }
    }
    else
    {
        for (const FString& Rule : Available)
        {
            Rules.Add(Rule);
        }
    }
    int32 MaxIssues = 200;
    if (Params->HasField(TEXT("max_issues")))
    {
        MaxIssues = FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("max_issues"))), 1, 1000);
    }

    TArray<TSharedPtr<FJsonValue>> Issues;
    int32 Detected = 0;
    auto AddIssue = [&](const FString& Rule, const FString& Severity, const UEdGraphNode* Node,
                        const UEdGraphPin* Pin, const FString& Detail, const FString& Hint,
                        const UEdGraphPin* Related = nullptr)
    {
        ++Detected;
        if (Issues.Num() >= MaxIssues) return;
        TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
        Issue->SetStringField(TEXT("rule"), Rule);
        Issue->SetStringField(TEXT("severity"), Severity);
        Issue->SetStringField(TEXT("node_id"), Node ? Node->NodeGuid.ToString() : FString());
        Issue->SetStringField(TEXT("pin_name"), Pin ? Pin->PinName.ToString() : FString());
        Issue->SetStringField(TEXT("detail"), Detail);
        Issue->SetStringField(TEXT("fix_hint"), Hint);
        if (Related && Related->GetOwningNode())
        {
            Issue->SetStringField(TEXT("related_node_id"), Related->GetOwningNode()->NodeGuid.ToString());
            Issue->SetStringField(TEXT("related_pin_name"), Related->PinName.ToString());
        }
        Issues.Add(MakeShared<FJsonValueObject>(Issue));
    };

    // Exec reachability from event/function entry nodes.
    TSet<const UEdGraphNode*> Reachable;
    // Entry points, by the engine's own definition rather than by node class alone.
    //
    // This used to seed only UK2Node_Event / UK2Node_FunctionEntry, which reported every chain hanging off
    // an input event as unreachable: UK2Node_InputKey (and InputAction / InputTouch) is a UK2Node with
    // IK2Node_EventNodeInterface, NOT a UK2Node_Event (K2Node_InputKey.h:35). Measured 2026-10-02 on a fully
    // connected BP_GrabDriver graph: 6 false "unreachable_node" reports, one per node behind a key event.
    //
    // The judgement below is KismetCompiler::GatherRootSet (KismetCompiler.cpp:104-128) verbatim, including
    // its second mode - "non-pure K2Nodes without input pins", which is what an event source is. Keep it in
    // sync with the engine if that helper ever changes.
    TArray<const UEdGraphNode*> Queue;
    for (const UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node)
        {
            continue;
        }

        const UK2Node* K2Node = Cast<UK2Node>(Node);
        bool bIsRoot = Node->IsA<UK2Node_FunctionEntry>() || Node->IsA<UK2Node_Event>()
            || Node->IsA<UK2Node_Timeline>() || (K2Node && K2Node->IsNodeRootSet());

        if (!bIsRoot && K2Node && !K2Node->IsNodePure())
        {
            bool bHasInputPins = false;
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin && Pin->Direction == EGPD_Input)
                {
                    bHasInputPins = true;
                    break;
                }
            }
            bIsRoot = !bHasInputPins;
        }

        if (bIsRoot)
        {
            Reachable.Add(Node);
            Queue.Add(Node);
        }
    }
    for (int32 Index = 0; Index < Queue.Num(); ++Index)
    {
        for (const UEdGraphPin* Pin : Queue[Index]->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;
            for (const UEdGraphPin* Linked : Pin->LinkedTo)
            {
                const UEdGraphNode* Next = Linked ? Linked->GetOwningNode() : nullptr;
                if (Next && !Reachable.Contains(Next)) { Reachable.Add(Next); Queue.Add(Next); }
            }
        }
    }

    const UEdGraphSchema* Schema = Graph->GetSchema();
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node) continue;
        if (Rules.Contains(TEXT("anim_sequence_missing")))
        {
            if (const UAnimGraphNode_SequencePlayer* Player = Cast<UAnimGraphNode_SequencePlayer>(Node))
            {
                const FStructProperty* RuntimeNodeProperty = FindFProperty<FStructProperty>(Player->GetClass(), TEXT("Node"));
                const FObjectPropertyBase* SequenceProperty = RuntimeNodeProperty
                    ? FindFProperty<FObjectPropertyBase>(RuntimeNodeProperty->Struct, TEXT("Sequence")) : nullptr;
                const void* RuntimeNode = RuntimeNodeProperty
                    ? RuntimeNodeProperty->ContainerPtrToValuePtr<void>(Player) : nullptr;
                if (!SequenceProperty || !RuntimeNode || !SequenceProperty->GetObjectPropertyValue(
                        SequenceProperty->ContainerPtrToValuePtr<void>(RuntimeNode)))
                {
                    AddIssue(TEXT("anim_sequence_missing"), TEXT("error"), Node, nullptr,
                        TEXT("SequencePlayer has no animation sequence"), TEXT("Set inner_node.sequence"));
                }
            }
        }
        if (Rules.Contains(TEXT("anim_root_unconnected")) && Node->IsA<UAnimGraphNode_Root>())
        {
            const UEdGraphPin* Pose = nullptr;
            for (const UEdGraphPin* CandidatePin : Node->Pins)
            {
                if (CandidatePin && CandidatePin->Direction == EGPD_Input)
                {
                    Pose = CandidatePin;
                    break;
                }
            }
            if (!Pose || Pose->LinkedTo.Num() == 0) AddIssue(TEXT("anim_root_unconnected"), TEXT("error"), Node, Pose,
                TEXT("AnimGraph root pose input is unconnected"), TEXT("Connect a pose producer to Result"));
        }

        bool bHasExecInput = false;
        bool bHasExecOutput = false;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin) continue;
            const bool bExec = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
            bHasExecInput |= bExec && Pin->Direction == EGPD_Input;
            bHasExecOutput |= bExec && Pin->Direction == EGPD_Output;
            if (Rules.Contains(TEXT("exec_multi_source")) && bExec && Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() > 1)
            {
                AddIssue(TEXT("exec_multi_source"), TEXT("error"), Node, Pin,
                    FString::Printf(TEXT("Exec input has %d sources"), Pin->LinkedTo.Num()), TEXT("Keep one exec source"));
            }
            if (Rules.Contains(TEXT("required_input_unconnected")) && !bExec && Pin->Direction == EGPD_Input &&
                Pin->LinkedTo.Num() == 0 && Pin->DefaultValue.IsEmpty() && !Pin->DefaultObject &&
                !Pin->PinName.ToString().Contains(TEXT("WorldContext")) &&
                !IsImplicitlyBoundSelfPin(Node, Pin) &&
                (Pin->PinName == UEdGraphSchema_K2::PN_Self || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object ||
                 Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Interface))
            {
                AddIssue(TEXT("required_input_unconnected"), TEXT("error"), Node, Pin,
                    TEXT("Required object input is unconnected and has no default"), TEXT("Connect a compatible object output"));
            }
            if (Rules.Contains(TEXT("dangling_producer")) && !bExec && Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() == 0)
            {
                // One issue per producer pin: the first compatible input is enough to act on, and a
                // per-candidate report would multiply the same finding across the whole graph.
                // The candidate must have the SAME pin type: anything else (object -> string, real ->
                // vector) only connects through an implicit conversion node and would flag normal
                // blueprints - an unused event output has a convertible input somewhere in most graphs.
                bool bCandidateReported = false;
                for (UEdGraphNode* CandidateNode : Graph->Nodes)
                {
                    if (bCandidateReported) break;
                    if (!CandidateNode || CandidateNode == Node) continue;
                    for (UEdGraphPin* Candidate : CandidateNode->Pins)
                    {
                        if (!Candidate || Candidate->Direction != EGPD_Input) continue;
                        const bool bSamePinType =
                            Candidate->PinType.PinCategory == Pin->PinType.PinCategory &&
                            Candidate->PinType.PinSubCategoryObject == Pin->PinType.PinSubCategoryObject;
                        if (bSamePinType && Candidate->LinkedTo.Num() == 0 &&
                            !Candidate->DefaultValue.IsEmpty() && Schema &&
                            Schema->CanCreateConnection(Pin, Candidate).Response != CONNECT_RESPONSE_DISALLOW)
                        {
                            AddIssue(TEXT("dangling_producer"), TEXT("warning"), Node, Pin,
                                TEXT("Unused data output has a same-type unconnected input candidate"),
                                TEXT("Connect output or remove unused producer"), Candidate);
                            bCandidateReported = true;
                            break;
                        }
                    }
                }
            }
        }
        if (Rules.Contains(TEXT("unreachable_node")) && bHasExecInput && bHasExecOutput && !Reachable.Contains(Node))
        {
            AddIssue(TEXT("unreachable_node"), TEXT("warning"), Node, nullptr,
                TEXT("Executable node is not reachable from an event or function entry"), TEXT("Connect its exec input"));
        }
    }

    TArray<FString> RulesRun;
    for (const FString& Rule : Rules)
    {
        RulesRun.Add(Rule);
    }
    RulesRun.Sort();
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    Result->SetStringField(TEXT("graph_name"), Graph->GetName());
    // Entry count, so "nothing to report" cannot be confused with "no entry points to start from".
    Result->SetNumberField(TEXT("root_node_count"), Reachable.Num());
    Result->SetArrayField(TEXT("rules_run"), MakeStringArray(RulesRun));
    Result->SetArrayField(TEXT("issues"), Issues);
    Result->SetNumberField(TEXT("issue_count"), Issues.Num());
    Result->SetBoolField(TEXT("truncated"), Detected > Issues.Num());
    if (Detected > Issues.Num()) Result->SetNumberField(TEXT("total_detected_at_least"), Detected);
    return Result;
}

// ========================================================================
// 处理蓝图节点：遍历指定 UEdGraph 中的 UEdGraphNode
// 每个节点有 NodeGuid、Pins（UEdGraphPin），支持按类型过滤（All/Event）
// 读命令不建图：无目标图时返回空列表（除非显式传了 graph_name）
// ========================================================================
TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::ProcessBlueprintNodes(UBlueprint* Blueprint, const FString& NodeType, const TSharedPtr<FJsonObject>& Params)
{
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Invalid blueprint"));
    }

    FString RequestedGraphName;
    Params->TryGetStringField(TEXT("graph_name"), RequestedGraphName);

    UEdGraph* Graph = nullptr;
    FString GraphErrorCode;
    FString GraphErrorMessage;
    TArray<FString> GraphCandidates;
    const bool bResolved = FUnrealMCPBlueprintGraphOps::ResolveGraph(Blueprint, RequestedGraphName,
        /*bCreateEventGraphIfMissing=*/false, Graph, GraphErrorCode, GraphErrorMessage, GraphCandidates);

    if (!bResolved)
    {
        // Reading must not create a graph: a blueprint without one simply has no nodes.
        if (!RequestedGraphName.IsEmpty())
        {
            TSharedPtr<FJsonObject> GraphError = FUnrealMCPCommonUtils::CreateErrorResponse(GraphErrorCode, GraphErrorMessage);
            TArray<TSharedPtr<FJsonValue>> CandidateArray;
            for (const FString& Candidate : GraphCandidates)
            {
                CandidateArray.Add(MakeShared<FJsonValueString>(Candidate));
            }
            GraphError->SetArrayField(TEXT("available_graphs"), CandidateArray);
            return GraphError;
        }

        TSharedPtr<FJsonObject> EmptyResult = MakeShared<FJsonObject>();
        EmptyResult->SetStringField(TEXT("graph"), TEXT(""));
        EmptyResult->SetStringField(TEXT("graph_name"), TEXT(""));
        EmptyResult->SetArrayField(TEXT("nodes"), {});
        EmptyResult->SetArrayField(TEXT("node_ids"), {});
        EmptyResult->SetNumberField(TEXT("count"), 0);
        EmptyResult->SetNumberField(TEXT("node_count"), 0);
        // Same two fields as the populated path, so a caller never has to branch on their presence.
        EmptyResult->SetBoolField(TEXT("truncated"), false);
        EmptyResult->SetBoolField(TEXT("filtered"), NodeType != TEXT("All"));
        return EmptyResult;
    }

    FString EventNameFilter;
    if (NodeType == TEXT("Event") && !Params->TryGetStringField(TEXT("event_name"), EventNameFilter))
    {
        if (!Params->TryGetStringField(TEXT("event_type"), EventNameFilter))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_event_filter"),
                TEXT("Missing 'event_name' or 'event_type' parameter for Event node search"));
        }
    }

    int32 MaxNodes = 200;
    if (Params->HasField(TEXT("max_nodes")))
    {
        MaxNodes = FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("max_nodes"))), 1, 1000);
    }
    bool bVerbose = true;
    Params->TryGetBoolField(TEXT("verbose"), bVerbose);

    TArray<TSharedPtr<FJsonValue>> NodesArray;
    TArray<TSharedPtr<FJsonValue>> EventNodeIds;

    // Truncation means "a node that SHOULD have been listed was dropped because of max_nodes". It used
    // to be computed as `NodesArray.Num() < Graph->Nodes.Num()`, which also fires in filtered mode -
    // there the returned nodes are a subset by design, so the caller could not tell "raise max_nodes and
    // re-ask" (payload incomplete) from "you asked for a subset" (re-asking gains nothing).
    bool bTruncated = false;

    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node)
        {
            continue;
        }

        if (NodeType == TEXT("Event"))
        {
            UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
            if (!EventNode || EventNode->EventReference.GetMemberName() != FName(*EventNameFilter))
            {
                continue;
            }
            EventNodeIds.Add(MakeShared<FJsonValueString>(Node->NodeGuid.ToString()));
        }

        if (NodesArray.Num() >= MaxNodes)
        {
            // Keep scanning instead of breaking: only a node that passed the filter and still did not
            // fit is a real truncation, and a break cannot tell that apart from "the tail is filtered
            // out". Nothing is appended from here on.
            bTruncated = true;
            continue;
        }

        FUnrealMCPNodeInfo NodeInfo;
        FUnrealMCPBlueprintGraphOps::MakeNodeInfo(Node, NodeInfo);
        NodesArray.Add(MakeShared<FJsonValueObject>(NodeInfoToJson(NodeInfo, bVerbose)));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("graph"), Graph->GetName());
    ResultObj->SetStringField(TEXT("graph_name"), Graph->GetName());
    ResultObj->SetArrayField(TEXT("nodes"), NodesArray);
    // node_ids is the legacy Event-mode companion of the node array: in All mode the field is omitted
    // rather than written as an empty array (an empty array reads as "this graph has no nodes").
    if (NodeType == TEXT("Event"))
    {
        ResultObj->SetArrayField(TEXT("node_ids"), EventNodeIds);
    }
    ResultObj->SetNumberField(TEXT("count"), NodesArray.Num());
    ResultObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
    ResultObj->SetBoolField(TEXT("truncated"), bTruncated);
    // Filtering is reported separately from truncation: `truncated` means "the payload is incomplete,
    // re-ask with a bigger max_nodes", which is the wrong reaction to a filtered subset.
    ResultObj->SetBoolField(TEXT("filtered"), NodeType != TEXT("All"));
    if (!bVerbose)
    {
        const TArray<FString> OmittedFields = { TEXT("properties"), TEXT("pins"), TEXT("connections") };
        ResultObj->SetArrayField(TEXT("omitted_fields"), MakeStringArray(OmittedFields));
    }
    return ResultObj;
}

// =====================================================================================
// Structural editing: node position, pin shape, custom events, editor session
// =====================================================================================

namespace
{
    /**
     * Resolve one existing node for a node command.
     *
     * With an explicit graph_name the node is looked up in that graph only; without one every graph
     * of the blueprint is searched, because a node guid is unique across the blueprint and defaulting
     * to the event graph would report node_not_found for a node that plainly exists.
     */
    UEdGraphNode* ResolveExistingNode(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params,
                                      UEdGraph*& OutGraph, TSharedPtr<FJsonObject>& OutError)
    {
        FString NodeId;
        if (!Params->TryGetStringField(TEXT("node_id"), NodeId))
        {
            OutError = FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
                TEXT("Missing 'node_id' parameter"));
            return nullptr;
        }

        FString GraphName;
        Params->TryGetStringField(TEXT("graph_name"), GraphName);

        if (!GraphName.IsEmpty())
        {
            if (!ResolveCommandGraph(Blueprint, Params, /*bCreateIfMissing=*/false, OutGraph, OutError))
            {
                return nullptr;
            }
            UEdGraphNode* Node = FUnrealMCPBlueprintGraphOps::FindNodeByGuid(OutGraph, NodeId);
            if (!Node)
            {
                OutError = FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::NodeNotFound,
                    FString::Printf(TEXT("Node '%s' not found in graph '%s'"), *NodeId, *OutGraph->GetName()));
            }
            return Node;
        }

        TArray<UEdGraph*> AllGraphs;
        Blueprint->GetAllGraphs(AllGraphs);
        for (UEdGraph* Graph : AllGraphs)
        {
            if (!Graph)
            {
                continue;
            }
            if (UEdGraphNode* Node = FUnrealMCPBlueprintGraphOps::FindNodeByGuid(Graph, NodeId))
            {
                OutGraph = Graph;
                return Node;
            }
        }

        OutError = FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::NodeNotFound,
            FString::Printf(TEXT("Node '%s' not found in blueprint '%s'"), *NodeId, *Blueprint->GetName()));
        return nullptr;
    }

    TArray<TSharedPtr<FJsonValue>> MakeSubPinNames(const UEdGraphPin* Pin)
    {
        TArray<TSharedPtr<FJsonValue>> Items;
        if (Pin)
        {
            for (const UEdGraphPin* SubPin : Pin->SubPins)
            {
                if (SubPin)
                {
                    Items.Add(MakeShared<FJsonValueString>(SubPin->PinName.ToString()));
                }
            }
        }
        return Items;
    }

    /** A custom event's parameters are its own non-exec pins. */
    TArray<TSharedPtr<FJsonValue>> MakeEventParamReadback(const UEdGraphNode* Node)
    {
        TArray<TSharedPtr<FJsonValue>> Items;
        if (!Node)
        {
            return Items;
        }
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                continue;
            }
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("name"), Pin->PinName.ToString());
            Obj->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
            Obj->SetStringField(TEXT("sub_type"), Pin->PinType.PinSubCategory.ToString());
            Obj->SetStringField(TEXT("container"), Pin->PinType.IsArray() ? TEXT("array")
                : Pin->PinType.IsSet() ? TEXT("set")
                : Pin->PinType.IsMap() ? TEXT("map") : TEXT("none"));
            Obj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
            Items.Add(MakeShared<FJsonValueObject>(Obj));
        }
        return Items;
    }

    void CollectCustomEventNames(const UBlueprint* Blueprint, TArray<FString>& OutNames)
    {
        if (!Blueprint)
        {
            return;
        }
        TArray<UK2Node_CustomEvent*> CustomEvents;
        FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, CustomEvents);
        for (const UK2Node_CustomEvent* EventNode : CustomEvents)
        {
            if (EventNode)
            {
                OutNames.Add(EventNode->GetFunctionName().ToString());
            }
        }
    }

    /** Shared tail for the custom event commands: read the event back and compile. */
    TSharedPtr<FJsonObject> MakeCustomEventResult(UBlueprint* Blueprint, UEdGraph* Graph, UK2Node_CustomEvent* EventNode)
    {
        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
        ResultObj->SetStringField(TEXT("graph_name"), Graph ? Graph->GetName() : FString());
        if (EventNode)
        {
            ResultObj->SetStringField(TEXT("node_id"), EventNode->NodeGuid.ToString());
            ResultObj->SetStringField(TEXT("event_name"), EventNode->GetFunctionName().ToString());
        }
        const TArray<TSharedPtr<FJsonValue>> Params = MakeEventParamReadback(EventNode);
        ResultObj->SetArrayField(TEXT("params"), Params);
        ResultObj->SetNumberField(TEXT("param_count"), Params.Num());

        FString Status;
        bool bCompiled = false;
        TArray<FString> Errors;
        TArray<FString> Warnings;
        FUnrealMCPBlueprintGraphOps::CompileChecked(Blueprint, Status, bCompiled, Errors, Warnings);
        ResultObj->SetStringField(TEXT("status"), Status);
        ResultObj->SetBoolField(TEXT("compiled"), bCompiled);
        TArray<TSharedPtr<FJsonValue>> ErrorsJson;
        for (const FString& Message : Errors)
        {
            ErrorsJson.Add(MakeShared<FJsonValueString>(Message));
        }
        ResultObj->SetArrayField(TEXT("errors"), ErrorsJson);
        return ResultObj;
    }

    /** Shared tail for the pin-shape commands: report the compile result next to the pin readback. */
    void AddCompileResult(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ResultObj)
    {
        if (!Blueprint || !ResultObj.IsValid())
        {
            return;
        }

        FString Status;
        bool bCompiled = false;
        TArray<FString> Errors;
        TArray<FString> Warnings;
        FUnrealMCPBlueprintGraphOps::CompileChecked(Blueprint, Status, bCompiled, Errors, Warnings);

        ResultObj->SetStringField(TEXT("status"), Status);
        ResultObj->SetBoolField(TEXT("compiled"), bCompiled);
        TArray<TSharedPtr<FJsonValue>> ErrorsJson;
        for (const FString& Message : Errors)
        {
            ErrorsJson.Add(MakeShared<FJsonValueString>(Message));
        }
        ResultObj->SetArrayField(TEXT("errors"), ErrorsJson);
    }

    /** The blueprint editor instance for an asset, opened on demand. Never guesses a toolkit. */
    FBlueprintEditor* FindBlueprintEditor(UBlueprint* Blueprint, bool& bOutSessionUnavailable)
    {
        bOutSessionUnavailable = false;

        UAssetEditorSubsystem* AssetEditorSubsystem = GEditor
            ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
        if (!AssetEditorSubsystem)
        {
            bOutSessionUnavailable = true;
            return nullptr;
        }

        IAssetEditorInstance* Instance = AssetEditorSubsystem->FindEditorForAsset(Blueprint, /*bFocusIfOpen=*/false);
        if (!Instance)
        {
            AssetEditorSubsystem->OpenEditorForAsset(Blueprint);
            Instance = AssetEditorSubsystem->FindEditorForAsset(Blueprint, /*bFocusIfOpen=*/false);
        }

        if (!Instance || Instance->GetEditorName() != FName(TEXT("BlueprintEditor")))
        {
            bOutSessionUnavailable = true;
            return nullptr;
        }
        return static_cast<FBlueprintEditor*>(Instance);
    }

    TSharedPtr<FJsonObject> MakeEditorSessionError(const FString& Message)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("editor_not_open"), Message);
    }
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleMoveBlueprintNode(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    UEdGraph* Graph = nullptr;
    UEdGraphNode* Node = ResolveExistingNode(Blueprint, Params, Graph, Error);
    if (!Node)
    {
        return Error;
    }

    if (!Params->HasField(TEXT("position")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("Missing 'position' parameter ([X, Y])"));
    }
    const FVector2D Position = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("position"));

    Node->Modify();
    Node->NodePosX = static_cast<int32>(Position.X);
    Node->NodePosY = static_cast<int32>(Position.Y);
    FUnrealMCPBlueprintGraphOps::MarkModified(Graph);

    // Read the position back off the node rather than echoing the request.
    TSharedPtr<FJsonObject> ResultObj = MakeWriteResult(Graph, Node, {});
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetNumberField(TEXT("pos_x"), Node->NodePosX);
    ResultObj->SetNumberField(TEXT("pos_y"), Node->NodePosY);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleRefreshBlueprintNode(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    UEdGraph* Graph = nullptr;
    UEdGraphNode* Node = ResolveExistingNode(Blueprint, Params, Graph, Error);
    if (!Node)
    {
        return Error;
    }

    const FString NodeIdBefore = Node->NodeGuid.ToString();
    Node->Modify();
    Node->ReconstructNode();
    FUnrealMCPBlueprintGraphOps::MarkModified(Graph);

    TSharedPtr<FJsonObject> ResultObj = MakeWriteResult(Graph, Node, BuildReadbackOfNode(Node));
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetStringField(TEXT("node_id_before"), NodeIdBefore);
    ResultObj->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
    ResultObj->SetBoolField(TEXT("node_id_changed"), NodeIdBefore != Node->NodeGuid.ToString());
    ResultObj->SetNumberField(TEXT("pin_count"), Node->Pins.Num());
    AddCompileResult(Blueprint, ResultObj);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleSplitBlueprintPin(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    UEdGraph* Graph = nullptr;
    UEdGraphNode* Node = ResolveExistingNode(Blueprint, Params, Graph, Error);
    if (!Node)
    {
        return Error;
    }

    FString PinName;
    if (!Params->TryGetStringField(TEXT("pin_name"), PinName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("Missing 'pin_name' parameter"));
    }

    UEdGraphPin* Pin = FUnrealMCPBlueprintGraphOps::FindPin(Node, PinName);
    if (!Pin)
    {
        TArray<FString> PinNames;
        FUnrealMCPBlueprintGraphOps::CollectPinNames(Node, EGPD_MAX, PinNames);
        return MakeCandidatesError(EUnrealMCPGraphError::PinNotFound,
            FString::Printf(TEXT("Pin '%s' not found on node '%s'"), *PinName, *Node->NodeGuid.ToString()), PinNames);
    }

    if (Pin->SubPins.Num() > 0)
    {
        TSharedPtr<FJsonObject> ErrorObj = FUnrealMCPCommonUtils::CreateErrorResponse(
            EUnrealMCPGraphError::PinNotSplittable,
            FString::Printf(TEXT("Pin '%s' is already split"), *PinName));
        ErrorObj->SetArrayField(TEXT("sub_pins"), MakeSubPinNames(Pin));
        return ErrorObj;
    }

    if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Struct)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::PinNotSplittable,
            FString::Printf(TEXT("Pin '%s' is a %s; only struct pins can be split"),
                *PinName, *Pin->PinType.PinCategory.ToString()));
    }

    // SplitPin / RecombinePin are virtual on the schema, so they are reachable across modules
    // (the non-virtual members of UEdGraphSchema_K2 are the ones that fail to link).
    GetDefault<UEdGraphSchema_K2>()->SplitPin(Pin, /*bNotify=*/true);
    FUnrealMCPBlueprintGraphOps::MarkModified(Graph);

    if (Pin->SubPins.Num() == 0)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::PinNotSplittable,
            FString::Printf(TEXT("The schema refused to split pin '%s' (it may be connected or already recombined)"),
                *PinName));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeWriteResult(Graph, Node, BuildReadbackOfNode(Node));
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetStringField(TEXT("pin_name"), PinName);
    ResultObj->SetArrayField(TEXT("sub_pins"), MakeSubPinNames(Pin));
    ResultObj->SetNumberField(TEXT("sub_pin_count"), Pin->SubPins.Num());
    AddCompileResult(Blueprint, ResultObj);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleRecombineBlueprintPin(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    UEdGraph* Graph = nullptr;
    UEdGraphNode* Node = ResolveExistingNode(Blueprint, Params, Graph, Error);
    if (!Node)
    {
        return Error;
    }

    FString PinName;
    if (!Params->TryGetStringField(TEXT("pin_name"), PinName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("Missing 'pin_name' parameter"));
    }

    UEdGraphPin* Pin = FUnrealMCPBlueprintGraphOps::FindPin(Node, PinName);
    if (!Pin)
    {
        TArray<FString> PinNames;
        FUnrealMCPBlueprintGraphOps::CollectPinNames(Node, EGPD_MAX, PinNames);
        return MakeCandidatesError(EUnrealMCPGraphError::PinNotFound,
            FString::Printf(TEXT("Pin '%s' not found on node '%s'"), *PinName, *Node->NodeGuid.ToString()), PinNames);
    }

    if (Pin->SubPins.Num() == 0)
    {
        // Passing a sub-pin is a common mistake: point at the parent pin instead of guessing.
        TSharedPtr<FJsonObject> ErrorObj = FUnrealMCPCommonUtils::CreateErrorResponse(
            EUnrealMCPGraphError::PinNotSplittable,
            FString::Printf(TEXT("Pin '%s' has no sub-pins to recombine"), *PinName));
        if (Pin->ParentPin)
        {
            ErrorObj->SetStringField(TEXT("parent_pin"), Pin->ParentPin->PinName.ToString());
            ErrorObj->SetStringField(TEXT("hint"), TEXT("recombine the parent pin"));
        }
        return ErrorObj;
    }

    GetDefault<UEdGraphSchema_K2>()->RecombinePin(Pin);
    FUnrealMCPBlueprintGraphOps::MarkModified(Graph);

    TSharedPtr<FJsonObject> ResultObj = MakeWriteResult(Graph, Node, BuildReadbackOfNode(Node));
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetStringField(TEXT("pin_name"), PinName);
    ResultObj->SetArrayField(TEXT("sub_pins"), MakeSubPinNames(Pin));
    ResultObj->SetNumberField(TEXT("sub_pin_count"), Pin->SubPins.Num());
    AddCompileResult(Blueprint, ResultObj);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintCustomEventNode(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString EventName;
    if (!Params->TryGetStringField(TEXT("event_name"), EventName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("Missing 'event_name' parameter"));
    }

    UEdGraph* Graph = nullptr;
    if (!ResolveCommandGraph(Blueprint, Params, /*bCreateIfMissing=*/true, Graph, Error))
    {
        return Error;
    }

    TArray<FString> ExistingEvents;
    CollectCustomEventNames(Blueprint, ExistingEvents);
    if (ExistingEvents.Contains(EventName))
    {
        return MakeCandidatesError(EUnrealMCPGraphError::EventNameInUse,
            FString::Printf(TEXT("A custom event named '%s' already exists"), *EventName), ExistingEvents);
    }

    const FName EventFName(*EventName);
    if (Blueprint->FunctionGraphs.ContainsByPredicate(
        [EventFName](const UEdGraph* FunctionGraph) { return FunctionGraph && FunctionGraph->GetFName() == EventFName; }))
    {
        return MakeCandidatesError(EUnrealMCPGraphError::EventNameInUse,
            FString::Printf(TEXT("'%s' is already a function of this blueprint"), *EventName), ExistingEvents);
    }

    const FVector2D Position = ReadNodePosition(Params);
    UK2Node_CustomEvent* EventNode = FUnrealMCPBlueprintGraphOps::CreateGraphNode<UK2Node_CustomEvent>(
        Graph, Position,
        [EventFName](UK2Node_CustomEvent* Node) { Node->CustomFunctionName = EventFName; });
    if (!EventNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidValue,
            TEXT("Failed to create the custom event node"));
    }

    const TArray<TSharedPtr<FJsonValue>>* ParamArray = nullptr;
    if (Params->TryGetArrayField(TEXT("params"), ParamArray) && ParamArray)
    {
        // A parameter that cannot be created must not leave a half-built event behind.
        auto DropCreatedEvent = [Graph, EventNode]()
        {
            FString DeleteErrorCode;
            FString DeleteErrorMessage;
            FUnrealMCPBlueprintGraphOps::DeleteNode(Graph, EventNode, DeleteErrorCode, DeleteErrorMessage);
        };

        for (const TSharedPtr<FJsonValue>& ParamValue : *ParamArray)
        {
            const TSharedPtr<FJsonObject>* ParamObj = nullptr;
            if (!ParamValue.IsValid() || !ParamValue->TryGetObject(ParamObj) || !ParamObj)
            {
                DropCreatedEvent();
                return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
                    TEXT("Each entry of 'params' must be an object with name and type"));
            }

            FString ParamName;
            FString ParamType;
            if (!(*ParamObj)->TryGetStringField(TEXT("name"), ParamName) ||
                !(*ParamObj)->TryGetStringField(TEXT("type"), ParamType))
            {
                DropCreatedEvent();
                return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
                    TEXT("Each entry of 'params' needs 'name' and 'type'"));
            }

            FString SubClass;
            (*ParamObj)->TryGetStringField(TEXT("sub_class"), SubClass);

            FEdGraphPinType PinType;
            FString TypeError;
            TArray<FString> SupportedTypes;
            if (!FUnrealMCPBlueprintGraphOps::BuildVariablePinType(ParamType, SubClass, PinType, TypeError, SupportedTypes))
            {
                DropCreatedEvent();
                TSharedPtr<FJsonObject> TypeErrorObj = FUnrealMCPCommonUtils::CreateErrorResponse(
                    EUnrealMCPGraphError::UnsupportedVariableType, TypeError);
                TArray<TSharedPtr<FJsonValue>> SupportedJson;
                for (const FString& Supported : SupportedTypes)
                {
                    SupportedJson.Add(MakeShared<FJsonValueString>(Supported));
                }
                TypeErrorObj->SetArrayField(TEXT("supported_types"), SupportedJson);
                return TypeErrorObj;
            }

            FText CannotCreate;
            if (!EventNode->CanCreateUserDefinedPin(PinType, EGPD_Output, CannotCreate))
            {
                DropCreatedEvent();
                return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::UnsupportedPinType,
                    CannotCreate.ToString());
            }

            if (!EventNode->CreateUserDefinedPin(FName(*ParamName), PinType, EGPD_Output))
            {
                DropCreatedEvent();
                return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
                    FString::Printf(TEXT("Failed to create parameter '%s'"), *ParamName));
            }
        }
        EventNode->ReconstructNode();
    }

    FUnrealMCPBlueprintGraphOps::MarkModified(Graph);
    return MakeCustomEventResult(Blueprint, Graph, EventNode);
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleRenameBlueprintCustomEvent(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    UEdGraph* Graph = nullptr;
    UEdGraphNode* Node = ResolveExistingNode(Blueprint, Params, Graph, Error);
    if (!Node)
    {
        return Error;
    }

    UK2Node_CustomEvent* EventNode = Cast<UK2Node_CustomEvent>(Node);
    if (!EventNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            FString::Printf(TEXT("Node '%s' is a %s, not a custom event"),
                *Node->NodeGuid.ToString(), *Node->GetClass()->GetName()));
    }

    FString NewName;
    if (!Params->TryGetStringField(TEXT("new_name"), NewName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("Missing 'new_name' parameter"));
    }

    const FName NewFName(*NewName);
    TArray<FString> ExistingEvents;
    CollectCustomEventNames(Blueprint, ExistingEvents);
    ExistingEvents.Remove(EventNode->GetFunctionName().ToString());
    if (ExistingEvents.Contains(NewName))
    {
        return MakeCandidatesError(EUnrealMCPGraphError::EventNameInUse,
            FString::Printf(TEXT("A custom event named '%s' already exists"), *NewName), ExistingEvents);
    }

    const FString NodeIdBefore = EventNode->NodeGuid.ToString();
    EventNode->Modify();
    // The engine's own rename path for a custom event (sets the event name and re-titles the node).
    EventNode->OnRenameNode(NewName);
    FUnrealMCPBlueprintGraphOps::MarkModified(Graph);

    TSharedPtr<FJsonObject> ResultObj = MakeCustomEventResult(Blueprint, Graph, EventNode);
    // OnRenameNode keeps the node object, but the id is reported either way so a caller that
    // rebuilt the node is never left holding a stale guid.
    ResultObj->SetStringField(TEXT("node_id_before"), NodeIdBefore);
    ResultObj->SetBoolField(TEXT("node_id_changed"), NodeIdBefore != EventNode->NodeGuid.ToString());
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleOpenBlueprintGraph(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    UEdGraph* Graph = nullptr;
    if (!ResolveCommandGraph(Blueprint, Params, /*bCreateIfMissing=*/false, Graph, Error))
    {
        return Error;
    }

    bool bSessionUnavailable = false;
    FBlueprintEditor* BlueprintEditor = FindBlueprintEditor(Blueprint, bSessionUnavailable);
    if (!BlueprintEditor)
    {
        return MakeEditorSessionError(FString::Printf(
            TEXT("The blueprint editor for '%s' could not be opened"), *Blueprint->GetName()));
    }

    BlueprintEditor->OpenGraphAndBringToFront(Graph, /*bSetFocus=*/true);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetStringField(TEXT("graph_name"), Graph->GetName());
    ResultObj->SetStringField(TEXT("editor"), TEXT("BlueprintEditor"));
    ResultObj->SetBoolField(TEXT("opened"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleFocusBlueprintNode(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintForCommand(Params, Error);
    if (!Blueprint)
    {
        return Error;
    }

    UEdGraph* Graph = nullptr;
    UEdGraphNode* Node = ResolveExistingNode(Blueprint, Params, Graph, Error);
    if (!Node)
    {
        return Error;
    }

    bool bSessionUnavailable = false;
    FBlueprintEditor* BlueprintEditor = FindBlueprintEditor(Blueprint, bSessionUnavailable);
    if (!BlueprintEditor)
    {
        return MakeEditorSessionError(FString::Printf(
            TEXT("The blueprint editor for '%s' could not be opened"), *Blueprint->GetName()));
    }

    BlueprintEditor->JumpToNode(Node, /*bRequestRename=*/false);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetStringField(TEXT("graph_name"), Graph->GetName());
    ResultObj->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
    ResultObj->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
    ResultObj->SetBoolField(TEXT("focused"), true);
    return ResultObj;
}
