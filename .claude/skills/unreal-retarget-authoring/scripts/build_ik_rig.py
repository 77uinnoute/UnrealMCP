"""建 IK Rig（target）到指定骨架：set mesh -> 建链 -> set retarget root -> 回读 -> 存盘。

链跨度用**骨下标**给（骨名从 dump_skeleton_bones.py 的 JSON 里取，保持本源码 ASCII）。
`add_retarget_chain` 是 **4 参数**（goal_name 传 ''），返回引擎唯一化后的链名——一律用返回值。

布局纪律（重定向 SKILL §二.4）：Root 链覆盖**骨架根骨**（无父骨那根），
**骨盆/腰不要放进任何链**（它只当 retarget root，否则局部平移会被链解码器写坏）。
本例 Ganyu：Root=全ての親(2)、Spine=上半身(160)->上半身2(162)、腿=足->足首、臂=腕->手首，retarget root=腰(7)。
"""
import json

import unreal

# ---------------- 参数 ----------------
MESH = '/Game/MCP/Ganyu/Ganyu_UE'
RIG = '/Game/MCP/Ganyu/IK_Ganyu'
BONES_JSON = r"e:\ue_proj\asset_test_UE55 (2)\asset_test_UE55\Saved\MCPAssets\MMD_Ganyu\ue_bones.json"
ROOT_IDX = 7                      # retarget root（骨盆/腰）
CHAINS = [('Root', 2, 2),         # (链名, start 下标, end 下标) —— 链名照抄源骨架的人形模板名
          ('Spine', 160, 162),
          ('Neck', 163, 163),
          ('Head', 164, 164),
          ('LeftClavicle', 211, 212),
          ('LeftArm', 213, 217),
          ('LeftLeg', 9, 11),     # 末端取踝（别含脚掌/脚尖，否则脚的相位分配被拉歪）
          ('RightClavicle', 260, 261),
          ('RightArm', 262, 266),
          ('RightLeg', 18, 20)]
# --------------------------------------

N = {r['i']: r['name'] for r in json.load(open(BONES_JSON, encoding='utf-8'))}
EA = unreal.EditorAssetLibrary
at = unreal.AssetToolsHelpers.get_asset_tools()
mesh = EA.load_asset(MESH)

rig = EA.load_asset(RIG)
if rig is None:
    rig = at.create_asset(RIG.split('/')[-1], RIG.rsplit('/', 1)[0], unreal.IKRigDefinition,
                          unreal.IKRigDefinitionFactory())
ctrl = unreal.IKRigController.get_controller(rig)
print('set_mesh', ctrl.set_skeletal_mesh(mesh))

existing = [c.get_editor_property('chain_name') for c in ctrl.get_retarget_chains()]
for name, s, e in CHAINS:
    if name in existing:
        print('CHAIN_EXIST', name)
        continue
    print('CHAIN', name, ctrl.add_retarget_chain(name, N[s], N[e], ''))

print('SET_RETARGET_ROOT', ctrl.set_retarget_root(N[ROOT_IDX]))
for c in ctrl.get_retarget_chains():
    n = c.get_editor_property('chain_name')
    print('VERIFY', n, '|', ctrl.get_retarget_chain_start_bone(n), '->', ctrl.get_retarget_chain_end_bone(n))
print('ROOT', ctrl.get_retarget_root())
print('SAVE', EA.save_asset(RIG))
print('IK_RIG_DONE')
