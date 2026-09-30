"""Insert the optional `persist` parameter into the registrations that need it.

Text patch, no editor needed. A registration is eligible when
  * its flags say persist_after_success (4th MCPFlags arg) - blueprint domains, or
  * its flags say mutates_graph (2nd arg) - animation / particle wrappers read the
    parameter themselves, or
  * its command name is in the explicit list below (self-saving handlers: PCG, UMG),
and it does not already declare `persist`.

Usage: python Build/patch_persist_param.py [--check]
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "Source" / "UnrealMCP" / "Private" / "Commands"

FILES = [
    SRC / "Blueprint" / "UnrealMCPBlueprintCommands.cpp",
    SRC / "Blueprint" / "UnrealMCPBlueprintNodeCommands.cpp",
    SRC / "Animation" / "UnrealMCPAnimationCommands.cpp",
    SRC / "Particle" / "UnrealMCPParticleCommands.cpp",
    SRC / "PCG" / "UnrealMCPPCGCommands.cpp",
    SRC / "UnrealMCPUMGCommands.cpp",
]

BY_NAME = {
    "create_blueprint",
    # PCG graph savers
    "add_pcg_node", "connect_pcg_pins", "disconnect_pcg_pins", "remove_pcg_node",
    "set_pcg_node_property", "create_pcg_graph", "set_pcg_mesh_selector_type",
    "set_pcg_mesh_selector_entries",
    # UMG (compile is kept; only the save is gated)
    "create_umg_widget_blueprint", "add_text_block_to_widget", "add_button_to_widget",
    "bind_widget_event", "set_widget_slot", "set_text_block_binding",
}

PARAM_LINE = ('            MCPParamOpt(TEXT("persist"), TEXT("bool"), '
              'TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),\n')

REG = re.compile(r"MCP_REGISTER_COMMAND\(Registry,\s*\"([a-z0-9_]+)\"")
FLAGS = re.compile(r"MCPFlags\(([^)]*)\)")
CLOSE = re.compile(r"\n\s*\}\)\s*,\s*MCPFlags\(")


def flags_of(block):
    m = FLAGS.search(block)
    if not m:
        return None
    args = [a.strip().lower() == "true" for a in m.group(1).split(",")]
    while len(args) < 4:
        args.append(False)
    return args


def insert_before_flags(block):
    """Insert the param as the last entry of the `(TArray<FMCPParamSpec>{ ... })` array.

    Anchor: the `}` that closes that array, i.e. the last `}` before `MCPFlags(`. Works for
    both the multi-line and the single-line array spellings, and adds a comma if the last
    entry has none.
    """
    fi = block.find("MCPFlags(")
    if fi < 0:
        return None
    close = block.rfind("}", 0, fi)
    if close < 0:
        return None
    before = block[:close].rstrip()
    needs_comma = not (before.endswith("{") or before.endswith(","))
    head = block[:close].rstrip()
    if needs_comma:
        head += ","
    tail = block[close:]
    return head + "\n" + PARAM_LINE.rstrip("\n") + "\n        " + tail


def patch(path, check):
    text = path.read_text(encoding="utf-8")
    parts = re.split(r"(?=MCP_REGISTER_COMMAND\(Registry,)", text)
    inserted, skipped, eligible = [], [], []
    out = []
    for i, part in enumerate(parts):
        if i == 0:
            out.append(part)
            continue
        name_m = REG.search(part)
        if not name_m:
            out.append(part)
            continue
        name = name_m.group(1)
        fl = flags_of(part)
        if fl is None:
            out.append(part)
            continue
        want = (fl[3] or fl[1] or name in BY_NAME)
        if not want or '"persist"' in part:
            skipped.append(name)
            out.append(part)
            continue
        eligible.append(name)
        new_part = insert_before_flags(part)
        if new_part is None:
            skipped.append(name + "(no-anchor)")
            out.append(part)
            continue
        out.append(new_part)
        inserted.append(name)
    if not check:
        path.write_text("".join(out), encoding="utf-8")
    return inserted, eligible, skipped


def main():
    check = "--check" in sys.argv
    total = 0
    for f in FILES:
        if not f.exists():
            print("MISSING %s" % f)
            continue
        ins, elig, skip = patch(f, check)
        total += len(ins)
        print("%-42s inserted=%d eligible=%d" % (f.name, len(ins), len(elig)))
        if ins:
            print("    first/last: %s / %s" % (ins[0], ins[-1]))
        no_anchor = [s for s in skip if s.endswith("(no-anchor)")]
        if no_anchor:
            print("    no-anchor (%d): %s" % (len(no_anchor), ", ".join(no_anchor[:12])))
    print("TOTAL_INSERTED %d%s" % (total, " (check mode, nothing written)" if check else ""))


if __name__ == "__main__":
    main()
