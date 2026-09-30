#pragma once

#include "CoreMinimal.h"
#include "Json.h"
#include "Core/MCPCommandRegistry.h"

/**
 * Handles UMG (Widget Blueprint) related MCP commands
 * Responsible for creating and modifying UMG Widget Blueprints,
 * adding widget components, and managing widget instances in the viewport.
 */
class UNREALMCP_API FUnrealMCPUMGCommands
{
public:
    FUnrealMCPUMGCommands();

    /** Declare this domain's commands (name, category, params, flags) in the process-wide table. */
    void RegisterCommands(FMCPCommandRegistry& Registry);

private:
    /**
     * Create a new UMG Widget Blueprint
     * @param Params - Must include "name" for the blueprint name
     * @return JSON response with the created blueprint details
     */
    TSharedPtr<FJsonObject> HandleCreateUMGWidgetBlueprint(const TSharedPtr<FJsonObject>& Params);

    /**
     * Add a Text Block widget to a UMG Widget Blueprint
     * @param Params - Must include:
     *                "blueprint_name" - Name of the target Widget Blueprint
     *                "widget_name" - Name for the new Text Block
     *                "text" - Initial text content (optional)
     *                "position" - [X, Y] position in the canvas (optional)
     * @return JSON response with the added widget details
     */
    TSharedPtr<FJsonObject> HandleAddTextBlockToWidget(const TSharedPtr<FJsonObject>& Params);

    /**
     * Add a widget instance to the game viewport
     * @param Params - Must include:
     *                "blueprint_name" - Name of the Widget Blueprint to instantiate
     *                "z_order" - Z-order for widget display (optional)
     * @return JSON response with the widget instance details
     */
    TSharedPtr<FJsonObject> HandleAddWidgetToViewport(const TSharedPtr<FJsonObject>& Params);

    /**
     * Add a Button widget to a UMG Widget Blueprint
     * @param Params - Must include:
     *                "blueprint_name" - Name of the target Widget Blueprint
     *                "widget_name" - Name for the new Button
     *                "text" - Button text
     *                "position" - [X, Y] position in the canvas
     * @return JSON response with the added widget details
     */
    TSharedPtr<FJsonObject> HandleAddButtonToWidget(const TSharedPtr<FJsonObject>& Params);

    /**
     * Bind an event to a widget (e.g. button click)
     * @param Params - Must include:
     *                "blueprint_name" - Name of the target Widget Blueprint
     *                "widget_name" - Name of the widget to bind
     *                "event_name" - Name of the event to bind
     * @return JSON response with the binding details
     */
    TSharedPtr<FJsonObject> HandleBindWidgetEvent(const TSharedPtr<FJsonObject>& Params);

    /**
     * Set up text block binding for dynamic updates
     * @param Params - Must include:
     *                "blueprint_name" - Name of the target Widget Blueprint
     *                "widget_name" - Name of the widget to bind
     *                "binding_name" - Name of the binding to set up
     * @return JSON response with the binding details
     */
    TSharedPtr<FJsonObject> HandleSetTextBlockBinding(const TSharedPtr<FJsonObject>& Params);

    /**
     * Bind any bindable widget property (<Property>Delegate) to a Get<binding_name> getter.
     * @param Params - Must include "blueprint_name", "widget_name", "property_name", "binding_name"
     * @return JSON response with the binding read back from the generated class
     */
    TSharedPtr<FJsonObject> HandleSetWidgetPropertyBinding(const TSharedPtr<FJsonObject>& Params);

    /**
     * Remove one property binding, optionally its getter and backing variable.
     * @param Params - Must include "blueprint_name", "widget_name", "property_name"
     * @return JSON response with the remaining binding counts
     */
    TSharedPtr<FJsonObject> HandleUnbindWidgetProperty(const TSharedPtr<FJsonObject>& Params);

    /**
     * Report (and optionally remove) stale bindings, orphan getters and unused binding variables.
     * @param Params - Must include "blueprint_name"; optional "dry_run" (default true)
     * @return JSON response with the items found / removed
     */
    TSharedPtr<FJsonObject> HandlePruneWidgetBindings(const TSharedPtr<FJsonObject>& Params);

    /**
     * Place / style one widget inside its Canvas Panel slot.
     * @param Params - Must include "blueprint_name" and "widget_name";
     *                optional "anchors" [min_x, min_y, max_x, max_y], "alignment" [x, y],
     *                "position" [x, y], "size" [w, h], "auto_size" bool, "z_order" int, and for
     *                Text Blocks "font_size" int, "color" [r, g, b, a], "justification"
     *                ("left" / "center" / "right").
     * @return JSON response with the slot values read back from the widget
     */
    TSharedPtr<FJsonObject> HandleSetWidgetSlot(const TSharedPtr<FJsonObject>& Params);

    /**
     * Read a Widget Blueprint back: widget hierarchy (with child order), slot values,
     * per-type properties, property bindings and bound widget events.
     * @param Params - Must include "blueprint_name" (name or path)
     * @return JSON response with the widget tree
     */
    TSharedPtr<FJsonObject> HandleGetWidgetTree(const TSharedPtr<FJsonObject>& Params);

    /**
     * One implementation behind set_widget_property_binding and its deprecated alias
     * set_text_block_binding: enumerates the target's bindable properties, derives the variable and
     * getter types from the delegate's return type, and verifies the binding reached the runtime table.
     * @param bAliasCommand - true when called through the deprecated name (adds deprecated_command)
     */
    TSharedPtr<FJsonObject> SetWidgetPropertyBindingInternal(const TSharedPtr<FJsonObject>& Params,
                                                             bool bAliasCommand);

    /**
     * Create any widget class inside a Widget Blueprint and add it to a parent panel.
     * @param Params - Must include "blueprint_name", "widget_class", "widget_name";
     *                optional "parent_widget" (default = the root widget) and "as_variable"
     * @return JSON response with the widget read back (class, parent, widget count, compiled)
     */
    TSharedPtr<FJsonObject> HandleAddWidget(const TSharedPtr<FJsonObject>& Params);

    /**
     * Write widget properties from a "props" object (common + per-type keys, reflection fallback).
     * @param Params - Must include "blueprint_name", "widget_name", "props"
     * @return JSON response with applied[] / failed[] and the properties read back
     */
    TSharedPtr<FJsonObject> HandleSetWidgetProperties(const TSharedPtr<FJsonObject>& Params);

    /**
     * Remove a widget and its children, plus the bindings, variable and getter function it owned.
     * @param Params - Must include "blueprint_name" and "widget_name"
     * @return JSON response with the removed widgets / bindings / variables / functions
     */
    TSharedPtr<FJsonObject> HandleRemoveWidget(const TSharedPtr<FJsonObject>& Params);

    /**
     * Move a widget into another panel (which changes its slot class).
     * @param Params - Must include "blueprint_name", "widget_name", "new_parent"
     * @return JSON response with the new parent, child order and the slot read back
     */
    TSharedPtr<FJsonObject> HandleReparentWidget(const TSharedPtr<FJsonObject>& Params);

    /**
     * Reorder one panel's children by moving a widget to an index.
     * @param Params - Must include "blueprint_name", "widget_name", "index"
     * @return JSON response with the parent's child order read back
     */
    TSharedPtr<FJsonObject> HandleReorderWidget(const TSharedPtr<FJsonObject>& Params);

    /**
     * Rename a widget and keep its variable, bindings, navigation and graph references in sync.
     * @param Params - Must include "blueprint_name", "widget_name", "new_name"
     * @return JSON response with the renamed widget and the reference nodes that were fixed up
     */
    TSharedPtr<FJsonObject> HandleRenameWidget(const TSharedPtr<FJsonObject>& Params);

    /**
     * Make an existing panel the widget tree's root, keeping the previous root as its child.
     * @param Params - Must include "blueprint_name" and "widget_name"
     * @return JSON response with the new root, previous root and child order
     */
    TSharedPtr<FJsonObject> HandleSetRootWidget(const TSharedPtr<FJsonObject>& Params);

    /**
     * Read the compiler's verdict on a widget blueprint: status, the messages this compile produced,
     * and the editor bindings the compile did not carry into the runtime table.
     * @param Params - Must include "blueprint_name"; optional "since" (ISO-8601 or epoch seconds)
     * @return JSON response with messages / split log / binding_summary / bindings_dropped
     */
    TSharedPtr<FJsonObject> HandleGetUMGCompileErrors(const TSharedPtr<FJsonObject>& Params);

    /**
     * Compile a widget blueprint on demand and report what the compiler said.
     * @param Params - Must include "blueprint_name"; optional "force_full" (default true)
     * @return JSON response with compiled / num_errors / num_warnings / messages
     */
    TSharedPtr<FJsonObject> HandleCompileUMGWidget(const TSharedPtr<FJsonObject>& Params);

    /**
     * Open the widget blueprint in the UMG designer, optionally selecting a widget.
     * @param Params - Must include "blueprint_name"; optional "focus_widget"
     * @return JSON response with opened / editor_open / has_preview / focused
     */
    TSharedPtr<FJsonObject> HandleOpenUMGDesigner(const TSharedPtr<FJsonObject>& Params);

    /**
     * Read the runtime widget tree of a live PIE instance of a widget blueprint (read-only).
     * @param Params - Must include "blueprint_name"; optional "instance_index", "root_widget", "max_depth"
     * @return JSON response with instance_count / instance_name / in_viewport / root
     */
    TSharedPtr<FJsonObject> HandleGetPIEWidgetTree(const TSharedPtr<FJsonObject>& Params);

    /**
     * Set the designer preview size (design-time only).
     * @param Params - Must include "blueprint_name" plus "width" and/or "height"
     * @return JSON response with design_size_before / design_size, or "unsupported" for dpi/platform
     */
    TSharedPtr<FJsonObject> HandleSetUMGDesignSize(const TSharedPtr<FJsonObject>& Params);

    /**
     * Play an animation in the designer preview (preview instance only, never the asset).
     * @param Params - Must include "blueprint_name" and "animation_name"
     * @return JSON response with playing / preview_animation_name / playback_context
     */
    TSharedPtr<FJsonObject> HandlePlayWidgetAnimationPreview(const TSharedPtr<FJsonObject>& Params);

    /**
     * Stop one animation (or all of them) in the designer preview.
     * @param Params - Must include "blueprint_name"; optional "animation_name"
     * @return JSON response with stopped / mode
     */
    TSharedPtr<FJsonObject> HandleStopWidgetAnimationPreview(const TSharedPtr<FJsonObject>& Params);

    /**
     * Create a widget animation on a widget blueprint.
     * @param Params - Must include "blueprint_name" and "animation_name"
     * @return JSON response with animation_name / animations / animation_effective
     */
    TSharedPtr<FJsonObject> HandleCreateWidgetAnimation(const TSharedPtr<FJsonObject>& Params);

    /**
     * Add the animation track for one widget property (RenderOpacity / Visibility / RenderTransform).
     * @param Params - Must include "blueprint_name", "animation_name", "widget_name", "property"
     * @return JSON response with the track (class / section / possessable guid)
     */
    TSharedPtr<FJsonObject> HandleAddWidgetAnimationTrack(const TSharedPtr<FJsonObject>& Params);

    /**
     * Write the keyframes of one animation track (replaces what was there).
     * @param Params - Must include "blueprint_name", "animation_name", "widget_name", "property", "keys"
     * @return JSON response with keys_written / keys_replaced / keys (read back)
     */
    TSharedPtr<FJsonObject> HandleSetWidgetAnimationKeyframes(const TSharedPtr<FJsonObject>& Params);

    /**
     * Expose a widget as a blueprint variable, or stop exposing it.
     * @param Params - Must include "blueprint_name", "widget_name" and "as_variable"
     * @return JSON response with was_variable / is_variable / generated_property_present / compiled
     */
    TSharedPtr<FJsonObject> HandleSetWidgetVariable(const TSharedPtr<FJsonObject>& Params);

    /**
     * Read every animation back: tracks (widget + property) and keyframes.
     * @param Params - Must include "blueprint_name"
     * @return JSON response with animations[] and whether each is in the generated class
     */
    TSharedPtr<FJsonObject> HandleListWidgetAnimations(const TSharedPtr<FJsonObject>& Params);

    /**
     * Delete a widget animation (and its movie scene) from a widget blueprint.
     * @param Params - Must include "blueprint_name" and "animation_name"
     * @return JSON response with removed / animations / animation_effective
     */
    TSharedPtr<FJsonObject> HandleRemoveWidgetAnimation(const TSharedPtr<FJsonObject>& Params);

    /**
     * Rename a widget animation.
     * @param Params - Must include "blueprint_name", "animation_name" and "new_name"
     * @return JSON response with animation_name / renamed / animations
     */
    TSharedPtr<FJsonObject> HandleRenameWidgetAnimation(const TSharedPtr<FJsonObject>& Params);

    /**
     * Set an animation's playback range (what "the animation's length" means).
     * @param Params - Must include "blueprint_name", "animation_name" and a length (or start/end)
     * @return JSON response with playback_start / playback_end / keys_beyond_range
     */
    TSharedPtr<FJsonObject> HandleSetWidgetAnimationPlaybackRange(const TSharedPtr<FJsonObject>& Params);
}; 