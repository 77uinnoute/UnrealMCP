import unreal

lib = unreal.UnrealMCPBlueprintGraphLibrary
EA = unreal.EditorAssetLibrary


def nodes_of(graph):
    try:
        return lib.get_graph_nodes(graph).nodes
    except Exception as e:
        print("      get_graph_nodes fail: " + str(e))
        return []


def raw(o, prop, default=None):
    try:
        return o.get_editor_property(prop)
    except Exception:
        return default


def anim_of(ni):
    try:
        n = ni.node
        if ni.type == "AnimGraphNode_SequencePlayer":
            return "seq=" + str(n.get_editor_property("node").get_editor_property("sequence").get_name())
        if ni.type == "AnimGraphNode_BlendSpacePlayer":
            return "bs=" + str(n.get_editor_property("node").get_editor_property("blend_space").get_name())
        if ni.type == "AnimGraphNode_TwoWayBlend":
            return "twoway"
        if ni.type == "AnimGraphNode_BlendListByBool":
            return "blendlistbybool"
        if ni.type == "AnimGraphNode_StateMachine":
            return "state_machine name=" + ni.name
    except Exception as e:
        return "anim_err=" + str(e)[:60]
    return ""


def dump_abp(path, label):
    print("=" * 72)
    print("### " + label + " : " + path)
    bp = EA.load_asset(path)
    if not bp:
        print("LOAD FAILED")
        return
    graphs = lib.get_graphs(bp).graphs
    for gi in graphs:
        print(" graph: " + gi.graph_name + " class=" + gi.graph.get_class().get_name() + " nodes=" + str(gi.node_count))
    for gi in graphs:
        if gi.graph_name != "AnimGraph":
            continue
        nis = nodes_of(gi.graph)
        nis = sorted(nis, key=lambda n: (n.pos_x, n.pos_y))
        print(" AnimGraph nodes = %d" % len(nis))
        for ni in nis:
            print("  [%s] %-28s pos=(%s,%s) %s" % (ni.type, ni.name[:28], ni.pos_x, ni.pos_y, anim_of(ni)))
            for p in ni.pins:
                try:
                    d = p.default_value
                except Exception:
                    d = ""
                lk = ""
                try:
                    lk = p.linked_to
                except Exception:
                    lk = ""
                if lk or d != "":
                    print("      pin %-14s %-6s def=%s links=%s" % (p.pin_name, p.direction, str(d)[:24], str(lk)[:120]))
    print("--- variables ---")
    try:
        for pr in lib.get_variables(bp).variables:
            print("  " + str(pr.variable_name) + " : " + str(pr.variable_type))
    except Exception as e:
        print("get_variables fail " + str(e)[:80])
    try:
        res = lib.compile_blueprint_checked(bp)
        print("compile status=" + str(res.status) + " errors=" + str(res.errors)[:200])
    except Exception as e:
        print("compile err " + str(e)[:100])


dump_abp("/Game/MMD/FeiYing/ABP_FeiYing", "FEIYING")
dump_abp("/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed", "TEMPLATE")
