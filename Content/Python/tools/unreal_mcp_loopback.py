"""Fail-fast guard for the editor-python loopback.

The bridge installs this module ahead of every `execute_python_command` / `execute_python_file` run
(see UnrealMCPBridge.cpp, `WrapPythonSource`), so a bridge command that fails raises
`UnrealMCPCommandError` at that line and the script stops there.

Why: a failed command normally just comes back as a reply the script has to inspect, so a script that
issues dozens of commands (building a widget tree, filling tables) keeps going on a broken state and
buries the real error under a pile of follow-up failures. Commands report "the effect did not happen"
as `success: false` (there is no "success: true but nothing changed" shape), which is what the guard
keys off.

Escape hatches
--------------
* one call - the guard consumes this key and never forwards it to the handler:

      execute_mcp_command("safe_delete_asset",
                          json.dumps({"asset_path": path, "on_error": "continue"}))

* whole run:      import unreal_mcp_loopback
                  unreal_mcp_loopback.FAIL_FAST = False

Scope: this lives inside the editor's python VM, so the MCP tool wrappers (which talk to the bridge
over TCP) are untouched. `first_failure` keeps the first failure of the run for post-mortem reads.
"""
import json

import unreal as _unreal

FAIL_FAST = True

#: {"command": ..., "reply": ...} of the first failure this run raised on, or None.
first_failure = None

_real_api = None
_installed = False


class UnrealMCPCommandError(RuntimeError):
    """Raised instead of returning a failure reply, so the script stops where it broke."""

    def __init__(self, command, reply):
        result = reply.get("result") if isinstance(reply, dict) else None
        if not isinstance(result, dict):
            result = {}
        code = (result.get("error_code") or reply.get("error_code")
                or reply.get("error") or "command_failed")
        detail = result.get("error") or result.get("message") or reply.get("error") or ""
        super(UnrealMCPCommandError, self).__init__(
            "%s failed (%s): %s" % (command, code, detail))
        self.command = command
        self.error_code = code
        self.reply = reply


def is_failure(reply):
    """True for a reply the guard should stop on.

    Only `status` / `success` decide: a command that did not do what it promised reports
    `success: false` with an `error_code` of its own (safe_delete_asset refuses to delete a referenced
    asset that way, instead of the old `success: true` + `deleted: false`), so the guard never has to
    guess from the shape of the payload.
    """
    if not isinstance(reply, dict):
        return True
    if reply.get("status") == "error":
        return True
    result = reply.get("result")
    if isinstance(result, dict):
        return result.get("success") is False
    return False


def _guarded(command, params_json=""):
    global first_failure

    body = {}
    if params_json:
        try:
            body = json.loads(params_json)
        except ValueError:
            body = None
    policy = body.pop("on_error", None) if isinstance(body, dict) else None

    forwarded = json.dumps(body) if isinstance(body, dict) else params_json
    reply_json = _real_api.execute_mcp_command(command, forwarded)

    if not FAIL_FAST or policy == "continue":
        return reply_json
    try:
        reply = json.loads(reply_json)
    except ValueError:
        return reply_json
    if is_failure(reply):
        if first_failure is None:
            first_failure = {"command": command, "reply": reply}
        raise UnrealMCPCommandError(command, reply)
    return reply_json


class _GuardedAPI(object):
    """Stands in for unreal.UnrealMCPPythonAPI.

    The native class is immutable (`setattr` answers "cannot set 'execute_mcp_command' attribute of
    immutable type"), but the module attribute is a plain module dict entry, so the guard replaces the
    name. `__mcp_real_api__` is the marker that lets a later execution find the real class instead of
    wrapping this shim again.
    """

    __mcp_real_api__ = None

    execute_mcp_command = staticmethod(_guarded)


#: Where the real class is remembered on the unreal module (survives across executions).
_REAL_ATTR = "__mcp_real_python_api__"


def _deepest_real(candidate):
    """Follow a chain of shims down to the real class."""
    for _ in range(8):
        if candidate is None or getattr(candidate, "__mcp_real_api__", None) is None:
            return candidate
        candidate = candidate.__mcp_real_api__
    return candidate


def install():
    """Patch unreal.UnrealMCPPythonAPI for this execution.

    The shim is rebuilt every run so the guard of THIS run owns the switch (`FAIL_FAST`) and the
    failure record (`first_failure`), and it always wraps the real class rather than the previous
    run's shim: nested shims pop the per-call policy off before the inner guard sees it, so an
    `on_error: "continue"` call raised anyway (measured on the second execution of a probe).
    """
    global _real_api, _installed

    if _installed:
        return
    try:
        real = _deepest_real(getattr(_unreal, _REAL_ATTR, None))
        if real is None:
            real = _deepest_real(_unreal.UnrealMCPPythonAPI)
        _real_api = real
        setattr(_unreal, _REAL_ATTR, real)
        _GuardedAPI.__mcp_real_api__ = real
        _unreal.UnrealMCPPythonAPI = _GuardedAPI
        _installed = True
    except Exception as error:  # a guard that cannot install must not break the run
        print("unreal_mcp_loopback: guard NOT installed: %s" % error)


def uninstall():
    """Put the real class back (used by the guard's own tests)."""
    global _installed
    if _installed and _real_api is not None:
        _unreal.UnrealMCPPythonAPI = _real_api
        _installed = False
