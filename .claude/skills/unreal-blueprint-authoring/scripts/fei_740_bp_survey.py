"""fei_740: read-only survey of BP_FeiYing / BP_FeiYingGameMode (graphs, variables, functions, components, nodes)."""
import json

import unreal


def bridge(command, **params):
    r = json.loads(unreal.UnrealMCPPythonAPI.execute_mcp_command(command, json.dumps(params)))
    res = r.get("result") or {}
    if r.get("status") != "success":
        return {"_error": json.dumps(r)[:300]}
    return res["data"] if isinstance(res, dict) and isinstance(res.get("data"), dict) else res


for BP in ("/Game/MMD/FeiYing/Blueprints/BP_FeiYing", "/Game/MMD/FeiYing/Blueprints/BP_FeiYingGameMode"):
    print("=" * 20, BP)
    g = bridge("list_blueprint_graphs", blueprint_name=BP)
    print("graphs:", [(x["graph_name"], x["graph_node_class"] if "graph_node_class" in x else x.get("graph_class"), x["node_count"])
                      for x in g.get("graphs", [])])
    v = bridge("list_blueprint_variables", blueprint_name=BP)
    for item in v.get("variables", []):
        print("  VAR", item.get("variable_name"), item.get("type"), item.get("container"),
              "def=", str(item.get("default_value"))[:40], "flags=", item.get("flags"))
    f = bridge("list_blueprint_function_graphs", blueprint_name=BP)
    for item in f.get("function_graphs", []):
        print("  FN", item.get("graph_name"), "override=", item.get("is_override"), "nodes=", item.get("node_count"))

BP = "/Game/MMD/FeiYing/Blueprints/BP_FeiYing"
h = bridge("get_blueprint_component_hierarchy", blueprint_name=BP)
print("=" * 20, "components")
for c in h.get("components", []):
    print("  COMP", c["component_name"], c["component_class"], "parent=", c.get("parent"), "root=", c.get("is_root"))
print("root_count", h.get("root_count"), "unique_root", h.get("unique_root"))

print("=" * 20, "nodes per graph")
for gr in bridge("list_blueprint_graphs", blueprint_name=BP).get("graphs", []):
    name = gr["graph_name"]
    nodes = bridge("find_blueprint_nodes", blueprint_name=BP, graph_name=name, node_type="All")
    print("--", name, "nodes:", nodes.get("count"))
    for n in nodes.get("nodes", []):
        fr = n.get("function_reference") or {}
        ev = n.get("event_reference") or {}
        props = n.get("properties") or {}
        interesting = {k: props[k] for k in ("EventReference", "InputKey", "TargetType", "VariableReference",
                                            "Enum", "StructType", "Montage", "AnimClass", "Sequence")
                       if k in props}
        print("   *", n.get("type"), "|", n.get("node_title") or n.get("name"),
              "| fn=", fr.get("member_name"), "| ev=", ev.get("member_name"),
              "| props=", json.dumps(interesting, default=str)[:160],
              "| pos=", n.get("pos_x"), n.get("pos_y"))
