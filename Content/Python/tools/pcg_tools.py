"""
PCG tools for Unreal MCP - graph queries and graph edits.

PCG graphs are not readable through the editor python layer in any structured way: property
names have to be guessed (`scale_min` exists on PCGTransformPointsSettings while
`b_uniform_scale` does not), a wrong pin label makes `add_edge` fail *silently*, and the
generation result can only be printed as a point count. These tools forward to the same-named
bridge commands, which reflect the real state instead:

    list_pcg_assets            PCG assets (PCGGraph / PCGGraphInstance / PCGDataAsset) under a folder
    get_pcg_graph              nodes, pin labels/connections, edge list, graph input/output nodes
    get_pcg_node               the reflected property table of one node's settings
    list_pcg_components        PCG components in the level with graph, trigger, bounds, resources
    get_pcg_generated_output   the existing generation result plus per-attribute statistics
    add_pcg_node               add a node by settings class
    connect_pcg_pins           connect two pins by label, read the connection back
    disconnect_pcg_pins        remove one edge by its two endpoints
    remove_pcg_node            remove one node (no automatic rewiring)
    set_pcg_node_property      write one settings property and read it back
    generate_pcg_component     dispatch generation (asynchronous)
    cleanup_pcg_component      dispatch cleanup (asynchronous)
    create_pcg_graph           create a graph asset (writes it to disk, never overwrites)
    set_pcg_component_graph    bind a graph to a level component (level is left unsaved)
    set_pcg_mesh_selector_type      pick the spawner's mesh selector class (instantiates it)
    set_pcg_mesh_selector_entries   replace the weighted mesh table (integer weights)

Read-only contract (the five query tools):
  * nothing there generates, cleans up, saves an asset or marks a package dirty;
  * `get_pcg_generated_output` reports `not_generated` instead of generating on demand;
  * a component that has never generated stays that way after a call.

Write contract (the nine edit tools):
  * they change the graph asset itself and save it on success (`saved`) - the editor is routinely
    killed by the build script, so an unsaved graph edit would be lost;
  * every write is read back: a connection that did not happen is `pin_not_connected`, a property
    whose value did not survive is `readback_mismatch` and is left unsaved;
  * `generate_pcg_component` / `cleanup_pcg_component` only dispatch - read the outcome with the
    query tools afterwards instead of waiting;
  * `create_pcg_graph` writes a new asset to disk (never overwriting) and `set_pcg_component_graph`
    changes the level but deliberately leaves it unsaved (`level_package_dirty`).

Two spellings appear in every pin/property readback on purpose:
  * `label` / `property_name` - what the graph side uses (`add_edge` takes these labels, e.g. "Out");
  * `python_name`            - the snake_case form the property layer uses (`lower_bound`).
An instanced property (`writable: false`, `instanced: true` - e.g. PCGStaticMeshSpawnerSettings'
MeshSelectorParameters) is readable but cannot be assigned; its hint says what to call instead, and
the write tool refuses it with that same hint.

Reading a generated result is a two step affair: call without `attribute` to get `attributes`
(the available name/type pairs), then call again with the attribute you want summarised.
"""

import logging
from typing import Any, Dict, List, Optional
from mcp.server.fastmcp import FastMCP, Context

# Get logger
logger = logging.getLogger("UnrealMCP")


def register_pcg_tools(mcp: FastMCP):
    """Register the read-only PCG tools with the MCP server."""

    def _send(command: str, params: Dict[str, Any]) -> Optional[Dict[str, Any]]:
        """Forward one command to the bridge; returns None when there is no connection."""
        from unreal_mcp_server import get_unreal_connection

        unreal = get_unreal_connection()
        if not unreal:
            return None

        response = unreal.send_command(command, params)
        return response

    @mcp.tool()
    def list_pcg_assets(ctx: Context, folder: str = "/Game",
                        class_filter: Optional[str] = None,
                        recursive: bool = True) -> Dict[str, Any]:
        """
        List PCG assets under a content folder (read-only, asset registry query only).

        Args:
            folder: Content folder to search (default "/Game").
            class_filter: Only this class - "PCGGraph", "PCGGraphInstance" or "PCGDataAsset".
            recursive: Include sub folders (default True).

        Returns:
            folder, recursive, count and assets[{asset_path, name, class, package_path}].
            Errors: folder_not_found, unknown_class_filter (with candidates).
        """
        params: Dict[str, Any] = {"folder": folder, "recursive": recursive}
        if class_filter is not None:
            params["class_filter"] = class_filter

        response = _send("list_pcg_assets", params)
        if not response:
            return {"success": False, "error_code": "no_connection",
                    "error": "Failed to forward list_pcg_assets to the editor"}
        return response

    @mcp.tool()
    def get_pcg_graph(ctx: Context, asset_path: str, detail: str = "summary",
                      max_nodes: Optional[int] = None) -> Dict[str, Any]:
        """
        Dump a PCG graph: nodes, pin labels/connections, edges and the input/output nodes (read-only).

        Each node carries `name`, `settings_class`, `editor_x`/`editor_y` and `input_pins` /
        `output_pins`; each pin carries `label` (what `add_edge` takes), `python_name`,
        `connected`, `edge_count` and `edges_to` (the nodes it links to). Empty pin arrays are
        returned as they are - a node genuinely without output pins (e.g. PCGDebugSettings)
        reports an empty array rather than an invented pin.

        Args:
            asset_path: PCG graph asset path.
            detail: "summary" (default) or "full" - full also returns each node's property table.
            max_nodes: Return at most this many nodes; `node_count` and `truncated` still describe
                the real graph.

        Returns:
            asset_path, graph_class, detail, node_count, truncated, nodes, edges, input_node,
            output_node. Errors: asset_not_found, not_a_pcg_graph.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "detail": detail}
        if max_nodes is not None:
            params["max_nodes"] = max_nodes

        response = _send("get_pcg_graph", params)
        if not response:
            return {"success": False, "error_code": "no_connection",
                    "error": "Failed to forward get_pcg_graph to the editor"}
        return response

    @mcp.tool()
    def get_pcg_node(ctx: Context, asset_path: str, node_name: Optional[str] = None,
                     node_index: Optional[int] = None,
                     max_properties: Optional[int] = None) -> Dict[str, Any]:
        """
        Reflect one PCG node's settings: every property with its type, value and writability (read-only).

        This is the command that removes the guessing: PCG settings properties are enumerated
        through engine reflection, so no name list is maintained here. Every entry carries
        `property_name` (reflection name, e.g. "LowerBound"), `python_name` ("lower_bound"),
        `type`, `value_text`, a structured `value` when the type allows it, the flags
        (`editable` / `edit_const` / `transient` / `instanced`) and `writable`.
        An instanced read-only subobject (For example PCGStaticMeshSpawnerSettings'
        MeshSelectorParameters) reports `writable: false` with a hint: read it, then write the
        inner object instead of assigning the property.

        Args:
            asset_path: PCG graph asset path.
            node_name: Node object name (from get_pcg_graph).
            node_index: Node index, used when node_name is absent.
            max_properties: Cap the returned property list.

        Returns:
            node_name, settings_class, settings_path, property_count, writable_count,
            readonly_instanced_count, truncated and properties[].
            Errors: asset_not_found, not_a_pcg_graph, node_not_found (with candidates).
        """
        params: Dict[str, Any] = {"asset_path": asset_path}
        if node_name is not None:
            params["node_name"] = node_name
        if node_index is not None:
            params["node_index"] = node_index
        if max_properties is not None:
            params["max_properties"] = max_properties

        response = _send("get_pcg_node", params)
        if not response:
            return {"success": False, "error_code": "no_connection",
                    "error": "Failed to forward get_pcg_node to the editor"}
        return response

    @mcp.tool()
    def list_pcg_components(ctx: Context, actor_label: Optional[str] = None) -> Dict[str, Any]:
        """
        List the PCG components in the current editor world (read-only).

        Each entry carries `actor_label`, `actor_class`, `component_name`, `graph_path`
        (`graph_instance_path` when the component drives a graph instance), `generation_trigger`,
        `active`, `generating`, `generated`, the actor's `bounds_min`/`bounds_max`, and what the
        generation left behind: `instanced_mesh_components`, `instanced_mesh_instances` and
        `instance_batches` (per batch: component name, mesh, instance count). Engine private
        managed-resource arrays are deliberately not reported - the instance batches are the
        observable equivalent. Nothing is generated here and no package is saved.

        Args:
            actor_label: Only components whose actor label contains this text.

        Returns:
            world_name, count and components[].
        """
        params: Dict[str, Any] = {}
        if actor_label is not None:
            params["actor_label"] = actor_label

        response = _send("list_pcg_components", params)
        if not response:
            return {"success": False, "error_code": "no_connection",
                    "error": "Failed to forward list_pcg_components to the editor"}
        return response

    @mcp.tool()
    def get_pcg_generated_output(ctx: Context, actor_label: Optional[str] = None,
                                component_name: Optional[str] = None,
                                attribute: Optional[str] = None,
                                max_samples: int = 8,
                                include_attributes: bool = True) -> Dict[str, Any]:
        """
        Read a PCG component's existing generation result (read-only, never generates).

        Call it once without `attribute` to see `attributes` (name/type pairs), then again with the
        attribute you want summarised. `attributes` mixes two sources, marked by `source`:
        point-intrinsic fields (`density`, `seed`, `color`, `position`, `scale` - no metadata
        attribute needed) and the point data's metadata attributes. Unnamed attributes are filtered
        out. `attribute_stats` gives `min`/`max`/`mean` plus per-component `channels` (r/g/b/a for
        colours, pitch/yaw/roll for rotations, otherwise x/y/z/w) and up to `max_samples` samples;
        names are matched against the point fields first (case insensitive), then the metadata.
        This replaces "look at a screenshot" with numbers that can be asserted.

        Args:
            actor_label: Owning actor label (a unique substring is accepted).
            component_name: PCG component object name.
            attribute: Attribute to summarise, e.g. "Density".
            max_samples: Samples to return, 1..64 (default 8).
            include_attributes: List the available attribute names (default True).

        Returns:
            actor_label, component_name, world_name, tagged_count, total_points, tagged_data[]
            ({tags, data_type, pin, is_point_data, point_count}), attributes[] and - when an
            attribute was requested - attribute_stats.
            Errors: component_not_found (with candidates), not_generated, invalid_attribute
            (with candidates), no_editor_world. `not_generated` is reported instead of
            generating: generate the component first with your own call.
        """
        params: Dict[str, Any] = {"max_samples": max_samples, "include_attributes": include_attributes}
        if actor_label is not None:
            params["actor_label"] = actor_label
        if component_name is not None:
            params["component_name"] = component_name
        if attribute is not None:
            params["attribute"] = attribute

        response = _send("get_pcg_generated_output", params)
        if not response:
            return {"success": False, "error_code": "no_connection",
                    "error": "Failed to forward get_pcg_generated_output to the editor"}
        return response

    @mcp.tool()
    def add_pcg_node(ctx: Context, asset_path: str, node_class: str,
                     editor_x: Optional[int] = None,
                     editor_y: Optional[int] = None) -> Dict[str, Any]:
        """
        Add a node to a PCG graph by settings class (writes the graph asset).

        `node_class` is the settings class name ("PCGCreatePointsSphereSettings") or an object path;
        unknown names fail with `unknown_node_class` plus candidates. The node gets its own engine
        name (e.g. "TransformPoints_3"), which is the key every other PCG tool addresses it by; the
        response reports it together with the node's real pins and position. When neither `editor_x`
        nor `editor_y` is given the node keeps the position the graph API assigned it.

        The write is transactional and the asset is saved on success (`saved`). Nothing is exported
        as a visual check: read the node back with get_pcg_node.

        Args:
            asset_path: PCG graph asset path.
            node_class: Settings class short name or object path.
            editor_x: Optional node position X in the graph editor.
            editor_y: Optional node position Y in the graph editor.

        Returns:
            node_name, settings_class, in_graph, editor_x, editor_y, input_pins, output_pins,
            node_count, saved.
            Errors: asset_not_found, not_a_pcg_graph, unknown_node_class (with candidates),
            node_create_failed.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "node_class": node_class}
        if editor_x is not None:
            params["editor_x"] = editor_x
        if editor_y is not None:
            params["editor_y"] = editor_y

        response = _send("add_pcg_node", params)
        if not response:
            return {"success": False, "error_code": "no_connection",
                    "error": "Failed to forward add_pcg_node to the editor"}
        return response

    @mcp.tool()
    def connect_pcg_pins(ctx: Context, asset_path: str, from_node: str, from_label: str,
                         to_node: str, to_label: str) -> Dict[str, Any]:
        """
        Connect two PCG pins by display label and read the connection back (writes the graph asset).

        Labels are the ones get_pcg_graph shows ("Out", "In", "Surface", "Overrides"); the
        snake_case spelling is accepted too. This exists because the engine's connect call is a
        *silent* no-op when a label does not match: a bad label is reported here as `pin_not_connected`
        with the candidate labels of both nodes instead of looking like success. The response reports
        `edges_before`/`edges_after` for the input pin, because a single-input pin replaces what was
        connected before rather than adding to it.

        Args:
            asset_path: PCG graph asset path.
            from_node: Upstream node object name (from get_pcg_graph).
            from_label: Upstream pin display label.
            to_node: Downstream node object name.
            to_label: Downstream pin display label.

        Returns:
            connected, edge{from_node, from_label, to_node, to_label}, edges_before, edges_after,
            saved.
            Errors: asset_not_found, not_a_pcg_graph, node_not_found, pin_not_found (with
            candidates), pin_not_connected (with the candidate labels of both pin sides).
        """
        params: Dict[str, Any] = {
            "asset_path": asset_path,
            "from_node": from_node,
            "from_label": from_label,
            "to_node": to_node,
            "to_label": to_label,
        }

        response = _send("connect_pcg_pins", params)
        if not response:
            return {"success": False, "error_code": "no_connection",
                    "error": "Failed to forward connect_pcg_pins to the editor"}
        return response

    @mcp.tool()
    def disconnect_pcg_pins(ctx: Context, asset_path: str, from_node: str, from_label: str,
                            to_node: str, to_label: str) -> Dict[str, Any]:
        """
        Remove one PCG edge by its two endpoints (writes the graph asset).

        Both endpoints must be spelled exactly as they are connected, labels included. If no such
        edge exists the call fails with `edge_not_found` and lists the edges that pin *does* have,
        so a stale assumption is visible instead of silently doing nothing.

        Args:
            asset_path: PCG graph asset path.
            from_node: Upstream node object name.
            from_label: Upstream pin display label.
            to_node: Downstream node object name.
            to_label: Downstream pin display label.

        Returns:
            removed, edges_before, edges_after, saved.
            Errors: asset_not_found, not_a_pcg_graph, node_not_found, pin_not_found,
            edge_not_found (with the existing edges of that pin as candidates).
        """
        params: Dict[str, Any] = {
            "asset_path": asset_path,
            "from_node": from_node,
            "from_label": from_label,
            "to_node": to_node,
            "to_label": to_label,
        }

        response = _send("disconnect_pcg_pins", params)
        if not response:
            return {"success": False, "error_code": "no_connection",
                    "error": "Failed to forward disconnect_pcg_pins to the editor"}
        return response

    @mcp.tool()
    def remove_pcg_node(ctx: Context, asset_path: str, node_name: str) -> Dict[str, Any]:
        """
        Remove one node from a PCG graph (writes the graph asset).

        The node's edges go with it and nothing is rewired automatically - reconnect explicitly with
        connect_pcg_pins when a chain has to be preserved. The graph's input and output nodes cannot
        be removed (`cannot_remove_graph_io_nodes`).

        Args:
            asset_path: PCG graph asset path.
            node_name: Node object name (from get_pcg_graph).

        Returns:
            removed_name, node_count (after the removal), saved.
            Errors: asset_not_found, not_a_pcg_graph, node_not_found (with candidates),
            cannot_remove_graph_io_nodes, remove_failed.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "node_name": node_name}

        response = _send("remove_pcg_node", params)
        if not response:
            return {"success": False, "error_code": "no_connection",
                    "error": "Failed to forward remove_pcg_node to the editor"}
        return response

    @mcp.tool()
    def set_pcg_node_property(ctx: Context, asset_path: str, property_name: str, value: Any,
                              node_name: Optional[str] = None,
                              node_index: Optional[int] = None) -> Dict[str, Any]:
        """
        Write one PCG settings property and read it back (writes the graph asset).

        `property_name` accepts both spellings get_pcg_node reports: the reflection name
        ("LowerBound") and the snake_case one ("lower_bound"). `value` must be in the shape that
        table shows for the property (number / bool / string / enum member name / array / object).

        The write goes through the same property writer the rest of the plugin uses and is verified
        by reading the property back inside the transaction:

        * the response carries `value_before`, `value_after`, `changed` and `verified`;
        * the write notifies the engine (`notified`), which is what invalidates PCG's compiled-graph
          cache: without it the next generation re-runs the graph but keeps using the old value;
        * if the readback does not match what was asked for, the call fails with
          `readback_mismatch`, the property is put back to its previous value (`reverted`) and the
          asset is left unsaved - this is how a float truncated into an int32 property becomes
          visible instead of silently landing on 0;
        * a property the read table marks `writable: false` (instanced subobjects such as
          MeshSelectorParameters, edit-const engine state) is refused with `property_not_writable`
          and the same hint, without attempting the assignment.

        Writing a property does not necessarily regenerate anything: trigger
        generate_pcg_component and read the result afterwards.

        Args:
            asset_path: PCG graph asset path.
            property_name: Reflection name or snake_case name of the property.
            value: Value in the shape get_pcg_node reports for that property.
            node_name: Node object name (from get_pcg_graph).
            node_index: Node index, used when node_name is absent.

        Returns:
            node_name, settings_class, property_name, python_name, property_type, value_before,
            value_after, value_shape, changed, verified, notified, saved (plus `readback_note` when
            the property type has no JSON readback).
            Errors: asset_not_found, not_a_pcg_graph, node_not_found, node_has_no_settings,
            unknown_property (with candidates), property_not_writable (with hint), readback_mismatch
            (with requested_value / readback_value / reverted), and the property writer's own codes
            (type_mismatch / write_failed / ...) with supported_shapes and available_fields.
        """
        params: Dict[str, Any] = {
            "asset_path": asset_path,
            "property_name": property_name,
            "value": value,
        }
        if node_name is not None:
            params["node_name"] = node_name
        if node_index is not None:
            params["node_index"] = node_index

        response = _send("set_pcg_node_property", params)
        if not response:
            return {"success": False, "error_code": "no_connection",
                    "error": "Failed to forward set_pcg_node_property to the editor"}
        return response

    @mcp.tool()
    def generate_pcg_component(ctx: Context, actor_label: Optional[str] = None,
                               component_name: Optional[str] = None,
                               force: bool = False) -> Dict[str, Any]:
        """
        Dispatch generation for one PCG component (asynchronous - this call does not wait).

        Generation runs through PCG's task graph, so the honest answer at this point is the
        component's state: `dispatched` says the call was made, `generating` / `active` /
        `generated_output_available` describe where it stands right now. Read the result with
        get_pcg_generated_output (and list_pcg_components for the instance batches) once it lands -
        never by sleeping inside a script.

        A component whose generation is owned by the runtime generation system is not generated from
        here (`runtime_generation_component`).

        Args:
            actor_label: Owning actor label (a unique substring is accepted).
            component_name: PCG component object name.
            force: Force regeneration even when the component is up to date.

        Returns:
            actor_label, component_name, graph_path, generation_trigger, dispatched, active,
            generating, generated_output_available.
            Errors: component_not_found (with candidates), no_editor_world,
            runtime_generation_component.
        """
        params: Dict[str, Any] = {"force": force}
        if actor_label is not None:
            params["actor_label"] = actor_label
        if component_name is not None:
            params["component_name"] = component_name

        response = _send("generate_pcg_component", params)
        if not response:
            return {"success": False, "error_code": "no_connection",
                    "error": "Failed to forward generate_pcg_component to the editor"}
        return response

    @mcp.tool()
    def cleanup_pcg_component(ctx: Context, actor_label: Optional[str] = None,
                              component_name: Optional[str] = None,
                              remove_components: bool = True,
                              save_generated_components: bool = False) -> Dict[str, Any]:
        """
        Dispatch cleanup for one PCG component (asynchronous, removes what generation produced).

        The graph asset is not touched, so this call does not save anything: `cleanup_dispatched`
        confirms the dispatch and `generating` / `generated_output_available` report the state at
        that instant. Use list_pcg_components afterwards to see the instance batches disappear.

        Args:
            actor_label: Owning actor label (a unique substring is accepted).
            component_name: PCG component object name.
            remove_components: Remove the generated components too (default True).
            save_generated_components: Save the generated components while cleaning up (default False).

        Returns:
            actor_label, component_name, graph_path, cleanup_dispatched, remove_components,
            generating, generated_output_available.
            Errors: component_not_found (with candidates), no_editor_world,
            runtime_generation_component.
        """
        params: Dict[str, Any] = {
            "remove_components": remove_components,
            "save_generated_components": save_generated_components,
        }
        if actor_label is not None:
            params["actor_label"] = actor_label
        if component_name is not None:
            params["component_name"] = component_name

        response = _send("cleanup_pcg_component", params)
        if not response:
            return {"success": False, "error_code": "no_connection",
                    "error": "Failed to forward cleanup_pcg_component to the editor"}
        return response

    @mcp.tool()
    def create_pcg_graph(ctx: Context, asset_path: str,
                         folder: Optional[str] = None) -> Dict[str, Any]:
        """
        Create a PCG graph asset and read its structure back (writes the asset to disk).

        `asset_path` is the full path including the asset name ("/Game/MCP/_PCGProbe/PCG_New");
        `folder` is only a fallback for callers that pass the bare name. An existing asset at that
        path is never overwritten - the call fails with `asset_exists` (delete it with
        safe_delete_asset or pick another name).

        The response reports the new graph's structure through the same readers the query tools use,
        including the names of its input and output nodes. A fresh graph has no chain: connect the
        nodes you add to `output_node`, because a graph that never reaches the output node generates
        successfully with an empty result.

        Args:
            asset_path: Full asset path (folder + name) of the graph to create.
            folder: Folder to use when asset_path carries the name only.

        Returns:
            asset_path, graph_class, node_count, input_node, output_node, saved.
            Errors: invalid_asset_path, asset_exists (with existing_assets and hint), create_failed.
        """
        params: Dict[str, Any] = {"asset_path": asset_path}
        if folder is not None:
            params["folder"] = folder

        response = _send("create_pcg_graph", params)
        if not response:
            return {"success": False, "error_code": "no_connection",
                    "error": "Failed to forward create_pcg_graph to the editor"}
        return response

    @mcp.tool()
    def set_pcg_component_graph(ctx: Context, graph_path: str,
                                actor_label: Optional[str] = None,
                                component_name: Optional[str] = None,
                                active: Optional[bool] = None) -> Dict[str, Any]:
        """
        Bind a PCG graph (or graph instance) asset to a PCG component in the level.

        `graph_path` accepts a `PCGGraph` or a `PCGGraphInstance` asset. `active` also sets the
        component's active state - a component that is not active generates nothing, which is why
        binding and activating in one call is usually what you want.

        This command only changes the component: it does not save the level (the response reports
        `level_package_dirty`, saving is your call) and it does not generate. The binding is read back
        from the component with the same reader `list_pcg_components` uses, so a binding that did not
        take is reported as `readback_mismatch` instead of looking successful. Trigger
        generate_pcg_component afterwards and read the result with the query tools.

        Args:
            graph_path: PCGGraph or PCGGraphInstance asset path.
            actor_label: Owning actor label (a unique substring is accepted).
            component_name: PCG component object name.
            active: Optionally set the component's active state.

        Returns:
            actor_label, component_name, requested_graph_path, graph_interface_path (the interface the
            component actually holds), graph_path (the effective graph), graph_instance_path, active,
            generation_trigger, generating, generated_output_available, level_package_dirty.
            Errors: asset_not_found, not_a_pcg_graph_interface (with the actual class),
            component_not_found (with candidates), no_editor_world, readback_mismatch.
        """
        params: Dict[str, Any] = {"graph_path": graph_path}
        if actor_label is not None:
            params["actor_label"] = actor_label
        if component_name is not None:
            params["component_name"] = component_name
        if active is not None:
            params["active"] = active

        response = _send("set_pcg_component_graph", params)
        if not response:
            return {"success": False, "error_code": "no_connection",
                    "error": "Failed to forward set_pcg_component_graph to the editor"}
        return response

    @mcp.tool()
    def set_pcg_mesh_selector_type(ctx: Context, asset_path: str, selector_class: str,
                                   node_name: Optional[str] = None,
                                   node_index: Optional[int] = None) -> Dict[str, Any]:
        """
        Set the mesh selector class on a static mesh spawner node (writes the graph asset).

        This is how you *change* the selector class: `selector_class` accepts the class name
        ("PCGMeshSelectorWeighted"), an object path, or the shorthand "weighted" / "by_attribute" /
        "weighted_by_category". A fresh spawner node already carries the engine's default weighted
        selector instance, so this call is only needed when you want a different class (or after the
        instance went missing - writing entries then reports `selector_not_instantiated`).

        The response proves the instance exists (`selector_instantiated`) and gives its object path -
        the same path `get_pcg_node` reports as the value of `MeshSelectorParameters`. If the instance
        is missing the call fails with `readback_mismatch` instead of reporting a type change that
        cannot be written to.

        Args:
            asset_path: PCG graph asset path.
            selector_class: Class name, object path or shorthand.
            node_name: Node object name (from get_pcg_graph).
            node_index: Node index, used when node_name is absent.

        Returns:
            node_name, selector_class, selector_parameters_path, selector_parameters_class,
            selector_instantiated, notified, saved.
            Errors: asset_not_found, not_a_pcg_graph, node_not_found, not_a_static_mesh_spawner
            (with the actual settings class), unknown_selector_class (with candidates),
            readback_mismatch.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "selector_class": selector_class}
        if node_name is not None:
            params["node_name"] = node_name
        if node_index is not None:
            params["node_index"] = node_index

        response = _send("set_pcg_mesh_selector_type", params)
        if not response:
            return {"success": False, "error_code": "no_connection",
                    "error": "Failed to forward set_pcg_mesh_selector_type to the editor"}
        return response

    @mcp.tool()
    def set_pcg_mesh_selector_entries(ctx: Context, asset_path: str, entries: List[Dict[str, Any]],
                                      node_name: Optional[str] = None,
                                      node_index: Optional[int] = None) -> Dict[str, Any]:
        """
        Replace the weighted mesh selector's mesh table (writes the graph asset).

        `entries` REPLACES the whole table - each item is
        `{"static_mesh": <path>, "weight": <int, default 1>, "override_materials": [<path>, ...]}`.
        A fresh spawner node already has the engine's default weighted selector, so this works without
        calling set_pcg_mesh_selector_type first; that call is only for changing the selector class
        (or restoring a missing instance, which is what `selector_not_instantiated` reports).

        Everything is validated before anything is written, and the traps that used to be silent are
        errors here:

        * `weight` must be an integer - a fraction (0.7) is rejected as `invalid_weight`, because the
          engine field is an int32 and the truncated 0 would leave that mesh never selected while the
          spawner quietly emits nothing;
        * `static_mesh` must resolve to a real UStaticMesh (`unknown_mesh`), and materials to
          UMaterialInterface (`unknown_material`);
        * a selector that is not PCGMeshSelectorWeighted is refused (`unsupported_selector`) rather
          than guessed at;
        * the write notifies the engine (`notified`), which is what invalidates PCG's compiled-graph
          cache - without it the next generation would keep using the previous table.

        Args:
            asset_path: PCG graph asset path.
            entries: Replacement table: [{static_mesh, weight, override_materials?}].
            node_name: Node object name (from get_pcg_graph).
            node_index: Node index, used when node_name is absent.

        Returns:
            node_name, selector_class, selector_parameters_path, entry_count, entries[] (same shape
            as the request, plus display_name), notified, saved.
            Errors: asset_not_found, not_a_pcg_graph, node_not_found, not_a_static_mesh_spawner,
            selector_not_instantiated, unsupported_selector, invalid_entry, invalid_weight,
            unknown_mesh, unknown_material, readback_mismatch.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "entries": entries}
        if node_name is not None:
            params["node_name"] = node_name
        if node_index is not None:
            params["node_index"] = node_index

        response = _send("set_pcg_mesh_selector_entries", params)
        if not response:
            return {"success": False, "error_code": "no_connection",
                    "error": "Failed to forward set_pcg_mesh_selector_entries to the editor"}
        return response
