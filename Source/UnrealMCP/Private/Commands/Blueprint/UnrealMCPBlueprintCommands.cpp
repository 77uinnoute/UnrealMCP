#include "Commands/Blueprint/UnrealMCPBlueprintCommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Commands/Blueprint/UnrealMCPBlueprintGraphOps.h"
#include "Commands/Blueprint/UnrealMCPBlueprintCommandHelpers.h"
#include "Reflection/MCPPropertyReflector.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Factories/BlueprintFactory.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Components/PrimitiveComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "UObject/Field.h"
#include "UObject/FieldPath.h"
#include "UObject/TopLevelAssetPath.h"
#include "ScopedTransaction.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"

// The response shapers every blueprint command file shares (error envelope with candidates,
// compile read-back, local variable names).
using namespace UnrealMCPBlueprintHelpers;

FUnrealMCPBlueprintCommands::FUnrealMCPBlueprintCommands()
{
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::RunCommand(const FString& CommandType,
    const TSharedPtr<FJsonObject>& Params,
    const TFunction<TSharedPtr<FJsonObject>(const TSharedPtr<FJsonObject>&)>& Body)
{
    const FMCPCommandEntry* Entry = FMCPCommandRegistry::Get().Find(CommandType);
    const bool bMutating = Entry && Entry->Flags.bMutatesGraph;

    // One MCP command = one undo step, so a wrong structural edit can be taken back with Ctrl+Z.
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

    // Persist blueprint mutations immediately (the editor is routinely killed by the build script).
    // The registry flag is the DEFAULT; an explicit `persist` parameter overrides it, so a batch of
    // writes can be flushed once by the caller instead of writing the package per command.
    bool bPersist = Entry && Entry->Flags.bPersistAfterSuccess;
    Params->TryGetBoolField(TEXT("persist"), bPersist);

    bool bSaved = false;
    if (bSuccess && bPersist)
    {
        bSaved = FUnrealMCPCommonUtils::SaveBlueprintFromParams(Params);
    }

    if (Result.IsValid())
    {
        // Commands that save themselves (create_blueprint) already reported `saved` - the wrapper
        // must not overwrite that with its own "I skipped the save" value.
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

void FUnrealMCPBlueprintCommands::RegisterCommands(FMCPCommandRegistry& Registry)
{
    MCP_REGISTER_COMMAND(Registry, "create_blueprint", "blueprint", "Create a Blueprint asset under /Game/Blueprints.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("name"), TEXT("string"), TEXT("Asset name of the new Blueprint")),
            MCPParamOpt(TEXT("parent_class"), TEXT("string"), TEXT("Parent class name (defaults to AActor)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, false, false, false) /* saves itself, not in the persist set */,
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("create_blueprint"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleCreateBlueprint(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "add_component_to_blueprint", "blueprint", "Add a component to a Blueprint's construction script.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("component_type"), TEXT("string"), TEXT("Component class name (e.g. StaticMeshComponent)")),
            MCPParam(TEXT("component_name"), TEXT("string"), TEXT("Variable name for the new component")),
            MCPParamOpt(TEXT("location"), TEXT("array"), TEXT("[X, Y, Z] relative location")),
            MCPParamOpt(TEXT("rotation"), TEXT("array"), TEXT("[Pitch, Yaw, Roll] relative rotation")),
            MCPParamOpt(TEXT("scale"), TEXT("array"), TEXT("[X, Y, Z] relative scale")),
            MCPParamOpt(TEXT("component_properties"), TEXT("object"), TEXT("Properties applied to the component template")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, false, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_component_to_blueprint"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddComponentToBlueprint(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_component_property", "blueprint", "Set one property on a Blueprint's component template.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("component_name"), TEXT("string"), TEXT("Component variable name")),
            MCPParam(TEXT("property_name"), TEXT("string"), TEXT("Property name on the component class")),
            MCPParam(TEXT("property_value"), TEXT("string"), TEXT("Value to set (number / bool / string / array / object)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, false, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_component_property"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetComponentProperty(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "attach_component_to_component", "blueprint", "Reparent a Blueprint component under another component, optionally to a socket.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("child_component"), TEXT("string"), TEXT("Component to attach")),
            MCPParam(TEXT("parent_component"), TEXT("string"), TEXT("Component to attach it to")),
            MCPParamOpt(TEXT("socket_name"), TEXT("string"), TEXT("Socket on the parent to attach to")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, false, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("attach_component_to_component"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAttachComponentToComponent(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_physics_properties", "blueprint", "Set physics flags, mass and damping on a Blueprint's primitive component.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("component_name"), TEXT("string"), TEXT("Primitive component variable name")),
            MCPParamOpt(TEXT("simulate_physics"), TEXT("bool"), TEXT("Enable physics simulation")),
            MCPParamOpt(TEXT("mass"), TEXT("float"), TEXT("Mass override in kg")),
            MCPParamOpt(TEXT("linear_damping"), TEXT("float"), TEXT("Linear damping")),
            MCPParamOpt(TEXT("angular_damping"), TEXT("float"), TEXT("Angular damping")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, false, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_physics_properties"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetPhysicsProperties(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "compile_blueprint", "blueprint", "Compile a Blueprint and report its status, errors and warnings.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the Blueprint to compile")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, false, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("compile_blueprint"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleCompileBlueprint(P); }); }));

    // spawn_blueprint_actor belongs to the editor domain: the legacy bridge chain already routed
    // it there, so this domain's arm for it was unreachable. Registering it here as well would only
    // produce a duplicate at Seal().

    MCP_REGISTER_COMMAND(Registry, "set_blueprint_property", "blueprint", "Set one property on a Blueprint's class default object.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("property_name"), TEXT("string"), TEXT("Property name on the Blueprint class")),
            MCPParam(TEXT("property_value"), TEXT("string"), TEXT("Value to set (number / bool / string / array / object)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, false, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_blueprint_property"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetBlueprintProperty(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_static_mesh_properties", "blueprint", "Set the static mesh and material of a Blueprint's StaticMeshComponent.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("component_name"), TEXT("string"), TEXT("StaticMeshComponent variable name")),
            MCPParamOpt(TEXT("static_mesh"), TEXT("string"), TEXT("Asset path of the static mesh")),
            MCPParamOpt(TEXT("material"), TEXT("string"), TEXT("Asset path of the material")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, false, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_static_mesh_properties"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetStaticMeshProperties(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_pawn_properties", "blueprint", "Set possess and controller-rotation defaults on a Pawn Blueprint.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParamOpt(TEXT("auto_possess_player"), TEXT("string"), TEXT("Auto possess setting (Disabled / Player0 / Player1 / ...)")),
            MCPParamOpt(TEXT("use_controller_rotation_yaw"), TEXT("bool"), TEXT("Use controller yaw")),
            MCPParamOpt(TEXT("use_controller_rotation_pitch"), TEXT("bool"), TEXT("Use controller pitch")),
            MCPParamOpt(TEXT("use_controller_rotation_roll"), TEXT("bool"), TEXT("Use controller roll")),
            MCPParamOpt(TEXT("can_be_damaged"), TEXT("bool"), TEXT("Actor can take damage")),
        }), MCPFlags(false, false, false, false) /* not in the persist set */,
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_pawn_properties"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetPawnProperties(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "get_asset_properties", "blueprint", "Return an asset's name, class, path and reflected properties.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_name"), TEXT("string"), TEXT("Name or path of the asset to read")),
        }), MCPFlags(false, false, false, false) /* read-only */,
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_asset_properties"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetAssetProperties(P); }); }));

    // --- Member variables: read, remove, rename, retype, default value, flags ---

    MCP_REGISTER_COMMAND(Registry, "list_blueprint_variables", "blueprint", "List a Blueprint's own member variables with type, default value and flags.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
        }), MCPFlags(false, false, false, false) /* read-only */,
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("list_blueprint_variables"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleListBlueprintVariables(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "get_blueprint_variable_info", "blueprint", "Read one member variable's type, default value and flags.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("variable_name"), TEXT("string"), TEXT("Member variable to read")),
        }), MCPFlags(false, false, false, false) /* read-only */,
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_blueprint_variable_info"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetBlueprintVariableInfo(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "remove_blueprint_variable", "blueprint", "Remove a member variable and the graph nodes that reference it.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("variable_name"), TEXT("string"), TEXT("Member variable to remove")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("remove_blueprint_variable"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRemoveBlueprintVariable(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "rename_blueprint_variable", "blueprint", "Rename a member variable; the graph nodes referencing it follow the new name.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("old_name"), TEXT("string"), TEXT("Current member variable name")),
            MCPParam(TEXT("new_name"), TEXT("string"), TEXT("New member variable name")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("rename_blueprint_variable"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRenameBlueprintVariable(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_blueprint_variable_type", "blueprint", "Change a member variable's type (same type grammar as add_blueprint_variable).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("variable_name"), TEXT("string"), TEXT("Member variable to retype")),
            MCPParam(TEXT("variable_type"), TEXT("string"), TEXT("Pin category, optionally with a container suffix (\"int[]\", \"struct{}\", \"int{,}\")")),
            MCPParamOpt(TEXT("sub_class"), TEXT("string"), TEXT("Class / struct / enum path for object and struct types")),
            MCPParamOpt(TEXT("container"), TEXT("string"), TEXT("Container kind (none / array / set / map) as an alternative to the type suffix")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_blueprint_variable_type"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetBlueprintVariableType(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_blueprint_variable_default_value", "blueprint", "Write a member variable's default value and read it back.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("variable_name"), TEXT("string"), TEXT("Member variable to write")),
            MCPParam(TEXT("default_value"), TEXT("string"), TEXT("Default value as text (\"42\", \"true\", \"(X=1,Y=2,Z=3)\")")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_blueprint_variable_default_value"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetBlueprintVariableDefaultValue(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_blueprint_variable_flags", "blueprint", "Set member variable flags and metadata (instance editable, replication, category, tooltip, ...).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("variable_name"), TEXT("string"), TEXT("Member variable to change")),
            MCPParam(TEXT("props"), TEXT("object"), TEXT("Flag name to value map (instance_editable / expose_on_spawn / blueprint_read_only / replicated / rep_notify_func / category / tooltip / transient / private)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_blueprint_variable_flags"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetBlueprintVariableFlags(P); }); }));

    // --- Local variables of a function graph ---

    MCP_REGISTER_COMMAND(Registry, "list_blueprint_local_variables", "blueprint", "List the local variables of a function graph.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("function_name"), TEXT("string"), TEXT("Function graph holding the local variables")),
        }), MCPFlags(false, false, false, false) /* read-only */,
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("list_blueprint_local_variables"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleListBlueprintLocalVariables(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "add_blueprint_local_variable", "blueprint", "Add a local variable to a function graph.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("function_name"), TEXT("string"), TEXT("Function graph that owns the local variable")),
            MCPParam(TEXT("variable_name"), TEXT("string"), TEXT("Name of the new local variable")),
            MCPParam(TEXT("variable_type"), TEXT("string"), TEXT("Pin category, optionally with a container suffix")),
            MCPParamOpt(TEXT("sub_class"), TEXT("string"), TEXT("Class / struct / enum path for object and struct types")),
            MCPParamOpt(TEXT("default_value"), TEXT("string"), TEXT("Default value as text")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_blueprint_local_variable"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddBlueprintLocalVariable(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "remove_blueprint_local_variable", "blueprint", "Remove a local variable and the graph nodes that reference it.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("function_name"), TEXT("string"), TEXT("Function graph that owns the local variable")),
            MCPParam(TEXT("variable_name"), TEXT("string"), TEXT("Local variable to remove")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("remove_blueprint_local_variable"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRemoveBlueprintLocalVariable(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "rename_blueprint_local_variable", "blueprint", "Rename a local variable; the graph nodes referencing it follow the new name.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("function_name"), TEXT("string"), TEXT("Function graph that owns the local variable")),
            MCPParam(TEXT("old_name"), TEXT("string"), TEXT("Current local variable name")),
            MCPParam(TEXT("new_name"), TEXT("string"), TEXT("New local variable name")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("rename_blueprint_local_variable"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRenameBlueprintLocalVariable(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_blueprint_local_variable_default", "blueprint", "Write a local variable's default value and read it back.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("function_name"), TEXT("string"), TEXT("Function graph that owns the local variable")),
            MCPParam(TEXT("variable_name"), TEXT("string"), TEXT("Local variable to write")),
            MCPParam(TEXT("default_value"), TEXT("string"), TEXT("Default value as text")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_blueprint_local_variable_default"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetBlueprintLocalVariableDefault(P); }); }));

    // --- Component hierarchy ---

    MCP_REGISTER_COMMAND(Registry, "get_blueprint_component_hierarchy", "blueprint", "Read a Blueprint's component tree: parent, root, socket, inheritance.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
        }), MCPFlags(false, false, false, false) /* read-only */,
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_blueprint_component_hierarchy"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetBlueprintComponentHierarchy(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "remove_component_from_blueprint", "blueprint", "Remove a component (and, by default, its children) from a Blueprint's construction script.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("component_name"), TEXT("string"), TEXT("Component variable name to remove")),
            MCPParamOpt(TEXT("recursive"), TEXT("bool"), TEXT("Also remove the component's children (default true)")),
            MCPParamOpt(TEXT("force"), TEXT("bool"), TEXT("Allow removing the blueprint's only root component (default false)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("remove_component_from_blueprint"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRemoveComponentFromBlueprint(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_blueprint_root_component", "blueprint", "Make a component the Blueprint's SCS root; the old root becomes its child.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("component_name"), TEXT("string"), TEXT("Scene component to promote to root")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_blueprint_root_component"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetBlueprintRootComponent(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "detach_component", "blueprint", "Detach a component from its parent and leave it under the SCS root (it is not deleted).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("component_name"), TEXT("string"), TEXT("Scene component to detach")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("detach_component"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleDetachComponent(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_component_collision", "blueprint", "Configure a primitive component's collision through the shared property reflector.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("component_name"), TEXT("string"), TEXT("Primitive component variable name")),
            MCPParam(TEXT("props"), TEXT("object"), TEXT("Collision field to value map (collision_enabled / collision_profile / object_type / responses)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_component_collision"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetComponentCollision(P); }); }));

    // --- Interfaces, existence checks, blueprint comparison ---

    MCP_REGISTER_COMMAND(Registry, "implement_blueprint_interface", "blueprint", "Implement an interface and read back the graphs the engine generated for it.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("interface_path"), TEXT("string"), TEXT("Interface class path (/Script/Module.Interface) or a blueprint interface asset path")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("implement_blueprint_interface"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleImplementBlueprintInterface(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "unimplement_blueprint_interface", "blueprint", "Remove an interface implementation and the graphs it produced.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("interface_path"), TEXT("string"), TEXT("Interface class path or blueprint interface asset path")),
            MCPParamOpt(TEXT("preserve_functions"), TEXT("bool"), TEXT("Keep the interface functions as normal blueprint functions (default false)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("unimplement_blueprint_interface"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleUnimplementBlueprintInterface(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "list_blueprint_interfaces", "blueprint", "List a Blueprint's implemented interfaces and each interface function's implementation state.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
        }), MCPFlags(false, false, false, false) /* read-only */,
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("list_blueprint_interfaces"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleListBlueprintInterfaces(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "list_overridable_functions", "blueprint", "List the parent / interface functions this Blueprint can override, with their override state.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParamOpt(TEXT("include_parent_events"), TEXT("bool"), TEXT("Include parent class events as well as interface functions (default true)")),
        }), MCPFlags(false, false, false, false) /* read-only */,
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("list_overridable_functions"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleListOverridableFunctions(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "check_blueprint_element", "blueprint", "Check whether one variable / function / component / graph / node exists, and locate it.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Blueprint")),
            MCPParam(TEXT("kind"), TEXT("string"), TEXT("variable / function / component / graph / node")),
            MCPParam(TEXT("name"), TEXT("string"), TEXT("Name (or node guid for kind=node) to look for")),
        }), MCPFlags(false, false, false, false) /* read-only */,
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("check_blueprint_element"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleCheckBlueprintElement(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "compare_blueprints", "blueprint", "Compare two Blueprints: variables, function graphs, components and interfaces.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_a"), TEXT("string"), TEXT("First Blueprint (left side)")),
            MCPParam(TEXT("blueprint_b"), TEXT("string"), TEXT("Second Blueprint (right side)")),
        }), MCPFlags(false, false, false, false) /* read-only */,
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("compare_blueprints"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleCompareBlueprints(P); }); }));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleCreateBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }

    // Check if blueprint already exists
    FString PackagePath = TEXT("/Game/Blueprints/");
    FString AssetName = BlueprintName;
    if (UEditorAssetLibrary::DoesAssetExist(PackagePath + AssetName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("asset_exists"),
            FString::Printf(TEXT("Blueprint already exists: %s"), *BlueprintName));
    }

    // Create the blueprint factory
    UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
    
    // Handle parent class
    FString ParentClass;
    Params->TryGetStringField(TEXT("parent_class"), ParentClass);

    // No parent_class given -> Actor. An explicit parent_class that cannot be resolved is an ERROR and no
    // asset is created: the old code prepended "A" and only recognised APawn/AActor, so Character, AnimNotify,
    // AnimInstance and plain typos all silently produced an Actor blueprint (see
    // TestResults/blueprint_parent_class.md).
    UClass* SelectedParentClass = AActor::StaticClass();
    if (!ParentClass.IsEmpty())
    {
        TArray<FString> TriedForms;
        FString ResolvedParentPath;
        UClass* Resolved = FUnrealMCPCommonUtils::ResolveUClass(ParentClass, TriedForms, ResolvedParentPath);
        if (!Resolved)
        {
            TSharedPtr<FJsonObject> ErrorResponse = FUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("parent_class_not_found"),
                FString::Printf(TEXT("parent_class_not_found: '%s'"), *ParentClass));
            ErrorResponse->SetStringField(TEXT("parent_class_requested"), ParentClass);
            TArray<TSharedPtr<FJsonValue>> TriedJson;
            for (const FString& Form : TriedForms)
            {
                TriedJson.Add(MakeShared<FJsonValueString>(Form));
            }
            ErrorResponse->SetArrayField(TEXT("tried"), TriedJson);
            return ErrorResponse;
        }
        SelectedParentClass = Resolved;
    }

    Factory->ParentClass = SelectedParentClass;

    // Create the blueprint
    UPackage* Package = CreatePackage(*(PackagePath + AssetName));
    UBlueprint* NewBlueprint = Cast<UBlueprint>(Factory->FactoryCreateNew(UBlueprint::StaticClass(), Package, *AssetName, RF_Standalone | RF_Public, nullptr, GWarn));

    if (NewBlueprint)
    {
        // Notify the asset registry
        FAssetRegistryModule::AssetCreated(NewBlueprint);

        // Mark the package dirty
        Package->MarkPackageDirty();

        // Save the package to disk so subsequent commands can load it by path.
        // `persist=false` skips the write: the asset stays in memory (in-memory lookups still find it,
        // but a path-based load from another session would not), which is what a batch that flushes
        // once wants.
        const FString AssetPath = PackagePath + AssetName;
        bool bPersist = true;
        Params->TryGetBoolField(TEXT("persist"), bPersist);
        bool bSaved = false;
        if (bPersist)
        {
            bSaved = FUnrealMCPCommonUtils::SaveAssetForObject(NewBlueprint);
        }

        // Compile once so the SimpleConstructionScript is bound before any component command runs
        FKismetEditorUtilities::CompileBlueprint(NewBlueprint);

        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("name"), AssetName);
        ResultObj->SetStringField(TEXT("path"), AssetPath);
        // Read the parent back instead of echoing the request: "Pawn" resolves to APawn, and a wrong
        // resolution used to be invisible to the caller.
        ResultObj->SetStringField(TEXT("parent_class"), SelectedParentClass ? SelectedParentClass->GetName() : TEXT(""));
        ResultObj->SetStringField(TEXT("parent_class_path"), SelectedParentClass ? SelectedParentClass->GetPathName() : TEXT(""));
        // The factory may pick a Blueprint subclass by itself (parent AnimInstance -> UAnimBlueprint).
        ResultObj->SetStringField(TEXT("blueprint_class"), NewBlueprint->GetClass()->GetName());
        // Non-Actor parents (AnimNotify / AnimInstance) have no SimpleConstructionScript; false here is expected,
        // not an error (component commands answer blueprint_not_ready for them).
        ResultObj->SetBoolField(TEXT("components_ready"),
            NewBlueprint->SimpleConstructionScript && NewBlueprint->SimpleConstructionScript->GetBlueprint() == NewBlueprint);
        ResultObj->SetBoolField(TEXT("saved"), bSaved);
        ResultObj->SetBoolField(TEXT("persist_requested"), bPersist);
        return ResultObj;
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create blueprint"));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleAddComponentToBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString ComponentType;
    if (!Params->TryGetStringField(TEXT("component_type"), ComponentType))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'type' parameter"));
    }

    FString ComponentName;
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Create the component - dynamically find the component class by name
    UClass* ComponentClass = nullptr;

    // Try to find the class with exact name first
    ComponentClass = FindObject<UClass>(UNREALMCP_ANY_PACKAGE, *ComponentType);
    
    // If not found, try with "Component" suffix
    if (!ComponentClass && !ComponentType.EndsWith(TEXT("Component")))
    {
        FString ComponentTypeWithSuffix = ComponentType + TEXT("Component");
        ComponentClass = FindObject<UClass>(UNREALMCP_ANY_PACKAGE, *ComponentTypeWithSuffix);
    }
    
    // If still not found, try with "U" prefix
    if (!ComponentClass && !ComponentType.StartsWith(TEXT("U")))
    {
        FString ComponentTypeWithPrefix = TEXT("U") + ComponentType;
        ComponentClass = FindObject<UClass>(UNREALMCP_ANY_PACKAGE, *ComponentTypeWithPrefix);
        
        // Try with both prefix and suffix
        if (!ComponentClass && !ComponentType.EndsWith(TEXT("Component")))
        {
            FString ComponentTypeWithBoth = TEXT("U") + ComponentType + TEXT("Component");
            ComponentClass = FindObject<UClass>(UNREALMCP_ANY_PACKAGE, *ComponentTypeWithBoth);
        }
    }
    
    // Verify that the class is a valid component type
    if (!ComponentClass || !ComponentClass->IsChildOf(UActorComponent::StaticClass()))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown component type: %s"), *ComponentType));
    }

    // The SCS must exist AND be bound to this blueprint before CreateNode, otherwise the engine
    // asserts (SimpleConstructionScript.cpp) and takes the editor process down with it.
    FString ReadyErrorCode;
    FString ReadyErrorMessage;
    if (!FUnrealMCPCommonUtils::EnsureBlueprintComponentsReady(Blueprint, ReadyErrorCode, ReadyErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ReadyErrorCode, ReadyErrorMessage);
    }

    // Add the component to the blueprint
    USCS_Node* NewNode = Blueprint->SimpleConstructionScript->CreateNode(ComponentClass, *ComponentName);
    if (!NewNode || !NewNode->ComponentTemplate)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("create_failed"),
            FString::Printf(TEXT("Failed to add component '%s' to blueprint '%s'"), *ComponentName, *Blueprint->GetName()));
    }

    // Set transform if provided
    USceneComponent* SceneComponent = Cast<USceneComponent>(NewNode->ComponentTemplate);
    if (SceneComponent)
    {
        if (Params->HasField(TEXT("location")))
        {
            SceneComponent->SetRelativeLocation(FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location")));
        }
        if (Params->HasField(TEXT("rotation")))
        {
            SceneComponent->SetRelativeRotation(FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation")));
        }
        if (Params->HasField(TEXT("scale")))
        {
            SceneComponent->SetRelativeScale3D(FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("scale")));
        }
    }

    // Add to root if no parent specified
    Blueprint->SimpleConstructionScript->AddNode(NewNode);

    // Apply component_properties on the fresh template (same dispatch as set_component_property)
    const TSharedPtr<FJsonObject>* ComponentPropsObject = nullptr;
    TArray<TSharedPtr<FJsonValue>> AppliedArray;
    TArray<TSharedPtr<FJsonValue>> FailedArray;

    if (Params->TryGetObjectField(TEXT("component_properties"), ComponentPropsObject) &&
        ComponentPropsObject && (*ComponentPropsObject).IsValid())
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*ComponentPropsObject)->Values)
        {
            FString PropertyErrorMessage;
            const bool bApplied = FUnrealMCPCommonUtils::SetObjectProperty(
                NewNode->ComponentTemplate, Pair.Key, Pair.Value, PropertyErrorMessage);

            FString RequestedValue;
            if (Pair.Value.IsValid())
            {
                switch (Pair.Value->Type)
                {
                case EJson::String:  RequestedValue = Pair.Value->AsString(); break;
                case EJson::Number:  RequestedValue = FString::SanitizeFloat(Pair.Value->AsNumber()); break;
                case EJson::Boolean: RequestedValue = Pair.Value->AsBool() ? TEXT("true") : TEXT("false"); break;
                default:             RequestedValue = TEXT("<non-scalar>"); break;
                }
            }

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("property_name"), Pair.Key);
            Entry->SetStringField(TEXT("requested_value"), RequestedValue);
            if (bApplied)
            {
                AppliedArray.Add(MakeShared<FJsonValueObject>(Entry));
            }
            else
            {
                Entry->SetStringField(TEXT("error"), PropertyErrorMessage);
                FailedArray.Add(MakeShared<FJsonValueObject>(Entry));
            }
        }
    }

    // Compile the blueprint
    FKismetEditorUtilities::CompileBlueprint(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("component_name"), ComponentName);
    ResultObj->SetStringField(TEXT("component_type"), ComponentType);
    ResultObj->SetStringField(TEXT("component_class"), NewNode->ComponentTemplate->GetClass()->GetName());
    ResultObj->SetStringField(TEXT("component_template"), NewNode->ComponentTemplate->GetPathName());
    ResultObj->SetArrayField(TEXT("applied"), AppliedArray);
    ResultObj->SetArrayField(TEXT("failed"), FailedArray);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleAttachComponentToComponent(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'blueprint_name' parameter"));
    }

    FString ChildComponentName;
    if (!Params->TryGetStringField(TEXT("child_component"), ChildComponentName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'child_component' parameter"));
    }

    FString ParentComponentName;
    if (!Params->TryGetStringField(TEXT("parent_component"), ParentComponentName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'parent_component' parameter"));
    }

    FString SocketName;
    Params->TryGetStringField(TEXT("socket_name"), SocketName);

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("blueprint_not_found"),
            FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString ErrorCode;
    FString ErrorMessage;
    USCS_Node* ChildNode = FUnrealMCPCommonUtils::FindBlueprintComponentNode(Blueprint, ChildComponentName, ErrorCode, ErrorMessage);
    if (!ChildNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorCode, ErrorMessage);
    }

    USCS_Node* ParentNode = FUnrealMCPCommonUtils::FindBlueprintComponentNode(Blueprint, ParentComponentName, ErrorCode, ErrorMessage);
    UClass* InheritedParentOwner = nullptr;
    UActorComponent* InheritedParent = nullptr;
    if (!ParentNode)
    {
        // The parent may be an inherited component: a parent blueprint's SCS component, or a native one
        // such as ACharacter::Mesh. USCS_Node::AddChildNode cannot express that parent, so it is recorded
        // on the child by name below (the engine's own shape for such a hierarchy).
        InheritedParent = FUnrealMCPCommonUtils::FindWritableComponentTemplate(
            Blueprint, ParentComponentName, ErrorCode, ErrorMessage, &InheritedParentOwner);
        if (!InheritedParent)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorCode, ErrorMessage);
        }
    }

    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;

    if (ChildNode == ParentNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_attach"),
            FString::Printf(TEXT("Component '%s' cannot be attached to itself"), *ChildComponentName));
    }

    // Attaching under one of your own descendants would build a cycle.
    for (USCS_Node* Walk = ParentNode; Walk; )
    {
        if (Walk == ChildNode)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_attach"),
                FString::Printf(TEXT("Component '%s' is an ancestor of '%s'; that would create a cycle in the component hierarchy"),
                    *ChildComponentName, *ParentComponentName));
        }
        Walk = SCS->FindParentNode(Walk);
    }

    USceneComponent* ChildTemplate = Cast<USceneComponent>(ChildNode->ComponentTemplate);
    UObject* ParentComponentObject = ParentNode ? static_cast<UObject*>(ParentNode->ComponentTemplate) : static_cast<UObject*>(InheritedParent);
    USceneComponent* ParentTemplate = Cast<USceneComponent>(ParentComponentObject);
    if (!ChildTemplate || !ParentTemplate)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_attach"),
            FString::Printf(TEXT("Both components must be scene components to form a hierarchy (child=%s, parent=%s)"),
                *ChildNode->ComponentTemplate->GetClass()->GetName(),
                *ParentComponentObject->GetClass()->GetName()));
    }

    SCS->Modify();

    // Detach from the current place. USCS_Node::AddChildNode appends to the SCS AllNodes array
    // without a duplicate check, so AllNodes may only be re-registered when the detach actually
    // removed the node from it (the root path); otherwise the list would collect duplicates.
    // The same holds for AddNode, which only self-checks RootNodes - so in the inherited-parent case
    // (where AddNode re-registers the node) the detach has to take the node out of AllNodes as well.
    bool bRemovedFromAllNodes = false;
    if (USCS_Node* OldParent = SCS->FindParentNode(ChildNode))
    {
        OldParent->RemoveChildNode(ChildNode, /*bRemoveFromAllNodes=*/ParentNode == nullptr);
        bRemovedFromAllNodes = (ParentNode == nullptr);
    }
    else if (SCS->GetRootNodes().Contains(ChildNode))
    {
        SCS->RemoveNode(ChildNode, /*bValidateSceneRootNodes=*/false);
        bRemovedFromAllNodes = true;
    }

    if (ParentNode)
    {
        ParentNode->AddChildNode(ChildNode, /*bAddToAllNodes=*/bRemovedFromAllNodes);

        // Both nodes come from this blueprint's own SCS (FindBlueprintComponentNode), so the parent is
        // expressed by the ChildNodes tree alone. The engine's own SCS editor only calls
        // USCS_Node::SetParent for a component inherited from a parent Blueprint (SSCSEditor.cpp:771-787);
        // writing those fields for a parent inside the same SCS makes the SCS report "possible cyclic
        // linkage" during its PostLoad fixup (SimpleConstructionScript.cpp:484), so clear them instead.
        // This also makes re-running the command the repair path for assets written by older revisions.
        ChildNode->bIsParentComponentNative = false;
        ChildNode->ParentComponentOrVariableName = NAME_None;
        ChildNode->ParentComponentOwnerClassName = NAME_None;
    }
    else
    {
        // Inherited parent. It cannot be expressed as an SCS child, so the node stays top-level and names
        // its parent instead - the shape the engine itself produces and consumes for a component hanging
        // under a native or inherited one (SimpleConstructionScript.cpp FixupRootNodeParentReferences):
        // native component -> bIsParentComponentNative, parent blueprint SCS component -> owner class.
        SCS->AddNode(ChildNode);
        const bool bParentIsNative = !InheritedParentOwner
            || !InheritedParentOwner->IsChildOf(UBlueprintGeneratedClass::StaticClass());
        ChildNode->bIsParentComponentNative = bParentIsNative;
        // Naming rule, straight from the engine's own fixup (SimpleConstructionScript.cpp:553 vs :580/:585):
        // a native parent is matched by the component's OBJECT name (ACharacter's Mesh object is named
        // 'CharacterMesh0', not 'Mesh'), while a parent blueprint's SCS component is matched by VARIABLE
        // name plus the declaring class. Writing the property name for a native parent silently falls back
        // to the scene root, because the fixup clears parent info it cannot resolve (:598-600).
        ChildNode->ParentComponentOrVariableName = bParentIsNative
            ? InheritedParent->GetFName()
            : FName(*ParentComponentName);
        ChildNode->ParentComponentOwnerClassName = bParentIsNative ? NAME_None : InheritedParentOwner->GetFName();
    }

    const FName SocketFName = SocketName.IsEmpty() ? NAME_None : FName(*SocketName);
    ChildNode->AttachToName = SocketFName;
    ChildTemplate->SetupAttachment(ParentTemplate, SocketFName);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    TArray<TSharedPtr<FJsonValue>> ComponentNames;
    for (USCS_Node* Node : SCS->GetAllNodes())
    {
        if (Node)
        {
            ComponentNames.Add(MakeShared<FJsonValueString>(Node->GetVariableName().ToString()));
        }
    }

    // Read the resulting hierarchy back off the SCS instead of echoing the request: the response
    // must report what the tree actually holds. A parent recorded by name (an inherited component) has no
    // SCS parent node, so report that name instead of calling the node a root.
    USCS_Node* ActualParent = SCS->FindParentNode(ChildNode);
    FString ActualParentName = ActualParent ? ActualParent->GetVariableName().ToString() : FString();
    const bool bParentRecordedByName = !ChildNode->ParentComponentOrVariableName.IsNone();
    if (ActualParentName.IsEmpty() && bParentRecordedByName)
    {
        ActualParentName = ChildNode->ParentComponentOrVariableName.ToString();
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("component_name"), ChildComponentName);
    ResultObj->SetStringField(TEXT("component_class"), ChildNode->ComponentTemplate->GetClass()->GetName());
    ResultObj->SetStringField(TEXT("component_template"), ChildNode->ComponentTemplate->GetPathName());
    ResultObj->SetStringField(TEXT("attach_parent"), ActualParentName);
    ResultObj->SetBoolField(TEXT("is_root"), ActualParent == nullptr && !bParentRecordedByName);
    ResultObj->SetBoolField(TEXT("attach_parent_inherited"), bParentRecordedByName);
    if (bParentRecordedByName)
    {
        // The name the engine will match against (an inherited native parent is stored as its object name).
        ResultObj->SetStringField(TEXT("attach_parent_recorded"), ChildNode->ParentComponentOrVariableName.ToString());
    }
    if (ChildNode->AttachToName.IsNone())
    {
        ResultObj->SetField(TEXT("attach_socket"), MakeShared<FJsonValueNull>());
    }
    else
    {
        ResultObj->SetStringField(TEXT("attach_socket"), ChildNode->AttachToName.ToString());
    }
    ResultObj->SetArrayField(TEXT("components"), ComponentNames);
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleSetComponentProperty(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString ComponentName;
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'component_name' parameter"));
    }

    FString PropertyName;
    if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'property_name' parameter"));
    }

    // Log all input parameters for debugging
    UE_LOG(LogTemp, Warning, TEXT("SetComponentProperty - Blueprint: %s, Component: %s, Property: %s"), 
        *BlueprintName, *ComponentName, *PropertyName);
    
    // Log property_value if available
    if (Params->HasField(TEXT("property_value")))
    {
        TSharedPtr<FJsonValue> JsonValue = Params->Values.FindRef(TEXT("property_value"));
        FString ValueType;
        
        switch(JsonValue->Type)
        {
            case EJson::Boolean: ValueType = FString::Printf(TEXT("Boolean: %s"), JsonValue->AsBool() ? TEXT("true") : TEXT("false")); break;
            case EJson::Number: ValueType = FString::Printf(TEXT("Number: %f"), JsonValue->AsNumber()); break;
            case EJson::String: ValueType = FString::Printf(TEXT("String: %s"), *JsonValue->AsString()); break;
            case EJson::Array: ValueType = TEXT("Array"); break;
            case EJson::Object: ValueType = TEXT("Object"); break;
            default: ValueType = TEXT("Unknown"); break;
        }
        
        UE_LOG(LogTemp, Warning, TEXT("SetComponentProperty - Value Type: %s"), *ValueType);
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("SetComponentProperty - No property_value provided"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        UE_LOG(LogTemp, Error, TEXT("SetComponentProperty - Blueprint not found: %s"), *BlueprintName);
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }
    else
    {
        UE_LOG(LogTemp, Log, TEXT("SetComponentProperty - Blueprint found: %s (Class: %s)"), 
            *BlueprintName, 
            Blueprint->GeneratedClass ? *Blueprint->GeneratedClass->GetName() : TEXT("NULL"));
    }

    // Find the writable component template through the shared safety checks (never dereferences a missing
    // SCS): the blueprint's own SCS node first, then an inherited component (a parent blueprint's SCS node
    // or a native component such as ACharacter::Mesh), whose write must land on this class's own instance.
    FString LookupErrorCode;
    FString LookupErrorMessage;
    UActorComponent* ComponentTemplate = FUnrealMCPCommonUtils::FindWritableComponentTemplate(
        Blueprint, ComponentName, LookupErrorCode, LookupErrorMessage);
    if (!ComponentTemplate)
    {
        UE_LOG(LogTemp, Error, TEXT("SetComponentProperty - %s"), *LookupErrorMessage);
        return FUnrealMCPCommonUtils::CreateErrorResponse(LookupErrorCode, LookupErrorMessage);
    }

    UE_LOG(LogTemp, Log, TEXT("SetComponentProperty - Component found: %s (Class: %s)"), 
        *ComponentName, 
        *ComponentTemplate->GetClass()->GetName());

    // The helper already guarantees a non-null template for both the SCS and the inherited case.

    // All component classes, including SpringArm, use the same reflected write/readback path.
    // Class-specific direct writes previously skipped typed before/after receipts and engine normalization.

    // Set the property value
    if (Params->HasField(TEXT("property_value")))
    {
        TSharedPtr<FJsonValue> JsonValue = Params->Values.FindRef(TEXT("property_value"));
        
        // Get the property
        FProperty* Property = FUnrealMCPCommonUtils::FindPropertyByNameNormalized(ComponentTemplate->GetClass(), PropertyName);
        if (!Property)
        {
            UE_LOG(LogTemp, Error, TEXT("SetComponentProperty - Property %s not found on component %s"), 
                *PropertyName, *ComponentName);
            
            // List all available properties for this component
            UE_LOG(LogTemp, Warning, TEXT("SetComponentProperty - Available properties for %s:"), *ComponentName);
            for (TFieldIterator<FProperty> PropIt(ComponentTemplate->GetClass()); PropIt; ++PropIt)
            {
                FProperty* Prop = *PropIt;
                UE_LOG(LogTemp, Warning, TEXT("  - %s (%s)"), *Prop->GetName(), *Prop->GetCPPType());
            }
            
            TArray<FString> Suggestions;
            FUnrealMCPCommonUtils::FindPropertyNameSuggestions(ComponentTemplate, PropertyName, Suggestions);

            TArray<FString> CandidateProperties;
            for (TFieldIterator<FProperty> PropertyIt(ComponentTemplate->GetClass()); PropertyIt; ++PropertyIt)
            {
                CandidateProperties.Add(PropertyIt->GetName());
            }

            const FString SuggestionText = Suggestions.Num() > 0
                ? FString::Join(Suggestions, TEXT(", "))
                : FString::Join(CandidateProperties, TEXT(", "));

            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unknown_property"),
                FString::Printf(TEXT("Property '%s' not found on component '%s'. Did you mean: %s"),
                    *PropertyName, *ComponentName, *SuggestionText));
        }
        else
        {
            UE_LOG(LogTemp, Log, TEXT("SetComponentProperty - Property found: %s (Type: %s)"), 
                *PropertyName, *Property->GetCPPType());
        }

        // Read the current value first so the response can report before/after
        const TSharedPtr<FJsonValue> PropertyValueBefore = FUnrealMCPCommonUtils::PropertyValueToJson(
            Property, Property->ContainerPtrToValuePtr<void>(ComponentTemplate));

        bool bSuccess = false;
        FString ErrorMessage;
        FWriteResult WriteResult;

        // One write path: the reflector owns the type dispatch and the property value address.
        // The old local chain passed the component template pointer itself as the value address for
        // enum and numeric writes, which clobbered whatever property lives at offset 0.
        try
        {
            WriteResult = FMCPPropertyReflector::FromJson(
                Property, Property->ContainerPtrToValuePtr<void>(ComponentTemplate), PropertyName, JsonValue);
            bSuccess = WriteResult.bSuccess;
            if (bSuccess)
            {
                // `UPROPERTY(Setter = ...)` fields are often only a mirror (SkeletalMeshAsset -> SkinnedAsset),
                // so run the declared setter too or the component keeps no mesh while the receipt looks fine.
                FUnrealMCPCommonUtils::InvokePropertySetter(
                    ComponentTemplate, Property, Property->ContainerPtrToValuePtr<void>(ComponentTemplate));
            }
            if (!bSuccess)
            {
                ErrorMessage = WriteResult.ErrorMessage;
                UE_LOG(LogTemp, Warning, TEXT("SetComponentProperty - %s"), *ErrorMessage);
            }
        }
        catch (const std::exception& Ex)
        {
            UE_LOG(LogTemp, Error, TEXT("SetComponentProperty - EXCEPTION: %s"), ANSI_TO_TCHAR(Ex.what()));
            return FUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Exception while setting property %s: %s"), *PropertyName, ANSI_TO_TCHAR(Ex.what())));
        }
        catch (...)
        {
            UE_LOG(LogTemp, Error, TEXT("SetComponentProperty - UNKNOWN EXCEPTION occurred while setting property %s"), *PropertyName);
            return FUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Unknown exception while setting property %s"), *PropertyName));
        }

        if (bSuccess)
        {
            // Mark the blueprint as modified
            UE_LOG(LogTemp, Log, TEXT("SetComponentProperty - Successfully set property %s on component %s"), 
                *PropertyName, *ComponentName);
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

            TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
            ResultObj->SetStringField(TEXT("component"), ComponentName);
            ResultObj->SetStringField(TEXT("property"), PropertyName);
            ResultObj->SetField(TEXT("property_value_before"), PropertyValueBefore);
            ResultObj->SetField(TEXT("property_value_after"), FUnrealMCPCommonUtils::PropertyValueToJson(
                Property, Property->ContainerPtrToValuePtr<void>(ComponentTemplate)));
            ResultObj->SetBoolField(TEXT("success"), true);
            return ResultObj;
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("SetComponentProperty - Failed to set property %s: %s"), 
                *PropertyName, *ErrorMessage);

            TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(
                WriteResult.ErrorCode.IsEmpty() ? TEXT("type_mismatch") : WriteResult.ErrorCode, ErrorMessage);
            Error->SetStringField(TEXT("property"), PropertyName);
            Error->SetField(TEXT("property_value_before"), PropertyValueBefore);
            Error->SetField(TEXT("property_value_after"), FUnrealMCPCommonUtils::PropertyValueToJson(
                Property, Property->ContainerPtrToValuePtr<void>(ComponentTemplate)));
            Error->SetBoolField(TEXT("unchanged"), WriteResult.bUnchanged);

            const auto AddStringArray = [&Error](const TCHAR* FieldName, const TArray<FString>& Values)
            {
                if (Values.Num() == 0)
                {
                    return;
                }
                TArray<TSharedPtr<FJsonValue>> Items;
                for (const FString& Value : Values)
                {
                    Items.Add(MakeShared<FJsonValueString>(Value));
                }
                Error->SetArrayField(FieldName, Items);
            };
            AddStringArray(TEXT("supported_shapes"), WriteResult.SupportedShapes);
            AddStringArray(TEXT("available_fields"), WriteResult.AvailableFields);
            // Legal members of the enum whose member name did not resolve.
            AddStringArray(TEXT("candidates"), WriteResult.Candidates);
            if (!WriteResult.Hint.IsEmpty())
            {
                Error->SetStringField(TEXT("hint"), WriteResult.Hint);
            }
            return Error;
        }
    }

    UE_LOG(LogTemp, Error, TEXT("SetComponentProperty - Missing 'property_value' parameter"));
    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'property_value' parameter"));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleSetPhysicsProperties(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString ComponentName;
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'component_name' parameter"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Find the component through the shared safety checks (never dereferences a missing SCS)
    FString LookupErrorCode;
    FString LookupErrorMessage;
    USCS_Node* ComponentNode = FUnrealMCPCommonUtils::FindBlueprintComponentNode(
        Blueprint, ComponentName, LookupErrorCode, LookupErrorMessage);
    if (!ComponentNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(LookupErrorCode, LookupErrorMessage);
    }

    UPrimitiveComponent* PrimComponent = Cast<UPrimitiveComponent>(ComponentNode->ComponentTemplate);
    if (!PrimComponent)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Component is not a primitive component"));
    }

    // Set physics properties
    if (Params->HasField(TEXT("simulate_physics")))
    {
        PrimComponent->SetSimulatePhysics(Params->GetBoolField(TEXT("simulate_physics")));
    }

    if (Params->HasField(TEXT("mass")))
    {
        float Mass = Params->GetNumberField(TEXT("mass"));
        // In UE5.5, use proper overrideMass instead of just scaling
        PrimComponent->SetMassOverrideInKg(NAME_None, Mass);
        UE_LOG(LogTemp, Display, TEXT("Set mass for component %s to %f kg"), *ComponentName, Mass);
    }

    if (Params->HasField(TEXT("linear_damping")))
    {
        PrimComponent->SetLinearDamping(Params->GetNumberField(TEXT("linear_damping")));
    }

    if (Params->HasField(TEXT("angular_damping")))
    {
        PrimComponent->SetAngularDamping(Params->GetNumberField(TEXT("angular_damping")));
    }

    // Mark the blueprint as modified
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("component"), ComponentName);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleCompileBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Compile the blueprint. Node messages from an earlier failing compile survive a later successful
    // one, so clear them first: the diagnostics below must describe this compile only.
    FUnrealMCPBlueprintGraphOps::ResetCompilerMessages(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint);

    // Read back the real result instead of assuming success
    const UEnum* StatusEnum = StaticEnum<EBlueprintStatus>();
    const FString StatusName = StatusEnum
        ? StatusEnum->GetNameStringByValue(static_cast<int64>(Blueprint->Status))
        : FString::FromInt(static_cast<int32>(Blueprint->Status));

    TArray<TSharedPtr<FJsonValue>> Errors;
    TArray<TSharedPtr<FJsonValue>> Warnings;

    auto CollectGraphDiagnostics = [&Errors, &Warnings](const UEdGraph* Graph)
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

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
            Entry->SetStringField(TEXT("node"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
            Entry->SetStringField(TEXT("message"), Node->ErrorMsg);
            Entry->SetStringField(TEXT("graph"), Graph->GetName());

            if (Node->bHasCompilerMessage && Node->ErrorType == EMessageSeverity::Warning)
            {
                Warnings.Add(MakeShared<FJsonValueObject>(Entry));
            }
            else
            {
                Errors.Add(MakeShared<FJsonValueObject>(Entry));
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

    // Engine semantics (same as UBlueprint::IsUpToDate): warnings still mean "compiled"
    const bool bCompiled = Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings;

    // Persist the regenerated class and the (now clean) compile status, the same way the reflection
    // library's CompileBlueprintChecked does. Leaving the asset dirty means the next editor start
    // regenerates the blueprint again - and a load-time regeneration reconstructs every node, which
    // resets schema-derived pin types (UEdGraphPin::TransferPersistentDataFromOldPin copies defaults and
    // links but explicitly not PinType). That is how BP_TPSDemoGameMode kept losing the type of its
    // Array_Length pins and came back with a compile error after every restart.
    //
    // A caller that passed persist=false takes that responsibility on itself (flush when the batch is
    // done), which is the point of the flag; this used to save regardless of it.
    const bool bSaved = FUnrealMCPCommonUtils::IsPersistEnabled()
        ? FUnrealMCPCommonUtils::SaveAssetForObject(Blueprint)
        : false;

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("name"), BlueprintName);
    ResultObj->SetStringField(TEXT("status"), StatusName);
    ResultObj->SetBoolField(TEXT("compiled"), bCompiled);
    ResultObj->SetBoolField(TEXT("saved"), bSaved);
    ResultObj->SetArrayField(TEXT("errors"), Errors);
    ResultObj->SetArrayField(TEXT("warnings"), Warnings);
    if (!bCompiled && Blueprint->Status == BS_Error && Errors.Num() == 0)
    {
        TArray<FString> Hints;
        for (UEdGraph* Graph : Blueprint->FunctionGraphs)
        {
            if (!Graph) continue;
            bool bEntry = false;
            bool bResult = false;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                bEntry |= Node && Node->IsA<UK2Node_FunctionEntry>();
                bResult |= Node && Node->IsA<UK2Node_FunctionResult>();
            }
            if (!bEntry)
            {
                Hints.Add(FString::Printf(TEXT("%s: missing_entry"), *Graph->GetName()));
            }
            UFunction* Function = Blueprint->GeneratedClass
                ? Blueprint->GeneratedClass->FindFunctionByName(Graph->GetFName()) : nullptr;
            bool bHasOutput = false;
            if (Function)
            {
                for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
                {
                    bHasOutput |= It->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm);
                }
            }
            if (bHasOutput && !bResult)
            {
                Hints.Add(FString::Printf(TEXT("%s: missing_result"), *Graph->GetName()));
            }
        }
        if (Hints.Num() > 0)
        {
            ResultObj->SetStringField(TEXT("skeleton_hint"), FString::Join(Hints, TEXT("; ")));
        }
    }
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleSpawnBlueprintActor(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString ActorName;
    if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'actor_name' parameter"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Get transform parameters
    FVector Location(0.0f, 0.0f, 0.0f);
    FRotator Rotation(0.0f, 0.0f, 0.0f);

    if (Params->HasField(TEXT("location")))
    {
        Location = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location"));
    }
    if (Params->HasField(TEXT("rotation")))
    {
        Rotation = FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation"));
    }

    // Spawn the actor
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    FTransform SpawnTransform;
    SpawnTransform.SetLocation(Location);
    SpawnTransform.SetRotation(FQuat(Rotation));

    AActor* NewActor = World->SpawnActor<AActor>(Blueprint->GeneratedClass, SpawnTransform);
    if (NewActor)
    {
        NewActor->SetActorLabel(*ActorName);
        return FUnrealMCPCommonUtils::ActorToJsonObject(NewActor, true);
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to spawn blueprint actor"));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleSetBlueprintProperty(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString PropertyName;
    if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'property_name' parameter"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Get the default object
    UObject* DefaultObject = Blueprint->GeneratedClass->GetDefaultObject();
    if (!DefaultObject)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get default object"));
    }

    // Set the property value
    if (Params->HasField(TEXT("property_value")))
    {
        FProperty* Property = FindFProperty<FProperty>(DefaultObject->GetClass(), *PropertyName);
        if (!Property)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unknown_property"),
                FString::Printf(TEXT("Property '%s' not found on '%s'"), *PropertyName, *DefaultObject->GetClass()->GetName()));
        }
        TSharedPtr<FJsonValue> Before = FUnrealMCPCommonUtils::PropertyValueToJson(
            Property, Property->ContainerPtrToValuePtr<void>(DefaultObject));
        TSharedPtr<FJsonValue> JsonValue = Params->Values.FindRef(TEXT("property_value"));
        
        FString ErrorMessage;
        if (FUnrealMCPCommonUtils::SetObjectProperty(DefaultObject, PropertyName, JsonValue, ErrorMessage))
        {
            // Mark the blueprint as modified
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

            TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
            ResultObj->SetStringField(TEXT("property"), PropertyName);
            ResultObj->SetField(TEXT("property_value_before"), Before);
            ResultObj->SetField(TEXT("property_value_after"), FUnrealMCPCommonUtils::PropertyValueToJson(
                Property, Property->ContainerPtrToValuePtr<void>(DefaultObject)));
            ResultObj->SetBoolField(TEXT("success"), true);
            return ResultObj;
        }
        else
        {
            TSharedPtr<FJsonObject> Failure = FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
            Failure->SetField(TEXT("property_value_before"), Before);
            Failure->SetField(TEXT("property_value_after"), FUnrealMCPCommonUtils::PropertyValueToJson(
                Property, Property->ContainerPtrToValuePtr<void>(DefaultObject)));
            return Failure;
        }
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'property_value' parameter"));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleSetStaticMeshProperties(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString ComponentName;
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'component_name' parameter"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Find the component through the shared safety checks (never dereferences a missing SCS)
    FString LookupErrorCode;
    FString LookupErrorMessage;
    USCS_Node* ComponentNode = FUnrealMCPCommonUtils::FindBlueprintComponentNode(
        Blueprint, ComponentName, LookupErrorCode, LookupErrorMessage);
    if (!ComponentNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(LookupErrorCode, LookupErrorMessage);
    }

    UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(ComponentNode->ComponentTemplate);
    if (!MeshComponent)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Component is not a static mesh component"));
    }

    // Set static mesh properties
    if (Params->HasField(TEXT("static_mesh")))
    {
        FString MeshPath = Params->GetStringField(TEXT("static_mesh"));
        UStaticMesh* Mesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(MeshPath));
        if (Mesh)
        {
            MeshComponent->SetStaticMesh(Mesh);
        }
    }

    if (Params->HasField(TEXT("material")))
    {
        FString MaterialPath = Params->GetStringField(TEXT("material"));
        UMaterialInterface* Material = Cast<UMaterialInterface>(UEditorAssetLibrary::LoadAsset(MaterialPath));
        if (Material)
        {
            MeshComponent->SetMaterial(0, Material);
        }
    }

    // Mark the blueprint as modified
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("component"), ComponentName);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleSetPawnProperties(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Get the default object
    UObject* DefaultObject = Blueprint->GeneratedClass->GetDefaultObject();
    if (!DefaultObject)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get default object"));
    }

    // Track if any properties were set successfully
    bool bAnyPropertiesSet = false;
    TSharedPtr<FJsonObject> ResultsObj = MakeShared<FJsonObject>();
    
    auto WritePawnProperty = [&](const FString& PropertyName, const TSharedPtr<FJsonValue>& Value)
    {
        TSharedPtr<FJsonObject> PropResult = MakeShared<FJsonObject>();
        FProperty* Property = FindFProperty<FProperty>(DefaultObject->GetClass(), *PropertyName);
        if (!Property)
        {
            PropResult->SetBoolField(TEXT("success"), false);
            PropResult->SetStringField(TEXT("error"), TEXT("Property not found"));
            ResultsObj->SetObjectField(PropertyName, PropResult);
            return false;
        }
        PropResult->SetField(TEXT("property_value_before"), FUnrealMCPCommonUtils::PropertyValueToJson(
            Property, Property->ContainerPtrToValuePtr<void>(DefaultObject)));
        FString ErrorMessage;
        const bool bSuccess = FUnrealMCPCommonUtils::SetObjectProperty(DefaultObject, PropertyName, Value, ErrorMessage);
        PropResult->SetBoolField(TEXT("success"), bSuccess);
        PropResult->SetField(TEXT("property_value_after"), FUnrealMCPCommonUtils::PropertyValueToJson(
            Property, Property->ContainerPtrToValuePtr<void>(DefaultObject)));
        if (!bSuccess) PropResult->SetStringField(TEXT("error"), ErrorMessage);
        ResultsObj->SetObjectField(PropertyName, PropResult);
        bAnyPropertiesSet |= bSuccess;
        return bSuccess;
    };

    // Set auto possess player if specified
    if (Params->HasField(TEXT("auto_possess_player")))
    {
        WritePawnProperty(TEXT("AutoPossessPlayer"), Params->Values.FindRef(TEXT("auto_possess_player")));
    }
    
    // Set controller rotation properties
    const TCHAR* RotationProps[] = {
        TEXT("bUseControllerRotationYaw"),
        TEXT("bUseControllerRotationPitch"),
        TEXT("bUseControllerRotationRoll")
    };
    
    const TCHAR* ParamNames[] = {
        TEXT("use_controller_rotation_yaw"),
        TEXT("use_controller_rotation_pitch"),
        TEXT("use_controller_rotation_roll")
    };
    
    for (int32 i = 0; i < 3; i++)
    {
        if (Params->HasField(ParamNames[i]))
        {
            WritePawnProperty(RotationProps[i], Params->Values.FindRef(ParamNames[i]));
        }
    }
    
    // Set can be damaged property
    if (Params->HasField(TEXT("can_be_damaged")))
    {
        WritePawnProperty(TEXT("bCanBeDamaged"), Params->Values.FindRef(TEXT("can_be_damaged")));
    }

    // Mark the blueprint as modified if any properties were set
    if (bAnyPropertiesSet)
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    }
    else if (ResultsObj->Values.Num() == 0)
    {
        // No properties were specified
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No properties specified to set"));
    }

    TSharedPtr<FJsonObject> ResponseObj = MakeShared<FJsonObject>();
    ResponseObj->SetStringField(TEXT("blueprint"), BlueprintName);
    ResponseObj->SetBoolField(TEXT("success"), bAnyPropertiesSet);
    ResponseObj->SetObjectField(TEXT("results"), ResultsObj);
    return ResponseObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleGetAssetProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetName;
    if (!Params->TryGetStringField(TEXT("asset_name"), AssetName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_name' parameter"));
    }

    UObject* Asset = FUnrealMCPCommonUtils::FindAsset(AssetName);
    if (!Asset)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Asset not found: %s"), *AssetName));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("name"), Asset->GetName());
    ResultObj->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
    ResultObj->SetStringField(TEXT("path"), Asset->GetPathName());
    ResultObj->SetObjectField(TEXT("properties"), FUnrealMCPCommonUtils::ObjectPropertiesToJson(Asset));

    return ResultObj;
}

// =====================================================================================
// Interfaces, existence checks and blueprint comparison
// =====================================================================================

namespace
{
    /**
     * Accept both spellings callers use for an interface: the class path the engine's own API needs
     * ("/Script/Engine.Interface_AssetUserData") and the blueprint interface asset path ("/Game/BPI_X",
     * whose generated class carries the real path).
     */
    UClass* ResolveInterfaceClass(const FString& InterfacePath, TArray<FString>& OutTried)
    {
        TArray<FString> Tried;
        FString ResolvedPath;
        UClass* Class = FUnrealMCPCommonUtils::ResolveUClass(InterfacePath, Tried, ResolvedPath);
        OutTried.Append(Tried);

        if (!Class)
        {
            if (UObject* Asset = FUnrealMCPCommonUtils::FindAsset(InterfacePath))
            {
                if (UBlueprint* InterfaceBlueprint = Cast<UBlueprint>(Asset))
                {
                    Class = InterfaceBlueprint->GeneratedClass;
                }
                else
                {
                    Class = Cast<UClass>(Asset);
                }
            }
        }

        if (Class && Class->HasAnyClassFlags(CLASS_Interface))
        {
            return Class;
        }
        return nullptr;
    }

    const FBPInterfaceDescription* FindImplementedInterface(const UBlueprint* Blueprint, const UClass* InterfaceClass)
    {
        if (!Blueprint || !InterfaceClass)
        {
            return nullptr;
        }
        return Blueprint->ImplementedInterfaces.FindByPredicate(
            [InterfaceClass](const FBPInterfaceDescription& Description)
            {
                return Description.Interface == InterfaceClass;
            });
    }

    TArray<FString> MakeInterfaceNames(const UBlueprint* Blueprint)
    {
        TArray<FString> Names;
        if (Blueprint)
        {
            for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
            {
                if (Description.Interface)
                {
                    Names.Add(Description.Interface->GetPathName());
                }
            }
        }
        return Names;
    }

    TArray<FString> MakeFunctionGraphNames(const UBlueprint* Blueprint)
    {
        TArray<FString> Names;
        if (Blueprint)
        {
            for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
            {
                if (Graph)
                {
                    Names.Add(Graph->GetName());
                }
            }
        }
        return Names;
    }

    /** Function / event names the blueprint already implements, used as the "implemented" test. */
    TSet<FName> CollectImplementedEventNames(const UBlueprint* Blueprint)
    {
        TSet<FName> Names;
        if (!Blueprint)
        {
            return Names;
        }

        for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
        {
            if (Graph)
            {
                Names.Add(Graph->GetFName());
            }
        }

        TArray<UK2Node_Event*> EventNodes;
        FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, EventNodes);
        for (const UK2Node_Event* EventNode : EventNodes)
        {
            if (EventNode)
            {
                Names.Add(EventNode->EventReference.GetMemberName());
            }
        }
        return Names;
    }

    /**
     * Every graph the blueprint owns that can carry an interface implementation.
     *
     * FBlueprintEditorUtils::AddInterfaceGraph puts an interface function's graph in the interface
     * description (FBPInterfaceDescription::Graphs) rather than in Blueprint->FunctionGraphs, so a
     * diff over FunctionGraphs alone cannot see interface graphs appear or disappear.
     */
    void CollectImplementationGraphNames(const UBlueprint* Blueprint, TArray<FString>& OutNames)
    {
        if (!Blueprint)
        {
            return;
        }
        for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
        {
            if (Graph)
            {
                OutNames.AddUnique(Graph->GetName());
            }
        }
        for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
        {
            for (const UEdGraph* Graph : Description.Graphs)
            {
                if (Graph)
                {
                    OutNames.AddUnique(Graph->GetName());
                }
            }
        }
    }

    TSharedPtr<FJsonObject> MakeInterfaceJson(UBlueprint* Blueprint, const FBPInterfaceDescription& Description)
    {
        UClass* InterfaceClass = Description.Interface;

        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("interface_name"), InterfaceClass ? InterfaceClass->GetName() : FString());
        Obj->SetStringField(TEXT("interface_path"),
            InterfaceClass ? InterfaceClass->GetClassPathName().ToString() : FString());

        TArray<FString> InterfaceGraphNames;
        for (const UEdGraph* Graph : Description.Graphs)
        {
            if (Graph)
            {
                InterfaceGraphNames.Add(Graph->GetName());
            }
        }

        // An interface function is implemented either by one of the interface's own graphs (a
        // function-style signature) or by an event placed in an ubergraph (an event-style one).
        const TSet<FName> ImplementedNames = CollectImplementedEventNames(Blueprint);

        TArray<TSharedPtr<FJsonValue>> Functions;
        if (InterfaceClass)
        {
            for (TFieldIterator<UFunction> It(InterfaceClass); It; ++It)
            {
                UFunction* Function = *It;
                if (!Function)
                {
                    continue;
                }

                const bool bImplemented = InterfaceGraphNames.Contains(Function->GetName())
                    || ImplementedNames.Contains(Function->GetFName());

                TSharedPtr<FJsonObject> FunctionObj = MakeShared<FJsonObject>();
                FunctionObj->SetStringField(TEXT("function_name"), Function->GetName());
                FunctionObj->SetBoolField(TEXT("implemented"), bImplemented);
                Functions.Add(MakeShared<FJsonValueObject>(FunctionObj));
            }
        }
        Obj->SetArrayField(TEXT("functions"), Functions);

        TArray<FString> GraphNames;
        for (const UEdGraph* Graph : Description.Graphs)
        {
            if (Graph)
            {
                GraphNames.Add(Graph->GetName());
            }
        }
        Obj->SetArrayField(TEXT("graphs"), MakeStringArray(GraphNames));
        return Obj;
    }

    TArray<TSharedPtr<FJsonValue>> MakeInterfaceJsonArray(UBlueprint* Blueprint)
    {
        TArray<TSharedPtr<FJsonValue>> Items;
        for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
        {
            Items.Add(MakeShared<FJsonValueObject>(MakeInterfaceJson(Blueprint, Description)));
        }
        return Items;
    }

    /** A comparable signature string per element, so "changed" means a real difference. */
    FString MakeVariableSignature(const FBPVariableDescription& Variable)
    {
        return FString::Printf(TEXT("%s/%s/%s"), *Variable.VarType.PinCategory.ToString(),
            *MakeContainerName(Variable.VarType), *MakeSubClassString(Variable.VarType));
    }

    FString MakeFunctionSignature(UEdGraph* Graph)
    {
        if (!Graph)
        {
            return FString();
        }

        TArray<TSharedPtr<FJsonObject>> Params;
        FUnrealMCPBlueprintGraphOps::DescribeFunctionParams(Graph, Params);

        TArray<FString> Parts;
        for (const TSharedPtr<FJsonObject>& Param : Params)
        {
            FString Name;
            FString Direction;
            FString Type;
            FString Container;
            if (Param.IsValid())
            {
                Param->TryGetStringField(TEXT("name"), Name);
                Param->TryGetStringField(TEXT("direction"), Direction);
                Param->TryGetStringField(TEXT("type"), Type);
                Param->TryGetStringField(TEXT("container"), Container);
            }
            Parts.Add(FString::Printf(TEXT("%s:%s:%s%s"), *Name, *Direction, *Type,
                Container == TEXT("none") ? TEXT("") : *FString::Printf(TEXT("[%s]"), *Container)));
        }
        Parts.Sort();
        return FString::Join(Parts, TEXT(","));
    }

    void AddDifference(TArray<TSharedPtr<FJsonValue>>&& OnlyInA, TArray<TSharedPtr<FJsonValue>>&& OnlyInB,
                       TArray<TSharedPtr<FJsonValue>>&& Changed, TSharedPtr<FJsonObject>& OutObj)
    {
        OutObj->SetArrayField(TEXT("only_in_a"), OnlyInA);
        OutObj->SetArrayField(TEXT("only_in_b"), OnlyInB);
        OutObj->SetArrayField(TEXT("changed"), Changed);
    }

    void DiffMaps(const TMap<FString, FString>& A, const TMap<FString, FString>& B,
                  TArray<TSharedPtr<FJsonValue>>& OutOnlyInA, TArray<TSharedPtr<FJsonValue>>& OutOnlyInB,
                  TArray<TSharedPtr<FJsonValue>>& OutChanged)
    {
        for (const TPair<FString, FString>& Pair : A)
        {
            const FString* Other = B.Find(Pair.Key);
            if (!Other)
            {
                OutOnlyInA.Add(MakeShared<FJsonValueString>(Pair.Key));
            }
            else if (*Other != Pair.Value)
            {
                TSharedPtr<FJsonObject> Change = MakeShared<FJsonObject>();
                Change->SetStringField(TEXT("name"), Pair.Key);
                Change->SetStringField(TEXT("value_in_a"), Pair.Value);
                Change->SetStringField(TEXT("value_in_b"), *Other);
                OutChanged.Add(MakeShared<FJsonValueObject>(Change));
            }
        }

        for (const TPair<FString, FString>& Pair : B)
        {
            if (!A.Contains(Pair.Key))
            {
                OutOnlyInB.Add(MakeShared<FJsonValueString>(Pair.Key));
            }
        }
    }

    void CollectVariableSignatures(UBlueprint* Blueprint, TMap<FString, FString>& Out)
    {
        for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
        {
            Out.Add(Variable.VarName.ToString(), MakeVariableSignature(Variable));
        }
    }

    void CollectFunctionSignatures(UBlueprint* Blueprint, TMap<FString, FString>& Out)
    {
        for (UEdGraph* Graph : Blueprint->FunctionGraphs)
        {
            if (Graph)
            {
                Out.Add(Graph->GetName(), MakeFunctionSignature(Graph));
            }
        }
    }

    void CollectComponentSignatures(UBlueprint* Blueprint, TMap<FString, FString>& Out)
    {
        // A blueprint without a usable construction script simply has no components to compare.
        if (!Blueprint->SimpleConstructionScript || Blueprint->SimpleConstructionScript->GetBlueprint() != Blueprint)
        {
            return;
        }
        USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
        for (USCS_Node* Node : SCS->GetAllNodes())
        {
            if (!Node)
            {
                continue;
            }
            USCS_Node* ParentNode = SCS->FindParentNode(Node);
            const FString ParentName = ParentNode ? ParentNode->GetVariableName().ToString() : FString();
            Out.Add(Node->GetVariableName().ToString(),
                FString::Printf(TEXT("%s/%s"),
                    Node->ComponentTemplate ? *Node->ComponentTemplate->GetClass()->GetName() : TEXT("null"),
                    *ParentName));
        }
    }

    void CollectInterfaceSignatures(UBlueprint* Blueprint, TMap<FString, FString>& Out)
    {
        for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
        {
            if (Description.Interface)
            {
                Out.Add(Description.Interface->GetName(), Description.Interface->GetClassPathName().ToString());
            }
        }
    }
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleImplementBlueprintInterface(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintOrNull(Params, TEXT("blueprint_name"), Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString InterfacePath;
    if (!Params->TryGetStringField(TEXT("interface_path"), InterfacePath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("Missing 'interface_path' parameter"));
    }

    TArray<FString> Tried;
    UClass* InterfaceClass = ResolveInterfaceClass(InterfacePath, Tried);
    if (!InterfaceClass)
    {
        TSharedPtr<FJsonObject> ErrorObj = MakeCandidatesError(EUnrealMCPGraphError::InterfaceNotFound,
            FString::Printf(TEXT("No interface matches '%s'"), *InterfacePath), MakeInterfaceNames(Blueprint));
        ErrorObj->SetArrayField(TEXT("tried"), MakeStringArray(Tried));
        return ErrorObj;
    }

    if (const FBPInterfaceDescription* Existing = FindImplementedInterface(Blueprint, InterfaceClass))
    {
        // Implementing twice must not generate a second set of graphs.
        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
        ResultObj->SetStringField(TEXT("interface_path"), InterfaceClass->GetClassPathName().ToString());
        ResultObj->SetBoolField(TEXT("implemented"), true);
        ResultObj->SetBoolField(TEXT("already_implemented"), true);
        ResultObj->SetArrayField(TEXT("generated_graphs"), MakeStringArray(MakeFunctionGraphNames(Blueprint)));
        ResultObj->SetObjectField(TEXT("interface"), MakeInterfaceJson(Blueprint, *Existing));
        return ResultObj;
    }

    TArray<FString> GraphsBefore;
    CollectImplementationGraphNames(Blueprint, GraphsBefore);

    const bool bImplemented = FBlueprintEditorUtils::ImplementNewInterface(Blueprint, InterfaceClass->GetClassPathName());
    if (!bImplemented)
    {
        return MakeCandidatesError(EUnrealMCPGraphError::InvalidValue,
            FString::Printf(TEXT("The engine refused to implement '%s' (a function or graph of the same name "
                "probably already exists)"), *InterfacePath), MakeFunctionGraphNames(Blueprint));
    }

    if (const FBPInterfaceDescription* Description = FindImplementedInterface(Blueprint, InterfaceClass))
    {
        TArray<FString> GeneratedGraphs;
        for (const UEdGraph* Graph : Description->Graphs)
        {
            if (Graph)
            {
                GeneratedGraphs.Add(Graph->GetName());
            }
        }

        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
        ResultObj->SetStringField(TEXT("interface_path"), InterfaceClass->GetClassPathName().ToString());
        ResultObj->SetBoolField(TEXT("implemented"), true);
        ResultObj->SetBoolField(TEXT("already_implemented"), false);
        // Read the graphs the engine generated back, so the caller does not have to diff graph lists.
        ResultObj->SetArrayField(TEXT("generated_graphs"), MakeStringArray(GeneratedGraphs));
        ResultObj->SetObjectField(TEXT("interface"), MakeInterfaceJson(Blueprint, *Description));
        ResultObj->SetArrayField(TEXT("interfaces"), MakeInterfaceJsonArray(Blueprint));
        AppendCompileResult(Blueprint, ResultObj);
        return ResultObj;
    }

    TArray<FString> GraphsAfter;
    CollectImplementationGraphNames(Blueprint, GraphsAfter);
    TArray<FString> Generated;
    for (const FString& Name : GraphsAfter)
    {
        if (!GraphsBefore.Contains(Name))
        {
            Generated.Add(Name);
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetStringField(TEXT("interface_path"), InterfacePath);
    ResultObj->SetBoolField(TEXT("implemented"), true);
    ResultObj->SetBoolField(TEXT("already_implemented"), false);
    ResultObj->SetNumberField(TEXT("generated_graph_count"), 0);
    ResultObj->SetArrayField(TEXT("generated_graphs"), MakeStringArray(Generated));
    TSharedPtr<FJsonObject> Note = MakeShared<FJsonObject>();
    Note->SetStringField(TEXT("note"), TEXT("The interface is registered but no description was found for it"));
    ResultObj->SetObjectField(TEXT("interface"), Note);
    ResultObj->SetArrayField(TEXT("interfaces"), MakeInterfaceJsonArray(Blueprint));
    AppendCompileResult(Blueprint, ResultObj);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleUnimplementBlueprintInterface(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintOrNull(Params, TEXT("blueprint_name"), Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString InterfacePath;
    if (!Params->TryGetStringField(TEXT("interface_path"), InterfacePath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("Missing 'interface_path' parameter"));
    }

    bool bPreserveFunctions = false;
    if (Params->HasField(TEXT("preserve_functions")))
    {
        Params->TryGetBoolField(TEXT("preserve_functions"), bPreserveFunctions);
    }

    TArray<FString> Tried;
    UClass* InterfaceClass = ResolveInterfaceClass(InterfacePath, Tried);
    if (!InterfaceClass || !FindImplementedInterface(Blueprint, InterfaceClass))
    {
        // RemoveInterface() on an interface that is not implemented trips an ensure in the engine,
        // so refuse it here with the implemented interfaces as candidates.
        TSharedPtr<FJsonObject> ErrorObj = MakeCandidatesError(EUnrealMCPGraphError::InterfaceNotFound,
            FString::Printf(TEXT("Blueprint '%s' does not implement '%s'"), *Blueprint->GetName(), *InterfacePath),
            MakeInterfaceNames(Blueprint));
        ErrorObj->SetArrayField(TEXT("tried"), MakeStringArray(Tried));
        return ErrorObj;
    }

    TArray<FString> GraphsBefore;
    CollectImplementationGraphNames(Blueprint, GraphsBefore);

    FBlueprintEditorUtils::RemoveInterface(Blueprint, InterfaceClass->GetClassPathName(), bPreserveFunctions);

    TArray<FString> GraphsAfter;
    CollectImplementationGraphNames(Blueprint, GraphsAfter);
    TArray<FString> RemovedGraphs;
    for (const FString& Name : GraphsBefore)
    {
        if (!GraphsAfter.Contains(Name))
        {
            RemovedGraphs.Add(Name);
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetStringField(TEXT("interface_path"), InterfaceClass->GetClassPathName().ToString());
    ResultObj->SetBoolField(TEXT("removed"), true);
    ResultObj->SetBoolField(TEXT("preserve_functions"), bPreserveFunctions);
    ResultObj->SetArrayField(TEXT("removed_graphs"), MakeStringArray(RemovedGraphs));
    ResultObj->SetArrayField(TEXT("interfaces"), MakeInterfaceJsonArray(Blueprint));
    ResultObj->SetArrayField(TEXT("function_graphs"), MakeStringArray(MakeFunctionGraphNames(Blueprint)));
    AppendCompileResult(Blueprint, ResultObj);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleListBlueprintInterfaces(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintOrNull(Params, TEXT("blueprint_name"), Error);
    if (!Blueprint)
    {
        return Error;
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetArrayField(TEXT("interfaces"), MakeInterfaceJsonArray(Blueprint));
    ResultObj->SetNumberField(TEXT("interface_count"), Blueprint->ImplementedInterfaces.Num());
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleListOverridableFunctions(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintOrNull(Params, TEXT("blueprint_name"), Error);
    if (!Blueprint)
    {
        return Error;
    }

    bool bIncludeParentEvents = true;
    if (Params->HasField(TEXT("include_parent_events")))
    {
        Params->TryGetBoolField(TEXT("include_parent_events"), bIncludeParentEvents);
    }

    const TSet<FName> Implemented = CollectImplementedEventNames(Blueprint);

    TArray<TSharedPtr<FJsonValue>> Functions;
    TSet<FName> Seen;

    auto AddFunction = [&Functions, &Seen, &Implemented](UFunction* Function, const TCHAR* Source)
    {
        if (!Function || Seen.Contains(Function->GetFName()))
        {
            return;
        }
        // Only functions a blueprint can actually override: BlueprintImplementableEvent / -NativeEvent.
        if (!UEdGraphSchema_K2::CanKismetOverrideFunction(Function))
        {
            return;
        }
        Seen.Add(Function->GetFName());

        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("function_name"), Function->GetName());
        Obj->SetStringField(TEXT("declaring_class"),
            Function->GetOwnerClass() ? Function->GetOwnerClass()->GetName() : FString());
        Obj->SetStringField(TEXT("source"), Source);
        Obj->SetBoolField(TEXT("overridden"), Implemented.Contains(Function->GetFName()));
        Obj->SetStringField(TEXT("signature_class"),
            Function->GetOwnerClass() ? Function->GetOwnerClass()->GetPathName() : FString());
        Functions.Add(MakeShared<FJsonValueObject>(Obj));
    };

    if (bIncludeParentEvents && Blueprint->ParentClass)
    {
        // TFieldIterator walks the class itself before its supers, so the most derived declaration wins.
        for (TFieldIterator<UFunction> It(Blueprint->ParentClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
        {
            AddFunction(*It, TEXT("parent"));
        }
    }

    for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
    {
        if (!Description.Interface)
        {
            continue;
        }
        for (TFieldIterator<UFunction> It(Description.Interface); It; ++It)
        {
            AddFunction(*It, TEXT("interface"));
        }
    }

    int32 OverriddenCount = 0;
    for (const TSharedPtr<FJsonValue>& Item : Functions)
    {
        const TSharedPtr<FJsonObject>* FunctionObj = nullptr;
        if (Item.IsValid() && Item->TryGetObject(FunctionObj) && FunctionObj && FunctionObj->IsValid()
            && (*FunctionObj)->GetBoolField(TEXT("overridden")))
        {
            ++OverriddenCount;
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetArrayField(TEXT("functions"), Functions);
    ResultObj->SetNumberField(TEXT("function_count"), Functions.Num());
    ResultObj->SetNumberField(TEXT("overridden_count"), OverriddenCount);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleCheckBlueprintElement(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintOrNull(Params, TEXT("blueprint_name"), Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString Kind;
    FString Name;
    if (!Params->TryGetStringField(TEXT("kind"), Kind) ||
        !Params->TryGetStringField(TEXT("name"), Name))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("Both 'kind' and 'name' are required"));
    }

    const TArray<FString> KnownKinds = {
        TEXT("variable"), TEXT("function"), TEXT("component"), TEXT("graph"), TEXT("node")
    };
    if (!KnownKinds.Contains(Kind))
    {
        TSharedPtr<FJsonObject> ErrorObj = FUnrealMCPCommonUtils::CreateErrorResponse(
            EUnrealMCPGraphError::InvalidParams,
            FString::Printf(TEXT("Unknown element kind '%s'"), *Kind));
        ErrorObj->SetArrayField(TEXT("candidates"), MakeStringArray(KnownKinds));
        return ErrorObj;
    }

    // "Not found" is a normal answer here, not an error code: the caller asked a question, and a
    // missing element is the answer. Error codes stay for bad parameters and missing blueprints.
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetStringField(TEXT("kind"), Kind);
    ResultObj->SetStringField(TEXT("name"), Name);

    if (Kind == TEXT("variable"))
    {
        const FBPVariableDescription* Variable = FindMutableMemberVariable(Blueprint, FName(*Name));
        ResultObj->SetBoolField(TEXT("found"), Variable != nullptr);
        if (Variable)
        {
            ResultObj->SetStringField(TEXT("type"), Variable->VarType.PinCategory.ToString());
            ResultObj->SetStringField(TEXT("container"), MakeContainerName(Variable->VarType));
            ResultObj->SetStringField(TEXT("sub_class"), MakeSubClassString(Variable->VarType));
            ResultObj->SetStringField(TEXT("default_value"), Variable->DefaultValue);
        }
        return ResultObj;
    }

    if (Kind == TEXT("function"))
    {
        TObjectPtr<UEdGraph>* FoundEntry = Blueprint->FunctionGraphs.FindByPredicate(
            [&Name](const UEdGraph* Graph) { return Graph && Graph->GetName() == Name; });
        UEdGraph* Found = FoundEntry ? FoundEntry->Get() : nullptr;
        ResultObj->SetBoolField(TEXT("found"), Found != nullptr);
        if (Found)
        {
            ResultObj->SetStringField(TEXT("graph_name"), Found->GetName());
            ResultObj->SetNumberField(TEXT("node_count"), Found->Nodes.Num());
        }
        return ResultObj;
    }

    if (Kind == TEXT("component"))
    {
        // A blueprint with no construction script has no components: that is a false answer, not an error.
        if (Blueprint->SimpleConstructionScript && Blueprint->SimpleConstructionScript->GetBlueprint() == Blueprint)
        {
            USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
            USCS_Node* const* FoundEntry = SCS->GetAllNodes().FindByPredicate(
                [&Name](const USCS_Node* Node) { return Node && Node->GetVariableName().ToString() == Name; });
            USCS_Node* Found = FoundEntry ? *FoundEntry : nullptr;
            ResultObj->SetBoolField(TEXT("found"), Found != nullptr);
            if (Found)
            {
                ResultObj->SetStringField(TEXT("component_class"),
                    Found->ComponentTemplate ? Found->ComponentTemplate->GetClass()->GetName() : FString());
                ResultObj->SetStringField(TEXT("component_template"),
                    Found->ComponentTemplate ? Found->ComponentTemplate->GetPathName() : FString());
                USCS_Node* ParentNode = SCS->FindParentNode(Found);
                ResultObj->SetStringField(TEXT("parent"),
                    ParentNode ? ParentNode->GetVariableName().ToString() : FString());
                ResultObj->SetBoolField(TEXT("is_root"), ParentNode == nullptr);
            }
        }
        else
        {
            ResultObj->SetBoolField(TEXT("found"), false);
            ResultObj->SetStringField(TEXT("reason"), TEXT("blueprint_not_ready"));
        }
        return ResultObj;
    }

    if (Kind == TEXT("graph"))
    {
        TArray<UEdGraph*> AllGraphs;
        Blueprint->GetAllGraphs(AllGraphs);
        UEdGraph** FoundEntry = AllGraphs.FindByPredicate(
            [&Name](const UEdGraph* Graph) { return Graph && Graph->GetName() == Name; });
        UEdGraph* Found = FoundEntry ? *FoundEntry : nullptr;
        ResultObj->SetBoolField(TEXT("found"), Found != nullptr);
        if (Found)
        {
            ResultObj->SetStringField(TEXT("graph_name"), Found->GetName());
            ResultObj->SetStringField(TEXT("graph_class"), Found->GetClass()->GetName());
            ResultObj->SetNumberField(TEXT("node_count"), Found->Nodes.Num());
        }
        return ResultObj;
    }

    // kind == node: the guid is unique across the blueprint, so no graph_name is needed.
    TArray<UEdGraph*> AllGraphs;
    Blueprint->GetAllGraphs(AllGraphs);
    for (UEdGraph* Graph : AllGraphs)
    {
        if (!Graph)
        {
            continue;
        }
        if (UEdGraphNode* Found = FUnrealMCPBlueprintGraphOps::FindNodeByGuid(Graph, Name))
        {
            ResultObj->SetBoolField(TEXT("found"), true);
            ResultObj->SetStringField(TEXT("graph_name"), Graph->GetName());
            ResultObj->SetStringField(TEXT("node_type"), Found->GetClass()->GetName());
            ResultObj->SetStringField(TEXT("node_title"), Found->GetNodeTitle(ENodeTitleType::ListView).ToString());
            return ResultObj;
        }
    }
    ResultObj->SetBoolField(TEXT("found"), false);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleCompareBlueprints(const TSharedPtr<FJsonObject>& Params)
{
    FString NameA;
    FString NameB;
    if (!Params->TryGetStringField(TEXT("blueprint_a"), NameA) ||
        !Params->TryGetStringField(TEXT("blueprint_b"), NameB))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("Both 'blueprint_a' and 'blueprint_b' are required"));
    }

    UBlueprint* BlueprintA = FUnrealMCPCommonUtils::FindBlueprint(NameA);
    if (!BlueprintA)
    {
        TSharedPtr<FJsonObject> ErrorObj = FUnrealMCPCommonUtils::CreateErrorResponse(
            EUnrealMCPGraphError::BlueprintNotFound, FString::Printf(TEXT("Blueprint not found: %s"), *NameA));
        ErrorObj->SetStringField(TEXT("side"), TEXT("a"));
        return ErrorObj;
    }

    UBlueprint* BlueprintB = FUnrealMCPCommonUtils::FindBlueprint(NameB);
    if (!BlueprintB)
    {
        TSharedPtr<FJsonObject> ErrorObj = FUnrealMCPCommonUtils::CreateErrorResponse(
            EUnrealMCPGraphError::BlueprintNotFound, FString::Printf(TEXT("Blueprint not found: %s"), *NameB));
        ErrorObj->SetStringField(TEXT("side"), TEXT("b"));
        return ErrorObj;
    }

    TMap<FString, FString> VariablesA;
    TMap<FString, FString> VariablesB;
    CollectVariableSignatures(BlueprintA, VariablesA);
    CollectVariableSignatures(BlueprintB, VariablesB);

    TMap<FString, FString> FunctionsA;
    TMap<FString, FString> FunctionsB;
    CollectFunctionSignatures(BlueprintA, FunctionsA);
    CollectFunctionSignatures(BlueprintB, FunctionsB);

    TMap<FString, FString> ComponentsA;
    TMap<FString, FString> ComponentsB;
    CollectComponentSignatures(BlueprintA, ComponentsA);
    CollectComponentSignatures(BlueprintB, ComponentsB);

    TMap<FString, FString> InterfacesA;
    TMap<FString, FString> InterfacesB;
    CollectInterfaceSignatures(BlueprintA, InterfacesA);
    CollectInterfaceSignatures(BlueprintB, InterfacesB);

    TSharedPtr<FJsonObject> VariablesDiff = MakeShared<FJsonObject>();
    {
        TArray<TSharedPtr<FJsonValue>> OnlyInA;
        TArray<TSharedPtr<FJsonValue>> OnlyInB;
        TArray<TSharedPtr<FJsonValue>> Changed;
        DiffMaps(VariablesA, VariablesB, OnlyInA, OnlyInB, Changed);
        AddDifference(MoveTemp(OnlyInA), MoveTemp(OnlyInB), MoveTemp(Changed), VariablesDiff);
    }

    TSharedPtr<FJsonObject> FunctionsDiff = MakeShared<FJsonObject>();
    {
        TArray<TSharedPtr<FJsonValue>> OnlyInA;
        TArray<TSharedPtr<FJsonValue>> OnlyInB;
        TArray<TSharedPtr<FJsonValue>> Changed;
        DiffMaps(FunctionsA, FunctionsB, OnlyInA, OnlyInB, Changed);
        AddDifference(MoveTemp(OnlyInA), MoveTemp(OnlyInB), MoveTemp(Changed), FunctionsDiff);
    }

    TSharedPtr<FJsonObject> ComponentsDiff = MakeShared<FJsonObject>();
    {
        TArray<TSharedPtr<FJsonValue>> OnlyInA;
        TArray<TSharedPtr<FJsonValue>> OnlyInB;
        TArray<TSharedPtr<FJsonValue>> Changed;
        DiffMaps(ComponentsA, ComponentsB, OnlyInA, OnlyInB, Changed);
        AddDifference(MoveTemp(OnlyInA), MoveTemp(OnlyInB), MoveTemp(Changed), ComponentsDiff);
    }

    TSharedPtr<FJsonObject> InterfacesDiff = MakeShared<FJsonObject>();
    {
        TArray<TSharedPtr<FJsonValue>> OnlyInA;
        TArray<TSharedPtr<FJsonValue>> OnlyInB;
        TArray<TSharedPtr<FJsonValue>> Changed;
        DiffMaps(InterfacesA, InterfacesB, OnlyInA, OnlyInB, Changed);
        AddDifference(MoveTemp(OnlyInA), MoveTemp(OnlyInB), MoveTemp(Changed), InterfacesDiff);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetObjectField(TEXT("variables"), VariablesDiff);
    ResultObj->SetObjectField(TEXT("functions"), FunctionsDiff);
    ResultObj->SetObjectField(TEXT("components"), ComponentsDiff);
    ResultObj->SetObjectField(TEXT("interfaces"), InterfacesDiff);

    TSharedPtr<FJsonObject> Sides = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> SideA = MakeShared<FJsonObject>();
    SideA->SetStringField(TEXT("blueprint_name"), BlueprintA->GetName());
    SideA->SetStringField(TEXT("parent_class"), BlueprintA->ParentClass ? BlueprintA->ParentClass->GetName() : FString());
    SideA->SetStringField(TEXT("parent_class_path"),
        BlueprintA->ParentClass ? BlueprintA->ParentClass->GetPathName() : FString());
    TSharedPtr<FJsonObject> SideB = MakeShared<FJsonObject>();
    SideB->SetStringField(TEXT("blueprint_name"), BlueprintB->GetName());
    SideB->SetStringField(TEXT("parent_class"), BlueprintB->ParentClass ? BlueprintB->ParentClass->GetName() : FString());
    SideB->SetStringField(TEXT("parent_class_path"),
        BlueprintB->ParentClass ? BlueprintB->ParentClass->GetPathName() : FString());
    Sides->SetObjectField(TEXT("a"), SideA);
    Sides->SetObjectField(TEXT("b"), SideB);
    ResultObj->SetObjectField(TEXT("sides"), Sides);
    return ResultObj;
}


