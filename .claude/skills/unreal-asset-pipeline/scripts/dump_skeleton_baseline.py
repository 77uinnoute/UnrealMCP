"""改名前的基线：把当前骨架每根骨的 rest 姿势（LOCAL 平移 + 旋转）落盘。

配合 verify_bone_rename_equiv.py 使用：改名换骨架后，用 rename_map.json 把旧名映射到新名，
逐骨比对 LOCAL 平移/旋转 —— 期望 Δt < 1e-4 cm、四元数 |dot| > 0.9999，
以此证明"只改了名字，没动姿势/层级/权重"。

必须在**改名前**、且旧骨架还在时跑。
"""
import json

import unreal

# ---------------- 参数 ----------------
SKEL = '/Game/MCP/Ganyu/Ganyu_UE_Skeleton'          # 改名前的骨架
OUT = r"e:\ue_proj\asset_test_UE55 (2)\asset_test_UE55\Saved\MCPAssets\MMD_Ganyu\baseline_pre_rename.json"
# --------------------------------------

sk = unreal.EditorAssetLibrary.load_asset(SKEL)
rp = unreal.AnimPoseExtensions.get_reference_pose(sk)
L = unreal.AnimPoseSpaces.LOCAL
rows = []
for i, n in enumerate([str(x) for x in unreal.AnimPoseExtensions.get_bone_names(rp)]):
    p = unreal.AnimPoseExtensions.get_bone_pose(rp, n, L)
    rows.append({'i': i, 'name': n,
                 't': [round(v, 5) for v in (p.translation.x, p.translation.y, p.translation.z)],
                 'r': [round(v, 5) for v in (p.rotation.x, p.rotation.y, p.rotation.z, p.rotation.w)]})
with open(OUT, 'w', encoding='utf-8') as f:
    json.dump(rows, f, ensure_ascii=True, indent=1)
print('BASELINE_BONES', len(rows), OUT)
print('BASELINE_DONE')
