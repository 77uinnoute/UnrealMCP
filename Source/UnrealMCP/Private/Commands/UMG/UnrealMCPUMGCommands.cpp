// UMG commands: the registry plus the small commands (create / add text block / add button /
// add to viewport). The larger command families live next to this file.
#include "Commands/UnrealMCPUMGCommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Components/TextBlock.h"
#include "WidgetBlueprint.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/ProgressBar.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/Button.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Editor.h"
#include "Commands/UMG/UnrealMCPUMGCommandHelpers.h"

FUnrealMCPUMGCommands::FUnrealMCPUMGCommands()
{
}

void FUnrealMCPUMGCommands::RegisterCommands(FMCPCommandRegistry& Registry)
{
    MCP_REGISTER_COMMAND(Registry, "create_umg_widget_blueprint", "umg", "Create a Widget Blueprint with a root Canvas Panel, in the requested content folder. parent_class picks what it derives from: a user widget class name (default UserWidget) or the path of an existing widget blueprint to derive from.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("name"), TEXT("string"), TEXT("Asset name, or a full asset path like /Game/UI/WBP_HUD")),
            MCPParamOpt(TEXT("path"), TEXT("string"), TEXT("Content folder for the new asset, e.g. /Game/UI (default /Game/Widgets). Ignored when 'name' is already a full path.")),
            MCPParamOpt(TEXT("parent_class"), TEXT("string"), TEXT("Parent class: a user widget class name (UserWidget / CommonUserWidget / ...) or a widget blueprint asset path to derive from (default UserWidget)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, /*bMutatesGraph=*/true),
        [this](const TSharedPtr<FJsonObject>& Params) { return EnsurePersistReceipt(Params, HandleCreateUMGWidgetBlueprint(Params)); });

    MCP_REGISTER_COMMAND(Registry, "add_text_block_to_widget", "umg", "Add a Text Block to a Widget Blueprint, inside the root panel or a named parent panel.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Widget Blueprint")),
            MCPParam(TEXT("widget_name"), TEXT("string"), TEXT("Name for the new Text Block")),
            MCPParamOpt(TEXT("text"), TEXT("string"), TEXT("Initial text (default 'New Text Block')")),
            MCPParamOpt(TEXT("position"), TEXT("array"), TEXT("[X, Y] canvas position (canvas parents only)")),
            MCPParamOpt(TEXT("parent_widget"), TEXT("string"), TEXT("Panel to add the Text Block to; default = the root panel")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, /*bMutatesGraph=*/true),
        [this](const TSharedPtr<FJsonObject>& Params) { return EnsurePersistReceipt(Params, HandleAddTextBlockToWidget(Params)); });

    MCP_REGISTER_COMMAND(Registry, "add_widget_to_viewport", "umg", "BREAKING (no longer a class-path echo): create the Widget Blueprint in the running PIE world and add it to the viewport. Requires a live PIE session - without one it answers pie_not_running (call start_pie first); with a session but no player controller yet it answers pie_not_ready. The created instance lives in the PIE world only and never touches the asset.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name or path of the target Widget Blueprint")),
            MCPParamOpt(TEXT("z_order"), TEXT("int"), TEXT("Z-order for the new widget (default 0)")),
        }), MCPFlags(),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleAddWidgetToViewport(Params); });

    MCP_REGISTER_COMMAND(Registry, "add_button_to_widget", "umg", "Add a Button with a text child to a Widget Blueprint, inside the root panel or a named parent panel.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Widget Blueprint")),
            MCPParam(TEXT("widget_name"), TEXT("string"), TEXT("Name for the new Button")),
            MCPParam(TEXT("text"), TEXT("string"), TEXT("Button text")),
            MCPParamOpt(TEXT("position"), TEXT("array"), TEXT("[X, Y] canvas position (canvas parents only)")),
            MCPParamOpt(TEXT("parent_widget"), TEXT("string"), TEXT("Panel to add the Button to; default = the root panel")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, /*bMutatesGraph=*/true),
        [this](const TSharedPtr<FJsonObject>& Params) { return EnsurePersistReceipt(Params, HandleAddButtonToWidget(Params)); });

    MCP_REGISTER_COMMAND(Registry, "bind_widget_event", "umg", "Create the bound event node for a widget's multicast delegate event.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Widget Blueprint")),
            MCPParam(TEXT("widget_name"), TEXT("string"), TEXT("Widget whose event is bound")),
            MCPParam(TEXT("event_name"), TEXT("string"), TEXT("Delegate name on the widget class (e.g. OnClicked)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, /*bMutatesGraph=*/true),
        [this](const TSharedPtr<FJsonObject>& Params) { return EnsurePersistReceipt(Params, HandleBindWidgetEvent(Params)); });

    MCP_REGISTER_COMMAND(Registry, "set_widget_property_binding", "umg", "Bind any bindable widget property (one that has a <Property>Delegate) to a Get<binding_name> function: creates the backing variable with the type the delegate returns, the pure getter, exposes the widget as a variable and registers the UMG property binding. Fails with binding_not_effective when the engine drops the binding at compile time.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Widget Blueprint")),
            MCPParam(TEXT("widget_name"), TEXT("string"), TEXT("Widget to bind")),
            MCPParam(TEXT("property_name"), TEXT("string"), TEXT("Widget property to bind (Text / Visibility / Percent / ColorAndOpacity / ...)")),
            MCPParam(TEXT("binding_name"), TEXT("string"), TEXT("Variable backing the binding; the getter is Get<binding_name>")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, /*bMutatesGraph=*/true),
        [this](const TSharedPtr<FJsonObject>& Params) { return EnsurePersistReceipt(Params, HandleSetWidgetPropertyBinding(Params)); });

    MCP_REGISTER_COMMAND(Registry, "unbind_widget_property", "umg", "Remove a property binding. By default only the binding entry goes; ask for remove_function / remove_variable explicitly. Refuses (blocked_by_references) while graph nodes, other bindings or events still refer to what would be deleted.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Widget Blueprint")),
            MCPParam(TEXT("widget_name"), TEXT("string"), TEXT("Widget whose binding is removed")),
            MCPParam(TEXT("property_name"), TEXT("string"), TEXT("Bound property name")),
            MCPParamOpt(TEXT("remove_function"), TEXT("bool"), TEXT("Also delete the getter function graph when nothing else uses it (default false)")),
            MCPParamOpt(TEXT("remove_variable"), TEXT("bool"), TEXT("Also delete the backing variable when nothing else uses it (default false)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, /*bMutatesGraph=*/true),
        [this](const TSharedPtr<FJsonObject>& Params) { return EnsurePersistReceipt(Params, HandleUnbindWidgetProperty(Params)); });

    MCP_REGISTER_COMMAND(Registry, "prune_widget_bindings", "umg", "Report the leftovers of the widget bindings: stale_binding (widget no longer in the tree), orphan_function (nobody calls the getter), unused_variable (no binding, no getter, no graph reference). dry_run defaults to true; dry_run=false applies exactly the same rules.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Widget Blueprint")),
            MCPParamOpt(TEXT("dry_run"), TEXT("bool"), TEXT("Report only, change nothing (default true)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, /*bMutatesGraph=*/true),
        [this](const TSharedPtr<FJsonObject>& Params) { return EnsurePersistReceipt(Params, HandlePruneWidgetBindings(Params)); });

    MCP_REGISTER_COMMAND(Registry, "set_text_block_binding", "umg", "DEPRECATED alias of set_widget_property_binding (kept one version, response carries deprecated_command). Binds a Text Block property to a Get<binding_name> function: creates the backing variable, the getter, exposes the widget as a variable and registers the UMG property binding.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Widget Blueprint")),
            MCPParam(TEXT("widget_name"), TEXT("string"), TEXT("Text Block to bind")),
            MCPParam(TEXT("binding_name"), TEXT("string"), TEXT("Variable backing the binding (Get<binding_name> function)")),
            MCPParamOpt(TEXT("property_name"), TEXT("string"), TEXT("Widget property to bind; default Text. Must have a <Property>Delegate with a supported return type.")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, /*bMutatesGraph=*/true),
        [this](const TSharedPtr<FJsonObject>& Params) { return EnsurePersistReceipt(Params, HandleSetTextBlockBinding(Params)); });

    MCP_REGISTER_COMMAND(Registry, "set_widget_slot", "umg", "Write one widget's slot from a 'slot' object whose keys mirror what get_widget_tree reads back: canvas slots take anchors/alignment/position/size/auto_size/z_order; box slots (vertical/horizontal/scroll) take padding/horizontal_alignment/vertical_alignment/size_rule; overlay and size box slots take padding/alignment; grid slots take padding/alignment/row/column/row_span/column_span/layer/nudge. Widget properties were removed from this command - use set_widget_properties.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Widget Blueprint")),
            MCPParam(TEXT("widget_name"), TEXT("string"), TEXT("Widget whose slot is written")),
            MCPParamOpt(TEXT("slot"), TEXT("object"), TEXT("Slot values keyed like get_widget_tree's slot object; keys not supported by this widget's slot class are rejected with the field list")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, /*bMutatesGraph=*/true),
        [this](const TSharedPtr<FJsonObject>& Params) { return EnsurePersistReceipt(Params, HandleSetWidgetSlot(Params)); });

    MCP_REGISTER_COMMAND(Registry, "get_widget_tree", "umg", "Read a Widget Blueprint back: widget hierarchy + child order, slot values, per-type properties, property bindings and bound widget events.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name or path of the target Widget Blueprint")),
        }), MCPFlags(),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleGetWidgetTree(Params); });

    MCP_REGISTER_COMMAND(Registry, "add_widget", "umg", "Create any widget (python class name, e.g. VerticalBox / ProgressBar / Image) inside a Widget Blueprint and add it to a parent panel, optionally writing its slot in the same call. GridPanel children without slot row/column come back with warnings[grid_slot_unplaced] (AddChild does not lay grid children out).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Widget Blueprint")),
            MCPParam(TEXT("widget_class"), TEXT("string"), TEXT("Widget class: python class name (ProgressBar) or C++ name (UProgressBar)")),
            MCPParam(TEXT("widget_name"), TEXT("string"), TEXT("Name for the new widget; unique inside the widget tree")),
            MCPParamOpt(TEXT("parent_widget"), TEXT("string"), TEXT("Panel to add the widget to; default = the root widget (which becomes the root when the tree has none)")),
            MCPParamOpt(TEXT("as_variable"), TEXT("bool"), TEXT("Expose the widget as a blueprint variable (default false, like the designer)")),
            MCPParamOpt(TEXT("slot"), TEXT("object"), TEXT("Slot values for the parent's slot class, same keys as set_widget_slot; an unsupported key rejects the call and the widget is not created")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, /*bMutatesGraph=*/true),
        [this](const TSharedPtr<FJsonObject>& Params) { return EnsurePersistReceipt(Params, HandleAddWidget(Params)); });

    MCP_REGISTER_COMMAND(Registry, "set_widget_properties", "umg", "Write widget properties by key: common ones (visibility, is_enabled, tooltip, render_opacity, render_transform, render_transform_pivot, clipping, navigation, navigation_all) plus per-type ones (TextBlock text/auto_wrap/wrap_text_at/font_size/color/justification, Button background_color, Image brush/tint, ProgressBar percent/fill_color, Slider value, CheckBox checked_state, EditableText text). Unknown keys are reported, never ignored.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Widget Blueprint")),
            MCPParam(TEXT("widget_name"), TEXT("string"), TEXT("Widget to write")),
            MCPParam(TEXT("props"), TEXT("object"), TEXT("{property_key: value}; each key is written independently and reported in applied[]/failed[]")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, /*bMutatesGraph=*/true),
        [this](const TSharedPtr<FJsonObject>& Params) { return EnsurePersistReceipt(Params, HandleSetWidgetProperties(Params)); });

    MCP_REGISTER_COMMAND(Registry, "remove_widget", "umg", "Remove a widget (and its children) from a Widget Blueprint, together with the property bindings, the widget variable and the Get<widget> getter it owned. Refuses when graph nodes still reference it.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Widget Blueprint")),
            MCPParam(TEXT("widget_name"), TEXT("string"), TEXT("Widget to remove")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, /*bMutatesGraph=*/true),
        [this](const TSharedPtr<FJsonObject>& Params) { return EnsurePersistReceipt(Params, HandleRemoveWidget(Params)); });

    MCP_REGISTER_COMMAND(Registry, "reparent_widget", "umg", "Move a widget to another panel (changes its slot class), carrying over the slot fields both slot types share.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Widget Blueprint")),
            MCPParam(TEXT("widget_name"), TEXT("string"), TEXT("Widget to move")),
            MCPParam(TEXT("new_parent"), TEXT("string"), TEXT("Panel widget the widget should be added to")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, /*bMutatesGraph=*/true),
        [this](const TSharedPtr<FJsonObject>& Params) { return EnsurePersistReceipt(Params, HandleReparentWidget(Params)); });

    MCP_REGISTER_COMMAND(Registry, "reorder_widget", "umg", "Reorder one panel's children (draw order) by moving a widget to an index.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Widget Blueprint")),
            MCPParam(TEXT("widget_name"), TEXT("string"), TEXT("Widget to move inside its parent panel")),
            MCPParam(TEXT("index"), TEXT("int"), TEXT("Target child index (0 = first/drawn first)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, /*bMutatesGraph=*/true),
        [this](const TSharedPtr<FJsonObject>& Params) { return EnsurePersistReceipt(Params, HandleReorderWidget(Params)); });

    MCP_REGISTER_COMMAND(Registry, "rename_widget", "umg", "Rename a widget and keep its variable, its property bindings, its navigation bindings and the graph nodes that refer to it in sync.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Widget Blueprint")),
            MCPParam(TEXT("widget_name"), TEXT("string"), TEXT("Current widget name")),
            MCPParam(TEXT("new_name"), TEXT("string"), TEXT("New widget name (must be free inside the widget tree)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, /*bMutatesGraph=*/true),
        [this](const TSharedPtr<FJsonObject>& Params) { return EnsurePersistReceipt(Params, HandleRenameWidget(Params)); });

    MCP_REGISTER_COMMAND(Registry, "set_root_widget", "umg", "Make an existing panel the widget tree's root, keeping the previous root as its child.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the target Widget Blueprint")),
            MCPParam(TEXT("widget_name"), TEXT("string"), TEXT("Widget to become the root (must be a panel when the tree already has a root)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, /*bMutatesGraph=*/true),
        [this](const TSharedPtr<FJsonObject>& Params) { return EnsurePersistReceipt(Params, HandleSetRootWidget(Params)); });

    MCP_REGISTER_COMMAND(Registry, "get_umg_compile_errors", "umg", "Report the widget compiler's verdict: status, the messages it produced (severity / text / offending node) split against the last MCP mutation boundary, and the editor property bindings the compile did not carry into the runtime table (bindings_dropped). The command compiles the asset to capture messages, and only reports the dropped bindings - it never repairs them.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name or path of the target Widget Blueprint")),
            MCPParamOpt(TEXT("since"), TEXT("string"), TEXT("Boundary for the this-compile/historical split: ISO-8601 timestamp or epoch seconds. Default = the last MCP write to this asset.")),
        }), MCPFlags(),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleGetUMGCompileErrors(Params); });

    MCP_REGISTER_COMMAND(Registry, "compile_umg_widget", "umg", "Compile a Widget Blueprint on demand and report compiled / status / num_errors / num_warnings / messages. force_full (default true) marks the blueprint structurally modified first - that is the path which also re-runs the editor-to-runtime property binding copy and its validity checks.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name or path of the target Widget Blueprint")),
            MCPParamOpt(TEXT("force_full"), TEXT("bool"), TEXT("Mark the blueprint structurally modified before compiling (default true)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, /*bMutatesGraph=*/true),
        [this](const TSharedPtr<FJsonObject>& Params) { return EnsurePersistReceipt(Params, HandleCompileUMGWidget(Params)); });

    MCP_REGISTER_COMMAND(Registry, "open_umg_designer", "umg", "Open a Widget Blueprint in the UMG designer, optionally selecting a widget. The engine's per-asset tab location record is put back exactly as it was, so an MCP-driven open does not change where the user sees the asset next time.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name or path of the target Widget Blueprint")),
            MCPParamOpt(TEXT("focus_widget"), TEXT("string"), TEXT("Widget to select in the designer once it is open")),
        }), MCPFlags(),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleOpenUMGDesigner(Params); });

    MCP_REGISTER_COMMAND(Registry, "get_pie_widget_tree", "umg", "Read-only: find a live instance of a Widget Blueprint in the running PIE world and read its RUNTIME widget tree - per widget visibility / effective visibility / render opacity & transform / desired size / cached geometry (last paint) / runtime slot values, TextBlock current text & font, Image current brush & image size, Button hover/press. Also returns instance_path, the object path python can load_object. Answers pie_not_running without PIE and widget_instance_not_found (with the UserWidget classes present) when the blueprint has no instance.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name or path of the Widget Blueprint whose instance is read")),
            MCPParamOpt(TEXT("instance_index"), TEXT("int"), TEXT("Which instance when there are several (creation order, default 0)")),
            MCPParamOpt(TEXT("root_widget"), TEXT("string"), TEXT("Only return the subtree under this widget")),
            MCPParamOpt(TEXT("max_depth"), TEXT("int"), TEXT("Limit the recursion depth (default unlimited)")),
        }), MCPFlags(),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleGetPIEWidgetTree(Params); });

    MCP_REGISTER_COMMAND(Registry, "set_umg_design_size", "umg", "Set the design-time preview size of a Widget Blueprint (width / height). This is designer preview only and never changes runtime behaviour. dpi_scale and preview_platform have no per-asset field in UMG (the DPI readout is derived from the project's UI scale curve; the preview platform is the designer view's own device profile), so asking for either is refused with 'unsupported' instead of half-applying the call.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name or path of the target Widget Blueprint")),
            MCPParamOpt(TEXT("width"), TEXT("number"), TEXT("Design preview width (switches the widget to the custom size mode)")),
            MCPParamOpt(TEXT("height"), TEXT("number"), TEXT("Design preview height")),
            MCPParamOpt(TEXT("dpi_scale"), TEXT("number"), TEXT("NOT supported: the designer derives DPI from the project's UI scale curve")),
            MCPParamOpt(TEXT("preview_platform"), TEXT("string"), TEXT("NOT supported: the preview platform is the designer view's own device profile")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, /*bMutatesGraph=*/true),
        [this](const TSharedPtr<FJsonObject>& Params) { return EnsurePersistReceipt(Params, HandleSetUMGDesignSize(Params)); });

    MCP_REGISTER_COMMAND(Registry, "create_widget_animation", "umg", "Create a widget animation (UWidgetAnimation with its own movie scene) on a Widget Blueprint and make it visible to the designer and the runtime.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name or path of the target Widget Blueprint")),
            MCPParam(TEXT("animation_name"), TEXT("string"), TEXT("Name for the new animation")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true.")),
        }), MCPFlags(false, /*bMutatesGraph=*/true),
        [this](const TSharedPtr<FJsonObject>& Params) { return EnsurePersistReceipt(Params, HandleCreateWidgetAnimation(Params)); });

    MCP_REGISTER_COMMAND(Registry, "add_widget_animation_track", "umg", "Add the animation track that drives one widget property: RenderOpacity (float), Visibility (enum) or RenderTransform (2D transform). The possessable and its binding entry are written by the engine's own writer, so the runtime can resolve the widget.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name or path of the target Widget Blueprint")),
            MCPParam(TEXT("animation_name"), TEXT("string"), TEXT("Animation to add the track to")),
            MCPParam(TEXT("widget_name"), TEXT("string"), TEXT("Widget the track animates")),
            MCPParam(TEXT("property"), TEXT("string"), TEXT("RenderOpacity / Visibility / RenderTransform")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true.")),
        }), MCPFlags(false, /*bMutatesGraph=*/true),
        [this](const TSharedPtr<FJsonObject>& Params) { return EnsurePersistReceipt(Params, HandleAddWidgetAnimationTrack(Params)); });

    MCP_REGISTER_COMMAND(Registry, "set_widget_animation_keyframes", "umg", "Write the keyframes of one animation track, replacing what was there. keys = [{time (seconds), value}]; the value shape follows the property (number for RenderOpacity, a visibility name or number for Visibility, {translation/scale/shear/angle} for RenderTransform). A key past the end of the playback range extends it (reported as playback_range_extended).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name or path of the target Widget Blueprint")),
            MCPParam(TEXT("animation_name"), TEXT("string"), TEXT("Animation that owns the track")),
            MCPParam(TEXT("widget_name"), TEXT("string"), TEXT("Widget the track animates")),
            MCPParam(TEXT("property"), TEXT("string"), TEXT("RenderOpacity / Visibility / RenderTransform")),
            MCPParam(TEXT("keys"), TEXT("array"), TEXT("[{time, value}] - the whole key list, it replaces the existing keys")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true.")),
        }), MCPFlags(false, /*bMutatesGraph=*/true),
        [this](const TSharedPtr<FJsonObject>& Params) { return EnsurePersistReceipt(Params, HandleSetWidgetAnimationKeyframes(Params)); });

    MCP_REGISTER_COMMAND(Registry, "list_widget_animations", "umg", "Read every animation back: its tracks (widget + property), the keyframes of each track, and whether the animation reached the generated class (the copy the runtime actually plays).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name or path of the target Widget Blueprint")),
        }), MCPFlags(),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleListWidgetAnimations(Params); });

    MCP_REGISTER_COMMAND(Registry, "play_widget_animation_preview", "umg", "Play an animation in the designer preview (the preview holds its own copies of the animations, so the asset object itself cannot be played there). Requires an open designer session; the preview stops mattering when PIE starts.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name or path of the target Widget Blueprint")),
            MCPParam(TEXT("animation_name"), TEXT("string"), TEXT("Animation to play")),
            MCPParamOpt(TEXT("start_at_time"), TEXT("number"), TEXT("Start time in seconds (default 0)")),
            MCPParamOpt(TEXT("loops"), TEXT("int"), TEXT("Number of loops (default 1)")),
            MCPParamOpt(TEXT("speed"), TEXT("number"), TEXT("Playback speed (default 1)")),
            MCPParamOpt(TEXT("play_mode"), TEXT("string"), TEXT("forward / reverse / pingpong (default forward)")),
        }), MCPFlags(),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandlePlayWidgetAnimationPreview(Params); });

    MCP_REGISTER_COMMAND(Registry, "stop_widget_animation_preview", "umg", "Stop one animation (or every animation) in the designer preview. Nothing here touches the asset.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name or path of the target Widget Blueprint")),
            MCPParamOpt(TEXT("animation_name"), TEXT("string"), TEXT("Animation to stop; omit to stop all of them")),
        }), MCPFlags(),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleStopWidgetAnimationPreview(Params); });

    MCP_REGISTER_COMMAND(Registry, "remove_widget_animation", "umg", "Delete a widget animation (its movie scene goes with it). Refuses with blocked_by_references when a graph node still holds the animation, naming the nodes instead of leaving them dangling.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name or path of the target Widget Blueprint")),
            MCPParam(TEXT("animation_name"), TEXT("string"), TEXT("Animation to delete")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true.")),
        }), MCPFlags(false, /*bMutatesGraph=*/true),
        [this](const TSharedPtr<FJsonObject>& Params) { return EnsurePersistReceipt(Params, HandleRemoveWidgetAnimation(Params)); });

    MCP_REGISTER_COMMAND(Registry, "rename_widget_animation", "umg", "Rename a widget animation (object name + display label) and recompile.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name or path of the target Widget Blueprint")),
            MCPParam(TEXT("animation_name"), TEXT("string"), TEXT("Animation to rename")),
            MCPParam(TEXT("new_name"), TEXT("string"), TEXT("New animation name")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true.")),
        }), MCPFlags(false, /*bMutatesGraph=*/true),
        [this](const TSharedPtr<FJsonObject>& Params) { return EnsurePersistReceipt(Params, HandleRenameWidgetAnimation(Params)); });

    MCP_REGISTER_COMMAND(Registry, "set_widget_animation_playback_range", "umg", "Set an animation's playback range (that range is what 'the animation's length' means). Keys outside the new range are kept and counted in keys_beyond_range - they simply never play.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name or path of the target Widget Blueprint")),
            MCPParam(TEXT("animation_name"), TEXT("string"), TEXT("Animation to retime")),
            MCPParamOpt(TEXT("length"), TEXT("number"), TEXT("Length in seconds, measured from start_time (default: the current start)")),
            MCPParamOpt(TEXT("end_time"), TEXT("number"), TEXT("Absolute end in seconds (takes precedence over length)")),
            MCPParamOpt(TEXT("start_time"), TEXT("number"), TEXT("Absolute start in seconds (default: keep the current start)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true.")),
        }), MCPFlags(false, /*bMutatesGraph=*/true),
        [this](const TSharedPtr<FJsonObject>& Params) { return EnsurePersistReceipt(Params, HandleSetWidgetAnimationPlaybackRange(Params)); });

    MCP_REGISTER_COMMAND(Registry, "set_widget_variable", "umg", "Expose a widget as a blueprint variable, or stop exposing it. The generated class property only appears or disappears on a compile, so the command compiles and verifies it (variable_not_effective otherwise) instead of trusting the flag.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name or path of the target Widget Blueprint")),
            MCPParam(TEXT("widget_name"), TEXT("string"), TEXT("Widget to toggle")),
            MCPParam(TEXT("as_variable"), TEXT("bool"), TEXT("true = expose as a variable, false = stop exposing it")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true.")),
        }), MCPFlags(false, /*bMutatesGraph=*/true),
        [this](const TSharedPtr<FJsonObject>& Params) { return EnsurePersistReceipt(Params, HandleSetWidgetVariable(Params)); });
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleCreateUMGWidgetBlueprint(const TSharedPtr<FJsonObject>& Params)
{
	// Get required parameters
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("name"), BlueprintName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'name' parameter"));
	}

	// Target folder comes from the caller; the name may also arrive as a full asset path.
	// (Historically both were hard-wired to /Game/Widgets, which silently broke the save of
	// any widget living elsewhere - see Docs/MCP_Findings_2026-09-29_umg-read-and-binding.md.)
	FString RequestedPath;
	Params->TryGetStringField(TEXT("path"), RequestedPath);
	RequestedPath.TrimStartAndEndInline();
	while (RequestedPath.EndsWith(TEXT("/")))
	{
		RequestedPath.LeftChopInline(1);
	}

	FString AssetName = BlueprintName;
	FString PackagePath = RequestedPath;
	if (AssetName.StartsWith(TEXT("/")))
	{
		int32 LastSlash = INDEX_NONE;
		if (AssetName.FindLastChar(TEXT('/'), LastSlash) && LastSlash > 0)
		{
			PackagePath = AssetName.Left(LastSlash);
			AssetName = AssetName.Mid(LastSlash + 1);
		}
	}
	if (PackagePath.IsEmpty())
	{
		PackagePath = TEXT("/Game/Widgets");
	}
	const FString FullPath = PackagePath + TEXT("/") + AssetName;

	// Check if asset already exists
	if (UEditorAssetLibrary::DoesAssetExist(FullPath))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("asset_exists"), FString::Printf(TEXT("Widget Blueprint '%s' already exists"), *BlueprintName));
	}

	// Create package
	UPackage* Package = CreatePackage(*FullPath);
	if (!Package)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("create_failed"), TEXT("Failed to create package"));
	}

	// Optional parent class: a native user widget class name, or the asset path of a widget blueprint
	// to derive from (the common "my own base widget" case). Default stays UUserWidget.
	UClass* ParentClass = UUserWidget::StaticClass();
	FString ParentClassLabel = TEXT("UserWidget");
	FString RequestedParent;
	if (Params->TryGetStringField(TEXT("parent_class"), RequestedParent) && !RequestedParent.TrimStartAndEnd().IsEmpty())
	{
		TArray<FString> ParentCandidates;
		UClass* Resolved = ResolveWidgetClassByName(RequestedParent, UUserWidget::StaticClass(), ParentCandidates);
		if (!Resolved)
		{
			TSharedPtr<FJsonObject> Failure = MakeListError(TEXT("invalid_parent_class"), FString::Printf(
				TEXT("'%s' is not a user widget class (a native class name, or the path of a widget blueprint)"),
				*RequestedParent), TEXT("parent_classes"), ParentCandidates);
			// The native candidates are thin here by nature (UUserWidget itself is abstract and every
			// concrete user widget in a project is a blueprint), so point at the thing that works.
			Failure->SetStringField(TEXT("hint"),
				TEXT("Most parents are widget blueprints: pass their asset path (e.g. /Game/UI/WBP_Base)"));
			return Failure;
		}
		// Abstract parents are normal (UUserWidget itself is abstract). What matters is whether the
		// engine would let a blueprint derive from it at all - the same check the editor factory runs.
		if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(Resolved))
		{
			return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_parent_class"), FString::Printf(
				TEXT("'%s' cannot be a blueprint parent (not blueprintable, deprecated or replaced)"), *Resolved->GetName()));
		}
		ParentClass = Resolved;
		ParentClassLabel = Resolved->GetName();
	}

	// Create Widget Blueprint using KismetEditorUtilities.
	// Must use UWidgetBlueprint / UWidgetBlueprintGeneratedClass as the blueprint
	// classes, otherwise a plain UBlueprint is created and the cast below fails.
	UBlueprint* NewBlueprint = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,                         // Parent class
		Package,                             // Outer package
		FName(*AssetName),                   // Blueprint name
		BPTYPE_Normal,                       // Blueprint type
		UWidgetBlueprint::StaticClass(),     // Blueprint class
		UWidgetBlueprintGeneratedClass::StaticClass(), // Generated class
		FName("CreateUMGWidget")             // Creation method name
	);

	// Make sure the Blueprint was created successfully
	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(NewBlueprint);
	if (!WidgetBlueprint)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("create_failed"), TEXT("Failed to create Widget Blueprint"));
	}

	// Add a default Canvas Panel if one doesn't exist
	if (!WidgetBlueprint->WidgetTree->RootWidget)
	{
		UCanvasPanel* RootCanvas = WidgetBlueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass());
		WidgetBlueprint->WidgetTree->RootWidget = RootCanvas;
	}

	// Mark the package dirty and notify asset registry
	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(WidgetBlueprint);

	// Compile the blueprint
	FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);

	// Create success response
	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("name"), BlueprintName);
	ResultObj->SetStringField(TEXT("path"), FullPath);
	ResultObj->SetStringField(TEXT("parent_class"), ParentClassLabel);
	FinishWidgetWrite(WidgetBlueprint, Params, ResultObj);
	return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleAddTextBlockToWidget(const TSharedPtr<FJsonObject>& Params)
{
	// Get required parameters
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'blueprint_name' parameter"));
	}

	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'widget_name' parameter"));
	}

	// Find the Widget Blueprint (in-memory lookup, supports not-yet-saved assets)
	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(FUnrealMCPCommonUtils::FindAsset(BlueprintName));
	if (!WidgetBlueprint)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("widget_blueprint_not_found"), FString::Printf(TEXT("Widget Blueprint '%s' not found"), *BlueprintName));
	}

	// Get optional parameters
	FString InitialText = TEXT("New Text Block");
	Params->TryGetStringField(TEXT("text"), InitialText);

	FVector2D Position(0.0f, 0.0f);
	if (Params->HasField(TEXT("position")))
	{
		const TArray<TSharedPtr<FJsonValue>>* PosArray;
		if (Params->TryGetArrayField(TEXT("position"), PosArray) && PosArray->Num() >= 2)
		{
			Position.X = (*PosArray)[0]->AsNumber();
			Position.Y = (*PosArray)[1]->AsNumber();
		}
	}

	// Resolve the parent panel before creating anything, so a bad target leaves no half-built widget.
	FString ParentName;
	TSharedPtr<FJsonObject> ParentError;
	UPanelWidget* Parent = ResolveParentPanel(WidgetBlueprint, Params, ParentName, ParentError);
	if (ParentError.IsValid())
	{
		return ParentError;
	}
	if (Parent && !Cast<UCanvasPanel>(Parent) && Params->HasField(TEXT("position")))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unsupported_parent"), FString::Printf(
			TEXT("Panel '%s' is a %s, not a Canvas Panel, so 'position' cannot be applied; use set_widget_slot"),
			*ParentName, *Parent->GetClass()->GetName()));
	}

	// Create Text Block widget
	UTextBlock* TextBlock = WidgetBlueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), *WidgetName);
	if (!TextBlock)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("create_failed"), TEXT("Failed to create Text Block widget"));
	}

	// Set initial text
	TextBlock->SetText(FText::FromString(InitialText));

	if (!Parent)
	{
		// The tree has no root yet, so this is it - the same thing the designer's first drop does.
		WidgetBlueprint->WidgetTree->RootWidget = TextBlock;
		ParentName = FString();
	}
	else
	{
		UPanelSlot* NewSlot = Parent->AddChild(TextBlock);
		if (!NewSlot)
		{
			// A single-child panel that is already full refuses the child: drop the half-built widget instead of
			// leaving an orphan the next compile would have to clean up.
			TextBlock->Rename(nullptr, GetTransientPackage());
			return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unsupported_parent"), FString::Printf(
				TEXT("Panel '%s' (%s) refused the child; it holds a limited number of children"),
				*ParentName, *Parent->GetClass()->GetName()));
		}
		if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(NewSlot))
		{
			CanvasSlot->SetPosition(Position);
		}
	}

	// Mark the package dirty and compile
	WidgetBlueprint->MarkPackageDirty();
	FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);

	// Create success response
	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("widget_name"), WidgetName);
	ResultObj->SetStringField(TEXT("text"), InitialText);
	ResultObj->SetStringField(TEXT("parent_widget"), ParentName);
	ResultObj->SetBoolField(TEXT("became_root"), ParentName.IsEmpty());
	if (TSharedPtr<FJsonObject> SlotJson = SlotToJson(TextBlock->Slot))
	{
		ResultObj->SetObjectField(TEXT("slot"), SlotJson);
	}
	FinishWidgetWrite(WidgetBlueprint, Params, ResultObj);
	return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleAddWidgetToViewport(const TSharedPtr<FJsonObject>& Params)
{
	// Get required parameters
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'blueprint_name' parameter"));
	}

	// Find the Widget Blueprint (in-memory lookup, supports not-yet-saved assets)
	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(FUnrealMCPCommonUtils::FindAsset(BlueprintName));
	if (!WidgetBlueprint)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("widget_blueprint_not_found"), FString::Printf(TEXT("Widget Blueprint '%s' not found"), *BlueprintName));
	}

	// Get optional Z-order parameter
	int32 ZOrder = 0;
	Params->TryGetNumberField(TEXT("z_order"), ZOrder);

	UClass* WidgetClass = WidgetBlueprint->GeneratedClass;
	if (!WidgetClass)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("widget_class_unavailable"), TEXT("Failed to get widget class"));
	}

	// "Put this widget on screen" only means something in a running game world: the viewport belongs
	// to the PIE session, not to the editor. Refusing here beats the old behaviour of reporting a
	// class path and letting the caller believe the widget was shown.
	UWorld* PlayWorld = GEditor ? GEditor->PlayWorld : nullptr;
	if (!PlayWorld)
	{
		TSharedPtr<FJsonObject> Result = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("pie_not_running"),
			TEXT("No PIE session is running; the viewport only exists in a game world"));
		Result->SetStringField(TEXT("hint"), TEXT("Call start_pie first, then retry (the widget appears in the PIE viewport)"));
		return Result;
	}

	APlayerController* PlayerController = PlayWorld->GetFirstPlayerController();
	if (!PlayerController)
	{
		TSharedPtr<FJsonObject> Result = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("pie_not_ready"),
			TEXT("The PIE world has no player controller yet"));
		Result->SetStringField(TEXT("hint"), TEXT("PIE is still starting; retry in a moment"));
		return Result;
	}

	UUserWidget* Widget = UWidgetBlueprintLibrary::Create(PlayWorld, WidgetClass, PlayerController);
	if (!Widget)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("create_failed"),
			FString::Printf(TEXT("Failed to create a widget instance of '%s'"), *WidgetClass->GetName()));
	}
	Widget->AddToViewport(ZOrder);

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("blueprint_name"), BlueprintName);
	ResultObj->SetStringField(TEXT("class_path"), WidgetClass->GetPathName());
	ResultObj->SetStringField(TEXT("created_widget_name"), Widget->GetName());
	ResultObj->SetNumberField(TEXT("z_order"), ZOrder);
	ResultObj->SetStringField(TEXT("pie_world"), PlayWorld->GetName());
	ResultObj->SetStringField(TEXT("owning_player"), PlayerController->GetName());
	ResultObj->SetBoolField(TEXT("in_viewport"), Widget->IsInViewport());
	ResultObj->SetStringField(TEXT("scope"),
		TEXT("this instance lives in the PIE world only: it is gone when PIE stops and never touches the asset"));
	return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleAddButtonToWidget(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();

	// Get required parameters
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing blueprint_name parameter"));
	}

	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing widget_name parameter"));
	}

	FString ButtonText;
	if (!Params->TryGetStringField(TEXT("text"), ButtonText))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing text parameter"));
	}

	// Load the Widget Blueprint (in-memory lookup, supports not-yet-saved assets)
	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(FUnrealMCPCommonUtils::FindAsset(BlueprintName));
	if (!WidgetBlueprint)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("widget_blueprint_not_found"), FString::Printf(TEXT("Failed to load Widget Blueprint: %s"), *BlueprintName));
	}

	// Resolve the parent panel before creating anything, so a bad target leaves no half-built widget.
	FVector2D ButtonPosition(0.0f, 0.0f);
	{
		const TArray<TSharedPtr<FJsonValue>>* Position;
		if (Params->TryGetArrayField(TEXT("position"), Position) && Position->Num() >= 2)
		{
			ButtonPosition = FVector2D((*Position)[0]->AsNumber(), (*Position)[1]->AsNumber());
		}
	}
	FString ParentName;
	TSharedPtr<FJsonObject> ParentError;
	UPanelWidget* Parent = ResolveParentPanel(WidgetBlueprint, Params, ParentName, ParentError);
	if (ParentError.IsValid())
	{
		return ParentError;
	}
	if (Parent && !Cast<UCanvasPanel>(Parent) && Params->HasField(TEXT("position")))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unsupported_parent"), FString::Printf(
			TEXT("Panel '%s' is a %s, not a Canvas Panel, so 'position' cannot be applied; use set_widget_slot"),
			*ParentName, *Parent->GetClass()->GetName()));
	}

	// Create Button widget (must be created through the WidgetTree, like the UMG
	// designer does, so it is owned by the tree and registers as a blueprint variable)
	UButton* Button = WidgetBlueprint->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), *WidgetName);
	if (!Button)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("create_failed"), TEXT("Failed to create Button widget"));
	}

	// Set button text
	UTextBlock* ButtonTextBlock = NewObject<UTextBlock>(Button, UTextBlock::StaticClass(), *(WidgetName + TEXT("_Text")));
	if (ButtonTextBlock)
	{
		ButtonTextBlock->SetText(FText::FromString(ButtonText));
		Button->AddChild(ButtonTextBlock);
	}

	if (!Parent)
	{
		// The tree has no root yet, so this is it - the same thing the designer's first drop does.
		WidgetBlueprint->WidgetTree->RootWidget = Button;
		ParentName = FString();
	}
	else
	{
		UPanelSlot* ButtonSlot = Parent->AddChild(Button);
		if (!ButtonSlot)
		{
			Button->Rename(nullptr, GetTransientPackage());
			return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unsupported_parent"), FString::Printf(
				TEXT("Panel '%s' (%s) refused the child; it holds a limited number of children"),
				*ParentName, *Parent->GetClass()->GetName()));
		}
		if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(ButtonSlot))
		{
			CanvasSlot->SetPosition(ButtonPosition);
		}
	}

	// Save the Widget Blueprint
	FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);

	Response->SetBoolField(TEXT("success"), true);
	Response->SetStringField(TEXT("widget_name"), WidgetName);
	Response->SetStringField(TEXT("parent_widget"), ParentName);
	Response->SetBoolField(TEXT("became_root"), ParentName.IsEmpty());
	if (TSharedPtr<FJsonObject> SlotJson = SlotToJson(Button->Slot))
	{
		Response->SetObjectField(TEXT("slot"), SlotJson);
	}
	FinishWidgetWrite(WidgetBlueprint, Params, Response);
	return Response;
}