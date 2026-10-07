"""Build-time check: can the python MCP server actually import and register its tools?

check_command_consistency.py compares the C++ registry, the python send sites, the tool signatures
and the skill docs - none of that notices a tool module that cannot even be imported. Measured
2026-10-05: a new `Optional[str]` parameter added to tools/python_api_tools.py, whose import line
only pulled `Dict, Any`, made `register_python_api_tools` raise NameError at server startup. Every
check in the build stayed green, the binaries were deployed, and the only symptom was the MCP client
reporting "Connection closed" with a screenshot of the stderr traceback - i.e. the failure landed on
the user, twice, with a full editor restart in between.

This check closes that gap by doing exactly what the server does at startup: import the server
module (which runs every register_*_tools call) and count the tools that came out. It then compares
that against the number of `@mcp.tool()` decorated functions declared in the tool modules, so a
registration that silently stops happening is caught too, not just a hard ImportError.

Usage (the build script calls it with the plugin venv's python):

    Content/Python/.venv/Scripts/python.exe Build/check_server_registration.py

It re-executes itself with the venv interpreter when it is started with some other python, and
reports SKIPPED (exit 0) when no interpreter with `mcp` installed can be found - the check must not
block a build on a machine where the venv was never created.

Exit codes: 0 = OK (or SKIPPED), 1 = import/registration failure or a tool-count mismatch.
"""

from __future__ import annotations

import ast
import asyncio
import subprocess
import sys
from pathlib import Path

BUILD_DIR = Path(__file__).resolve().parent
PLUGIN_DIR = BUILD_DIR.parent
PYTHON_DIR = PLUGIN_DIR / "Content" / "Python"
RESULT_PREFIX = "SERVER_REGISTRATION_RESULT:"


def venv_python() -> Path | None:
    candidates = [
        PYTHON_DIR / ".venv" / "Scripts" / "python.exe",  # Windows
        PYTHON_DIR / ".venv" / "bin" / "python",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return None


def ensure_venv_interpreter() -> int | None:
    """Re-exec under the venv python when we were not started with it.

    Returns an exit code when the caller should stop, None when this process already is the right
    interpreter (or is the best available one).
    """
    interpreter = venv_python()
    if interpreter is None:
        return None
    if Path(sys.executable).resolve() == interpreter.resolve():
        return None
    print("%s re-running under %s" % (RESULT_PREFIX, interpreter), flush=True)
    completed = subprocess.run([str(interpreter), str(Path(__file__).resolve())],
                               cwd=str(PYTHON_DIR))
    sys.stdout.flush()
    return completed.returncode


def declared_tool_names() -> dict[str, list[str]]:
    """`@mcp.tool()` decorated function names per file, read statically."""
    found: dict[str, list[str]] = {}
    sources = sorted(PYTHON_DIR.glob("tools/*.py")) + [PYTHON_DIR / "unreal_mcp_server.py"]
    for path in sources:
        if not path.exists():
            continue
        try:
            tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        except SyntaxError as error:
            print("%s python parse error in %s: %s" % (RESULT_PREFIX, path, error))
            found.setdefault(str(path), []).append("<parse error>")
            continue

        names: list[str] = []
        for node in ast.walk(tree):
            if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                continue
            for decorator in node.decorator_list:
                target = decorator.func if isinstance(decorator, ast.Call) else decorator
                if isinstance(target, ast.Attribute) and target.attr == "tool":
                    names.append(node.name)
        if names:
            found[str(path.relative_to(PLUGIN_DIR))] = names
    return found


def main() -> int:
    stopped = ensure_venv_interpreter()
    if stopped is not None:
        return stopped

    declared = declared_tool_names()
    declared_total = sum(len(names) for names in declared.values())

    if str(PYTHON_DIR) not in sys.path:
        sys.path.insert(0, str(PYTHON_DIR))

    try:
        import unreal_mcp_server  # noqa: PLC0415  (import on purpose: it runs every registration)
    except ModuleNotFoundError as error:
        # No `mcp` package: the venv is missing on this machine. Not a build failure.
        print("%s SKIPPED (cannot import the server module: %s)" % (RESULT_PREFIX, error))
        return 0
    except Exception:  # noqa: BLE001  (any failure here is the bug we are looking for)
        import traceback

        print("%s FAILED" % RESULT_PREFIX)
        print("importing Content/Python/unreal_mcp_server.py raised - the MCP server cannot start:")
        traceback.print_exc()
        return 1

    try:
        live = asyncio.run(unreal_mcp_server.mcp.list_tools())
    except Exception:  # noqa: BLE001
        import traceback

        print("%s FAILED" % RESULT_PREFIX)
        print("the server module imported but list_tools() raised:")
        traceback.print_exc()
        return 1

    live_names = sorted(tool.name for tool in live)
    declared_names = sorted(name for names in declared.values() for name in names)

    # The server withdraws a curated list from the advertised surface after registering it all
    # (unreal_mcp_server.HIDDEN_TOOLS, applied by _hide_tools). Those names are registered and
    # callable over the bridge - they are simply not offered to an MCP client, so they must not be
    # reported as "registration did not happen".
    hidden = set(getattr(unreal_mcp_server, "HIDDEN_TOOLS", ()))
    expected_surface = sorted(set(declared_names) - hidden)

    problems: list[str] = []

    unaccounted = sorted(set(declared_names) - set(live_names) - hidden)
    if unaccounted:
        problems.append("declared in a tool module but neither advertised nor hidden - the "
                        "registration did not run for them: %s" % ", ".join(unaccounted))

    undeclared = sorted(set(live_names) - set(declared_names))
    if undeclared:
        problems.append("advertised but not declared in any tool module: %s" % ", ".join(undeclared))

    if live_names != expected_surface:
        problems.append("advertised tools (%d) do not match declared minus hidden (%d)"
                        % (len(live_names), len(expected_surface)))

    stale = sorted(hidden - set(declared_names))
    if stale:
        problems.append("HIDDEN_TOOLS names that no longer exist: %s" % ", ".join(stale))

    if problems:
        print("%s FAILED" % RESULT_PREFIX)
        for problem in problems:
            print("  %s" % problem)
        for path, names in sorted(declared.items()):
            print("  declared in %-52s %d" % (path, len(names)))
        return 1

    print("%s OK (%d tools advertised + %d curated/hidden = %d declared @mcp.tool() functions)"
          % (RESULT_PREFIX, len(live_names), len(hidden), len(declared_names)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
