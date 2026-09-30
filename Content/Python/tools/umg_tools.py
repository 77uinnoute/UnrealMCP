"""
UMG Tools for Unreal MCP.

This module provides tools for creating and manipulating UMG Widget Blueprints in Unreal Engine.
"""

import logging
from typing import Dict, List, Any, Optional
from mcp.server.fastmcp import FastMCP, Context

# Get logger
logger = logging.getLogger("UnrealMCP")

# 同一件事在工具层与 bridge 层曾用同一个名字 "widget_name" 表示两个对象（控件蓝图 / 树内控件），
# 脚本经 execute_mcp_command 直连 bridge 时照抄工具层写法就会指错对象。参数名已统一为 bridge 的，
# 旧名保留一版：识别到旧名即按旧语义换算并在响应里标注 deprecated_params。
_UMG_PARAM_MIGRATION = ("; this tool now takes blueprint_name (the Widget Blueprint) + widget_name "
                        "(the widget inside it) - the old name goes away next version")

def register_umg_tools(mcp: FastMCP):

    def _mark_deprecated(response, deprecated):
        """Attach the migration note to a successful response that used an old parameter name."""
        if not deprecated:
            return response
        if isinstance(response, dict):
            response.setdefault("deprecated_params", list(deprecated))
            response.setdefault("migration", _UMG_PARAM_MIGRATION.lstrip("; "))
        return response
    """Register UMG tools with the MCP server."""

    @mcp.tool()
    def create_umg_widget_blueprint(
        ctx: Context,
        widget_name: str,
        path: str = "/Game/Widgets",
        parent_class: Optional[str] = None
    ) -> Dict[str, Any]:
        """
        Create a new UMG Widget Blueprint with a root Canvas Panel.

        Args:
            widget_name: Asset name, or a full asset path like /Game/UI/WBP_HUD
            path: Content folder to create it in (default /Game/Widgets); ignored when
                widget_name is already a full path
            parent_class: What the widget derives from - a user widget class name
                (UserWidget / CommonUserWidget / ...) or the path of an existing widget blueprint
                to derive from. Default UserWidget.

        Returns:
            Dict with the new asset's name, content path and parent_class
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            params = {
                "name": widget_name,
                "path": path,
            }
            if parent_class is not None:
                params["parent_class"] = parent_class
            
            logger.info(f"Creating UMG Widget Blueprint with params: {params}")
            response = unreal.send_command("create_umg_widget_blueprint", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Create UMG Widget Blueprint response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error creating UMG Widget Blueprint: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def add_text_block_to_widget(
        ctx: Context,
        blueprint_name: str = None,
        widget_name: str = None,
        text: str = "",
        position: List[float] = [0.0, 0.0],
        parent_widget: str = "",
        text_block_name: str = None
    ) -> Dict[str, Any]:
        """
        Add a Text Block widget to a UMG Widget Blueprint.

        Size / font size / colour are not creation parameters - set them afterwards with
        set_widget_slot and set_widget_properties.

        Parameter names are the bridge command's own (same dict works via execute_mcp_command).
        DEPRECATED form, recognised by the presence of text_block_name and answered with
        deprecated_params: widget_name = the Widget Blueprint, text_block_name = the new Text Block.

        Args:
            blueprint_name: Name or path of the target Widget Blueprint
            widget_name: Name to give the new Text Block
            text: Initial text content
            position: [X, Y] position in the canvas panel (canvas parents only)
            parent_widget: Panel to add the Text Block to; default = the root panel
            text_block_name: DEPRECATED, see above

        Returns:
            Dict containing success status and text block properties
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            deprecated = []
            if text_block_name is not None:
                deprecated.append("text_block_name")
                target_blueprint = blueprint_name or widget_name
                target_widget = text_block_name
            else:
                target_blueprint = blueprint_name
                target_widget = widget_name

            missing = [n for n, v in (("blueprint_name", target_blueprint), ("widget_name", target_widget)) if not v]
            if missing:
                return {"status": "error", "error_code": "missing_parameter",
                        "error": "Missing parameter(s): " + ", ".join(missing) + _UMG_PARAM_MIGRATION}

            params = {
                "blueprint_name": target_blueprint,
                "widget_name": target_widget,
                "text": text,
                "position": position
            }
            if parent_widget:
                params["parent_widget"] = parent_widget

            logger.info(f"Adding Text Block to widget with params: {params}")
            response = unreal.send_command("add_text_block_to_widget", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Add Text Block response: {response}")
            return _mark_deprecated(response, deprecated)
            
        except Exception as e:
            error_msg = f"Error adding Text Block to widget: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def add_button_to_widget(
        ctx: Context,
        blueprint_name: str = None,
        widget_name: str = None,
        text: str = "",
        position: List[float] = [0.0, 0.0],
        parent_widget: str = "",
        button_name: str = None
    ) -> Dict[str, Any]:
        """
        Add a Button widget (with an inner Text Block label) to a UMG Widget Blueprint.

        The button's own place/size is set afterwards with set_widget_slot and its colours with
        set_widget_properties (background_color).

        Parameter names are the bridge command's own (same dict works via execute_mcp_command).
        DEPRECATED form, recognised by the presence of button_name and answered with
        deprecated_params: widget_name = the Widget Blueprint, button_name = the new Button.

        Args:
            blueprint_name: Name or path of the target Widget Blueprint
            widget_name: Name to give the new Button
            text: Text to display on the button
            position: [X, Y] position in the canvas panel (canvas parents only)
            parent_widget: Panel to add the Button to; default = the root panel
            button_name: DEPRECATED, see above

        Returns:
            Dict containing success status and button properties
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            deprecated = []
            if button_name is not None:
                deprecated.append("button_name")
                target_blueprint = blueprint_name or widget_name
                target_widget = button_name
            else:
                target_blueprint = blueprint_name
                target_widget = widget_name

            missing = [n for n, v in (("blueprint_name", target_blueprint), ("widget_name", target_widget)) if not v]
            if missing:
                return {"status": "error", "error_code": "missing_parameter",
                        "error": "Missing parameter(s): " + ", ".join(missing) + _UMG_PARAM_MIGRATION}

            params = {
                "blueprint_name": target_blueprint,
                "widget_name": target_widget,
                "text": text,
                "position": position
            }
            if parent_widget:
                params["parent_widget"] = parent_widget
            
            logger.info(f"Adding Button to widget with params: {params}")
            response = unreal.send_command("add_button_to_widget", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Add Button response: {response}")
            return _mark_deprecated(response, deprecated)
            
        except Exception as e:
            error_msg = f"Error adding Button to widget: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def bind_widget_event(
        ctx: Context,
        blueprint_name: str = None,
        widget_name: str = None,
        event_name: str = None,
        widget_component_name: str = None
    ) -> Dict[str, Any]:
        """
        Create the bound event node for an event on a widget inside a Widget Blueprint (OnClicked, ...).

        The node body is filled in afterwards with the blueprint graph tools; there is no
        function-name parameter (the command never used one).

        Parameter names are the bridge command's own, so the same dict works through
        execute_mcp_command in scripts:
            bind_widget_event(blueprint_name="WBP_Menu", widget_name="Btn_Play", event_name="OnClicked")

        DEPRECATED form (kept one version, answers with deprecated_params): widget_name = the
        Widget Blueprint and widget_component_name = the widget inside it. It is recognised by the
        presence of widget_component_name and will be removed in the next version.

        Args:
            blueprint_name: Name or path of the target Widget Blueprint
            widget_name: Widget inside the tree whose event is bound (button, etc.)
            event_name: Name of the event to bind (OnClicked, etc.)
            widget_component_name: DEPRECATED, see above

        Returns:
            Dict containing success status and the created event node's name
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            deprecated = []
            if widget_component_name is not None:
                # Old form: widget_name named the blueprint, widget_component_name the widget.
                deprecated.append("widget_component_name")
                target_blueprint = blueprint_name or widget_name
                target_widget = widget_component_name
            else:
                target_blueprint = blueprint_name
                target_widget = widget_name

            missing = [n for n, v in (("blueprint_name", target_blueprint), ("widget_name", target_widget),
                                      ("event_name", event_name)) if not v]
            if missing:
                return {"status": "error", "error_code": "missing_parameter",
                        "error": "Missing parameter(s): " + ", ".join(missing)}

            params = {
                "blueprint_name": target_blueprint,
                "widget_name": target_widget,
                "event_name": event_name,
            }

            logger.info(f"Binding widget event with params: {params}")
            response = unreal.send_command("bind_widget_event", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            if deprecated and isinstance(response, dict):
                response["deprecated_params"] = deprecated
                response["migration"] = ("bind_widget_event now takes blueprint_name (the Widget Blueprint) + "
                                         "widget_name (the widget inside it); widget_component_name goes away next version")
            logger.info(f"Bind widget event response: {response}")
            return response

        except Exception as e:
            error_msg = f"Error binding widget event: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def add_widget_to_viewport(
        ctx: Context,
        widget_name: str,
        z_order: int = 0
    ) -> Dict[str, Any]:
        """
        Create a Widget Blueprint in the RUNNING PIE world and add it to the viewport.

        BREAKING: this no longer just echoes the asset's class path (a call used to look successful
        while nothing appeared on screen). It requires a live PIE session: without one it answers
        pie_not_running (call start_pie first), and with a session that has no player controller yet
        it answers pie_not_ready. The instance lives in the PIE world only and is gone when PIE stops.

        Args:
            widget_name: Name or path of the Widget Blueprint to instantiate
            z_order: Z-order for the widget (higher numbers appear on top)

        Returns:
            Dict with created_widget_name, z_order, pie_world, owning_player and in_viewport;
            a structured error (pie_not_running / pie_not_ready / widget_blueprint_not_found) otherwise.
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            params = {
                "blueprint_name": widget_name,
                "z_order": z_order
            }
            
            logger.info(f"Adding widget to viewport with params: {params}")
            response = unreal.send_command("add_widget_to_viewport", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Add widget to viewport response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error adding widget to viewport: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def get_pie_widget_tree(
        ctx: Context,
        blueprint_name: str,
        instance_index: int = 0,
        root_widget: str = None,
        max_depth: int = None
    ) -> Dict[str, Any]:
        """
        Read-only: read the RUNTIME widget tree of a live instance of a Widget Blueprint in the
        running PIE world - what the UI actually holds right now, which python cannot reach
        (UUserWidget.WidgetTree is invisible to python and widget variables read back None).

        Per widget: visibility, is_visible (effective, parent chain included), render_opacity,
        render_transform, desired_size, geometry (absolute_position / absolute_size / local_size
        from the cached geometry), has_geometry, runtime slot values. TextBlock adds the current
        text / font_size / font_object / color / auto_wrap / wrap_text_at; Image adds the current
        brush_resource / image_size / tint; Button adds is_hovered / is_pressed.

        Geometry is the LAST PAINTED layout: after changing state (a click, a call) read it in a
        later call, not in the same script dispatch. PIE itself needs editor ticks, so start_pie,
        create the widget, and read it in separate calls.

        Args:
            blueprint_name: Name or path of the Widget Blueprint
            instance_index: Which instance when several exist (creation order, default 0)
            root_widget: Only return the subtree under this widget (e.g. "RightCol")
            max_depth: Limit recursion depth

        Returns:
            Dict with instance_count / instance_name / instance_path / in_viewport / owning_player /
            root; pie_not_running / widget_instance_not_found (+ candidates = UserWidget classes in
            PIE) / widget_blueprint_not_found / widget_not_found otherwise.

        instance_path is the object path of that live instance, so python can reach the very same
        widget (e.g. unreal.load_object(None, instance_path) and then call_method("SomeBpFunction",
        ...) - blueprint functions ARE callable this way, PyWrapperObject.h:1135).
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            params = {"blueprint_name": blueprint_name, "instance_index": instance_index}
            if root_widget:
                params["root_widget"] = root_widget
            if max_depth is not None:
                params["max_depth"] = max_depth
            return unreal.send_command("get_pie_widget_tree", params)
        except Exception as e:
            error_msg = f"Error reading PIE widget tree: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def set_widget_property_binding(
        ctx: Context,
        blueprint_name: str,
        widget_name: str,
        property_name: str,
        binding_name: str
    ) -> Dict[str, Any]:
        """
        Bind any bindable widget property to a Get<binding_name> getter.

        A property is bindable when the widget class carries a matching <Property>Delegate (Text,
        Visibility, Percent, ColorAndOpacity, ... - the list is read off the class, not hard-coded).
        The backing variable and the getter's return type are derived from what the delegate returns.
        The command then compiles and checks the generated class: if the engine dropped the binding
        (a non-pure getter, a type it cannot bind), it fails with binding_not_effective and hands back
        the compiler message instead of reporting success.

        Args:
            blueprint_name: Name of the target Widget Blueprint
            widget_name: Widget to bind
            property_name: Widget property to bind (must have a <Property>Delegate)
            binding_name: Name of the backing variable; the getter is Get<binding_name>

        Returns:
            Dict with binding_name / widget_name / property_name / function_name / variable_type /
            binding_count / runtime_binding_count / effective
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {
                "blueprint_name": blueprint_name,
                "widget_name": widget_name,
                "property_name": property_name,
                "binding_name": binding_name,
            }
            logger.info(f"Setting widget property binding with params: {params}")
            response = unreal.send_command("set_widget_property_binding", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error setting widget property binding: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def unbind_widget_property(
        ctx: Context,
        blueprint_name: str,
        widget_name: str,
        property_name: str,
        remove_function: bool = False,
        remove_variable: bool = False
    ) -> Dict[str, Any]:
        """
        Remove one property binding.

        Only the binding entry goes by default - the getter and the backing variable stay, because
        other bindings or graph nodes may still use them. Ask for remove_function / remove_variable to
        delete those too; while anything still refers to them the command refuses with
        blocked_by_references and lists the referrers instead of breaking the blueprint.

        Args:
            blueprint_name: Name of the target Widget Blueprint
            widget_name: Widget whose binding is removed
            property_name: Bound property name
            remove_function: Also delete the getter function graph when nothing else uses it
            remove_variable: Also delete the backing variable when nothing else uses it

        Returns:
            Dict with function_removed / variable_removed / binding_count / runtime_binding_count
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {
                "blueprint_name": blueprint_name,
                "widget_name": widget_name,
                "property_name": property_name,
                "remove_function": remove_function,
                "remove_variable": remove_variable,
            }
            logger.info(f"Unbinding widget property with params: {params}")
            response = unreal.send_command("unbind_widget_property", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error unbinding widget property: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def prune_widget_bindings(
        ctx: Context,
        blueprint_name: str,
        dry_run: bool = True
    ) -> Dict[str, Any]:
        """
        Report the leftovers of widget bindings: stale_binding (the widget is no longer in the tree),
        orphan_function (nothing calls the getter and no binding uses it), unused_variable (the
        variable only that orphan getter used).

        dry_run defaults to true, so a call changes nothing; dry_run=false applies exactly the same
        rules. Variables that are not binding leftovers (your own variables, widget variables) are not
        reported or touched.

        Args:
            blueprint_name: Name of the target Widget Blueprint
            dry_run: Report only, change nothing (default true)

        Returns:
            Dict with dry_run / items[] (kind, name, detail) / item_count / removed[] / counts
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"blueprint_name": blueprint_name, "dry_run": dry_run}
            logger.info(f"Pruning widget bindings with params: {params}")
            response = unreal.send_command("prune_widget_bindings", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error pruning widget bindings: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def set_text_block_binding(
        ctx: Context,
        blueprint_name: str = None,
        widget_name: str = None,
        binding_name: str = None,
        property_name: str = "Text",
        text_block_name: str = None,
        binding_property: str = None
    ) -> Dict[str, Any]:
        """
        DEPRECATED alias of set_widget_property_binding (kept one version; the response carries
        deprecated_command). Binds a widget property (default Text) to a Get<...> function.

        Creates the backing variable and the Get<...> getter, exposes the widget as a blueprint
        variable and registers the UMG property binding (UWidgetBlueprint::Bindings), so the widget
        is refreshed from that variable at runtime. The property must have a <Property>Delegate
        companion; otherwise the command fails with unsupported_property (carrying
        bindable_properties). The binding is verified against the generated class - if the engine
        dropped it at compile time the command fails with binding_not_effective instead of lying.

        Parameter names are the bridge command's own. DEPRECATED form (answered with
        deprecated_params): widget_name = the Widget Blueprint, text_block_name = the widget,
        binding_property = the backing variable.

        Args:
            blueprint_name: Name or path of the target Widget Blueprint
            widget_name: Widget inside the tree whose property is bound
            binding_name: Name of the backing variable / getter to create
            property_name: Widget property to bind (default Text)
            text_block_name: DEPRECATED, see above
            binding_property: DEPRECATED, see above

        Returns:
            Dict with binding_name, widget_name, property_name, function_name, binding_count,
            runtime_binding_count, deprecated_command
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            deprecated = []
            target_blueprint = blueprint_name
            target_widget = widget_name
            target_binding = binding_name
            if text_block_name is not None:
                deprecated.append("text_block_name")
                target_blueprint = blueprint_name or widget_name
                target_widget = text_block_name
            if binding_property is not None:
                deprecated.append("binding_property")
                target_binding = binding_property

            missing = [n for n, v in (("blueprint_name", target_blueprint), ("widget_name", target_widget),
                                      ("binding_name", target_binding)) if not v]
            if missing:
                return {"status": "error", "error_code": "missing_parameter",
                        "error": "Missing parameter(s): " + ", ".join(missing) + _UMG_PARAM_MIGRATION}

            params = {
                "blueprint_name": target_blueprint,
                "widget_name": target_widget,
                "binding_name": target_binding,
                "property_name": property_name,
            }
            
            logger.info(f"Setting text block binding with params: {params}")
            response = unreal.send_command("set_text_block_binding", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Set text block binding response: {response}")
            return _mark_deprecated(response, deprecated)
            
        except Exception as e:
            error_msg = f"Error setting text block binding: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def set_widget_slot(
        ctx: Context,
        blueprint_name: str,
        widget_name: str,
        slot: Dict[str, Any] = None
    ) -> Dict[str, Any]:
        """
        Write one widget's slot, keyed exactly like get_widget_tree reads it back.

        Which keys exist depends on the widget's slot class:
          Canvas Panel : anchors / alignment / position / size / auto_size / z_order
          Box / Scroll : padding / horizontal_alignment / vertical_alignment / size_rule
          Overlay, SizeBox : padding / horizontal_alignment / vertical_alignment
          Grid Panel   : padding / horizontal_alignment / vertical_alignment / row / column /
                         row_span / column_span / layer / nudge

        The anchor set decides what the position means: anchors [0.5, 0.5, 0.5, 0.5] plus
        alignment [0.5, 0.5] pins the widget's centre to the middle of the screen at any
        resolution (this is how a crosshair is centred), whereas the default anchors
        [0, 0, 0, 0] treat the position as pixels from the top left.

        Widget properties (text colour, font size, justification, brush, percent ...) are not
        slot values - use set_widget_properties for those.

        Args:
            blueprint_name: Name of the target Widget Blueprint
            widget_name: Widget whose slot is written
            slot: Slot values, e.g. {"anchors": [0.5, 0.5, 0.5, 0.5], "alignment": [0.5, 0.5]}
                  or {"padding": {"left": 8, "top": 4}, "vertical_alignment": "Center"}.
                  Keys the widget's slot class does not take are rejected with the field list.
                  An empty object {} is a no-op (changed: false, nothing compiled or saved).

        Returns:
            Dict with the slot read back from the widget plus applied / compiled.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"blueprint_name": blueprint_name, "widget_name": widget_name}
            if slot is not None:
                params["slot"] = slot

            logger.info(f"Setting widget slot with params: {params}")
            response = unreal.send_command("set_widget_slot", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error setting widget slot: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def add_widget(
        ctx: Context,
        blueprint_name: str,
        widget_class: str,
        widget_name: str,
        parent_widget: str = "",
        as_variable: bool = False,
        slot: Dict[str, Any] = None
    ) -> Dict[str, Any]:
        """
        Create any widget inside a Widget Blueprint and add it to a parent panel.

        This is the general constructor: add_text_block_to_widget / add_button_to_widget are
        shortcuts for the two most common widgets, everything else (VerticalBox, Image,
        ProgressBar, Overlay, GridPanel, Slider, CheckBox, ...) goes through here.

        GridPanel children: AddChild does not lay them out, so give each one slot={"row", "column"}
        here; without them the response carries warnings[{code: grid_slot_unplaced}] and every such
        child stacks in cell (0, 0).

        Args:
            blueprint_name: Name of the target Widget Blueprint
            widget_class: python class name (ProgressBar) or C++ name (UProgressBar)
            widget_name: Name for the new widget; must be free inside the widget tree
            parent_widget: Panel to add it to; default = the root widget (which becomes the root
                when the tree has none)
            as_variable: Expose the widget as a blueprint variable (default false)
            slot: Optional slot values written in the same call, same keys as set_widget_slot for
                the parent's slot class. A key that slot class does not take rejects the whole call
                (unsupported_slot + fields) and the widget is not created.

        Returns:
            Dict with widget_name / widget_class / parent_widget / slot / warnings / compiled
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            params = {
                "blueprint_name": blueprint_name,
                "widget_class": widget_class,
                "widget_name": widget_name,
                "as_variable": as_variable,
            }
            if parent_widget:
                params["parent_widget"] = parent_widget
            if slot:
                params["slot"] = slot
            return unreal.send_command("add_widget", params)
        except Exception as e:
            error_msg = f"Error adding widget: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def set_widget_properties(
        ctx: Context,
        blueprint_name: str,
        widget_name: str,
        props: Dict[str, Any]
    ) -> Dict[str, Any]:
        """
        Write widget properties by key; each key is applied independently and reported.

        Common keys: visibility, is_enabled, tooltip, render_opacity, render_transform
        ({translation/scale/shear/angle}), render_transform_pivot, clipping, navigation,
        navigation_all.
        Per type: TextBlock text / auto_wrap / wrap_text_at / font_size / color / justification,
        Button background_color, Image brush / tint, ProgressBar percent / fill_color,
        Slider value, CheckBox checked_state, EditableText text.
        Any other CPF_Edit property on the widget class is written through reflection; a key
        that does not exist comes back in failed[] with the candidate list.

        Args:
            blueprint_name: Name of the target Widget Blueprint
            widget_name: Widget to write
            props: {property_key: value}

        Returns:
            Dict with applied[] (key / property / value_before / value_after) and failed[]
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            params = {"blueprint_name": blueprint_name, "widget_name": widget_name, "props": props}
            return unreal.send_command("set_widget_properties", params)
        except Exception as e:
            error_msg = f"Error setting widget properties: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def remove_widget(ctx: Context, blueprint_name: str, widget_name: str) -> Dict[str, Any]:
        """
        Remove a widget (and its children), together with what it owned.

        The cleanup chain covers the property bindings on that widget, the Get<widget> getter
        function they pointed at and the widget's blueprint variable. If any graph node still
        refers to the widget the command refuses with blocked_by_references and lists the nodes.

        Args:
            blueprint_name: Name of the target Widget Blueprint
            widget_name: Widget to remove
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            return unreal.send_command("remove_widget", {"blueprint_name": blueprint_name, "widget_name": widget_name})
        except Exception as e:
            error_msg = f"Error removing widget: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def reparent_widget(
        ctx: Context,
        blueprint_name: str,
        widget_name: str,
        new_parent: str
    ) -> Dict[str, Any]:
        """
        Move a widget into another panel, which changes its slot class.

        Slot fields both slot types share (padding / alignment / size rule / canvas placement /
        grid cell) are carried over and reported in migrated_fields; the rest is reported in
        dropped_fields.

        Args:
            blueprint_name: Name of the target Widget Blueprint
            widget_name: Widget to move
            new_parent: Panel widget it should be added to
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            params = {"blueprint_name": blueprint_name, "widget_name": widget_name, "new_parent": new_parent}
            return unreal.send_command("reparent_widget", params)
        except Exception as e:
            error_msg = f"Error reparenting widget: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def reorder_widget(
        ctx: Context,
        blueprint_name: str,
        widget_name: str,
        index: int
    ) -> Dict[str, Any]:
        """
        Reorder one panel's children (draw order) by moving a widget to an index.

        Args:
            blueprint_name: Name of the target Widget Blueprint
            widget_name: Widget to move inside its parent panel
            index: Target child index (0 = first / drawn first)
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            params = {"blueprint_name": blueprint_name, "widget_name": widget_name, "index": index}
            return unreal.send_command("reorder_widget", params)
        except Exception as e:
            error_msg = f"Error reordering widget: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def rename_widget(
        ctx: Context,
        blueprint_name: str,
        widget_name: str,
        new_name: str
    ) -> Dict[str, Any]:
        """
        Rename a widget, keeping its variable, bindings, navigation rules and graph references in sync.

        Args:
            blueprint_name: Name of the target Widget Blueprint
            widget_name: Current widget name
            new_name: New name (must be free inside the widget tree)
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            params = {"blueprint_name": blueprint_name, "widget_name": widget_name, "new_name": new_name}
            return unreal.send_command("rename_widget", params)
        except Exception as e:
            error_msg = f"Error renaming widget: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def set_root_widget(ctx: Context, blueprint_name: str, widget_name: str) -> Dict[str, Any]:
        """
        Make an existing panel the widget tree's root, keeping the previous root as its child.

        Args:
            blueprint_name: Name of the target Widget Blueprint
            widget_name: Widget to become the root (must be a panel when the tree already has a root)
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            return unreal.send_command("set_root_widget", {"blueprint_name": blueprint_name, "widget_name": widget_name})
        except Exception as e:
            error_msg = f"Error setting root widget: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def get_widget_tree(ctx: Context, blueprint_name: str) -> Dict[str, Any]:
        """
        Read a Widget Blueprint back: widget hierarchy (with child order), slot values,
        per-type properties, property bindings and bound widget events.

        Args:
            blueprint_name: Name or path of the target Widget Blueprint

        Returns:
            Dict with root (nested name/class/visibility/is_variable/slot/properties/children),
            widget_count, widget_names, bindings (widget/property/function/kind),
            events (widget/event/graph), has_root, compiled, status (compiler status name, e.g.
            "BS_UpToDate") and status_code (the matching number).
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"blueprint_name": blueprint_name}

            logger.info(f"Reading widget tree with params: {params}")
            response = unreal.send_command("get_widget_tree", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error reading widget tree: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def get_umg_compile_errors(
        ctx: Context, blueprint_name: str, since: Optional[str] = None
    ) -> Dict[str, Any]:
        """
        Report the widget compiler's verdict on a Widget Blueprint: status, the messages the compile
        produced (severity / text / offending node), and the editor property bindings the compile did
        NOT carry into the runtime table (bindings_dropped) - the silent failure mode where the
        designer shows a binding that the widget never applies.

        The command compiles the asset to capture the messages (it does not save), and only reports
        the dropped bindings; repair them with set_widget_property_binding / prune_widget_bindings.

        Args:
            blueprint_name: Name or path of the target Widget Blueprint
            since: Optional boundary for the this-compile/historical split (ISO-8601 timestamp or
                epoch seconds). Default = the last MCP write to this asset.

        Returns:
            Dict with compiled, status, status_code, num_errors, num_warnings, messages[],
            log_this_compile[], log_historical[], boundary_source, since_boundary,
            binding_summary{editor_count, runtime_count}, binding_reconciled and bindings_dropped[].
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"blueprint_name": blueprint_name}
            if since is not None:
                params["since"] = since

            logger.info(f"Reading UMG compile errors with params: {params}")
            response = unreal.send_command("get_umg_compile_errors", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error reading UMG compile errors: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def compile_umg_widget(
        ctx: Context, blueprint_name: str, force_full: bool = True
    ) -> Dict[str, Any]:
        """
        Compile a Widget Blueprint on demand and report what the compiler said.

        force_full (default true) marks the blueprint structurally modified before compiling. That is
        the path which also re-runs the editor-to-runtime property binding copy together with its
        validity checks; it is not an engine "full compile" switch.

        Args:
            blueprint_name: Name or path of the target Widget Blueprint
            force_full: Mark the blueprint structurally modified first (default True)

        Returns:
            Dict with compiled, status, num_errors, num_warnings, messages[], saved and
            persist_requested.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"blueprint_name": blueprint_name, "force_full": force_full}

            logger.info(f"Compiling UMG widget with params: {params}")
            response = unreal.send_command("compile_umg_widget", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error compiling UMG widget: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def open_umg_designer(
        ctx: Context, blueprint_name: str, focus_widget: Optional[str] = None
    ) -> Dict[str, Any]:
        """
        Open a Widget Blueprint in the UMG designer (widget blueprints, unlike graphs, have no other
        way to be opened). The engine records where an asset should open next time when its tab
        closes; that record is captured and put back, so an MCP-driven open does not change where the
        user sees the asset afterwards.

        Args:
            blueprint_name: Name or path of the target Widget Blueprint
            focus_widget: Optional widget to select once the designer is open

        Returns:
            Dict with opened, editor_open, editor_name, has_preview, focused / focus_reason and
            the current animation names.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"blueprint_name": blueprint_name}
            if focus_widget is not None:
                params["focus_widget"] = focus_widget

            logger.info(f"Opening UMG designer with params: {params}")
            response = unreal.send_command("open_umg_designer", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error opening UMG designer: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def set_umg_design_size(
        ctx: Context, blueprint_name: str, width: Optional[float] = None, height: Optional[float] = None
    ) -> Dict[str, Any]:
        """
        Set the design-time preview size of a Widget Blueprint (designer preview only - the runtime
        size is decided by the widget's parent at play time).

        Honest boundary: UMG has no per-asset DPI scale (the designer derives DPI from the project's
        UI scale curve) and no per-asset preview platform (that is the designer view's own device
        profile). Those two are deliberately NOT parameters here: asking for them answers
        'unsupported' instead of pretending to write something.

        Args:
            blueprint_name: Name or path of the target Widget Blueprint
            width: Design preview width (switches the widget into the custom size mode)
            height: Design preview height

        Returns:
            Dict with applied, design_size_before, design_size (read back) and size_mode_changed.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"blueprint_name": blueprint_name}
            if width is not None:
                params["width"] = width
            if height is not None:
                params["height"] = height

            logger.info(f"Setting UMG design size with params: {params}")
            response = unreal.send_command("set_umg_design_size", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error setting UMG design size: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def create_widget_animation(
        ctx: Context, blueprint_name: str, animation_name: str
    ) -> Dict[str, Any]:
        """
        Create a widget animation (UWidgetAnimation with its own movie scene) on a Widget Blueprint,
        attach it to the blueprint and compile, so both the designer's animation list and the runtime
        (which reads the generated class copy) see it.

        Args:
            blueprint_name: Name or path of the target Widget Blueprint
            animation_name: Name for the new animation (name_collision if it is taken)

        Returns:
            Dict with animation_name, animations, animation_count, compiled and animation_effective.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"blueprint_name": blueprint_name, "animation_name": animation_name}

            logger.info(f"Creating widget animation with params: {params}")
            response = unreal.send_command("create_widget_animation", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error creating widget animation: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def add_widget_animation_track(
        ctx: Context, blueprint_name: str, animation_name: str, widget_name: str, property: str
    ) -> Dict[str, Any]:
        """
        Add the animation track that drives one widget property.

        Supported properties (anything else answers unsupported_property with the list):
            RenderOpacity    -> float track
            Visibility       -> enum track (visibility names, e.g. "Collapsed")
            RenderTransform  -> 2D transform track (translation / scale / shear / angle)

        The possessable and its binding entry are written by the engine's own writer, so the runtime
        can resolve which widget the track drives.

        Args:
            blueprint_name: Name or path of the target Widget Blueprint
            animation_name: Animation to add the track to
            widget_name: Widget the track animates
            property: RenderOpacity / Visibility / RenderTransform

        Returns:
            Dict with the track (track_class, section_class, possessable_guid), track_count and the
            usual compiled / animation_effective tail.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {
                "blueprint_name": blueprint_name,
                "animation_name": animation_name,
                "widget_name": widget_name,
                "property": property,
            }

            logger.info(f"Adding widget animation track with params: {params}")
            response = unreal.send_command("add_widget_animation_track", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error adding widget animation track: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def set_widget_animation_keyframes(
        ctx: Context,
        blueprint_name: str,
        animation_name: str,
        widget_name: str,
        property: str,
        keys: List[Dict[str, Any]],
    ) -> Dict[str, Any]:
        """
        Write the keyframes of one animation track. The list replaces whatever was there (the same
        "no hidden leftovers" contract as the slot writer); keys_replaced reports what went away.

        Value shape follows the property:
            RenderOpacity    {"time": 0.0, "value": 0.0}
            Visibility       {"time": 0.5, "value": "Visible"}  (a name or a number)
            RenderTransform  {"time": 1.0, "value": {"translation": [x, y], "angle": deg}}
                             (translation / scale / shear / angle - at least one of them)

        A key past the end of the playback range extends the range (playback_range_extended: true):
        keys outside the range would otherwise never play.

        Args:
            blueprint_name: Name or path of the target Widget Blueprint
            animation_name: Animation that owns the track
            widget_name: Widget the track animates
            property: RenderOpacity / Visibility / RenderTransform
            keys: The whole key list, [{time, value}], time in seconds

        Returns:
            Dict with keys_written, keys_replaced, keys (read back), playback_range_extended.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {
                "blueprint_name": blueprint_name,
                "animation_name": animation_name,
                "widget_name": widget_name,
                "property": property,
                "keys": keys,
            }

            logger.info(f"Writing widget animation keyframes with params: {params}")
            response = unreal.send_command("set_widget_animation_keyframes", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error writing widget animation keyframes: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def list_widget_animations(ctx: Context, blueprint_name: str) -> Dict[str, Any]:
        """
        Read every animation of a Widget Blueprint back: its tracks (widget + property), the keyframes
        of each track (time + value; RenderTransform reports per-channel keys), and whether the
        animation reached the generated class (in_generated_class is the copy the runtime plays - the
        asset object alone proves nothing).

        Args:
            blueprint_name: Name or path of the target Widget Blueprint

        Returns:
            Dict with animations[] (animation_name, start_time, end_time, tracks[], in_generated_class),
            animation_count, generated_animation_count and supported_properties.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"blueprint_name": blueprint_name}

            logger.info(f"Listing widget animations with params: {params}")
            response = unreal.send_command("list_widget_animations", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error listing widget animations: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def play_widget_animation_preview(
        ctx: Context,
        blueprint_name: str,
        animation_name: str,
        start_at_time: float = 0.0,
        loops: int = 1,
        speed: float = 1.0,
        play_mode: str = "forward",
    ) -> Dict[str, Any]:
        """
        Play an animation in the designer preview (for the user to watch in the designer viewport).

        Requires an open designer session (open_umg_designer): the preview widget holds its own copies
        of the animations, so the asset object itself cannot be played there. Nothing here touches the
        asset - the instance lives in the preview world.

        Args:
            blueprint_name: Name or path of the target Widget Blueprint
            animation_name: Animation to play as it is named in the asset
            start_at_time: Start time in seconds
            loops: Number of loops
            speed: Playback speed
            play_mode: forward / reverse / pingpong

        Returns:
            Dict with playing, preview_animation_name, playback_context and the applied parameters;
            editor_not_open / preview_not_ready / animation_not_in_preview when it cannot play.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {
                "blueprint_name": blueprint_name,
                "animation_name": animation_name,
                "start_at_time": start_at_time,
                "loops": loops,
                "speed": speed,
                "play_mode": play_mode,
            }

            logger.info(f"Playing widget animation preview with params: {params}")
            response = unreal.send_command("play_widget_animation_preview", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error playing widget animation preview: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def stop_widget_animation_preview(
        ctx: Context, blueprint_name: str, animation_name: Optional[str] = None
    ) -> Dict[str, Any]:
        """
        Stop one animation (or every animation) in the designer preview.

        Args:
            blueprint_name: Name or path of the target Widget Blueprint
            animation_name: Animation to stop; omit to stop all of them

        Returns:
            Dict with stopped, mode ("one" / "all") and playback_context.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"blueprint_name": blueprint_name}
            if animation_name is not None:
                params["animation_name"] = animation_name

            logger.info(f"Stopping widget animation preview with params: {params}")
            response = unreal.send_command("stop_widget_animation_preview", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error stopping widget animation preview: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def remove_widget_animation(
        ctx: Context, blueprint_name: str, animation_name: str
    ) -> Dict[str, Any]:
        """
        Delete a widget animation (its movie scene goes with it) and recompile.

        Refuses with blocked_by_references while a graph node still holds the animation, naming the
        nodes - deleting under them would leave the nodes pointing at nothing.

        Args:
            blueprint_name: Name or path of the target Widget Blueprint
            animation_name: Animation to delete

        Returns:
            Dict with removed, animations (what is left), animation_count and the compile tail;
            animation_not_found / blocked_by_references / animation_still_present otherwise.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"blueprint_name": blueprint_name, "animation_name": animation_name}

            logger.info(f"Removing widget animation with params: {params}")
            response = unreal.send_command("remove_widget_animation", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error removing widget animation: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def rename_widget_animation(
        ctx: Context, blueprint_name: str, animation_name: str, new_name: str
    ) -> Dict[str, Any]:
        """
        Rename a widget animation (object name + display label) and recompile.

        Args:
            blueprint_name: Name or path of the target Widget Blueprint
            animation_name: Animation to rename
            new_name: New animation name (name_collision if it is taken)

        Returns:
            Dict with previous_name, animation_name, renamed and the compile tail.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"blueprint_name": blueprint_name, "animation_name": animation_name,
                      "new_name": new_name}

            logger.info(f"Renaming widget animation with params: {params}")
            response = unreal.send_command("rename_widget_animation", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error renaming widget animation: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def set_widget_animation_playback_range(
        ctx: Context,
        blueprint_name: str,
        animation_name: str,
        length: Optional[float] = None,
        end_time: Optional[float] = None,
        start_time: Optional[float] = None,
    ) -> Dict[str, Any]:
        """
        Set an animation's playback range - that range is what "the animation's length" means.

        Keys outside the new range are kept (never trimmed behind your back) and counted in
        keys_beyond_range: they simply never play.

        Args:
            blueprint_name: Name or path of the target Widget Blueprint
            animation_name: Animation to retime
            length: Length in seconds, measured from start_time (default: the current start)
            end_time: Absolute end in seconds (takes precedence over length)
            start_time: Absolute start in seconds (default: keep the current start)

        Returns:
            Dict with playback_start_before / playback_end_before, playback_start / playback_end /
            playback_length (read back), key_count, keys_beyond_range.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"blueprint_name": blueprint_name, "animation_name": animation_name}
            if length is not None:
                params["length"] = length
            if end_time is not None:
                params["end_time"] = end_time
            if start_time is not None:
                params["start_time"] = start_time

            logger.info(f"Setting widget animation playback range with params: {params}")
            response = unreal.send_command("set_widget_animation_playback_range", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error setting widget animation playback range: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def set_widget_variable(
        ctx: Context, blueprint_name: str, widget_name: str, as_variable: bool
    ) -> Dict[str, Any]:
        """
        Expose a widget as a blueprint variable, or stop exposing it.

        The flag alone changes nothing at runtime: the generated class property appears or disappears
        on a compile, so the command compiles and verifies it, and answers variable_not_effective
        instead of reporting success when the property did not come out as asked.

        Args:
            blueprint_name: Name or path of the target Widget Blueprint
            widget_name: Widget to toggle
            as_variable: True = expose as a variable, False = stop exposing it

        Returns:
            Dict with was_variable, is_variable, generated_property_present, changed, compiled.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"blueprint_name": blueprint_name, "widget_name": widget_name,
                      "as_variable": as_variable}

            logger.info(f"Setting widget variable flag with params: {params}")
            response = unreal.send_command("set_widget_variable", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error setting widget variable flag: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    logger.info("UMG tools registered successfully") 