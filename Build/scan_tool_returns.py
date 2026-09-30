"""Static scan: MCP tool functions whose top-level return value is a list/tuple.

MCP SDK flattens a returned list into one content block per element
(mcp/server/fastmcp/utilities/func_metadata.py::_convert_to_content), which shows up
in clients as one response per element. Tools must return a single dict (or str).

Diagnostic only - not an acceptance tool.
"""
import ast
import pathlib
import sys

TOOLS_DIR = pathlib.Path(__file__).resolve().parent.parent / "Content" / "Python" / "tools"
if len(sys.argv) > 1:
    TOOLS_DIR = pathlib.Path(sys.argv[1])


def decorated_as_tool(node: ast.FunctionDef) -> bool:
    for dec in node.decorator_list:
        target = dec.func if isinstance(dec, ast.Call) else dec
        if isinstance(target, ast.Attribute) and target.attr == "tool":
            return True
    return False


class Collector(ast.NodeVisitor):
    def __init__(self):
        self.hits = []

    def visit_FunctionDef(self, node):
        if decorated_as_tool(node):
            for stmt in ast.walk(node):
                if isinstance(stmt, (ast.FunctionDef, ast.AsyncFunctionDef)) and stmt is not node:
                    continue
                if isinstance(stmt, ast.Return):
                    self.note(node, stmt)
        # do not descend into nested defs of tools again
        for child in node.body:
            if not isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef)):
                self.generic_visit(child)

    def note(self, fn, ret):
        if ret.value is None:
            self.hits.append((fn, ret, "None -> empty content block"))
            return
        if isinstance(ret.value, (ast.List, ast.ListComp, ast.Tuple)):
            self.hits.append((fn, ret, "list/tuple literal -> one response per element"))
        elif isinstance(ret.value, ast.IfExp):
            for branch in (ret.value.body, ret.value.orelse):
                if isinstance(branch, (ast.List, ast.ListComp, ast.Tuple)):
                    self.hits.append((fn, ret, "conditional list/tuple -> one response per element"))
        elif isinstance(ret.value, ast.Call):
            attr = getattr(ret.value.func, "attr", None)
            name = getattr(ret.value.func, "id", None)
            if attr in ("get", "values", "keys", "items", "split", "copy") or name in ("list", "tuple", "sorted", "set"):
                self.hits.append((fn, ret, f"unclear call ({name or attr}) - check if list at runtime"))
        elif isinstance(ret.value, ast.Subscript):
            self.hits.append((fn, ret, "subscript return - check if list at runtime"))
        elif isinstance(ret.value, ast.Name):
            self.hits.append((fn, ret, f"named return '{ret.value.id}' - trace its source"))


def main():
    total_tools = 0
    for path in sorted(TOOLS_DIR.glob("*.py")):
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        col = Collector()
        col.visit(tree)
        total_tools += sum(1 for n in ast.walk(tree) if isinstance(n, ast.FunctionDef) and decorated_as_tool(n))
        for fn, ret, why in col.hits:
            print(f"{path.name}:{ret.lineno} {fn.name}() -> {why}")
    print(f"scanned {total_tools} tools in {TOOLS_DIR}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
