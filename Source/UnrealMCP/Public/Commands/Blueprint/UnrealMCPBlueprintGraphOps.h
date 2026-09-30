#pragma once

#include "CoreMinimal.h"
#include "Commands/Blueprint/UnrealMCPBlueprintGraphLibrary.h"
#include "Reflection/MCPPropertyReflector.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Json.h"
#include "Kismet2/BlueprintEditorUtils.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class UFunction;
class UK2Node_CallFunction;
class UK2Node_Event;
class UK2Node_VariableGet;
class UK2Node_VariableSet;

/**
 * Structured error codes shared by the MCP commands and the python reflection library.
 * Keeping one set of codes is what lets the two entry points stay interchangeable.
 */
namespace EUnrealMCPGraphError
{
    inline const TCHAR* BlueprintNotFound   = TEXT("blueprint_not_found");
    inline const TCHAR* GraphNotFound       = TEXT("graph_not_found");
    inline const TCHAR* UnsupportedGraphKind= TEXT("unsupported_graph_kind");
    inline const TCHAR* NodeNotFound        = TEXT("node_not_found");
    inline const TCHAR* NodeClassNotFound   = TEXT("node_class_not_found");
    inline const TCHAR* FunctionNotFound    = TEXT("function_not_found");
    inline const TCHAR* VariableNotFound    = TEXT("variable_not_found");
    inline const TCHAR* PinNotFound         = TEXT("pin_not_found");
    inline const TCHAR* OutputNotFound      = TEXT("output_not_found");
    inline const TCHAR* InputNotFound       = TEXT("input_not_found");
    inline const TCHAR* NotAnInputPin       = TEXT("not_an_input_pin");
    inline const TCHAR* IncompatibleTypes   = TEXT("incompatible_types");
    inline const TCHAR* UnsupportedPinType  = TEXT("unsupported_pin_type");
    inline const TCHAR* InvalidParams       = TEXT("invalid_params");
    inline const TCHAR* InvalidValue        = TEXT("invalid_value");
    inline const TCHAR* UnsupportedLiteralType    = TEXT("unsupported_literal_type");
    inline const TCHAR* UnsupportedVariableType   = TEXT("unsupported_variable_type");
    inline const TCHAR* UnsupportedContainerType  = TEXT("unsupported_container_type");
    /** signature_class was given but does not declare a function of that name. */
    inline const TCHAR* SignatureFunctionNotFound = TEXT("signature_function_not_found");
    /** The requested function graph does not exist (or is one the engine owns). */
    inline const TCHAR* FunctionGraphNotFound     = TEXT("function_graph_not_found");
    /** The graph exists but the engine manages it (event graph / construction script): cannot remove. */
    inline const TCHAR* GraphNotRemovable         = TEXT("graph_not_removable");
    /** A graph (or function) with the requested new name already exists. */
    inline const TCHAR* GraphNameInUse            = TEXT("graph_name_in_use");
    /** The function overrides a parent signature: its parameters come from the parent, not from us. */
    inline const TCHAR* FunctionNotEditable       = TEXT("function_not_editable");
    /** No parameter / output of that name on the function. */
    inline const TCHAR* ParamNotFound             = TEXT("param_not_found");
    /** A member or local variable of the requested new name already exists in that scope. */
    inline const TCHAR* VariableNameInUse         = TEXT("variable_name_in_use");
    /** A custom event of the requested name already exists in the blueprint. */
    inline const TCHAR* EventNameInUse            = TEXT("custom_event_name_in_use");
    /** A requested variable default did not reach the class default object after compiling. */
    inline const TCHAR* DefaultValueNotApplied    = TEXT("default_value_not_applied");
    /** A Set node was asked for a property the owner class exposes as BlueprintReadOnly. */
    inline const TCHAR* PropertyNotWritable       = TEXT("property_not_writable");
    /** Removing this component would drop child components the caller did not ask to remove. */
    inline const TCHAR* ComponentHasChildren      = TEXT("component_has_children");
    /** The component is the blueprint's only root; removing it needs force=true. */
    inline const TCHAR* RootComponentProtected    = TEXT("root_component_protected");
    /** The component template is not a scene component, so it cannot live in the SCS tree. */
    inline const TCHAR* ComponentNotScene         = TEXT("component_not_scene");
    /** The component template holds no BodyInstance, so it has no collision to configure. */
    inline const TCHAR* ComponentNotPrimitive     = TEXT("component_not_primitive");
    /** The pin cannot be split or recombined (not a struct, already split, or the engine refused). */
    inline const TCHAR* PinNotSplittable          = TEXT("pin_not_splittable");
    /** No implemented interface matches the requested path. */
    inline const TCHAR* InterfaceNotFound         = TEXT("interface_not_found");
}

/**
 * Outcome of one node-property write.
 *
 * The values are JSON values, not text: the old implementation flattened them through a text encoder,
 * so a caller received `"[\"A\",\"B\"]"` where the property actually holds an array, and had to parse it
 * back to use it. `Write` carries the reflector's structured error (code, message, candidates, accepted
 * shapes, hint, failed index, unchanged) so no caller has to guess an error code from message text.
 */
struct UNREALMCP_API FUnrealMCPNodePropertyWriteResult
{
    /** Engine type of the property, e.g. "TArray<FName>". */
    FString PropertyType;

    /** Structured description (container / element type / accepted shapes / semantics). */
    TSharedPtr<FJsonObject> PropertyTypeDetail;

    TSharedPtr<FJsonValue> ValueBefore;
    TSharedPtr<FJsonValue> ValueAfter;

    /** True when ReconstructNode() ran because the property can drive the pin layout. */
    bool bPinsRebuilt = false;

    /** Success flag plus the structured failure details. */
    FWriteResult Write;

    /** Near misses for an unknown property name. */
    TArray<FString> Candidates;

    /**
     * Resolved path actually written, e.g. "Node.ChainEnd" for a "node.chain_end" request. Empty
     * only when nothing was resolved; echoing it makes "I read one level too high" impossible to
     * mistake for success.
     */
    FString PropertyPath;

    /** Fields reachable at the path segment that failed, so a misspelled inner field self-corrects. */
    TArray<FString> AvailablePathSegments;
};

/**
 * The single implementation of blueprint graph reading and writing.
 *
 * Both entry points are thin shells over this class: the MCP command handlers parse JSON and
 * serialize the result structs, the python reflection library converts arguments and hands the
 * structs straight back. Neither of them owns any graph logic, so "MCP can do it, python can't"
 * and behavioural drift between the two are structurally impossible.
 *
 * All functions must be called on the GameThread.
 */
class UNREALMCP_API FUnrealMCPBlueprintGraphOps
{
public:
    // --- Graph resolution ------------------------------------------------------

    /** Every graph of the blueprint: ubergraph pages, function graphs, macro graphs. */
    static bool ListGraphs(UBlueprint* Blueprint, TArray<FUnrealMCPGraphInfo>& OutGraphs,
                           FString& OutErrorCode, FString& OutErrorMessage);

    /**
     * Resolve the graph a node command works on.
     * Explicit GraphName wins (exact match). Without one, a single graph whose name contains
     * "EventGraph" is used; several such graphs are ambiguous and reported as graph_not_found
     * with every graph name as a candidate. Write commands may create the event graph when the
     * blueprint has none.
     */
    static bool ResolveGraph(UBlueprint* Blueprint, const FString& GraphName, bool bCreateEventGraphIfMissing,
                             UEdGraph*& OutGraph, FString& OutErrorCode, FString& OutErrorMessage,
                             TArray<FString>& OutCandidates);

    static bool EnsureEventGraph(UBlueprint* Blueprint, UEdGraph*& OutGraph,
                                 FString& OutErrorCode, FString& OutErrorMessage);

    static bool AddFunctionGraph(UBlueprint* Blueprint, const FString& GraphName, UEdGraph*& OutGraph,
                                 FString& OutErrorCode, FString& OutErrorMessage);

    /**
     * Create (or return) a function graph.
     *
     * SignatureClass nullptr  -> a user function (bIsUserCreated = true).
     * SignatureClass given    -> an implementation of that class' BlueprintImplementableEvent of the same
     *                            name (bIsUserCreated = false with SignatureFromObject = SignatureClass,
     *                            the same recipe the engine's own Persona uses for UAnimNotify).
     *                            The class MUST declare the function, otherwise signature_function_not_found
     *                            with candidate names and no graph is created.
     *
     * Idempotent: an existing graph of that name is returned with bOutCreated = false, never duplicated.
     * OutSignatureOwner is the class the entry node's signature comes from (nullptr for user functions).
     */
    static bool AddFunctionGraph(UBlueprint* Blueprint, const FString& GraphName, UClass* SignatureClass,
                                 UEdGraph*& OutGraph, bool& bOutCreated, UClass*& OutSignatureOwner,
                                 FString& OutErrorCode, FString& OutErrorMessage,
                                 TArray<FString>& OutCandidates);

    /** The blueprint's FunctionGraphs with their signature source and node count. */
    static void ListFunctionGraphs(UBlueprint* Blueprint, TArray<UEdGraph*>& OutGraphs);

    /** Removes one function graph. Engine-owned graphs (event graph, construction script) are refused. */
    static bool RemoveFunctionGraph(UBlueprint* Blueprint, const FString& FunctionName,
                                    FString& OutErrorCode, FString& OutErrorMessage,
                                    TArray<FString>& OutCandidates);

    /** Renames a function graph (which renames the function it defines). */
    static bool RenameFunctionGraph(UBlueprint* Blueprint, const FString& OldName, const FString& NewName,
                                    FString& OutErrorCode, FString& OutErrorMessage);

    /** One function parameter / return value. is_output targets the function's result node. */
    struct FFunctionParamRequest
    {
        FString Name;
        FString Type;
        FString SubClass;
        FString DefaultValue;
        bool bIsOutput = false;
    };

    /**
     * Adds a parameter to a function graph.
     *  - input  -> a user-defined pin on the function entry node (direction EGPD_Output on an entry node)
     *  - output -> a user-defined pin on the function's result node, created on demand by the engine
     *              (FBlueprintEditorUtils::FindOrCreateFunctionResultNode)
     * The entry node must be user-editable: overriding a parent's event does not allow parameter edits
     * (function_not_editable), because the signature belongs to that parent.
     */
    static bool AddFunctionParam(UBlueprint* Blueprint, UEdGraph* Graph, const FFunctionParamRequest& Request,
                                 FString& OutErrorCode, FString& OutErrorMessage,
                                 TArray<FString>& OutCandidates);

    static bool RemoveFunctionParam(UBlueprint* Blueprint, UEdGraph* Graph, const FString& ParamName,
                                    bool& bOutWasOutput, FString& OutErrorCode, FString& OutErrorMessage);

    static bool RenameFunctionParam(UBlueprint* Blueprint, UEdGraph* Graph, const FString& OldName,
                                    const FString& NewName, FString& OutErrorCode, FString& OutErrorMessage);

    /** The entry node of a function graph, or nullptr when the graph is not a function graph. */
    static class UK2Node_FunctionEntry* FindFunctionEntryNode(UEdGraph* Graph);

    /** Parameters (inputs) and outputs (return values) of a function graph, as [{name, direction, type}]. */
    static void DescribeFunctionParams(UEdGraph* Graph, TArray<TSharedPtr<FJsonObject>>& OutParams);

    /** Macro graphs can be read but not built into by this plugin. */
    static bool IsMacroGraph(const UBlueprint* Blueprint, const UEdGraph* Graph);

    // --- Node lookup -----------------------------------------------------------

    static UEdGraphNode* FindNodeByGuid(const UEdGraph* Graph, const FString& NodeGuid);

    /** Exact match, then case-insensitive, then the first data output of a variable get node. */
    static UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName,
                                EEdGraphPinDirection Direction = EGPD_MAX);

    static void CollectPinNames(const UEdGraphNode* Node, EEdGraphPinDirection Direction,
                                TArray<FString>& OutNames);

    // --- Serialization ---------------------------------------------------------

    static void MakePinInfo(const UEdGraphPin* Pin, FUnrealMCPPinInfo& OutInfo);
    static void MakeNodeInfo(UEdGraphNode* Node, FUnrealMCPNodeInfo& OutInfo);
    static void MakeGraphInfo(UBlueprint* Blueprint, UEdGraph* Graph, FUnrealMCPGraphInfo& OutInfo);

    // --- Node properties -------------------------------------------------------

    /**
     * The single filter for "what counts as a readable node property": UPROPERTYs carrying CPF_Edit
     * and not CPF_Transient, minus a small denylist of bookkeeping fields (NodeGuid / position /
     * comment plumbing / diagnostics). Both the command JSON and the reflection structs are built
     * from this one list so the two routes cannot drift.
     */
    static void CollectEditableNodeProperties(UEdGraphNode* Node, TArray<FProperty*>& OutProperties);

    /** Typed JSON object of the editable properties (numbers stay numbers, structs become arrays). */
    static TSharedPtr<FJsonObject> MakeNodePropertiesJson(UEdGraphNode* Node);

    /** Same set, flattened to name / type / text value for the reflection library. */
    static void CollectNodePropertyInfos(UEdGraphNode* Node, TArray<FUnrealMCPNodePropertyInfo>& OutProperties);

    static FString JsonValueToText(const TSharedPtr<FJsonValue>& Value);

    // --- Node creation ---------------------------------------------------------

    /**
     * Create a node of class T inside Graph.
     * Fixed order: NewObject -> Configure -> position -> CreateNewGuid -> PostPlacedNewNode ->
     * AllocateDefaultPins -> AddNode -> ReconstructNode -> mark modified.
     *
     * Configure runs before the pins exist, which is where class-specific setup belongs
     * (FunctionReference / EventReference / VariableReference / InputActionName). Every creator
     * below goes through one of these two entry points.
     */
    template <typename T, typename TConfigurator>
    static T* CreateGraphNode(UEdGraph* Graph, const FVector2D& Position, TConfigurator&& Configure)
    {
        if (!Graph)
        {
            return nullptr;
        }

        T* Node = NewObject<T>(Graph);
        if (!Node)
        {
            return nullptr;
        }

        Configure(Node);
        Node->NodePosX = static_cast<int32>(Position.X);
        Node->NodePosY = static_cast<int32>(Position.Y);
        Node->CreateNewGuid();
        Node->PostPlacedNewNode();
        Node->AllocateDefaultPins();
        Graph->AddNode(Node, /*bFromUI=*/false, /*bSelectNewNode=*/false);
        Node->ReconstructNode();

        if (UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph))
        {
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        }
        return Node;
    }

    template <typename T>
    static T* CreateGraphNode(UEdGraph* Graph, const FVector2D& Position)
    {
        return CreateGraphNode<T>(Graph, Position, [](T*) {});
    }

    /** Same lifecycle, but for a class only known at runtime (add_blueprint_node_by_class). */
    static UEdGraphNode* CreateGraphNodeByClass(UEdGraph* Graph, UClass* NodeClass, const FVector2D& Position,
                                                TFunctionRef<void(UEdGraphNode*)> Configure);

    static UBlueprint* FindBlueprintForGraph(UEdGraph* Graph);

    static bool CreateFunctionCallNode(UEdGraph* Graph, const FString& TargetClass, const FString& FunctionName,
                                       const FVector2D& Position, UK2Node_CallFunction*& OutNode,
                                       FString& OutErrorCode, FString& OutErrorMessage,
                                       TArray<FString>& OutCandidates);

    static bool CreateEventNode(UEdGraph* Graph, const FString& EventName, const FVector2D& Position,
                                UK2Node_Event*& OutNode, FString& OutErrorCode, FString& OutErrorMessage);

    /** Events a blueprint of this class can implement (BlueprintEvent functions of the parent chain), sorted. */
    static TArray<FString> ListImplementableEventNames(const UBlueprint* Blueprint);

    static bool CreateVariableNode(UEdGraph* Graph, const FString& VariableName, bool bSet, const FVector2D& Position,
                                   UEdGraphNode*& OutNode, FString& OutErrorCode, FString& OutErrorMessage,
                                   TArray<FString>& OutCandidates);

    /**
     * A Get/Set node for a property of ANOTHER class (non-self context): the node carries a Target
     * (self) pin typed to that class, which the caller wires. OwnerClass is a class name,
     * "/Script/<Module>.<Class>" or a blueprint generated class path. The property must be
     * BlueprintVisible; a Set of a BlueprintReadOnly property is refused (property_not_writable).
     */
    static bool CreateExternalVariableNode(UEdGraph* Graph, const FString& OwnerClass, const FString& VariableName,
                                           bool bSet, const FVector2D& Position, UEdGraphNode*& OutNode,
                                           FString& OutErrorCode, FString& OutErrorMessage,
                                           TArray<FString>& OutCandidates);

    /**
     * Same lifecycle as CreateVariableNode, but for a variable scoped to a function graph (a local
     * variable): the reference is a local member of the top-level graph rather than a class member.
     * The variable must already exist on the graph's entry node; its description carries the VarGuid
     * the reference needs.
     */
    static bool CreateLocalVariableNode(UEdGraph* Graph, const FString& VariableName, const FGuid& VariableGuid,
                                        bool bSet, const FVector2D& Position, UEdGraphNode*& OutNode,
                                        FString& OutErrorCode, FString& OutErrorMessage,
                                        TArray<FString>& OutCandidates);

    static bool CreateLiteralNode(UEdGraph* Graph, const FString& LiteralType, const FString& Value,
                                  const FVector2D& Position, UEdGraphNode*& OutNode, FString& OutValueAfter,
                                  FString& OutErrorCode, FString& OutErrorMessage, TArray<FString>& OutCandidates);

    static bool CreateNodeByClass(UEdGraph* Graph, const FString& NodeClass, const FVector2D& Position,
                                  UEdGraphNode*& OutNode, FString& OutErrorCode, FString& OutErrorMessage,
                                  TArray<FString>& OutCandidates);

    static bool CreateSelfReferenceNode(UEdGraph* Graph, const FVector2D& Position, UEdGraphNode*& OutNode,
                                        FString& OutErrorCode, FString& OutErrorMessage);

    static bool CreateInputActionNode(UEdGraph* Graph, const FString& ActionName, const FVector2D& Position,
                                      UEdGraphNode*& OutNode, FString& OutErrorCode, FString& OutErrorMessage);

    // --- Mutation --------------------------------------------------------------

    static bool ConnectNodes(UEdGraph* Graph, UEdGraphNode* SourceNode, const FString& SourcePinName,
                             UEdGraphNode* TargetNode, const FString& TargetPinName,
                             FString& OutErrorCode, FString& OutErrorMessage, TArray<FString>& OutCandidates);

    static bool SetPinDefaultValue(UEdGraphNode* Node, const FString& PinName,
                                   const TSharedPtr<FJsonValue>& Value,
                                   FString& OutErrorCode, FString& OutErrorMessage,
                                   TArray<FString>& OutCandidates);

    /** String form of the same dispatch, for the reflection library. */
    static bool SetPinDefaultValueFromString(UEdGraphNode* Node, const FString& PinName,
                                             const FString& Value, const FString& ValueKind,
                                             FString& OutErrorCode, FString& OutErrorMessage,
                                             TArray<FString>& OutCandidates);

    static bool DisconnectPin(UEdGraphNode* Node, const FString& PinName,
                              const FString& LinkedNodeId, const FString& LinkedPinName,
                              int32& OutDisconnectedCount, int32& OutRemainingLinks,
                              FString& OutErrorCode, FString& OutErrorMessage, TArray<FString>& OutCandidates);

    static bool DeleteNode(UEdGraph* Graph, UEdGraphNode* Node,
                           FString& OutErrorCode, FString& OutErrorMessage);

    /**
     * Write one of the node's own UPROPERTYs through the shared property reflector, so node properties
     * accept exactly the shapes the reflector defines (and report its structured errors).
     *
     * A property whose *type* can drive the pin layout (an enum, or a container) is followed by
     * ReconstructNode(); see PropertyAffectsPinLayout for why the decision is made from the property
     * rather than from a list of property names.
     */
    static bool SetNodeProperty(UEdGraphNode* Node, const FString& PropertyName,
                                const TSharedPtr<FJsonValue>& Value,
                                FUnrealMCPNodePropertyWriteResult& OutResult);

    /**
     * True when writing this property can change the node's pin layout, judged from the property's type:
     * enum-valued properties (a Cast/Switch node's target enum) and container properties (a Switch
     * node's pin name list) can, purely visual scalars/strings/structs cannot.
     */
    static bool PropertyAffectsPinLayout(const FProperty* Property);

    // --- Type / value helpers --------------------------------------------------

    /** Short name, U-prefixed name or /Script/Module.Class path. */
    static bool ResolveClass(const FString& ClassName, UClass*& OutClass, TArray<FString>& OutCandidates);

    static bool ResolveFunction(UClass* TargetClass, const FString& FunctionName, UFunction*& OutFunction,
                                TArray<FString>& OutCandidates);

    static bool ResolveNodeClass(const FString& NodeClass, UClass*& OutClass, TArray<FString>& OutCandidates);

    /** JSON value form of a python string, using ValueKind ("auto" guesses by shape). */
    static bool MakeJsonValueFromString(const FString& Value, const FString& ValueKind,
                                        TSharedPtr<FJsonValue>& OutValue, FString& OutErrorMessage);

    /** Split "struct[]" / "int{,}" into the base type and the container kind (0=none,1=array,2=set,3=map). */
    static void SplitContainerType(const FString& TypeString, FString& OutBaseType, int32& OutContainerKind);

    static bool BuildVariablePinType(const FString& TypeString, const FString& SubClass,
                                     FEdGraphPinType& OutPinType, FString& OutErrorMessage,
                                     TArray<FString>& OutSupportedTypes);

    static TArray<FString> SupportedVariableTypes();
    static TArray<FString> SupportedLiteralTypes();
    static bool LiteralFunctionName(const FString& LiteralType, FString& OutFunctionName,
                                    TArray<FString>& OutSupportedTypes);

    // --- Compile / misc --------------------------------------------------------

    /** Compile and read the status back; same judgement the compile_blueprint command uses. */
    static bool CompileChecked(UBlueprint* Blueprint, FString& OutStatus, bool& bOutCompiled,
                               TArray<FString>& OutErrors, TArray<FString>& OutWarnings);

    /** Drop the per-node compiler messages of every graph before compiling, so the diagnostics the
     *  compile commands report describe this compile and not an earlier failing one. */
    static void ResetCompilerMessages(UBlueprint* Blueprint);

    static void MarkModified(UEdGraph* Graph);

    /** Persist the package that owns the graph (blueprint writes must survive a restart). */
    static void PersistGraphOwner(UEdGraph* Graph);
};
