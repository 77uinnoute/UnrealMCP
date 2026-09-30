"""
Unreal Engine MCP Server

A simple MCP server for interacting with Unreal Engine.
"""

import logging
import socket
import sys
import json
import struct
import argparse
from contextlib import asynccontextmanager
from typing import AsyncIterator, Dict, Any, Optional
from mcp.server.fastmcp import FastMCP

# Configure logging with more detailed format
logging.basicConfig(
    level=logging.DEBUG,  # Change to DEBUG level for more details
    format='%(asctime)s - %(name)s - %(levelname)s - [%(filename)s:%(lineno)d] - %(message)s',
    handlers=[
        logging.FileHandler('unreal_mcp.log'),
        # logging.StreamHandler(sys.stdout) # Remove this handler to unexpected non-whitespace characters in JSON
    ]
)
logger = logging.getLogger("UnrealMCP")

# Configuration
UNREAL_HOST = "127.0.0.1"
UNREAL_PORT = 55557
# Receive timeout (seconds), overridable via --recv-timeout CLI arg
# Must be >= the C++ bridge GameThread timeout (120s), otherwise slow commands
# (screenshots, heavy python) fail here while still succeeding in the editor.
RECV_TIMEOUT = 120.0

class UnrealConnection:
    """Connection to an Unreal Engine instance."""
    
    def __init__(self):
        """Initialize the connection."""
        self.socket = None
        self.connected = False
    
    def connect(self) -> bool:
        """Connect to the Unreal Engine instance."""
        try:
            # Close any existing socket
            if self.socket:
                try:
                    self.socket.close()
                except:
                    pass
                self.socket = None
            
            logger.info(f"Connecting to Unreal at {UNREAL_HOST}:{UNREAL_PORT}...")
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.socket.settimeout(10)  # 10 second timeout
            
            # Set socket options for better stability
            self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
            
            # Set larger buffer sizes
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
            
            self.socket.connect((UNREAL_HOST, UNREAL_PORT))
            self.connected = True
            logger.info("Connected to Unreal Engine")
            return True
            
        except Exception as e:
            logger.error(f"Failed to connect to Unreal: {e}")
            self.connected = False
            return False
    
    def disconnect(self):
        """Disconnect from the Unreal Engine instance."""
        if self.socket:
            try:
                self.socket.close()
            except:
                pass
        self.socket = None
        self.connected = False

    def _drop_connection(self):
        """Close the current socket and mark disconnected (no reconnect logic)."""
        if self.socket:
            try:
                self.socket.close()
            except Exception:
                pass
        self.socket = None
        self.connected = False

    def receive_full_response(self, sock, buffer_size=4096, timeout=None) -> bytes:
        """Receive a complete response from Unreal, handling chunked data.

        帧完整性只按**字节**判定：累积缓冲区始终保持 bytes，直接交给 json.loads
        （它按 UTF-8 整段解码）。收满之前的任何中间态都不是错误 ——
        半截 JSON 抛 JSONDecodeError，多字节字符被读块切开抛 UnicodeDecodeError，
        两者同等视为"帧未完整"，继续收即可，MUST NOT 让调用失败。
        """
        chunks = []
        sock.settimeout(timeout if timeout is not None else RECV_TIMEOUT)
        try:
            while True:
                chunk = sock.recv(buffer_size)
                if not chunk:
                    if not chunks:
                        raise Exception("Connection closed before receiving data")
                    # 对端在响应中途关闭：已收到部分字节，但帧不完整（半帧）。
                    # MUST NOT 用 break —— break 跳出循环后函数体结束 ⇒ 隐式返回
                    # None，上层 send_command 随即执行 response_data.decode('utf-8')
                    # 会抛 AttributeError: 'NoneType' object has no attribute 'decode'，
                    # 把"传输被截断"伪装成 python 属性错误（既不说明半帧、也不带字节数）。
                    # 改为返回结构化错误的 JSON 字节，与超时分支同形态，仅错误码区分
                    # 触发条件（对端关闭 vs 本端超时 response_incomplete）。
                    data = b''.join(chunks)
                    logger.error(
                        f"Connection closed mid-frame after {len(data)} bytes received"
                    )
                    return json.dumps({
                        "status": "error",
                        "error": "connection_closed_mid_frame",
                        "detail": (
                            f"connection closed by peer after receiving {len(data)} bytes; "
                            f"buffered data is not a complete response (truncated mid-frame)"
                        ),
                        "bytes_received_so_far": len(data),
                    }).encode('utf-8')
                chunks.append(chunk)

                # Process the data received so far (bytes, never decoded mid-frame)
                data = b''.join(chunks)

                # Try to parse as JSON to check if complete
                try:
                    json.loads(data)
                    logger.info(f"Received complete response ({len(data)} bytes)")
                    return data
                except (json.JSONDecodeError, UnicodeDecodeError):
                    # Not complete yet (half JSON, or a multi-byte character split
                    # across the read-block boundary) — keep receiving.
                    logger.debug(f"Received partial response ({len(data)} bytes), waiting for more data...")
                    continue
                except Exception as e:
                    # 解析阶段的其它异常同样按"未完整"处理，但留下痕迹（不静默吞）
                    logger.warning(f"Error parsing buffered response ({len(data)} bytes): {str(e)}")
                    continue
        except socket.timeout:
            logger.warning("Socket timeout during receive")
            if chunks:
                # 超时只认完整帧：半截数据绝不当响应返回，回报结构化错误 + 已收字节数
                data = b''.join(chunks)
                try:
                    json.loads(data)
                    logger.info(f"Received complete response on timeout ({len(data)} bytes)")
                    return data
                except (json.JSONDecodeError, UnicodeDecodeError):
                    logger.error(f"Timeout with incomplete response ({len(data)} bytes received)")
                    return json.dumps({
                        "status": "error",
                        "error": "response_incomplete",
                        "detail": (
                            f"timeout after receiving {len(data)} bytes; buffered data is not a "
                            f"complete response (truncated mid-frame)"
                        ),
                        "bytes_received_so_far": len(data),
                    }).encode('utf-8')
            raise Exception("Timeout receiving Unreal response")
        except Exception as e:
            logger.error(f"Error during receive: {str(e)}")
            raise
    
    def send_command(self, command: str, params: Dict[str, Any] = None, recv_timeout: float = None) -> Optional[Dict[str, Any]]:
        """Send a command to Unreal Engine and get the response.

        One command = one connection: a fresh socket is opened per command and closed
        as soon as the response is in hand (the C++ bridge also closes right after
        responding). No persistent connection state — a hung/zombie connection can
        only ever affect its own command, never wedge the bridge for other clients
        (multi-instance / restart scenarios self-heal).
        """
        try:
            if not self.connect():
                logger.error("Failed to connect to Unreal Engine for command")
                return None

            command_obj = {
                "type": command,  # Use "type" instead of "command"
                "params": params or {}  # Use Unity's params or {} pattern
            }

            payload = json.dumps(command_obj).encode('utf-8')
            logger.info(f"Sending command: {command} ({len(payload)} bytes, framed, per-command connection)")
            # 4 字节大端长度前缀 + payload：bridge 按长度精确收满后再解析，
            # 避免 TCP 分片下大 payload（如内嵌 HLSL 的多行 python）中段丢字节。
            self.socket.sendall(struct.pack(">I", len(payload)) + payload)

            # Read response using improved handler
            response_data = self.receive_full_response(self.socket, timeout=recv_timeout)
            response = json.loads(response_data.decode('utf-8'))

            # 完整性校验：bridge 在响应里回带 bytes_received，与发送长度不一致
            # 说明 payload 在传输中被截断/损坏，绝不能把残缺命令当作执行成功。
            bytes_received = response.pop("bytes_received", None)
            if bytes_received is not None and bytes_received != len(payload):
                logger.error(
                    f"Payload corrupted: sent {len(payload)} bytes, bridge received {bytes_received}"
                )
                self._drop_connection()
                return {
                    "status": "error",
                    "error": "payload_corrupted",
                    "detail": f"sent {len(payload)} bytes, bridge received {bytes_received} bytes",
                }

            # Log complete response for debugging
            logger.info(f"Complete response from Unreal: {response}")

            # Check for both error formats: {"status": "error", ...} and {"success": false, ...}
            if response.get("status") == "error":
                error_message = response.get("error") or response.get("message", "Unknown Unreal error")
                logger.error(f"Unreal error (status=error): {error_message}")
                # We want to preserve the original error structure but ensure error is accessible
                if "error" not in response:
                    response["error"] = error_message
            elif response.get("success") is False:
                # This format uses {"success": false, "error": "message"} or {"success": false, "message": "message"}
                error_message = response.get("error") or response.get("message", "Unknown Unreal error")
                logger.error(f"Unreal error (success=false): {error_message}")
                # Convert to the standard format expected by higher layers
                response = {
                    "status": "error",
                    "error": error_message
                }

            return response

        except (socket.timeout, ConnectionError, OSError, json.JSONDecodeError) as e:
            logger.error(f"Error sending command: {e}")
            return {
                "status": "error",
                "error": str(e)
            }

        except Exception as e:
            logger.error(f"Error sending command: {e}")
            return {
                "status": "error",
                "error": str(e)
            }
        finally:
            # 一命令一连接：无论成败，命令已执行（或已失败），连接即终结。
            # bridge 端回完响应后也会主动关闭；下一命令将建立全新连接。
            self._drop_connection()

# Global connection state
_unreal_connection: UnrealConnection = None

def get_unreal_connection() -> Optional[UnrealConnection]:
    """Get the connection to Unreal Engine."""
    global _unreal_connection
    try:
        if _unreal_connection is None:
            _unreal_connection = UnrealConnection()
            if not _unreal_connection.connect():
                logger.warning("Could not connect to Unreal Engine")
                _unreal_connection = None
        
        return _unreal_connection
    except Exception as e:
        logger.error(f"Error getting Unreal connection: {e}")
        return None

@asynccontextmanager
async def server_lifespan(server: FastMCP) -> AsyncIterator[Dict[str, Any]]:
    """Handle server startup and shutdown."""
    global _unreal_connection
    logger.info("UnrealMCP server starting up")
    # 一命令一连接：启动时绝不建立常驻连接。闲置的 lifespan 连接会占住
    # bridge 的单客户端接收循环（C++ 侧等待可读直到对端关闭），饿死真正
    # 发命令的客户端。连接完全由 send_command 按命令建立/关闭。
    _unreal_connection = None

    try:
        yield {}
    finally:
        if _unreal_connection:
            _unreal_connection.disconnect()
            _unreal_connection = None
        logger.info("Unreal MCP server shut down")

# Initialize server
mcp = FastMCP(
    "UnrealMCP",
    lifespan=server_lifespan
)

# Import and register tools
from tools.editor_tools import register_editor_tools
from tools.blueprint_tools import register_blueprint_tools
from tools.node_tools import register_blueprint_node_tools
from tools.project_tools import register_project_tools
from tools.umg_tools import register_umg_tools
from tools.material_tools import register_material_tools
from tools.ue_safe_api import register_ue_safe_api_tools
from tools.asset_safety_tools import register_asset_safety_tools
from tools.asset_pipeline_tools import register_asset_pipeline_tools
from tools.particle_tools import register_particle_tools
from tools.animation_tools import register_animation_tools
from tools.pcg_tools import register_pcg_tools
from tools.reflection_tools import register_reflection_tools
from tools.registry_tools import register_registry_tools
from tools.python_api_tools import register_python_api_tools
from tools.pie_tools import register_pie_tools

# Register tools
register_editor_tools(mcp)
register_blueprint_tools(mcp)
register_blueprint_node_tools(mcp)
register_project_tools(mcp)
register_umg_tools(mcp)
register_material_tools(mcp)
register_ue_safe_api_tools(mcp)
register_asset_safety_tools(mcp)
register_asset_pipeline_tools(mcp)
register_particle_tools(mcp)
register_animation_tools(mcp)
register_pcg_tools(mcp)
register_reflection_tools(mcp)
register_registry_tools(mcp)
register_python_api_tools(mcp)
register_pie_tools(mcp)

def _collect_registered_tools() -> str:
    """反射汇总 FastMCP 实际注册的工具清单，杜绝 info() 与注册表漂移。"""
    try:
        tool_manager = getattr(mcp, "_tool_manager", None)
        if tool_manager is None or not hasattr(tool_manager, "list_tools"):
            return "(tool list unavailable)"
        lines = []
        for tool in tool_manager.list_tools():
            desc_lines = (tool.description or "").strip().splitlines()
            summary = desc_lines[0].strip() if desc_lines else ""
            lines.append(f"- `{tool.name}` - {summary}" if summary else f"- `{tool.name}`")
        return "\n    ".join(lines) if lines else "(no tools registered)"
    except Exception as e:
        return f"(tool list unavailable: {e})"

@mcp.prompt()
def info():
    """Information about available Unreal MCP tools and best practices."""
    return f"""
    # Unreal MCP Server Tools and Best Practices

    ## Registered Tools (auto-generated from the live registry — always accurate)
    {_collect_registered_tools()}

    ## Best Practices (CRITICAL — violating these froze the whole MCP bridge in practice)

    ### Asset Safety
    - NEVER call `unreal.EditorAssetLibrary.create_asset`/`delete_asset` directly inside
      `execute_python_command`: dirty assets or name conflicts pop MODAL DIALOGS that block
      the editor GameThread and freeze the entire MCP bridge (90s tool timeouts, WinError 10053).
      Always use the `create_asset_safe` / `delete_asset_safe` tools instead.
    - `delete_asset_safe` silently saves dirty assets first (no save prompt), then deletes;
      duplicate names return a structured `asset_exists` error instead of a rename dialog.

    ### Asset Import
    - Import source files with `import_assets(paths, destination_path)` — NEVER hand-roll
      `unreal.AssetImportTask` + `asset_tools.import_asset_tasks` inside execute_python_*.
      Interchange (the UE5 default texture importer) drains the GameThread task queue from
      inside the import and hits `Assertion failed: ++Queue(QueueIndex).RecursionGuard == 1`
      (TaskGraph.cpp:677) — the EDITOR CRASHES, it is not an error return.
    - `import_assets` disables the Interchange feature flag for the requested extensions and
      READS IT BACK before importing; a readback mismatch aborts with `cvar_override_failed`
      and nothing is imported. The flags stay off for the rest of the session, so later imports
      of those extensions also take the legacy importer — `cvar_overrides` records what changed.
    - Generic asset property writes (texture srgb / lod_group / compression / mip_gen / filter)
      belong in `set_asset_properties(asset_path, props)`: friendly property names, enum members
      matched ignoring case and underscores, per-item errors instead of python exceptions.
      Always check `failed_count`; nothing is rolled back.
    - Expression output names must not be guessed when connecting: read the `outputs` array
      returned by `create_material_expression`, or the `available_outputs` in a failed connect.
      `TextureSample` exposes RGB/R/G/B/A; VectorParameter / TextureCoordinate / every
      single-output node exposes "" (empty string, not "RGB" or "UV").

    ### Long-running Python
    - Do NOT `time.sleep()`/poll inside `execute_python_command`: it blocks the GameThread,
      and the event you are waiting for (render, file write) needs the GameThread to tick —
      a deadlock. Split waits into multiple short commands instead.
    - Scripts longer than ~30 lines MUST use `execute_python_file`: write the script to a
      local .py file (e.g. Saved/MCPScripts/x.py) and pass the absolute path. Long inline
      strings have historically been corrupted upstream of the bridge (random
      middle-of-payload loss → SyntaxError); file execution is immune to this.
    - Execution is SYNC BY DEFAULT; `deferred=True` is the EXCEPTION. Use it only when the work
      provably exceeds the client tool timeout (~90s): bulk writes across dozens of assets,
      heavy compiles/imports, long batch loops. For merely slow calls widen the sync wait with
      `timeout=<seconds>` (<=600s) instead — do not go async "to be safe".
      What deferred costs: while the job holds the GameThread the whole MCP channel is frozen
      (`poll_python_job` itself may time out — Saved/MCPJobs/<job_id>.json on disk is the
      fallback), the job file is written ONLY on completion (so 'pending' cannot distinguish
      queued / running / editor-already-dead), and there is no progress or cancellation. Prefer
      several short IDEMPOTENT jobs over one long one, and pass a `description` to label it.
    - Prefer an existing tool over a hand-rolled script: call `list_mcp_commands` (optionally
      with a category) before writing python. Re-implementing an engine path the plugin already
      wraps is how editors die — asset import is the canonical case (`import_assets` disables
      Interchange and reads the flag back; a raw AssetImportTask crashes the editor).
    - Do NOT use top-level `return` in the python code (executed as a file); print() results.

    ### Python API pitfalls (prefer the safe wrappers)
    - Create assets: `unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, path, cls, factory)`
      (`EditorAssetLibrary.create_asset` does NOT exist). To rebuild an existing asset use
      `create_asset_safe(..., recreate=True)` (silent save+delete first, no modal).
    - Shading model enum is `unreal.MaterialShadingModel.MSM_UNLIT` (there is no `unreal.ShadingModel`).
      Enum member names differ in 5.5 — probe with `list_enum_values` before hardcoding.
    - Connect expressions: `MaterialEditingLibrary.connect_material_expressions(src, '', dst, 'InputName')`
      (`FExpressionInput` is not python-constructible; '' = first input/output; input names are
      C++ member names like A/B/Alpha/ExponentIn; Custom node inputs use their declared input_name).
      NOTE: `MaterialEditingLibrary` has NO get_material_expression(index) in UE 5.5 — the
      `connect_material_expressions_safe` tool resolves indices via list_material_expressions +
      load_object; don't hand-roll index lookups in scripts.
      `list_material_expressions` returns each expression's `object_path`, and the entries'
      `inputs` are structured as `{{input_name, expression, output_index, connected}}` (read
      `input_name`, not a bare string, when matching pins).
      In python, get an expression object with `unreal.load_object(mat, '<ExprName>')` — the
      outer must be the material object; a package-rooted object path returns None.
    - Custom HLSL: run `validate_custom_hlsl(code, output_type)` before writing it;
      `set_material_expression_property(property="code")` runs the same C++ rule set and refuses
      the write on errors (naming a shader parameter struct such as `SceneTexturesStruct`/
      `*UniformParameters` crashes the editor at draw time, not at compile time). The check lives
      in the bridge, so it also guards scripts that write `code` themselves - see the
      `unreal-material-authoring` skill.
    - Custom node input pins bind to the code by NAME: create the node with its pins in ONE call
      (`create_material_expression(asset_path, 'MaterialExpressionCustom', code=..., inputs=['In_Base', ...])`,
      linted before the node exists), add/remove a pin on an EXISTING node with
      `add_custom_input(asset_path, expression_name, input_name)` /
      `remove_custom_input(asset_path, expression_name, input_name)` (both keep the other pins'
      wiring — never assign the whole `inputs` array, which rebuilds the elements and drops it),
      rename a pin with
      `set_custom_input_name(asset_path, expression_name, old_name, new_name)`, rename the
      matching HLSL variable in the same pass, then run `validate_custom_expression(asset_path, expression_name)`
      to cross-check pins against code before compiling. Pins have no type of their own —
      type comes from whatever is connected upstream, so `inputs` takes names only.
      A pin that was added but never wired shows up at compile time as
      `Custom material <node> missing input N (X)`, decoded under `error_details` of
      `get_material_compile_errors`.
    - Whole-graph cleanup (leftover orphan nodes inflate `count` and can hold names): use
      `wipe_material_graph(asset_path)` and check `remaining` is 0. Announcing "empty" from a
      single bulk delete is wrong — the engine's bulk delete removes from the array it iterates,
      so one pass only clears part of the graph (observed 102 -> 50 -> 24 -> 11).
    - Compile diagnostics: judge the CURRENT state ONLY by `errors_by_feature_level`. The log
      fallback is split into `log_errors_this_compile` / `log_errors_historical` by the time of
      the last graph-changing MCP command (`boundary_source`), because the log tail still holds
      intermediate-state failures from earlier edits.
    - Wiring does NOT compile any more: `connect_material_expressions_safe` / `connect_material_pin` /
      `disconnect_material_property` only wire (a recompile used to ride along and cost ~250ms per
      connect, i.e. ~2.4s for a 10-node graph — measured). Finish a material with
      `recompile_material(asset_path, save=True)` ONCE: it does the material-level refresh that makes
      the viewport pick expression-only edits up, plus one recompile. Skip it and the asset reads back
      correct while the viewport keeps drawing the old shader.
    - Deleting a throwaway/probe asset MUST go through `safe_delete_asset`: engine
      `EditorAssetLibrary.delete_asset` on an asset that is still referenced fails, leaves the package
      flagged `potentially corrupt`, and the editor then AVs on a later refresh (logged crash).
    - Material expression operations MUST use current `name` or `desc`; expression `index` is diagnostic only and globally deprecated as an operation argument. Duplicate or missing locators return candidates without modifying the material. The addressing keys are exactly `expression_name` / `expression_desc` / `expression_type` (`name` / `desc` / `type` are NOT read — a misspelled key comes back with `unknown_keys` + `did_you_mean`). Use `delete_material_expressions` for independent batch deletion; it continues per item, does not inspect references or roll back, and reports `failed_count` / `partial` next to a `success` that reflects the items.
    - PostProcessVolume blendables: `PostProcessSettings.blendables` is read-protected; only
      `volume.add_or_update_blendable(mat, weight)` works, AND you must toggle `enabled`
      False->True afterwards or the binding has no visual effect — use `add_blendable_to_post_volume`.
    - Custom depth: set `render_custom_depth=True` + `custom_depth_stencil_value` on components
      (NOT `custom_stencil_value`), and the project needs `r.CustomDepth=3` (check via
      `get_console_variable` — python has no cvar read API).
    - `set_actor_location` needs explicit sweep arg: `actor.set_actor_location(loc, False, False)`.
    - Selection: `EditorLevelLibrary.get_selected_level_actors()` + `actor.set_editor_property('selected', False)`.

    ### Asset editors (closing them without side effects)
    - To close an asset's editors use the `close_asset_editors(asset_path)` TOOL, not
      `AssetEditorSubsystem.close_all_editors_for_asset` from python. The engine records
      the asset's next open location when its tab closes (`[AssetEditorToolkitTabLocation]`),
      so a raw close rewrites where the user sees that asset: with the default
      `AssetEditorOpenLocation`, the asset silently becomes a standalone floating window
      (and a tiny one when no layout file has been saved yet). `close_asset_editors`
      restores the record (dropping the key when it never existed) and reports
      `closed_editors` / `tab_location_before` / `tab_location_restored`.
    - You still MUST close the editors before scripted graph edits (an open material editor
      keeps its own graph nodes + undo stack and will roll the changes back).

    ### Dirty marking and notifications from python
    - `set_editor_property(...)` already fires `PostEditChangeProperty`; there is NO
      `post_edit_change()` method on expression/material objects and NO `mark_package_dirty()`
      on materials — calling them raises AttributeError AFTER the write already happened.
    - To dirty a package from python either rely on `set_editor_property`, or call
      `asset.modify()`, or `unreal.EditorAssetLibrary.save_loaded_asset(asset, only_if_is_dirty=True)`
      when you actually want it written to disk. Save return values are not proof: read the value
      back and assert on it.
    - A python command that fails may already have produced writes before raising (the traceback
      is reported, the side effects are not rolled back). Read the asset back to establish what
      actually landed instead of assuming the previous step was a no-op.

    ### Protocol integrity
    - Commands are sent with a 4-byte big-endian length prefix; the bridge echoes `bytes_received`
      which is verified against the sent length. If a command returns
      `{{"status": "error", "error": "payload_corrupted"}}`, simply resend it.

    ### General
    - Check command responses for `status` before proceeding; errors are returned directly.
    - Compile blueprints after structural changes; recompile materials after wiring changes.
    - Use `take_screenshot` with an absolute .png path for visual verification (filename is
      honored; unlike the HighResShot console route it never black-frames or renames files).
    - Keep the viewport focused/visible for reliable screenshots.
    """

# Run the server
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Unreal MCP server")
    parser.add_argument("--recv-timeout", type=float, default=10.0,
                        help="Response receive timeout in seconds (default: 10)")
    parser.add_argument("-m", "--mcp-mode", choices=["stdio", "sse"], default="stdio",
                        help="MCP transport (default: stdio)")
    args, _ = parser.parse_known_args()
    RECV_TIMEOUT = args.recv_timeout
    logger.info(f"Starting MCP server with {args.mcp_mode} transport, recv-timeout={RECV_TIMEOUT}s")
    mcp.run(transport=args.mcp_mode)