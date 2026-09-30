"""建运行时重定向 AnimBP：工厂带 target_skeleton -> AnimGraph 加 RetargetPoseFromMesh
-> 写 IKRetargeterAsset / bUseAttachedParent -> Pose -> Root.Result -> 编译断言 -> 换 actor anim_class。

**必须用工厂带 skeleton 建新资产**（重定向 SKILL §三⑥）：先建空 ABP 再事后
`set_editor_property('target_skeleton')` + 重编译的类在 PIE 里会**静默输出 rest pose**
（编译全绿、无警告、节点属性与能用的 ABP 逐字段相同），而且这种类 `delete_asset` 删不掉 ——
所以本脚本总是 **建新名**（NAME 里带版本号），不要复用旧名字。
"""
import unreal

# ---------------- 参数 ----------------
NAME = 'ABP_GanyuRT2'
DIR = '/Game/Blueprints'
SKELETON = '/Game/MCP/Ganyu/Gan_n2_Skeleton'
RTG = '/Game/MCP/Ganyu/RTG_Ganyu'
USE_ATTACHED_PARENT = True
RETARGET_ACTOR_LABEL = 'RT_Target_Ganyu'     # 顺便把关卡里目标 actor 的 anim_class 换成新类（可留空串跳过）
LEVEL_SAVE = True
# --------------------------------------

EA = unreal.EditorAssetLibrary
lib = unreal.UnrealMCPBlueprintGraphLibrary
at = unreal.AssetToolsHelpers.get_asset_tools()

if EA.does_asset_exist(DIR + '/' + NAME):
    raise SystemExit('资产已存在：换个新名字（别复用，旧类可能是废类）')

fac = unreal.AnimBlueprintFactory()
fac.set_editor_property('parent_class', unreal.AnimInstance)
fac.set_editor_property('target_skeleton', EA.load_asset(SKELETON))
bp = at.create_asset(NAME, DIR, unreal.AnimBlueprint, fac)
print('bp', bp, 'skel', bp.get_editor_property('target_skeleton'))

ag = None
root = None
for g in lib.get_graphs(bp).graphs:
    if g.graph_name == 'AnimGraph':
        ag = g.graph
for n in lib.get_graph_nodes(ag).nodes:
    if n.type == 'AnimGraphNode_Root':
        root = n.node          # node_info -> 节点对象
r = lib.add_node_by_class(ag, 'AnimGraphNode_RetargetPoseFromMesh', 320, 0)
node = r.node
inner = node.get_editor_property('node')      # 节点对象 -> FAnimNode 结构
inner.set_editor_property('ik_retargeter_asset', EA.load_asset(RTG))
inner.set_editor_property('bUseAttachedParent', USE_ATTACHED_PARENT)
node.set_editor_property('node', inner)
print('node_cfg', inner.get_editor_property('ik_retargeter_asset'), inner.get_editor_property('bUseAttachedParent'))
print('connect', lib.connect_pins(node, 'Pose', root, 'Result').success)

res = lib.compile_blueprint_checked(bp)
print('COMPILE', res.status, res.compiled, 'errors', list(res.errors), 'warnings', list(res.warnings))
assert res.compiled and not list(res.errors), 'ABP 编译失败'
cls = unreal.load_class(None, '%s/%s.%s_C' % (DIR, NAME, NAME))
print('CLASS', cls)
print('SAVE', unreal.EditorLoadingAndSavingUtils.save_packages([bp.get_outermost()], False))

if RETARGET_ACTOR_LABEL:
    w = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    for a in unreal.GameplayStatics.get_all_actors_of_class(w, unreal.SkeletalMeshActor):
        if a.get_actor_label() == RETARGET_ACTOR_LABEL:
            c = a.get_components_by_class(unreal.SkeletalMeshComponent)[0]
            c.set_editor_property('anim_class', cls)
            print('actor anim_class ->', cls.get_name())
    if LEVEL_SAVE:
        print('save_level', unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level())
print('ABP_DONE')
