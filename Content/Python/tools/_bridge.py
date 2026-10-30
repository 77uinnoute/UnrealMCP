"""One bridge call path, shared by every tool wrapper.

A tool wrapper used to repeat these steps by hand:

    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}   # or {"actors": [], "count": 0}
    response = unreal.send_command(cmd, params)
    if not response:
        return {"success": False, "message": "No response from Unreal Engine"}
    if response.get("status") == "error":
        return {"success": False, "message": response.get("error")}

Two failure modes came out of that: a dead editor was reported as a normal empty result (`{"actors": [],
"count": 0}` is the same reply as "the level has no actors"), and each tool invented its own error
shape. `_bridge()` owns those steps once and RAISES on failure, so a caller sees an error instead of an
empty success. The success envelope is passed through unchanged, so every tool keeps its reply shape.

What counts as a failure: `status == "error"`, or `result.success is False` (a command that refuses to
do the work reports it that way, e.g. `safe_delete_asset` on a referenced asset). Nothing is inferred
from the payload beyond those two fields.
"""

import json
import logging
import re
from typing import Any, Dict, Optional

logger = logging.getLogger("UnrealMCP")

BRIDGE_ENDPOINT = "127.0.0.1:55557"

_CODE_LIKE = re.compile(r"^[a-z][a-z0-9_]{2,59}$")


class MCPToolError(RuntimeError):
    """A tool call that did not do what it was asked.

    Raised out of the tool so the MCP client sees a failure; `error_code` is the machine-readable half
    (the command's own code when it has one) and `reply` keeps the full bridge envelope for a post-mortem.
    """

    def __init__(self, command: str, error_code: str, detail: str, reply: Optional[Dict[str, Any]] = None):
        super(MCPToolError, self).__init__("%s failed (%s): %s" % (command, error_code, detail))
        self.command = command
        self.error_code = error_code
        self.detail = detail
        self.reply = reply or {}

    def __str__(self) -> str:
        """code + detail, then the rest of the refusal payload as JSON.

        A refusal is often richer than one sentence (workarounds[], referencers[], candidates[],
        level_switched, ...). An exception only carries text, so the fields that are not already in
        the headline are appended - otherwise raising would throw away exactly the part an agent
        needs to pick a different route.
        """
        text = "%s failed (%s): %s" % (self.command, self.error_code, self.detail)
        extra = {k: v for k, v in self.reply.items() if k not in ("status", "error", "error_code")}
        if extra:
            try:
                blob = json.dumps(extra, ensure_ascii=False, default=str)
            except (TypeError, ValueError):
                blob = repr(extra)
            if len(blob) > 4000:
                blob = blob[:4000] + "...(truncated)"
            text += "\n" + blob
        return text


def _connection():
    """The loopback connection, or raise - a missing editor is not an empty result."""
    from unreal_mcp_server import get_unreal_connection

    connection = get_unreal_connection()
    if connection is None:
        raise MCPToolError("bridge", "editor_unreachable",
                           "no connection to the editor bridge (%s) - is the editor running with the "
                           "UnrealMCP plugin loaded?" % BRIDGE_ENDPOINT)
    return connection


def is_failure(reply: Any) -> bool:
    """True for a reply that reports the work was not done."""
    if not isinstance(reply, dict) or not reply:
        return True
    if reply.get("status") == "error":
        return True
    result = reply.get("result")
    return isinstance(result, dict) and result.get("success") is False


def _looks_like_code(text: str) -> bool:
    """True for a bare machine-readable token ('viewport_not_found'), false for a sentence."""
    return bool(_CODE_LIKE.match(text or ""))


def failure_of(reply: Optional[Dict[str, Any]], command: str) -> MCPToolError:
    """Build the error a failed reply stands for (shared by _bridge and the loopback guard's callers)."""
    reply = reply if isinstance(reply, dict) else {}
    result = reply.get("result") if isinstance(reply.get("result"), dict) else {}
    detail = (reply.get("error") or result.get("error") or result.get("message") or reply.get("message")
              or "the command failed without a message")
    # Handlers disagree on where the code lives: `error_code` when they have one, otherwise the code
    # IS the message for the many commands that answer {"status": "error", "error": "viewport_not_found"}.
    code = reply.get("error_code") or result.get("error_code")
    if not code:
        code = detail if _looks_like_code(detail) else "command_failed"
    return MCPToolError(command, code, detail, reply)


def _bridge(command: str, params: Optional[Dict[str, Any]] = None,
            recv_timeout: Optional[float] = None) -> Dict[str, Any]:
    """Send one command and return its reply envelope; raise MCPToolError when it did not succeed.

    Args:
        command: Registered bridge command name.
        params: Command parameters (an empty dict when omitted).
        recv_timeout: Seconds to wait for the reply. Must be >= the bridge-side GameThread wait
            (default 120s) for slow commands, otherwise the client gives up while the editor is
            still working.
    """
    connection = _connection()
    try:
        reply = connection.send_command(command, params or {}, recv_timeout=recv_timeout)
    except MCPToolError:
        raise
    except Exception as error:  # the transport object itself broke (socket/json layer)
        logger.error("Bridge transport error on %s: %s", command, error)
        raise MCPToolError(command, "transport_error", str(error))

    if not isinstance(reply, dict) or not reply:
        logger.error("No reply from the bridge for %s", command)
        raise MCPToolError(command, "no_response",
                           "the bridge closed the connection without a reply (editor crashed, or the "
                           "command is still running after its timeout)")

    if is_failure(reply):
        failure = failure_of(reply, command)
        logger.error("Bridge reported failure: %s", failure)
        raise failure

    return reply


def result_of(reply: Dict[str, Any]) -> Dict[str, Any]:
    """The handler payload of a reply (empty dict when the command has none)."""
    result = reply.get("result") if isinstance(reply, dict) else None
    return result if isinstance(result, dict) else {}


def surface_hidden_python_errors(reply: Dict[str, Any]) -> Optional[str]:
    """Error text collected from `result.log` of an otherwise-successful python execution, or None.

    Older bridge builds report success even when the executed python raised: the traceback only shows
    up as Error entries in the log, so a caller that trusts `status` misses it.
    """
    result = result_of(reply)
    parts = []
    for entry in (result.get("log") or []):
        if isinstance(entry, dict) and entry.get("type") == "Error":
            line = str(entry.get("output", "")).replace("\r\n", "\n")
            if line and not line.endswith("\n"):
                line += "\n"
            parts.append(line)
    return "".join(parts) if parts else None
