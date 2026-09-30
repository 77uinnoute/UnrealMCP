"""
Material Tools for Unreal MCP.

Provides tools for scanning material custom-expression nodes, connecting material
property pins, converting static-switch parameters to dynamic scalars, and rendering a
single graph node into a PNG for inspection.
"""

import json
import logging
from typing import Dict, Any, Optional
from mcp.server.fastmcp import FastMCP, Context

# Get logger
logger = logging.getLogger("UnrealMCP")

# The node preview itself runs inside the editor (Content/Python/mcp_material_preview.py):
# the engine python API cannot build the "route this node into EmissiveColor" graph edit,
# and the draw has to happen on the editor GameThread. This layer only ships the request
# over the bridge and reads the JSON the editor-side module prints back.
PREVIEW_MODULE = "mcp_material_preview"
PREVIEW_SENTINEL = "MCP_PREVIEW_JSON:"


def _editor_log_text(response: Dict[str, Any]) -> str:
    """print() output from the editor python lands in the response log entries."""
    result = response.get("result") or {}
    parts = []
    for entry in (result.get("log") or []):
        if isinstance(entry, dict):
            parts.append(str(entry.get("output", "")))
    return "".join(parts)


def _extract_preview_payload(response: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    for line in reversed(_editor_log_text(response).splitlines()):
        line = line.strip()
        if line.startswith(PREVIEW_SENTINEL):
            try:
                return json.loads(line[len(PREVIEW_SENTINEL):])
            except ValueError:
                return None
    return None


# The Custom HLSL rule set lives in C++ (UnrealMCPMaterialHlslLint) so the write
# path in set_material_expression_property and the validate_custom_hlsl tool can
# never drift apart; this layer only forwards to the bridge commands.


def register_material_tools(mcp: FastMCP):
    """Register material tools with the MCP server."""

    @mcp.tool()
    def scan_material_custom_nodes(ctx: Context, folder: str) -> Dict[str, Any]:
        """
        Scan a content folder for materials that contain Custom expression nodes.

        Args:
            folder: Content browser folder path (e.g. "/Game/Materials")

        Returns:
            Dict containing the list of materials with custom expression nodes
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"folder": folder}
            logger.info(f"Scanning materials in folder: {folder}")
            response = unreal.send_command("scan_material_custom_nodes", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error scanning material custom nodes: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def validate_custom_hlsl(ctx: Context, code: str, output_type: Optional[str] = None) -> Dict[str, Any]:
        """
        Statically check Custom node HLSL before it is written (no shader compile).

        Before editing materials, load the "unreal-material-authoring" skill (use_skill).

        Runs the same C++ rule set the write path uses, so a script that writes code
        itself can ask the identical question first. Errors block a subsequent write
        through set_material_expression_property(property="code"); warnings are
        informational. Catches the editor-crashing cases (naming a shader parameter
        struct such as SceneTexturesStruct / *UniformParameters -> RHICoreShader.cpp:55
        assert), forced compile errors (missing return, non-virtual #include,
        malformed/duplicate Input: declarations) and the known traps (View.GameTime
        frozen outside PIE, hand-written buffer UV/size math, View members the
        translator does not emit, unused declared inputs, return count vs Output Type).

        Args:
            code: The Custom node HLSL source
            output_type: Optional node Output Type (e.g. "Float3" or "CMOT_Float3"), used to
                cross-check the return value component count

        Returns:
            Dict with ok, error_count, warning_count and per-item errors/warnings
            (line, rule, message, suggestion, snippet)
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params: Dict[str, Any] = {"code": code}
            if output_type is not None:
                params["output_type"] = output_type
            response = unreal.send_command("validate_custom_hlsl", params)
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            return response

        except Exception as e:
            error_msg = f"Error validating custom HLSL: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def validate_custom_expression(
        ctx: Context,
        asset_path: str,
        expression_name: str,
        expression_desc: Optional[str] = None
    ) -> Dict[str, Any]:
        """
        Cross-check a Custom node's input pins against its HLSL code.

        Before editing materials, load the "unreal-material-authoring" skill (use_skill).

        The engine never parses 'Input: X' lines and binds code variables to pins by
        name, so renaming a pin without renaming the code variable (or the reverse)
        only surfaces later as an 'undeclared identifier' compile error. This reads
        Inputs[] and Code together: errors are code references with no pin of that name
        (and 'Input:' labels no pin matches); warnings are pins the code never
        references.

        Args:
            asset_path: Full material asset path
            expression_name: Exact expression object name (must resolve to a Custom node)
            expression_desc: Optional exact expression description for cross-checking

        Returns:
            Dict with pin_count, code_reference_count, errors and warnings
            (each item has kind and detail)
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params: Dict[str, Any] = {"asset_path": asset_path, "expression_name": expression_name}
            if expression_desc is not None:
                params["expression_desc"] = expression_desc
            response = unreal.send_command("validate_custom_expression", params)
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            return response

        except Exception as e:
            error_msg = f"Error validating custom expression: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def set_custom_input_name(
        ctx: Context,
        asset_path: str,
        expression_name: str,
        old_name: str,
        new_name: str,
        expression_desc: Optional[str] = None,
        recompile: bool = False
    ) -> Dict[str, Any]:
        """
        Rename one input pin of a Custom expression node.

        Before editing materials, load the "unreal-material-authoring" skill (use_skill).

        Use this instead of writing the whole `inputs` array (set_material_expression_property
        rejects FArrayProperty values): the C++ side renames the single FCustomInput and
        leaves every upstream connection untouched. The pin name is what binds the code
        variable, so rename the matching variable in `code` in the same pass and re-check
        with validate_custom_expression.

        Args:
            asset_path: Full material asset path
            expression_name: Exact expression object name (must resolve to a Custom node)
            old_name: Current pin name (must be unique on this node)
            new_name: New pin name (must not already exist on this node)
            expression_desc: Optional exact expression description for cross-checking
            recompile: recompile the material after the rename (default false)

        Returns:
            Dict with old_name, new_name, available_inputs (names after the rename)
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params: Dict[str, Any] = {
                "asset_path": asset_path,
                "expression_name": expression_name,
                "old_name": old_name,
                "new_name": new_name,
                "recompile": recompile,
            }
            if expression_desc is not None:
                params["expression_desc"] = expression_desc
            logger.info(f"Renaming Custom input pin {old_name} -> {new_name} on {asset_path}[{expression_name}]")
            response = unreal.send_command("set_custom_input_name", params)
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            return response

        except Exception as e:
            error_msg = f"Error renaming custom input pin: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def add_custom_input(
        ctx: Context,
        asset_path: str,
        input_name: str,
        expression_name: str,
        expression_desc: Optional[str] = None,
        index: Optional[int] = None,
        recompile: bool = False
    ) -> Dict[str, Any]:
        """
        Add one input pin to an existing Custom expression node.

        Before editing materials, load the "unreal-material-authoring" skill (use_skill).

        Use this instead of rewriting the whole `inputs` array: the C++ side inserts a single
        FCustomInput and leaves every existing connection alone (assigning the array from the
        MCP surface is refused for exactly that reason). Pin order IS the HLSL argument order,
        so the pin is appended by default - pass `index` only when the argument order really
        has to change.

        Args:
            asset_path: Full material asset path
            input_name: Name of the pin to add (must not already exist on this node)
            expression_name: Exact expression object name (must resolve to a Custom node)
            expression_desc: Optional exact expression description for cross-checking
            index: Optional insert position (0-based); defaults to appending
            recompile: recompile the material after the insert (default false)

        Returns:
            Dict with expression_name, input_name, inserted_index, input_count and the
            read-back `inputs` (input_name / expression / output_index / connected). The new
            pin starts unconnected, so until something is wired into it the compile reports
            "Custom material ... missing input N (name)" - see get_material_compile_errors
            and its error_details.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params: Dict[str, Any] = {
                "asset_path": asset_path,
                "expression_name": expression_name,
                "input_name": input_name,
                "recompile": recompile,
            }
            if expression_desc is not None:
                params["expression_desc"] = expression_desc
            if index is not None:
                params["index"] = index
            logger.info(f"Adding Custom input pin {input_name} on {asset_path}[{expression_name}]")
            response = unreal.send_command("add_custom_input", params)
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            return response

        except Exception as e:
            error_msg = f"Error adding custom input pin: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def remove_custom_input(
        ctx: Context,
        asset_path: str,
        input_name: str,
        expression_name: str,
        expression_desc: Optional[str] = None,
        recompile: bool = False
    ) -> Dict[str, Any]:
        """
        Remove one input pin from an existing Custom expression node.

        Before editing materials, load the "unreal-material-authoring" skill (use_skill).

        The other pins keep their connections; the removed pin's own wiring goes away with it,
        which the response reports as was_connected / disconnected_from (the upstream node is
        not deleted). Remove the matching variable from `code` in the same pass and re-check
        with validate_custom_expression.

        Args:
            asset_path: Full material asset path
            input_name: Name of the pin to remove (must be unique on this node)
            expression_name: Exact expression object name (must resolve to a Custom node)
            expression_desc: Optional exact expression description for cross-checking
            recompile: recompile the material after the removal (default false)

        Returns:
            Dict with expression_name, removed_name, removed_index, was_connected,
            disconnected_from, input_count and the read-back `inputs`.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params: Dict[str, Any] = {
                "asset_path": asset_path,
                "expression_name": expression_name,
                "input_name": input_name,
                "recompile": recompile,
            }
            if expression_desc is not None:
                params["expression_desc"] = expression_desc
            logger.info(f"Removing Custom input pin {input_name} on {asset_path}[{expression_name}]")
            response = unreal.send_command("remove_custom_input", params)
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            return response

        except Exception as e:
            error_msg = f"Error removing custom input pin: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def connect_material_pin(
        ctx: Context,
        asset_path: str,
        property: str,
        expression_name: Optional[str] = None,
        expression_type: Optional[str] = None,
        expression_desc: Optional[str] = None,
        source_output_name: Optional[str] = None
    ) -> Dict[str, Any]:
        """
        Connect a material expression output to a material property pin
        (e.g. BaseColor, WorldPositionOffset).

        Before editing materials, load the "unreal-material-authoring" skill (use_skill).

        The source expression is resolved by unique expression_name, expression_desc, or expression_type. Ambiguous or missing locators return candidates without modifying the material.

        The source output is resolved through the GetOutputs() virtual, so
        dynamic-output node classes (Custom, SceneTexture, Add...) work as the
        source — the engine python API fails on these.

        Args:
            asset_path: Full material asset path (e.g. "/Game/Materials/M_Test.M_Test")
            property: Material property to connect to (e.g. "WorldPositionOffset")
            expression_name: Optional exact expression object name
            expression_desc: Optional exact match on the expression's Desc field
            expression_type: Optional exact match on the expression class name
                (e.g. "MaterialExpressionCustom")
            source_output_name: Optional source output name (empty/None = first output)

        Returns:
            Dict indicating success or failure
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {
                "asset_path": asset_path,
                "property": property,
            }
            if expression_name is not None:
                params["expression_name"] = expression_name
            if expression_type is not None:
                params["expression_type"] = expression_type
            if expression_desc is not None:
                params["expression_desc"] = expression_desc
            if source_output_name is not None:
                params["source_output_name"] = source_output_name

            logger.info(f"Connecting material pin: {property} in {asset_path}")
            response = unreal.send_command("connect_material_pin", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error connecting material pin: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def convert_static_switch_to_dynamic(ctx: Context, asset_name: str) -> Dict[str, Any]:
        """
        Convert all static-switch parameters in a material to dynamic scalar
        parameters (works around known UE5.5 material issues).

        Args:
            asset_name: Material asset name or path

        Returns:
            Dict indicating how many switches were converted
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"asset_name": asset_name}
            logger.info(f"Converting static switches in: {asset_name}")
            response = unreal.send_command("convert_static_switch_to_dynamic", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error converting static switch: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def recompile_material(
        ctx: Context,
        asset_path: str,
        refresh: bool = True,
        save: bool = False
    ) -> Dict[str, Any]:
        """
        Finish a material edit: material-level refresh + ONE recompile (+ optional save).

        Before editing materials, load the "unreal-material-authoring" skill (use_skill).

        The wiring commands (connect_material_expressions_safe, connect_material_pin,
        disconnect_material_property) deliberately do NOT recompile any more: a recompile rebuilds
        every shader permutation, and wiring runs in loops (a 10-node rebuild is 10 connects), which
        measured ~250ms per connect. Call this ONCE after the graph is in its final shape.

        refresh=True writes a material-level property (flip + flip back), which is the step that
        makes the LIVE RENDER pick expression-only edits up; without it the asset reads back correct
        while the viewport keeps drawing the previous shader/uniforms. That is also why
        get_material_compile_errors can look empty on a material that still draws stale.

        Args:
            asset_path: Full material asset path
            refresh: do the material-level PostEditChange (default true; only skip it if you
                recompile for a reason other than making the viewport current)
            save: save the asset package after recompiling (default false - in a batch, save once at
                the very end instead of per slot)

        Returns:
            Dict with refreshed, recompiled, saved. Shader compilation is asynchronous, so read
            get_material_compile_errors once more after a moment.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            logger.info(f"Recompiling material: {asset_path} (refresh={refresh}, save={save})")
            response = unreal.send_command("recompile_material", {
                "asset_path": asset_path,
                "refresh": refresh,
                "save": save,
            })
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            return response

        except Exception as e:
            error_msg = f"Error recompiling material: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def get_material_compile_errors(ctx: Context, asset_path: str, since: Optional[str] = None) -> Dict[str, Any]:
        """
        Retrieve the real shader compile errors of a material.

        errors_by_feature_level (SM5/SM6, from GetMaterialResource().GetCompileErrors())
        is the AUTHORITY on the material's current compile state. The log fallback is
        split by time so an intermediate-state failure from an earlier edit cannot be
        mistaken for a current one:
          - log_errors_this_compile: lines newer than the boundary
          - log_errors_historical: older lines (or every line when the boundary is unknown)
        Each log entry is {line, timestamp, age_seconds} (timestamp "" and age_seconds
        -1 when the line has no parsable stamp).

        The boundary defaults to the last time an MCP material command changed this
        material, reported as since_boundary / boundary_source
        ("material_mutation" | "explicit_since" | "unknown").

        error_details decodes the known engine wordings into machine-readable findings.
        Right now: "Custom material <node> missing input N (X)" -> kind
        "custom_input_unconnected", which means the pin X exists but NOTHING is wired into
        it (N is 1-based). The engine aborts that node's compile at the first unconnected
        pin, so a fix can uncover the next one. When the node's Description is the default
        ("Custom") the raw text cannot name the expression: expression_name carries it when
        the description is unique, otherwise expression_name_candidates does.

        Args:
            asset_path: Full material asset path (e.g. "/Game/Materials/M_Test.M_Test")
            since: Optional explicit boundary - ISO-8601 ("2026-09-11T05:39:17") or
                epoch seconds. Overrides the recorded mutation time.

        Returns:
            Dict with errors_by_feature_level, error_details, log_errors_this_compile,
            log_errors_historical, since_boundary, boundary_source, scanned_lines,
            matched_lines, total_error_count
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"asset_path": asset_path}
            if since is not None:
                params["since"] = since
            logger.info(f"Getting compile errors for material: {asset_path}")
            response = unreal.send_command("get_material_compile_errors", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error getting material compile errors: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def list_material_expressions(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        List every expression of a material with its diagnostic index, type, name and desc.

        Index is diagnostic only and MUST NOT be used for later operations. Use the current name or desc and reject ambiguous matches.
        Each entry carries object_path (the expression's full object path), so python
        can resolve the expression object directly instead of relying on the implicit
        "outer is the material" form of load_object.

        Args:
            asset_path: Full material asset path (e.g. "/Game/Materials/M_Test.M_Test")

        Returns:
            Dict with expressions (index/type/name/desc/object_path/referenced/inputs),
            count and unreferenced_count; inputs is [{input_name, expression,
            output_index, connected}].

        referenced is true when the expression is reachable from a material property
        input (a live node) and false for an orphan left behind by earlier edits -
        orphans still occupy names and inflate the node count. Clean the graph with
        wipe_material_graph rather than deleting by this flag.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"asset_path": asset_path}
            logger.info(f"Listing expressions of material: {asset_path}")
            response = unreal.send_command("list_material_expressions", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error listing material expressions: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def get_material_graph(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Get the full connection graph of a material in one call.

        Returns every expression-to-expression edge (with input/output pin
        names), every connected material property pin, and all scalar/vector
        parameters with their default values.

        Args:
            asset_path: Full material asset path (e.g. "/Game/Materials/M_Test.M_Test")

        Returns:
            Dict with edges, property_connections, parameters, expression_count
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"asset_path": asset_path}
            logger.info(f"Getting material graph: {asset_path}")
            response = unreal.send_command("get_material_graph", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error getting material graph: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def get_material_expression_property(ctx: Context, asset_path: str, expression_name: str, property: str, expression_desc: Optional[str] = None) -> Dict[str, Any]:
        """
        Read a UPROPERTY of an expression by name or description.

        Index is diagnostic only; name/desc must uniquely identify the current expression.

        Works for any supported property type: numbers, booleans, strings, enums,
        objects, and structs. Compact structs (FVector / FVector2D / FRotator /
        FLinearColor / FColor / FQuat / FIntPoint) come back as number arrays -
        [x, y, z] for a FVector, [r, g, b, a] for a colour - NOT as objects, while
        FTransform comes back as {Rotation, Translation, Scale3D}. Other structs come
        back as field objects, so the value is the value the write path accepts. An
        expression input (FExpressionInput, e.g. Add.A) comes back as
        {expression, output_index, input_name}.

        property="inputs" is a virtual read returning the node's pin list as
        [{input_name, expression, output_index, connected}] - the connections the
        python API cannot read back (FCustomInput.Input is protected there).

        Args:
            asset_path: Full material asset path
            expression_name: Exact expression object name
            expression_desc: Optional exact expression description for cross-checking
            property: UPROPERTY name (e.g. "code", "default_value", "exponent",
                "material_expression_editor_x") or "inputs"

        Returns:
            Dict with value
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"asset_path": asset_path, "expression_name": expression_name, "property": property}
            if expression_desc is not None:
                params["expression_desc"] = expression_desc
            logger.info(f"Getting expression property: {asset_path}[{expression_name}].{property}")
            response = unreal.send_command("get_material_expression_property", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error getting expression property: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def set_material_expression_property(
        ctx: Context,
        asset_path: str,
        expression_name: str,
        property: str,
        value: Any,
        expression_desc: Optional[str] = None,
        recompile: bool = False
    ) -> Dict[str, Any]:
        """
        Write a UPROPERTY of an expression by name or description.

        Before editing materials, load the "unreal-material-authoring" skill (use_skill).

        Writing `property="code"` is checked by the same static HLSL rules the
        validate_custom_hlsl tool exposes; on an error the bridge refuses the write and
        returns `invalid_custom_hlsl` with the offending lines, while warnings are
        written through and returned as `warnings`. The guard lives in the bridge, so it
        also covers python scripts that write code without going through this tool.

        The Custom node's `inputs` array is the one refusal: its elements carry material
        graph wiring, so it is rejected with `unsupported_property_type` and a hint
        pointing at set_custom_input_name (renaming a pin keeps its connections). Every
        other property is written through the shared property reflector, so enums, FText,
        soft references and arrays all work, and a rejection reports
        `supported_shapes` / `available_fields` / `hint` / `failed_index` / `unchanged`.

        Set recompile=true only for the final write of a batch; each recompile
        triggers a full shader compilation.

        Args:
            asset_path: Full material asset path
            expression_name: Exact expression object name
            expression_desc: Optional exact expression description for cross-checking
            property: UPROPERTY name (e.g. "code", "default_value")
            value: number / boolean / string / enum member name, [x,y,z] or {x,y,z} for
                structs, asset path for object properties, array for array properties
            recompile: recompile the material after the write (default false)

        Returns:
            Dict with success, recompiled (and warnings for code writes)
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {
                "asset_path": asset_path,
                "expression_name": expression_name,
                "property": property,
                "value": value,
                "recompile": recompile,
            }
            if expression_desc is not None:
                params["expression_desc"] = expression_desc
            logger.info(f"Setting expression property: {asset_path}[{expression_name}].{property}")
            response = unreal.send_command("set_material_expression_property", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error setting expression property: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def create_material_expression(
        ctx: Context,
        asset_path: str,
        expression_class: str,
        editor_x: int = 0,
        editor_y: int = 0,
        desc: Optional[str] = None,
        code: Optional[str] = None,
        inputs: Optional[list] = None
    ) -> Dict[str, Any]:
        """
        Create a new expression node in a material.

        Before editing materials, load the "unreal-material-authoring" skill (use_skill).

        Args:
            asset_path: Full material asset path
            expression_class: UClass name without module prefix (e.g.
                "MaterialExpressionScalarParameter", "MaterialExpressionCustom")
            editor_x: Node X position (default 0)
            editor_y: Node Y position (default 0)
            desc: Optional description for the node
            code: Custom HLSL source. MaterialExpressionCustom only; the shader is
                linted before the node exists, so a rejected shader leaves the graph
                untouched. Other classes reject this parameter.
            inputs: Custom input pins, applied in order. MaterialExpressionCustom
                only. Accepts ["In_Base", "In_Tint"] or [{"input_name": "In_Base"}].
                Pins carry no type of their own - the pin type comes from whatever
                is connected upstream. One call therefore replaces the old
                create -> set_editor_property("inputs") -> rename dance. For pins on a
                node that already exists use add_custom_input instead, which keeps the
                connections of the other pins.

        Returns:
            Dict with name, desc, type, asset_path, the read-back `inputs`
            (same shape as list_material_expressions) and `outputs`
            ([{output_index, name}] of the LEGAL output names).

        Output name cheat sheet - pass these as source_output_name to
        connect_material_expressions_safe (never guess):
            - TextureSample and other multi-output nodes: "RGB", "R", "G", "B", "A"
            - VectorParameter / TextureCoordinate / ScalarParameter and every other
              single-output node: "" (empty string, not "RGB" / "UV")
        Read the `outputs` array in the response when unsure; it comes from the same
        GetOutputs() the connect path validates against.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {
                "asset_path": asset_path,
                "expression_class": expression_class,
                "editor_x": editor_x,
                "editor_y": editor_y,
            }
            if desc is not None:
                params["desc"] = desc
            if code is not None:
                params["code"] = code
            if inputs is not None:
                params["inputs"] = inputs
            logger.info(f"Creating expression {expression_class} in: {asset_path}")
            response = unreal.send_command("create_material_expression", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error creating material expression: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def delete_material_expression(
        ctx: Context,
        asset_path: str,
        expression_name: Optional[str] = None,
        expression_desc: Optional[str] = None
    ) -> Dict[str, Any]:
        """
        Delete one expression by unique name or description. Index is deprecated.
        Duplicate or missing locators return candidates and do not delete.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"asset_path": asset_path}
            if expression_name is not None:
                params["expression_name"] = expression_name
            if expression_desc is not None:
                params["expression_desc"] = expression_desc
            logger.info(f"Deleting expression from: {asset_path}")
            response = unreal.send_command("delete_material_expression", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error deleting material expression: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def delete_material_expressions(
        ctx: Context,
        asset_path: str,
        expressions: list[Dict[str, Optional[str]]]
    ) -> Dict[str, Any]:
        """Delete multiple expressions by name/desc, processing every item independently.

        The top-level `success` reflects the items: it is false when every item failed, and a
        batch that failed some of them also sets `partial` with `failed_count` (deleted_count
        and requested_count come alongside). The per-item shapes are in `results` - ALWAYS
        read them, a failing item does not raise.

        Args:
            asset_path: Full material asset path
            expressions: One selector object per expression, using the same keys as
                delete_material_expression (expression_name / expression_desc /
                expression_type); a selector matching nothing or several expressions fails
                only its own item
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            response = unreal.send_command("delete_material_expressions", {
                "asset_path": asset_path,
                "expressions": expressions,
            })
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            return {"success": False, "message": f"Error deleting material expressions: {e}"}

    @mcp.tool()
    def wipe_material_graph(
        ctx: Context,
        asset_path: str,
        max_iterations: int = 32
    ) -> Dict[str, Any]:
        """
        Delete every expression of a material and prove the graph is empty.

        Use this instead of "delete everything" loops: the engine's bulk delete
        removes from the array it iterates, so a single pass only clears part of the
        graph (observed 102 -> 50 -> 24 -> 11). This tool loops "delete + GC" until
        the expression count is really 0 and reports what it did.

        Args:
            asset_path: Full material asset path (e.g. "/Game/Materials/M_Test.M_Test")
            max_iterations: Safety bound on the delete + GC loop (default 32)

        Returns:
            Dict with iterations, deleted_total, remaining. remaining != 0 means the
            graph could not be emptied: success is false and error is "wipe_incomplete".
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"asset_path": asset_path, "max_iterations": max_iterations}
            logger.info(f"Wiping material graph: {asset_path}")
            response = unreal.send_command("wipe_material_graph", params)
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            return response
        except Exception as e:
            error_msg = f"Error wiping material graph: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def disconnect_material_property(ctx: Context, asset_path: str, property: str) -> Dict[str, Any]:
        """
        Disconnect the expression currently feeding a material property pin.

        Args:
            asset_path: Full material asset path
            property: Property name (e.g. "BaseColor", "WorldPositionOffset")

        Returns:
            Dict with success, property, was_connected
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"asset_path": asset_path, "property": property}
            logger.info(f"Disconnecting property {property} on: {asset_path}")
            response = unreal.send_command("disconnect_material_property", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error disconnecting material property: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def get_material_parameters(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Read all scalar/vector parameters of a material or material instance.

        For a material instance, each entry includes the override value (or the
        parent default) and an `overridden` flag.

        Args:
            asset_path: Full asset path (material or material instance)

        Returns:
            Dict with asset_kind (material / material_instance) and parameters array
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"asset_path": asset_path}
            logger.info(f"Getting material parameters: {asset_path}")
            response = unreal.send_command("get_material_parameters", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error getting material parameters: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def set_material_parameters(ctx: Context, asset_path: str, values: Dict[str, Any], recompile: bool = False) -> Dict[str, Any]:
        """
        Batch-set scalar/vector parameters by name on a material or material instance.

        Scalar values are plain numbers; vector values are {r,g,b,a} objects.
        Unknown parameter names are reported in the `missing` array and skipped.

        Args:
            asset_path: Full asset path (material or material instance)
            values: Mapping of parameter name to value, e.g.
                {"WaveHeight": 6.0, "DeepColor": {"r": 0.1, "g": 0.4, "b": 0.8}}
            recompile: recompile after the batch (materials only, default false)

        Returns:
            Dict with updated and missing arrays
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"asset_path": asset_path, "values": values, "recompile": recompile}
            logger.info(f"Setting material parameters on: {asset_path}")
            response = unreal.send_command("set_material_parameters", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error setting material parameters: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def preview_material_expression(
        ctx: Context,
        asset_path: str,
        expression_name: Optional[str] = None,
        expression_desc: Optional[str] = None,
        output_index: int = 0,
        channel: str = "rgba",
        size: int = 256,
        keep_temp: bool = False,
        wait_s: float = 20.0,
        timeout: Optional[float] = None
    ) -> Dict[str, Any]:
        """
        Render ONE expression of a material into a PNG, so a graph layer can be LOOKED AT
        instead of guessed (a wear mask, the difference between two UV setups, how much a
        tint chain darkens a colour).

        Before editing materials, load the "unreal-material-authoring" skill (use_skill).

        DIAGNOSTIC ONLY - this is material-internal inspection, NOT visual acceptance:
        there is no lighting, no camera and no scene, so it never replaces looking at the
        result in the editor viewport (see .codemaker/rules/rules.mdc). The tool returns
        the image plus metadata, never a conclusion.

        How it works: draw_material_to_render_target evaluates a MATERIAL, not a node, so the
        node is routed into EmissiveColor in a throw-away material under /Game/MCP/_Preview/.
        That material is BUILT FRESH by the bridge command build_material_preview: it receives
        a clone of the target node plus its upstream chain, and is recompiled synchronously.
        (Copying the source material instead does not work: the copy keeps rendering the
        source's compiled shader, so the preview would silently show the old material.) The
        original asset is never modified; the throw-away material is deleted again unless
        keep_temp=true.

        World-space / view-dependent nodes are NOT overridden: the preview quad sits at
        the origin with a fixed view, so anything reading WorldPosition, VertexNormalWS or
        CameraVector degrades to a constant. The result carries `purpose: diagnostic` and
        the raw samples so the values can be judged numerically.

        Args:
            asset_path: Material asset path ("/Game/Materials/M_X" or ".../M_X.M_X")
            expression_name: Exact expression object name (from list_material_expressions)
            expression_desc: Or the expression's Desc ("WEAR") - must match exactly one node
            output_index: Output slot of the node (0 = first). Slots beyond the node's outputs
                report output_index_not_found with the available outputs.
            channel: "rgba" (node output as-is) or "r"/"g"/"b"/"a" (single channel through
                a ComponentMask, so a mask shows up as a grey image)
            size: Render target edge in pixels, 8..512 (default 256)
            keep_temp: Keep the throw-away preview material for inspection (default false)
            wait_s: How long to keep re-rendering while the image is still changing (default 20)
            timeout: Optional bridge receive timeout in seconds (shader compile + draw)

        Returns:
            On success {"status": "success", "result": {png_path, width, height, rt_format,
            encoding, source_asset_path, temp_asset_path, expression, output_index, channel,
            copied_nodes, samples, render_attempts, render_settled, waited_ms, purpose, ...}}.
            `samples` holds the centre pixel and a 3x3 mean of the rendered target in the
            material's own 0..1 linear units (the numeric cross-check for tests).
            On failure {"status": "error", "error": <error_code>, "detail": ..., "result":
            {...}} with codes such as material_not_found, expression_not_found (candidates),
            ambiguous_expression, invalid_channel, output_index_not_found, temp_asset_exists,
            temp_create_failed, clone_failed, rt_create_failed, export_failed.
        """
        import time
        from unreal_mcp_server import get_unreal_connection

        def run_step(function: str, payload: Dict[str, Any]):
            """Run one editor-side step (build / render / cleanup) and return its payload."""
            # Short inline command: the implementation lives in Content/Python, which the editor
            # python environment imports from its own sys.path. The reload keeps edits to that
            # module live inside a running editor session (the editor caches imports).
            code = (
                "import json as _json\n"
                "import importlib\n"
                "import %s as _preview\n"
                "importlib.reload(_preview)\n"
                "_result = _preview.run_step('%s', _json.loads(%r))\n"
                "print('%s' + _json.dumps(_result, default=str))\n"
            ) % (PREVIEW_MODULE, function, json.dumps(payload), PREVIEW_SENTINEL)

            params = {"command": code}
            if timeout is not None:
                params["timeout_ms"] = int(timeout * 1000)
            response = unreal.send_command("execute_python_command", params, recv_timeout=timeout)
            if not response:
                return {"success": False, "error_code": "no_response",
                        "message": "No response from Unreal Engine"}
            step = _extract_preview_payload(response)
            if step is None:
                return {"success": False, "error_code": "preview_failed",
                        "message": response.get("error") or "no %s line in the editor output" % PREVIEW_SENTINEL,
                        "editor_output": _editor_log_text(response)[-4000:]}
            return step

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"status": "error", "error": "Failed to connect to Unreal Engine"}

            request = {
                "asset_path": asset_path,
                "expression_name": expression_name,
                "expression_desc": expression_desc,
                "output_index": output_index,
                "channel": channel,
                "size": size,
            }
            logger.info(f"Previewing expression {expression_desc or expression_name} of {asset_path}")

            built = run_step("build", request)
            if not built.get("success"):
                # A failure after the preview material was created reports its path: clean it up.
                partial = built.get("state")
                if partial is None and built.get("temp_asset_path"):
                    partial = {"temp_asset_path": built["temp_asset_path"]}
                if partial and not keep_temp:
                    run_step("cleanup", partial)
                return {"status": "error", "error": built.get("error_code") or "build_failed",
                        "detail": built.get("message", ""), "result": built}

            state = built["state"]
            try:
                # The build step recompiles synchronously, so the first draw is normally the final
                # one; keep polling until the image stops changing so a late shader/uniform refresh
                # can never be handed back as the preview.
                started = time.monotonic()
                deadline = started + max(0.0, wait_s)
                previous = None
                attempts = 0
                settled = False
                while True:
                    drawn = run_step("render", {"state": state, "write_png": False})
                    if not drawn.get("success"):
                        return {"status": "error", "error": drawn.get("error_code") or "render_failed",
                                "detail": drawn.get("message", ""), "result": drawn}
                    attempts += 1
                    samples = drawn.get("samples")
                    if previous is not None and samples == previous:
                        settled = True
                        break
                    previous = samples
                    if time.monotonic() >= deadline:
                        break
                    time.sleep(0.4)

                final = run_step("render", {"state": state, "write_png": True})
                if not final.get("success"):
                    return {"status": "error", "error": final.get("error_code") or "render_failed",
                            "detail": final.get("message", ""), "result": final}

                result = dict(built["info"])
                result.update({
                    "purpose": "diagnostic",
                    "png_path": final.get("png_path"),
                    "width": final.get("width"),
                    "height": final.get("height"),
                    "rt_format": final.get("rt_format"),
                    "encoding": final.get("encoding"),
                    "samples": final.get("samples"),
                    "render_attempts": attempts + 1,
                    "render_settled": settled,
                    "waited_ms": int((time.monotonic() - started) * 1000),
                    "temp_asset_kept": keep_temp,
                })
                return {"status": "success", "result": result}
            finally:
                if not keep_temp:
                    run_step("cleanup", state)

        except Exception as e:
            error_msg = f"Error previewing material expression: {e}"
            logger.error(error_msg)
            return {"status": "error", "error": error_msg}

    logger.info("Material tools registered successfully")
