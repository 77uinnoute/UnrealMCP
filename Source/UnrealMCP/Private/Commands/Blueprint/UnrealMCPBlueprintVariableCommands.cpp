#include "Commands/Blueprint/UnrealMCPBlueprintCommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Commands/Blueprint/UnrealMCPBlueprintGraphOps.h"
#include "Commands/Blueprint/UnrealMCPBlueprintCommandHelpers.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

// Member and local variables of a Blueprint: the commands and the JSON shapes they answer with.
// Still the same handler class (FUnrealMCPBlueprintCommands) - only the file changed, so the
// registry, the command surface and the python layer are untouched.

using namespace UnrealMCPBlueprintHelpers;

namespace
{
    /** Names of the blueprint's own member variables (for error candidates). */
    TArray<FString> MakeMemberVariableNames(const UBlueprint* Blueprint)
    {
        TArray<FString> Names;
        if (Blueprint)
        {
            for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
            {
                Names.Add(Variable.VarName.ToString());
            }
        }
        return Names;
    }

    bool HasTruthyMetaData(const FBPVariableDescription& Variable, const FName& Key)
    {
        return Variable.HasMetaData(Key) && Variable.GetMetaData(Key) != TEXT("false");
    }

    TSharedPtr<FJsonObject> MakeVariableFlagsJson(const FBPVariableDescription& Variable)
    {
        TSharedPtr<FJsonObject> Flags = MakeShared<FJsonObject>();
        Flags->SetBoolField(TEXT("instance_editable"), (Variable.PropertyFlags & CPF_DisableEditOnInstance) == 0);
        Flags->SetBoolField(TEXT("expose_on_spawn"), HasTruthyMetaData(Variable, FBlueprintMetadata::MD_ExposeOnSpawn));
        Flags->SetBoolField(TEXT("blueprint_read_only"), (Variable.PropertyFlags & CPF_BlueprintReadOnly) != 0);
        Flags->SetBoolField(TEXT("replicated"), (Variable.PropertyFlags & CPF_Net) != 0);
        Flags->SetStringField(TEXT("rep_notify_func"), Variable.RepNotifyFunc.ToString());
        Flags->SetStringField(TEXT("category"), Variable.Category.ToString());
        Flags->SetStringField(TEXT("tooltip"),
            Variable.HasMetaData(FBlueprintMetadata::MD_Tooltip) ? Variable.GetMetaData(FBlueprintMetadata::MD_Tooltip) : FString());
        Flags->SetBoolField(TEXT("transient"), (Variable.PropertyFlags & CPF_Transient) != 0);
        Flags->SetBoolField(TEXT("private"), HasTruthyMetaData(Variable, FBlueprintMetadata::MD_Private));
        return Flags;
    }

    /** Member variables pass their blueprint so the default is read off the CDO; locals pass null. */
    TSharedPtr<FJsonObject> MakeVariableJson(const FBPVariableDescription& Variable, const UBlueprint* MemberOf = nullptr)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("variable_name"), Variable.VarName.ToString());
        Obj->SetStringField(TEXT("type"), Variable.VarType.PinCategory.ToString());
        Obj->SetStringField(TEXT("sub_class"), MakeSubClassString(Variable.VarType));
        Obj->SetStringField(TEXT("container"), MakeContainerName(Variable.VarType));
        if (MemberOf)
        {
            WriteMemberDefaultValue(MemberOf, Variable, Obj);
        }
        else
        {
            Obj->SetStringField(TEXT("default_value"), Variable.DefaultValue);
        }
        // is_exposed is the editor's "instance editable" eye: the same bit set_blueprint_variable_flags
        // (instance_editable=...) writes and the same one flags.instance_editable reports. It used to
        // report CPF_Edit here while the flag command wrote CPF_DisableEditOnInstance - two names for
        // "instance editable" reading two different bits, which made is_exposed=false a silent no-op.
        Obj->SetBoolField(TEXT("is_exposed"), (Variable.PropertyFlags & CPF_DisableEditOnInstance) == 0);
        Obj->SetBoolField(TEXT("class_editable"), (Variable.PropertyFlags & CPF_Edit) != 0);
        Obj->SetObjectField(TEXT("flags"), MakeVariableFlagsJson(Variable));
        return Obj;
    }

    /**
     * A graph's local variables live on its function entry node, while the engine's local-variable
     * API is keyed by the UFunction scope (whose name is the graph's). Resolve one from the other
     * so neither call has to guess at compile state.
     */
    UFunction* FindScopeFunctionForGraph(UBlueprint* Blueprint, UEdGraph* Graph)
    {
        if (!Blueprint || !Graph)
        {
            return nullptr;
        }

        const FName GraphName = Graph->GetFName();
        auto Lookup = [GraphName](UClass* Class) -> UFunction*
        {
            return Class ? Class->FindFunctionByName(GraphName) : nullptr;
        };

        if (UFunction* Function = Lookup(Blueprint->SkeletonGeneratedClass))
        {
            return Function;
        }
        if (UFunction* Function = Lookup(Blueprint->GeneratedClass))
        {
            return Function;
        }

        // The skeleton class only mirrors the graph set after a compile.
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
        if (UFunction* Function = Lookup(Blueprint->SkeletonGeneratedClass))
        {
            return Function;
        }
        return Lookup(Blueprint->GeneratedClass);
    }

    UEdGraph* FindFunctionGraphByName(UBlueprint* Blueprint, const FString& FunctionName, TArray<FString>& OutCandidates)
    {
        TArray<UEdGraph*> Graphs;
        FUnrealMCPBlueprintGraphOps::ListFunctionGraphs(Blueprint, Graphs);
        UEdGraph* Found = nullptr;
        for (UEdGraph* Candidate : Graphs)
        {
            if (!Candidate)
            {
                continue;
            }
            OutCandidates.Add(Candidate->GetName());
            if (!Found && Candidate->GetName() == FunctionName)
            {
                Found = Candidate;
            }
        }
        return Found;
    }

    /**
     * Resolve the entry node of a function graph for local-variable editing.
     * Reports function_graph_not_found (with the blueprint's function graphs as candidates) and
     * function_not_editable for an override built with signature_class, whose signature is the parent's.
     */
    UK2Node_FunctionEntry* ResolveLocalVariableScope(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params,
                                                     FString& OutFunctionName, TSharedPtr<FJsonObject>& OutError)
    {
        if (!Params->TryGetStringField(TEXT("function_name"), OutFunctionName))
        {
            OutError = FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
                TEXT("Missing 'function_name' parameter"));
            return nullptr;
        }

        TArray<FString> Candidates;
        UEdGraph* Graph = FindFunctionGraphByName(Blueprint, OutFunctionName, Candidates);
        if (!Graph)
        {
            OutError = MakeCandidatesError(EUnrealMCPGraphError::FunctionGraphNotFound,
                FString::Printf(TEXT("No function graph named '%s'"), *OutFunctionName), Candidates);
            return nullptr;
        }

        UK2Node_FunctionEntry* Entry = FUnrealMCPBlueprintGraphOps::FindFunctionEntryNode(Graph);
        if (!Entry)
        {
            OutError = FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::FunctionGraphNotFound,
                FString::Printf(TEXT("'%s' is not a function graph"), *OutFunctionName));
            return nullptr;
        }

        if (!Entry->IsEditable())
        {
            OutError = FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::FunctionNotEditable,
                FString::Printf(TEXT("Function '%s' overrides a parent signature; its local variables cannot be edited"),
                    *OutFunctionName));
            return nullptr;
        }
        return Entry;
    }

    TArray<TSharedPtr<FJsonValue>> MakeVariableJsonArray(const UBlueprint* Blueprint)
    {
        TArray<TSharedPtr<FJsonValue>> Items;
        if (Blueprint)
        {
            for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
            {
                Items.Add(MakeShared<FJsonValueObject>(MakeVariableJson(Variable, Blueprint)));
            }
        }
        return Items;
    }

    TArray<TSharedPtr<FJsonValue>> MakeLocalVariableJsonArray(const UK2Node_FunctionEntry* Entry)
    {
        TArray<TSharedPtr<FJsonValue>> Items;
        if (Entry)
        {
            for (const FBPVariableDescription& Variable : Entry->LocalVariables)
            {
                Items.Add(MakeShared<FJsonValueObject>(MakeVariableJson(Variable)));
            }
        }
        return Items;
    }

    /** "int" plus container=array becomes "int[]": one type grammar serves both spellings. */
    bool MergeContainerIntoTypeString(FString& InOutType, const FString& Container, FString& OutErrorMessage)
    {
        if (Container.IsEmpty())
        {
            return true;
        }

        FString Suffix;
        if (Container == TEXT("none") || Container == TEXT("single"))
        {
            Suffix.Reset();
        }
        else if (Container == TEXT("array"))
        {
            Suffix = TEXT("[]");
        }
        else if (Container == TEXT("set"))
        {
            Suffix = TEXT("{}");
        }
        else if (Container == TEXT("map"))
        {
            Suffix = TEXT("{,}");
        }
        else
        {
            OutErrorMessage = FString::Printf(
                TEXT("Unknown container '%s'. Use none / array / set / map, or write the suffix on variable_type"),
                *Container);
            return false;
        }

        FString BaseType;
        int32 ContainerKind = 0;
        FUnrealMCPBlueprintGraphOps::SplitContainerType(InOutType, BaseType, ContainerKind);
        if (ContainerKind != 0 && !Suffix.IsEmpty())
        {
            OutErrorMessage = FString::Printf(
                TEXT("variable_type '%s' already carries a container; drop 'container' or the suffix"), *InOutType);
            return false;
        }

        InOutType = Suffix.IsEmpty() ? BaseType : BaseType + Suffix;
        return true;
    }

}

// =====================================================================================
// Member variables
// =====================================================================================

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleListBlueprintVariables(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintOrNull(Params, TEXT("blueprint_name"), Error);
    if (!Blueprint)
    {
        return Error;
    }

    TArray<TSharedPtr<FJsonValue>> Variables;
    for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
    {
        Variables.Add(MakeShared<FJsonValueObject>(MakeVariableJson(Variable, Blueprint)));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetArrayField(TEXT("variables"), Variables);
    ResultObj->SetNumberField(TEXT("variable_count"), Variables.Num());
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleGetBlueprintVariableInfo(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintOrNull(Params, TEXT("blueprint_name"), Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString VariableName;
    if (!Params->TryGetStringField(TEXT("variable_name"), VariableName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("Missing 'variable_name' parameter"));
    }

    const FBPVariableDescription* Variable = FindMutableMemberVariable(Blueprint, FName(*VariableName));
    if (!Variable)
    {
        return MakeCandidatesError(EUnrealMCPGraphError::VariableNotFound,
            FString::Printf(TEXT("Variable '%s' not found on blueprint '%s'"), *VariableName, *Blueprint->GetName()),
            MakeMemberVariableNames(Blueprint));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeVariableJson(*Variable, Blueprint);
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleRemoveBlueprintVariable(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintOrNull(Params, TEXT("blueprint_name"), Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString VariableName;
    if (!Params->TryGetStringField(TEXT("variable_name"), VariableName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("Missing 'variable_name' parameter"));
    }

    if (!FindMutableMemberVariable(Blueprint, FName(*VariableName)))
    {
        return MakeCandidatesError(EUnrealMCPGraphError::VariableNotFound,
            FString::Printf(TEXT("Variable '%s' not found on blueprint '%s'"), *VariableName, *Blueprint->GetName()),
            MakeMemberVariableNames(Blueprint));
    }

    // RemoveMemberVariable drops the description; RemoveVariableNodes drops the Get/Set nodes that
    // referenced it. Both are needed: a variable that is gone while the graph still references it
    // would only surface later as a blueprints-that-does-not-compile.
    FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, FName(*VariableName));
    FBlueprintEditorUtils::RemoveVariableNodes(Blueprint, FName(*VariableName));

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetStringField(TEXT("removed"), VariableName);
    ResultObj->SetArrayField(TEXT("variables"), MakeVariableJsonArray(Blueprint));
    ResultObj->SetNumberField(TEXT("variable_count"), Blueprint->NewVariables.Num());
    AppendCompileResult(Blueprint, ResultObj);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleRenameBlueprintVariable(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintOrNull(Params, TEXT("blueprint_name"), Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString OldName;
    FString NewName;
    if (!Params->TryGetStringField(TEXT("old_name"), OldName) ||
        !Params->TryGetStringField(TEXT("new_name"), NewName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("Both 'old_name' and 'new_name' are required"));
    }

    FBPVariableDescription* Variable = FindMutableMemberVariable(Blueprint, FName(*OldName));
    if (!Variable)
    {
        return MakeCandidatesError(EUnrealMCPGraphError::VariableNotFound,
            FString::Printf(TEXT("Variable '%s' not found on blueprint '%s'"), *OldName, *Blueprint->GetName()),
            MakeMemberVariableNames(Blueprint));
    }

    // RenameMemberVariable does not check for collisions, and the engine's own rename path pops up a
    // modal dialog when the variable has an OnRep function. Both are handled here: a collision is a
    // structured error, and the OnRep link is broken up front exactly as that dialog's "Yes" does.
    if (FindMutableMemberVariable(Blueprint, FName(*NewName)))
    {
        return MakeCandidatesError(EUnrealMCPGraphError::VariableNameInUse,
            FString::Printf(TEXT("A variable named '%s' already exists on blueprint '%s'"), *NewName, *Blueprint->GetName()),
            MakeMemberVariableNames(Blueprint));
    }

    FString ClearedRepNotify;
    if (Variable->RepNotifyFunc != NAME_None)
    {
        ClearedRepNotify = Variable->RepNotifyFunc.ToString();
        Variable->RepNotifyFunc = NAME_None;
    }

    FBlueprintEditorUtils::RenameMemberVariable(Blueprint, FName(*OldName), FName(*NewName));

    const FBPVariableDescription* Renamed = FindMutableMemberVariable(Blueprint, FName(*NewName));
    if (!Renamed)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidValue,
            FString::Printf(TEXT("Renaming '%s' to '%s' did not take effect"), *OldName, *NewName));
    }

    // The graph nodes follow the rename through ReplaceVariableReferences; read the result back so a
    // node left pointing at the old name is visible instead of silent.
    int32 MovedNodeCount = 0;
    int32 StaleNodeCount = 0;
    TArray<UEdGraph*> AllGraphs;
    Blueprint->GetAllGraphs(AllGraphs);
    for (UEdGraph* Graph : AllGraphs)
    {
        if (!Graph)
        {
            continue;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (const UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node))
            {
                const FName Referenced = VariableNode->VariableReference.GetMemberName();
                if (Referenced == FName(*NewName))
                {
                    ++MovedNodeCount;
                }
                else if (Referenced == FName(*OldName))
                {
                    ++StaleNodeCount;
                }
            }
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetStringField(TEXT("old_name"), OldName);
    ResultObj->SetStringField(TEXT("new_name"), NewName);
    ResultObj->SetObjectField(TEXT("variable"), MakeVariableJson(*Renamed, Blueprint));
    ResultObj->SetNumberField(TEXT("reference_nodes_renamed"), MovedNodeCount);
    ResultObj->SetNumberField(TEXT("reference_nodes_stale"), StaleNodeCount);
    if (!ClearedRepNotify.IsEmpty())
    {
        ResultObj->SetStringField(TEXT("rep_notify_cleared"), ClearedRepNotify);
    }
    AppendCompileResult(Blueprint, ResultObj);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleSetBlueprintVariableType(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintOrNull(Params, TEXT("blueprint_name"), Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString VariableName;
    FString VariableType;
    if (!Params->TryGetStringField(TEXT("variable_name"), VariableName) ||
        !Params->TryGetStringField(TEXT("variable_type"), VariableType))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("Both 'variable_name' and 'variable_type' are required"));
    }

    if (!FindMutableMemberVariable(Blueprint, FName(*VariableName)))
    {
        return MakeCandidatesError(EUnrealMCPGraphError::VariableNotFound,
            FString::Printf(TEXT("Variable '%s' not found on blueprint '%s'"), *VariableName, *Blueprint->GetName()),
            MakeMemberVariableNames(Blueprint));
    }

    FString Container;
    Params->TryGetStringField(TEXT("container"), Container);
    FString ContainerError;
    if (!MergeContainerIntoTypeString(VariableType, Container, ContainerError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::UnsupportedContainerType, ContainerError);
    }

    FString SubClass;
    Params->TryGetStringField(TEXT("sub_class"), SubClass);

    FEdGraphPinType PinType;
    FString TypeError;
    TArray<FString> SupportedTypes;
    if (!FUnrealMCPBlueprintGraphOps::BuildVariablePinType(VariableType, SubClass, PinType, TypeError, SupportedTypes))
    {
        TSharedPtr<FJsonObject> TypeErrorObj = FUnrealMCPCommonUtils::CreateErrorResponse(
            EUnrealMCPGraphError::UnsupportedVariableType, TypeError);
        TypeErrorObj->SetArrayField(TEXT("supported_types"), MakeStringArray(SupportedTypes));
        return TypeErrorObj;
    }

    // Everything below this point is refused when the variable is already used by graph nodes.
    //
    // ChangeMemberVariableType rebuilds the pins of every referencing node, and on that shape the
    // call has been measured to freeze the game thread dead: the editor stays alive, the port keeps
    // listening, no further log line appears and the client times out (Docs/MCP_Findings_2026-10-06
    // _platformer-round.md section 3). Whether the engine put up a modal or looped internally is
    // still unknown, so there is nothing here that could be answered or timed out - refusing before
    // the call is the only containment there is: force=true was tried and removed (measured - it did not
    // apply the change, it put the dialog up anyway, and answering that dialog by hand did not apply it
    // either), and the dialog turned out NOT to travel through FCoreDelegates::ModalMessageDialog, so the
    // auto-answer scope could not take it. Nothing can be answered here; refusal is absolute.

    TArray<TSharedPtr<FJsonValue>> ReferencingNodes;
    TArray<UEdGraph*> AllGraphs;
    Blueprint->GetAllGraphs(AllGraphs);
    for (UEdGraph* Graph : AllGraphs)
    {
        if (!Graph)
        {
            continue;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            const UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node);
            if (VariableNode && VariableNode->VariableReference.GetMemberName() == FName(*VariableName))
            {
                TSharedPtr<FJsonObject> NodeJson = MakeShared<FJsonObject>();
                NodeJson->SetStringField(TEXT("graph"), Graph->GetName());
                NodeJson->SetStringField(TEXT("node"), Node->GetName());
                NodeJson->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
                ReferencingNodes.Add(MakeShareable(new FJsonValueObject(NodeJson)));
            }
        }
    }

    if (ReferencingNodes.Num() > 0)
    {
        TSharedPtr<FJsonObject> Refusal = FUnrealMCPCommonUtils::CreateErrorResponse(
            EUnrealMCPGraphError::VariableReferencedByNodes,
            FString::Printf(TEXT("'%s' is referenced by %d graph node(s). Changing a referenced variable's type "
                                 "has been measured to put up a modal dialog that holds the editor's game thread "
                                 "until a human clicks it (the MCP bridge stops answering meanwhile), and the "
                                 "retype does not take effect even then. Remove the variable and re-add it with "
                                 "the target type instead, then rebuild the referencing nodes."),
                            *VariableName, ReferencingNodes.Num()));
        Refusal->SetArrayField(TEXT("referencing_nodes"), ReferencingNodes);
        Refusal->SetStringField(TEXT("hint"),
            TEXT("remove_blueprint_variable (it clears the referencing nodes too) -> add_blueprint_variable with "
                 "the target type -> rebuild the nodes that used it"));
        return Refusal;
    }

    FBlueprintEditorUtils::ChangeMemberVariableType(Blueprint, FName(*VariableName), PinType);

    const FBPVariableDescription* Variable = FindMutableMemberVariable(Blueprint, FName(*VariableName));
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetStringField(TEXT("variable_name"), VariableName);
    if (Variable)
    {
        // Read the type back off the variable rather than trusting the request.
        ResultObj->SetObjectField(TEXT("variable"), MakeVariableJson(*Variable, Blueprint));
        ResultObj->SetStringField(TEXT("type"), Variable->VarType.PinCategory.ToString());
        ResultObj->SetStringField(TEXT("container"), MakeContainerName(Variable->VarType));
        if (Variable->VarType.PinCategory != PinType.PinCategory)
        {
            // The engine can drop the change and still hand back a clean compile (measured: a referenced
            // variable kept its old type while the receipt carried compiled=true). Say it outright instead
            // of letting that read as a retype.
            ResultObj->SetBoolField(TEXT("type_change_not_applied"), true);
            ResultObj->SetStringField(TEXT("hint"),
                TEXT("the engine did not apply the type change; use remove_blueprint_variable -> "
                     "add_blueprint_variable with the target type -> rebuild the referencing nodes"));
        }
    }
    AppendCompileResult(Blueprint, ResultObj);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleSetBlueprintVariableDefaultValue(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintOrNull(Params, TEXT("blueprint_name"), Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString VariableName;
    FString DefaultValue;
    if (!Params->TryGetStringField(TEXT("variable_name"), VariableName) ||
        !Params->TryGetStringField(TEXT("default_value"), DefaultValue))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("Both 'variable_name' and 'default_value' are required"));
    }

    FBPVariableDescription* Variable = FindMutableMemberVariable(Blueprint, FName(*VariableName));
    if (!Variable)
    {
        return MakeCandidatesError(EUnrealMCPGraphError::VariableNotFound,
            FString::Printf(TEXT("Variable '%s' not found on blueprint '%s'"), *VariableName, *Blueprint->GetName()),
            MakeMemberVariableNames(Blueprint));
    }

    // The compiler parses FBPVariableDescription::DefaultValue into the class default object
    // (KismetCompiler SetPropertyDefaultValue), so writing it here and recompiling is the whole
    // "keep the CDO in sync" story - and a value that cannot be parsed surfaces in `errors`.
    Variable->DefaultValue = DefaultValue;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetStringField(TEXT("variable_name"), VariableName);
    AppendCompileResult(Blueprint, ResultObj);
    // Read back after the compile: the compiler moves the value into the CDO (and clears the
    // description for struct/container types).
    if (const FBPVariableDescription* Written = FindMutableMemberVariable(Blueprint, FName(*VariableName)))
    {
        WriteMemberDefaultValue(Blueprint, *Written, ResultObj);
    }
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleSetBlueprintVariableFlags(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintOrNull(Params, TEXT("blueprint_name"), Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString VariableName;
    if (!Params->TryGetStringField(TEXT("variable_name"), VariableName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("Missing 'variable_name' parameter"));
    }

    const TSharedPtr<FJsonObject>* Props = nullptr;
    if (!Params->TryGetObjectField(TEXT("props"), Props) || !Props || !Props->IsValid())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("Missing 'props' parameter (object of flag name to value)"));
    }

    if (!FindMutableMemberVariable(Blueprint, FName(*VariableName)))
    {
        return MakeCandidatesError(EUnrealMCPGraphError::VariableNotFound,
            FString::Printf(TEXT("Variable '%s' not found on blueprint '%s'"), *VariableName, *Blueprint->GetName()),
            MakeMemberVariableNames(Blueprint));
    }

    const TArray<FString> KnownFlags = {
        TEXT("instance_editable"), TEXT("expose_on_spawn"), TEXT("blueprint_read_only"), TEXT("replicated"),
        TEXT("rep_notify_func"), TEXT("category"), TEXT("tooltip"), TEXT("transient"), TEXT("private")
    };

    const FName VarName(*VariableName);
    TArray<TSharedPtr<FJsonValue>> Applied;
    TArray<TSharedPtr<FJsonValue>> Failed;

    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Props)->Values)
    {
        const FString Key = Pair.Key;
        const TSharedPtr<FJsonValue> Value = Pair.Value;

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("property"), Key);
        if (Value.IsValid())
        {
            Entry->SetField(TEXT("requested"), Value);
        }

        const bool bKnown = KnownFlags.Contains(Key);
        if (!bKnown)
        {
            Entry->SetStringField(TEXT("error_code"), EUnrealMCPGraphError::InvalidParams);
            Entry->SetStringField(TEXT("error"), FString::Printf(TEXT("Unknown variable flag '%s'"), *Key));
            Entry->SetArrayField(TEXT("candidates"), MakeStringArray(KnownFlags));
            Failed.Add(MakeShared<FJsonValueObject>(Entry));
            continue;
        }

        auto RequireBool = [&Entry, &Value](bool& OutBool) -> bool
        {
            if (!Value.IsValid() || Value->Type != EJson::Boolean)
            {
                Entry->SetStringField(TEXT("error_code"), EUnrealMCPGraphError::InvalidParams);
                Entry->SetStringField(TEXT("error"), TEXT("This flag expects true or false"));
                return false;
            }
            OutBool = Value->AsBool();
            return true;
        };

        auto RequireString = [&Entry, &Value](FString& OutString) -> bool
        {
            if (!Value.IsValid() || Value->Type != EJson::String)
            {
                Entry->SetStringField(TEXT("error_code"), EUnrealMCPGraphError::InvalidParams);
                Entry->SetStringField(TEXT("error"), TEXT("This flag expects a string"));
                return false;
            }
            OutString = Value->AsString();
            return true;
        };

        if (Key == TEXT("instance_editable"))
        {
            bool bValue = false;
            if (!RequireBool(bValue)) { Failed.Add(MakeShared<FJsonValueObject>(Entry)); continue; }
            FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, VarName, !bValue);
        }
        else if (Key == TEXT("blueprint_read_only"))
        {
            bool bValue = false;
            if (!RequireBool(bValue)) { Failed.Add(MakeShared<FJsonValueObject>(Entry)); continue; }
            FBlueprintEditorUtils::SetBlueprintPropertyReadOnlyFlag(Blueprint, VarName, bValue);
        }
        else if (Key == TEXT("expose_on_spawn"))
        {
            bool bValue = false;
            if (!RequireBool(bValue)) { Failed.Add(MakeShared<FJsonValueObject>(Entry)); continue; }
            if (bValue)
            {
                FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VarName, nullptr,
                    FBlueprintMetadata::MD_ExposeOnSpawn, TEXT("true"));
            }
            else
            {
                FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(Blueprint, VarName, nullptr,
                    FBlueprintMetadata::MD_ExposeOnSpawn);
            }
        }
        else if (Key == TEXT("private"))
        {
            bool bValue = false;
            if (!RequireBool(bValue)) { Failed.Add(MakeShared<FJsonValueObject>(Entry)); continue; }
            if (bValue)
            {
                FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VarName, nullptr,
                    FBlueprintMetadata::MD_Private, TEXT("true"));
            }
            else
            {
                FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(Blueprint, VarName, nullptr,
                    FBlueprintMetadata::MD_Private);
            }
        }
        else if (Key == TEXT("tooltip"))
        {
            FString StringValue;
            if (!RequireString(StringValue)) { Failed.Add(MakeShared<FJsonValueObject>(Entry)); continue; }
            if (StringValue.IsEmpty())
            {
                FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(Blueprint, VarName, nullptr,
                    FBlueprintMetadata::MD_Tooltip);
            }
            else
            {
                FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VarName, nullptr,
                    FBlueprintMetadata::MD_Tooltip, StringValue);
            }
        }
        else if (Key == TEXT("category"))
        {
            FString StringValue;
            if (!RequireString(StringValue)) { Failed.Add(MakeShared<FJsonValueObject>(Entry)); continue; }
            FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, VarName, nullptr, FText::FromString(StringValue));
        }
        else if (Key == TEXT("transient"))
        {
            bool bValue = false;
            if (!RequireBool(bValue)) { Failed.Add(MakeShared<FJsonValueObject>(Entry)); continue; }
            uint64* Flags = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(Blueprint, VarName);
            if (!Flags)
            {
                Entry->SetStringField(TEXT("error_code"), EUnrealMCPGraphError::VariableNotFound);
                Entry->SetStringField(TEXT("error"), TEXT("The variable's property flags are unavailable"));
                Failed.Add(MakeShared<FJsonValueObject>(Entry));
                continue;
            }
            if (bValue) { *Flags |= CPF_Transient; } else { *Flags &= ~CPF_Transient; }
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        }
        else if (Key == TEXT("replicated"))
        {
            bool bValue = false;
            if (!RequireBool(bValue)) { Failed.Add(MakeShared<FJsonValueObject>(Entry)); continue; }
            uint64* Flags = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(Blueprint, VarName);
            if (!Flags)
            {
                Entry->SetStringField(TEXT("error_code"), EUnrealMCPGraphError::VariableNotFound);
                Entry->SetStringField(TEXT("error"), TEXT("The variable's property flags are unavailable"));
                Failed.Add(MakeShared<FJsonValueObject>(Entry));
                continue;
            }

            if (bValue)
            {
                *Flags |= CPF_Net;
            }
            else
            {
                // Mirror the engine's "Replication = None" branch: the OnRep association goes away too.
                *Flags &= ~CPF_Net;
                *Flags &= ~CPF_RepNotify;
                FBlueprintEditorUtils::SetBlueprintVariableRepNotifyFunc(Blueprint, VarName, NAME_None);
                if (const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName); Index != INDEX_NONE)
                {
                    Blueprint->NewVariables[Index].ReplicationCondition = COND_None;
                }
            }
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        }
        else if (Key == TEXT("rep_notify_func"))
        {
            FString StringValue;
            if (!RequireString(StringValue)) { Failed.Add(MakeShared<FJsonValueObject>(Entry)); continue; }
            const FName FunctionName = StringValue.IsEmpty() ? NAME_None : FName(*StringValue);

            FBlueprintEditorUtils::SetBlueprintVariableRepNotifyFunc(Blueprint, VarName, FunctionName);
            if (uint64* Flags = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(Blueprint, VarName))
            {
                if (FunctionName != NAME_None)
                {
                    *Flags |= CPF_RepNotify | CPF_Net;
                }
                else
                {
                    *Flags &= ~CPF_RepNotify;
                }
            }
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        }

        Entry->SetBoolField(TEXT("applied"), true);
        Applied.Add(MakeShared<FJsonValueObject>(Entry));
    }

    const FBPVariableDescription* Variable = FindMutableMemberVariable(Blueprint, VarName);
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetStringField(TEXT("variable_name"), VariableName);
    ResultObj->SetArrayField(TEXT("applied"), Applied);
    ResultObj->SetArrayField(TEXT("failed"), Failed);
    ResultObj->SetNumberField(TEXT("applied_count"), Applied.Num());
    ResultObj->SetNumberField(TEXT("failed_count"), Failed.Num());
    if (Variable)
    {
        ResultObj->SetObjectField(TEXT("flags"), MakeVariableFlagsJson(*Variable));
    }
    AppendCompileResult(Blueprint, ResultObj);
    return ResultObj;
}

// =====================================================================================
// Local variables of a function graph
// =====================================================================================

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleListBlueprintLocalVariables(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintOrNull(Params, TEXT("blueprint_name"), Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString FunctionName;
    UK2Node_FunctionEntry* Entry = ResolveLocalVariableScope(Blueprint, Params, FunctionName, Error);
    if (!Entry)
    {
        return Error;
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetStringField(TEXT("function_name"), FunctionName);
    ResultObj->SetArrayField(TEXT("variables"), MakeLocalVariableJsonArray(Entry));
    ResultObj->SetNumberField(TEXT("variable_count"), Entry->LocalVariables.Num());
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleAddBlueprintLocalVariable(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintOrNull(Params, TEXT("blueprint_name"), Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString FunctionName;
    UK2Node_FunctionEntry* Entry = ResolveLocalVariableScope(Blueprint, Params, FunctionName, Error);
    if (!Entry)
    {
        return Error;
    }

    FString VariableName;
    FString VariableType;
    if (!Params->TryGetStringField(TEXT("variable_name"), VariableName) ||
        !Params->TryGetStringField(TEXT("variable_type"), VariableType))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("Both 'variable_name' and 'variable_type' are required"));
    }

    const FName VarName(*VariableName);
    if (Entry->LocalVariables.ContainsByPredicate(
        [&VarName](const FBPVariableDescription& V) { return V.VarName == VarName; }))
    {
        return MakeCandidatesError(EUnrealMCPGraphError::VariableNameInUse,
            FString::Printf(TEXT("Local variable '%s' already exists in '%s'"), *VariableName, *FunctionName),
            MakeLocalVariableNames(Entry));
    }

    // A local variable may not mask a member variable of this blueprint or of a parent class.
    TSet<FName> ClassVariables;
    FBlueprintEditorUtils::GetClassVariableList(Blueprint, ClassVariables, /*bIncludePrivateVars=*/true);
    if (ClassVariables.Contains(VarName))
    {
        TArray<FString> Candidates;
        for (const FName& ClassVariable : ClassVariables)
        {
            Candidates.Add(ClassVariable.ToString());
        }
        return MakeCandidatesError(EUnrealMCPGraphError::VariableNameInUse,
            FString::Printf(TEXT("'%s' is already a member variable; a local variable cannot mask it"), *VariableName),
            Candidates);
    }

    FString SubClass;
    Params->TryGetStringField(TEXT("sub_class"), SubClass);
    FString DefaultValue;
    Params->TryGetStringField(TEXT("default_value"), DefaultValue);

    FEdGraphPinType PinType;
    FString TypeError;
    TArray<FString> SupportedTypes;
    if (!FUnrealMCPBlueprintGraphOps::BuildVariablePinType(VariableType, SubClass, PinType, TypeError, SupportedTypes))
    {
        TSharedPtr<FJsonObject> TypeErrorObj = FUnrealMCPCommonUtils::CreateErrorResponse(
            EUnrealMCPGraphError::UnsupportedVariableType, TypeError);
        TypeErrorObj->SetArrayField(TEXT("supported_types"), MakeStringArray(SupportedTypes));
        return TypeErrorObj;
    }

    UEdGraph* Graph = Entry->GetGraph();
    if (!FBlueprintEditorUtils::AddLocalVariable(Blueprint, Graph, VarName, PinType, DefaultValue))
    {
        return MakeCandidatesError(EUnrealMCPGraphError::InvalidValue,
            FString::Printf(TEXT("Could not add local variable '%s' to '%s'"), *VariableName, *FunctionName),
            MakeLocalVariableNames(Entry));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetStringField(TEXT("function_name"), FunctionName);
    ResultObj->SetStringField(TEXT("variable_name"), VariableName);
    ResultObj->SetArrayField(TEXT("variables"), MakeLocalVariableJsonArray(Entry));
    ResultObj->SetNumberField(TEXT("variable_count"), Entry->LocalVariables.Num());
    AppendCompileResult(Blueprint, ResultObj);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleRemoveBlueprintLocalVariable(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintOrNull(Params, TEXT("blueprint_name"), Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString FunctionName;
    UK2Node_FunctionEntry* Entry = ResolveLocalVariableScope(Blueprint, Params, FunctionName, Error);
    if (!Entry)
    {
        return Error;
    }

    FString VariableName;
    if (!Params->TryGetStringField(TEXT("variable_name"), VariableName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("Missing 'variable_name' parameter"));
    }

    const FName VarName(*VariableName);
    if (!Entry->LocalVariables.ContainsByPredicate(
        [&VarName](const FBPVariableDescription& V) { return V.VarName == VarName; }))
    {
        return MakeCandidatesError(EUnrealMCPGraphError::VariableNotFound,
            FString::Printf(TEXT("Local variable '%s' not found in '%s'"), *VariableName, *FunctionName),
            MakeLocalVariableNames(Entry));
    }

    UFunction* Scope = FindScopeFunctionForGraph(Blueprint, Entry->GetGraph());
    if (!Scope)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidValue,
            FString::Printf(TEXT("No function scope for '%s'"), *FunctionName));
    }

    // RemoveLocalVariable drops the description and the Get/Set nodes that referenced it.
    FBlueprintEditorUtils::RemoveLocalVariable(Blueprint, Scope, VarName);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetStringField(TEXT("function_name"), FunctionName);
    ResultObj->SetStringField(TEXT("removed"), VariableName);
    ResultObj->SetArrayField(TEXT("variables"), MakeLocalVariableJsonArray(Entry));
    ResultObj->SetNumberField(TEXT("variable_count"), Entry->LocalVariables.Num());
    AppendCompileResult(Blueprint, ResultObj);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleRenameBlueprintLocalVariable(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintOrNull(Params, TEXT("blueprint_name"), Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString FunctionName;
    UK2Node_FunctionEntry* Entry = ResolveLocalVariableScope(Blueprint, Params, FunctionName, Error);
    if (!Entry)
    {
        return Error;
    }

    FString OldName;
    FString NewName;
    if (!Params->TryGetStringField(TEXT("old_name"), OldName) ||
        !Params->TryGetStringField(TEXT("new_name"), NewName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("Both 'old_name' and 'new_name' are required"));
    }

    const FName OldFName(*OldName);
    const FName NewFName(*NewName);
    if (!Entry->LocalVariables.ContainsByPredicate(
        [&OldFName](const FBPVariableDescription& V) { return V.VarName == OldFName; }))
    {
        return MakeCandidatesError(EUnrealMCPGraphError::VariableNotFound,
            FString::Printf(TEXT("Local variable '%s' not found in '%s'"), *OldName, *FunctionName),
            MakeLocalVariableNames(Entry));
    }

    // RenameLocalVariable silently no-ops on a collision, so refuse it here instead.
    if (Entry->LocalVariables.ContainsByPredicate(
        [&NewFName](const FBPVariableDescription& V) { return V.VarName == NewFName; }))
    {
        return MakeCandidatesError(EUnrealMCPGraphError::VariableNameInUse,
            FString::Printf(TEXT("Local variable '%s' already exists in '%s'"), *NewName, *FunctionName),
            MakeLocalVariableNames(Entry));
    }

    TSet<FName> ClassVariables;
    FBlueprintEditorUtils::GetClassVariableList(Blueprint, ClassVariables, /*bIncludePrivateVars=*/true);
    if (ClassVariables.Contains(NewFName))
    {
        return MakeCandidatesError(EUnrealMCPGraphError::VariableNameInUse,
            FString::Printf(TEXT("'%s' is already a member variable; a local variable cannot mask it"), *NewName),
            MakeLocalVariableNames(Entry));
    }

    UFunction* Scope = FindScopeFunctionForGraph(Blueprint, Entry->GetGraph());
    if (!Scope)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidValue,
            FString::Printf(TEXT("No function scope for '%s'"), *FunctionName));
    }

    FBlueprintEditorUtils::RenameLocalVariable(Blueprint, Scope, OldFName, NewFName);

    // Read the graph back: the reference nodes must have followed the rename.
    int32 RenamedNodeCount = 0;
    int32 StaleNodeCount = 0;
    if (UEdGraph* Graph = Entry->GetGraph())
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (const UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node))
            {
                const FName Referenced = VariableNode->VariableReference.GetMemberName();
                if (Referenced == NewFName)
                {
                    ++RenamedNodeCount;
                }
                else if (Referenced == OldFName)
                {
                    ++StaleNodeCount;
                }
            }
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetStringField(TEXT("function_name"), FunctionName);
    ResultObj->SetStringField(TEXT("old_name"), OldName);
    ResultObj->SetStringField(TEXT("new_name"), NewName);
    ResultObj->SetArrayField(TEXT("variables"), MakeLocalVariableJsonArray(Entry));
    ResultObj->SetNumberField(TEXT("reference_nodes_renamed"), RenamedNodeCount);
    ResultObj->SetNumberField(TEXT("reference_nodes_stale"), StaleNodeCount);
    AppendCompileResult(Blueprint, ResultObj);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleSetBlueprintLocalVariableDefault(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintOrNull(Params, TEXT("blueprint_name"), Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString FunctionName;
    UK2Node_FunctionEntry* Entry = ResolveLocalVariableScope(Blueprint, Params, FunctionName, Error);
    if (!Entry)
    {
        return Error;
    }

    FString VariableName;
    FString DefaultValue;
    if (!Params->TryGetStringField(TEXT("variable_name"), VariableName) ||
        !Params->TryGetStringField(TEXT("default_value"), DefaultValue))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("Both 'variable_name' and 'default_value' are required"));
    }

    const FName VarName(*VariableName);
    FBPVariableDescription* Variable = Entry->LocalVariables.FindByPredicate(
        [&VarName](const FBPVariableDescription& V) { return V.VarName == VarName; });
    if (!Variable)
    {
        return MakeCandidatesError(EUnrealMCPGraphError::VariableNotFound,
            FString::Printf(TEXT("Local variable '%s' not found in '%s'"), *VariableName, *FunctionName),
            MakeLocalVariableNames(Entry));
    }

    Entry->Modify();
    Variable->DefaultValue = DefaultValue;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetStringField(TEXT("function_name"), FunctionName);
    ResultObj->SetStringField(TEXT("variable_name"), VariableName);
    ResultObj->SetStringField(TEXT("default_value"), Variable->DefaultValue);
    AppendCompileResult(Blueprint, ResultObj);
    return ResultObj;
}

