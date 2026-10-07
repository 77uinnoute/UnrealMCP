"""
Blueprint Node Tools for Unreal MCP.

This module provides tools for manipulating Blueprint graph nodes and connections.
"""

import logging
from typing import Dict, List, Any, Optional
from mcp.server.fastmcp import FastMCP, Context

# Get logger
logger = logging.getLogger("UnrealMCP")

def register_blueprint_node_tools(mcp: FastMCP):
    """Register Blueprint node manipulation tools with the MCP server."""
    
    @mcp.tool()
    def add_blueprint_event_node(
        ctx: Context,
        blueprint_name: str,
        event_name: str,
        node_position = None,
        graph_name: str = None
    ) -> Dict[str, Any]:
        """
        Add an event node to a Blueprint's event graph.
        
        Args:
            blueprint_name: Name of the target Blueprint
            event_name: Name of the event. Use 'Receive' prefix for standard events:
                       - 'ReceiveBeginPlay' for Begin Play
                       - 'ReceiveTick' for Tick
                       - etc.
            node_position: Optional [X, Y] position in the graph
            
        Returns:
            Dict with node_id, graph_name, node_count and readback (the created
            node's pins with their default values and links)
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            # Handle default value within the method body
            if node_position is None:
                node_position = [0, 0]
            
            params = {
                "blueprint_name": blueprint_name,
                "event_name": event_name,
                "node_position": node_position
            }
            if graph_name:
                params["graph_name"] = graph_name
            
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            logger.info(f"Adding event node '{event_name}' to blueprint '{blueprint_name}'")
            response = unreal.send_command("add_blueprint_event_node", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Event node creation response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error adding event node: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
    
    @mcp.tool()
    def add_blueprint_input_action_node(
        ctx: Context,
        blueprint_name: str,
        action_name: str,
        node_position = None,
        graph_name: str = None
    ) -> Dict[str, Any]:
        """
        Add an input action event node to a Blueprint's event graph.
        
        Args:
            blueprint_name: Name of the target Blueprint
            action_name: Name of the input action to respond to
            node_position: Optional [X, Y] position in the graph
            
        Returns:
            Dict with node_id, graph_name, node_count and readback (the created
            node's pins with their default values and links)
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            # Handle default value within the method body
            if node_position is None:
                node_position = [0, 0]
            
            params = {
                "blueprint_name": blueprint_name,
                "action_name": action_name,
                "node_position": node_position
            }
            if graph_name:
                params["graph_name"] = graph_name
            
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            logger.info(f"Adding input action node for '{action_name}' to blueprint '{blueprint_name}'")
            response = unreal.send_command("add_blueprint_input_action_node", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Input action node creation response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error adding input action node: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
    
    @mcp.tool()
    def add_blueprint_function_node(
        ctx: Context,
        blueprint_name: str,
        target: str,
        function_name: str,
        params = None,
        node_position = None,
        graph_name: str = None
    ) -> Dict[str, Any]:
        """
        Add a function call node to a Blueprint's event graph.

        Args:
            blueprint_name: Name of the target Blueprint
            target: Class that owns the function (e.g. "KismetSystemLibrary",
                "MeshComponent"). Classes not loaded yet are resolved by module path.
            function_name: Name of the function to call
            params: Pin name -> default value. Supported value types: number, bool,
                string (FName/FString/FText), enum member name or its number,
                [x, y] / [x, y, z] / [r, g, b, a] arrays or {x,y,z} / {r,g,b,a} objects
                for vector structs, and asset paths for object/class pins.
            node_position: Optional [X, Y] position in the graph

            graph_name: Optional graph to target; defaults to the event graph

        Returns:
            Dict with node_id, graph_name, node_count, applied (per written pin:
            pin + value), failed (pin + error_code + error) and readback. A pin
            that could not be written is reported in failed - never silently dropped.
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            # Handle default values within the method body
            if params is None:
                params = {}
            if node_position is None:
                node_position = [0, 0]
            
            command_params = {
                "blueprint_name": blueprint_name,
                "target": target,
                "function_name": function_name,
                "params": params,
                "node_position": node_position
            }
            if graph_name:
                command_params["graph_name"] = graph_name
            
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            logger.info(f"Adding function node '{function_name}' to blueprint '{blueprint_name}'")
            response = unreal.send_command("add_blueprint_function_node", command_params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Function node creation response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error adding function node: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
            
    @mcp.tool()
    def connect_blueprint_nodes(
        ctx: Context,
        blueprint_name: str,
        source_node_id: str,
        source_pin: str,
        target_node_id: str,
        target_pin: str,
        graph_name: str = None
    ) -> Dict[str, Any]:
        """
        Connect two nodes in a Blueprint's event graph.
        
        Args:
            blueprint_name: Name of the target Blueprint
            source_node_id: ID of the source node
            source_pin: Name of the output pin on the source node
            target_node_id: ID of the target node
            target_pin: Name of the input pin on the target node
            graph_name: Optional graph to target; defaults to the event graph

        Returns:
            Dict with resolved pins, source_linked_to / target_linked_from,
            graph_name, node_count and readback. The link goes through the graph
            schema, so type compatibility, implicit conversion and replacing an
            existing link on the target input are handled by the editor.
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            params = {
                "blueprint_name": blueprint_name,
                "source_node_id": source_node_id,
                "source_pin": source_pin,
                "target_node_id": target_node_id,
                "target_pin": target_pin
            }
            if graph_name:
                params["graph_name"] = graph_name
            
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            logger.info(f"Connecting nodes in blueprint '{blueprint_name}'")
            response = unreal.send_command("connect_blueprint_nodes", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Node connection response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error connecting nodes: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
    
    @mcp.tool()
    def add_blueprint_variable(
        ctx: Context,
        blueprint_name: str,
        variable_name: str,
        variable_type: str,
        is_exposed: bool = None,
        default_value: str = None,
        sub_class: str = None
    ) -> Dict[str, Any]:
        """
        Add a variable to a Blueprint.
        
        Args:
            blueprint_name: Name of the target Blueprint
            variable_name: Name of the variable
            variable_type: Base type plus an optional container suffix. Base types:
                bool / byte / int / int64 / float / double / name / string / text /
                object / class / struct / enum / vector / vector2d / rotator /
                transform / linear_color. Containers: "int[]" (Array), "struct{}"
                (Set), "int{,}" (Map).
            is_exposed: Instance editable - the "eye" in the details panel
                (CPF_DisableEditOnInstance). true = a level instance may change it,
                false = it may not. Omitted = leave the flag as the engine set it.
                This is the same bit set_blueprint_variable_flags(instance_editable=...)
                writes. For "visible in the class defaults panel" (CPF_Edit) read the
                separate read-only field `class_editable`.
            default_value: Optional default value, as text matching the type
            sub_class: Class/struct/enum asset path for object / class / struct / enum

        Returns:
            Dict with variable_name, variable_type, pin_category, container_type
            (when set), default_value, is_exposed (instance editable) and class_editable.
            An unknown type returns unsupported_variable_type together with a
            supported_types list.
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            params = {
                "blueprint_name": blueprint_name,
                "variable_name": variable_name,
                "variable_type": variable_type
            }
            if is_exposed is not None:
                params["is_exposed"] = is_exposed
            if default_value is not None:
                params["default_value"] = default_value
            if sub_class:
                params["sub_class"] = sub_class
            
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            logger.info(f"Adding variable '{variable_name}' to blueprint '{blueprint_name}'")
            response = unreal.send_command("add_blueprint_variable", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Variable creation response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error adding variable: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
    
    @mcp.tool()
    def add_blueprint_get_self_component_reference(
        ctx: Context,
        blueprint_name: str,
        component_name: str,
        node_position = None,
        graph_name: str = None
    ) -> Dict[str, Any]:
        """
        Add a node that gets a reference to a component owned by the current Blueprint.
        This creates a node similar to what you get when dragging a component from the Components panel.
        
        Args:
            blueprint_name: Name of the target Blueprint
            component_name: Name of the component to get a reference to
            node_position: Optional [X, Y] position in the graph
            
        Returns:
            Response containing the node ID and success status
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            # Handle None case explicitly in the function
            if node_position is None:
                node_position = [0, 0]
            
            params = {
                "blueprint_name": blueprint_name,
                "component_name": component_name,
                "node_position": node_position
            }
            if graph_name:
                params["graph_name"] = graph_name
            
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            logger.info(f"Adding self component reference node for '{component_name}' to blueprint '{blueprint_name}'")
            response = unreal.send_command("add_blueprint_get_self_component_reference", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Self component reference node creation response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error adding self component reference node: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
    
    @mcp.tool()
    def add_blueprint_self_reference(
        ctx: Context,
        blueprint_name: str,
        node_position = None,
        graph_name: str = None
    ) -> Dict[str, Any]:
        """
        Add a 'Get Self' node to a Blueprint's event graph that returns a reference to this actor.
        
        Args:
            blueprint_name: Name of the target Blueprint
            node_position: Optional [X, Y] position in the graph
            
        Returns:
            Response containing the node ID and success status
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            if node_position is None:
                node_position = [0, 0]
                
            params = {
                "blueprint_name": blueprint_name,
                "node_position": node_position
            }
            if graph_name:
                params["graph_name"] = graph_name
            
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            logger.info(f"Adding self reference node to blueprint '{blueprint_name}'")
            response = unreal.send_command("add_blueprint_self_reference", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Self reference node creation response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error adding self reference node: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
    
    @mcp.tool()
    def find_blueprint_nodes(
        ctx: Context,
        blueprint_name: str,
        node_type = None,
        event_type = None,
        graph_name: str = None,
        max_nodes: int = 200,
        verbose: bool = True
    ) -> Dict[str, Any]:
        """
        Read a Blueprint's event graph nodes (read-only: never creates a graph).

        Args:
            blueprint_name: Name of the target Blueprint
            node_type: "All" (default) or "Event"
            event_type: Event member name filter, only used when node_type="Event"
            graph_name: Optional graph to read. Without it the blueprint's event
                graph is used; an unknown graph_name reports graph_not_found with
                available_graphs.

        Returns:
            Dict with graph, graph_name, count, node_count, nodes and node_ids.
            Every node: node_id (guid), name, type, properties, pins, connections.
            Every pin: pin_name, direction ("input"/"output"), category,
            default_value (present only when the pin is unconnected) and
            linked_to (["<node guid>.<pin>", ...]). connections uses guids on both
            ends: {"from": "<guid>.<pin>", "to": "<guid>.<pin>"}.
            node_ids holds the matching event guids (node_type="Event" only).
            truncated is true ONLY when a node that should have been listed was
            dropped by max_nodes (re-ask with a bigger max_nodes); filtering does
            NOT set it - read filtered for that, because re-asking a filtered
            query gains nothing.

        verbose=False drops properties, pins and connections: the response declares that in
        omitted_fields[] and keeps pin_count per node instead. Such a payload cannot answer any
        pin-name question - filtering it by pin name silently matches nothing. Call again with
        verbose=True (the default) for pin-level work.
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            params = {
                "blueprint_name": blueprint_name,
                "node_type": node_type or "All",
                "event_type": event_type,
                "max_nodes": max(1, min(max_nodes, 1000)),
                "verbose": verbose
            }
            if graph_name:
                params["graph_name"] = graph_name
            
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            logger.info(f"Finding nodes in blueprint '{blueprint_name}'")
            response = unreal.send_command("find_blueprint_nodes", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Node find response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error finding nodes: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
    
    @mcp.tool()
    def set_blueprint_pin_default(
        ctx: Context,
        blueprint_name: str,
        node_id: str,
        pin_name: str,
        value,
        graph_name: str = None
    ) -> Dict[str, Any]:
        """
        Write one input pin's default value on an existing node.

        Use this to repair a node whose default value could not be set at creation
        time, or to change one afterwards. Same value dispatch as
        add_blueprint_function_node's params.

        Args:
            blueprint_name: Name of the target Blueprint
            node_id: Node guid (from find_blueprint_nodes)
            pin_name: Input pin name
            value: number / bool / string / enum member / vector array or object /
                asset path (object and class pins)

        Returns:
            Dict with node_id, pin_name, default_value_after, graph_name, node_count
            and readback, or a structured error (pin_not_found with available inputs
            / unsupported_pin_type / type_mismatch / load_failed).
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            params = {
                "blueprint_name": blueprint_name,
                "node_id": node_id,
                "pin_name": pin_name,
                "value": value
            }
            if graph_name:
                params["graph_name"] = graph_name

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            response = unreal.send_command("set_blueprint_pin_default", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error setting pin default: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def delete_blueprint_nodes(
        ctx: Context,
        blueprint_name: str,
        node_ids: List[str],
        graph_name: str = None,
        force: bool = False
    ) -> Dict[str, Any]:
        """
        Delete nodes (and their links) from a Blueprint's event graph.

        Args:
            blueprint_name: Name of the target Blueprint
            node_ids: Node guids to delete (from find_blueprint_nodes)

        Returns:
            Dict with deleted, failed (node_not_found per guid), deleted_count,
            failed_count and remaining_nodes. Each guid is handled independently.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            params = {
                "blueprint_name": blueprint_name,
                "node_ids": node_ids,
                "force": force
            }
            if graph_name:
                params["graph_name"] = graph_name

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            response = unreal.send_command("delete_blueprint_nodes", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error deleting blueprint nodes: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def disconnect_blueprint_pins(
        ctx: Context,
        blueprint_name: str,
        node_id: str,
        pin_name: str,
        linked_node_id: str = None,
        linked_pin_name: str = None,
        graph_name: str = None
    ) -> Dict[str, Any]:
        """
        Break the links of one pin (all of them, or only those matching the filter).

        Args:
            blueprint_name: Name of the target Blueprint
            node_id: Guid of the node owning the pin
            pin_name: Pin name (either direction)
            linked_node_id: Optional guid of the node on the other end
            linked_pin_name: Optional pin name on the other end

        Returns:
            Dict with links_before, disconnected_count and remaining_links, or a
            structured error (node_not_found / pin_not_found with available pins).
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            params = {
                "blueprint_name": blueprint_name,
                "node_id": node_id,
                "pin_name": pin_name
            }
            if graph_name:
                params["graph_name"] = graph_name
            if linked_node_id:
                params["linked_node_id"] = linked_node_id
            if linked_pin_name:
                params["linked_pin_name"] = linked_pin_name

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            response = unreal.send_command("disconnect_blueprint_pins", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error disconnecting blueprint pins: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def verify_blueprint_graph(
        ctx: Context,
        blueprint_name: str,
        graph_name: str = None,
        rules: List[str] = None,
        max_issues: int = 200
    ) -> Dict[str, Any]:
        """Run read-only Blueprint graph checks without compiling or saving.

        Rules: dangling_producer, required_input_unconnected, exec_multi_source,
        unreachable_node, anim_sequence_missing, anim_root_unconnected.
        Returns stable issue records and bounded truncation metadata.
        """
        from unreal_mcp_server import get_unreal_connection
        params = {"blueprint_name": blueprint_name,
                  "max_issues": max(1, min(max_issues, 1000))}
        if graph_name:
            params["graph_name"] = graph_name
        if rules is not None:
            params["rules"] = rules
        unreal = get_unreal_connection()
        if not unreal:
            return {"success": False, "message": "Failed to connect to Unreal Engine"}
        return unreal.send_command("verify_blueprint_graph", params) or {
            "success": False, "message": "No response from Unreal Engine"}

    @mcp.tool()
    def list_blueprint_graphs(
        ctx: Context,
        blueprint_name: str
    ) -> Dict[str, Any]:
        """
        List every graph of a Blueprint (read-only: never creates a graph).

        Args:
            blueprint_name: Name of the target Blueprint

        Returns:
            Dict with graphs, each item carrying graph_name, graph_class, node_count
            and is_editable (macro graphs are readable but not built into by this
            plugin). Pass graph_name to the other node tools to target a function
            graph or a macro graph.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            params = {"blueprint_name": blueprint_name}

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            response = unreal.send_command("list_blueprint_graphs", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error listing blueprint graphs: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def add_blueprint_function_graph(
        ctx: Context,
        blueprint_name: str,
        function_name: str,
        signature_class: str = None
    ) -> Dict[str, Any]:
        """
        Create a function graph on a Blueprint.

        Without signature_class this makes a user function. With signature_class it
        implements that class' BlueprintImplementableEvent of the same name (the
        override graph, same recipe the editor uses for e.g. AnimNotify's
        Received_Notify) -- useful for blueprint animation-notify callbacks.

        Args:
            blueprint_name: Name of the target Blueprint
            function_name: Function / event name; it becomes the graph name
            signature_class: Optional class declaring the event to implement

        Returns:
            Dict with graph_name, created (false when that graph already existed --
            the call is idempotent, a same-named graph is never duplicated),
            is_override, signature_class, entry_node_ids, result_node_ids, and the
            compile result (compiled / errors / warnings). Assert on the compile
            result, not on command success alone.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            params = {"blueprint_name": blueprint_name, "function_name": function_name}
            if signature_class:
                params["signature_class"] = signature_class

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            return unreal.send_command("add_blueprint_function_graph", params) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error adding blueprint function graph: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def remove_blueprint_function_graph(
        ctx: Context,
        blueprint_name: str,
        function_name: str
    ) -> Dict[str, Any]:
        """
        Remove a function graph from a Blueprint.

        Engine-owned graphs (the construction script, event graphs) are refused with
        a structured error instead of being silently skipped.

        Args:
            blueprint_name: Name of the target Blueprint
            function_name: Name of the function graph to remove

        Returns:
            Dict with removed, the remaining function_graphs list, and the compile
            result.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            params = {"blueprint_name": blueprint_name, "function_name": function_name}

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            return unreal.send_command("remove_blueprint_function_graph", params) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error removing blueprint function graph: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def list_blueprint_function_graphs(
        ctx: Context,
        blueprint_name: str
    ) -> Dict[str, Any]:
        """
        List a Blueprint's function graphs with their signature source (read-only).

        Args:
            blueprint_name: Name of the target Blueprint

        Returns:
            Dict with function_graphs; each item carries graph_name, node_count,
            signature_class and is_override (true when the graph implements a
            parent class' BlueprintImplementableEvent rather than a user function).
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            params = {"blueprint_name": blueprint_name}

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            return unreal.send_command("list_blueprint_function_graphs", params) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error listing blueprint function graphs: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def rename_blueprint_function_graph(
        ctx: Context,
        blueprint_name: str,
        old_name: str,
        new_name: str
    ) -> Dict[str, Any]:
        """
        Rename a function graph (the function it defines is renamed with it).

        Args:
            blueprint_name: Name of the target Blueprint
            old_name: Current function / graph name
            new_name: New function / graph name

        Returns:
            Dict with renamed, the function_graphs list after the rename, and the
            compile result. A name that is already taken is reported as
            graph_name_in_use instead of being silently suffixed.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            params = {"blueprint_name": blueprint_name, "old_name": old_name, "new_name": new_name}

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            return unreal.send_command("rename_blueprint_function_graph", params) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error renaming blueprint function graph: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def add_blueprint_function_param(
        ctx: Context,
        blueprint_name: str,
        function_name: str,
        param_name: str,
        param_type: str,
        sub_class: str = None,
        is_output: bool = False,
        default_value: str = None
    ) -> Dict[str, Any]:
        """
        Add an input parameter or an output (return value) to a function graph.

        Args:
            blueprint_name: Name of the target Blueprint
            function_name: Function graph to edit
            param_name: Parameter name
            param_type: Pin category (bool / int / int64 / float / double / name / string /
                text / object / class / struct / enum / vector / rotator / transform ...);
                containers use the same suffixes as add_blueprint_variable
                ("int[]" array, "struct{}" set, "int{,}" map)
            sub_class: Class / struct / enum path for object and struct types
            is_output: True adds a return value (the function's result node is created on
                demand) instead of an input parameter
            default_value: Optional default for an input parameter

        Returns:
            Dict with the function's full signature back (params: name / direction /
            type / sub_type / container / default_value) plus the compile result.
            An overriding function (created with signature_class) reports
            function_not_editable, because its parameters belong to the parent class.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            params = {"blueprint_name": blueprint_name, "function_name": function_name,
                      "param_name": param_name, "param_type": param_type}
            if sub_class:
                params["sub_class"] = sub_class
            if is_output:
                params["is_output"] = True
            if default_value is not None:
                params["default_value"] = default_value

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            return unreal.send_command("add_blueprint_function_param", params) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error adding blueprint function param: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def remove_blueprint_function_param(
        ctx: Context,
        blueprint_name: str,
        function_name: str,
        param_name: str
    ) -> Dict[str, Any]:
        """
        Remove an input parameter or an output from a function graph.

        Args:
            blueprint_name: Name of the target Blueprint
            function_name: Function graph to edit
            param_name: Parameter / output name to remove

        Returns:
            Dict with removed, was_output, the remaining signature (params) and the
            compile result. A missing parameter reports param_not_found, and the
            current names are listed in candidates.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            params = {"blueprint_name": blueprint_name, "function_name": function_name,
                      "param_name": param_name}

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            return unreal.send_command("remove_blueprint_function_param", params) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error removing blueprint function param: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def rename_blueprint_function_param(
        ctx: Context,
        blueprint_name: str,
        function_name: str,
        old_name: str,
        new_name: str
    ) -> Dict[str, Any]:
        """
        Rename an input parameter or an output of a function graph.

        Existing links on the pin follow the rename (the engine renames the pin and its
        user pin info together).

        Args:
            blueprint_name: Name of the target Blueprint
            function_name: Function graph to edit
            old_name: Current parameter name
            new_name: New parameter name

        Returns:
            Dict with the signature after the rename (params) and the compile result.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            params = {"blueprint_name": blueprint_name, "function_name": function_name,
                      "old_name": old_name, "new_name": new_name}

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            return unreal.send_command("rename_blueprint_function_param", params) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error renaming blueprint function param: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def add_blueprint_literal_node(
        ctx: Context,
        blueprint_name: str,
        literal_type: str,
        value: str,
        node_position = None,
        graph_name: str = None
    ) -> Dict[str, Any]:
        """
        Add a constant (literal) node holding a fixed value.

        Sugar over a function call node: creates KismetSystemLibrary.MakeLiteral<Type>
        and writes its Value pin, so no custom node class is injected into the
        blueprint. For a bare literal on an existing pin use set_blueprint_pin_default.

        Args:
            blueprint_name: Name of the target Blueprint
            literal_type: float / double / int / int64 / bool / name / byte / string / text
            value: The literal value as text ("3.0", "true", "MyName")
            node_position: Optional [X, Y] position in the graph
            graph_name: Optional graph to target; defaults to the event graph

        Returns:
            Dict with node_id, literal_type, value (read back), graph_name,
            node_count and readback. An unknown type returns unsupported_literal_type;
            a value that does not parse returns invalid_value and creates no node.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            if node_position is None:
                node_position = [0, 0]

            params = {
                "blueprint_name": blueprint_name,
                "literal_type": literal_type,
                "value": value,
                "node_position": node_position
            }
            if graph_name:
                params["graph_name"] = graph_name

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            response = unreal.send_command("add_blueprint_literal_node", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error adding literal node: {e}"
            logger.error(error_msg)
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def add_blueprint_node_by_class(
        ctx: Context,
        blueprint_name: str,
        node_class: str,
        node_position = None,
        graph_name: str = None
    ) -> Dict[str, Any]:
        """
        Add a skeleton node of any K2 node class, for node types without a dedicated tool.

        The node gets its default pins but no type-specific configuration (a Cast node
        has no target type yet, a Switch has no enum yet). Configure it afterwards with
        set_blueprint_pin_default / connect_blueprint_nodes.

        Args:
            blueprint_name: Name of the target Blueprint
            node_class: Node class short name or full path, e.g.
                K2Node_ExecutionSequence, K2Node_Select, K2Node_SwitchEnum,
                K2Node_CastByteToEnum, K2Node_Knot, EdGraphNode_Comment
            node_position: Optional [X, Y] position in the graph
            graph_name: Optional graph to target; defaults to the event graph

        Returns:
            Dict with node_id, node_class, graph_name, node_count and readback (the
            created node's pins). An unknown class returns node_class_not_found with
            candidate class names.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            if node_position is None:
                node_position = [0, 0]

            params = {
                "blueprint_name": blueprint_name,
                "node_class": node_class,
                "node_position": node_position
            }
            if graph_name:
                params["graph_name"] = graph_name

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            response = unreal.send_command("add_blueprint_node_by_class", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error adding node by class: {e}"
            logger.error(error_msg)
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def add_blueprint_variable_node(
        ctx: Context,
        blueprint_name: str,
        variable_name: str,
        node_kind: str = "get",
        node_position = None,
        graph_name: str = None,
        is_local: bool = False,
        variable_type: str = None,
        sub_class: str = None,
        default_value: str = None,
        owner_class: str = None
    ) -> Dict[str, Any]:
        """
        Add a variable Get or Set node to a Blueprint graph.

        Args:
            blueprint_name: Name of the target Blueprint
            variable_name: Name of the variable to read or write
            node_kind: "get" (default) or "set"
            node_position: Optional [X, Y] position in the graph
            graph_name: Required together with is_local (names the function graph)
            is_local: Use a local variable on the function graph instead of a member
                variable. When it does not exist yet, variable_type (and sub_class for
                object/struct/enum) is required to create it.
            variable_type: Only used when is_local creates a new local variable
            sub_class: Class/struct/enum path for a new local variable
            default_value: Default value for a newly created local variable
            owner_class: Read/write a property of ANOTHER class instead of this blueprint's own
                (e.g. owner_class="PlayerController", variable_name="bShowMouseCursor"). The node is
                non-self: it carries a Target (self) pin typed to that class, which you must wire
                (e.g. from GetPlayerController) or the blueprint will not compile. Unknown
                properties answer variable_not_found with that class' blueprint-visible
                properties; a Set of a BlueprintReadOnly property answers property_not_writable.
                Not combinable with is_local.

        Returns:
            Dict with node_id, variable_name, node_kind, is_local, owner_class, self_context,
            graph_name, node_count and readback. An unknown member variable returns
            variable_not_found together with the available variable names.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            if node_position is None:
                node_position = [0, 0]

            params = {
                "blueprint_name": blueprint_name,
                "variable_name": variable_name,
                "node_kind": node_kind or "get",
                "node_position": node_position,
                "is_local": is_local
            }
            if graph_name:
                params["graph_name"] = graph_name
            if variable_type:
                params["variable_type"] = variable_type
            if sub_class:
                params["sub_class"] = sub_class
            if default_value is not None:
                params["default_value"] = default_value
            if owner_class:
                params["owner_class"] = owner_class

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            response = unreal.send_command("add_blueprint_variable_node", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error adding variable node: {e}"
            logger.error(error_msg)
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def set_blueprint_node_property(
        ctx: Context,
        blueprint_name: str,
        node_id: str,
        property_name: str,
        property_value,
        graph_name: str = None
    ) -> Dict[str, Any]:
        """
        Write one of a graph node's own properties (not a pin default value).

        The write goes through the shared property reflector, so any node property type works and the
        values come back as real JSON: numbers, booleans, strings, enum member names, objects, and
        structs or containers in whatever shape reflect_probe reports (compact structs such as
        CommentColor are number arrays, e.g. [r, g, b, a]; a Switch node's PinNames is an array of
        strings). Call reflect_probe(target=<blueprint path>, node_id=..., property=...) to see the
        exact shapes for a node.

        Common properties:
        * any node:       NodePosX / NodePosY (int), NodeComment (string),
                          EnabledState ("Enabled" / "Disabled" / "DevelopmentOnly")
        * Comment node:   CommentColor ([r,g,b,a]), FontSize (int), MoveMode ("CommentBox" / ...)
        * Cast byte to enum: Enum (enum asset path), bSafe (bool)
        * Switch on enum: Enum (enum asset path); Switch on string: PinNames (array of strings)

        Writing an enum or container property can change the pin layout, so the node is rebuilt
        afterwards and the response reports pins_rebuilt plus the refreshed pin list. Other properties
        (numbers, strings, colours, ...) are written without a rebuild.

        Args:
            blueprint_name: Name of the target Blueprint
            node_id: Node guid (from find_blueprint_nodes)
            property_name: Property name on the node class (C++ name)
            property_value: value in the shape reflect_probe reports for the property
            graph_name: Optional graph to target; defaults to the event graph

        Returns:
            Dict with property_name, property_type, property_type_detail (container / element type /
            accepted shapes), property_value_before and property_value_after as JSON values,
            pins_rebuilt, plus node_id / graph_name / node_count and readback when the pins were
            rebuilt. Errors carry error_code (unknown_property with a "Did you mean:" suggestion /
            type_mismatch / load_failed / unsupported_property_type / invalid_value) together with
            supported_shapes, available_fields, hint, failed_index and unchanged.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            params = {
                "blueprint_name": blueprint_name,
                "node_id": node_id,
                "property_name": property_name,
                "property_value": property_value
            }
            if graph_name:
                params["graph_name"] = graph_name

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            response = unreal.send_command("set_blueprint_node_property", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error setting node property: {e}"
            logger.error(error_msg)
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def blueprint_graph_reflection_help(ctx: Context) -> Dict[str, Any]:
        """
        Usage notes for driving blueprint graphs from an editor python script.

        Inside execute_python_command / execute_python_file the MCP tools are not
        available, but the plugin exposes the same graph kernel through reflected
        functions, so a script can build a graph, assert the result and clean up in
        one pass:

            import unreal
            lib  = unreal.UnrealMCPBlueprintGraphLibrary
            bp   = unreal.load_asset('/Game/Blueprints/BP_Foo')
            g    = lib.get_graphs(bp).graphs[0].graph
            res  = lib.add_function_call_node(g, 'KismetSystemLibrary', 'Delay', 320, 0)
            assert res.success, res.error_message
            assert lib.set_pin_default(res.node, 'Duration', '3.0', 'auto').success
            for pin in lib.get_node_pins(res.node):
                print(pin.pin_name, pin.direction, pin.default_value, pin.linked_to)
            lib.delete_node(res.node)

        Returns:
            Dict listing the reflection functions and the MCP tools that mirror them.
        """
        return {
            "success": True,
            "message": "Blueprint graph reflection surface",
            "library": "unreal.UnrealMCPBlueprintGraphLibrary",
            "read": ["get_graphs", "get_graph_nodes", "get_node_pins", "get_node_info",
                     "get_node_properties", "find_graph",
                     "get_supported_variable_types", "get_supported_literal_types"],
            "write": ["add_function_call_node", "add_event_node", "add_variable_get_node",
                      "add_variable_set_node", "add_literal_node", "add_node_by_class",
                      "add_self_reference_node", "add_input_action_node", "connect_pins",
                      "set_pin_default", "set_node_property", "disconnect_pin", "delete_node",
                      "ensure_event_graph", "add_function_graph"],
            "compile_and_undo": ["compile_blueprint_checked", "begin_transaction", "end_transaction"],
            "mirrored_mcp_tools": ["find_blueprint_nodes", "list_blueprint_graphs",
                                   "add_blueprint_function_node", "add_blueprint_literal_node",
                                   "add_blueprint_node_by_class", "add_blueprint_variable_node",
                                   "connect_blueprint_nodes", "set_blueprint_pin_default",
                                   "set_blueprint_node_property",
                                   "disconnect_blueprint_pins", "delete_blueprint_nodes",
                                   "compile_blueprint"],
            "notes": "Both entry points call the same kernel, so graph state and error codes match.",
        }

    @mcp.tool()
    def move_blueprint_node(
        ctx: Context,
        blueprint_name: str,
        node_id: str,
        position: List[float],
        graph_name: str = None
    ) -> Dict[str, Any]:
        """
        Move a graph node to a new position (NodePosX / NodePosY) and read it back.

        Args:
            blueprint_name: Name of the target Blueprint
            node_id: Node guid (from find_blueprint_nodes)
            position: [X, Y] new position in the graph
            graph_name: Optional graph to target; without it every graph is searched for the guid

        Returns:
            Dict with pos_x / pos_y read back from the node, plus node_id / graph_name / node_count.
            A guid that does not exist reports node_not_found.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            params = {"blueprint_name": blueprint_name, "node_id": node_id, "position": position}
            if graph_name:
                params["graph_name"] = graph_name

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            response = unreal.send_command("move_blueprint_node", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error moving node: {e}"
            logger.error(error_msg)
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def refresh_blueprint_node(
        ctx: Context,
        blueprint_name: str,
        node_id: str,
        graph_name: str = None
    ) -> Dict[str, Any]:
        """
        Rebuild a node's pins (ReconstructNode) and read the refreshed pin list back.

        Use it after writing a property that the rebuild did not already handle, so "the pins did
        not catch up" can never be silent.

        Args:
            blueprint_name: Name of the target Blueprint
            node_id: Node guid
            graph_name: Optional graph to target; without it every graph is searched for the guid

        Returns:
            Dict with the refreshed readback (pin list), pin_count, node_id read back,
            node_id_before and node_id_changed (a node that rebuilt itself may come back with a new
            guid), plus graph_name / node_count.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            params = {"blueprint_name": blueprint_name, "node_id": node_id}
            if graph_name:
                params["graph_name"] = graph_name

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            response = unreal.send_command("refresh_blueprint_node", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error refreshing node: {e}"
            logger.error(error_msg)
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def split_blueprint_pin(
        ctx: Context,
        blueprint_name: str,
        node_id: str,
        pin_name: str,
        graph_name: str = None
    ) -> Dict[str, Any]:
        """
        Split a struct pin into its sub-pins (UEdGraphSchema_K2::SplitPin).

        Args:
            blueprint_name: Name of the target Blueprint
            node_id: Node guid holding the pin
            pin_name: Struct pin to split
            graph_name: Optional graph to target; without it every graph is searched for the guid

        Returns:
            Dict with sub_pins / sub_pin_count read back (empty sub_pins means the split did not
            happen) plus the node readback. A non-struct pin, an already split pin, or a refusal by
            the schema reports pin_not_splittable.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            params = {"blueprint_name": blueprint_name, "node_id": node_id, "pin_name": pin_name}
            if graph_name:
                params["graph_name"] = graph_name

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            response = unreal.send_command("split_blueprint_pin", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error splitting pin: {e}"
            logger.error(error_msg)
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def recombine_blueprint_pin(
        ctx: Context,
        blueprint_name: str,
        node_id: str,
        pin_name: str,
        graph_name: str = None
    ) -> Dict[str, Any]:
        """
        Recombine a split struct pin back into one pin (UEdGraphSchema_K2::RecombinePin).

        Args:
            blueprint_name: Name of the target Blueprint
            node_id: Node guid holding the pin
            pin_name: Split pin to recombine (the parent pin, not a sub-pin)
            graph_name: Optional graph to target; without it every graph is searched for the guid

        Returns:
            Dict with sub_pins (empty once recombined) plus the node readback. A pin that has no
            sub-pins reports pin_not_splittable, and passing a sub-pin points at its parent pin.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            params = {"blueprint_name": blueprint_name, "node_id": node_id, "pin_name": pin_name}
            if graph_name:
                params["graph_name"] = graph_name

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            response = unreal.send_command("recombine_blueprint_pin", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error recombining pin: {e}"
            logger.error(error_msg)
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def add_blueprint_custom_event_node(
        ctx: Context,
        blueprint_name: str,
        event_name: str,
        params: List[Dict[str, Any]] = None,
        graph_name: str = None,
        node_position: List[float] = None
    ) -> Dict[str, Any]:
        """
        Add a Custom Event node, optionally with parameters.

        Args:
            blueprint_name: Name of the target Blueprint
            event_name: Name of the custom event
            params: Parameters to add: [{"name": "Damage", "type": "float", "sub_class": ...}];
                types use the same grammar as add_blueprint_variable
            graph_name: Optional graph to target; defaults to the event graph
            node_position: [X, Y] node position in the graph

        Returns:
            Dict with node_id, event_name, params (name / type / container / default_value read
            back), param_count and the compile result. A name that is already a custom event or a
            function reports custom_event_name_in_use with candidates; a parameter that cannot be
            created aborts the command and the node is removed again.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            if node_position is None:
                node_position = [0, 0]

            cmd_params = {"blueprint_name": blueprint_name, "event_name": event_name,
                          "node_position": node_position}
            if params:
                cmd_params["params"] = params
            if graph_name:
                cmd_params["graph_name"] = graph_name

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            response = unreal.send_command("add_blueprint_custom_event_node", cmd_params)
            return response or {"success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error adding custom event node: {e}"
            logger.error(error_msg)
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def rename_blueprint_custom_event(
        ctx: Context,
        blueprint_name: str,
        node_id: str,
        new_name: str,
        graph_name: str = None
    ) -> Dict[str, Any]:
        """
        Rename a Custom Event node (its parameters are kept).

        Args:
            blueprint_name: Name of the target Blueprint
            node_id: Node guid of the custom event
            new_name: New event name
            graph_name: Optional graph to target; without it every graph is searched for the guid

        Returns:
            Dict with event_name read back, node_id, node_id_before / node_id_changed (the guid is
            reported either way so a rebuilt node cannot leave a stale id) and the compile result.
            A name that is already taken reports custom_event_name_in_use with candidates.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            params = {"blueprint_name": blueprint_name, "node_id": node_id, "new_name": new_name}
            if graph_name:
                params["graph_name"] = graph_name

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            response = unreal.send_command("rename_blueprint_custom_event", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error renaming custom event: {e}"
            logger.error(error_msg)
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def open_blueprint_graph(
        ctx: Context,
        blueprint_name: str,
        graph_name: str = None
    ) -> Dict[str, Any]:
        """
        Open a Blueprint's graph in the blueprint editor (opening the editor when needed).

        Args:
            blueprint_name: Name of the target Blueprint
            graph_name: Graph to open; defaults to the event graph

        Returns:
            Dict with opened, graph_name and editor. A graph that does not exist reports
            graph_not_found with available_graphs, a missing asset reports blueprint_not_found,
            and an unusable editor session reports editor_not_open (never a silent false).
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            params = {"blueprint_name": blueprint_name}
            if graph_name:
                params["graph_name"] = graph_name

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            response = unreal.send_command("open_blueprint_graph", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error opening blueprint graph: {e}"
            logger.error(error_msg)
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def focus_blueprint_node(
        ctx: Context,
        blueprint_name: str,
        node_id: str,
        graph_name: str = None
    ) -> Dict[str, Any]:
        """
        Bring an open Blueprint's graph editor to a node (jump to / focus it).

        Args:
            blueprint_name: Name of the target Blueprint
            node_id: Node guid to focus
            graph_name: Optional graph holding the node; without it every graph is searched

        Returns:
            Dict with focused, node_id, node_title and graph_name. A missing node reports
            node_not_found and an unusable editor session reports editor_not_open.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            params = {"blueprint_name": blueprint_name, "node_id": node_id}
            if graph_name:
                params["graph_name"] = graph_name

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            response = unreal.send_command("focus_blueprint_node", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error focusing node: {e}"
            logger.error(error_msg)
            return {"success": False, "message": str(e)}

    logger.info("Blueprint node tools registered successfully")