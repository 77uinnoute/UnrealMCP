#include "Commands/Blueprint/UnrealMCPBlueprintGraphOps.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Compat/UnrealMCPVersionCompat.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_CallDataTableFunction.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallMaterialParameterCollectionFunction.h"
#include "K2Node_CommutativeAssociativeBinaryOperator.h"
#include "K2Node_Event.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_InputAction.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealMCPGraphOps, Log, All);

namespace
{
    /** Every graph the plugin is willing to address, in a stable order. */
    void GatherGraphs(UBlueprint* Blueprint, TArray<UEdGraph*>& OutGraphs)
    {
        OutGraphs.Reset();
        if (!Blueprint)
        {
            return;
        }

        for (UEdGraph* Graph : Blueprint->UbergraphPages)
        {
            if (Graph)
            {
                OutGraphs.Add(Graph);
            }
        }
        for (UEdGraph* Graph : Blueprint->FunctionGraphs)
        {
            if (Graph)
            {
                OutGraphs.Add(Graph);
            }
        }
        for (UEdGraph* Graph : Blueprint->MacroGraphs)
        {
            if (Graph)
            {
                OutGraphs.Add(Graph);
            }
        }
    }

    FString DescribePinType(const UEdGraphPin* Pin)
    {
        if (!Pin)
        {
            return FString(TEXT("<none>"));
        }

        FString Type = Pin->PinType.PinCategory.ToString();
        if (Pin->PinType.PinSubCategoryObject.IsValid())
        {
            Type += TEXT("/") + Pin->PinType.PinSubCategoryObject.Get()->GetName();
        }
        switch (Pin->PinType.ContainerType)
        {
        case EPinContainerType::Array: Type += TEXT("[]"); break;
        case EPinContainerType::Set:   Type += TEXT("{}"); break;
        case EPinContainerType::Map:   Type += TEXT("{,}"); break;
        default: break;
        }
        return Type;
    }

    /** Comma / space separated floats, for the string form of struct pin values. */
    bool ParseFloats(const FString& Text, TArray<double>& OutNumbers)
    {
        OutNumbers.Reset();

        TArray<FString> Tokens;
        Text.ParseIntoArray(Tokens, TEXT(","), true);
        if (Tokens.Num() <= 1)
        {
            Text.ParseIntoArray(Tokens, TEXT(" "), true);
        }

        for (const FString& Token : Tokens)
        {
            const FString Trimmed = Token.TrimStartAndEnd();
            if (Trimmed.IsEmpty())
            {
                continue;
            }
            if (!Trimmed.IsNumeric())
            {
                return false;
            }
            OutNumbers.Add(FCString::Atod(*Trimmed));
        }
        return OutNumbers.Num() > 0;
    }

    bool IsEventGraphName(const UEdGraph* Graph)
    {
        return Graph && Graph->GetName().Contains(TEXT("EventGraph"));
    }
}

UBlueprint* FUnrealMCPBlueprintGraphOps::FindBlueprintForGraph(UEdGraph* Graph)
{
    return Graph ? FBlueprintEditorUtils::FindBlueprintForGraph(Graph) : nullptr;
}

bool FUnrealMCPBlueprintGraphOps::IsMacroGraph(const UBlueprint* Blueprint, const UEdGraph* Graph)
{
    return Blueprint && Graph && Blueprint->MacroGraphs.Contains(const_cast<UEdGraph*>(Graph));
}

void FUnrealMCPBlueprintGraphOps::MarkModified(UEdGraph* Graph)
{
    if (UBlueprint* Blueprint = FindBlueprintForGraph(Graph))
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    }
}

void FUnrealMCPBlueprintGraphOps::PersistGraphOwner(UEdGraph* Graph)
{
    if (UBlueprint* Blueprint = FindBlueprintForGraph(Graph))
    {
        FUnrealMCPCommonUtils::SaveAssetForObject(Blueprint);
    }
}

// ---------------------------------------------------------------------------
// Graph resolution
// ---------------------------------------------------------------------------

bool FUnrealMCPBlueprintGraphOps::ListGraphs(UBlueprint* Blueprint, TArray<FUnrealMCPGraphInfo>& OutGraphs,
                                             FString& OutErrorCode, FString& OutErrorMessage)
{
    OutErrorCode.Reset();
    OutErrorMessage.Reset();
    OutGraphs.Reset();

    if (!Blueprint)
    {
        OutErrorCode = EUnrealMCPGraphError::BlueprintNotFound;
        OutErrorMessage = TEXT("Invalid blueprint");
        return false;
    }

    TArray<UEdGraph*> Graphs;
    GatherGraphs(Blueprint, Graphs);
    for (UEdGraph* Graph : Graphs)
    {
        FUnrealMCPGraphInfo Info;
        MakeGraphInfo(Blueprint, Graph, Info);
        OutGraphs.Add(MoveTemp(Info));
    }
    return true;
}

bool FUnrealMCPBlueprintGraphOps::ResolveGraph(UBlueprint* Blueprint, const FString& GraphName,
                                               bool bCreateEventGraphIfMissing, UEdGraph*& OutGraph,
                                               FString& OutErrorCode, FString& OutErrorMessage,
                                               TArray<FString>& OutCandidates)
{
    OutGraph = nullptr;
    OutErrorCode.Reset();
    OutErrorMessage.Reset();
    OutCandidates.Reset();

    if (!Blueprint)
    {
        OutErrorCode = EUnrealMCPGraphError::BlueprintNotFound;
        OutErrorMessage = TEXT("Invalid blueprint");
        return false;
    }

    TArray<UEdGraph*> Graphs;
    GatherGraphs(Blueprint, Graphs);
    for (UEdGraph* Graph : Graphs)
    {
        OutCandidates.Add(Graph->GetName());
    }

    // Explicit name wins and must match exactly: no guessing among similarly named graphs.
    if (!GraphName.IsEmpty())
    {
        for (UEdGraph* Graph : Graphs)
        {
            if (Graph->GetName() == GraphName)
            {
                OutGraph = Graph;
                return true;
            }
        }

        OutErrorCode = EUnrealMCPGraphError::GraphNotFound;
        OutErrorMessage = FString::Printf(TEXT("Graph '%s' not found in blueprint '%s'. Available graphs: %s"),
            *GraphName, *Blueprint->GetName(),
            OutCandidates.Num() > 0 ? *FString::Join(OutCandidates, TEXT(", ")) : TEXT("<none>"));
        return false;
    }

    // Backwards compatible fallback: the single graph whose name contains "EventGraph".
    TArray<UEdGraph*> EventGraphs;
    for (UEdGraph* Graph : Graphs)
    {
        if (IsEventGraphName(Graph))
        {
            EventGraphs.Add(Graph);
        }
    }

    if (EventGraphs.Num() == 1)
    {
        OutGraph = EventGraphs[0];
        return true;
    }

    if (EventGraphs.Num() > 1)
    {
        OutErrorCode = EUnrealMCPGraphError::GraphNotFound;
        OutErrorMessage = FString::Printf(
            TEXT("Blueprint '%s' has %d graphs whose name contains 'EventGraph'; pass 'graph_name'. Available graphs: %s"),
            *Blueprint->GetName(), EventGraphs.Num(), *FString::Join(OutCandidates, TEXT(", ")));
        return false;
    }

    if (!bCreateEventGraphIfMissing)
    {
        OutErrorCode = EUnrealMCPGraphError::GraphNotFound;
        OutErrorMessage = FString::Printf(TEXT("Blueprint '%s' has no event graph. Available graphs: %s"),
            *Blueprint->GetName(),
            OutCandidates.Num() > 0 ? *FString::Join(OutCandidates, TEXT(", ")) : TEXT("<none>"));
        return false;
    }

    UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
        Blueprint, FName(TEXT("EventGraph")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    if (!NewGraph)
    {
        OutErrorCode = EUnrealMCPGraphError::GraphNotFound;
        OutErrorMessage = TEXT("Failed to create an event graph for the blueprint");
        return false;
    }

    FBlueprintEditorUtils::AddUbergraphPage(Blueprint, NewGraph);
    OutGraph = NewGraph;
    return true;
}

bool FUnrealMCPBlueprintGraphOps::EnsureEventGraph(UBlueprint* Blueprint, UEdGraph*& OutGraph,
                                                   FString& OutErrorCode, FString& OutErrorMessage)
{
    TArray<FString> Candidates;
    return ResolveGraph(Blueprint, FString(), /*bCreateEventGraphIfMissing=*/true,
                        OutGraph, OutErrorCode, OutErrorMessage, Candidates);
}

bool FUnrealMCPBlueprintGraphOps::AddFunctionGraph(UBlueprint* Blueprint, const FString& GraphName,
                                                   UEdGraph*& OutGraph, FString& OutErrorCode,
                                                   FString& OutErrorMessage)
{
    bool bCreated = false;
    UClass* SignatureOwner = nullptr;
    TArray<FString> Candidates;
    return AddFunctionGraph(Blueprint, GraphName, /*SignatureClass=*/nullptr, OutGraph, bCreated,
                            SignatureOwner, OutErrorCode, OutErrorMessage, Candidates);
}

// The signature-owner of an existing function graph: the class the entry node's signature came from.
// A user function points at the blueprint's own generated class; an override points at the parent that
// declares the BlueprintImplementableEvent.
static UClass* FunctionGraphSignatureOwner(UBlueprint* Blueprint, UEdGraph* Graph)
{
    if (!Blueprint || !Graph)
    {
        return nullptr;
    }
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
        {
            return Entry->FunctionReference.GetMemberParentClass();
        }
    }
    return nullptr;
}

bool FUnrealMCPBlueprintGraphOps::AddFunctionGraph(UBlueprint* Blueprint, const FString& GraphName,
                                                   UClass* SignatureClass, UEdGraph*& OutGraph,
                                                   bool& bOutCreated, UClass*& OutSignatureOwner,
                                                   FString& OutErrorCode, FString& OutErrorMessage,
                                                   TArray<FString>& OutCandidates)
{
    OutGraph = nullptr;
    bOutCreated = false;
    OutSignatureOwner = nullptr;
    OutErrorCode.Reset();
    OutErrorMessage.Reset();
    OutCandidates.Reset();

    if (!Blueprint)
    {
        OutErrorCode = EUnrealMCPGraphError::BlueprintNotFound;
        OutErrorMessage = TEXT("Invalid blueprint");
        return false;
    }
    if (GraphName.IsEmpty())
    {
        OutErrorCode = EUnrealMCPGraphError::InvalidParams;
        OutErrorMessage = TEXT("Graph name must not be empty");
        return false;
    }

    // Implementing a parent's BlueprintImplementableEvent needs a signature to copy; resolve it up front so a
    // wrong class fails before any graph exists (no half-built function).
    if (SignatureClass)
    {
        UFunction* Signature = nullptr;
        if (!ResolveFunction(SignatureClass, GraphName, Signature, OutCandidates))
        {
            OutErrorCode = EUnrealMCPGraphError::SignatureFunctionNotFound;
            OutErrorMessage = FString::Printf(
                TEXT("Class '%s' does not declare a function named '%s'"), *SignatureClass->GetName(), *GraphName);

            // The caller's next move is to pick a real event: list what this class actually declares,
            // implementable events first (those are the ones a blueprint can implement).
            if (OutCandidates.Num() == 0)
            {
                TArray<FString> Implementable;
                TArray<FString> Others;
                for (TFieldIterator<UFunction> It(SignatureClass); It; ++It)
                {
                    const UFunction* Function = *It;
                    if (!Function || !Function->HasAnyFunctionFlags(FUNC_BlueprintEvent | FUNC_BlueprintCallable))
                    {
                        continue;
                    }
                    if (Function->HasAnyFunctionFlags(FUNC_BlueprintEvent))
                    {
                        Implementable.Add(Function->GetName());
                    }
                    else
                    {
                        Others.Add(Function->GetName());
                    }
                }
                Implementable.Sort();
                Others.Sort();
                Implementable.Append(Others);
                const int32 MaxCandidates = 40;
                for (int32 Index = 0; Index < Implementable.Num() && Index < MaxCandidates; ++Index)
                {
                    OutCandidates.Add(Implementable[Index]);
                }
            }
            return false;
        }
    }

    // Idempotent: an existing graph of that name is reused (that is what makes "implement the event" safe to
    // re-run after a failed compile), never duplicated.
    TArray<UEdGraph*> Graphs;
    GatherGraphs(Blueprint, Graphs);
    for (UEdGraph* Graph : Graphs)
    {
        if (Graph->GetName() == GraphName)
        {
            OutGraph = Graph;
            OutSignatureOwner = FunctionGraphSignatureOwner(Blueprint, Graph);
            return true;
        }
    }

    UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
        Blueprint, FName(*GraphName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    if (!NewGraph)
    {
        OutErrorCode = EUnrealMCPGraphError::InvalidParams;
        OutErrorMessage = FString::Printf(TEXT("Failed to create function graph '%s'"), *GraphName);
        return false;
    }

    // bIsUserCreated=false + SignatureFromObject is how the engine's own Persona adds the Received_Notify
    // override graph for an AnimNotify blueprint (PersonaModule.cpp:1646). Passing the signature class for a
    // plain user function would mark it as an override, so it is only used when the caller asked for one.
    FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, NewGraph,
        /*bIsUserCreated=*/ SignatureClass == nullptr, SignatureClass);

    OutGraph = NewGraph;
    bOutCreated = true;
    OutSignatureOwner = SignatureClass;
    return true;
}

UK2Node_FunctionEntry* FUnrealMCPBlueprintGraphOps::FindFunctionEntryNode(UEdGraph* Graph)
{
    if (!Graph)
    {
        return nullptr;
    }
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
        {
            return Entry;
        }
    }
    return nullptr;
}

// UK2Node_EditablePinBase::UserDefinedPinExists is not exported from the BlueprintGraph module (the class is
// MinimalAPI and that accessor carries no API macro), so scan the node's own pins instead. Exec pins are
// skipped for the same reason the signature listing skips them.
static bool HasUserDefinedParamPin(const UK2Node_EditablePinBase* Node, const FName ParamName)
{
    if (!Node)
    {
        return false;
    }
    for (const UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->PinName == ParamName && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
        {
            return true;
        }
    }
    return false;
}

// A parameter's pin lives on the entry node (inputs) or the result node (return values). Resolving both from
// one place is what lets the listing, the rename and the removal agree on what "that parameter" means.
static UK2Node_EditablePinBase* FindFunctionTerminatorForParam(UEdGraph* Graph, const FName ParamName,
                                                               bool& bOutIsOutput)
{
    bOutIsOutput = false;

    UK2Node_FunctionEntry* Entry = FUnrealMCPBlueprintGraphOps::FindFunctionEntryNode(Graph);
    if (Entry && HasUserDefinedParamPin(Entry, ParamName))
    {
        return Entry;
    }
    if (Entry)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node);
            if (Result && HasUserDefinedParamPin(Result, ParamName))
            {
                bOutIsOutput = true;
                return Result;
            }
        }
    }
    return nullptr;
}

void FUnrealMCPBlueprintGraphOps::DescribeFunctionParams(UEdGraph* Graph, TArray<TSharedPtr<FJsonObject>>& OutParams)
{
    OutParams.Reset();

    auto AddPins = [&OutParams](const UK2Node_EditablePinBase* Node, const TCHAR* Direction)
    {
        if (!Node)
        {
            return;
        }
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                continue;
            }
            TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("name"), Pin->PinName.ToString());
            Item->SetStringField(TEXT("direction"), Direction);
            Item->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
            Item->SetStringField(TEXT("sub_type"), Pin->PinType.PinSubCategory.ToString());
            Item->SetStringField(TEXT("container"), Pin->PinType.IsArray() ? TEXT("array")
                : Pin->PinType.IsSet() ? TEXT("set")
                : Pin->PinType.IsMap() ? TEXT("map") : TEXT("none"));
            Item->SetStringField(TEXT("default_value"), Pin->DefaultValue);
            OutParams.Add(Item);
        }
    };

    UK2Node_FunctionEntry* Entry = FindFunctionEntryNode(Graph);
    AddPins(Entry, TEXT("input"));
    if (Entry)
    {
        // Read-only: report a result node, never create one.
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node))
            {
                AddPins(Result, TEXT("output"));
                break;
            }
        }
    }
}

bool FUnrealMCPBlueprintGraphOps::AddFunctionParam(UBlueprint* Blueprint, UEdGraph* Graph,
                                                   const FFunctionParamRequest& Request,
                                                   FString& OutErrorCode, FString& OutErrorMessage,
                                                   TArray<FString>& OutCandidates)
{
    OutErrorCode.Reset();
    OutErrorMessage.Reset();
    OutCandidates.Reset();

    UK2Node_FunctionEntry* Entry = FindFunctionEntryNode(Graph);
    if (!Blueprint || !Graph || !Entry)
    {
        OutErrorCode = EUnrealMCPGraphError::FunctionGraphNotFound;
        OutErrorMessage = TEXT("The named graph is not a function graph");
        return false;
    }

    // An override's signature belongs to the parent class, so the engine marks its entry node non-editable.
    // Reporting that explicitly beats a silent no-op.
    if (!Entry->IsEditable())
    {
        OutErrorCode = EUnrealMCPGraphError::FunctionNotEditable;
        OutErrorMessage = FString::Printf(
            TEXT("Function '%s' overrides a parent signature; its parameters cannot be edited"),
            *Graph->GetName());
        return false;
    }

    FEdGraphPinType PinType;
    FString TypeError;
    if (!BuildVariablePinType(Request.Type, Request.SubClass, PinType, TypeError, OutCandidates))
    {
        OutErrorCode = EUnrealMCPGraphError::UnsupportedVariableType;
        OutErrorMessage = TypeError;
        return false;
    }

    UK2Node_EditablePinBase* Terminator = Entry;
    EEdGraphPinDirection Direction = EGPD_Output;   // an entry node's parameter pins point out of it
    if (Request.bIsOutput)
    {
        // Return values live on the result node, which the engine creates on demand.
        Terminator = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(Entry);
        Direction = EGPD_Input;                     // a result node's pins point into it
        if (!Terminator)
        {
            OutErrorCode = EUnrealMCPGraphError::InvalidParams;
            OutErrorMessage = TEXT("Could not create the function's result node");
            return false;
        }
    }

    if (HasUserDefinedParamPin(Terminator, FName(*Request.Name)))
    {
        TArray<TSharedPtr<FJsonObject>> Params;
        DescribeFunctionParams(Graph, Params);
        for (const TSharedPtr<FJsonObject>& Param : Params)
        {
            OutCandidates.Add(Param->GetStringField(TEXT("name")));
        }
        OutErrorCode = EUnrealMCPGraphError::InvalidParams;
        OutErrorMessage = FString::Printf(TEXT("A parameter named '%s' already exists"), *Request.Name);
        return false;
    }

    FText CannotCreate;
    if (!Terminator->CanCreateUserDefinedPin(PinType, Direction, CannotCreate))
    {
        OutErrorCode = EUnrealMCPGraphError::UnsupportedPinType;
        OutErrorMessage = CannotCreate.ToString();
        return false;
    }

    UEdGraphPin* NewPin = Terminator->CreateUserDefinedPin(FName(*Request.Name), PinType, Direction);
    if (!NewPin)
    {
        OutErrorCode = EUnrealMCPGraphError::InvalidParams;
        OutErrorMessage = FString::Printf(TEXT("Failed to create parameter '%s'"), *Request.Name);
        return false;
    }

    if (!Request.DefaultValue.IsEmpty())
    {
        GetDefault<UEdGraphSchema_K2>()->TrySetDefaultValue(*NewPin, Request.DefaultValue);
    }

    Terminator->ReconstructNode();
    return true;
}

bool FUnrealMCPBlueprintGraphOps::RemoveFunctionParam(UBlueprint* Blueprint, UEdGraph* Graph,
                                                      const FString& ParamName, bool& bOutWasOutput,
                                                      FString& OutErrorCode, FString& OutErrorMessage)
{
    OutErrorCode.Reset();
    OutErrorMessage.Reset();
    bOutWasOutput = false;

    UK2Node_FunctionEntry* Entry = FindFunctionEntryNode(Graph);
    if (!Blueprint || !Graph || !Entry)
    {
        OutErrorCode = EUnrealMCPGraphError::FunctionGraphNotFound;
        OutErrorMessage = TEXT("The named graph is not a function graph");
        return false;
    }

    UK2Node_EditablePinBase* Terminator = FindFunctionTerminatorForParam(Graph, FName(*ParamName), bOutWasOutput);
    if (!Terminator)
    {
        OutErrorCode = EUnrealMCPGraphError::ParamNotFound;
        OutErrorMessage = FString::Printf(TEXT("No parameter or output named '%s'"), *ParamName);
        return false;
    }
    if (!Terminator->IsEditable())
    {
        OutErrorCode = EUnrealMCPGraphError::FunctionNotEditable;
        OutErrorMessage = FString::Printf(
            TEXT("Function '%s' overrides a parent signature; its parameters cannot be edited"), *Graph->GetName());
        return false;
    }

    Terminator->RemoveUserDefinedPinByName(FName(*ParamName));
    Terminator->ReconstructNode();
    return true;
}

bool FUnrealMCPBlueprintGraphOps::RenameFunctionParam(UBlueprint* Blueprint, UEdGraph* Graph,
                                                      const FString& OldName, const FString& NewName,
                                                      FString& OutErrorCode, FString& OutErrorMessage)
{
    OutErrorCode.Reset();
    OutErrorMessage.Reset();

    UK2Node_FunctionEntry* Entry = FindFunctionEntryNode(Graph);
    if (!Blueprint || !Graph || !Entry)
    {
        OutErrorCode = EUnrealMCPGraphError::FunctionGraphNotFound;
        OutErrorMessage = TEXT("The named graph is not a function graph");
        return false;
    }
    if (!Entry->IsEditable())
    {
        OutErrorCode = EUnrealMCPGraphError::FunctionNotEditable;
        OutErrorMessage = FString::Printf(
            TEXT("Function '%s' overrides a parent signature; its parameters cannot be edited"), *Graph->GetName());
        return false;
    }

    bool bIsOutput = false;
    UK2Node_EditablePinBase* Terminator = FindFunctionTerminatorForParam(Graph, FName(*OldName), bIsOutput);
    if (!Terminator)
    {
        OutErrorCode = EUnrealMCPGraphError::ParamNotFound;
        OutErrorMessage = FString::Printf(TEXT("No parameter or output named '%s'"), *OldName);
        return false;
    }

    // Renaming a parameter is NOT just a pin rename: the terminal nodes' UserDefinedPins records drive the
    // signature and ReconstructNode rebuilds the pins from them, so renaming only the pin makes the name
    // bounce back. This mirrors the editor's own "Rename Parameter" action
    // (BlueprintDetailsCustomization.cpp OnPinRenamed): Modify -> test -> rename -> update UserDefinedPins ->
    // fix the getter nodes that reference the parameter by name.
    TArray<UK2Node_EditablePinBase*> Terminals;
    Terminals.Add(Entry);
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node))
        {
            Terminals.Add(Result);
        }
    }

    const FName OldFName(*OldName);
    const FName NewFName(*NewName);
    for (UK2Node_EditablePinBase* Terminal : Terminals)
    {
        Terminal->Modify();
        if (Terminal->RenameUserDefinedPin(OldFName, NewFName, /*bTest=*/true) ==
            ERenamePinResult::ERenamePinResult_NameCollision)
        {
            OutErrorCode = EUnrealMCPGraphError::GraphNameInUse;
            OutErrorMessage = FString::Printf(TEXT("'%s' collides with an existing parameter"), *NewName);
            return false;
        }
    }
    for (UK2Node_EditablePinBase* Terminal : Terminals)
    {
        Terminal->RenameUserDefinedPin(OldFName, NewFName, /*bTest=*/false);
        if (TSharedPtr<FUserPinInfo>* PinInfo = Terminal->UserDefinedPins.FindByPredicate(
                [&OldFName](const TSharedPtr<FUserPinInfo>& Pin)
                {
                    return Pin.IsValid() && Pin->PinName == OldFName;
                }))
        {
            (*PinInfo)->PinName = NewFName;
        }
        Terminal->ReconstructNode();
    }

    // A function input becomes a getter node inside the graph, and those reference the parameter by name.
    TArray<UK2Node_VariableGet*> GetterNodes;
    Graph->GetNodesOfClass<UK2Node_VariableGet>(GetterNodes);
    for (UK2Node_VariableGet* GetterNode : GetterNodes)
    {
        if (GetterNode && GetterNode->ReferencesVariable(OldFName, nullptr))
        {
            GetterNode->HandleVariableRenamed(Blueprint, Blueprint->GeneratedClass, Graph, OldFName, NewFName);
        }
    }
    return true;
}

bool FUnrealMCPBlueprintGraphOps::RenameFunctionGraph(UBlueprint* Blueprint, const FString& OldName,
                                                      const FString& NewName, FString& OutErrorCode,
                                                      FString& OutErrorMessage)
{
    OutErrorCode.Reset();
    OutErrorMessage.Reset();

    if (!Blueprint)
    {
        OutErrorCode = EUnrealMCPGraphError::BlueprintNotFound;
        OutErrorMessage = TEXT("Invalid blueprint");
        return false;
    }

    UEdGraph* Graph = nullptr;
    TArray<UEdGraph*> FunctionGraphs;
    ListFunctionGraphs(Blueprint, FunctionGraphs);
    for (UEdGraph* Candidate : FunctionGraphs)
    {
        if (Candidate->GetName() == OldName)
        {
            Graph = Candidate;
            break;
        }
    }
    if (!Graph)
    {
        OutErrorCode = EUnrealMCPGraphError::FunctionGraphNotFound;
        OutErrorMessage = FString::Printf(TEXT("No function graph named '%s'"), *OldName);
        return false;
    }
    if (Graph->GetName() == NewName)
    {
        return true;    // idempotent: already the requested name
    }

    // The engine's own validator would silently suffix a colliding name, hiding the caller's mistake.
    for (UEdGraph* Other : Blueprint->UbergraphPages)
    {
        if (Other && Other->GetName() == NewName)
        {
            OutErrorCode = EUnrealMCPGraphError::GraphNameInUse;
            OutErrorMessage = FString::Printf(TEXT("A graph named '%s' already exists"), *NewName);
            return false;
        }
    }
    for (UEdGraph* Other : FunctionGraphs)
    {
        if (Other && Other->GetName() == NewName)
        {
            OutErrorCode = EUnrealMCPGraphError::GraphNameInUse;
            OutErrorMessage = FString::Printf(TEXT("A function named '%s' already exists"), *NewName);
            return false;
        }
    }

    FBlueprintEditorUtils::RenameGraph(Graph, NewName);
    return true;
}

void FUnrealMCPBlueprintGraphOps::ListFunctionGraphs(UBlueprint* Blueprint, TArray<UEdGraph*>& OutGraphs)
{
    OutGraphs.Reset();
    if (!Blueprint)
    {
        return;
    }
    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        if (Graph)
        {
            OutGraphs.Add(Graph);
        }
    }
}

bool FUnrealMCPBlueprintGraphOps::RemoveFunctionGraph(UBlueprint* Blueprint, const FString& FunctionName,
                                                      FString& OutErrorCode, FString& OutErrorMessage,
                                                      TArray<FString>& OutCandidates)
{
    OutErrorCode.Reset();
    OutErrorMessage.Reset();
    OutCandidates.Reset();

    if (!Blueprint)
    {
        OutErrorCode = EUnrealMCPGraphError::BlueprintNotFound;
        OutErrorMessage = TEXT("Invalid blueprint");
        return false;
    }
    if (FunctionName.IsEmpty())
    {
        OutErrorCode = EUnrealMCPGraphError::InvalidParams;
        OutErrorMessage = TEXT("Function name must not be empty");
        return false;
    }

    // The construction script is a FunctionGraph but the engine owns it: removing it would break the
    // blueprint's component setup, so it is refused with an explicit code instead of silently skipping.
    const FString OwnedName = FName(*FunctionName).ToString();
    if (OwnedName.Equals(UEdGraphSchema_K2::FN_UserConstructionScript.ToString(), ESearchCase::IgnoreCase))
    {
        OutErrorCode = EUnrealMCPGraphError::GraphNotRemovable;
        OutErrorMessage = FString::Printf(TEXT("'%s' is engine-managed and cannot be removed"), *FunctionName);
        return false;
    }

    TArray<UEdGraph*> FunctionGraphs;
    ListFunctionGraphs(Blueprint, FunctionGraphs);
    for (UEdGraph* Graph : FunctionGraphs)
    {
        if (Graph->GetName() == FunctionName)
        {
            FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph, EGraphRemoveFlags::Recompile);
            return true;
        }
        OutCandidates.Add(Graph->GetName());
    }

    OutErrorCode = EUnrealMCPGraphError::FunctionGraphNotFound;
    OutErrorMessage = FString::Printf(TEXT("No function graph named '%s'"), *FunctionName);
    return false;
}


// ---------------------------------------------------------------------------
// Node lookup
// ---------------------------------------------------------------------------

UEdGraphNode* FUnrealMCPBlueprintGraphOps::FindNodeByGuid(const UEdGraph* Graph, const FString& NodeGuid)
{
    if (!Graph || NodeGuid.IsEmpty())
    {
        return nullptr;
    }

    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node && Node->NodeGuid.ToString() == NodeGuid)
        {
            return Node;
        }
    }
    return nullptr;
}

UEdGraphPin* FUnrealMCPBlueprintGraphOps::FindPin(UEdGraphNode* Node, const FString& PinName,
                                                  EEdGraphPinDirection Direction)
{
    if (!Node)
    {
        return nullptr;
    }

    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->PinName.ToString() == PinName && (Direction == EGPD_MAX || Pin->Direction == Direction))
        {
            return Pin;
        }
    }

    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase) &&
            (Direction == EGPD_MAX || Pin->Direction == Direction))
        {
            return Pin;
        }
    }

    // A component/self reference exposes its value through an unnamed data output.
    if (Direction == EGPD_Output && Cast<UK2Node_VariableGet>(Node) != nullptr)
    {
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
            {
                return Pin;
            }
        }
    }

    return nullptr;
}

void FUnrealMCPBlueprintGraphOps::CollectPinNames(const UEdGraphNode* Node, EEdGraphPinDirection Direction,
                                                  TArray<FString>& OutNames)
{
    OutNames.Reset();
    if (!Node)
    {
        return;
    }

    for (const UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && (Direction == EGPD_MAX || Pin->Direction == Direction))
        {
            OutNames.Add(Pin->PinName.ToString());
        }
    }
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

void FUnrealMCPBlueprintGraphOps::MakePinInfo(const UEdGraphPin* Pin, FUnrealMCPPinInfo& OutInfo)
{
    OutInfo = FUnrealMCPPinInfo();
    if (!Pin)
    {
        return;
    }

    OutInfo.PinName = Pin->PinName.ToString();
    OutInfo.Direction = Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output");
    OutInfo.Category = DescribePinType(Pin);
    OutInfo.DefaultValue = Pin->DefaultValue;
    if (OutInfo.DefaultValue.IsEmpty() && Pin->DefaultObject)
    {
        // An object/class pin carries its reference in DefaultObject (the engine rejects a string
        // there), but callers read the path from default_value - so report it here too.
        OutInfo.DefaultValue = Pin->DefaultObject->GetPathName();
    }

    if (Pin->DefaultObject)
    {
        OutInfo.DefaultObject = Pin->DefaultObject->GetPathName();
    }
    else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class && Pin->DefaultValue.IsEmpty() == false)
    {
        OutInfo.DefaultObject = Pin->DefaultValue;
    }

    for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
    {
        if (LinkedPin && LinkedPin->GetOwningNode())
        {
            OutInfo.LinkedTo.Add(FString::Printf(TEXT("%s.%s"),
                *LinkedPin->GetOwningNode()->NodeGuid.ToString(), *LinkedPin->PinName.ToString()));
        }
    }

    OutInfo.Connected = OutInfo.LinkedTo.Num() > 0;
    OutInfo.DefaultTextValue = Pin->DefaultTextValue.ToString();
    OutInfo.bHidden = Pin->bHidden;
    OutInfo.bNotConnectable = Pin->bNotConnectable;
}

void FUnrealMCPBlueprintGraphOps::MakeNodeInfo(UEdGraphNode* Node, FUnrealMCPNodeInfo& OutInfo)
{
    OutInfo = FUnrealMCPNodeInfo();
    if (!Node)
    {
        return;
    }

    OutInfo.NodeId = Node->NodeGuid.ToString();
    OutInfo.Name = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
    OutInfo.Type = Node->GetClass()->GetName();
    OutInfo.PosX = Node->NodePosX;
    OutInfo.PosY = Node->NodePosY;
    OutInfo.GraphName = Node->GetGraph() ? Node->GetGraph()->GetName() : FString();
    OutInfo.Node = Node;

    CollectNodePropertyInfos(Node, OutInfo.Properties);

    for (const UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin)
        {
            FUnrealMCPPinInfo PinInfo;
            MakePinInfo(Pin, PinInfo);
            OutInfo.Pins.Add(MoveTemp(PinInfo));
        }
    }
}

void FUnrealMCPBlueprintGraphOps::MakeGraphInfo(UBlueprint* Blueprint, UEdGraph* Graph, FUnrealMCPGraphInfo& OutInfo)
{
    OutInfo = FUnrealMCPGraphInfo();
    if (!Graph)
    {
        return;
    }

    OutInfo.GraphName = Graph->GetName();
    OutInfo.GraphClass = Graph->GetClass()->GetName();
    OutInfo.NodeCount = Graph->Nodes.Num();
    OutInfo.IsEditable = !IsMacroGraph(Blueprint, Graph);
    OutInfo.Graph = Graph;
}

// ---------------------------------------------------------------------------
// Node properties
// ---------------------------------------------------------------------------

void FUnrealMCPBlueprintGraphOps::CollectEditableNodeProperties(UEdGraphNode* Node, TArray<FProperty*>& OutProperties)
{
    OutProperties.Reset();
    if (!Node)
    {
        return;
    }

    for (TFieldIterator<FProperty> It(Node->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
    {
        FProperty* Property = *It;
        if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Transient))
        {
            continue;
        }

        const FString PropertyName = Property->GetName();

        // Bookkeeping / diagnostics / comment plumbing: already reported through dedicated fields,
        // and not useful as a "node property" to write back.
        if (PropertyName == TEXT("NodeGuid") || PropertyName == TEXT("NodePosX") ||
            PropertyName == TEXT("NodePosY") || PropertyName == TEXT("NodeComment") ||
            PropertyName == TEXT("ErrorType") || PropertyName == TEXT("ErrorMsg") ||
            PropertyName == TEXT("AdvancedPinDisplay") || PropertyName.StartsWith(TEXT("bComment")))
        {
            continue;
        }

        OutProperties.Add(Property);
    }
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintGraphOps::MakeNodePropertiesJson(UEdGraphNode* Node)
{
    TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();

    TArray<FProperty*> Properties;
    CollectEditableNodeProperties(Node, Properties);
    for (FProperty* Property : Properties)
    {
        // One converter for every property type (numbers stay numbers, structs become arrays,
        // enums become member names, objects become asset paths).
        PropertiesObject->SetField(Property->GetName(),
            FUnrealMCPCommonUtils::PropertyValueToJson(Property, Property->ContainerPtrToValuePtr<void>(Node)));
    }
    return PropertiesObject;
}

FString FUnrealMCPBlueprintGraphOps::JsonValueToText(const TSharedPtr<FJsonValue>& Value)
{
    if (!Value.IsValid())
    {
        return FString();
    }

    switch (Value->Type)
    {
    case EJson::Number:
        return FString::Printf(TEXT("%.6g"), Value->AsNumber());

    case EJson::Boolean:
        return Value->AsBool() ? TEXT("true") : TEXT("false");

    case EJson::Array:
    {
        TArray<FString> Items;
        const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
        if (Value->TryGetArray(Array) && Array)
        {
            for (const TSharedPtr<FJsonValue>& Item : *Array)
            {
                Items.Add(JsonValueToText(Item));
            }
        }
        return FString::Printf(TEXT("(%s)"), *FString::Join(Items, TEXT(",")));
    }

    case EJson::Object:
    {
        TArray<FString> Items;
        const TSharedPtr<FJsonObject> Object = Value->AsObject();
        if (Object.IsValid())
        {
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
            {
                Items.Add(FString::Printf(TEXT("%s=%s"), *Pair.Key, *JsonValueToText(Pair.Value)));
            }
        }
        return FString::Printf(TEXT("(%s)"), *FString::Join(Items, TEXT(",")));
    }

    default:
        return Value->AsString();
    }
}

void FUnrealMCPBlueprintGraphOps::CollectNodePropertyInfos(UEdGraphNode* Node,
                                                           TArray<FUnrealMCPNodePropertyInfo>& OutProperties)
{
    OutProperties.Reset();

    TArray<FProperty*> Properties;
    CollectEditableNodeProperties(Node, Properties);

    for (FProperty* Property : Properties)
    {
        FUnrealMCPNodePropertyInfo Info;
        Info.Name = Property->GetName();
        Info.Type = Property->GetCPPType();
        Info.Value = JsonValueToText(
            FUnrealMCPCommonUtils::PropertyValueToJson(Property, Property->ContainerPtrToValuePtr<void>(Node)));
        OutProperties.Add(MoveTemp(Info));
    }
}

bool FUnrealMCPBlueprintGraphOps::PropertyAffectsPinLayout(const FProperty* Property)
{
    if (!Property)
    {
        return false;
    }

    // Pin names can be driven by a container (UK2Node_SwitchString::PinNames) and pin sets can be
    // driven by an enum (a Cast / Switch node targets a UEnum through an object property). Everything
    // else a node exposes - positions, comments, colours, fonts, enabled state - is visual. The decision
    // comes from the property TYPE rather than from a list of property names, so a new node class is
    // handled correctly without editing this function.
    if (Property->IsA<FArrayProperty>() || Property->IsA<FSetProperty>() || Property->IsA<FMapProperty>())
    {
        return true;
    }
    if (Property->IsA<FEnumProperty>())
    {
        return true;
    }
    if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
    {
        return ByteProperty->GetIntPropertyEnum() != nullptr;
    }
    if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
    {
        return ObjectProperty->PropertyClass && ObjectProperty->PropertyClass->IsChildOf(UEnum::StaticClass());
    }
    return false;
}

bool FUnrealMCPBlueprintGraphOps::SetNodeProperty(UEdGraphNode* Node, const FString& PropertyName,
                                                  const TSharedPtr<FJsonValue>& Value,
                                                  FUnrealMCPNodePropertyWriteResult& OutResult)
{
    OutResult = FUnrealMCPNodePropertyWriteResult();

    if (!Node || !Value.IsValid())
    {
        OutResult.Write = FWriteResult::Failure(EUnrealMCPGraphError::InvalidParams, TEXT("Invalid node or value"));
        return false;
    }

    // A dotted name walks into the node's own struct / object properties ("node.chain_end").
    // AnimGraph nodes keep their whole FAnimNode in one UPROPERTY, so the field a caller actually
    // wants sits one level below anything a flat lookup can reach - and before this, that field was
    // simply unreachable (the caller had to write python to do the read-modify-write by hand).
    TArray<FString> Segments;
    PropertyName.ParseIntoArray(Segments, TEXT("."), /*InCullEmpty=*/ true);
    if (Segments.Num() == 0)
    {
        OutResult.Write = FWriteResult::Failure(TEXT("invalid_params"),
            FString::Printf(TEXT("Property name '%s' has no usable path segments"), *PropertyName));
        return false;
    }

    FProperty* Property = Node->GetClass()
        ? FindFProperty<FProperty>(Node->GetClass(), FName(*Segments[0]))
        : nullptr;

    if (!Property)
    {
        FUnrealMCPCommonUtils::FindPropertyNameSuggestions(Node, Segments[0], OutResult.Candidates);
        if (OutResult.Candidates.Num() == 0)
        {
            for (TFieldIterator<FProperty> It(Node->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
            {
                if (It->HasAnyPropertyFlags(CPF_Edit))
                {
                    OutResult.Candidates.Add(It->GetName());
                }
            }
        }

        OutResult.Write = FWriteResult::Failure(TEXT("unknown_property"),
            FString::Printf(TEXT("Property '%s' not found on node '%s'%s"),
                *Segments[0], *Node->GetClass()->GetName(),
                OutResult.Candidates.Num() > 0
                    ? *FString::Printf(TEXT(". Did you mean: %s"), *FString::Join(OutResult.Candidates, TEXT(", ")))
                    : TEXT("")));
        return false;
    }

    // The address keeps being re-derived as the walk descends, so the final write lands on the leaf
    // and not on the struct that contains it.
    void* ValueAddress = Property->ContainerPtrToValuePtr<void>(Node);
    FString ResolvedPath = Property->GetName();

    for (int32 SegmentIndex = 1; SegmentIndex < Segments.Num(); ++SegmentIndex)
    {
        const FString& Segment = Segments[SegmentIndex];

        if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
        {
            FProperty* Field = StructProperty->Struct
                ? StructProperty->Struct->FindPropertyByName(FName(*Segment))
                : nullptr;
            if (!Field)
            {
                // Name the fields this level actually has: "the property lives in a memory struct"
                // is the fact the old `unknown_property` error hid, and it is not guessable.
                if (StructProperty->Struct)
                {
                    for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
                    {
                        OutResult.AvailablePathSegments.Add(It->GetName());
                    }
                }
                OutResult.Write = FWriteResult::Failure(TEXT("path_segment_not_found"),
                    FString::Printf(TEXT("'%s' is not a field of struct '%s' (reached at '%s')"),
                        *Segment, *StructProperty->Struct->GetName(), *ResolvedPath))
                    .WithHint(FString::Printf(
                        TEXT("'%s' is a struct held in memory by this node; retry with '%s.<field>' using available_path_segments"),
                        *ResolvedPath, *ResolvedPath));
                return false;
            }
            Property = Field;
            ValueAddress = Field->ContainerPtrToValuePtr<void>(ValueAddress);
            ResolvedPath += TEXT(".") + Field->GetName();
            continue;
        }

        if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
        {
            UObject* TargetObject = ObjectProperty->GetObjectPropertyValue(ValueAddress);
            if (!TargetObject)
            {
                OutResult.Write = FWriteResult::Failure(TEXT("path_target_missing"),
                    FString::Printf(TEXT("'%s' is an object property that holds nothing, so '%s' cannot be reached"),
                        *ResolvedPath, *Segment));
                return false;
            }
            FProperty* Field = FindFProperty<FProperty>(TargetObject->GetClass(), FName(*Segment));
            if (!Field)
            {
                for (TFieldIterator<FProperty> It(TargetObject->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
                {
                    OutResult.AvailablePathSegments.Add(It->GetName());
                }
                OutResult.Write = FWriteResult::Failure(TEXT("path_segment_not_found"),
                    FString::Printf(TEXT("'%s' is not a property of '%s' (reached at '%s')"),
                        *Segment, *TargetObject->GetClass()->GetName(), *ResolvedPath));
                return false;
            }
            Property = Field;
            ValueAddress = Field->ContainerPtrToValuePtr<void>(TargetObject);
            ResolvedPath += TEXT(".") + Field->GetName();
            continue;
        }

        // Containers are refused by name: walking "array.0.field" is a different feature, and
        // silently writing the container instead would be the exact failure mode this fixes.
        OutResult.Write = FWriteResult::Failure(TEXT("path_not_walkable"),
            FString::Printf(TEXT("'%s' is a %s; only struct and object properties can be walked with '.'"),
                *ResolvedPath, *Property->GetClass()->GetName()));
        return false;
    }

    OutResult.PropertyPath = ResolvedPath;

    // One write path for node properties too: the reflector owns the type dispatch, the value address and
    // the error code, so a node property accepts whatever any other property accepts.
    const FPropertyDescriptor Descriptor = FMCPPropertyReflector::Describe(Property);
    OutResult.PropertyType = Descriptor.CppType;

    OutResult.PropertyTypeDetail = MakeShared<FJsonObject>();
    OutResult.PropertyTypeDetail->SetStringField(TEXT("cpp_type"), Descriptor.CppType);
    OutResult.PropertyTypeDetail->SetStringField(TEXT("property_class"), Descriptor.PropertyClass);
    OutResult.PropertyTypeDetail->SetBoolField(TEXT("supported"), Descriptor.bSupported);
    if (!Descriptor.Container.IsEmpty())
    {
        OutResult.PropertyTypeDetail->SetStringField(TEXT("container"), Descriptor.Container);
        OutResult.PropertyTypeDetail->SetStringField(TEXT("semantics"), Descriptor.Semantics);
    }
    if (!Descriptor.ElementType.IsEmpty())
    {
        OutResult.PropertyTypeDetail->SetStringField(TEXT("element_type"), Descriptor.ElementType);
    }
    if (!Descriptor.ValueType.IsEmpty())
    {
        OutResult.PropertyTypeDetail->SetStringField(TEXT("value_type"), Descriptor.ValueType);
    }
    if (!Descriptor.Hint.IsEmpty())
    {
        OutResult.PropertyTypeDetail->SetStringField(TEXT("hint"), Descriptor.Hint);
    }
    TArray<TSharedPtr<FJsonValue>> ShapeValues;
    for (const FString& Shape : Descriptor.SupportedShapes)
    {
        ShapeValues.Add(MakeShared<FJsonValueString>(Shape));
    }
    OutResult.PropertyTypeDetail->SetArrayField(TEXT("supported_shapes"), ShapeValues);

    OutResult.ValueBefore = FUnrealMCPCommonUtils::PropertyValueToJson(Property, ValueAddress);

    // The leaf property and its address (inside whatever struct/object the path walked through): the
    // write is in-place, so to the caller it is a read-modify-write of the outer struct done for them.
    OutResult.Write = FMCPPropertyReflector::FromJson(Property, ValueAddress, ResolvedPath, Value);
    if (!OutResult.Write.bSuccess)
    {
        OutResult.Write.ErrorMessage = FString::Printf(TEXT("Failed to write property '%s' on node '%s': %s"),
            *ResolvedPath, *Node->GetClass()->GetName(), *OutResult.Write.ErrorMessage);
        return false;
    }

    OutResult.ValueAfter = FUnrealMCPCommonUtils::PropertyValueToJson(Property, ValueAddress);

    // Writing an enum or container property leaves the pins stale until the node rebuilds them; writing a
    // visual property must not disturb the node, so the decision comes from the property's type.
    if (PropertyAffectsPinLayout(Property))
    {
        Node->ReconstructNode();
        OutResult.bPinsRebuilt = true;
    }

    if (UEdGraph* Graph = Node->GetGraph())
    {
        MarkModified(Graph);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Node creation
// ---------------------------------------------------------------------------

/**
 * The UK2Node_CallFunction subclass the editor's own function spawner picks for a function
 * (BlueprintFunctionNodeSpawner.cpp:208-247).
 *
 * This matters: Array_Length / Array_Get / Array_Add and friends carry ArrayParm metadata, and the
 * wildcard behaviour of their array pin lives in UK2Node_CallArrayFunction (AllocateDefaultPins forces
 * the pin to a wildcard, NotifyPinConnectionListChanged copies the type over from whatever gets linked,
 * PostReconstructNode re-derives it). A plain UK2Node_CallFunction never does any of that, so the pin
 * stays an untyped wildcard and the blueprint fails to compile with "Target Array type not determined"
 * - which is exactly what happened to the Array_Length nodes of BP_TPSDemoGameMode before this existed.
 */
static TSubclassOf<UK2Node_CallFunction> CallFunctionNodeClassFor(const UFunction* Function)
{
    if (!Function)
    {
        return UK2Node_CallFunction::StaticClass();
    }

    const bool bIsPure = Function->HasAllFunctionFlags(FUNC_BlueprintPure);
    if (bIsPure && Function->HasMetaData(FBlueprintMetadata::MD_CommutativeAssociativeBinaryOperator))
    {
        return UK2Node_CommutativeAssociativeBinaryOperator::StaticClass();
    }
    if (Function->HasMetaData(FBlueprintMetadata::MD_MaterialParameterCollectionFunction))
    {
        return UK2Node_CallMaterialParameterCollectionFunction::StaticClass();
    }
    if (Function->HasMetaData(FBlueprintMetadata::MD_DataTablePin))
    {
        return UK2Node_CallDataTableFunction::StaticClass();
    }
    if (Function->HasMetaData(FBlueprintMetadata::MD_ArrayParam))
    {
        return UK2Node_CallArrayFunction::StaticClass();
    }
    return UK2Node_CallFunction::StaticClass();
}

UEdGraphNode* FUnrealMCPBlueprintGraphOps::CreateGraphNodeByClass(UEdGraph* Graph, UClass* NodeClass,
                                                                  const FVector2D& Position,
                                                                  TFunctionRef<void(UEdGraphNode*)> Configure)
{
    if (!Graph || !NodeClass)
    {
        return nullptr;
    }

    UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, NodeClass);
    if (!Node)
    {
        return nullptr;
    }

    // Configure runs before the pins exist, like the typed template above.
    Configure(Node);

    Node->NodePosX = static_cast<int32>(Position.X);
    Node->NodePosY = static_cast<int32>(Position.Y);
    Node->CreateNewGuid();
    Node->PostPlacedNewNode();
    Node->AllocateDefaultPins();
    Graph->AddNode(Node, /*bFromUI=*/false, /*bSelectNewNode=*/false);
    Node->ReconstructNode();

    MarkModified(Graph);
    return Node;
}

bool FUnrealMCPBlueprintGraphOps::ResolveClass(const FString& ClassName, UClass*& OutClass,
                                               TArray<FString>& OutCandidates)
{
    OutClass = nullptr;
    OutCandidates.Reset();

    if (ClassName.IsEmpty())
    {
        return false;
    }

    // One resolver for the whole plugin: full path first, then the global short-name lookup. The old
    // body spelled the short-name lookup as FindObject<UClass>(ANY_PACKAGE, ...), which on 5.7 is a
    // null outer - and a null outer only matches TOP-LEVEL packages (UObjectHash.cpp:1118), so no class
    // ever resolved and the module whitelist below was papering over the failure. FindFirstObject
    // (UObjectHash.cpp:1226) hashes on the object name alone, so it searches every package.
    TArray<FString> TriedForms;
    FString ResolvedPath;
    OutClass = FUnrealMCPCommonUtils::ResolveUClass(ClassName, TriedForms, ResolvedPath);

    // A caller who writes the C++ spelling of a class ("UK2Node_Event") names an object that does not
    // exist: the UClass object itself carries no prefix. ResolveUClass adds the "U"/"A" spellings but
    // never strips one, so that form is retried here.
    if (!OutClass && ClassName.Len() > 1 &&
        (ClassName.StartsWith(TEXT("U")) || ClassName.StartsWith(TEXT("A"))))
    {
        OutClass = FUnrealMCPCommonUtils::ResolveUClass(ClassName.RightChop(1), TriedForms, ResolvedPath);
    }

    if (OutClass)
    {
        return true;
    }

    // Report near misses instead of a bare failure.
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* CandidateClass = *It;
        if (CandidateClass && CandidateClass->GetName().Contains(ClassName))
        {
            OutCandidates.Add(CandidateClass->GetName());
            if (OutCandidates.Num() >= 15)
            {
                break;
            }
        }
    }
    return false;
}

bool FUnrealMCPBlueprintGraphOps::ResolveNodeClass(const FString& NodeClass, UClass*& OutClass,
                                                   TArray<FString>& OutCandidates)
{
    OutCandidates.Reset();

    if (ResolveClass(NodeClass, OutClass, OutCandidates) &&
        OutClass && OutClass->IsChildOf(UEdGraphNode::StaticClass()))
    {
        return true;
    }

    OutClass = nullptr;
    OutCandidates.Reset();
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* CandidateClass = *It;
        if (CandidateClass && CandidateClass->IsChildOf(UEdGraphNode::StaticClass()) &&
            CandidateClass->GetName().Contains(NodeClass))
        {
            OutCandidates.Add(CandidateClass->GetName());
            if (OutCandidates.Num() >= 15)
            {
                break;
            }
        }
    }
    return false;
}

bool FUnrealMCPBlueprintGraphOps::ResolveFunction(UClass* TargetClass, const FString& FunctionName,
                                                  UFunction*& OutFunction, TArray<FString>& OutCandidates)
{
    OutFunction = nullptr;
    OutCandidates.Reset();

    if (!TargetClass || FunctionName.IsEmpty())
    {
        return false;
    }

    for (UClass* Current = TargetClass; Current; Current = Current->GetSuperClass())
    {
        if (UFunction* Found = Current->FindFunctionByName(*FunctionName))
        {
            OutFunction = Found;
            return true;
        }
    }

    for (UClass* Current = TargetClass; Current && OutFunction == nullptr; Current = Current->GetSuperClass())
    {
        for (TFieldIterator<UFunction> It(Current); It; ++It)
        {
            UFunction* Candidate = *It;
            if (Candidate && Candidate->GetName().Equals(FunctionName, ESearchCase::IgnoreCase))
            {
                OutFunction = Candidate;
                return true;
            }
        }
    }

    for (TFieldIterator<UFunction> It(TargetClass); It; ++It)
    {
        UFunction* Candidate = *It;
        if (Candidate && Candidate->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure) &&
            Candidate->GetName().Contains(FunctionName))
        {
            OutCandidates.Add(Candidate->GetName());
            if (OutCandidates.Num() >= 15)
            {
                break;
            }
        }
    }
    return false;
}

bool FUnrealMCPBlueprintGraphOps::CreateFunctionCallNode(UEdGraph* Graph, const FString& TargetClass,
                                                         const FString& FunctionName, const FVector2D& Position,
                                                         UK2Node_CallFunction*& OutNode, FString& OutErrorCode,
                                                         FString& OutErrorMessage, TArray<FString>& OutCandidates)
{
    OutNode = nullptr;
    OutErrorCode.Reset();
    OutErrorMessage.Reset();
    OutCandidates.Reset();

    if (!Graph)
    {
        OutErrorCode = EUnrealMCPGraphError::InvalidParams;
        OutErrorMessage = TEXT("Invalid graph");
        return false;
    }

    UClass* ResolvedTargetClass = nullptr;
    if (TargetClass.IsEmpty())
    {
        // No target: the function must live on the blueprint's own class.
        UBlueprint* Blueprint = FindBlueprintForGraph(Graph);
        ResolvedTargetClass = Blueprint ? Blueprint->GeneratedClass : nullptr;
        if (!ResolvedTargetClass)
        {
            OutErrorCode = EUnrealMCPGraphError::BlueprintNotFound;
            OutErrorMessage = TEXT("Cannot resolve the blueprint that owns this graph");
            return false;
        }
    }
    else if (!ResolveClass(TargetClass, ResolvedTargetClass, OutCandidates))
    {
        OutErrorCode = TEXT("class_not_found");
        OutErrorMessage = FString::Printf(TEXT("Target class '%s' could not be resolved"), *TargetClass);
        return false;
    }

    UFunction* Function = nullptr;
    if (!ResolveFunction(ResolvedTargetClass, FunctionName, Function, OutCandidates))
    {
        OutErrorCode = EUnrealMCPGraphError::FunctionNotFound;
        OutErrorMessage = FString::Printf(TEXT("Function '%s' not found in class '%s'%s"),
            *FunctionName, *ResolvedTargetClass->GetName(),
            OutCandidates.Num() > 0 ? *FString::Printf(TEXT(". Similar functions: %s"), *FString::Join(OutCandidates, TEXT(", "))) : TEXT(""));
        return false;
    }

    OutNode = Cast<UK2Node_CallFunction>(CreateGraphNodeByClass(
        Graph, CallFunctionNodeClassFor(Function), Position,
        [Function](UEdGraphNode* Node)
        {
            if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
            {
                CallNode->SetFromFunction(Function);
            }
        }));

    if (!OutNode)
    {
        OutErrorCode = EUnrealMCPGraphError::InvalidParams;
        OutErrorMessage = TEXT("Failed to create the function call node");
        return false;
    }
    return true;
}

TArray<FString> FUnrealMCPBlueprintGraphOps::ListImplementableEventNames(const UBlueprint* Blueprint)
{
    TArray<FString> Names;
    UClass* Parent = Blueprint ? Blueprint->ParentClass.Get() : nullptr;
    if (!Parent)
    {
        return Names;
    }
    for (TFieldIterator<UFunction> It(Parent, EFieldIteratorFlags::IncludeSuper); It; ++It)
    {
        const UFunction* Function = *It;
        // Event semantics: a BlueprintEvent without a return value (those with one are overridable
        // functions, built through add_blueprint_function_graph instead).
        if (Function && Function->HasAnyFunctionFlags(FUNC_BlueprintEvent)
            && Function->GetReturnProperty() == nullptr
            && !Function->HasMetaData(TEXT("BlueprintInternalUseOnly")))
        {
            Names.AddUnique(Function->GetName());
        }
    }
    Names.Sort();
    return Names;
}

bool FUnrealMCPBlueprintGraphOps::CreateEventNode(UEdGraph* Graph, const FString& EventName,
                                                  const FVector2D& Position, UK2Node_Event*& OutNode,
                                                  FString& OutErrorCode, FString& OutErrorMessage)
{
    OutNode = nullptr;
    OutErrorCode.Reset();
    OutErrorMessage.Reset();

    if (!Graph)
    {
        OutErrorCode = EUnrealMCPGraphError::InvalidParams;
        OutErrorMessage = TEXT("Invalid graph");
        return false;
    }

    UBlueprint* Blueprint = FindBlueprintForGraph(Graph);
    if (!Blueprint || !Blueprint->GeneratedClass)
    {
        OutErrorCode = EUnrealMCPGraphError::BlueprintNotFound;
        OutErrorMessage = TEXT("Cannot resolve the blueprint that owns this graph");
        return false;
    }

    // Resolve the event on the PARENT class first: a standard event (ReceiveBeginPlay / ReceiveTick /
    // ReceiveAnyDamage / ReceiveActorBeginOverlap ...) is a BlueprintImplementableEvent inherited from
    // AActor or one of its subclasses, and the node has to be an OVERRIDE of that function.
    UFunction* EventFunction = nullptr;
    if (Blueprint->ParentClass)
    {
        EventFunction = Blueprint->ParentClass->FindFunctionByName(FName(*EventName));
    }
    if (!EventFunction && Blueprint->GeneratedClass)
    {
        EventFunction = Blueprint->GeneratedClass->FindFunctionByName(FName(*EventName));
    }
    if (!EventFunction)
    {
        OutErrorCode = EUnrealMCPGraphError::FunctionNotFound;
        const TArray<FString> Events = ListImplementableEventNames(Blueprint);
        OutErrorMessage = FString::Printf(TEXT("Event '%s' does not exist on blueprint class '%s'. Implementable events: %s"),
            *EventName, *Blueprint->GeneratedClass->GetName(), *FString::Join(Events, TEXT(", ")));
        return false;
    }

    // A same-named event is reused rather than duplicated - and repaired on the way, so nodes
    // created before this fix (external member reference, no bOverrideFunction) start working.
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
        if (EventNode && EventNode->EventReference.GetMemberName() == FName(*EventName))
        {
            EventNode->EventReference.SetFromField<UFunction>(EventFunction, /*bIsConsideredSelfContext=*/false);
            EventNode->bOverrideFunction = true;
            EventNode->ReconstructNode();
            OutNode = EventNode;
            return true;
        }
    }

    // Same two lines the editor's own spawner uses (BlueprintEventNodeSpawner.cpp:211-218).
    // Without them the compiler builds a brand-new event nothing ever calls: the node looks right,
    // compiles clean, ListOverridableFunctions even reports it as overridden, but the engine never
    // dispatches to the body at runtime.
    OutNode = CreateGraphNode<UK2Node_Event>(Graph, Position,
        [EventFunction](UK2Node_Event* Node)
        {
            Node->EventReference.SetFromField<UFunction>(EventFunction, /*bIsConsideredSelfContext=*/false);
            Node->bOverrideFunction = true;
        });

    if (!OutNode)
    {
        OutErrorCode = EUnrealMCPGraphError::InvalidParams;
        OutErrorMessage = TEXT("Failed to create the event node");
        return false;
    }
    return true;
}

bool FUnrealMCPBlueprintGraphOps::CreateVariableNode(UEdGraph* Graph, const FString& VariableName, bool bSet,
                                                     const FVector2D& Position, UEdGraphNode*& OutNode,
                                                     FString& OutErrorCode, FString& OutErrorMessage,
                                                     TArray<FString>& OutCandidates)
{
    OutNode = nullptr;
    OutErrorCode.Reset();
    OutErrorMessage.Reset();
    OutCandidates.Reset();

    if (!Graph)
    {
        OutErrorCode = EUnrealMCPGraphError::InvalidParams;
        OutErrorMessage = TEXT("Invalid graph");
        return false;
    }

    UBlueprint* Blueprint = FindBlueprintForGraph(Graph);
    if (!Blueprint || !Blueprint->GeneratedClass)
    {
        OutErrorCode = EUnrealMCPGraphError::BlueprintNotFound;
        OutErrorMessage = TEXT("Cannot resolve the blueprint that owns this graph");
        return false;
    }

    const FName VarName(*VariableName);
    FProperty* Property = Blueprint->GeneratedClass
        ? FindFProperty<FProperty>(Blueprint->GeneratedClass, VarName)
        : nullptr;

    if (!Property)
    {
        // A member variable added by add_blueprint_variable only becomes a property on the
        // generated class once the blueprint has been compiled, so compile once and retry
        // before declaring the variable missing.
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
        Property = Blueprint->GeneratedClass
            ? FindFProperty<FProperty>(Blueprint->GeneratedClass, VarName)
            : nullptr;
    }

    if (!Property)
    {
        for (TFieldIterator<FProperty> It(Blueprint->GeneratedClass); It; ++It)
        {
            const FProperty* Candidate = *It;
            if (Candidate && Candidate->GetName().Contains(VariableName))
            {
                OutCandidates.Add(Candidate->GetName());
            }
        }
        for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
        {
            OutCandidates.AddUnique(Variable.VarName.ToString());
        }

        OutErrorCode = EUnrealMCPGraphError::VariableNotFound;
        OutErrorMessage = FString::Printf(TEXT("Variable '%s' not found on blueprint '%s'%s"),
            *VariableName, *Blueprint->GetName(),
            OutCandidates.Num() > 0 ? *FString::Printf(TEXT(". Available variables: %s"), *FString::Join(OutCandidates, TEXT(", "))) : TEXT(""));
        return false;
    }

    // The editor builds self-variable nodes as SELF-context references (see the component drag path
    // in SSCSEditor.cpp:4614 / SSubobjectEditor.cpp:2348). A non-self reference makes the compiler
    // demand that the node's own self pin be connected or carry a default object
    // (K2Node_Variable.cpp:765-773), which is exactly what made a self-component-reference node fail
    // to compile with "Variable node ... uses an invalid target".
    if (bSet)
    {
        OutNode = CreateGraphNode<UK2Node_VariableSet>(Graph, Position,
            [VarName](UK2Node_VariableSet* Node) { Node->VariableReference.SetSelfMember(VarName); });
    }
    else
    {
        OutNode = CreateGraphNode<UK2Node_VariableGet>(Graph, Position,
            [VarName](UK2Node_VariableGet* Node) { Node->VariableReference.SetSelfMember(VarName); });
    }

    if (!OutNode)
    {
        OutErrorCode = EUnrealMCPGraphError::InvalidParams;
        OutErrorMessage = TEXT("Failed to create the variable node");
        return false;
    }
    return true;
}

bool FUnrealMCPBlueprintGraphOps::CreateExternalVariableNode(UEdGraph* Graph, const FString& OwnerClass,
                                                             const FString& VariableName, bool bSet,
                                                             const FVector2D& Position, UEdGraphNode*& OutNode,
                                                             FString& OutErrorCode, FString& OutErrorMessage,
                                                             TArray<FString>& OutCandidates)
{
    OutNode = nullptr;
    OutErrorCode.Reset();
    OutErrorMessage.Reset();
    OutCandidates.Reset();

    if (!Graph)
    {
        OutErrorCode = EUnrealMCPGraphError::InvalidParams;
        OutErrorMessage = TEXT("Invalid graph");
        return false;
    }

    TArray<FString> Tried;
    FString ResolvedPath;
    UClass* Owner = FUnrealMCPCommonUtils::ResolveUClass(OwnerClass, Tried, ResolvedPath);
    if (!Owner)
    {
        OutErrorCode = EUnrealMCPGraphError::NodeClassNotFound;
        OutErrorMessage = FString::Printf(TEXT("owner_class '%s' does not resolve to a class (tried: %s)"),
            *OwnerClass, *FString::Join(Tried, TEXT(", ")));
        OutCandidates = Tried;
        return false;
    }

    const FName VarName(*VariableName);
    FProperty* Property = FindFProperty<FProperty>(Owner, VarName);
    if (!Property || !Property->HasAnyPropertyFlags(CPF_BlueprintVisible))
    {
        for (TFieldIterator<FProperty> It(Owner); It; ++It)
        {
            if (It->HasAnyPropertyFlags(CPF_BlueprintVisible))
            {
                OutCandidates.Add(It->GetName());
            }
        }
        OutCandidates.Sort();
        OutErrorCode = EUnrealMCPGraphError::VariableNotFound;
        OutErrorMessage = FString::Printf(TEXT("'%s' is not a blueprint-visible property of '%s'"),
            *VariableName, *Owner->GetName());
        return false;
    }

    if (bSet && Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly))
    {
        OutErrorCode = EUnrealMCPGraphError::PropertyNotWritable;
        OutErrorMessage = FString::Printf(TEXT("'%s.%s' is BlueprintReadOnly; only a Get node can be built"),
            *Owner->GetName(), *VariableName);
        return false;
    }

    // Non-self context: the node gets a Target pin typed to the property's owner class, which the
    // caller wires (an unwired Target is a compile error by design, K2Node_Variable::CheckForErrors).
    if (bSet)
    {
        OutNode = CreateGraphNode<UK2Node_VariableSet>(Graph, Position,
            [Property](UK2Node_VariableSet* Node) { Node->VariableReference.SetFromField<FProperty>(Property, false); });
    }
    else
    {
        OutNode = CreateGraphNode<UK2Node_VariableGet>(Graph, Position,
            [Property](UK2Node_VariableGet* Node) { Node->VariableReference.SetFromField<FProperty>(Property, false); });
    }

    if (!OutNode)
    {
        OutErrorCode = EUnrealMCPGraphError::InvalidParams;
        OutErrorMessage = TEXT("Failed to create the variable node");
        return false;
    }
    return true;
}

bool FUnrealMCPBlueprintGraphOps::CreateLocalVariableNode(UEdGraph* Graph, const FString& VariableName,
                                                          const FGuid& VariableGuid, bool bSet,
                                                          const FVector2D& Position, UEdGraphNode*& OutNode,
                                                          FString& OutErrorCode, FString& OutErrorMessage,
                                                          TArray<FString>& OutCandidates)
{
    OutNode = nullptr;
    OutErrorCode.Reset();
    OutErrorMessage.Reset();
    OutCandidates.Reset();

    if (!Graph)
    {
        OutErrorCode = EUnrealMCPGraphError::InvalidParams;
        OutErrorMessage = TEXT("Invalid graph");
        return false;
    }

    UBlueprint* Blueprint = FindBlueprintForGraph(Graph);
    if (!Blueprint)
    {
        OutErrorCode = EUnrealMCPGraphError::BlueprintNotFound;
        OutErrorMessage = TEXT("Cannot resolve the blueprint that owns this graph");
        return false;
    }

    // A local variable is scoped to its function graph, not to the class, so the reference is a local
    // member of the *top-level* graph: SetLocalMember(name, top-level graph name, guid), exactly what
    // the engine writes when it builds these nodes itself (K2Node_LocalVariable.cpp:154/185). A local
    // scope reference also produces no self pin, which is why nothing here needs the self handling
    // CreateVariableNode does.
    const FName VarName(*VariableName);
    const FString ScopeName = FBlueprintEditorUtils::GetTopLevelGraph(Graph)->GetName();

    if (bSet)
    {
        OutNode = CreateGraphNode<UK2Node_VariableSet>(Graph, Position,
            [VarName, &ScopeName, &VariableGuid](UK2Node_VariableSet* Node)
            { Node->VariableReference.SetLocalMember(VarName, ScopeName, VariableGuid); });
    }
    else
    {
        OutNode = CreateGraphNode<UK2Node_VariableGet>(Graph, Position,
            [VarName, &ScopeName, &VariableGuid](UK2Node_VariableGet* Node)
            { Node->VariableReference.SetLocalMember(VarName, ScopeName, VariableGuid); });
    }

    if (!OutNode)
    {
        OutErrorCode = EUnrealMCPGraphError::InvalidParams;
        OutErrorMessage = TEXT("Failed to create the local variable node");
        return false;
    }
    return true;
}

bool FUnrealMCPBlueprintGraphOps::LiteralFunctionName(const FString& LiteralType, FString& OutFunctionName,
                                                      TArray<FString>& OutSupportedTypes)
{
    OutSupportedTypes = SupportedLiteralTypes();
    OutFunctionName.Reset();

    // Engine facts (5.5): MakeLiteralFloat is NOT a UFUNCTION any more (deprecated in 5.1, never
    // reflected), so a float literal is served by MakeLiteralDouble, whose blueprint display name
    // is still "Make Literal Float". Every other entry mirrors a reflected MakeLiteral* function.
    struct FLiteralMapping
    {
        const TCHAR* Type;
        const TCHAR* Function;
    };
    static const FLiteralMapping Mapping[] = {
        { TEXT("float"),  TEXT("MakeLiteralDouble") },
        { TEXT("double"), TEXT("MakeLiteralDouble") },
        { TEXT("int"),    TEXT("MakeLiteralInt") },
        { TEXT("int64"),  TEXT("MakeLiteralInt64") },
        { TEXT("bool"),   TEXT("MakeLiteralBool") },
        { TEXT("name"),   TEXT("MakeLiteralName") },
        { TEXT("byte"),   TEXT("MakeLiteralByte") },
        { TEXT("string"), TEXT("MakeLiteralString") },
        { TEXT("text"),   TEXT("MakeLiteralText") },
    };

    for (const FLiteralMapping& Entry : Mapping)
    {
        if (LiteralType.Equals(Entry.Type, ESearchCase::IgnoreCase))
        {
            // Guard against engine-version drift: an unreflected function cannot be called.
            if (!UKismetSystemLibrary::StaticClass()->FindFunctionByName(FName(Entry.Function)))
            {
                return false;
            }
            OutFunctionName = Entry.Function;
            return true;
        }
    }
    return false;
}

TArray<FString> FUnrealMCPBlueprintGraphOps::SupportedLiteralTypes()
{
    return {
        TEXT("float"), TEXT("double"), TEXT("int"), TEXT("int64"), TEXT("bool"),
        TEXT("name"), TEXT("byte"), TEXT("string"), TEXT("text")
    };
}

bool FUnrealMCPBlueprintGraphOps::CreateLiteralNode(UEdGraph* Graph, const FString& LiteralType,
                                                    const FString& Value, const FVector2D& Position,
                                                    UEdGraphNode*& OutNode, FString& OutValueAfter,
                                                    FString& OutErrorCode, FString& OutErrorMessage,
                                                    TArray<FString>& OutCandidates)
{
    OutNode = nullptr;
    OutValueAfter.Reset();
    OutErrorCode.Reset();
    OutErrorMessage.Reset();
    OutCandidates.Reset();

    FString FunctionName;
    if (!LiteralFunctionName(LiteralType, FunctionName, OutCandidates))
    {
        OutErrorCode = EUnrealMCPGraphError::UnsupportedLiteralType;

        const bool bKnownType = OutCandidates.ContainsByPredicate(
            [&LiteralType](const FString& Candidate) { return Candidate.Equals(LiteralType, ESearchCase::IgnoreCase); });
        OutErrorMessage = bKnownType
            ? FString::Printf(TEXT("Literal type '%s' maps to a MakeLiteral* function that this engine build does not reflect"),
                *LiteralType)
            : FString::Printf(TEXT("Unsupported literal type '%s'. Supported: %s"),
                *LiteralType, *FString::Join(OutCandidates, TEXT(", ")));
        return false;
    }

    UK2Node_CallFunction* FunctionNode = nullptr;
    FString CallErrorCode;
    FString CallErrorMessage;
    TArray<FString> CallCandidates;
    if (!CreateFunctionCallNode(Graph, TEXT("KismetSystemLibrary"), FunctionName, Position,
                                FunctionNode, CallErrorCode, CallErrorMessage, CallCandidates))
    {
        OutErrorCode = MoveTemp(CallErrorCode);
        OutErrorMessage = MoveTemp(CallErrorMessage);
        OutCandidates = MoveTemp(CallCandidates);
        return false;
    }

    // The value has to be valid before the node is allowed to stay in the graph.
    TSharedPtr<FJsonValue> ValueJson;
    FString ValueErrorMessage;
    if (!MakeJsonValueFromString(Value, LiteralType, ValueJson, ValueErrorMessage))
    {
        Graph->RemoveNode(FunctionNode, /*bBreakAllLinks=*/true);
        OutErrorCode = EUnrealMCPGraphError::InvalidValue;
        OutErrorMessage = FString::Printf(TEXT("Value '%s' is not valid for literal type '%s': %s"),
            *Value, *LiteralType, *ValueErrorMessage);
        return false;
    }

    FString PinErrorCode;
    FString PinErrorMessage;
    TArray<FString> PinCandidates;
    if (!SetPinDefaultValue(FunctionNode, TEXT("Value"), ValueJson, PinErrorCode, PinErrorMessage, PinCandidates))
    {
        Graph->RemoveNode(FunctionNode, /*bBreakAllLinks=*/true);
        OutErrorCode = MoveTemp(PinErrorCode);
        OutErrorMessage = MoveTemp(PinErrorMessage);
        OutCandidates = MoveTemp(PinCandidates);
        return false;
    }

    if (const UEdGraphPin* ValuePin = FindPin(FunctionNode, TEXT("Value"), EGPD_Input))
    {
        OutValueAfter = ValuePin->DefaultValue;
    }
    OutNode = FunctionNode;
    MarkModified(Graph);
    return true;
}

bool FUnrealMCPBlueprintGraphOps::CreateNodeByClass(UEdGraph* Graph, const FString& NodeClass,
                                                    const FVector2D& Position, UEdGraphNode*& OutNode,
                                                    FString& OutErrorCode, FString& OutErrorMessage,
                                                    TArray<FString>& OutCandidates)
{
    OutNode = nullptr;
    OutErrorCode.Reset();
    OutErrorMessage.Reset();
    OutCandidates.Reset();

    if (!Graph)
    {
        OutErrorCode = EUnrealMCPGraphError::InvalidParams;
        OutErrorMessage = TEXT("Invalid graph");
        return false;
    }

    UClass* ResolvedClass = nullptr;
    if (!ResolveNodeClass(NodeClass, ResolvedClass, OutCandidates) || !ResolvedClass)
    {
        OutErrorCode = EUnrealMCPGraphError::NodeClassNotFound;
        OutErrorMessage = FString::Printf(TEXT("Node class '%s' could not be resolved%s"), *NodeClass,
            OutCandidates.Num() > 0 ? *FString::Printf(TEXT(" (did you mean: %s?)"), *FString::Join(OutCandidates, TEXT(", "))) : TEXT(""));
        return false;
    }

    OutNode = CreateGraphNodeByClass(Graph, ResolvedClass, Position, [](UEdGraphNode*) {});
    if (!OutNode)
    {
        OutErrorCode = EUnrealMCPGraphError::InvalidParams;
        OutErrorMessage = FString::Printf(TEXT("Failed to instantiate node class '%s'"), *ResolvedClass->GetName());
        return false;
    }
    return true;
}

bool FUnrealMCPBlueprintGraphOps::CreateSelfReferenceNode(UEdGraph* Graph, const FVector2D& Position,
                                                          UEdGraphNode*& OutNode, FString& OutErrorCode,
                                                          FString& OutErrorMessage)
{
    OutNode = nullptr;
    OutErrorCode.Reset();
    OutErrorMessage.Reset();

    OutNode = CreateGraphNode<UK2Node_Self>(Graph, Position);
    if (!OutNode)
    {
        OutErrorCode = EUnrealMCPGraphError::InvalidParams;
        OutErrorMessage = TEXT("Failed to create the self reference node");
        return false;
    }
    return true;
}

bool FUnrealMCPBlueprintGraphOps::CreateInputActionNode(UEdGraph* Graph, const FString& ActionName,
                                                        const FVector2D& Position, UEdGraphNode*& OutNode,
                                                        FString& OutErrorCode, FString& OutErrorMessage)
{
    OutNode = nullptr;
    OutErrorCode.Reset();
    OutErrorMessage.Reset();

    OutNode = CreateGraphNode<UK2Node_InputAction>(Graph, Position,
        [&ActionName](UK2Node_InputAction* Node) { Node->InputActionName = FName(*ActionName); });

    if (!OutNode)
    {
        OutErrorCode = EUnrealMCPGraphError::InvalidParams;
        OutErrorMessage = TEXT("Failed to create the input action node");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Mutation
// ---------------------------------------------------------------------------

bool FUnrealMCPBlueprintGraphOps::ConnectNodes(UEdGraph* Graph, UEdGraphNode* SourceNode, const FString& SourcePinName,
                                               UEdGraphNode* TargetNode, const FString& TargetPinName,
                                               FString& OutErrorCode, FString& OutErrorMessage,
                                               TArray<FString>& OutCandidates, TArray<FDisplacedLink>* OutDisplaced)
{
    OutErrorCode.Reset();
    OutErrorMessage.Reset();
    OutCandidates.Reset();

    if (!Graph || !SourceNode || !TargetNode)
    {
        OutErrorCode = EUnrealMCPGraphError::NodeNotFound;
        OutErrorMessage = TEXT("Invalid graph, source node or target node");
        return false;
    }

    UEdGraphPin* SourcePin = FindPin(SourceNode, SourcePinName, EGPD_Output);
    if (!SourcePin)
    {
        CollectPinNames(SourceNode, EGPD_Output, OutCandidates);
        OutErrorCode = EUnrealMCPGraphError::OutputNotFound;
        OutErrorMessage = FString::Printf(TEXT("Output pin '%s' not found on '%s'. Available outputs: %s"),
            *SourcePinName, *SourceNode->GetNodeTitle(ENodeTitleType::ListView).ToString(),
            OutCandidates.Num() > 0 ? *FString::Join(OutCandidates, TEXT(", ")) : TEXT("<none>"));
        return false;
    }

    UEdGraphPin* TargetPin = FindPin(TargetNode, TargetPinName, EGPD_Input);
    if (!TargetPin)
    {
        CollectPinNames(TargetNode, EGPD_Input, OutCandidates);
        OutErrorCode = EUnrealMCPGraphError::InputNotFound;
        OutErrorMessage = FString::Printf(TEXT("Input pin '%s' not found on '%s'. Available inputs: %s"),
            *TargetPinName, *TargetNode->GetNodeTitle(ENodeTitleType::ListView).ToString(),
            OutCandidates.Num() > 0 ? *FString::Join(OutCandidates, TEXT(", ")) : TEXT("<none>"));
        return false;
    }

    const UEdGraphSchema* Schema = Graph->GetSchema();
    if (!Schema)
    {
        OutErrorCode = TEXT("no_schema");
        OutErrorMessage = FString::Printf(TEXT("Graph '%s' has no schema to validate connections"), *Graph->GetName());
        return false;
    }

    // Snapshot BOTH pins before connecting. The schema breaks the side that cannot hold two wires
    // (exec pins answer CONNECT_RESPONSE_BREAK_OTHERS_A), and that side is usually the SOURCE - the
    // platformer round lost Tick -> Branch when Tick was re-connected to another node, and the receipt
    // showed nothing because only the target side was ever inspected. See the FDisplacedLink comment.
    TArray<TPair<UEdGraphPin*, UEdGraphPin*>> LinksBefore;
    if (OutDisplaced)
    {
        OutDisplaced->Reset();
        for (UEdGraphPin* Pin : {SourcePin, TargetPin})
        {
            for (UEdGraphPin* Linked : Pin->LinkedTo)
            {
                if (Linked)
                {
                    LinksBefore.Emplace(Pin, Linked);
                }
            }
        }
    }

    // NOTE: the schema legitimately breaks the source pin's existing links here (the K2 schema answers
    // CONNECT_RESPONSE_BREAK_OTHERS_A for exec output pins, which may only hold a single connection -
    // branching needs an explicit Sequence node). Do NOT "restore" them: that produces graphs the
    // compiler rejects with "an exec output pin cannot have more than one connection".
    if (!Schema->TryCreateConnection(SourcePin, TargetPin))
    {
        const FPinConnectionResponse Response = Schema->CanCreateConnection(SourcePin, TargetPin);
        OutErrorCode = EUnrealMCPGraphError::IncompatibleTypes;
        OutErrorMessage = FString::Printf(TEXT("Cannot connect '%s' (%s) to '%s' (%s): %s"),
            *SourcePin->PinName.ToString(), *DescribePinType(SourcePin),
            *TargetPin->PinName.ToString(), *DescribePinType(TargetPin),
            *Response.Message.ToString());
        return false;
    }

    // Whatever was linked before and is not any more was dropped by the schema. Report it rather than
    // letting the caller discover it later as an unreachable_node.
    if (OutDisplaced)
    {
        for (const TPair<UEdGraphPin*, UEdGraphPin*>& Link : LinksBefore)
        {
            UEdGraphPin* OurPin = Link.Key;
            UEdGraphPin* OtherPin = Link.Value;
            if (!OurPin || !OtherPin || OurPin->LinkedTo.Contains(OtherPin))
            {
                continue;
            }
            UEdGraphNode* OurNode = OurPin->GetOwningNode();
            UEdGraphNode* OtherNode = OtherPin->GetOwningNode();
            FDisplacedLink Displaced;
            Displaced.NodeId = OurNode ? OurNode->NodeGuid.ToString() : FString();
            Displaced.NodeName = OurNode ? OurNode->GetName() : FString();
            Displaced.PinName = OurPin->PinName.ToString();
            Displaced.Direction = (OurPin->Direction == EGPD_Output) ? TEXT("output") : TEXT("input");
            Displaced.LostNodeId = OtherNode ? OtherNode->NodeGuid.ToString() : FString();
            Displaced.LostPinName = OtherPin->PinName.ToString();
            OutDisplaced->Add(Displaced);
        }
    }

    // Wildcard pins have to be resolved from the side they were linked to. UEdGraphSchema::
    // TryCreateConnection leaves them wildcard (the graph editor does it afterwards through
    // UK2Node::PinConnectionListChanged -> UK2Node_CallArrayFunction::NotifyPinConnectionListChanged,
    // which copies exactly these three fields). Without this an Array_Length node built over MCP keeps
    // "Target Array: type not determined" and the blueprint fails to compile with the link in place.
    auto ResolveWildcardPin = [](UEdGraphPin* Pin)
    {
        if (!Pin || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard || Pin->LinkedTo.Num() == 0)
        {
            return;
        }
        const UEdGraphPin* LinkedPin = Pin->LinkedTo[0];
        if (!LinkedPin || LinkedPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
        {
            return;
        }
        Pin->PinType.PinCategory = LinkedPin->PinType.PinCategory;
        Pin->PinType.PinSubCategory = LinkedPin->PinType.PinSubCategory;
        Pin->PinType.PinSubCategoryObject = LinkedPin->PinType.PinSubCategoryObject;
    };
    ResolveWildcardPin(TargetPin);
    ResolveWildcardPin(SourcePin);

    // Also notify the nodes (same entry the editor uses after a drag) so array-call nodes and other
    // wildcard owners get their dependent pins re-derived. UEdGraphSchema::TryCreateConnection already
    // notifies the two endpoints, but not the pins on the far side of an automatic conversion node.
    TArray<UEdGraphPin*, TInlineAllocator<4>> NotifiedPins;
    for (const UEdGraphPin* LinkedPin : SourcePin->LinkedTo)
    {
        NotifiedPins.Add(const_cast<UEdGraphPin*>(LinkedPin));
    }
    for (const UEdGraphPin* LinkedPin : TargetPin->LinkedTo)
    {
        NotifiedPins.Add(const_cast<UEdGraphPin*>(LinkedPin));
    }
    for (UEdGraphPin* Pin : NotifiedPins)
    {
        if (UEdGraphNode* OwningNode = Pin ? Pin->GetOwningNodeUnchecked() : nullptr)
        {
            OwningNode->PinConnectionListChanged(Pin);
        }
    }

    MarkModified(Graph);
    return true;
}

bool FUnrealMCPBlueprintGraphOps::TryBuildMultiValuePinHint(const TSharedPtr<FJsonValue>& Value, FString& OutHint)
{
    OutHint.Reset();

    // Only a list value explains this failure. Measured: a scalar written to even an Array[Name] pin is
    // ACCEPTED (it lands in the pin's DefaultValue); a JSON list is refused because the pin holds one value.
    if (!Value.IsValid() || Value->Type != EJson::Array)
    {
        return false;
    }

    OutHint = TEXT("a pin default holds ONE value and this was a list. To feed several values, put a MakeArray "
                   "node beside it, connect each element pin to this pin, and write the elements' defaults - the "
                   "element pins are wildcard until they are connected (writing before that fails with a "
                   "different error).");
    return true;
}

bool FUnrealMCPBlueprintGraphOps::SetPinDefaultValue(UEdGraphNode* Node, const FString& PinName,
                                                     const TSharedPtr<FJsonValue>& Value,
                                                     FString& OutErrorCode, FString& OutErrorMessage,
                                                     TArray<FString>& OutCandidates)
{
    OutErrorCode.Reset();
    OutErrorMessage.Reset();
    OutCandidates.Reset();

    if (!Node || !Value.IsValid())
    {
        OutErrorCode = EUnrealMCPGraphError::InvalidParams;
        OutErrorMessage = TEXT("Invalid node or value");
        return false;
    }

    UEdGraphPin* Pin = FindPin(Node, PinName, EGPD_Input);
    if (!Pin)
    {
        CollectPinNames(Node, EGPD_Input, OutCandidates);
        OutErrorCode = EUnrealMCPGraphError::PinNotFound;
        OutErrorMessage = FString::Printf(TEXT("Input pin '%s' not found. Available inputs: %s"),
            *PinName, OutCandidates.Num() > 0 ? *FString::Join(OutCandidates, TEXT(", ")) : TEXT("<none>"));
        return false;
    }

    const FName Category = Pin->PinType.PinCategory;
    const FName SubCategory = Pin->PinType.PinSubCategory;

    // A tool caller reaches this through a string-typed `value`, so a pin that demands a number or a
    // boolean also takes the text form ("0.25", "true"). Nothing is loosened for the pins that want
    // text: the coercion only runs on the kinds below.
    auto NumberFromJson = [](const TSharedPtr<FJsonValue>& In, double& Out) -> bool
    {
        if (!In.IsValid())
        {
            return false;
        }
        if (In->Type == EJson::Number)
        {
            Out = In->AsNumber();
            return true;
        }
        if (In->Type == EJson::String)
        {
            const FString Text = In->AsString().TrimStartAndEnd();
            if (Text.IsNumeric())
            {
                Out = FCString::Atod(*Text);
                return true;
            }
        }
        return false;
    };
    auto BoolFromJson = [](const TSharedPtr<FJsonValue>& In, bool& Out) -> bool
    {
        if (!In.IsValid())
        {
            return false;
        }
        if (In->Type == EJson::Boolean)
        {
            Out = In->AsBool();
            return true;
        }
        if (In->Type == EJson::Number)
        {
            Out = In->AsNumber() != 0.0;
            return true;
        }
        if (In->Type == EJson::String)
        {
            const FString Text = In->AsString().TrimStartAndEnd();
            if (Text.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Text == TEXT("1"))
            {
                Out = true;
                return true;
            }
            if (Text.Equals(TEXT("false"), ESearchCase::IgnoreCase) || Text == TEXT("0"))
            {
                Out = false;
                return true;
            }
        }
        return false;
    };

    // --- numeric ---
    if (Category == UEdGraphSchema_K2::PC_Int || Category == UEdGraphSchema_K2::PC_Int64)
    {
        double Number = 0.0;
        if (!NumberFromJson(Value, Number))
        {
            OutErrorCode = TEXT("type_mismatch");
            OutErrorMessage = FString::Printf(TEXT("Integer pin '%s' requires a number value"), *PinName);
            return false;
        }
        Pin->DefaultValue = FString::Printf(TEXT("%lld"), static_cast<int64>(Number));
        return true;
    }

    if (Category == UEdGraphSchema_K2::PC_Real || Category == UEdGraphSchema_K2::PC_Float || Category == UEdGraphSchema_K2::PC_Double)
    {
        double Number = 0.0;
        if (!NumberFromJson(Value, Number))
        {
            OutErrorCode = TEXT("type_mismatch");
            OutErrorMessage = FString::Printf(TEXT("Float pin '%s' requires a number value"), *PinName);
            return false;
        }
        Pin->DefaultValue = SubCategory == UEdGraphSchema_K2::PC_Float
            ? FString::SanitizeFloat(static_cast<float>(Number))
            : FString::Printf(TEXT("%.17g"), Number);
        return true;
    }

    // --- boolean ---
    if (Category == UEdGraphSchema_K2::PC_Boolean)
    {
        bool Flag = false;
        if (BoolFromJson(Value, Flag))
        {
            Pin->DefaultValue = Flag ? TEXT("true") : TEXT("false");
            return true;
        }
        OutErrorCode = TEXT("type_mismatch");
        OutErrorMessage = FString::Printf(TEXT("Bool pin '%s' requires a boolean value"), *PinName);
        return false;
    }

    // --- name / string / text ---
    if (Category == UEdGraphSchema_K2::PC_Name || Category == UEdGraphSchema_K2::PC_String || Category == UEdGraphSchema_K2::PC_Text)
    {
        if (Value->Type != EJson::String)
        {
            OutErrorCode = TEXT("type_mismatch");
            OutErrorMessage = FString::Printf(TEXT("'%s' pin '%s' requires a string value"), *Category.ToString(), *PinName);
            return false;
        }
        Pin->DefaultValue = Value->AsString();

        // A text pin's literal lives in DefaultTextValue: that is what the compiler materialises as the
        // FText argument. The schema's own writer sets DefaultValue AND DefaultTextValue
        // (EdGraphSchema_K2.cpp:4880-4882); writing DefaultValue alone compiles to an EMPTY text -
        // silently, with no error and no warning.
        if (Category == UEdGraphSchema_K2::PC_Text)
        {
            Pin->DefaultTextValue = FText::FromString(Value->AsString());
        }
        return true;
    }

    // --- enum (and plain byte) ---
    if (Category == UEdGraphSchema_K2::PC_Byte || Category == UEdGraphSchema_K2::PC_Enum)
    {
        UEnum* EnumDef = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get());
        if (!EnumDef)
        {
            double Number = 0.0;
            if (!NumberFromJson(Value, Number))
            {
                OutErrorCode = TEXT("type_mismatch");
                OutErrorMessage = FString::Printf(TEXT("Byte pin '%s' requires a number value"), *PinName);
                return false;
            }
            Pin->DefaultValue = FString::FromInt(static_cast<int32>(Number));
            return true;
        }

        int64 EnumValue = INDEX_NONE;
        double EnumNumber = 0.0;
        if (NumberFromJson(Value, EnumNumber))
        {
            EnumValue = static_cast<int64>(EnumNumber);
        }
        else if (Value->Type == EJson::String)
        {
            FString RequestedName = Value->AsString();
            EnumValue = EnumDef->GetValueByNameString(RequestedName);
            if (EnumValue == INDEX_NONE && RequestedName.Contains(TEXT("::")))
            {
                RequestedName.Split(TEXT("::"), nullptr, &RequestedName);
                EnumValue = EnumDef->GetValueByNameString(RequestedName);
            }
        }
        else
        {
            OutErrorCode = TEXT("type_mismatch");
            OutErrorMessage = FString::Printf(TEXT("Enum pin '%s' requires a member name or its numeric value"), *PinName);
            return false;
        }

        const FString EntryName = EnumDef->GetNameStringByValue(EnumValue);
        if (EnumValue == INDEX_NONE || EntryName.IsEmpty())
        {
            for (int32 Index = 0; Index < EnumDef->NumEnums(); ++Index)
            {
                OutCandidates.Add(EnumDef->GetNameStringByIndex(Index));
            }
            OutErrorCode = TEXT("unknown_enum_member");
            OutErrorMessage = FString::Printf(TEXT("Unknown member for enum '%s'. Candidates: %s"),
                *EnumDef->GetName(), *FString::Join(OutCandidates, TEXT(", ")));
            return false;
        }

        Pin->DefaultValue = EntryName;
        return true;
    }

    // --- structs (vector / vector2d / linear color / vector4) ---
    if (Category == UEdGraphSchema_K2::PC_Struct)
    {
        UScriptStruct* StructType = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get());
        if (!StructType)
        {
            OutErrorCode = EUnrealMCPGraphError::UnsupportedPinType;
            OutErrorMessage = FString::Printf(TEXT("Struct pin '%s' has no resolvable struct type"), *PinName);
            return false;
        }

        TArray<double> Numbers;
        FString ReadError;
        if (StructType == TBaseStructure<FVector2D>::Get())
        {
            if (!FUnrealMCPCommonUtils::ReadNumbersFromJson(Value, { TEXT("X"), TEXT("Y") }, Numbers, ReadError) || Numbers.Num() != 2)
            {
                OutErrorCode = TEXT("type_mismatch");
                OutErrorMessage = FString::Printf(TEXT("Vector2D pin '%s': %s"), *PinName, *ReadError);
                return false;
            }
            Pin->DefaultValue = FString::Printf(TEXT("(X=%f,Y=%f)"), Numbers[0], Numbers[1]);
            return true;
        }
        if (StructType == TBaseStructure<FVector>::Get())
        {
            if (!FUnrealMCPCommonUtils::ReadNumbersFromJson(Value, { TEXT("X"), TEXT("Y"), TEXT("Z") }, Numbers, ReadError) || Numbers.Num() != 3)
            {
                OutErrorCode = TEXT("type_mismatch");
                OutErrorMessage = FString::Printf(TEXT("Vector pin '%s': %s"), *PinName, *ReadError);
                return false;
            }
            Pin->DefaultValue = FString::Printf(TEXT("(X=%f,Y=%f,Z=%f)"), Numbers[0], Numbers[1], Numbers[2]);
            return true;
        }
        if (StructType == TBaseStructure<FLinearColor>::Get())
        {
            if (!FUnrealMCPCommonUtils::ReadNumbersFromJson(Value, { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") }, Numbers, ReadError) || Numbers.Num() != 4)
            {
                OutErrorCode = TEXT("type_mismatch");
                OutErrorMessage = FString::Printf(TEXT("LinearColor pin '%s': %s"), *PinName, *ReadError);
                return false;
            }
            Pin->DefaultValue = FString::Printf(TEXT("(R=%f,G=%f,B=%f,A=%f)"), Numbers[0], Numbers[1], Numbers[2], Numbers[3]);
            return true;
        }
        if (StructType == TBaseStructure<FVector4>::Get())
        {
            if (!FUnrealMCPCommonUtils::ReadNumbersFromJson(Value, { TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W") }, Numbers, ReadError) || Numbers.Num() != 4)
            {
                OutErrorCode = TEXT("type_mismatch");
                OutErrorMessage = FString::Printf(TEXT("Vector4 pin '%s': %s"), *PinName, *ReadError);
                return false;
            }
            Pin->DefaultValue = FString::Printf(TEXT("(X=%f,Y=%f,Z=%f,W=%f)"), Numbers[0], Numbers[1], Numbers[2], Numbers[3]);
            return true;
        }

        OutErrorCode = EUnrealMCPGraphError::UnsupportedPinType;
        OutErrorMessage = FString::Printf(TEXT("Struct pin '%s' of type '%s' is not supported"), *PinName, *StructType->GetName());
        return false;
    }

    // --- class references ---
    if (Category == UEdGraphSchema_K2::PC_Class || Category == UEdGraphSchema_K2::PC_SoftClass)
    {
        if (Value->Type != EJson::String)
        {
            OutErrorCode = TEXT("type_mismatch");
            OutErrorMessage = FString::Printf(TEXT("Class pin '%s' requires a class path string"), *PinName);
            return false;
        }

        const FString ClassPath = Value->AsString();
        UClass* Class = FindObject<UClass>(nullptr, *ClassPath);
        if (!Class)
        {
            Class = LoadObject<UClass>(nullptr, *ClassPath);
        }
        if (!Class)
        {
            Class = LoadClass<UObject>(nullptr, *ClassPath);
        }
        if (!Class)
        {
            FString UnusedErrorCode;
            TArray<FString> ClassCandidates;
            ResolveClass(ClassPath, Class, ClassCandidates);
            OutCandidates = MoveTemp(ClassCandidates);
        }
        if (!Class)
        {
            OutErrorCode = TEXT("load_failed");
            OutErrorMessage = FString::Printf(TEXT("Could not load class '%s' for pin '%s'"), *ClassPath, *PinName);
            return false;
        }

        // Each family has its own rule (EdGraphSchema_K2.cpp:4233 / :4344): a PC_Class pin must
        // carry the object and NO string, a PC_SoftClass pin must carry the path string and NO
        // object. Writing both is a compile error ("String NewDefaultValue ... specified on class
        // pin"), which is why a class pin written by this path never produced a compilable graph.
        if (Category == UEdGraphSchema_K2::PC_Class)
        {
            Pin->DefaultObject = Class;
            Pin->DefaultValue.Reset();
        }
        else
        {
            Pin->DefaultObject = nullptr;
            Pin->DefaultValue = Class->GetPathName();
        }
        return true;
    }

    // --- object references ---
    if (Category == UEdGraphSchema_K2::PC_Object || Category == UEdGraphSchema_K2::PC_Interface || Category == UEdGraphSchema_K2::PC_SoftObject)
    {
        if (Value->Type != EJson::String)
        {
            OutErrorCode = TEXT("type_mismatch");
            OutErrorMessage = FString::Printf(TEXT("Object pin '%s' requires an asset path string"), *PinName);
            return false;
        }

        const FString RawPath = Value->AsString().TrimStartAndEnd();
        UClass* RequiredClass = Cast<UClass>(Pin->PinType.PinSubCategoryObject.Get());
        auto MatchesPin = [RequiredClass](UObject* Candidate)
        {
            return Candidate && (!RequiredClass || Candidate->IsA(RequiredClass));
        };

        // Resolve without a class filter first: a path may name an outer object (asset helpers stop
        // there) whose sub-object is the real target, and the class gate has to run on the leaf.
        auto ResolveRaw = [](const FString& Path) -> UObject*
        {
            if (UObject* Direct = FUnrealMCPCommonUtils::FindAsset(Path))
            {
                return Direct;
            }
            if (UObject* Loaded = FindObject<UObject>(nullptr, *Path))
            {
                return Loaded;
            }
            return StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
        };

        UObject* Resolved = ResolveRaw(RawPath);
        UObject* Mismatched = MatchesPin(Resolved) ? nullptr : Resolved;
        UObject* Asset = MatchesPin(Resolved) ? Resolved : nullptr;

        // "<SomeOuter>:<LeafName>": when only the outer came back (or the quote-colon form failed
        // outright), walk the outer's sub-objects for the leaf.
        int32 ColonIndex = INDEX_NONE;
        if (!Asset && RawPath.FindLastChar(TEXT(':'), ColonIndex))
        {
            const FString OuterPath = RawPath.Left(ColonIndex);
            const FString LeafName = RawPath.Mid(ColonIndex + 1);
            UObject* Outer = Resolved ? Resolved : ResolveRaw(OuterPath);
            if (Outer)
            {
                TArray<UObject*> SubObjects;
                SubObjects.Add(Outer);
                GetObjectsWithOuter(Outer, SubObjects, /*bIncludeNestedObjects=*/true);
                for (UObject* Candidate : SubObjects)
                {
                    if (MatchesPin(Candidate) && Candidate->GetName() == LeafName)
                    {
                        Asset = Candidate;
                        break;
                    }
                }
            }
        }

        // A bare name: search the owning blueprint (its own sub-objects first - that is where a widget
        // animation lives) and then its package, matching both name and required class.
        if (!Asset && !RawPath.IsEmpty() && !RawPath.Contains(TEXT("/")))
        {
            UBlueprint* OwnerBlueprint = Node ? Node->GetTypedOuter<UBlueprint>() : nullptr;
            TArray<UObject*> SearchScope;
            if (OwnerBlueprint)
            {
                SearchScope.Add(OwnerBlueprint);
                GetObjectsWithOuter(OwnerBlueprint, SearchScope, /*bIncludeNestedObjects=*/true);
                if (UPackage* OwnerPackage = OwnerBlueprint->GetOutermost())
                {
                    SearchScope.Add(OwnerPackage);
                    GetObjectsWithOuter(OwnerPackage, SearchScope, /*bIncludeNestedObjects=*/true);
                }
            }
            for (UObject* Candidate : SearchScope)
            {
                if (!Candidate || Candidate == OwnerBlueprint || Candidate->GetName() != RawPath)
                {
                    continue;
                }
                if (MatchesPin(Candidate))
                {
                    Asset = Candidate;
                    break;
                }
            }
            // Nothing matched: hand back what IS in scope so the caller can pick a real name.
            if (!Asset && RequiredClass)
            {
                for (UObject* Candidate : SearchScope)
                {
                    if (MatchesPin(Candidate))
                    {
                        OutCandidates.AddUnique(Candidate->GetName());
                    }
                }
                OutCandidates.Sort();
                if (OutCandidates.Num() > 15)
                {
                    OutCandidates.SetNum(15);
                }
            }
        }

        if (!Asset && Mismatched)
        {
            OutErrorCode = TEXT("type_mismatch");
            OutErrorMessage = FString::Printf(TEXT("Asset '%s' (%s) is not assignable to pin '%s' (%s)"),
                *Mismatched->GetName(), *Mismatched->GetClass()->GetName(), *PinName,
                RequiredClass ? *RequiredClass->GetName() : TEXT("?"));
            return false;
        }
        if (!Asset)
        {
            OutErrorCode = TEXT("load_failed");
            OutErrorMessage = FString::Printf(TEXT("Could not resolve object '%s' for pin '%s'%s"),
                *RawPath, *PinName,
                RequiredClass ? *FString::Printf(TEXT(" (expected a %s: an asset path, the inner-object path, or the object's own name)"), *RequiredClass->GetName()) : TEXT(""));
            return false;
        }

        // Same split as the class family above (EdGraphSchema_K2.cpp:4320 / :4344): PC_Object and
        // PC_Interface pins must carry the object and NO string, PC_SoftObject must carry the path
        // string and NO object.
        if (Category == UEdGraphSchema_K2::PC_SoftObject)
        {
            Pin->DefaultObject = nullptr;
            Pin->DefaultValue = Asset->GetPathName();
        }
        else
        {
            Pin->DefaultObject = Asset;
            Pin->DefaultValue.Reset();
        }
        return true;
    }

    OutErrorCode = EUnrealMCPGraphError::UnsupportedPinType;
    OutErrorMessage = FString::Printf(TEXT("Pin '%s' has unsupported category '%s'"), *PinName, *Category.ToString());
    return false;
}

bool FUnrealMCPBlueprintGraphOps::MakeJsonValueFromString(const FString& Value, const FString& ValueKind,
                                                          TSharedPtr<FJsonValue>& OutValue, FString& OutErrorMessage)
{
    OutValue.Reset();
    OutErrorMessage.Reset();

    const FString Kind = ValueKind.IsEmpty() ? TEXT("auto") : ValueKind.ToLower();

    if (Kind == TEXT("number") || Kind == TEXT("float") || Kind == TEXT("double") ||
        Kind == TEXT("int") || Kind == TEXT("int64") || Kind == TEXT("byte"))
    {
        if (!Value.IsNumeric())
        {
            OutErrorMessage = FString::Printf(TEXT("'%s' is not a number"), *Value);
            return false;
        }
        OutValue = MakeShared<FJsonValueNumber>(FCString::Atod(*Value));
        return true;
    }

    if (Kind == TEXT("bool") || Kind == TEXT("boolean"))
    {
        if (Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value == TEXT("1"))
        {
            OutValue = MakeShared<FJsonValueBoolean>(true);
            return true;
        }
        if (Value.Equals(TEXT("false"), ESearchCase::IgnoreCase) || Value == TEXT("0"))
        {
            OutValue = MakeShared<FJsonValueBoolean>(false);
            return true;
        }
        OutErrorMessage = FString::Printf(TEXT("'%s' is not a boolean"), *Value);
        return false;
    }

    if (Kind == TEXT("vector") || Kind == TEXT("vector2d") || Kind == TEXT("vector4") ||
        Kind == TEXT("linear_color") || Kind == TEXT("linearcolor") || Kind == TEXT("rotator"))
    {
        TArray<double> Numbers;
        if (!ParseFloats(Value, Numbers))
        {
            OutErrorMessage = FString::Printf(TEXT("'%s' is not a comma separated list of numbers"), *Value);
            return false;
        }

        const TCHAR* Keys[] = { TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W") };
        if (Kind == TEXT("linear_color") || Kind == TEXT("linearcolor"))
        {
            Keys[0] = TEXT("R"); Keys[1] = TEXT("G"); Keys[2] = TEXT("B"); Keys[3] = TEXT("A");
        }
        else if (Kind == TEXT("rotator"))
        {
            Keys[0] = TEXT("Pitch"); Keys[1] = TEXT("Yaw"); Keys[2] = TEXT("Roll");
        }

        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        for (int32 Index = 0; Index < Numbers.Num() && Index < 4; ++Index)
        {
            Object->SetNumberField(Keys[Index], Numbers[Index]);
        }
        OutValue = MakeShared<FJsonValueObject>(Object);
        return true;
    }

    // auto + everything else that is carried as text (string / name / text / object / class / enum)
    if (Kind == TEXT("auto"))
    {
        if (Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("false"), ESearchCase::IgnoreCase))
        {
            OutValue = MakeShared<FJsonValueBoolean>(Value.ToBool());
            return true;
        }
        if (Value.IsNumeric())
        {
            OutValue = MakeShared<FJsonValueNumber>(FCString::Atod(*Value));
            return true;
        }
    }

    OutValue = MakeShared<FJsonValueString>(Value);
    return true;
}

bool FUnrealMCPBlueprintGraphOps::SetPinDefaultValueFromString(UEdGraphNode* Node, const FString& PinName,
                                                              const FString& Value, const FString& ValueKind,
                                                              FString& OutErrorCode, FString& OutErrorMessage,
                                                              TArray<FString>& OutCandidates)
{
    TSharedPtr<FJsonValue> ValueJson;
    FString ValueErrorMessage;
    if (!MakeJsonValueFromString(Value, ValueKind, ValueJson, ValueErrorMessage))
    {
        OutErrorCode = EUnrealMCPGraphError::InvalidValue;
        OutErrorMessage = FString::Printf(TEXT("Pin '%s': %s"), *PinName, *ValueErrorMessage);
        OutCandidates.Reset();
        return false;
    }

    return SetPinDefaultValue(Node, PinName, ValueJson, OutErrorCode, OutErrorMessage, OutCandidates);
}

bool FUnrealMCPBlueprintGraphOps::DisconnectPin(UEdGraphNode* Node, const FString& PinName,
                                                const FString& LinkedNodeId, const FString& LinkedPinName,
                                                int32& OutDisconnectedCount, int32& OutRemainingLinks,
                                                FString& OutErrorCode, FString& OutErrorMessage,
                                                TArray<FString>& OutCandidates)
{
    OutDisconnectedCount = 0;
    OutRemainingLinks = 0;
    OutErrorCode.Reset();
    OutErrorMessage.Reset();
    OutCandidates.Reset();

    if (!Node)
    {
        OutErrorCode = EUnrealMCPGraphError::NodeNotFound;
        OutErrorMessage = TEXT("Invalid node");
        return false;
    }

    // Either direction: the pin name decides, not the caller.
    UEdGraphPin* Pin = FindPin(Node, PinName, EGPD_MAX);
    if (!Pin)
    {
        CollectPinNames(Node, EGPD_MAX, OutCandidates);
        OutErrorCode = EUnrealMCPGraphError::PinNotFound;
        OutErrorMessage = FString::Printf(TEXT("Pin '%s' not found on node '%s'. Available pins: %s"),
            *PinName, *Node->NodeGuid.ToString(),
            OutCandidates.Num() > 0 ? *FString::Join(OutCandidates, TEXT(", ")) : TEXT("<none>"));
        return false;
    }

    TArray<UEdGraphPin*> LinksToBreak;
    for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
    {
        if (!LinkedPin || !LinkedPin->GetOwningNode())
        {
            continue;
        }
        if (!LinkedNodeId.IsEmpty() && LinkedPin->GetOwningNode()->NodeGuid.ToString() != LinkedNodeId)
        {
            continue;
        }
        if (!LinkedPinName.IsEmpty() && LinkedPin->PinName.ToString() != LinkedPinName)
        {
            continue;
        }
        LinksToBreak.Add(LinkedPin);
    }

    for (UEdGraphPin* LinkedPin : LinksToBreak)
    {
        Pin->BreakLinkTo(LinkedPin);
    }

    OutDisconnectedCount = LinksToBreak.Num();
    OutRemainingLinks = Pin->LinkedTo.Num();

    if (UEdGraph* Graph = Node->GetGraph())
    {
        MarkModified(Graph);
    }
    return true;
}

bool FUnrealMCPBlueprintGraphOps::DeleteNode(UEdGraph* Graph, UEdGraphNode* Node,
                                             FString& OutErrorCode, FString& OutErrorMessage)
{
    OutErrorCode.Reset();
    OutErrorMessage.Reset();

    if (!Graph || !Node)
    {
        OutErrorCode = EUnrealMCPGraphError::NodeNotFound;
        OutErrorMessage = TEXT("Invalid graph or node");
        return false;
    }

    Graph->RemoveNode(Node, /*bBreakAllLinks=*/true);
    MarkModified(Graph);
    return true;
}

// ---------------------------------------------------------------------------
// Variable types
// ---------------------------------------------------------------------------

TArray<FString> FUnrealMCPBlueprintGraphOps::SupportedVariableTypes()
{
    return {
        TEXT("bool"), TEXT("byte"), TEXT("int"), TEXT("int64"), TEXT("float"), TEXT("double"),
        TEXT("name"), TEXT("string"), TEXT("text"),
        TEXT("object"), TEXT("class"), TEXT("struct"), TEXT("enum"),
        TEXT("vector"), TEXT("vector2d"), TEXT("rotator"), TEXT("transform"), TEXT("linear_color"),
        TEXT("bool[]"), TEXT("int[]"), TEXT("float[]"), TEXT("string[]"), TEXT("object[]"),
        TEXT("struct{}"), TEXT("int{,}")
    };
}

void FUnrealMCPBlueprintGraphOps::SplitContainerType(const FString& TypeString, FString& OutBaseType,
                                                     int32& OutContainerKind)
{
    OutBaseType = TypeString.TrimStartAndEnd();
    OutContainerKind = 0;

    if (OutBaseType.EndsWith(TEXT("[]")))
    {
        OutBaseType = OutBaseType.LeftChop(2);
        OutContainerKind = 1; // Array
    }
    else if (OutBaseType.EndsWith(TEXT("{,}")))
    {
        OutBaseType = OutBaseType.LeftChop(3);
        OutContainerKind = 3; // Map
    }
    else if (OutBaseType.EndsWith(TEXT("{}")))
    {
        OutBaseType = OutBaseType.LeftChop(2);
        OutContainerKind = 2; // Set
    }

    OutBaseType = OutBaseType.TrimStartAndEnd().ToLower();
}

bool FUnrealMCPBlueprintGraphOps::BuildVariablePinType(const FString& TypeString, const FString& SubClass,
                                                       FEdGraphPinType& OutPinType, FString& OutErrorMessage,
                                                       TArray<FString>& OutSupportedTypes)
{
    OutPinType = FEdGraphPinType();
    OutErrorMessage.Reset();
    OutSupportedTypes = SupportedVariableTypes();

    FString BaseType;
    int32 ContainerKind = 0;
    SplitContainerType(TypeString, BaseType, ContainerKind);

    switch (ContainerKind)
    {
    case 1: OutPinType.ContainerType = EPinContainerType::Array; break;
    case 2: OutPinType.ContainerType = EPinContainerType::Set; break;
    case 3: OutPinType.ContainerType = EPinContainerType::Map; break;
    default: OutPinType.ContainerType = EPinContainerType::None; break;
    }

    auto Fail = [&OutErrorMessage, &TypeString](const TCHAR* Why)
    {
        OutErrorMessage = FString::Printf(TEXT("Unsupported variable type '%s': %s"), *TypeString, Why);
        return false;
    };

    if (BaseType == TEXT("bool") || BaseType == TEXT("boolean"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    }
    else if (BaseType == TEXT("byte"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
    }
    else if (BaseType == TEXT("int") || BaseType == TEXT("integer") || BaseType == TEXT("int32"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
    }
    else if (BaseType == TEXT("int64") || BaseType == TEXT("long"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
    }
    else if (BaseType == TEXT("float") || BaseType == TEXT("real"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
        OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
    }
    else if (BaseType == TEXT("double"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
        OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
    }
    else if (BaseType == TEXT("name"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
    }
    else if (BaseType == TEXT("string"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
    }
    else if (BaseType == TEXT("text"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
    }
    else if (BaseType == TEXT("object"))
    {
        UClass* ObjectClass = UObject::StaticClass();
        if (!SubClass.IsEmpty())
        {
            TArray<FString> Candidates;
            if (!ResolveClass(SubClass, ObjectClass, Candidates))
            {
                return Fail(*FString::Printf(TEXT("class '%s' could not be resolved"), *SubClass));
            }
        }
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
        OutPinType.PinSubCategoryObject = ObjectClass;
    }
    else if (BaseType == TEXT("class"))
    {
        UClass* MetaClass = UObject::StaticClass();
        if (!SubClass.IsEmpty())
        {
            TArray<FString> Candidates;
            if (!ResolveClass(SubClass, MetaClass, Candidates))
            {
                return Fail(*FString::Printf(TEXT("class '%s' could not be resolved"), *SubClass));
            }
        }
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Class;
        OutPinType.PinSubCategoryObject = MetaClass;
    }
    else if (BaseType == TEXT("struct"))
    {
        UScriptStruct* LoadedStruct = SubClass.IsEmpty()
            ? nullptr
            : (LoadObject<UScriptStruct>(nullptr, *SubClass));
        if (!LoadedStruct)
        {
            return Fail(TEXT("a struct asset path is required in 'sub_class'"));
        }
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
        OutPinType.PinSubCategoryObject = LoadedStruct;
    }
    else if (BaseType == TEXT("enum"))
    {
        UEnum* LoadedEnum = SubClass.IsEmpty() ? nullptr : LoadObject<UEnum>(nullptr, *SubClass);
        if (!LoadedEnum)
        {
            return Fail(TEXT("an enum asset path is required in 'sub_class'"));
        }
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
        OutPinType.PinSubCategoryObject = LoadedEnum;
    }
    else if (BaseType == TEXT("vector"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
        OutPinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
    }
    else if (BaseType == TEXT("vector2d"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
        OutPinType.PinSubCategoryObject = TBaseStructure<FVector2D>::Get();
    }
    else if (BaseType == TEXT("rotator"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
        OutPinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
    }
    else if (BaseType == TEXT("transform"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
        OutPinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
    }
    else if (BaseType == TEXT("linear_color") || BaseType == TEXT("linearcolor") || BaseType == TEXT("color"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
        OutPinType.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get();
    }
    else
    {
        return Fail(TEXT("unknown base type"));
    }

    return true;
}

// ---------------------------------------------------------------------------
// Compile
// ---------------------------------------------------------------------------

void FUnrealMCPBlueprintGraphOps::ResetCompilerMessages(UBlueprint* Blueprint)
{
    if (!Blueprint)
    {
        return;
    }

    // UEdGraphNode::ErrorMsg / bHasCompilerMessage are only rewritten when the compiler visits the node
    // again: after a blueprint that failed to compile is fixed, the nodes keep the old message (observed
    // on BP_TPSDemoGameMode, whose two Array_Length nodes stayed "Target Array type not determined" in
    // the diagnostics while Status was already BS_UpToDate). Clear them so what the compile commands
    // report is this compile's result.
    auto ResetGraph = [](const UEdGraph* Graph)
    {
        if (!Graph)
        {
            return;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node)
            {
                Node->ErrorMsg.Empty();
                Node->bHasCompilerMessage = false;
            }
        }
    };

    for (UEdGraph* Graph : Blueprint->UbergraphPages)
    {
        ResetGraph(Graph);
    }
    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        ResetGraph(Graph);
    }
    for (UEdGraph* Graph : Blueprint->MacroGraphs)
    {
        ResetGraph(Graph);
    }
    for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
    {
        ResetGraph(Graph);
    }
}

bool FUnrealMCPBlueprintGraphOps::CompileChecked(UBlueprint* Blueprint, FString& OutStatus, bool& bOutCompiled,
                                                 TArray<FString>& OutErrors, TArray<FString>& OutWarnings)
{
    OutStatus.Reset();
    bOutCompiled = false;
    OutErrors.Reset();
    OutWarnings.Reset();

    if (!Blueprint)
    {
        return false;
    }

    ResetCompilerMessages(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint);

    const UEnum* StatusEnum = StaticEnum<EBlueprintStatus>();
    OutStatus = StatusEnum
        ? StatusEnum->GetNameStringByValue(static_cast<int64>(Blueprint->Status))
        : FString::FromInt(static_cast<int32>(Blueprint->Status));

    auto CollectGraphDiagnostics = [&OutErrors, &OutWarnings](const UEdGraph* Graph)
    {
        if (!Graph)
        {
            return;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node || Node->ErrorMsg.IsEmpty())
            {
                continue;
            }

            const FString Entry = FString::Printf(TEXT("[%s] %s: %s (%s)"),
                *Graph->GetName(), *Node->NodeGuid.ToString(),
                *Node->ErrorMsg, *Node->GetNodeTitle(ENodeTitleType::ListView).ToString());

            if (Node->bHasCompilerMessage && Node->ErrorType == EMessageSeverity::Warning)
            {
                OutWarnings.Add(Entry);
            }
            else
            {
                OutErrors.Add(Entry);
            }
        }
    };

    for (UEdGraph* Graph : Blueprint->UbergraphPages)
    {
        CollectGraphDiagnostics(Graph);
    }
    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        CollectGraphDiagnostics(Graph);
    }

    // Engine semantics (same as UBlueprint::IsUpToDate): warnings still mean "compiled".
    bOutCompiled = Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings;
    return true;
}
