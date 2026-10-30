"""
Niagara tools for Unreal MCP - system / emitter / stack / scratch-pad authoring.

The editor python layer cannot author Niagara: `unreal.NiagaraSystem` exposes no stack editing, the
Map Get / Map Set nodes that carry a module's parameter-map wiring live in NiagaraEditor's private
headers, and a scratch-pad module's HLSL is only reachable through the node's own property. These
tools forward to the same-named bridge commands, which go through the engine's exported stack
utilities (FNiagaraStackGraphUtilities) and read every result back:

    create_niagara_system              create a system asset (engine default graphs included)
    duplicate_niagara_system           copy a system to another content path
    list_niagara_emitters              handles of a system: id, name, enabled, sim target, stage count
    add_niagara_emitter                add an emitter, from a template asset or empty
    remove_niagara_emitter             remove an emitter by handle id or name
    set_niagara_emitter_properties     sim target / local space / determinism / pool size
    set_niagara_renderer_material      bind a material to one of the emitter's renderers
    add_niagara_simulation_stage       add a GPU iteration stage (stage + script + graph output)
    compile_niagara_system             compile and read per-script errors/warnings back
    get_niagara_parameters             a system's user parameters with type and current value
    set_niagara_parameter              write one user parameter and read it back
    list_niagara_modules               stack modules of an emitter (or system) stage
    add_niagara_module                 add a module script to a stage
    set_niagara_module_enabled         enable / disable one module
    set_niagara_module_input           write one module input (the stack UI's rapid-iteration write)
    remove_niagara_module              remove a module plus the override nodes the stack built for it
    create_niagara_scratch_pad_module  create a scratch-pad module on an emitter stage
    list_niagara_scratch_pad_nodes     dump a scratch graph: nodes and every node's pins
    set_niagara_custom_hlsl            write HLSL into a Custom HLSL node (creates the node if needed)
    set_niagara_scratch_pad_pins       declare module inputs/outputs, or typed pins on one node
    remove_niagara_scratch_pad_node    remove a node from a scratch graph (input/output node protected)
    remove_niagara_scratch_pad_pin     remove a pin; a Custom HLSL node's signature is rebuilt
    connect_niagara_scratch_pad_pins   wire two pins in a scratch graph
    spawn_niagara_actor                spawn an ANiagaraActor for a system into the editor world
    set_niagara_component_parameter    override one user parameter on that actor's component

Contract:
  * every write command is one undo step and saves the asset on success (`saved`); `persist=False`
    batches writes and lets the caller flush once;
  * writes are read back from the graph: a module reports its real node guid and name, HLSL reports
    the code that came back out of the node, pins report their namespaced names;
  * compile_niagara_system reports per-script status plus `errors` / `warnings` with script path and
    node guid - a scratch graph that does not compile says which script and which node;
  * `list_niagara_scratch_pad_nodes` also returns `pins` (every pin of every node, with its type), so
    a wiring call can be built from real names instead of guesses;
  * module stages are checked, not assumed: SpawnRate belongs to EmitterUpdate. A mis-staged module
    compiles to an invalid system with only "System is invalid after compilation" as the message;
  * `spawn_niagara_actor` changes the level and deliberately leaves it unsaved.

Two naming rules the bridge normalizes:
  * user parameters: "Gravity" and "User.Gravity" are the same parameter;
  * scratch-pad pins: a module input "Radius" becomes the pin "Module.Radius" on the Map Get, a module
    output "Height" becomes "Particles.Height" on the Map Set - pass the short name, read the full one.
"""

import logging
from typing import Any, Dict, List, Optional
from mcp.server.fastmcp import FastMCP, Context

from tools._bridge import MCPToolError, _bridge

logger = logging.getLogger("UnrealMCP")


def register_niagara_tools(mcp: FastMCP):
    """Register the Niagara tools with the MCP server."""

    def _send(command: str, params: Dict[str, Any]) -> Dict[str, Any]:
        """Forward one command to the bridge; raises when it did not succeed."""
        return _bridge(command, params)

    # ------------------------------------------------------------------
    # Assets
    # ------------------------------------------------------------------

    @mcp.tool()
    def create_niagara_system(ctx: Context, name: str, folder: str = "/Game/VFX",
                              persist: bool = True) -> Dict[str, Any]:
        """
        Create a Niagara system asset.

        The system is created through the engine's own initialisation, so it has its spawn/update
        graphs, output nodes and the SystemUpdate module - a system built by hand would not compile.

        Args:
            name: Asset name (no path).
            folder: Content folder; default "/Game/VFX".
            persist: Save after the command; pass False to batch writes and flush once yourself.

        Returns:
            asset_path (the created system), system_path (the same path), saved.
            Errors: asset_exists (with the class of the existing asset).
        """
        params: Dict[str, Any] = {"name": name, "folder": folder, "persist": persist}
        return _send("create_niagara_system", params)

    @mcp.tool()
    def duplicate_niagara_system(ctx: Context, source_path: str, destination_path: Optional[str] = None,
                                 persist: bool = True) -> Dict[str, Any]:
        """
        Duplicate a Niagara system to another content path.

        Args:
            source_path: System asset to copy.
            destination_path: Full target path; default "<source>_Copy" next to the source.
            persist: Save after the command.

        Returns:
            asset_path (the copy), source_path, saved.
        """
        params: Dict[str, Any] = {"source_path": source_path, "persist": persist}
        if destination_path is not None:
            params["destination_path"] = destination_path
        return _send("duplicate_niagara_system", params)

    # ------------------------------------------------------------------
    # Emitters
    # ------------------------------------------------------------------

    @mcp.tool()
    def list_niagara_emitters(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        List the emitter handles of a system (read-only).

        Args:
            asset_path: System asset path.

        Returns:
            emitter_count and emitters[{id, name, enabled, emitter_path, sim_target,
            simulation_stage_count}]. `id` is what the other tools accept as `emitter_name`.
        """
        return _send("list_niagara_emitters", {"asset_path": asset_path})

    @mcp.tool()
    def add_niagara_emitter(ctx: Context, asset_path: str, emitter_name: Optional[str] = None,
                            source_emitter_path: Optional[str] = None, add_default_modules: bool = False,
                            persist: bool = True) -> Dict[str, Any]:
        """
        Add an emitter to a system.

        Args:
            asset_path: System asset path.
            emitter_name: Name for the new handle; default "Emitter". A taken name is uniquified.
            source_emitter_path: Emitter asset to copy (a template - the copy lands in the system);
                omit it to create an empty emitter.
            add_default_modules: For an empty emitter, also add the engine's default modules and
                renderers (a renderer of some kind is what makes the emitter visible).
            persist: Save after the command.

        Returns:
            emitter_id, emitter_name (both read back from the new handle), asset_path, saved.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "add_default_modules": add_default_modules,
                                  "persist": persist}
        if emitter_name is not None:
            params["emitter_name"] = emitter_name
        if source_emitter_path is not None:
            params["source_emitter_path"] = source_emitter_path
        return _send("add_niagara_emitter", params)

    @mcp.tool()
    def remove_niagara_emitter(ctx: Context, asset_path: str, emitter_name: str,
                               persist: bool = True) -> Dict[str, Any]:
        """
        Remove an emitter from a system.

        Args:
            asset_path: System asset path.
            emitter_name: Emitter handle id or emitter name.
            persist: Save after the command.

        Returns:
            emitter_id, emitter_name of the removed emitter, saved.
            Errors: emitter_not_found (with the emitter names as candidates).
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "emitter_name": emitter_name,
                                  "persist": persist}
        return _send("remove_niagara_emitter", params)

    @mcp.tool()
    def set_niagara_emitter_properties(ctx: Context, asset_path: str, emitter_name: str,
                                       sim_target: Optional[str] = None,
                                       local_space: Optional[bool] = None,
                                       determinism: Optional[bool] = None,
                                       random_seed: Optional[int] = None,
                                       interpolated_spawning: Optional[bool] = None,
                                       pre_allocation_count: Optional[int] = None,
                                       persist: bool = True) -> Dict[str, Any]:
        """
        Change simulator properties of an emitter, and read the emitter back.

        Args:
            asset_path: System asset path.
            emitter_name: Emitter handle id or name.
            sim_target: "CPUSim" / "GPUComputeSim". A Grid2D / RenderTarget effect needs
                GPUComputeSim - the default CPU emitter refuses those data interfaces.
            local_space: Simulate in emitter space instead of world space.
            determinism: Deterministic simulation.
            random_seed: Seed for determinism.
            interpolated_spawning: Use the interpolated spawning script.
            pre_allocation_count: Particles allocated up front.
            persist: Save after the command.

        Returns:
            emitter (the full readback: sim_target, local_space, determinism, random_seed,
            interpolated_spawning, pre_allocation_count, simulation_stage_count, renderers), saved.
            Errors: emitter_not_found, emitter_data_missing, unknown_sim_target (candidates = legal values).
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "emitter_name": emitter_name,
                                  "persist": persist}
        for key, value in (("sim_target", sim_target), ("local_space", local_space),
                           ("determinism", determinism), ("random_seed", random_seed),
                           ("interpolated_spawning", interpolated_spawning),
                           ("pre_allocation_count", pre_allocation_count)):
            if value is not None:
                params[key] = value
        return _send("set_niagara_emitter_properties", params)

    @mcp.tool()
    def set_niagara_renderer_material(ctx: Context, asset_path: str, emitter_name: str,
                                      material_path: Optional[str] = None, renderer_index: int = 0,
                                      persist: bool = True) -> Dict[str, Any]:
        """
        Bind a material to one of an emitter's renderers (or clear it).

        The material property is found by shape (the renderer's editable material-typed property), so
        this works for Sprite, Mesh, Ribbon and Decal renderers without a name table.

        Args:
            asset_path: System asset path.
            emitter_name: Emitter handle id or name.
            material_path: Material or material instance; omit it to clear the binding.
            renderer_index: Renderer to change; default 0. `list_niagara_emitters` reports the classes.
            persist: Save after the command.

        Returns:
            emitter_name, material_property (which property was written), material (read back off the
            renderer), emitter (full readback), saved.
            Errors: renderer_not_found (candidates = "index: class"), material_property_not_found,
            material_not_found.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "emitter_name": emitter_name,
                                  "renderer_index": renderer_index, "persist": persist}
        if material_path is not None:
            params["material_path"] = material_path
        return _send("set_niagara_renderer_material", params)

    @mcp.tool()
    def add_niagara_renderer(ctx: Context, asset_path: str, emitter_name: str, renderer_type: str,
                             material_path: Optional[str] = None,
                             persist: bool = True) -> Dict[str, Any]:
        """
        Add a renderer to an emitter, optionally binding a material in the same write.

        The renderer array is private to the emitter, so the object is created and handed to
        UNiagaraEmitter::AddRenderer. A renderer that is never added is never drawn: this is what makes
        anything a solver computed visible at all.

        Args:
            asset_path: System asset path.
            emitter_name: Emitter handle id or name.
            renderer_type: Sprite / Mesh / Ribbon / Light / Decal / Component, or a full renderer class
                name (e.g. "NiagaraSpriteRendererProperties").
            material_path: Material or material instance to bind; omit it to add a renderer with no
                material and bind later with set_niagara_renderer_material.
            persist: Save after the command.

        Returns:
            emitter_name, renderer_class, renderer_index, material_path (read back off the renderer),
            renderers (every renderer of the emitter afterwards, in order), saved.
            Errors: emitter_not_found (candidates = emitter names), unknown_renderer_type (candidates =
            renderer classes), create_failed, load_failed, material_property_not_found.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "emitter_name": emitter_name,
                                  "renderer_type": renderer_type, "persist": persist}
        if material_path is not None:
            params["material_path"] = material_path
        return _send("add_niagara_renderer", params)

    @mcp.tool()
    def remove_niagara_renderer(ctx: Context, asset_path: str, emitter_name: str,
                                renderer_index: int = 0, persist: bool = True) -> Dict[str, Any]:
        """
        Remove one of an emitter's renderers by index (the order list_niagara_emitters reports).

        Args:
            asset_path: System asset path.
            emitter_name: Emitter handle id or name.
            renderer_index: Renderer to remove; default 0.
            persist: Save after the command.

        Returns:
            emitter_name, removed_renderer_class, renderers (the ones left, in order), saved.
            Errors: emitter_not_found, renderer_not_found (candidates = "index: class"),
            emitter_data_missing.
        """
        return _send("remove_niagara_renderer",
                     {"asset_path": asset_path, "emitter_name": emitter_name,
                      "renderer_index": renderer_index, "persist": persist})

    @mcp.tool()
    def remove_niagara_module(ctx: Context, asset_path: str, module: str,
                              emitter_name: Optional[str] = None, persist: bool = True) -> Dict[str, Any]:
        """
        Remove a module from a stage, with the parameter-map override nodes the stack created for it.

        The engine's own removal (FNiagaraStackGraphUtilities::RemoveModuleFromStack) is not exported,
        so the nodes are identified here by walking the stage's parameter-map chain backwards and only
        taking an override node whose pins all name this module. `removed_nodes` reports exactly what
        went, and the response carries the stage as it is afterwards plus any compile errors.

        Args:
            asset_path: System asset path.
            module: Module - node guid (from list_niagara_modules) or its display name.
            emitter_name: Emitter handle id or name; omit it to target the system stack.
            persist: Save after the command.

        Returns:
            removed_module_guid, removed_module_name, removed_nodes (type + guid of every node taken),
            modules (the stage after the removal), errors (compile diagnostics), saved.
            Errors: module_not_found, module_not_connected (not wired into a stage chain),
            graph_unavailable.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "module": module, "persist": persist}
        if emitter_name is not None:
            params["emitter_name"] = emitter_name
        return _send("remove_niagara_module", params)

    @mcp.tool()
    def add_niagara_simulation_stage(ctx: Context, asset_path: str, emitter_name: str,
                                     stage_name: Optional[str] = None,
                                     iteration_source: Optional[str] = None,
                                     enabled: bool = True, persist: bool = True) -> Dict[str, Any]:
        """
        Add a simulation stage (GPU iteration) to an emitter.

        A stage runs a script over the particles or over a data interface (a Grid2D write, a PBF solve,
        a shallow-water sweep). It needs a GPU emitter to be useful: set `sim_target="GPUComputeSim"`
        with set_niagara_emitter_properties. The stage's script, its usage id and the stage's graph
        output are all created here; the stack item appears when the asset editor is (re)opened.

        Args:
            asset_path: System asset path.
            emitter_name: Emitter handle name.
            stage_name: Stage name; default "Simulation Stage".
            iteration_source: "Particles" / "DataInterface" / "DirectSet"; default Particles.
            enabled: Enable the stage; default True.
            persist: Save after the command.

        Returns:
            stage_name, iteration_source, enabled, script_path (the stage's script),
            emitter (full readback, including simulation_stage_names), saved.
            Errors: emitter_not_found, emitter_data_missing, graph_unavailable,
            unknown_iteration_source (candidates = legal values), simulation_stages_unavailable.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "emitter_name": emitter_name,
                                  "enabled": enabled, "persist": persist}
        if stage_name is not None:
            params["stage_name"] = stage_name
        if iteration_source is not None:
            params["iteration_source"] = iteration_source
        return _send("add_niagara_simulation_stage", params)

    @mcp.tool()
    def set_niagara_simulation_stage(ctx: Context, asset_path: str, emitter_name: str, stage: str,
                                     stage_name: Optional[str] = None, enabled: Optional[bool] = None,
                                     iteration_source: Optional[str] = None,
                                     num_iterations: Optional[int] = None,
                                     data_interface_parameter: Optional[str] = None,
                                     persist: bool = True) -> Dict[str, Any]:
        """
        Configure an existing simulation stage.

        Two things here need a Niagara-aware command rather than the generic property writer:
        `enabled` is a bitfield (reflect_probe reports it read-only) and `num_iterations` keeps its
        number in a raw byte blob. `iteration_source` and `data_interface_parameter` are semantic:
        the parameter is resolved to a real DataInterface user parameter instead of a hand-built
        variable, and binding one switches the source to DataInterface when no source was given.

        Args:
            asset_path: System asset path.
            emitter_name: Emitter handle id or name.
            stage: Stage name as `list_niagara_emitters` reports it, or its index ("0").
            stage_name: Rename the stage.
            enabled: Enable or disable it.
            iteration_source: "Particles" / "DataInterface" / "DirectSet".
            num_iterations: Iteration count (meaningful for DirectSet).
            data_interface_parameter: DataInterface user parameter to iterate over, e.g. "User.WaterGrid".
            persist: Save after the command.

        Returns:
            stage_name, enabled, iteration_source, num_iterations, data_interface_parameter,
            emitter (full readback), saved.
            Errors: emitter_not_found, stage_not_found (candidates = "index: name"),
            unknown_iteration_source, unsupported_stage_type, unsupported_iteration_value_size,
            parameter_not_found, not_a_data_interface_parameter.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "emitter_name": emitter_name,
                                  "stage": stage, "persist": persist}
        for key, value in (("stage_name", stage_name), ("enabled", enabled),
                           ("iteration_source", iteration_source), ("num_iterations", num_iterations),
                           ("data_interface_parameter", data_interface_parameter)):
            if value is not None:
                params[key] = value
        return _send("set_niagara_simulation_stage", params)

    # ------------------------------------------------------------------
    # Compile
    # ------------------------------------------------------------------

    @mcp.tool()
    def compile_niagara_system(ctx: Context, asset_path: str, wait: bool = True,
                               persist: bool = True) -> Dict[str, Any]:
        """
        Compile a system and read the diagnostics back.

        Args:
            asset_path: System asset path.
            wait: Block until the compile finished (default True). False polls once.
            persist: Save after the command.

        Returns:
            compile_status ("success" / "error"), script_count, scripts[{script_path, usage, status}],
            errors[{severity, script_path, message, node_guid}], warnings[...].
            A scratch-graph failure names the script and the node guid it came from.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "wait": wait, "persist": persist}
        return _send("compile_niagara_system", params)

    # ------------------------------------------------------------------
    # Parameters
    # ------------------------------------------------------------------

    @mcp.tool()
    def get_niagara_parameters(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Read a system's user parameters (read-only).

        Args:
            asset_path: System asset path.

        Returns:
            parameter_count and parameters[{name, type, value}]. `name` is namespaced ("User.Gravity");
            `value` uses the engine's export form ("(X=0,Y=0,Z=-980)"), which
            set_niagara_parameter accepts as-is.
        """
        return _send("get_niagara_parameters", {"asset_path": asset_path})

    @mcp.tool()
    def set_niagara_parameter(ctx: Context, asset_path: str, param_name: str, value: str,
                              param_type: Optional[str] = None, add_if_missing: bool = True,
                              persist: bool = True) -> Dict[str, Any]:
        """
        Write one user parameter of a system and read it back.

        Object-valued parameters are written here too: give an asset path as the value and the class as
        `param_type` ("TextureRenderTarget2D", "Texture2D", "StaticMesh"). That is how a Grid2DCollection
        gets its output RenderTarget - `RenderTargetUserParameter` binds a user parameter of type
        UTextureRenderTarget, and it is the only path from a grid the solver writes to something a
        material can sample.

        Args:
            asset_path: System asset path.
            param_name: "Gravity" and "User.Gravity" are the same parameter.
            value: Text value: "12.5", "(X=0,Y=0,Z=-980)", "(R=1,G=0,B=0,A=1)", "true" - or an asset path
                for an object parameter ("" / "None" clears it).
            param_type: float / int / bool / vec2 / vec3 / vec4 / color / position / quat, a data
                interface class, or a UObject class. Taken from the existing parameter when omitted; a
                mismatch with the real type is an error.
            add_if_missing: Create the user parameter when it does not exist yet (default True).
            persist: Save after the command.

        Returns:
            param_name, param_type, value (read back from the store), saved.
            Errors: parameter_not_found (candidates = the system's parameters),
            parameter_type_mismatch, unsupported_parameter_type (a data interface parameter: those are
            configured with set_niagara_data_interface_properties), load_failed (no asset at the path).
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "param_name": param_name, "value": value,
                                  "add_if_missing": add_if_missing, "persist": persist}
        if param_type is not None:
            params["param_type"] = param_type
        return _send("set_niagara_parameter", params)

    @mcp.tool()
    def set_niagara_data_interface_properties(ctx: Context, asset_path: str, param_name: str,
                                              data_interface_type: Optional[str] = None,
                                              properties: Optional[Dict[str, Any]] = None,
                                              persist: bool = True) -> Dict[str, Any]:
        """
        Create a DataInterface user parameter, or configure one; with no `properties`, list what is settable.

        This is how a GPU simulation stage gets its surface: a Grid2DCollection user parameter that the
        stage's HLSL reads and writes. A DataInterface parameter is an object inside the system, so it
        is not writable through set_niagara_parameter - this command is.

        Args:
            asset_path: System asset path.
            param_name: User parameter name; "Grid" and "User.Grid" are the same parameter.
            data_interface_type: DataInterface class for a parameter that does not exist yet. A class
                name ("Grid2DCollection"), a partial name, or a full class path.
            properties: Values to write, e.g. {"ResolutionMaxAxis": 256, "CellSize": 12.5}. Omit it to
                read the DataInterface's editable property names (they are also the `candidates` of a
                `data_interface_property_not_found` error).
            persist: Save after the command.

        Returns:
            param_name, data_interface_class, data_interface_path, properties (name/type/value read back
            off the object), editable_properties, saved.
            Errors: parameter_not_found (candidates = the system's parameters),
            unknown_data_interface_type (candidates = matching classes),
            data_interface_property_not_found (candidates = editable properties),
            unsupported_property_type, not_a_data_interface_parameter, create_failed.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "param_name": param_name, "persist": persist}
        if data_interface_type is not None:
            params["data_interface_type"] = data_interface_type
        if properties is not None:
            params["properties"] = properties
        return _send("set_niagara_data_interface_properties", params)

    @mcp.tool()
    def list_niagara_grid_attributes(ctx: Context, asset_path: str, param_name: str) -> Dict[str, Any]:
        """
        Read a Grid2DCollection parameter's attribute table (read-only).

        A grid written into a RenderTarget lands there as one slice per attribute, in the order the
        engine's own FindAttributes discovers them - which is neither the user parameter list order nor
        the scratch-pad pin order. This is the only authoritative tile index <-> attribute name map:
        the RenderTarget tile an attribute is copied to is "first_slice - slot_count" (row-major, the
        same index the render-target tile readback uses). slot_count is the DI's NumAttributes and may
        be larger or smaller than attribute_count; 0 means the grid lays out no tiles at all.

        preview_attribute / preview_grid_enabled are the DI asset's editor preview settings
        (PreviewAttribute / bPreviewGrid), NOT a running instance's Vars. The runtime warning
        "Failed to map PreviewAttribute <name> to a grid index" comes from a different path, so an
        empty warnings[] here is not contradicted by it.

        Args:
            asset_path: System asset path.
            param_name: Grid2DCollection user parameter; "WaterGrid" and "User.WaterGrid" are the same.

        Returns:
            param_name, data_interface_class, attribute_count, slot_count, channel_count,
            preview_attribute, preview_grid_enabled, attributes[{name, type, first_slice, channels}]
            (RenderTarget tile = first_slice - slot_count) and warnings[].
            Errors: parameter_not_found (candidates = the system's parameters),
            not_a_grid_collection (the parameter is not a Grid2DCollection).
        """
        if not param_name:
            raise MCPToolError("list_niagara_grid_attributes", "invalid_params",
                               "Missing 'param_name' parameter")

        return _send("list_niagara_grid_attributes",
                     {"asset_path": asset_path, "param_name": param_name})

    # ------------------------------------------------------------------
    # Stack modules
    # ------------------------------------------------------------------

    @mcp.tool()
    def list_niagara_modules(ctx: Context, asset_path: str, emitter_name: Optional[str] = None,
                             stage: Optional[str] = None) -> Dict[str, Any]:
        """
        List the stack modules of a stage (read-only).

        Args:
            asset_path: System asset path.
            emitter_name: Emitter handle id or name; omit it to list the SYSTEM stack.
            stage: SystemSpawn / SystemUpdate / EmitterSpawn / EmitterUpdate / ParticleSpawn /
                ParticleUpdate, or a simulation stage of that emitter by name ("SWE Sweep") or
                index ("0"); omit it to list every stage.

        Returns:
            module_count and modules[{guid, name, stage, enabled, script_path, order, editor_x,
            editor_y}]. `order` is the position in the stage's evaluation order (read from the stage's
            parameter-map chain, not from X/Y layout); -1 when the node is not on a stage chain.
        """
        params: Dict[str, Any] = {"asset_path": asset_path}
        if emitter_name is not None:
            params["emitter_name"] = emitter_name
        if stage is not None:
            params["stage"] = stage
        return _send("list_niagara_modules", params)

    @mcp.tool()
    def add_niagara_module(ctx: Context, asset_path: str, module_asset_path: str, stage: str,
                           emitter_name: Optional[str] = None, insert_index: int = -1,
                           persist: bool = True) -> Dict[str, Any]:
        """
        Add a module script to one stage of an emitter (or of the system) stack.

        Stage matters: SpawnRate is an EmitterUpdate module, InitializeParticle is ParticleSpawn,
        ScaleColor / SolveForcesAndVelocity are ParticleUpdate. A mis-staged module compiles to an
        invalid system whose only message is "System is invalid after compilation".

        A simulation stage of the emitter is a stage too: pass its name ("SWE Sweep") or its index
        ("0") and the module lands on that stage's GPU iteration instead of on a fixed stage.

        Args:
            asset_path: System asset path.
            module_asset_path: e.g. "/Niagara/Modules/Emitter/SpawnRate.SpawnRate" - a Module script.
            stage: SystemSpawn / SystemUpdate / EmitterSpawn / EmitterUpdate / ParticleSpawn /
                ParticleUpdate, or a simulation stage of the emitter by name or index.
            emitter_name: Emitter handle id or name; omit it to target the system stack.
            insert_index: Position within the stage; -1 appends.
            persist: Save after the command.

        Returns:
            module_guid (the new node), module_name, script_path, saved.
            Errors: module_script_not_found, not_a_module_script, unknown_stage, stage_output_not_found.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "module_asset_path": module_asset_path,
                                  "stage": stage, "insert_index": insert_index, "persist": persist}
        if emitter_name is not None:
            params["emitter_name"] = emitter_name
        return _send("add_niagara_module", params)

    @mcp.tool()
    def set_niagara_module_enabled(ctx: Context, asset_path: str, module_guid: str, enabled: bool,
                                   emitter_name: Optional[str] = None,
                                   persist: bool = True) -> Dict[str, Any]:
        """
        Enable or disable one stack module.

        Args:
            asset_path: System asset path.
            module_guid: Node guid of the module (its display name also works).
            enabled: New state.
            emitter_name: Emitter handle id or name; omit it to target the system stack.
            persist: Save after the command.

        Returns:
            module_guid, module_name, enabled (read back from the node), saved.
            Errors: module_not_found (candidates = "name (guid)" of every module in that graph).
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "module_guid": module_guid,
                                  "enabled": enabled, "persist": persist}
        if emitter_name is not None:
            params["emitter_name"] = emitter_name
        return _send("set_niagara_module_enabled", params)

    @mcp.tool()
    def set_niagara_module_input(ctx: Context, asset_path: str, module: str, input_name: str, value: str,
                                 emitter_name: Optional[str] = None, param_type: Optional[str] = None,
                                 persist: bool = True) -> Dict[str, Any]:
        """
        Write one input of a stack module, and read it back.

        This is the stack UI's own write (the editor's FNiagaraStackFunctionInputBinder), so the value
        becomes the module's rapid iteration parameter. It is how a stock module is configured -
        SpawnRate, spawn counts, sizes, curve inputs - without touching the emitter asset.

        Enum inputs are written by member name or by number: `input_name="Loop Behavior"`,
        `value="Infinite"` (EmitterState). A name that is not a member comes back as
        `unknown_enum_member` with the legal members.

        Args:
            asset_path: System asset path.
            module: Module - node guid (from list_niagara_modules) or its display name.
            input_name: Input name as the stack shows it ("SpawnRate"), or "Module.SpawnRate".
            value: Text value: "25", "(X=0,Y=0,Z=-980)", "true", or an enum member name ("Infinite").
            emitter_name: Emitter handle id or name; omit it to target the system stack.
            param_type: Expected type; a mismatch with the input's real type is an error.
            persist: Save after the command.

        Returns:
            module_guid, module_name, input_name, input_type, value (read back out of the script's
            rapid iteration store), rapid_iteration_parameter (its full name), saved.
            Errors: module_not_found, module_input_not_found (candidates = the input names),
            stage_script_not_found, module_input_bind_failed, parameter_type_mismatch, write_failed,
            unknown_enum_member (candidates = the enum's members).
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "module": module,
                                  "input_name": input_name, "value": value, "persist": persist}
        if emitter_name is not None:
            params["emitter_name"] = emitter_name
        if param_type is not None:
            params["param_type"] = param_type
        return _send("set_niagara_module_input", params)

    # ------------------------------------------------------------------
    # Scratch-pad graphs
    # ------------------------------------------------------------------

    @mcp.tool()
    def create_niagara_scratch_pad_module(ctx: Context, asset_path: str, emitter_name: str, stage: str,
                                          module_name: Optional[str] = None,
                                          persist: bool = True) -> Dict[str, Any]:
        """
        Create an empty scratch-pad module on an emitter stage, as a stack module.

        The module starts from the engine's default module template (Map In -> Map Get -> Map Set ->
        Map Out), and is added to the stack at the end of the stage. Reach inside it with
        set_niagara_custom_hlsl / set_niagara_scratch_pad_pins / connect_niagara_scratch_pad_pins.

        A simulation stage is a valid target: name it ("SWE Sweep") or give its index ("0"), and the
        module runs as that stage's GPU iteration - which is where a per-cell solver (a shallow-water
        sweep, a PBF step) has to live.

        Args:
            asset_path: System asset path.
            emitter_name: Emitter handle id or name.
            stage: EmitterSpawn / EmitterUpdate / ParticleSpawn / ParticleUpdate, or a simulation
                stage of the emitter by name or index.
            module_name: Module name; default "ScratchModule".
            persist: Save after the command.

        Returns:
            module_guid (the stack node - pass it as `module` to the scratch-pad tools),
            module_name, script_path (the scratch UNiagaraScript inside the emitter), saved.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "emitter_name": emitter_name,
                                  "stage": stage, "persist": persist}
        if module_name is not None:
            params["module_name"] = module_name
        return _send("create_niagara_scratch_pad_module", params)

    @mcp.tool()
    def list_niagara_scratch_pad_nodes(ctx: Context, asset_path: str, emitter_name: str,
                                       module: str) -> Dict[str, Any]:
        """
        Dump a scratch-pad module's graph: nodes, plus every pin of every node (read-only).

        Args:
            asset_path: System asset path.
            emitter_name: Emitter handle id or name.
            module: Scratch-pad module - the stack module's node guid, or its name.

        Returns:
            module_guid, module_name, node_count, nodes[{guid, type, title, editor_x, editor_y}] and
            pins[{name, direction, type, node_guid, connected}]. Node `type` is one of CustomHlsl, Op,
            MapGet, MapSet, If, Input, Output, FunctionCall. Parameter-map pin names are already
            namespaced ("Module.Radius", "Particles.Height").
        """
        params = {"asset_path": asset_path, "emitter_name": emitter_name, "module": module}
        return _send("list_niagara_scratch_pad_nodes", params)

    @mcp.tool()
    def set_niagara_custom_hlsl(ctx: Context, asset_path: str, emitter_name: str, module: str,
                                custom_hlsl_code: str, node_guid: Optional[str] = None,
                                persist: bool = True) -> Dict[str, Any]:
        """
        Write custom HLSL into a scratch-pad module.

        Without `node_guid` a new Custom HLSL node is added; with it, the existing node is rewritten.
        In the HLSL body a pin is a variable by its own name; vector types are Float3/Float2/Float4.
        Declare the pins with set_niagara_scratch_pad_pins, then wire them.

        Args:
            asset_path: System asset path.
            emitter_name: Emitter handle id or name.
            module: Scratch-pad module (stack module node guid, or its name).
            custom_hlsl_code: HLSL source.
            node_guid: Existing Custom HLSL node to rewrite; omit to create a new one.
            persist: Save after the command.

        Returns:
            node_guid, custom_hlsl_code (read back out of the node), module_guid, module_name, saved.
            Errors: readback_mismatch when the code did not survive the write.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "emitter_name": emitter_name,
                                  "module": module, "custom_hlsl_code": custom_hlsl_code,
                                  "persist": persist}
        if node_guid is not None:
            params["node_guid"] = node_guid
        return _send("set_niagara_custom_hlsl", params)

    @mcp.tool()
    def set_niagara_scratch_pad_pins(ctx: Context, asset_path: str, emitter_name: str, module: str,
                                     input_pins: Optional[List[Dict[str, str]]] = None,
                                     output_pins: Optional[List[Dict[str, str]]] = None,
                                     node_guid: Optional[str] = None,
                                     persist: bool = True) -> Dict[str, Any]:
        """
        Declare scratch-pad pins: the module's own inputs/outputs, or typed pins on one node.

        Without `node_guid` the pins are the module's interface: an input becomes a "Module.<name>"
        read pin on the Map Get (and shows up as a stack input), an output becomes a
        "Particles.<name>" write pin on the Map Set. With `node_guid` the pins are created on that
        node (a Custom HLSL node's signature is rebuilt so the pins take part in compilation).

        Both directions are idempotent: an existing pin of the same name is reused, never duplicated -
        two same-named connected Map Set pins make the compiled system invalid.

        Args:
            asset_path: System asset path.
            emitter_name: Emitter handle id or name.
            module: Scratch-pad module (stack module node guid, or its name).
            input_pins: [{"name": "Radius", "type": "float"}].
            output_pins: [{"name": "Height", "type": "float"}].
            node_guid: Target node; omit for module-level inputs/outputs.
            persist: Save after the command.

        Returns:
            pin_count and pins[{name (namespaced), direction, type, node_guid, connected}], saved.
            Types: float / int / bool / vec2 / vec3 / vec4 / position / color / quat, plus data
            interface types (ArrayFloat, ArrayVector, Grid2D, RenderTarget2D, ...) which are the
            ones that force the emitter onto a GPU sim target.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "emitter_name": emitter_name,
                                  "module": module, "persist": persist}
        if input_pins is not None:
            params["input_pins"] = input_pins
        if output_pins is not None:
            params["output_pins"] = output_pins
        if node_guid is not None:
            params["node_guid"] = node_guid
        return _send("set_niagara_scratch_pad_pins", params)

    @mcp.tool()
    def remove_niagara_scratch_pad_node(ctx: Context, asset_path: str, emitter_name: str, module: str,
                                        node_guid: str, persist: bool = True) -> Dict[str, Any]:
        """
        Remove one node from a scratch-pad graph; its links go with it.

        The module's Input / Output node is protected (the graph does not compile without them).

        Args:
            asset_path: System asset path.
            emitter_name: Emitter handle id or name.
            module: Scratch-pad module (stack module node guid, or its name).
            node_guid: Node to remove, as listed by list_niagara_scratch_pad_nodes.
            persist: Save after the command.

        Returns:
            removed_node_guid, removed_node_type, node_count and the graph as it is now (nodes, pins).
            Errors: node_not_found (candidates = every node with its guid), structural_node.
        """
        params = {"asset_path": asset_path, "emitter_name": emitter_name, "module": module,
                  "node_guid": node_guid, "persist": persist}
        return _send("remove_niagara_scratch_pad_node", params)

    @mcp.tool()
    def remove_niagara_scratch_pad_pin(ctx: Context, asset_path: str, emitter_name: str, module: str,
                                       node_guid: str, pin_name: str, direction: Optional[str] = None,
                                       persist: bool = True) -> Dict[str, Any]:
        """
        Remove one pin from a scratch-pad node.

        On a Custom HLSL node the signature is rebuilt, so the pin really leaves the compiled script -
        that matters because a leftover Position (LWC) pin keeps failing the VM compile. A
        parameter-map pin is refused: it is the chain the module is built from.

        Args:
            asset_path: System asset path.
            emitter_name: Emitter handle id or name.
            module: Scratch-pad module (stack module node guid, or its name).
            node_guid: Node that owns the pin.
            pin_name: Pin name (a Map Get/Map Set pin is namespaced: "Module.Radius").
            direction: input / output; omit to match either.
            persist: Save after the command.

        Returns:
            removed_pin_name, node_count and the node's remaining pins.
            Errors: pin_not_found (candidates = the node's pins), structural_pin, node_not_found.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "emitter_name": emitter_name,
                                  "module": module, "node_guid": node_guid, "pin_name": pin_name,
                                  "persist": persist}
        if direction is not None:
            params["direction"] = direction
        return _send("remove_niagara_scratch_pad_pin", params)

    @mcp.tool()
    def connect_niagara_scratch_pad_pins(ctx: Context, asset_path: str, emitter_name: str, module: str,
                                         from_node: str, from_pin: str, to_node: str, to_pin: str,
                                         persist: bool = True) -> Dict[str, Any]:
        """
        Connect two pins inside a scratch-pad graph (through the graph schema, so the engine type
        checks the connection and a refusal is reported, not silently swallowed).

        Args:
            asset_path: System asset path.
            emitter_name: Emitter handle id or name.
            module: Scratch-pad module (stack module node guid, or its name).
            from_node: Source node guid (an output pin owner).
            from_pin: Source pin name.
            to_node: Target node guid (an input pin owner).
            to_pin: Target pin name.
            persist: Save after the command.

        Returns:
            connected (verified on both pins), saved.
            Errors: pin_not_found (candidates list every pin of both nodes), connect_failed.
        """
        params = {"asset_path": asset_path, "emitter_name": emitter_name, "module": module,
                  "from_node": from_node, "from_pin": from_pin, "to_node": to_node, "to_pin": to_pin,
                  "persist": persist}
        return _send("connect_niagara_scratch_pad_pins", params)

    # ------------------------------------------------------------------
    # Level actor + component parameters
    # ------------------------------------------------------------------

    @mcp.tool()
    def spawn_niagara_actor(ctx: Context, system_path: str, actor_name: str,
                            location: Optional[Dict[str, float]] = None,
                            rotation: Optional[Dict[str, float]] = None,
                            auto_activate: bool = True) -> Dict[str, Any]:
        """
        Spawn an ANiagaraActor with a system into the editor world.

        Args:
            system_path: System asset to play.
            actor_name: Actor name; a name already taken is rejected (`name_taken`).
            location: {"x": .., "y": .., "z": ..}; default 0,0,0.
            rotation: {"pitch": .., "yaw": .., "roll": ..}; default 0,0,0.
            auto_activate: Activate the component on spawn; default True.

        Returns:
            actor_name, actor_path, system_path, component_path. The level is marked dirty but NOT
            saved - save it yourself when the placement is final.
        """
        params: Dict[str, Any] = {"system_path": system_path, "actor_name": actor_name,
                                  "auto_activate": auto_activate}
        if location is not None:
            params["location"] = location
        if rotation is not None:
            params["rotation"] = rotation
        return _send("spawn_niagara_actor", params)

    @mcp.tool()
    def set_niagara_component_parameter(ctx: Context, actor_name: str, param_name: str, value: str,
                                        param_type: str) -> Dict[str, Any]:
        """
        Override one user parameter on a spawned Niagara actor's component, and read it back.

        Args:
            actor_name: Niagara actor name in the editor world (the play world while PIE runs).
            param_name: "Gravity" and "User.Gravity" are the same parameter.
            value: Text value: "12.5", "(X=0,Y=0,Z=-980)".
            param_type: float / int / bool / vec2 / vec3 / vec4 / color / quat.

        Returns:
            actor_name, param_name, param_type, value (read back from the component's override store).
            Errors: actor_not_found, component_not_found, write_failed, readback_failed.
        """
        params = {"actor_name": actor_name, "param_name": param_name, "value": value,
                  "param_type": param_type}
        return _send("set_niagara_component_parameter", params)
