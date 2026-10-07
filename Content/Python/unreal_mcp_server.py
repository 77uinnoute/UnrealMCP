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

# Text the MCP client receives on initialize (MCP protocol `instructions`). Kept short on
# purpose: it is injected into every session, so it carries only the lay of the land plus the
# three escape hatches - everything else is invisible at load time, where a client shows tool
# NAMES and nothing more.
SERVER_INSTRUCTIONS = """\
UnrealMCP —— UE 编辑器控制面：300+ 条命令经本地 TCP bridge(127.0.0.1:55557) 打到运行中的编辑器。
会话加载时你只会拿到工具「名字」加这一段；正文要用 search_tool 现取。

【域与条数】（快照，实时值以 list_mcp_commands(category=...) 为准）
  anim_sequence 78  动画序列：骨骼轨道/曲线/Notify/NotifyTrack/SyncMarker/段/帧率/压缩/加权/采样回读（建序列必须走 C++ 命令）
  anim_montage  54  蒙太奇：section/slot/segment/branching point/notify track/blend in-out
  blueprint     33  蓝图资产与组件层级：变量、函数图与签名、接口、组件增删/换根/挂接、比对、存在性检查
  blueprint_node 32 图节点：事件/函数/变量/常量节点、引脚默认值、连线与拆线、Cast/Switch/Comment、拆合引脚
  umg           33  控件蓝图：控件树与槽、属性、绑定、控件动画、加入 PIE 视口
  material      23  材质图：表达式、连线、Custom HLSL 校验、后处理 blendable、单节点预览、重编译
  particle      19  Cascade 粒子：emitter/module/属性与分布曲线/LOD
  pcg           16  程序化生成：图节点、mesh selector、组件生成与清理
  editor        17  关卡 actor：增删查改、变换、视口聚焦、控制台变量、截图、资产编辑器开关
  asset         12  资产生命周期：create/delete/move(_directory)、引用阻断者、磁盘孤儿、后处理 blendable
  asset_edit     6  导入与资产属性：import_assets/texture/skeletal_mesh/animation、inspect_skeletal_mesh、set_asset_properties
  physics        6  PhysicsAsset：body/constraint 增删、碰撞设置、body 清单
  reflection     3  通用属性反射：reflect_probe / set_object_property / list_enum_values
  pie            3  PIE：start_pie / stop_pie / get_actor_pose
  mcp            4  python 执行（execute_python_command/file）、list_mcp_commands、ping
  python-api     2  引擎编辑器 python API 层：python_api_index / python_api_doc
  project        1  create_input_mapping（写 Config/DefaultInput.ini）
  cloth          1  apply_cloth_masks

【三个逃生口，分工不同、不可互相替代】
  1) 命令面 list_mcp_commands(category?)：权威清单，带 params(含 required) 与 policy flags
     （loopback_forbidden=回环禁用 / mutates_graph / persist_after_success / hidden）。查名字优先于猜名字。
  2) 属性面 reflect_probe / set_object_property：通用反射读写。target 可收资产 / 子对象路径 / 类（读 CDO）/
     蓝图图节点 / SCS 组件模板 / 材质表达式；property 可收路径 A[0].B[2].C（数组下标、映射键）。
     裸 UPROPERTY、Transient、只读（VisibleInstanceOnly）字段只有它能读——例：UPhysicsAsset.SkeletalBodySetups
     读不到就换它拿子对象路径（…:SkeletalBodySetup_0），再用 python 批量读子对象属性。
  3) python 面 python_api_index(class) / python_api_doc(class, function)：引擎编辑器 python API 的成员与签名，
     命中失败时回最近候选名。list_mcp_commands 看不到这一层。

【命令面状态】少数内部/诊断用命令被有意摘出工具面（搜不到，但仍可经 bridge 直调）：见服务端 HIDDEN_TOOLS。

【硬约束（违反会冻死整条 bridge 或崩编辑器）】
  - 资产别手搓 unreal.EditorAssetLibrary.create_asset / delete_asset：脏资产或重名会弹模态框卡死 MCP 通道；
    用 create_asset_safe / delete_asset_safe（后者先静默保存再删，重名回结构化 asset_exists）。
  - 导入必须走 import_assets（手搓 AssetImportTask 命中 Interchange 会崩编辑器）。
  - 结构性写（AnimGraph / SCS 组件树 / 材质图 / 蒙太奇）先 close_asset_editors。
  - 长脚本落盘再 execute_python_file；>30 行不要塞内联字符串（传输层会截断/损坏）。
  - python 里不要 sleep / 轮询阻塞 GameThread；等待拆成多次短派发。命令一律同步（没有 deferred/job 模式）：
    必须能在客户端超时（~90s）内跑完，重活拆成多次短调用，慢活用 timeout<=600 拉长等待。
  - 协议：命令带 4 字节大端长度前缀，回执有 bytes_received 校验；返回 payload_corrupted 时直接重发即可。
  - dirty：set_editor_property() 本身已触发 PostEditChangeProperty；表达式/材质上没有 post_edit_change()、
    也没有 mark_package_dirty()（调了会在写入之后才抛 AttributeError）。
  - 通用：先读回执 status 再往下走；蓝图结构改动后编译；材质连线只在收尾 recompile_material 一次。

【流程】动手前按域加载对应 skill（material→unreal-material-authoring、blueprint→unreal-blueprint-authoring、
umg/particle/pcg/asset-pipeline/animation/cloth/retarget/level 同理）。改了
Plugins/UnrealMCP/Content/Python/** 必须重启 unrealMCP server。\
"""

# Initialize server
mcp = FastMCP(
    "UnrealMCP",
    instructions=SERVER_INSTRUCTIONS,
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

# Curated tool surface. Names listed here are withdrawn from what an MCP client is offered, while
# staying callable over the bridge (list_mcp_commands keeps reporting them). Keep the list short
# and justified: most commands ARE part of a documented skill workflow, and withdrawing one of
# those removes the capability outright - a client cannot search for a tool that is not registered.
HIDDEN_TOOLS = (
    "anim_self_check",             # liveness probe for the animation domain - same role as ping
    "asset_status",                # read-only asset triage; does_asset_exist + list_asset_blockers cover the daily case
    "get_source_files",            # import provenance, rarely needed after the import itself
    "list_disk_only_assets",       # orphan .uasset scan, hand-run only
    "prune_widget_bindings",       # only reports leftovers; the fix is a graph edit
    "set_actor_custom_depth_safe",  # custom depth / stencil, niche
)


def _hide_tools(names):
    """Withdraw the curated names from the advertised tool surface (best effort, never fatal)."""
    manager = getattr(mcp, "_tool_manager", None)
    if manager is None or not hasattr(manager, "remove_tool"):
        logger.warning("Tool surface curation skipped: ToolManager.remove_tool is unavailable")
        return
    for name in names:
        try:
            manager.remove_tool(name)
        except Exception as exc:  # not registered, or already withdrawn
            logger.warning("Tool %s left on the surface (%s)", name, exc)
    try:
        logger.info("Tool surface: %d tools advertised after curation", len(manager.list_tools()))
    except Exception:
        pass


_hide_tools(HIDDEN_TOOLS)

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
    """List the Unreal MCP tools currently registered with this server."""
    return f"""
    # Unreal MCP Server Tools

    ## Registered Tools (auto-generated from the live registry - always accurate)
    {_collect_registered_tools()}

    ## Guidance
    Per-tool guidance lives in each tool's own description and in the matching skill
    (unreal-material-authoring, unreal-blueprint-authoring, ...). Read it there: call
    list_mcp_commands for the authoritative list, or the tool itself when a call fails.
    This prompt deliberately carries no duplicate prose.
    """


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