#!/usr/bin/env python3
"""Static cross-check of the MCP command surface.

Four sources are collected and compared, all without an editor, a build, or network access:

  1. registered commands   - C++ `MCP_REGISTER_COMMAND(...)` / `Registry.Register({...})` literals
  2. python send sites     - `send_command("x" / _send("x"` literals, parsed with `ast`
  3. python tool names     - `@mcp.tool()` decorated functions
  4. documentation mentions - tool-shaped identifiers in `.claude/skills/*/SKILL.md`

What fails (exit 1):
  - a python send site names a command that is not registered
  - two python tools share a name
  - documentation names something that is neither a python tool nor a registered command
  - a command-name comparison (`CommandType == TEXT("...")`) reappears outside the registry,
    i.e. a second list of names is coming back
  - a tool's static params dict sends a key the command does not declare
  - a tool forwards its argument `p` under another key while `p` is itself a parameter of that
    command (the same name meaning two things in the two layers)

What only warns (exit 0):
  - a registered, non-hidden command that no python tool ever sends
  - a registered, non-hidden command no skill document mentions
  - a plain rename between the tool argument and the bridge key

Usage:
    python Build/check_command_consistency.py                 # full check
    python Build/check_command_consistency.py --warn-only     # never fail (rollout mode)
    python Build/check_command_consistency.py --baseline      # legacy routing chain vs registry
"""

import argparse
import ast
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "Source" / "UnrealMCP"
PYTHON_DIR = ROOT / "Content" / "Python"
SKILLS_DIR = ROOT / ".claude" / "skills"
ALLOWLIST = Path(__file__).resolve().parent / "command_consistency_allowlist.txt"

# Only the command surface is scanned. Content/Python also holds a .venv with ~1200 site-packages
# files; parsing those made a run take a minute instead of a second.
PYTHON_ROOTS = (PYTHON_DIR / "tools", PYTHON_DIR / "scripts")
PYTHON_EXCLUDES = (".venv", "site-packages", "__pycache__", "Lib", "build", "dist")

_AST_CACHE = {}

# Identifiers in the docs that look like a tool call start with one of these verbs. Anything else in
# backticks (engine API, C++ names, python locals) is not treated as a tool reference.
TOOL_VERBS = (
    "set_", "get_", "add_", "create_", "delete_", "list_", "find_", "spawn_", "read_", "write_",
    "copy_", "move_", "remove_", "clear_", "validate_", "compile_", "connect_", "disconnect_",
    "attach_", "detach_", "apply_", "import_", "export_", "scan_", "wipe_", "duplicate_",
    "reflect_", "poll_", "execute_", "take_", "close_", "toggle_", "save_", "load_", "render_",
    "heal_", "fix_", "rename_", "update_", "refresh_", "inspect_", "describe_", "generate_",
    "convert_", "focus_", "bind_", "open_", "mark_", "select_", "verify_",
)

REGISTER_MACRO = re.compile(r'MCP_REGISTER_COMMAND\s*\([^,]+,\s*"([a-z0-9_]+)"')
REGISTER_TABLE = re.compile(r'Registry\.Register\s*\(\s*\{\s*TEXT\("([a-z0-9_]+)"\)')
COMMAND_COMPARISON = re.compile(r'CommandType\s*==\s*TEXT\("([a-z0-9_]+)"\)')
C_COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)
BACKTICK = re.compile(r"`([^`\n]+)`")

errors = []
warnings = []
exempted = []


def fail(message):
    errors.append(message)


def warn(message):
    warnings.append(message)


_TEXT_CACHE = {}


def read_text(path):
    # Memoised: the collectors look at the same C++ files more than once.
    if path not in _TEXT_CACHE:
        _TEXT_CACHE[path] = path.read_text(encoding="utf-8", errors="replace")
    return _TEXT_CACHE[path]


def rel(path, line=None):
    text = str(path.relative_to(ROOT)).replace("\\", "/")
    return "%s:%d" % (text, line) if line else text


def cpp_files():
    return [p for p in SOURCE.rglob("*.cpp")] + [p for p in SOURCE.rglob("*.h")]


def python_files():
    """Every file of the command surface, with the virtualenv and caches left out."""
    files = list(PYTHON_DIR.glob("*.py"))
    for root in PYTHON_ROOTS:
        if root.exists():
            files.extend(root.rglob("*.py"))
    return sorted(p for p in files if not any(part in PYTHON_EXCLUDES for part in p.parts))


def parse_python(path):
    """Parsed once per run: four collectors look at the same files."""
    if path not in _AST_CACHE:
        try:
            _AST_CACHE[path] = ast.parse(read_text(path), filename=str(path))
        except SyntaxError as error:
            fail("%s: python parse error: %s" % (rel(path, error.lineno or 0), error.msg))
            _AST_CACHE[path] = None
    return _AST_CACHE[path]


def tool_decorated(node):
    return any(isinstance(d, ast.Call) and isinstance(d.func, ast.Attribute) and d.func.attr == "tool"
               for d in node.decorator_list)


def load_allowlist():
    """identifier -> reason. A line without a reason comment is an error in itself."""
    allowed = {}
    if not ALLOWLIST.exists():
        return allowed
    for number, raw in enumerate(ALLOWLIST.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "#" not in line:
            fail("%s:%d allowlist entry has no reason comment: %s" % (rel(ALLOWLIST), number, line))
            continue
        identifier, reason = line.split("#", 1)
        identifier = identifier.strip()
        if not identifier or not reason.strip():
            fail("%s:%d allowlist entry needs both an identifier and a reason: %s"
                 % (rel(ALLOWLIST), number, line))
            continue
        allowed[identifier] = reason.strip()
    return allowed


def collect_registered():
    """Registered command names -> (definition site, category)."""
    found = {}
    for path in cpp_files():
        text = read_text(path)
        for match in REGISTER_MACRO.finditer(text):
            name = match.group(1)
            line = text[:match.start()].count("\n") + 1
            found.setdefault(name, rel(path, line))
        for match in REGISTER_TABLE.finditer(text):
            name = match.group(1)
            line = text[:match.start()].count("\n") + 1
            found.setdefault(name, rel(path, line))
    return found


def collect_param_names():
    """Every parameter name declared by any command (used to ignore doc mentions of parameters)."""
    pattern = re.compile(r'MCPParam(?:Opt)?\(\s*TEXT\("([a-z0-9_]+)"\)')
    names = set()
    for path in cpp_files():
        names.update(pattern.findall(read_text(path)))
    return names


def collect_legacy_routing():
    """Command-name comparisons left in dispatch code, i.e. a second list of names."""
    found = {}
    for path in cpp_files():
        text = C_COMMENT.sub("", read_text(path))
        for match in COMMAND_COMPARISON.finditer(text):
            line = text[:match.start()].count("\n") + 1
            found.setdefault(match.group(1), rel(path, line))
    return found


def collect_python_sends():
    """Command name -> list of "file:line" py send sites."""
    sends = {}
    for path in python_files():
        tree = parse_python(path)
        if tree is None:
            continue
        for node in ast.walk(tree):
            if not isinstance(node, ast.Call) or not node.args:
                continue
            func = node.func
            name = func.attr if isinstance(func, ast.Attribute) else getattr(func, "id", None)
            # _bridge() is the local forwarding helper in tools/ue_safe_api.py: it takes the command
            # name as a literal first argument, exactly like send_command does.
            if name not in ("send_command", "_send", "_bridge"):
                continue
            first = node.args[0]
            if not (isinstance(first, ast.Constant) and isinstance(first.value, str)):
                continue  # a computed name cannot be checked; the spec asks for literals
            sends.setdefault(first.value, []).append(rel(path, node.lineno))
    return sends


def collect_python_tools():
    """Registered tool name -> list of "file:line" definitions (a duplicate is an error)."""
    tools = {}
    for path in python_files():
        tree = parse_python(path)
        if tree is None:
            continue
        for node in ast.walk(tree):
            if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                continue
            if tool_decorated(node):
                tools.setdefault(node.name, []).append(rel(path, node.lineno))
    return tools


def collect_tool_supplied_params():
    """Tool name -> the bridge parameters it can supply.

    A tool may name its own parameters for the caller (widget_name) and map them onto the bridge
    names when it builds the request, so the keys of every literal dict in its body count as
    supplied parameters too. Without that, a legitimate rename looks like a missing required param.
    """
    supplied = {}
    for path in python_files():
        tree = parse_python(path)
        if tree is None:
            continue
        for node in ast.walk(tree):
            if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                continue
            if not tool_decorated(node):
                continue
            names = {a.arg for a in node.args.args + node.args.kwonlyargs}
            names.discard("ctx")
            for inner in ast.walk(node):
                if isinstance(inner, ast.Dict):
                    names.update(key.value for key in inner.keys
                                 if isinstance(key, ast.Constant) and isinstance(key.value, str))
            supplied[node.name] = names
    return supplied


def collect_tool_forwarded_command():
    """Tool name -> the bridge command it forwards (first literal send inside its body)."""
    forwarded = {}
    for path in python_files():
        tree = parse_python(path)
        if tree is None:
            continue
        for node in ast.walk(tree):
            if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                continue
            if not tool_decorated(node):
                continue
            for inner in ast.walk(node):
                if not isinstance(inner, ast.Call) or not inner.args:
                    continue
                func = inner.func
                name = func.attr if isinstance(func, ast.Attribute) else getattr(func, "id", None)
                if name not in ("send_command", "_send"):
                    continue
                first = inner.args[0]
                if isinstance(first, ast.Constant) and isinstance(first.value, str):
                    forwarded[node.name] = first.value
                    break
    return forwarded


ENTRY_BLOCK = re.compile(r'MCP_REGISTER_COMMAND\(Registry, "([a-z0-9_]+)"(.*?)(?=MCP_REGISTER_COMMAND\(|\Z)',
                         re.DOTALL)
REQUIRED_PARAM = re.compile(r'MCPParam\(\s*TEXT\("([a-z0-9_]+)"\)')


def collect_required_params():
    """Command name -> the parameter names its registration marks as required."""
    required = {}
    for path in cpp_files():
        text = read_text(path)
        for match in ENTRY_BLOCK.finditer(text):
            names = REQUIRED_PARAM.findall(match.group(2))
            if names:
                required.setdefault(match.group(1), set()).update(names)
    return required


ANY_PARAM = re.compile(r'MCPParam(?:Opt)?\(\s*TEXT\("([a-z0-9_]+)"\)')


def collect_command_params():
    """Command name -> every parameter name its registration declares (required or optional).

    Only registrations that list their parameters inline are covered; a command whose entry block
    declares none is left out (its spec is unknown here, so it is not compared).
    """
    params = {}
    for path in cpp_files():
        text = read_text(path)
        for match in ENTRY_BLOCK.finditer(text):
            names = ANY_PARAM.findall(match.group(2))
            if names:
                params.setdefault(match.group(1), set()).update(names)
    return params


def _sent_dict_entries(function, argument):
    """(key, source) pairs of the params dict a send site passes, or None when it is not static.

    source is the forwarded function argument name when the value is a bare Name, else None.
    Handles a dict literal passed inline and a local variable built from dict literals and
    `params["k"] = v` assignments inside the same tool function.
    """
    def entries_of(dict_node):
        out = []
        for key, value in zip(dict_node.keys, dict_node.values):
            if key is None:
                return None  # **spread: not static
            if not (isinstance(key, ast.Constant) and isinstance(key.value, str)):
                return None
            out.append((key.value, value.id if isinstance(value, ast.Name) else None))
        return out

    if isinstance(argument, ast.Dict):
        return entries_of(argument)
    if not isinstance(argument, ast.Name):
        return None

    variable = argument.id
    entries = []
    found = False
    for inner in ast.walk(function):
        if isinstance(inner, ast.Assign):
            for target in inner.targets:
                if isinstance(target, ast.Name) and target.id == variable:
                    if not isinstance(inner.value, ast.Dict):
                        return None
                    part = entries_of(inner.value)
                    if part is None:
                        return None
                    entries.extend(part)
                    found = True
                elif (isinstance(target, ast.Subscript) and isinstance(target.value, ast.Name)
                      and target.value.id == variable):
                    key = target.slice
                    if not (isinstance(key, ast.Constant) and isinstance(key.value, str)):
                        return None
                    entries.append((key.value, inner.value.id if isinstance(inner.value, ast.Name) else None))
    return entries if found else None


def collect_tool_send_params():
    """[(tool, command, [(key, source)] or None, "file:line")] for every literal send inside a tool."""
    sites = []
    for path in python_files():
        tree = parse_python(path)
        if tree is None:
            continue
        for node in ast.walk(tree):
            if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) or not tool_decorated(node):
                continue
            arguments = {a.arg for a in node.args.args + node.args.kwonlyargs}
            for inner in ast.walk(node):
                if not isinstance(inner, ast.Call) or len(inner.args) < 2:
                    continue
                func = inner.func
                name = func.attr if isinstance(func, ast.Attribute) else getattr(func, "id", None)
                if name not in ("send_command", "_send"):
                    continue
                first = inner.args[0]
                if not (isinstance(first, ast.Constant) and isinstance(first.value, str)):
                    continue
                entries = _sent_dict_entries(node, inner.args[1])
                if entries is not None:
                    entries = [(key, source if source in arguments else None) for key, source in entries]
                sites.append((node.name, first.value, entries, rel(path, inner.lineno)))
    return sites


def collect_doc_mentions(registered_param_names):
    """Tool-shaped identifier -> list of "file:line".

    Two shapes are NOT tool references even though they look like one:
      - a parameter name of a registered command (docs name parameters in backticks constantly);
      - anything on a line that documents the python reflection library (`lib.*`), whose methods
        share the verb prefixes but live in the plugin's C++ library rather than the command table.
    """
    mentions = {}
    if not SKILLS_DIR.exists():
        return mentions
    for path in sorted(SKILLS_DIR.rglob("*.md")):
        for number, line in enumerate(read_text(path).splitlines(), start=1):
            if "lib." in line:
                continue
            for token in BACKTICK.findall(line):
                token = token.strip()
                if not token.startswith(TOOL_VERBS) or "." in token or "(" in token or " " in token:
                    continue
                if not re.fullmatch(r"[a-z][a-z0-9_]*", token):
                    continue
                if token in registered_param_names:
                    continue
                mentions.setdefault(token, []).append(rel(path, number))
    return mentions


def report(title, items):
    if not items:
        return
    print("")
    print(title)
    for item in items:
        print("  " + item)


def main():
    parser = argparse.ArgumentParser(description="Cross-check the MCP command surface.")
    parser.add_argument("--warn-only", action="store_true",
                        help="report everything but never fail (used while the legacy routing is retired)")
    parser.add_argument("--baseline", action="store_true",
                        help="print the legacy routing names that are not registered yet, then stop")
    args = parser.parse_args()

    allowlist = load_allowlist()
    registered = collect_registered()
    legacy = collect_legacy_routing()

    if args.baseline:
        print("registered: %d" % len(registered))
        print("legacy routing names: %d" % len(legacy))
        missing = sorted(set(legacy) - set(registered))
        print("in legacy routing but not registered (%d):" % len(missing))
        for name in missing:
            print("  %-40s %s" % (name, legacy[name]))
        extra = sorted(set(registered) - set(legacy))
        print("registered but not in legacy routing (%d):" % len(extra))
        for name in extra:
            print("  %-40s %s" % (name, registered[name]))
        return 1 if errors else 0

    sends = collect_python_sends()
    tools = collect_python_tools()
    docs = collect_doc_mentions(collect_param_names())

    # --- ERROR: a python send site names an unregistered command -------------------------------
    for name in sorted(sends):
        if name in registered:
            continue
        if name in allowlist:
            exempted.append("%s sent from %s (%s)" % (name, ", ".join(sends[name]), allowlist[name]))
            continue
        fail("python sends unregistered command '%s' from %s" % (name, ", ".join(sends[name])))

    # --- ERROR: a tool does not expose a required parameter of the command it forwards -----------
    forwarded = collect_tool_forwarded_command()
    signatures = collect_tool_supplied_params()
    required_params = collect_required_params()
    missing_params = []
    for tool, command in sorted(forwarded.items()):
        declared = signatures.get(tool, set())
        for param in sorted(required_params.get(command, set())):
            if param in declared or tool in allowlist:
                continue
            missing_params.append("tool '%s' forwards '%s' but does not expose required param '%s'"
                                  % (tool, command, param))
    for message in missing_params:
        fail(message)

    # --- ERROR: a tool sends keys the command does not declare, or forwards an argument under a
    # different key that is itself one of the command's parameters (same name, two meanings) -----
    command_params = collect_command_params()
    compared = 0
    skipped = []
    for tool, command, entries, site in collect_tool_send_params():
        declared = command_params.get(command)
        if declared is None:
            continue
        if entries is None:
            skipped.append("%s -> %s at %s" % (tool, command, site))
            continue
        compared += 1
        for key, source in entries:
            ident = "%s.%s" % (command, key)
            if key not in declared:
                # Advisory only: the param-spec extraction cannot see specs built by helpers
                # (AssetPathParam(), shared arrays), and some handlers read keys they never declare.
                if ident in allowlist:
                    exempted.append("%s sent by %s at %s (%s)" % (ident, tool, site, allowlist[ident]))
                    continue
                warn("tool '%s' sends key '%s' that command '%s' does not declare inline (%s)"
                     % (tool, key, command, site))
            elif source and source != key:
                ident = "%s.%s" % (command, source)
                if source in declared:
                    if ident in allowlist:
                        exempted.append("%s: %s -> %s in %s at %s (%s)"
                                        % (command, source, key, tool, site, allowlist[ident]))
                        continue
                    fail("tool '%s' forwards its argument '%s' as '%s', but '%s' is also a parameter of '%s' "
                         "with another meaning (%s); declared: %s"
                         % (tool, source, key, source, command, site, ", ".join(sorted(declared))))
                else:
                    warn("tool '%s' renames '%s' -> '%s' for '%s' (%s)" % (tool, source, key, command, site))

    # --- ERROR: duplicate tool names -----------------------------------------------------------
    for name, sites in sorted(tools.items()):
        if len(sites) > 1:
            fail("tool '%s' is registered %d times: %s" % (name, len(sites), ", ".join(sites)))

    # --- ERROR: documentation names something that does not exist ------------------------------
    known = set(tools) | set(registered)
    for name, sites in sorted(docs.items()):
        if name in known:
            continue
        if name in allowlist:
            exempted.append("doc mention %s at %s (%s)" % (name, ", ".join(sites), allowlist[name]))
            continue
        fail("skill doc names '%s' which is neither a python tool nor a registered command (%s)"
             % (name, ", ".join(sites)))

    # --- ERROR: a second list of command names is back -----------------------------------------
    for name, site in sorted(legacy.items()):
        fail("command name compared in dispatch code outside the registry: '%s' at %s" % (name, site))

    # --- WARNING: registered commands nobody sends / nobody documents --------------------------
    for name in sorted(registered):
        if name in sends:
            continue
        if name in allowlist:
            continue
        warn("registered command '%s' is never sent from python (%s)" % (name, registered[name]))
    for name in sorted(tools):
        if any(name in line for line in docs.values()) or name in docs:
            continue
        warn("python tool '%s' is not mentioned by any skill doc" % name)

    print("registered commands : %d" % len(registered))
    print("python send sites   : %d distinct commands from %d call sites"
          % (len(sends), sum(len(v) for v in sends.values())))
    print("python tools        : %d" % len(tools))
    print("documented mentions : %d" % len(docs))
    print("legacy name compares: %d" % len(legacy))
    print("tools forwarding one command: %d" % len(forwarded))
    print("commands with required params: %d" % len(required_params))
    print("tool send sites param-checked: %d (skipped, not static: %d)" % (compared, len(skipped)))
    if exempted:
        report("allowlisted (reviewed, not a mistake):", exempted)
    report("WARNINGS (%d):" % len(warnings), warnings)
    report("ERRORS (%d):" % len(errors), errors)

    if errors and not args.warn_only:
        print("")
        print("CONSISTENCY_RESULT: FAIL (%d errors)" % len(errors))
        return 1
    print("")
    print("CONSISTENCY_RESULT: %s" % ("WARN-ONLY (%d errors downgraded)" % len(errors) if errors else "OK"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
