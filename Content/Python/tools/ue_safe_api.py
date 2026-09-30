"""
Safe Unreal API wrappers for Unreal MCP.

封装实战中反复踩坑的编辑器 Python API（来源见 Docs/mcp-bugfix-plan.md）：
- 资产创建/删除的模态框卡死问题（P0-1）：直接调 create/delete 遇到 dirty 资产或
  重名会弹模态框、阻塞 GameThread、冻死整个 MCP bridge。
- PostProcessVolume blendable 绑定不生效、着色模型枚举、表达式连线、
  custom depth 等 API 坑位（P2-2）。

所有工具通过 execute_python_command 在编辑器内执行；编辑器侧脚本用
`RESULT:<state>[:<detail>]` 哨兵行输出结果，保证返回结构化 JSON 而非残缺日志。
"""

import logging
from typing import Dict, Any, List, Optional
from mcp.server.fastmcp import FastMCP, Context

logger = logging.getLogger("UnrealMCP")


def _run_editor_python(code: str) -> Dict[str, Any]:
    """在编辑器中执行 python 并返回原始响应。"""
    from unreal_mcp_server import get_unreal_connection

    conn = get_unreal_connection()
    if not conn:
        return {"status": "error", "error": "Failed to connect to Unreal Engine"}
    return conn.send_command("execute_python_command", {"command": code})


def _stdout_text(response: Dict[str, Any]) -> str:
    """提取编辑器 python 的日志输出（print 落在这里）。"""
    result = response.get("result") or {}
    parts = []
    for entry in (result.get("log") or []):
        if isinstance(entry, dict):
            parts.append(str(entry.get("output", "")))
    return "".join(parts)


def _parse_sentinel(response: Dict[str, Any]) -> Dict[str, Any]:
    """把编辑器输出中的 RESULT:<state>[:<detail>] 哨兵转成结构化结果。

    state == 'ok' 视为成功，其余（asset_exists / asset_not_found / ...）
    原样作为错误码返回，绝不弹模态框。
    """
    if response.get("status") == "error":
        return {
            "status": "error",
            "error": response.get("error", "editor python failed"),
        }

    for line in _stdout_text(response).splitlines():
        line = line.strip()
        if line.startswith("RESULT:"):
            body = line[len("RESULT:"):].strip()
            if ":" in body:
                state, detail = body.split(":", 1)
            else:
                state, detail = body, ""
            if state == "ok":
                return {"status": "success", "result": detail}
            return {"status": "error", "error": state, "detail": detail}

    # 没有哨兵：python 异常等，错误信息应在 status/error 里
    return {
        "status": "error",
        "error": response.get("error") or "no RESULT sentinel in output",
        "output": _stdout_text(response),
    }


def _bridge(cmd: str, params: Dict[str, Any]) -> Dict[str, Any]:
    """Forward to a bridge command (the C++ command layer).

    These tools used to assemble editor-side python and run it through
    execute_python_command; the implementations now live in C++ so that editor-side
    scripts (execute_python_*) can reach the same code by calling the command over the
    loopback, and so that a modal dialog can never be raised from the python path.
    """
    from unreal_mcp_server import get_unreal_connection

    conn = get_unreal_connection()
    if not conn:
        return {"status": "error", "error": "Failed to connect to Unreal Engine"}
    response = conn.send_command(cmd, params)
    return response if isinstance(response, dict) else {"status": "error", "error": "no response from Unreal"}


def _payload(response: Dict[str, Any]) -> Dict[str, Any]:
    """The command's data object, through BOTH envelopes.

    The bridge wraps every answer as {"status", "result"}, and a handler that used
    CreateSuccessResponse puts {"success", "data"} inside that. Unwrapping only the outer one
    returns the inner envelope, so every field lookup misses and each wrapper falls back to its
    own default error code - which is exactly how "the command never ran" looked once.
    """
    if not isinstance(response, dict):
        return {}
    inner = response.get("result")
    if isinstance(inner, dict):
        if isinstance(inner.get("data"), dict):
            return inner["data"]
        return inner
    if isinstance(response.get("data"), dict):
        return response["data"]
    return response


def register_ue_safe_api_tools(mcp: FastMCP):
    """Register safe API tools with the MCP server."""

    @mcp.tool()
    def create_asset_safe(
        ctx: Context,
        asset_name: str,
        package_path: str,
        asset_class_name: str,
        factory_name: Optional[str] = None,
        recreate: bool = False
    ) -> Dict[str, Any]:
        """Create an asset WITHOUT the rename/replace modal dialog.

        Checks existence first: if the asset already exists it returns a structured
        'asset_exists' error instead of popping the blocking dialog that freezes the
        whole MCP bridge. With recreate=True an existing asset is silently saved and
        deleted first (no save prompt), then recreated — if the deletion is still
        pending in the asset registry it returns 'delete_pending' (retry) instead of
        risking a modal. Uses AssetToolsHelpers.get_asset_tools().create_asset()
        (unreal.EditorAssetLibrary.create_asset does not exist).

        Args:
            asset_name: Asset name, e.g. "M_MyMaterial"
            package_path: Content path, e.g. "/Game/Materials"
            asset_class_name: unreal class name, e.g. "Material", "MaterialFunction",
                "MaterialInstanceConstant", "ParticleSystem", "NiagaraSystem", "SoundCue"
            factory_name: Optional factory class override
                (default: built-in mapping or "<class>FactoryNew" convention)
            recreate: Delete an existing asset first, then create (default False)

        Returns:
            Dict with status success (result = created asset path) or a structured error
            (asset_exists / delete_pending / unknown_class / no_factory / create_failed)
        """
        response = _bridge("create_asset_safe", {
            "asset_name": asset_name,
            "package_path": package_path,
            "asset_class": asset_class_name,
            "factory": factory_name,
            "recreate": bool(recreate),
        })
        data = _payload(response)
        if data.get("created"):
            return {"status": "success", "result": data.get("asset_path")}
        reason = data.get("reason") or str(
            data.get("error_code") or data.get("error") or response.get("error") or "create_failed")
        return {"status": "error", "error": reason.split(":")[0],
                "detail": data.get("asset_path") or data.get("delete_result")}
    @mcp.tool()
    def delete_asset_safe(
        ctx: Context,
        asset_path: str,
        save_dirty: bool = True
    ) -> Dict[str, Any]:
        """Delete an asset WITHOUT the save-changes modal dialog.

        If the asset has unsaved modifications it is silently saved first
        (only_if_is_dirty=True), so UE never pops the blocking 'Save changes?'
        dialog that freezes the whole MCP bridge. All failures return structured
        errors instead of dialogs.

        Args:
            asset_path: Full asset path, e.g. "/Game/Materials/M_Old.M_Old"
            save_dirty: Silently save dirty assets before deleting (default True)

        Returns:
            Dict with status success (result = deleted path) or a structured error
            (asset_not_found / delete_failed)
        """
        response = _bridge("delete_asset_safe", {
            "asset_path": asset_path,
            "save_dirty": bool(save_dirty),
        })
        data = _payload(response)
        if data.get("deleted"):
            return {"status": "success", "result": asset_path,
                    "saved_before_delete": data.get("saved_before_delete", False)}
        error = str(data.get("error_code") or data.get("error") or response.get("error") or "")
        if error.startswith("asset_not_found"):
            return {"status": "error", "error": "asset_not_found", "detail": asset_path}
        reason = data.get("reason") or error or "delete_failed"
        return {"status": "error", "error": reason.split(":")[0],
                "detail": data.get("referencers") or data.get("detail")}
    @mcp.tool()
    def add_blendable_to_post_volume(
        ctx: Context,
        material_path: str,
        volume_name: Optional[str] = None,
        weight: float = 1.0
    ) -> Dict[str, Any]:
        """Bind a post-process material to a PostProcessVolume AND make it take effect.

        Before editing materials, load the "unreal-material-authoring" skill (use_skill).

        Setting PostProcessSettings.blendables directly is read-protected in python.
        The only working call is volume.add_or_update_blendable(mat, weight), and the
        volume's 'enabled' flag must then be toggled False->True to refresh the render
        state — otherwise the binding exists but has no visual effect.

        Args:
            material_path: Post-process material, e.g. "/Game/Materials/M_PP.M_PP"
            volume_name: PostProcessVolume actor name (falls back to actor label);
                None = match a volume whose label equals the material's short
                name, else the unique volume; ambiguous candidates return
                volume_ambiguous listing name(label) pairs
            weight: Blendable weight (0..1)

        Returns:
            Dict with status success (result = volume name) or a structured error
            (material_not_found / volume_not_found / volume_ambiguous)
        """
        response = _bridge("add_blendable_to_post_volume", {
            "material_path": material_path,
            "volume_name": volume_name,
            "weight": float(weight),
        })
        data = _payload(response)
        if data.get("volume_name"):
            return {"status": "success", "result": data.get("volume_name"),
                    "added": data.get("added"), "updated": data.get("updated"),
                    "weight": data.get("weight")}
        error = str(data.get("error_code") or data.get("error") or response.get("error") or "bind_failed")
        return {"status": "error", "error": error.split(":")[0],
                "detail": data.get("candidates") or data.get("detail") or error}
    @mcp.tool()
    def connect_material_expressions_safe(
        ctx: Context,
        asset_path: str,
        source_name: str,
        target_name: str,
        source_desc: Optional[str] = None,
        target_desc: Optional[str] = None,
        input_name: str = "",
        source_output_name: str = ""
    ) -> Dict[str, Any]:
        """Connect one material expression's output to another expression's input.

        Before editing materials, load the "unreal-material-authoring" skill (use_skill).

        FExpressionInput is not python-constructible. Resolve source and target by current name/desc from list_material_expressions, then connect through the C++ bridge command `connect_material_expression`, which resolves the source output via the GetOutputs() virtual — so dynamic-output node classes (Add/Subtract, SceneTexture, python-created Custom...) work as sources, unlike the engine python API that only reads the member Outputs array.

        Args:
            asset_path: Material asset path ('/Game/M/M_X' or '/Game/M/M_X.M_X')
            source_name: Exact source expression name
            target_name: Exact target expression name
            source_desc: Optional exact source description
            target_desc: Optional exact target description
            input_name: Target input name ('' = first input; C++ member names like
                A/B/Alpha/ExponentIn; Custom node inputs use their declared input_name)
            source_output_name: Source output name ('' = first output; single-output
                nodes use ''; named outputs like SceneTexture's 'Color' should be
                passed explicitly)

        Returns:
            Dict with status success or a structured error (not_material / expression_not_found / ambiguous_expression / expression_not_in_material / output_not_found with available_outputs / input_not_found with available_inputs)
        """
        from unreal_mcp_server import get_unreal_connection

        # 归一路径：末段无 '.' 时补 '.basename'（load_object 需要 <pkg>.<obj>:<sub> 形式）
        path = asset_path.rstrip('/')
        last = path.rsplit('/', 1)[-1]
        if '.' not in last:
            path = path + '.' + last

        conn = get_unreal_connection()
        if not conn:
            return {"status": "error", "error": "Failed to connect to Unreal Engine"}

        listing = conn.send_command("list_material_expressions", {"asset_path": path})
        if not listing or listing.get("status") != "success":
            return listing or {"status": "error", "error": "list_material_expressions failed"}

        expressions = (listing.get("result") or {}).get("expressions") or []
        count = len(expressions)
        def resolve(name, desc):
            matches = [e for e in expressions if (desc is not None and e.get("desc") == desc) or (desc is None and e.get("name") == name)]
            return matches[0] if len(matches) == 1 else None
        src = resolve(source_name, source_desc)
        dst = resolve(target_name, target_desc)
        if src is None or dst is None:
            return {"status": "error", "error": "resolve_failed", "detail": "missing or ambiguous expression locator", "candidates": expressions}
        src_name = src.get("name")
        dst_name = dst.get("name")

        # Route through the C++ bridge command instead of
        # unreal.MaterialEditingLibrary.connect_material_expressions: the engine
        # python API resolves the SOURCE output via the member `Outputs` array,
        # which stays empty for dynamic-output nodes (Add/Subtract, SceneTexture,
        # python-created Custom...) so it always returns False for them. The C++
        # command uses the GetOutputs() virtual (compiler/graph UI path) instead.
        params = {
            "asset_path": path,
            "source_name": src_name,
            "target_name": dst_name,
            "output_name": source_output_name,
            "input_name": input_name,
        }
        if source_desc is not None:
            params["source_desc"] = source_desc
        if target_desc is not None:
            params["target_desc"] = target_desc
        result = conn.send_command("connect_material_expression", params)
        if not result or result.get("status") != "success":
            return result or {"status": "error", "error": "connect_material_expression failed"}
        connect_result = result.get("result") or {}
        graph = conn.send_command("get_material_graph", {"asset_path": path})
        if graph and graph.get("status") == "success":
            graph_result = graph.get("result") or {}
            index_to_name = {e.get("index"): e.get("name") for e in expressions}
            for edge in graph_result.get("edges") or []:
                if (index_to_name.get(edge.get("from_index")) == src_name and
                        index_to_name.get(edge.get("to_index")) == dst_name and
                        edge.get("input_name") == connect_result.get("input_name", input_name)):
                    return {
                        "status": "success",
                        "result": {
                            "source_name": src_name,
                            "source_desc": src.get("desc", ""),
                            "target_name": dst_name,
                            "target_desc": dst.get("desc", ""),
                            "input_name": edge.get("input_name", input_name),
                            "output_name": edge.get("output_name", source_output_name),
                            "connected": True,
                        },
                    }
        return {
            "status": "success",
            "result": {
                "source_name": src_name,
                "source_desc": src.get("desc", ""),
                "target_name": dst_name,
                "target_desc": dst.get("desc", ""),
                "input_name": connect_result.get("input_name", input_name),
                "output_name": connect_result.get("output_name", source_output_name),
                "connected": True,
            },
        }

    @mcp.tool()
    def set_actor_custom_depth_safe(
        ctx: Context,
        actor_name: str,
        stencil_value: int = 1,
        enabled: bool = True
    ) -> Dict[str, Any]:
        """Enable custom depth + stencil on ALL primitive components of an actor.

        The property names are render_custom_depth and custom_depth_stencil_value
        (NOT custom_stencil_value), set per component. Note the project also needs
        r.CustomDepth=3 for stencil writing (check via get_console_variable).

        Args:
            actor_name: Level actor name
            stencil_value: Stencil value 0-255 (default 1)
            enabled: Enable (True) or disable (False) custom depth

        Returns:
            Dict with status success or a structured error (actor_not_found)
        """
        response = _bridge("set_actor_custom_depth_safe", {
            "name": actor_name,
            "stencil_value": int(stencil_value),
            "enabled": bool(enabled),
        })
        data = _payload(response)
        if data.get("component_count") is not None:
            return {"status": "success",
                    "result": "%s (%d components)" % (data.get("name") or actor_name,
                                                      data.get("component_count")),
                    "enabled": data.get("enabled"), "stencil_value": data.get("stencil_value")}
        error = str(data.get("error_code") or data.get("error") or response.get("error") or "actor_not_found")
        return {"status": "error", "error": error.split(":")[0], "detail": actor_name}
    @mcp.tool()
    def set_actor_location_safe(
        ctx: Context,
        actor_name: str,
        location: List[float],
        sweep: bool = False,
        teleport: bool = False
    ) -> Dict[str, Any]:
        """Move an actor with the correct set_actor_location signature.

        The python overload REQUIRES the sweep argument positionally:
        actor.set_actor_location(loc, sweep, teleport) — omitting it is a TypeError.

        Args:
            actor_name: Level actor name
            location: [x, y, z] world location
            sweep: Sweep against blocking geometry (default False)
            teleport: Teleport physics (default False)

        Returns:
            Dict with status success or a structured error (actor_not_found)
        """
        if not isinstance(location, list) or len(location) != 3:
            return {"status": "error", "error": "invalid_params",
                    "detail": "location must be a list of 3 floats"}
        response = _bridge("set_actor_location_safe", {
            "name": actor_name,
            "location": [float(location[0]), float(location[1]), float(location[2])],
            "sweep": bool(sweep),
            "teleport": bool(teleport),
        })
        data = _payload(response)
        if data.get("moved") is not None or data.get("name"):
            return {"status": "success", "result": data.get("name") or actor_name,
                    "moved": data.get("moved")}
        error = str(data.get("error_code") or data.get("error") or response.get("error") or "actor_not_found")
        return {"status": "error", "error": error.split(":")[0], "detail": actor_name}
    @mcp.tool()
    def list_enum_values(ctx: Context, enum_name: str) -> Dict[str, Any]:
        """List the valid member names of a unreal.* enum class.

        UE 5.5 enum member names differ a lot from docs/older versions (e.g.
        BlendableLocation has no BEFORE_TONEMAPPING — the members are
        BL_REPLACING_TONEMAPPER, BL_SCENE_COLOR_AFTER_DOF, ...). Probe before
        hardcoding member names in scripts.

        Args:
            enum_name: Enum class name without the 'unreal.' prefix,
                e.g. "BlendableLocation", "MaterialShadingModel"

        Returns:
            Dict with status success and 'values' (member name list), or a
            structured error (unknown_enum / not_an_enum)
        """
        response = _bridge("list_enum_values", {"enum_name": enum_name})
        data = _payload(response)
        names = data.get("names")
        if names is None:
            values = data.get("values") or []
            names = [v.get("name") for v in values if isinstance(v, dict)]
        if not names:
            error = str(data.get("error_code") or data.get("error") or response.get("error") or "unknown_enum")
            return {"status": "error", "error": error.split(":")[0], "detail": data.get("tried")}
        return {"status": "success", "result": ",".join(names), "values": names,
                "enum_path": data.get("enum_path")}
