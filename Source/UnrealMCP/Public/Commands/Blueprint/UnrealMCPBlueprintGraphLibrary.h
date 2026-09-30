#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UnrealMCPBlueprintGraphLibrary.generated.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;

/**
 * One pin of a graph node, as seen by python.
 *
 * UEdGraphNode::Pins is not reflected, so python can never read a node's pin list directly.
 * The kernel fills this struct and both the reflection library and the MCP commands use it as
 * their shared currency (the command layer serializes it to JSON).
 */
USTRUCT(BlueprintType)
struct FUnrealMCPPinInfo
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString PinName;

    /** "input" or "output" */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString Direction;

    /** Pin type, e.g. "float", "exec", "struct/Vector" */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString Category;

    /** Literal value when the pin is not connected */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString DefaultValue;

    /** Asset path for object pins (DefaultObject), empty otherwise */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString DefaultObject;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    bool Connected = false;

    /**
     * Literal for text pins. FText lives in its own field (DefaultTextValue) - the compiler
     * materialises THAT as the FText argument, so a text pin whose literal is visible only through
     * DefaultValue cannot be proven to be written correctly.
     */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString DefaultTextValue;

    /** Engine does not draw this pin (it may still be a real, connectable, unbound input). */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    bool bHidden = false;

    /** Meta HideSelfPin: engine fills the target in, the pin cannot be wired at all. */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    bool bNotConnectable = false;

    /** "<node guid>.<pin name>" for every link */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    TArray<FString> LinkedTo;
};

/** One editable UPROPERTY of a graph node. */
USTRUCT(BlueprintType)
struct FUnrealMCPNodePropertyInfo
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString Name;

    /** C++ type of the property, e.g. "bool", "int32", "FLinearColor" */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString Type;

    /** Text form of the current value */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString Value;
};

/** One node of a graph, as seen by python. */
USTRUCT(BlueprintType)
struct FUnrealMCPNodeInfo
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString NodeId;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString Name;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString Type;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString GraphName;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    int32 PosX = 0;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    int32 PosY = 0;

    /** Editable UPROPERTYs of this node (same set the command route reports as "properties") */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    TArray<FUnrealMCPNodePropertyInfo> Properties;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    TArray<FUnrealMCPPinInfo> Pins;

    /** The live node object, so python can chain further reflected calls */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    UEdGraphNode* Node = nullptr;
};

/** One graph of a blueprint, as seen by python. */
USTRUCT(BlueprintType)
struct FUnrealMCPGraphInfo
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString GraphName;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString GraphClass;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    int32 NodeCount = 0;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    bool IsEditable = false;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    UEdGraph* Graph = nullptr;
};

/** Result envelope for graph operations (no node payload). */
USTRUCT(BlueprintType)
struct FUnrealMCPGraphOpResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    bool Success = false;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString ErrorCode;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString ErrorMessage;

    /** Actual value read back after a write */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString ValueAfter;

    /** Available pin / graph / type names, for a failed lookup */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    TArray<FString> Candidates;
};

/** Result envelope for node operations (carries the node plus its pins when relevant). */
USTRUCT(BlueprintType)
struct FUnrealMCPNodeOpResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    bool Success = false;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString ErrorCode;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString ErrorMessage;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    TArray<FString> Candidates;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    UEdGraphNode* Node = nullptr;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString ValueAfter;

    /** Pins touched by this operation, with the values actually written */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    TArray<FUnrealMCPPinInfo> Readback;
};

/** Result envelope for graph list queries. */
USTRUCT(BlueprintType)
struct FUnrealMCPGraphArrayResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    bool Success = false;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString ErrorCode;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString ErrorMessage;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    TArray<FUnrealMCPGraphInfo> Graphs;
};

/** Result envelope for node list queries. */
USTRUCT(BlueprintType)
struct FUnrealMCPNodeArrayResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    bool Success = false;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString ErrorCode;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString ErrorMessage;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString GraphName;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    TArray<FUnrealMCPNodeInfo> Nodes;
};

/** Result of writing one node property. */
USTRUCT(BlueprintType)
struct FUnrealMCPNodePropertyOpResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    bool Success = false;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString ErrorCode;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString ErrorMessage;

    /** Near misses for an unknown property name */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    TArray<FString> Candidates;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString PropertyType;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString ValueBefore;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString ValueAfter;

    /** True when the write changed the node's pins, so they were rebuilt */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    bool PinsRebuilt = false;

    /** Pins after the write (same shape as get_node_pins) */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    TArray<FUnrealMCPPinInfo> Pins;
};

/** Result of a blueprint compile; the judgement matches the compile_blueprint command. */
USTRUCT(BlueprintType)
struct FUnrealMCPCompileResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    bool Success = false;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString ErrorCode;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString ErrorMessage;

    /** EBlueprintStatus name, e.g. "BS_UpToDate" */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    FString Status;

    /** True for BS_UpToDate and BS_UpToDateWithWarnings (engine semantics) */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    bool Compiled = false;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    TArray<FString> Errors;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Blueprint")
    TArray<FString> Warnings;
};

/**
 * Python-facing reflection surface over blueprint graphs.
 *
 * Why this exists: editor python can reach a UEdGraph object but nothing inside it --
 * UEdGraph::Nodes is a protected property, UEdGraphNode::Pins has no reflected property at all,
 * and the K2 node creation helpers (AllocateDefaultPins / PostPlacedNewNode / CreateNewGuid)
 * are plain C++ members. Object names ("<graph>.K2Node_CallFunction_0") are the only way in,
 * and they drift as soon as a node is added or removed.
 *
 * Every function here wraps the same kernel the MCP commands use
 * (FUnrealMCPBlueprintGraphOps), so both entry points produce identical graph state and
 * identical error codes.
 *
 * Usage (editor python):
 *   lib  = unreal.UnrealMCPBlueprintGraphLibrary
 *   bp   = unreal.load_asset('/Game/Blueprints/BP_Foo')
 *   g    = lib.get_graphs(bp).graphs[0].graph
 *   res  = lib.add_function_call_node(g, 'KismetSystemLibrary', 'Delay', 320, 0)
 *   assert res.success, res.error_message
 *   for pin in lib.get_node_pins(res.node):
 *       print(pin.pin_name, pin.direction, pin.default_value, pin.linked_to)
 */
UCLASS()
class UNREALMCP_API UUnrealMCPBlueprintGraphLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    // --- Read -----------------------------------------------------------------

    /** Every graph of the blueprint (ubergraph pages, function graphs, macro graphs). */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Blueprint")
    static FUnrealMCPGraphArrayResult GetGraphs(UBlueprint* Blueprint);

    /** Every node of the graph. Read-only: never creates a graph. */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Blueprint")
    static FUnrealMCPNodeArrayResult GetGraphNodes(UEdGraph* Graph);

    /** Pins of one node, including default values and links. */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Blueprint")
    static TArray<FUnrealMCPPinInfo> GetNodePins(UEdGraphNode* Node);

    /** Pins of one node plus the node's own identity, for a single call. */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Blueprint")
    static FUnrealMCPNodeInfo GetNodeInfo(UEdGraphNode* Node);

    /** Editable UPROPERTYs of one node (mirrors the "properties" field of the command route). */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Blueprint")
    static TArray<FUnrealMCPNodePropertyInfo> GetNodeProperties(UEdGraphNode* Node);

    // --- Node creation --------------------------------------------------------

    /** Function call node; TargetClass accepts a short name, a U-prefixed name or a /Script path. */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Blueprint")
    static FUnrealMCPNodeOpResult AddFunctionCallNode(UEdGraph* Graph, const FString& TargetClass,
                                                      const FString& FunctionName, int32 X, int32 Y);

    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Blueprint")
    static FUnrealMCPNodeOpResult AddEventNode(UEdGraph* Graph, const FString& EventName, int32 X, int32 Y);

    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Blueprint")
    static FUnrealMCPNodeOpResult AddVariableGetNode(UEdGraph* Graph, const FString& VariableName, int32 X, int32 Y);

    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Blueprint")
    static FUnrealMCPNodeOpResult AddVariableSetNode(UEdGraph* Graph, const FString& VariableName, int32 X, int32 Y);

    /**
     * Get (bSet=false) or Set node of a property on ANOTHER class (non-self context), e.g.
     * OwnerClass="PlayerController", VariableName="bShowMouseCursor". The node carries a Target pin
     * the caller must wire. Same kernel as add_blueprint_variable_node(owner_class=...).
     */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Blueprint")
    static FUnrealMCPNodeOpResult AddExternalVariableNode(UEdGraph* Graph, const FString& OwnerClass,
                                                          const FString& VariableName, bool bSet, int32 X, int32 Y);

    /** Constant node: maps LiteralType to KismetSystemLibrary.MakeLiteral<Type>. */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Blueprint")
    static FUnrealMCPNodeOpResult AddLiteralNode(UEdGraph* Graph, const FString& LiteralType,
                                                 const FString& Value, int32 X, int32 Y);

    /** Skeleton node of any K2 node class (Cast / Sequence / Switch / Comment / ...). */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Blueprint")
    static FUnrealMCPNodeOpResult AddNodeByClass(UEdGraph* Graph, const FString& NodeClass, int32 X, int32 Y);

    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Blueprint")
    static FUnrealMCPNodeOpResult AddSelfReferenceNode(UEdGraph* Graph, int32 X, int32 Y);

    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Blueprint")
    static FUnrealMCPNodeOpResult AddInputActionNode(UEdGraph* Graph, const FString& ActionName, int32 X, int32 Y);

    // --- Graph mutation -------------------------------------------------------

    /**
     * Connect an output pin to an input pin. Routed through the graph schema, so type checks,
     * implicit conversion and "break the existing link on a single-connection input" all apply.
     */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Blueprint")
    static FUnrealMCPGraphOpResult ConnectPins(UEdGraphNode* SourceNode, const FString& SourcePin,
                                               UEdGraphNode* TargetNode, const FString& TargetPin);

    /**
     * Write a pin's default value.
     * ValueKind is one of: auto (default), number, bool, string, name, text, enum, vector,
     * vector2d, linear_color, object, class. "auto" picks number/bool/string by shape.
     */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Blueprint")
    static FUnrealMCPGraphOpResult SetPinDefault(UEdGraphNode* Node, const FString& PinName,
                                                 const FString& Value, const FString& ValueKind);

    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Blueprint")
    static FUnrealMCPGraphOpResult DisconnectPin(UEdGraphNode* Node, const FString& PinName,
                                                 const FString& LinkedNodeId, const FString& LinkedPinName);

    /**
     * Write one of the node's own UPROPERTYs (not a pin default).
     * Value is text; ValueKind is one of: auto (default), number, bool, string, name, text, enum,
     * vector, vector2d, linear_color, object, class.
     * Properties that affect the pin layout (e.g. UK2Node_CastByteToEnum::Enum) rebuild the node
     * afterwards so its pins match; purely visual/positional properties do not.
     */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Blueprint")
    static FUnrealMCPNodePropertyOpResult SetNodeProperty(UEdGraphNode* Node, const FString& PropertyName,
                                                          const FString& Value, const FString& ValueKind);

    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Blueprint")
    static FUnrealMCPGraphOpResult DeleteNode(UEdGraphNode* Node);

    // --- Graph management -----------------------------------------------------

    /** Existing event graph, or a newly created one when the blueprint has none. */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Blueprint")
    static FUnrealMCPGraphArrayResult EnsureEventGraph(UBlueprint* Blueprint);

    /** Resolve one graph by name (EventGraph / UserConstructionScript / function / macro). */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Blueprint")
    static FUnrealMCPGraphArrayResult FindGraph(UBlueprint* Blueprint, const FString& GraphName);

    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Blueprint")
    static FUnrealMCPGraphArrayResult AddFunctionGraph(UBlueprint* Blueprint, const FString& GraphName);

    // --- Compile / transaction ------------------------------------------------

    /** Compile and read the real status back; same judgement as compile_blueprint. */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Blueprint")
    static FUnrealMCPCompileResult CompileBlueprintChecked(UBlueprint* Blueprint);

    /** Group several graph edits into a single undo step. */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Blueprint")
    static void BeginTransaction(const FString& Description);

    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Blueprint")
    static void EndTransaction();

    // --- Introspection helpers ------------------------------------------------

    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Blueprint")
    static TArray<FString> GetSupportedVariableTypes();

    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Blueprint")
    static TArray<FString> GetSupportedLiteralTypes();
};
