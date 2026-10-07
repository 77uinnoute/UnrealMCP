"""fei_741: detailed dump of BP_FeiYing EventGraph + CanJumpInternal (pins, defaults, links)."""
import json

import unreal


def bridge(command, **params):
    r = json.loads(unreal.UnrealMCPPythonAPI.execute_mcp_command(command, json.dumps(params)))
    res = r.get("result") or {}
    if r.get("status") != "success":
        return {"_error": json.dumps(r)[:300]}
    return res["data"] if isinstance(res, dict) and isinstance(res.get("data"), dict) else res


BP = "/Game/MMD/FeiYing/Blueprints/BP_FeiYing"
for graph_name in ("EventGraph", "CanJumpInternal"):
    nodes = bridge("find_blueprint_nodes", blueprint_name=BP, graph_name=graph_name, node_type="All")
    print("=" * 30, graph_name)
    for n in nodes.get("nodes", []):
        fr = n.get("function_reference") or {}
        label = fr.get("member_name") or n.get("type")
        print("NODE %s  %s  pos=(%s,%s)  id=%s" % (n.get("type"), label, n.get("pos_x"), n.get("pos_y"), n.get("node_id")[:8]))
        for p in n.get("pins", []):
            dv = p.get("default_value")
            do = p.get("default_object")
            if not (p.get("connected") or (dv not in (None, "", "None")) or do):
                continue
            print("    pin %-28s %-6s %-22s def=%-28s obj=%-40s link=%s" % (
                p.get("pin_name"), p.get("direction"), p.get("category"),
                str(dv)[:28], str(do)[:40], p.get("linked_to")))
        props = n.get("properties") or {}
        if props:
            print("    props", json.dumps(props, default=str)[:300])

# parent class + CDO defaults
bp = unreal.load_asset(BP)
print("parent_class", bp.get_editor_property("parent_class").get_path_name())
cdo = unreal.get_default_object(bp.generated_class())
for name in ("AttackMontages", "AttackIndex", "JumpAllowed"):
    try:
        print("CDO", name, "=", cdo.get_editor_property(name))
    except Exception as e:  # noqa: BLE001
        print("CDO", name, "err", str(e)[:80])
h = bridge("get_blueprint_component_hierarchy", blueprint_name=BP)
print("components:", [(c["component_name"], c["component_class"], c.get("parent"), c.get("is_root"), c.get("is_inherited"))
                      for c in h.get("components", [])])
print("root_components", h.get("root_components"), "duplicate", h.get("duplicate_components"))
