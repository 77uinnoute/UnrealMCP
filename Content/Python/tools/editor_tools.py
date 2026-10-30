"""
Editor Tools for Unreal MCP.

This module provides tools for controlling the Unreal Editor viewport and other editor functionality.
"""

import logging
from typing import Dict, List, Any, Optional
from mcp.server.fastmcp import FastMCP, Context

from tools._bridge import MCPToolError, _bridge, result_of, surface_hidden_python_errors

# Get logger
logger = logging.getLogger("UnrealMCP")

def register_editor_tools(mcp: FastMCP):
    """Register editor tools with the MCP server."""
    
    @mcp.tool()
    def get_actors_in_level(ctx: Context) -> Dict[str, Any]:
        """Get all actors in the current level as a single JSON object.

        Returns:
            {"actors": [...], "count": N} — one response, not one per actor.
        """
        actors = result_of(_bridge("get_actors_in_level")).get("actors") or []
        logger.info(f"Found {len(actors)} actors in level")
        return {"actors": actors, "count": len(actors)}

    @mcp.tool()
    def find_actors_by_name(ctx: Context, pattern: str) -> Dict[str, Any]:
        """Find actors by name pattern.

        Returns:
            {"actors": [...], "count": N}
        """
        actors = result_of(_bridge("find_actors_by_name", {"pattern": pattern})).get("actors") or []

        return {"actors": actors, "count": len(actors)}
    
    @mcp.tool()
    def spawn_actor(
        ctx: Context,
        name: str,
        type: str,
        location: List[float] = [0.0, 0.0, 0.0],
        rotation: List[float] = [0.0, 0.0, 0.0]
    ) -> Dict[str, Any]:
        """Create a new actor in the current level.
        
        Args:
            ctx: The MCP context
            name: The name to give the new actor (must be unique)
            type: Actor class name - any AActor subclass works (StaticMeshActor, PointLight, SpotLight,
                DirectionalLight, SkyLight, SkyAtmosphere, ExponentialHeightFog, PostProcessVolume,
                CameraActor, ...)
            location: The [x, y, z] world location to spawn at
            rotation: The [pitch, yaw, roll] rotation in degrees
            
        Returns:
            Dict containing the created actor's properties
        """
        # Ensure all parameters are properly formatted
        params = {
            "name": name,
            "type": type,          # class name as given; the editor resolves it case-insensitively
            "location": location,
            "rotation": rotation
        }

        # Validate location and rotation formats
        for param_name in ["location", "rotation"]:
            param_value = params[param_name]
            if not isinstance(param_value, list) or len(param_value) != 3:
                raise MCPToolError("spawn_actor", "invalid_params",
                                   f"Invalid {param_name} format: {param_value}. Must be a list of 3 float values.")
            params[param_name] = [float(val) for val in param_value]

        logger.info(f"Creating actor '{name}' of type '{type}' with params: {params}")
        return _bridge("spawn_actor", params)
    
    @mcp.tool()
    def delete_actor(ctx: Context, name: str, flush: bool = False) -> Dict[str, Any]:
        """
        Delete an actor by name.

        Destroy() only marks the actor pending-kill: it leaves every actor listing at once, but its name
        stays taken until garbage collection. So spawning a same-name actor right after this call is a
        hard engine Fatal ("Cannot generate unique name"), and saving the level writes the deleted actor
        back. The response reports `pending_kill` (probed on the object graph, because the actor list is
        already clean) and explains that in `hint` when it is true.

        Args:
            name: Name of the actor to destroy.
            flush: Garbage-collect before returning so the name is actually free and `pending_kill`
                reflects that (default False).

        Returns:
            Dict with deleted_actor, pending_kill, hint (when pending_kill), flushed (when flush).
        """
        return _bridge("delete_actor", {"name": name, "flush": flush})

    @mcp.tool()
    def set_actor_transform(
        ctx: Context,
        name: str,
        location: List[float]  = None,
        rotation: List[float]  = None,
        scale: List[float] = None
    ) -> Dict[str, Any]:
        """Set the transform of an actor."""
        params = {"name": name}
        if location is not None:
            params["location"] = location
        if rotation is not None:
            params["rotation"] = rotation
        if scale is not None:
            params["scale"] = scale

        return _bridge("set_actor_transform", params)

    @mcp.tool()
    def get_actor_properties(ctx: Context, name: str) -> Dict[str, Any]:
        """Get all properties of an actor."""
        return _bridge("get_actor_properties", {"name": name})

    @mcp.tool()
    def set_actor_property(
        ctx: Context,
        name: str,
        property_name: str,
        property_value,
    ) -> Dict[str, Any]:
        """
        Set a property on an actor, on one of its components, or inside a struct/object property it holds.

        Two dotted forms are accepted, tried in this order:

        * '<component>.<property>' - a component property (light intensity, camera FOV, a static mesh,
          component visibility), e.g. name='Sun' property_name='LightComponent.intensity'. The component is
          matched by its object name or by its class name (the U prefix and the 'Component' suffix are
          optional). set_component_property is NOT a substitute: it writes a Blueprint's SCS template and has
          nothing to say about an actor already standing in the level.
        * a property path inside the actor itself, e.g. name='PPV_0'
          property_name='Settings.bOverride_AutoExposureMethod' (a field of the FPostProcessSettings struct
          property 'Settings'), or 'LodData[0].PhysicalMeshData.Values'. This is the same path grammar
          reflect_probe / set_object_property resolve, so a path can be probed before it is written.

        Args:
            name: Name of the actor
            property_name: Property name, '<component>.<property>', or a path like
                'Settings.bOverride_AutoExposureMethod'
            property_value: Value to set the property to

        Returns:
            On success: actor, property, `property_value_after` (read back from the object that was written),
            `component` when the write went to a component, and `resolved_property` when a path was walked.
            On failure: the reflector's error code, `components` (the names to choose from),
            `component_property_paths` when a component owns a property resembling the request, and -
            for a dotted name - `path_prefix` / `path_candidates` / `path_error` describing how far the path
            got and which fields exist at the step that failed.
        """
        return _bridge("set_actor_property", {
            "name": name,
            "property_name": property_name,
            "property_value": property_value
        })

    # @mcp.tool() commented out because it's buggy
    def focus_viewport(
        ctx: Context,
        target: str = None,
        location: List[float] = None,
        distance: float = 1000.0,
        orientation: List[float] = None
    ) -> Dict[str, Any]:
        """
        Focus the viewport on a specific actor or location.
        
        Args:
            target: Name of the actor to focus on (if provided, location is ignored)
            location: [X, Y, Z] coordinates to focus on (used if target is None)
            distance: Distance from the target/location
            orientation: Optional [Pitch, Yaw, Roll] for the viewport camera
            
        Returns:
            Response from Unreal Engine
        """
        params = {}
        if target:
            params["target"] = target
        elif location:
            params["location"] = location

        if distance:
            params["distance"] = distance

        if orientation:
            params["orientation"] = orientation

        return _bridge("focus_viewport", params)

    @mcp.tool()
    def spawn_blueprint_actor(
        ctx: Context,
        blueprint_name: str,
        actor_name: str,
        location: List[float] = [0.0, 0.0, 0.0],
        rotation: List[float] = [0.0, 0.0, 0.0],
        scale: List[float] = None
    ) -> Dict[str, Any]:
        """Spawn an actor from a Blueprint.
        
        Args:
            ctx: The MCP context
            blueprint_name: Name of the Blueprint to spawn from
            actor_name: Name to give the spawned actor
            location: The [x, y, z] world location to spawn at
            rotation: The [pitch, yaw, roll] rotation in degrees
            scale: The [x, y, z] scale. Applied after the spawn, not through the spawn
                transform: a Blueprint actor's root component comes from the SCS, so the
                engine rebuilds it and the transform's scale is lost (measured: [1.4,1.4,1.4]
                came back (1,1,1) while location/rotation were exact).
            
        Returns:
            Dict containing the spawned actor's properties
        """
        # Ensure all parameters are properly formatted
        params = {
            "blueprint_name": blueprint_name,
            "actor_name": actor_name,
            "location": location or [0.0, 0.0, 0.0],
            "rotation": rotation or [0.0, 0.0, 0.0]
        }
        if scale is not None:
            params["scale"] = scale

        # Validate location and rotation formats
        for param_name in ["location", "rotation"]:
            param_value = params[param_name]
            if not isinstance(param_value, list) or len(param_value) != 3:
                raise MCPToolError("spawn_blueprint_actor", "invalid_params",
                                   f"Invalid {param_name} format: {param_value}. Must be a list of 3 float values.")
            params[param_name] = [float(val) for val in param_value]

        logger.info(f"Spawning blueprint actor with params: {params}")
        return _bridge("spawn_blueprint_actor", params)

    @mcp.tool()
    def execute_python_command(ctx: Context, command: str, timeout: float = None) -> Dict[str, Any]:
        """Execute arbitrary Python code inside the Unreal Editor Python environment (requires Python Editor Script Plugin).

        SKILLS: if the task matches an available skill's description (e.g. Unreal material work ->
        "unreal-material-authoring"), load that skill with use_skill before running the script.

        This can read/modify assets, actors, project settings, etc. Use unreal Python API (e.g. unreal.load_asset,
        unreal.EditorAssetLibrary, set_editor_property).

        NOTE: Executed as a file (ExecuteFile mode). Do NOT use top-level 'return'; output via print() instead.
        NOTE: Do NOT time.sleep()/poll inside the code — it blocks the editor GameThread and
        deadlocks. Split waits into multiple short commands.
        NOTE: For scripts longer than ~30 lines prefer execute_python_file (write the script to
        disk, execute by path) — long inline strings have historically been corrupted upstream
        of the bridge (MCP transport / generation), causing random middle-of-payload loss.

        ALWAYS SYNCHRONOUS — there is no job/queue mode:
        - The result (and the structured error envelope) comes back in-band; the editor answers
          other commands again as soon as the call returns.
        - A call must therefore FINISH inside the client tool timeout (~90s). Split heavier work
          into several short idempotent calls, and widen the wait for a merely slow one with
          timeout=<seconds> (<=600s).
        - Prefer a dedicated tool over hand-rolled python whenever one exists — introspect with
          list_mcp_commands first. Asset import is the hard example: it MUST go through
          import_assets (a hand-rolled AssetImportTask hits Interchange and crashes the editor).

        On failure (exception/traceback), the tool call itself fails with the traceback as its message —
        no need to inspect the log files.

        Args:
            ctx: The MCP context
            command: Python code to execute in the editor (multi-line allowed)
            timeout: Response receive timeout in seconds (default: server recv-timeout)

        Returns:
            Dict with 'status': 'success' carrying 'result' (with 'success', evaluation 'result',
            and 'log' entries). result also carries 'duration_ms' (the whole bridge-side handler:
            banned-API scan + python exec + log collection) and 'python_ms' (the engine's python
            call alone). Both exclude the TCP round trip and this tool wrapper.
        """
        params = {"command": command}
        if timeout is not None:
            # Keep the C++ GameThread wait in sync with the client receive timeout so long-running
            # compiles are not killed at 120s on one layer while the other waits longer.
            params["timeout_ms"] = int(timeout * 1000)
        reply = _bridge("execute_python_command", params, recv_timeout=timeout)

        # Fallback: older bridge builds report success even when the Python command raised - the
        # traceback only shows up as Error entries in result.log.
        hidden = surface_hidden_python_errors(reply)
        if hidden:
            logger.error("Hidden Python errors detected in success response")
            raise MCPToolError("execute_python_command", "python_error", hidden, reply)

        return reply

    @mcp.tool()
    def execute_python_file(ctx: Context, file_path: str, timeout: float = None) -> Dict[str, Any]:
        """Execute a local Python FILE inside the Unreal Editor (recommended for scripts > ~30 lines).

        SKILLS: if the task matches an available skill's description (e.g. Unreal material work ->
        "unreal-material-authoring"), load that skill with use_skill before running the script.

        The bridge reads the file on the editor side and runs it through the same pipeline as
        execute_python_command (ExecuteFile mode, same success/result/log/error response shape).
        Because only the short file path travels over the wire, this route is immune to the
        upstream long-string corruption issues seen with inline commands.

        ALWAYS SYNCHRONOUS (same policy as execute_python_command): the call must FINISH inside
        the client tool timeout (~90s). Split heavier work into several short idempotent calls;
        widen the wait for a merely slow one with timeout=<seconds> (<=600s). Always prefer an
        existing tool (list_mcp_commands) over a hand-rolled script — asset import must use
        import_assets, never AssetImportTask.

        Args:
            ctx: The MCP context
            file_path: ABSOLUTE path to a .py file on this machine (e.g.
                "E:/proj/Saved/MCPScripts/build_material.py")
            timeout: Response receive timeout in seconds (default: server recv-timeout)

        Returns:
            Same shape as execute_python_command; additionally result carries file_path
            and code_bytes, plus the same duration_ms / python_ms timing fields.
        """
        params = {"file_path": file_path}
        if timeout is not None:
            params["timeout_ms"] = int(timeout * 1000)
        reply = _bridge("execute_python_file", params, recv_timeout=timeout)

        # Surface hidden errors (same fallback as execute_python_command)
        hidden = surface_hidden_python_errors(reply)
        if hidden:
            logger.error("Hidden Python errors detected in success response")
            raise MCPToolError("execute_python_file", "python_error", hidden, reply)

        return reply

    @mcp.tool()
    def take_screenshot(ctx: Context, filepath: str, source: str = "level_viewport",
                        route: str = "auto", asset_path: Optional[str] = None) -> Dict[str, Any]:
        """Capture a PNG file (synchronous).

        source="level_viewport" (default) reads a level viewport backbuffer. That
        image never contains PIE or UMG: UMG is composited by Slate on top of the game
        viewport, not drawn into the scene. The level viewport is picked by world identity,
        not by being the ACTIVE viewport, so it keeps working while PIE runs (the active
        viewport is then the game one).
        source="pie" reads the running PIE game viewport backbuffer, and route picks between
        its two routes: "auto" (default, backbuffer first), "viewport" (backbuffer only) or
        "slate" (a Slate screenshot of the game viewport widget). The backbuffer route needs
        no visible window, so it works with a minimized editor, but its pixels NEVER contain
        UMG - Slate is the only route that does, and it needs the editor window on screen.
        Because the backbuffer route succeeds whenever PIE has a viewport, "auto" alone can
        never show a widget: pass route="slate" for that. The reply says which route ran:
        captured_via is "pie_viewport_readback" (includes_umg false) or "pie_slate"
        (includes_umg true). Needs a running PIE (pie_not_running otherwise). Use it for UI
        layout triage only (hit areas, scaling, clipping); it is not an acceptance verdict -
        the user judges the result in the viewport.
        source="asset_editor" reads an asset editor's preview viewport (Persona, the material
        editor, ...). With asset_path the command focuses THAT asset's editor first and then
        captures its viewport; the editor is never opened for you (editor_not_open lists the
        open_* commands otherwise). Without asset_path it uses the focused asset editor
        viewport; several candidates with no focus come back as viewport_ambiguous with the
        list instead of a guess. A UMG designer is the special case: it has NO scene viewport,
        so for a Widget Blueprint the capture is the designer preview's own Slate widget
        instead of any viewport (captured_via="widget_preview_slate", includes_umg true,
        camera null) - that is the asset's design, but the designer paints its own widget
        outlines over the preview area and Slate's capture covers the whole window region, so
        includes_editor_overlay is true here as well: for pixels WITHOUT them, use
        source="pie" + route="slate". It needs the designer open (open_umg_designer) and the
        editor window on screen. Also triage only, not an acceptance verdict.

        If the widget preview comes back with no pixels (widget_preview_screenshot_failed), the designer
        view itself is stale - it is not a window position problem: close and reopen the asset editor
        (close_asset_editors then open_umg_designer) and retry.

        Writes to the EXACT path given — unlike unreal.AutomationLibrary.take_high_res_screenshot
        / the HighResShot console command, the filename is honored (no
        HighresScreenshot0000X.png renaming). Files that come out suspiciously small (<10KB)
        are retried once. A small file is only an error for a scene readback: a pure UI capture
        (includes_umg true) can legitimately compress below that, and comes back with a
        "warning" field instead of being rejected.

        Args:
            ctx: The MCP context
            filepath: ABSOLUTE output path ending in .png, e.g.
                "E:/proj/Saved/shot.png" (relative paths resolve against the
                editor working directory and are unreliable)
            source: "level_viewport" (default), "pie" or "asset_editor"
            route: With source="pie": "auto" (default), "viewport" (no UMG) or "slate"
                (the only route that includes UMG).
            asset_path: With source="asset_editor": the asset whose editor to capture, e.g.
                "/Game/MMD/FeiYing/Animation/Clips/Fei_Idle" (or a Widget Blueprint, which is
                captured as its designer preview widget).

        Returns:
            Dict with status success and result carrying filepath, width, height,
            file_size (bytes), source, captured_via (level_viewport_readback /
            pie_viewport_readback / pie_slate / widget_preview_slate), includes_umg,
            includes_editor_overlay (true for level_viewport and asset_editor viewports: the
            editor's sprite icons, grid and gizmo are IN the image - a light's sprite is not
            scene content; false for the PIE Slate route, true for the widget-preview route
            because the designer's widget outlines are painted over the preview area), and
            (for asset_editor) editor_name / resolved_viewport / focused
        """
        params = {"filepath": filepath, "source": source, "route": route}
        if asset_path:
            params["asset_path"] = asset_path
        reply = _bridge("take_screenshot", params)

        result_obj = result_of(reply)
        file_size = int(result_obj.get("file_size") or 0)

        # 黑帧/陈旧帧防护：文件过小则重试一次。只对场景回读成立 —— 纯 UI 截图（含 UMG）本来就
        # 可能很小（一个干净的控件预览比一帧 3D 画面小得多），那种情况下报错会把有效截图挡掉。
        if file_size < 10 * 1024:
            logger.warning(f"Suspiciously small screenshot ({file_size} bytes), retrying once")
            try:
                result_obj = result_of(_bridge("take_screenshot", params))
                file_size = int(result_obj.get("file_size") or 0)
            except MCPToolError as retry_error:
                logger.warning(f"Screenshot retry failed: {retry_error}")
            if file_size < 10 * 1024 and not result_obj.get("includes_umg"):
                raise MCPToolError("take_screenshot", "screenshot_too_small",
                                   f"Screenshot file too small ({file_size} bytes) — viewport may be unfocused/idle",
                                   reply)
            if file_size < 10 * 1024:
                result_obj["warning"] = (
                    f"small capture ({file_size} bytes) - expected for a pure UI capture, "
                    "check the PNG content rather than the size"
                )
        return reply

    @mcp.tool()
    def get_console_variable(ctx: Context, name: str) -> Dict[str, Any]:
        """Read a console variable value (e.g. 'r.CustomDepth').

        The Unreal python API has NO console-variable read functions, so this is
        implemented on the C++ side (IConsoleManager).

        Args:
            name: Console variable name, e.g. "r.CustomDepth"

        Returns:
            Dict with status success and result carrying name and value
        """
        return _bridge("get_console_variable", {"name": name})

    @mcp.tool()
    def set_console_variable(ctx: Context, name: str, value: str) -> Dict[str, Any]:
        """Set a console variable value (e.g. r.CustomDepth = 3).

        Args:
            name: Console variable name, e.g. "r.CustomDepth"
            value: New value as a string, e.g. "3"

        Returns:
            Dict with status success and result carrying name and the value read back
        """
        return _bridge("set_console_variable", {"name": name, "value": value})

    @mcp.tool()
    def live_coding_compile(ctx: Context, wait: bool = True) -> Dict[str, Any]:
        """Hot reload C++ edits through Live Coding (the editor's Ctrl+Alt+F11, driven from MCP).

        Use this after editing project C++ instead of killing and restarting the editor. It only
        applies to function bodies: adding a UCLASS/UPROPERTY, changing a class layout or adding a
        new class still needs a full editor restart.

        The compile runs on the GameThread, so a waiting call keeps the MCP channel busy; pass
        wait=False when the patch may take long, then poll `live_coding_status`.

        Args:
            wait: Block until the compile finishes and report its outcome (default True).

        Returns:
            Dict with the result envelope; the fields that matter are under result.data:
              outcome     - success / no_changes / failure / cancelled / not_started / in_progress
              is_compiling- whether a compile is still running when this reply was built
              duration_ms - how long the call took (wait=True only)
              log_tail    - the LogLiveCoding lines captured for this request
        """
        logger.info(f"live_coding_compile requested (wait={wait})")
        return _bridge("live_coding_compile", {"wait": wait})

    @mcp.tool()
    def live_coding_status(ctx: Context) -> Dict[str, Any]:
        """Read Live Coding state: availability, session enablement, whether a compile is running.

        This is the poll target for `live_coding_compile(wait=False)`. When the outstanding request
        has stopped compiling, this call finalises it and reports the outcome it read from the
        captured LogLiveCoding output (the engine only reports a result to the call that waited).

        Returns:
            Dict with result.data carrying available / has_started / enabled_for_session /
            is_compiling / request_pending / outcome / outcome_source / log_tail.
        """
        return _bridge("live_coding_status", {})

    @mcp.tool()
    def close_asset_editors(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """Close every editor of an asset without changing where it opens next time.

        Use this instead of calling `AssetEditorSubsystem.close_all_editors_for_asset`
        from python. Closing an asset editor tab makes the engine record the asset's
        next open location (`[AssetEditorToolkitTabLocation]`); a raw close therefore
        silently turns the asset into a standalone floating window next time it is
        opened. This tool captures that record before closing and restores it after
        (removing the key when it did not exist), so an MCP-driven close leaves the
        user's open location untouched.

        Material graph work still needs the editor closed first (an open material
        editor holds its own graph nodes and undo stack and will roll back scripted
        changes) - see the "unreal-material-authoring" skill.

        Args:
            asset_path: Full asset path, e.g. "/Game/MCP/M_Outline_RT"

        Returns:
            Dict with asset_kind, closed_editors and three decidable fields:
              restored_value        - the state the record is left in
                                      ("docked" / "standalone" / "absent")
              key_removed           - whether the command actively deleted the key
                                      (true when there was no record to begin with)
              configuration_changed - whether the ini was touched at all (false for a
                                      pure no-op, i.e. closed_editors == 0)
        """
        logger.info(f"Closing asset editors for: {asset_path}")
        return _bridge("close_asset_editors", {"asset_path": asset_path})

    @mcp.tool()
    def spawn_particle_actor(
        ctx: Context,
        name: str,
        template: str,
        location: List[float] = [0.0, 0.0, 0.0],
        rotation: List[float] = [0.0, 0.0, 0.0],
        attach_to: str = None,
        relative_location: List[float] = None,
        relative_rotation: List[float] = None,
        auto_activate: bool = True
    ) -> Dict[str, Any]:
        """Spawn an Emitter actor for a particle system, optionally attached to another actor.

        This is the one call that used to be a stack of python boilerplate: spawn an AEmitter,
        set its ParticleSystemComponent template, attach it and activate it.

        Args:
            ctx: The MCP context
            name: Name for the new actor (must be unique)
            template: Particle system asset path (e.g. /Game/Particles/PS_Buff_AttributeBoost)
            location: [x, y, z] world location to spawn at
            rotation: [pitch, yaw, roll] rotation in degrees
            attach_to: Optional actor name to attach to (world transform is kept)
            relative_location: [x, y, z] location relative to the parent (applied after attaching)
            relative_rotation: [pitch, yaw, roll] degrees relative to the parent
            auto_activate: Activate the system right away (default True)

        Returns:
            Dict with actor, template, attached_to (null when not attached), the applied
            relative_location and activated. Errors: not_particle_system (template is not a
            UParticleSystem) / actor_not_found (attach_to, with candidates) / duplicate name.
        """
        params = {
            "name": name,
            "template": template,
            "location": location,
            "rotation": rotation,
            "auto_activate": auto_activate,
        }
        if attach_to:
            params["attach_to"] = attach_to
        if relative_location is not None:
            params["relative_location"] = relative_location
        if relative_rotation is not None:
            params["relative_rotation"] = relative_rotation

        for param_name in ("location", "rotation", "relative_location", "relative_rotation"):
            param_value = params.get(param_name)
            if param_value is None:
                continue
            if not isinstance(param_value, list) or len(param_value) != 3:
                raise MCPToolError("spawn_particle_actor", "invalid_params",
                                   f"Invalid {param_name} format: {param_value}. Must be a list of 3 float values.")
            params[param_name] = [float(val) for val in param_value]

        reply = _bridge("spawn_particle_actor", params)
        result = result_of(reply)
        return result if result else {"success": True, "actor": name}

    logger.info("Editor tools registered successfully")
