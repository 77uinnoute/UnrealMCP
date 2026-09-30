#include "Commands/Blueprint/UnrealMCPBlueprintGraphLibrary.h"
#include "Commands/Blueprint/UnrealMCPBlueprintGraphOps.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Engine/Blueprint.h"
#include "Editor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"

namespace
{
    FUnrealMCPGraphOpResult MakeGraphOpSuccess(const FString& ValueAfter = FString())
    {
        FUnrealMCPGraphOpResult Result;
        Result.Success = true;
        Result.ValueAfter = ValueAfter;
        return Result;
    }

    FUnrealMCPGraphOpResult MakeGraphOpFailure(const FString& ErrorCode, const FString& ErrorMessage,
                                               const TArray<FString>& Candidates)
    {
        FUnrealMCPGraphOpResult Result;
        Result.Success = false;
        Result.ErrorCode = ErrorCode;
        Result.ErrorMessage = ErrorMessage;
        Result.Candidates = Candidates;
        return Result;
    }

    FUnrealMCPGraphOpResult ToGraphOpResult(bool bSuccess, const FString& ErrorCode, const FString& ErrorMessage,
                                            const TArray<FString>& Candidates, const FString& ValueAfter = FString())
    {
        FUnrealMCPGraphOpResult Result;
        Result.Success = bSuccess;
        Result.ErrorCode = ErrorCode;
        Result.ErrorMessage = ErrorMessage;
        Result.Candidates = Candidates;
        Result.ValueAfter = ValueAfter;
        return Result;
    }

    FUnrealMCPNodeOpResult MakeNodeOpFailure(const FString& ErrorCode, const FString& ErrorMessage,
                                             const TArray<FString>& Candidates)
    {
        FUnrealMCPNodeOpResult Result;
        Result.Success = false;
        Result.ErrorCode = ErrorCode;
        Result.ErrorMessage = ErrorMessage;
        Result.Candidates = Candidates;
        return Result;
    }

    /** Every node result carries the created node plus its pins, so python can chain calls. */
    FUnrealMCPNodeOpResult MakeNodeOpSuccess(UEdGraphNode* Node, const FString& ValueAfter = FString())
    {
        FUnrealMCPNodeOpResult Result;
        Result.Success = true;
        Result.Node = Node;
        Result.ValueAfter = ValueAfter;

        if (Node)
        {
            FUnrealMCPNodeInfo Info;
            FUnrealMCPBlueprintGraphOps::MakeNodeInfo(Node, Info);
            Result.Readback = MoveTemp(Info.Pins);
        }
        return Result;
    }

    // Persist after every library node op, unless the dispatch asked not to: a python batch that set
    // persist=false (or a command that runs under one) flushes once at the end instead of once per call.
    void GuardPersist(UEdGraphNode* Node)
    {
        if (Node && FUnrealMCPCommonUtils::IsPersistEnabled())
        {
            FUnrealMCPBlueprintGraphOps::PersistGraphOwner(Node->GetGraph());
        }
    }

    void GuardPersist(UEdGraph* Graph)
    {
        if (Graph && FUnrealMCPCommonUtils::IsPersistEnabled())
        {
            FUnrealMCPBlueprintGraphOps::PersistGraphOwner(Graph);
        }
    }

    /** Node-building is refused on macro graphs: those are read-only for this plugin. */
    bool RejectMacroGraphBuild(UEdGraph* Graph, FString& OutErrorCode, FString& OutErrorMessage)
    {
        if (!Graph)
        {
            OutErrorCode = EUnrealMCPGraphError::InvalidParams;
            OutErrorMessage = TEXT("Invalid graph");
            return true;
        }

        UBlueprint* Blueprint = FUnrealMCPBlueprintGraphOps::FindBlueprintForGraph(Graph);
        if (FUnrealMCPBlueprintGraphOps::IsMacroGraph(Blueprint, Graph))
        {
            OutErrorCode = EUnrealMCPGraphError::UnsupportedGraphKind;
            OutErrorMessage = FString::Printf(
                TEXT("Graph '%s' is a macro graph; building nodes into macro graphs is not supported"),
                *Graph->GetName());
            return true;
        }
        return false;
    }
}

// ---------------------------------------------------------------------------
// Read
// ---------------------------------------------------------------------------

FUnrealMCPGraphArrayResult UUnrealMCPBlueprintGraphLibrary::GetGraphs(UBlueprint* Blueprint)
{
    FUnrealMCPGraphArrayResult Result;
    FString ErrorCode;
    FString ErrorMessage;
    Result.Success = FUnrealMCPBlueprintGraphOps::ListGraphs(Blueprint, Result.Graphs, ErrorCode, ErrorMessage);
    Result.ErrorCode = ErrorCode;
    Result.ErrorMessage = ErrorMessage;
    return Result;
}

FUnrealMCPNodeArrayResult UUnrealMCPBlueprintGraphLibrary::GetGraphNodes(UEdGraph* Graph)
{
    FUnrealMCPNodeArrayResult Result;

    if (!Graph)
    {
        Result.ErrorCode = EUnrealMCPGraphError::GraphNotFound;
        Result.ErrorMessage = TEXT("Invalid graph");
        return Result;
    }

    Result.Success = true;
    Result.GraphName = Graph->GetName();
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node)
        {
            FUnrealMCPNodeInfo Info;
            FUnrealMCPBlueprintGraphOps::MakeNodeInfo(Node, Info);
            Result.Nodes.Add(MoveTemp(Info));
        }
    }
    return Result;
}

TArray<FUnrealMCPPinInfo> UUnrealMCPBlueprintGraphLibrary::GetNodePins(UEdGraphNode* Node)
{
    TArray<FUnrealMCPPinInfo> Pins;
    if (!Node)
    {
        return Pins;
    }

    for (const UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin)
        {
            FUnrealMCPPinInfo Info;
            FUnrealMCPBlueprintGraphOps::MakePinInfo(Pin, Info);
            Pins.Add(MoveTemp(Info));
        }
    }
    return Pins;
}

FUnrealMCPNodeInfo UUnrealMCPBlueprintGraphLibrary::GetNodeInfo(UEdGraphNode* Node)
{
    FUnrealMCPNodeInfo Info;
    FUnrealMCPBlueprintGraphOps::MakeNodeInfo(Node, Info);
    return Info;
}

TArray<FUnrealMCPNodePropertyInfo> UUnrealMCPBlueprintGraphLibrary::GetNodeProperties(UEdGraphNode* Node)
{
    TArray<FUnrealMCPNodePropertyInfo> Properties;
    FUnrealMCPBlueprintGraphOps::CollectNodePropertyInfos(Node, Properties);
    return Properties;
}

// ---------------------------------------------------------------------------
// Node creation
// ---------------------------------------------------------------------------

FUnrealMCPNodeOpResult UUnrealMCPBlueprintGraphLibrary::AddFunctionCallNode(UEdGraph* Graph,
                                                                           const FString& TargetClass,
                                                                           const FString& FunctionName,
                                                                           int32 X, int32 Y)
{
    FString ErrorCode;
    FString ErrorMessage;
    if (RejectMacroGraphBuild(Graph, ErrorCode, ErrorMessage))
    {
        return MakeNodeOpFailure(ErrorCode, ErrorMessage, {});
    }

    UK2Node_CallFunction* Node = nullptr;
    TArray<FString> Candidates;
    if (!FUnrealMCPBlueprintGraphOps::CreateFunctionCallNode(Graph, TargetClass, FunctionName,
                                                            FVector2D(X, Y), Node, ErrorCode, ErrorMessage, Candidates))
    {
        return MakeNodeOpFailure(ErrorCode, ErrorMessage, Candidates);
    }

    GuardPersist(Node);
    return MakeNodeOpSuccess(Node);
}

FUnrealMCPNodeOpResult UUnrealMCPBlueprintGraphLibrary::AddEventNode(UEdGraph* Graph, const FString& EventName,
                                                                    int32 X, int32 Y)
{
    FString ErrorCode;
    FString ErrorMessage;
    if (RejectMacroGraphBuild(Graph, ErrorCode, ErrorMessage))
    {
        return MakeNodeOpFailure(ErrorCode, ErrorMessage, {});
    }

    UK2Node_Event* Node = nullptr;
    if (!FUnrealMCPBlueprintGraphOps::CreateEventNode(Graph, EventName, FVector2D(X, Y), Node,
                                                     ErrorCode, ErrorMessage))
    {
        return MakeNodeOpFailure(ErrorCode, ErrorMessage, {});
    }

    GuardPersist(Node);
    return MakeNodeOpSuccess(Node);
}

FUnrealMCPNodeOpResult UUnrealMCPBlueprintGraphLibrary::AddVariableGetNode(UEdGraph* Graph,
                                                                          const FString& VariableName,
                                                                          int32 X, int32 Y)
{
    FString ErrorCode;
    FString ErrorMessage;
    if (RejectMacroGraphBuild(Graph, ErrorCode, ErrorMessage))
    {
        return MakeNodeOpFailure(ErrorCode, ErrorMessage, {});
    }

    UEdGraphNode* Node = nullptr;
    TArray<FString> Candidates;
    if (!FUnrealMCPBlueprintGraphOps::CreateVariableNode(Graph, VariableName, /*bSet=*/false, FVector2D(X, Y),
                                                        Node, ErrorCode, ErrorMessage, Candidates))
    {
        return MakeNodeOpFailure(ErrorCode, ErrorMessage, Candidates);
    }

    GuardPersist(Node);
    return MakeNodeOpSuccess(Node);
}

FUnrealMCPNodeOpResult UUnrealMCPBlueprintGraphLibrary::AddVariableSetNode(UEdGraph* Graph,
                                                                          const FString& VariableName,
                                                                          int32 X, int32 Y)
{
    FString ErrorCode;
    FString ErrorMessage;
    if (RejectMacroGraphBuild(Graph, ErrorCode, ErrorMessage))
    {
        return MakeNodeOpFailure(ErrorCode, ErrorMessage, {});
    }

    UEdGraphNode* Node = nullptr;
    TArray<FString> Candidates;
    if (!FUnrealMCPBlueprintGraphOps::CreateVariableNode(Graph, VariableName, /*bSet=*/true, FVector2D(X, Y),
                                                        Node, ErrorCode, ErrorMessage, Candidates))
    {
        return MakeNodeOpFailure(ErrorCode, ErrorMessage, Candidates);
    }

    GuardPersist(Node);
    return MakeNodeOpSuccess(Node);
}

FUnrealMCPNodeOpResult UUnrealMCPBlueprintGraphLibrary::AddExternalVariableNode(UEdGraph* Graph,
                                                                               const FString& OwnerClass,
                                                                               const FString& VariableName,
                                                                               bool bSet, int32 X, int32 Y)
{
    FString ErrorCode;
    FString ErrorMessage;
    if (RejectMacroGraphBuild(Graph, ErrorCode, ErrorMessage))
    {
        return MakeNodeOpFailure(ErrorCode, ErrorMessage, {});
    }

    UEdGraphNode* Node = nullptr;
    TArray<FString> Candidates;
    if (!FUnrealMCPBlueprintGraphOps::CreateExternalVariableNode(Graph, OwnerClass, VariableName, bSet, FVector2D(X, Y),
                                                                Node, ErrorCode, ErrorMessage, Candidates))
    {
        return MakeNodeOpFailure(ErrorCode, ErrorMessage, Candidates);
    }

    GuardPersist(Node);
    return MakeNodeOpSuccess(Node);
}

FUnrealMCPNodeOpResult UUnrealMCPBlueprintGraphLibrary::AddLiteralNode(UEdGraph* Graph, const FString& LiteralType,
                                                                      const FString& Value, int32 X, int32 Y)
{
    FString ErrorCode;
    FString ErrorMessage;
    if (RejectMacroGraphBuild(Graph, ErrorCode, ErrorMessage))
    {
        return MakeNodeOpFailure(ErrorCode, ErrorMessage, {});
    }

    UEdGraphNode* Node = nullptr;
    FString ValueAfter;
    TArray<FString> Candidates;
    if (!FUnrealMCPBlueprintGraphOps::CreateLiteralNode(Graph, LiteralType, Value, FVector2D(X, Y),
                                                       Node, ValueAfter, ErrorCode, ErrorMessage, Candidates))
    {
        return MakeNodeOpFailure(ErrorCode, ErrorMessage, Candidates);
    }

    GuardPersist(Node);
    return MakeNodeOpSuccess(Node, ValueAfter);
}

FUnrealMCPNodeOpResult UUnrealMCPBlueprintGraphLibrary::AddNodeByClass(UEdGraph* Graph, const FString& NodeClass,
                                                                       int32 X, int32 Y)
{
    FString ErrorCode;
    FString ErrorMessage;
    if (RejectMacroGraphBuild(Graph, ErrorCode, ErrorMessage))
    {
        return MakeNodeOpFailure(ErrorCode, ErrorMessage, {});
    }

    UEdGraphNode* Node = nullptr;
    TArray<FString> Candidates;
    if (!FUnrealMCPBlueprintGraphOps::CreateNodeByClass(Graph, NodeClass, FVector2D(X, Y),
                                                       Node, ErrorCode, ErrorMessage, Candidates))
    {
        return MakeNodeOpFailure(ErrorCode, ErrorMessage, Candidates);
    }

    GuardPersist(Node);
    return MakeNodeOpSuccess(Node);
}

FUnrealMCPNodeOpResult UUnrealMCPBlueprintGraphLibrary::AddSelfReferenceNode(UEdGraph* Graph, int32 X, int32 Y)
{
    FString ErrorCode;
    FString ErrorMessage;
    if (RejectMacroGraphBuild(Graph, ErrorCode, ErrorMessage))
    {
        return MakeNodeOpFailure(ErrorCode, ErrorMessage, {});
    }

    UEdGraphNode* Node = nullptr;
    if (!FUnrealMCPBlueprintGraphOps::CreateSelfReferenceNode(Graph, FVector2D(X, Y), Node,
                                                             ErrorCode, ErrorMessage))
    {
        return MakeNodeOpFailure(ErrorCode, ErrorMessage, {});
    }

    GuardPersist(Node);
    return MakeNodeOpSuccess(Node);
}

FUnrealMCPNodeOpResult UUnrealMCPBlueprintGraphLibrary::AddInputActionNode(UEdGraph* Graph,
                                                                          const FString& ActionName,
                                                                          int32 X, int32 Y)
{
    FString ErrorCode;
    FString ErrorMessage;
    if (RejectMacroGraphBuild(Graph, ErrorCode, ErrorMessage))
    {
        return MakeNodeOpFailure(ErrorCode, ErrorMessage, {});
    }

    UEdGraphNode* Node = nullptr;
    if (!FUnrealMCPBlueprintGraphOps::CreateInputActionNode(Graph, ActionName, FVector2D(X, Y), Node,
                                                           ErrorCode, ErrorMessage))
    {
        return MakeNodeOpFailure(ErrorCode, ErrorMessage, {});
    }

    GuardPersist(Node);
    return MakeNodeOpSuccess(Node);
}

// ---------------------------------------------------------------------------
// Graph mutation
// ---------------------------------------------------------------------------

FUnrealMCPGraphOpResult UUnrealMCPBlueprintGraphLibrary::ConnectPins(UEdGraphNode* SourceNode,
                                                                    const FString& SourcePin,
                                                                    UEdGraphNode* TargetNode,
                                                                    const FString& TargetPin)
{
    if (!SourceNode || !TargetNode)
    {
        return MakeGraphOpFailure(EUnrealMCPGraphError::NodeNotFound, TEXT("Invalid source or target node"), {});
    }

    UEdGraph* Graph = SourceNode->GetGraph();
    if (!Graph)
    {
        return MakeGraphOpFailure(EUnrealMCPGraphError::GraphNotFound, TEXT("Source node has no graph"), {});
    }

    FString ErrorCode;
    FString ErrorMessage;
    TArray<FString> Candidates;
    if (!FUnrealMCPBlueprintGraphOps::ConnectNodes(Graph, SourceNode, SourcePin, TargetNode, TargetPin,
                                                  ErrorCode, ErrorMessage, Candidates))
    {
        return MakeGraphOpFailure(ErrorCode, ErrorMessage, Candidates);
    }

    GuardPersist(SourceNode);

    // Report the real endpoints, which may differ from the requested names after schema resolution.
    FUnrealMCPGraphOpResult Result = ToGraphOpResult(true, FString(), FString(), {});
    if (const UEdGraphPin* ResolvedSource = FUnrealMCPBlueprintGraphOps::FindPin(SourceNode, SourcePin, EGPD_Output))
    {
        Result.Candidates.Add(FString::Printf(TEXT("%s.%s"), *SourceNode->NodeGuid.ToString(),
            *ResolvedSource->PinName.ToString()));
        Result.ValueAfter = ResolvedSource->PinName.ToString();
    }
    if (const UEdGraphPin* ResolvedTarget = FUnrealMCPBlueprintGraphOps::FindPin(TargetNode, TargetPin, EGPD_Input))
    {
        Result.Candidates.Add(FString::Printf(TEXT("%s.%s"), *TargetNode->NodeGuid.ToString(),
            *ResolvedTarget->PinName.ToString()));
    }
    return Result;
}

FUnrealMCPGraphOpResult UUnrealMCPBlueprintGraphLibrary::SetPinDefault(UEdGraphNode* Node, const FString& PinName,
                                                                      const FString& Value, const FString& ValueKind)
{
    if (!Node)
    {
        return MakeGraphOpFailure(EUnrealMCPGraphError::NodeNotFound, TEXT("Invalid node"), {});
    }

    FString ErrorCode;
    FString ErrorMessage;
    TArray<FString> Candidates;
    if (!FUnrealMCPBlueprintGraphOps::SetPinDefaultValueFromString(Node, PinName, Value, ValueKind,
                                                                  ErrorCode, ErrorMessage, Candidates))
    {
        return MakeGraphOpFailure(ErrorCode, ErrorMessage, Candidates);
    }

    FString ValueAfter = Value;
    if (const UEdGraphPin* Pin = FUnrealMCPBlueprintGraphOps::FindPin(Node, PinName, EGPD_Input))
    {
        ValueAfter = Pin->DefaultValue;
    }

    FUnrealMCPGraphOpResult Result = MakeGraphOpSuccess(ValueAfter);
    GuardPersist(Node);
    return Result;
}

FUnrealMCPGraphOpResult UUnrealMCPBlueprintGraphLibrary::DisconnectPin(UEdGraphNode* Node, const FString& PinName,
                                                                      const FString& LinkedNodeId,
                                                                      const FString& LinkedPinName)
{
    if (!Node)
    {
        return MakeGraphOpFailure(EUnrealMCPGraphError::NodeNotFound, TEXT("Invalid node"), {});
    }

    int32 DisconnectedCount = 0;
    int32 RemainingLinks = 0;
    FString ErrorCode;
    FString ErrorMessage;
    TArray<FString> Candidates;
    if (!FUnrealMCPBlueprintGraphOps::DisconnectPin(Node, PinName, LinkedNodeId, LinkedPinName,
                                                   DisconnectedCount, RemainingLinks,
                                                   ErrorCode, ErrorMessage, Candidates))
    {
        return MakeGraphOpFailure(ErrorCode, ErrorMessage, Candidates);
    }

    GuardPersist(Node);

    FUnrealMCPGraphOpResult Result = MakeGraphOpSuccess(PinName);
    Result.Candidates.Add(FString::Printf(TEXT("disconnected=%d"), DisconnectedCount));
    Result.Candidates.Add(FString::Printf(TEXT("remaining=%d"), RemainingLinks));
    return Result;
}

FUnrealMCPNodePropertyOpResult UUnrealMCPBlueprintGraphLibrary::SetNodeProperty(UEdGraphNode* Node,
                                                                               const FString& PropertyName,
                                                                               const FString& Value,
                                                                               const FString& ValueKind)
{
    FUnrealMCPNodePropertyOpResult Result;

    if (!Node)
    {
        Result.ErrorCode = EUnrealMCPGraphError::NodeNotFound;
        Result.ErrorMessage = TEXT("Invalid node");
        return Result;
    }

    TSharedPtr<FJsonValue> ValueJson;
    FString ValueErrorMessage;
    if (!FUnrealMCPBlueprintGraphOps::MakeJsonValueFromString(Value, ValueKind, ValueJson, ValueErrorMessage))
    {
        Result.ErrorCode = EUnrealMCPGraphError::InvalidValue;
        Result.ErrorMessage = FString::Printf(TEXT("Property '%s': %s"), *PropertyName, *ValueErrorMessage);
        return Result;
    }

    FUnrealMCPNodePropertyWriteResult WriteResult;
    if (!FUnrealMCPBlueprintGraphOps::SetNodeProperty(Node, PropertyName, ValueJson, WriteResult))
    {
        Result.ErrorCode = WriteResult.Write.ErrorCode;
        Result.ErrorMessage = WriteResult.Write.ErrorMessage;
        Result.Candidates = WriteResult.Candidates.Num() > 0 ? WriteResult.Candidates : WriteResult.Write.Candidates;
        return Result;
    }

    Result.Success = true;
    Result.PropertyType = WriteResult.PropertyType;
    // This python-facing struct carries text (a USTRUCT field cannot hold a JSON value), so the values are
    // encoded only here; the MCP command route returns the same values as real JSON.
    Result.ValueBefore = FUnrealMCPBlueprintGraphOps::JsonValueToText(WriteResult.ValueBefore);
    Result.ValueAfter = FUnrealMCPBlueprintGraphOps::JsonValueToText(WriteResult.ValueAfter);
    Result.PinsRebuilt = WriteResult.bPinsRebuilt;
    if (WriteResult.bPinsRebuilt)
    {
        Result.Pins = GetNodePins(Node);
    }

    GuardPersist(Node);
    return Result;
}

FUnrealMCPGraphOpResult UUnrealMCPBlueprintGraphLibrary::DeleteNode(UEdGraphNode* Node)
{
    if (!Node)
    {
        return MakeGraphOpFailure(EUnrealMCPGraphError::NodeNotFound, TEXT("Invalid node"), {});
    }

    UEdGraph* Graph = Node->GetGraph();
    const FString NodeId = Node->NodeGuid.ToString();

    FString ErrorCode;
    FString ErrorMessage;
    if (!FUnrealMCPBlueprintGraphOps::DeleteNode(Graph, Node, ErrorCode, ErrorMessage))
    {
        return MakeGraphOpFailure(ErrorCode, ErrorMessage, {});
    }

    FUnrealMCPGraphOpResult Result = MakeGraphOpSuccess(NodeId);
    Result.Candidates.Add(FString::Printf(TEXT("remaining_nodes=%d"), Graph ? Graph->Nodes.Num() : 0));
    GuardPersist(Graph);
    return Result;
}

// ---------------------------------------------------------------------------
// Graph management
// ---------------------------------------------------------------------------

FUnrealMCPGraphArrayResult UUnrealMCPBlueprintGraphLibrary::EnsureEventGraph(UBlueprint* Blueprint)
{
    FUnrealMCPGraphArrayResult Result;

    UEdGraph* Graph = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!FUnrealMCPBlueprintGraphOps::EnsureEventGraph(Blueprint, Graph, ErrorCode, ErrorMessage))
    {
        Result.ErrorCode = ErrorCode;
        Result.ErrorMessage = ErrorMessage;
        return Result;
    }

    Result.Success = true;
    FUnrealMCPGraphInfo Info;
    FUnrealMCPBlueprintGraphOps::MakeGraphInfo(Blueprint, Graph, Info);
    Result.Graphs.Add(MoveTemp(Info));
    return Result;
}

FUnrealMCPGraphArrayResult UUnrealMCPBlueprintGraphLibrary::FindGraph(UBlueprint* Blueprint, const FString& GraphName)
{
    FUnrealMCPGraphArrayResult Result;

    UEdGraph* Graph = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    TArray<FString> Candidates;
    if (!FUnrealMCPBlueprintGraphOps::ResolveGraph(Blueprint, GraphName, /*bCreateEventGraphIfMissing=*/false,
                                                  Graph, ErrorCode, ErrorMessage, Candidates))
    {
        Result.ErrorCode = ErrorCode;
        Result.ErrorMessage = ErrorMessage;
        return Result;
    }

    Result.Success = true;
    FUnrealMCPGraphInfo Info;
    FUnrealMCPBlueprintGraphOps::MakeGraphInfo(Blueprint, Graph, Info);
    Result.Graphs.Add(MoveTemp(Info));
    return Result;
}

FUnrealMCPGraphArrayResult UUnrealMCPBlueprintGraphLibrary::AddFunctionGraph(UBlueprint* Blueprint,
                                                                            const FString& GraphName)
{
    FUnrealMCPGraphArrayResult Result;

    UEdGraph* Graph = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!FUnrealMCPBlueprintGraphOps::AddFunctionGraph(Blueprint, GraphName, Graph, ErrorCode, ErrorMessage))
    {
        Result.ErrorCode = ErrorCode;
        Result.ErrorMessage = ErrorMessage;
        return Result;
    }

    Result.Success = true;
    FUnrealMCPGraphInfo Info;
    FUnrealMCPBlueprintGraphOps::MakeGraphInfo(Blueprint, Graph, Info);
    Result.Graphs.Add(MoveTemp(Info));
    return Result;
}

// ---------------------------------------------------------------------------
// Compile / transaction
// ---------------------------------------------------------------------------

FUnrealMCPCompileResult UUnrealMCPBlueprintGraphLibrary::CompileBlueprintChecked(UBlueprint* Blueprint)
{
    FUnrealMCPCompileResult Result;

    if (!Blueprint)
    {
        Result.ErrorCode = EUnrealMCPGraphError::BlueprintNotFound;
        Result.ErrorMessage = TEXT("Invalid blueprint");
        return Result;
    }

    Result.Success = FUnrealMCPBlueprintGraphOps::CompileChecked(Blueprint, Result.Status, Result.Compiled,
                                                                Result.Errors, Result.Warnings);
    // A compile can rewrite the generated class; persist so the state survives a restart - unless the
    // dispatch asked to batch, in which case its own flush writes the same bytes once.
    if (FUnrealMCPCommonUtils::IsPersistEnabled())
    {
        FUnrealMCPCommonUtils::SaveAssetForObject(Blueprint);
    }
    return Result;
}

void UUnrealMCPBlueprintGraphLibrary::BeginTransaction(const FString& Description)
{
    if (GEditor)
    {
        GEditor->BeginTransaction(FText::FromString(Description.IsEmpty() ? TEXT("UnrealMCP graph edit") : Description));
    }
}

void UUnrealMCPBlueprintGraphLibrary::EndTransaction()
{
    if (GEditor)
    {
        GEditor->EndTransaction();
    }
}

// ---------------------------------------------------------------------------
// Introspection helpers
// ---------------------------------------------------------------------------

TArray<FString> UUnrealMCPBlueprintGraphLibrary::GetSupportedVariableTypes()
{
    return FUnrealMCPBlueprintGraphOps::SupportedVariableTypes();
}

TArray<FString> UUnrealMCPBlueprintGraphLibrary::GetSupportedLiteralTypes()
{
    return FUnrealMCPBlueprintGraphOps::SupportedLiteralTypes();
}
