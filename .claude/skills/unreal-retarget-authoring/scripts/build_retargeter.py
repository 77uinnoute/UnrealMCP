"""建/改 IK Retargeter：绑两侧 IK Rig -> preview mesh -> auto_map_chains -> auto_align(TARGET)
-> root settings（默认 1.0）-> 存盘 -> 回读链映射。

关键：**干净 cm 骨架不要动 root scale**（保持 1.0）。老的 MMD 骨架（骨盆父骨在原点 + 骨架内嵌
x100）才需要 `scale_horizontal/vertical = 0.01` 把厘米量级的根全局缩回米量级——见
重定向 SKILL §二.3b，别把那个 0.01 抄到干净骨架上。
"""
import unreal

# ---------------- 参数 ----------------
RTG = '/Game/MCP/Ganyu/RTG_Ganyu'
SOURCE_RIG = '/Game/Characters/Mannequins/Rigs/IK_Mannequin'
TARGET_RIG = '/Game/MCP/Ganyu/IK_Ganyu'
TARGET_MESH = '/Game/MCP/Ganyu/Ganyu_UE'
ROOT_SCALE = (1.0, 1.0)           # (horizontal, vertical)；干净骨架 = 1.0
# --------------------------------------

T = unreal.RetargetSourceOrTarget.TARGET
S = unreal.RetargetSourceOrTarget.SOURCE
EA = unreal.EditorAssetLibrary
at = unreal.AssetToolsHelpers.get_asset_tools()

rtr = EA.load_asset(RTG)
if rtr is None:
    rtr = at.create_asset(RTG.split('/')[-1], RTG.rsplit('/', 1)[0], unreal.IKRetargeter,
                          unreal.IKRetargetFactory())
rc = unreal.IKRetargeterController.get_controller(rtr)
rc.set_ik_rig(S, EA.load_asset(SOURCE_RIG))
rc.set_ik_rig(T, EA.load_asset(TARGET_RIG))
try:
    rc.set_preview_mesh(T, EA.load_asset(TARGET_MESH))
except Exception as e:
    print('preview_err', str(e)[:90])

print('auto_map', rc.auto_map_chains(unreal.AutoMapChainType.EXACT, True))
print('auto_align', rc.auto_align_all_bones(T))
rs = rc.get_root_settings()
rs.set_editor_property('scale_horizontal', ROOT_SCALE[0])
rs.set_editor_property('scale_vertical', ROOT_SCALE[1])
rc.set_root_settings(rs)
rs2 = rc.get_root_settings()
print('ROOT_SCALE', rs2.get_editor_property('scale_horizontal'), rs2.get_editor_property('scale_vertical'))

for s in rc.get_all_chain_settings():
    src = tgt = '?'
    try:
        src = str(s.get_editor_property('source_chain'))
        tgt = str(s.get_editor_property('target_chain'))
    except Exception:
        pass
    print('MAP', src, '<->', tgt)
print('SAVE', EA.save_asset(RTG))
print('RTG_DONE')
