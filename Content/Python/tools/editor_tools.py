"""
Editor Tools for Unreal MCP.

This module provides tools for controlling the Unreal Editor viewport and other editor functionality.
"""

import logging
from typing import Dict, List, Any, Optional
from mcp.server.fastmcp import FastMCP, Context

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
        from unreal_mcp_server import get_unreal_connection
        
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.warning("Failed to connect to Unreal Engine")
                return {"actors": [], "count": 0}
                
            response = unreal.send_command("get_actors_in_level", {})
            
            if not response:
                logger.warning("No response from Unreal Engine")
                return {"actors": [], "count": 0}
                
            # Log the complete response for debugging
            logger.info(f"Complete response from Unreal: {response}")
            
            # Check response format
            actors = None
            if "result" in response and "actors" in response["result"]:
                actors = response["result"]["actors"]
            elif "actors" in response:
                actors = response["actors"]
                
            if actors is None:
                logger.warning(f"Unexpected response format: {response}")
                return {"actors": [], "count": 0}
                
            logger.info(f"Found {len(actors)} actors in level")
            return {"actors": actors, "count": len(actors)}
            
        except Exception as e:
            logger.error(f"Error getting actors: {e}")
            return {"actors": [], "count": 0, "error": str(e)}

    @mcp.tool()
    def find_actors_by_name(ctx: Context, pattern: str) -> Dict[str, Any]:
        """Find actors by name pattern.

        Returns:
            {"actors": [...], "count": N}
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.warning("Failed to connect to Unreal Engine")
                return {"actors": [], "count": 0}
                
            response = unreal.send_command("find_actors_by_name", {
                "pattern": pattern
            })
            
            if not response:
                return {"actors": [], "count": 0}

            # The bridge wraps command payloads as {"status": "success", "result": {...}}.
            actors = None
            if "result" in response and "actors" in response["result"]:
                actors = response["result"]["actors"]
            elif "actors" in response:
                actors = response["actors"]

            if actors is None:
                logger.warning(f"Unexpected response format: {response}")
                return {"actors": [], "count": 0, "error": response.get("error")}

            return {"actors": actors, "count": len(actors)}
            
        except Exception as e:
            logger.error(f"Error finding actors: {e}")
            return {"actors": [], "count": 0, "error": str(e)}
    
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
        from unreal_mcp_server import get_unreal_connection
        
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
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
                    logger.error(f"Invalid {param_name} format: {param_value}. Must be a list of 3 float values.")
                    return {"success": False, "message": f"Invalid {param_name} format. Must be a list of 3 float values."}
                # Ensure all values are float
                params[param_name] = [float(val) for val in param_value]
            
            logger.info(f"Creating actor '{name}' of type '{type}' with params: {params}")
            response = unreal.send_command("spawn_actor", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            # Log the complete response for debugging
            logger.info(f"Actor creation response: {response}")
            
            # Handle error responses correctly
            if response.get("status") == "error":
                error_message = response.get("error", "Unknown error")
                logger.error(f"Error creating actor: {error_message}")
                return {"success": False, "message": error_message}
            
            return response
            
        except Exception as e:
            error_msg = f"Error creating actor: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
    
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
        from unreal_mcp_server import get_unreal_connection
        
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
                
            response = unreal.send_command("delete_actor", {
                "name": name,
                "flush": flush,
            })
            return response or {}
            
        except Exception as e:
            logger.error(f"Error deleting actor: {e}")
            return {}
    
    @mcp.tool()
    def set_actor_transform(
        ctx: Context,
        name: str,
        location: List[float]  = None,
        rotation: List[float]  = None,
        scale: List[float] = None
    ) -> Dict[str, Any]:
        """Set the transform of an actor."""
        from unreal_mcp_server import get_unreal_connection
        
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
                
            params = {"name": name}
            if location is not None:
                params["location"] = location
            if rotation is not None:
                params["rotation"] = rotation
            if scale is not None:
                params["scale"] = scale
                
            response = unreal.send_command("set_actor_transform", params)
            return response or {}
            
        except Exception as e:
            logger.error(f"Error setting transform: {e}")
            return {}
    
    @mcp.tool()
    def get_actor_properties(ctx: Context, name: str) -> Dict[str, Any]:
        """Get all properties of an actor."""
        from unreal_mcp_server import get_unreal_connection
        
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
                
            response = unreal.send_command("get_actor_properties", {
                "name": name
            })
            return response or {}
            
        except Exception as e:
            logger.error(f"Error getting properties: {e}")
            return {}

    @mcp.tool()
    def set_actor_property(
        ctx: Context,
        name: str,
        property_name: str,
        property_value,
    ) -> Dict[str, Any]:
        """
        Set a property on an actor.
        
        Args:
            name: Name of the actor
            property_name: Name of the property to set
            property_value: Value to set the property to
            
        Returns:
            Dict containing response from Unreal with operation status
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
                
            response = unreal.send_command("set_actor_property", {
                "name": name,
                "property_name": property_name,
                "property_value": property_value
            })
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Set actor property response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error setting actor property: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

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
        from unreal_mcp_server import get_unreal_connection
        
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
                
            params = {}
            if target:
                params["target"] = target
            elif location:
                params["location"] = location
            
            if distance:
                params["distance"] = distance
                
            if orientation:
                params["orientation"] = orientation
                
            response = unreal.send_command("focus_viewport", params)
            return response or {}
            
        except Exception as e:
            logger.error(f"Error focusing viewport: {e}")
            return {"status": "error", "message": str(e)}

    @mcp.tool()
    def spawn_blueprint_actor(
        ctx: Context,
        blueprint_name: str,
        actor_name: str,
        location: List[float] = [0.0, 0.0, 0.0],
        rotation: List[float] = [0.0, 0.0, 0.0]
    ) -> Dict[str, Any]:
        """Spawn an actor from a Blueprint.
        
        Args:
            ctx: The MCP context
            blueprint_name: Name of the Blueprint to spawn from
            actor_name: Name to give the spawned actor
            location: The [x, y, z] world location to spawn at
            rotation: The [pitch, yaw, roll] rotation in degrees
            
        Returns:
            Dict containing the spawned actor's properties
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            # Ensure all parameters are properly formatted
            params = {
                "blueprint_name": blueprint_name,
                "actor_name": actor_name,
                "location": location or [0.0, 0.0, 0.0],
                "rotation": rotation or [0.0, 0.0, 0.0]
            }
            
            # Validate location and rotation formats
            for param_name in ["location", "rotation"]:
                param_value = params[param_name]
                if not isinstance(param_value, list) or len(param_value) != 3:
                    logger.error(f"Invalid {param_name} format: {param_value}. Must be a list of 3 float values.")
                    return {"success": False, "message": f"Invalid {param_name} format. Must be a list of 3 float values."}
                # Ensure all values are float
                params[param_name] = [float(val) for val in param_value]
            
            logger.info(f"Spawning blueprint actor with params: {params}")
            response = unreal.send_command("spawn_blueprint_actor", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Spawn blueprint actor response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error spawning blueprint actor: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

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

        On failure (exception/traceback), the error is returned DIRECTLY as {'status': 'error',
        'error': <full traceback>, 'result': {...}} — no need to inspect the log files.

        Args:
            ctx: The MCP context
            command: Python code to execute in the editor (multi-line allowed)
            timeout: Response receive timeout in seconds (default: server recv-timeout)

        Returns:
            Dict with 'status': 'success' carries 'result' (with 'success', evaluation 'result',
            and 'log' entries); on failure 'status': 'error' with the traceback in 'error'.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"command": command}
            if timeout is not None:
                # Keep the C++ GameThread wait in sync with the client receive
                # timeout so long-running compiles are not killed at 120s on
                # one layer while the other waits longer.
                params["timeout_ms"] = int(timeout * 1000)
            response = unreal.send_command("execute_python_command", params, recv_timeout=timeout)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            logger.info(f"Execute python command response: {response}")

            # Fallback: older bridge builds report success even when the Python command
            # raised — the traceback only shows up as Error entries in result.log.
            # Surface those errors directly in the tool result.
            if response.get("status") == "success":
                result_obj = response.get("result")
                if isinstance(result_obj, dict):
                    error_parts = []
                    for entry in (result_obj.get("log") or []):
                        if isinstance(entry, dict) and entry.get("type") == "Error":
                            line = str(entry.get("output", "")).replace("\r\n", "\n")
                            if line and not line.endswith("\n"):
                                line += "\n"
                            error_parts.append(line)
                    if error_parts:
                        logger.error("Hidden Python errors detected in success response")
                        return {
                            "status": "error",
                            "error": "".join(error_parts),
                            "result": result_obj,
                        }

            return response

        except Exception as e:
            error_msg = f"Error executing python command: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

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
            and code_bytes.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"file_path": file_path}
            if timeout is not None:
                params["timeout_ms"] = int(timeout * 1000)
            response = unreal.send_command("execute_python_file", params, recv_timeout=timeout)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            logger.info(f"Execute python file response: {response}")

            # Surface hidden errors (same fallback as execute_python_command)
            if response.get("status") == "success":
                result_obj = response.get("result")
                if isinstance(result_obj, dict):
                    error_parts = []
                    for entry in (result_obj.get("log") or []):
                        if isinstance(entry, dict) and entry.get("type") == "Error":
                            line = str(entry.get("output", "")).replace("\r\n", "\n")
                            if line and not line.endswith("\n"):
                                line += "\n"
                            error_parts.append(line)
                    if error_parts:
                        logger.error("Hidden Python errors detected in success response")
                        return {
                            "status": "error",
                            "error": "".join(error_parts),
                            "result": result_obj,
                        }

            return response

        except Exception as e:
            error_msg = f"Error executing python file: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def take_screenshot(ctx: Context, filepath: str, source: str = "level_viewport",
                        asset_path: Optional[str] = None) -> Dict[str, Any]:
        """Capture a PNG file (synchronous).

        source="level_viewport" (default) reads the active level viewport backbuffer. That
        image never contains PIE or UMG: UMG is composited by Slate on top of the game
        viewport, not drawn into the scene.
        source="pie" takes a Slate screenshot of the running PIE game viewport widget, so
        widgets added to the viewport ARE in it. Needs a running PIE (pie_not_running
        otherwise). Use it for UI layout triage only (hit areas, scaling, clipping); it is
        not an acceptance verdict - the user judges the result in the viewport.
        source="asset_editor" reads an asset editor's preview viewport (Persona, the material
        editor, ...). With asset_path the command focuses THAT asset's editor first and then
        captures its viewport; the editor is never opened for you (editor_not_open lists the
        open_* commands otherwise). Without asset_path it uses the focused asset editor
        viewport; several candidates with no focus come back as viewport_ambiguous with the
        list instead of a guess. Also triage only, not an acceptance verdict.

        Writes to the EXACT path given — unlike unreal.AutomationLibrary.take_high_res_screenshot
        / the HighResShot console command, the filename is honored (no
        HighresScreenshot0000X.png renaming). Files that come out suspiciously small (<10KB)
        are retried once.

        Args:
            ctx: The MCP context
            filepath: ABSOLUTE output path ending in .png, e.g.
                "E:/proj/Saved/shot.png" (relative paths resolve against the
                editor working directory and are unreliable)
            source: "level_viewport" (default), "pie" or "asset_editor"
            asset_path: With source="asset_editor": the asset whose editor to capture, e.g.
                "/Game/MMD/FeiYing/Animation/Clips/Fei_Idle".

        Returns:
            Dict with status success and result carrying filepath, width, height,
            file_size (bytes), source and (for asset_editor) editor_name /
            resolved_viewport / focused
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"status": "error", "error": "Failed to connect to Unreal Engine"}

            params = {"filepath": filepath, "source": source}
            if asset_path:
                params["asset_path"] = asset_path
            response = unreal.send_command("take_screenshot", params)
            if not response or response.get("status") != "success":
                return response or {"status": "error", "error": "No response from Unreal Engine"}

            result_obj = response.get("result") or {}
            file_size = int(result_obj.get("file_size") or 0)

            # 黑帧/陈旧帧防护：文件过小则重试一次
            if file_size < 10 * 1024:
                logger.warning(f"Suspiciously small screenshot ({file_size} bytes), retrying once")
                response = unreal.send_command("take_screenshot", params)
                if response and response.get("status") == "success":
                    result_obj = response.get("result") or {}
                    file_size = int(result_obj.get("file_size") or 0)
                if file_size < 10 * 1024:
                    return {
                        "status": "error",
                        "error": f"Screenshot file too small ({file_size} bytes) — viewport may be unfocused/idle",
                        "result": result_obj,
                    }
            return response

        except Exception as e:
            error_msg = f"Error taking screenshot: {e}"
            logger.error(error_msg)
            return {"status": "error", "error": error_msg}

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
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"status": "error", "error": "Failed to connect to Unreal Engine"}

            return unreal.send_command("get_console_variable", {"name": name}) or {}

        except Exception as e:
            error_msg = f"Error getting console variable: {e}"
            logger.error(error_msg)
            return {"status": "error", "error": error_msg}

    @mcp.tool()
    def set_console_variable(ctx: Context, name: str, value: str) -> Dict[str, Any]:
        """Set a console variable value (e.g. r.CustomDepth = 3).

        Args:
            name: Console variable name, e.g. "r.CustomDepth"
            value: New value as a string, e.g. "3"

        Returns:
            Dict with status success and result carrying name and the value read back
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"status": "error", "error": "Failed to connect to Unreal Engine"}

            return unreal.send_command("set_console_variable", {"name": name, "value": value}) or {}

        except Exception as e:
            error_msg = f"Error setting console variable: {e}"
            logger.error(error_msg)
            return {"status": "error", "error": error_msg}

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
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"status": "error", "error": "Failed to connect to Unreal Engine"}

            logger.info(f"live_coding_compile requested (wait={wait})")
            return unreal.send_command("live_coding_compile", {"wait": wait}) or {}

        except Exception as e:
            error_msg = f"Error requesting a Live Coding compile: {e}"
            logger.error(error_msg)
            return {"status": "error", "error": error_msg}

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
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"status": "error", "error": "Failed to connect to Unreal Engine"}

            return unreal.send_command("live_coding_status", {}) or {}

        except Exception as e:
            error_msg = f"Error reading Live Coding status: {e}"
            logger.error(error_msg)
            return {"status": "error", "error": error_msg}

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
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"status": "error", "error": "Failed to connect to Unreal Engine"}

            logger.info(f"Closing asset editors for: {asset_path}")
            return unreal.send_command("close_asset_editors", {"asset_path": asset_path}) or {}

        except Exception as e:
            error_msg = f"Error closing asset editors: {e}"
            logger.error(error_msg)
            return {"status": "error", "error": error_msg}

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
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

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
                    logger.error(f"Invalid {param_name} format: {param_value}. Must be a list of 3 float values.")
                    return {"success": False, "message": f"Invalid {param_name} format. Must be a list of 3 float values."}
                params[param_name] = [float(val) for val in param_value]

            response = unreal.send_command("spawn_particle_actor", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            if response.get("status") == "error":
                return {"success": False, "message": response.get("error", "Unknown error")}

            result = response.get("result", response)
            if isinstance(result, dict) and result.get("success") is False:
                return {"success": False, "message": result.get("error", "Unknown error")}
            return result if isinstance(result, dict) else {"success": True, "actor": name}

        except Exception as e:
            logger.error(f"Error spawning particle actor: {e}")
            return {"success": False, "message": str(e)}

    logger.info("Editor tools registered successfully")
