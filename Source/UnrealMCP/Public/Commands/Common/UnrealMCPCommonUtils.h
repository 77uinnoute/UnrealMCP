#pragma once

#include "CoreMinimal.h"
#include "Json.h"

// Forward declarations
struct FWriteResult;
class AActor;
class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class UK2Node_Event;
class UK2Node_CallFunction;
class UK2Node_VariableGet;
class UK2Node_VariableSet;
class UK2Node_InputAction;
class UK2Node_Self;
class UFunction;
class UMaterial;
class UMaterialFunction;
class USCS_Node;
class UWorld;

/**
 * Common utilities for UnrealMCP commands
 */
class UNREALMCP_API FUnrealMCPCommonUtils
{
public:
    // JSON utilities
    static TSharedPtr<FJsonObject> CreateErrorResponse(const FString& Message);
    // Error response carrying a machine-readable code in addition to the message
    static TSharedPtr<FJsonObject> CreateErrorResponse(const FString& ErrorCode, const FString& Message);
    static TSharedPtr<FJsonObject> CreateSuccessResponse(const TSharedPtr<FJsonObject>& Data = nullptr);

    // True while a play session (PIE or simulate-in-editor) is live. Read from the editor every call:
    // a cached flag goes stale exactly when it matters (the caller decides whether a null asset means
    // "missing" or "not loadable in play mode").
    static bool IsPlaySessionRunning();

    /**
     * How many instances of a Blueprint's generated class are placed in the world the user is looking
     * at, plus which world that was (OutWorldKind = "editor" or "pie").
     *
     * This is what a command writing an SCS component TEMPLATE has to report: a level instance keeps
     * its own copy of the property values and does NOT follow the template, so "the template changed"
     * and "the thing standing in the level changed" are two different statements. Counting in the play
     * world while PIE runs is deliberate - that is the world whose behaviour the caller is looking at.
     */
    static int32 CountPlacedInstances(const UBlueprint* Blueprint, FString& OutWorldKind);

    /**
     * Attach the SCS-template consequence to a response: `placed_instances` + `counted_in`, and - when
     * there is at least one - a `hint` saying the placed instances do NOT follow the template and naming
     * the three routes that do change them. One place for the wording, so both template-writing commands
     * say the same thing.
     */
    static void AddTemplateInstanceReport(const UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Response);

    /**
     * Null when a requested actor name is free, otherwise a structured `name_taken` response.
     *
     * Uses the SAME test the spawn path uses - StaticFindObjectFast(nullptr, Level, Name), which sees
     * pending-kill objects (LevelActor.cpp:575) - because actor listings skip them. Without this the request
     * reaches the engine's Fatal branch ("Cannot generate unique name") and kills the whole editor.
     */
    static TSharedPtr<FJsonObject> MakeNameTakenResponseIfTaken(UWorld* World, const FString& ActorName);

    /**
     * Attach `editor_state {pie_running, simulating_in_editor, world, level_name}` to a response.
     *
     * Called from both response factories, so every command answers it without each one remembering to.
     * This is what makes "the asset does not exist" separable from "PIE is running and this asset type
     * cannot be loaded in play mode" - a conflation that has already produced a wrong decision.
     */
    static void AddEditorState(const TSharedPtr<FJsonObject>& Response);

    /**
     * Error code to use when a path did not resolve: "load_failed_in_pie" when a play session is
     * running AND the asset registry knows the package (so the asset exists, it just cannot be loaded
     * in play mode), otherwise an empty string, which means "keep the existing code".
     *
     * Conservative on purpose: renaming every play-mode load failure would turn real path typos into
     * "PIE's fault", which is worse than the conflation this fixes.
     */
    static FString ClassifyAssetLoadFailure(const FString& AssetPath);
    // Set a string-array field, omitting it entirely when empty (keeps "nothing to report" out of the JSON)
    static void AddStringArrayField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName,
                                    const TArray<FString>& Values);
    static void GetIntArrayFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, TArray<int32>& OutArray);
    static void GetFloatArrayFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, TArray<float>& OutArray);
    // Numbers from either an array value or a struct-shaped object with the given field names.
    // One implementation, shared by the property writer and the graph kernel.
    static bool ReadNumbersFromJson(const TSharedPtr<FJsonValue>& Value, const TArray<FString>& ObjectKeys,
                                    TArray<double>& OutNumbers, FString& OutErrorMessage);
    static FVector2D GetVector2DFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName);
    static FVector GetVectorFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName);
    static FRotator GetRotatorFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName);
    
    // Actor utilities
    static TSharedPtr<FJsonValue> ActorToJson(AActor* Actor);
    static TSharedPtr<FJsonObject> ActorToJsonObject(AActor* Actor, bool bDetailed = false);
    
    // Blueprint utilities
    // Find blueprint by asset path (e.g. /Game/Blueprints/MyBP) or by name (MyBP)
    // If BlueprintName is empty, returns the currently opened blueprint in the editor
    static UBlueprint* FindBlueprint(const FString& BlueprintName);
    static UBlueprint* FindBlueprintByName(const FString& BlueprintName);
    static UBlueprint* GetCurrentBlueprint();
    static UEdGraph* FindOrCreateEventGraph(UBlueprint* Blueprint);

    // Blueprint component utilities
    // Resolve a blueprint's component (USCS_Node) through the shared safety checks:
    // blueprint valid -> SimpleConstructionScript present AND bound to this blueprint -> node found -> template non-null.
    // Returns nullptr on failure with OutErrorCode ("blueprint_not_ready" / "component_not_found")
    // and OutErrorMessage listing the available component names.
    static USCS_Node* FindBlueprintComponentNode(UBlueprint* Blueprint, const FString& ComponentName,
        FString& OutErrorCode, FString& OutErrorMessage);

    /** Writable component template: the blueprint's own SCS node first, then an inherited component
     *  (a parent blueprint's SCS node, or a native component such as ACharacter::Mesh). For the latter
     *  this returns the class's own component instance, so writing it never touches the parent class.
     *  OutOwnerClass (optional) receives the class that DECLARES the inherited component: a
     *  UBlueprintGeneratedClass for a parent blueprint's SCS component, a native class otherwise. */
    static UActorComponent* FindWritableComponentTemplate(UBlueprint* Blueprint, const FString& ComponentName,
        FString& OutErrorCode, FString& OutErrorMessage, UClass** OutOwnerClass = nullptr);

    // Verify the blueprint's SimpleConstructionScript can accept component commands, compiling once if
    // it is not ready yet. Returns false with OutErrorCode == "blueprint_not_ready" when it still cannot.
    static bool EnsureBlueprintComponentsReady(UBlueprint* Blueprint, FString& OutErrorCode, FString& OutErrorMessage);
    
    // Material utilities - find material/material function by path or name
    static UMaterial* FindMaterial(const FString& MaterialName);
    static UMaterialFunction* FindMaterialFunction(const FString& FunctionName);
    
    // Blueprint node utilities
    // NOTE: node creation, pin lookup, pin default dispatch and connection validation all live in
    // FUnrealMCPBlueprintGraphOps; the wrappers below only exist for the remaining legacy callers.
    // Connect through the graph schema so type compatibility, automatic conversion nodes and
    // "break the existing link on the target input pin" are all handled by the editor itself.
    // On failure fills OutErrorCode ("output_not_found" / "input_not_found" / "incompatible_types")
    // and OutErrorMessage listing the available pins of the offending node.
    static bool ConnectGraphNodes(UEdGraph* Graph, UEdGraphNode* SourceNode, const FString& SourcePinName,
                                UEdGraphNode* TargetNode, const FString& TargetPinName,
                                FString& OutErrorCode, FString& OutErrorMessage);
    static UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction = EGPD_MAX);
    // Set one pin's default value from JSON. Same dispatch as add_blueprint_function_node's `params`
    // and the set_blueprint_pin_default command: numeric / bool / FName / FString / FText / enum /
    // FVector / FVector2D / FLinearColor / FVector4 / class / object (asset path).
    // On failure fills OutErrorCode ("pin_not_found" / "unsupported_pin_type" / "type_mismatch" / "load_failed").
    static bool SetPinDefaultValue(UEdGraphNode* Node, const FString& PinName,
                                   const TSharedPtr<FJsonValue>& Value,
                                   FString& OutErrorCode, FString& OutErrorMessage);

    // Collect property names that differ from PropertyName only by case/underscores, so a
    // failed exact-name write can report "did you mean" instead of guessing.
    static void FindPropertyNameSuggestions(UObject* Object, const FString& PropertyName, TArray<FString>& OutSuggestions);

    /** Exact name first, then case/underscore-insensitive with an optional C++ `b` prefix. */
    static FProperty* FindPropertyByNameNormalized(UClass* Class, const FString& PropertyName);

    /** Run a `UPROPERTY(Setter = ...)` field's setter with the value currently stored in that field. */
    static void InvokePropertySetter(UObject* Object, FProperty* Property, void* ValueAddr);
    // Persist the package that owns Object (only when it is dirty)
    static bool SaveAssetForObject(UObject* Object);
    // Resolve blueprint_name from command params and persist its package
    /** Persist the Blueprint named in `blueprint_name`. Returns whether a package was actually written. */
    static bool SaveBlueprintFromParams(const TSharedPtr<FJsonObject>& Params);

    /**
     * Read the optional `persist` parameter (default true). Commands that save on their own read this to
     * let a caller batch writes and flush once instead of paying one package write per command.
     */
    static bool IsPersistRequested(const TSharedPtr<FJsonObject>& Params);

    /**
     * Whether the command being dispatched wants its effects written to disk.
     *
     * The registry sets this once per dispatch from the `persist` parameter, and every helper that saves
     * on its own (graph library GuardPersist, the node-write result builder, the compile handlers) reads
     * it instead of taking the decision again - otherwise a batch that said `persist=false` still paid
     * one package write per command inside those helpers.
     *
     * True outside a dispatch, so a python script calling the graph library directly keeps the
     * persist-by-default contract.
     */
    static bool IsPersistEnabled();

    /** Holds one dispatch's persist decision; restores the previous value on exit (commands can nest). */
    struct FMCPPersistScope
    {
        explicit FMCPPersistScope(bool bInPersist);
        ~FMCPPersistScope();

    private:
        bool bPrevious;
    };
    // A response without an "error" field means the command succeeded
    static bool ResponseIndicatesSuccess(const TSharedPtr<FJsonObject>& Response);
    // On failure OutErrorCode receives the reflector's own code ("unknown_property" / "unknown_field" /
    // "type_mismatch" / "load_failed" / "unsupported_property_type" / "write_failed") and
    // OutAvailableFields the writable fields of the struct whose field name could not be resolved.
    // OutWriteResult (optional) hands back the full result for callers that surface container
    // position (failed_index) and the atomicity flag (unchanged).
    static bool SetObjectProperty(UObject* Object, const FString& PropertyName,
                                 const TSharedPtr<FJsonValue>& Value, FString& OutErrorMessage,
                                 TArray<FString>* OutAvailableFields = nullptr,
                                 FString* OutErrorCode = nullptr,
                                 FWriteResult* OutWriteResult = nullptr);

    // Generic asset utilities
    // Find any loaded asset by path, package name, or short name (supports Blueprint, Material, MaterialFunction, MaterialInstance, etc.)
    static UObject* FindAsset(const FString& AssetName);
    // Resolve a class reference written any of the ways callers write them:
    //   1. an object path ("/Script/Engine.Character", "/Game/BP/BP_X.BP_X_C") - FindObject then LoadClass
    //   2. the class name as it really is ("Character", "AnimNotify", "AnimNotifyState", "AnimInstance")
    //   3. a legacy short name that needs an "A" prefix ("Pawn" -> APawn) or "U" ("UserWidget" -> UUserWidget)
    // OutTried lists every form that was attempted (for the caller's error message); OutResolvedPath is the
    // resolved class' path so callers can read the real name back instead of echoing the request.
    static UClass* ResolveUClass(const FString& ClassReference, TArray<FString>& OutTried, FString& OutResolvedPath);
    /**
     * Create an asset with a factory, the way IAssetTools::CreateAsset does, but without its
     * CanCreateAsset step.
     *
     * CanCreateAsset calls UPackageTools::HandleFullyLoadingPackages, which synchronously fully loads
     * a package that is not loaded yet. For a path whose package was deleted earlier in the same
     * editor session the file is gone, and that load never finishes - the game thread stops inside
     * the MCP call (measured: `MCPCREATE: create-asset-begin` with no `create-asset-end`). The
     * conflict check that step performs is what the callers do themselves (FindAsset + disk check),
     * so the direct sequence is used instead.
     */
    static UObject* CreateAssetDirect(const FString& AssetName, const FString& AssetFolder,
                                      UClass* AssetClass, class UFactory* Factory);
    // Convert property + value ptr to JSON (recursive: handles structs/arrays)
    static TSharedPtr<FJsonValue> PropertyValueToJson(FProperty* Property, const void* ValuePtr);
    // Convert a UObject's properties to JSON (numeric, bool, string, enum, object ref)
    static TSharedPtr<FJsonObject> ObjectPropertiesToJson(UObject* Object);
}; 