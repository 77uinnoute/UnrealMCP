"""改名换骨架后的等价性验证：逐骨比对 rest 姿势（只改名、不动数据的硬证据）。

流程：
  1) 改名前：dump_skeleton_baseline.py -> baseline_pre_rename.json
  2) Blender：blender_mmd_rename_ascii.py -> rename_map.json + 新 FBX
  3) purge 旧 mesh/skeleton（purge_skeletal_assets.py）-> import_assets 新 FBX
  4) 本脚本：用 rename_map.json 把旧名映射到新名，逐骨比 LOCAL t/r

通过判据（实测 Ganyu 315/315）：非 ASCII 骨名 0 根；Δt_max < 1e-4 cm；|quat_dot| > 0.9999。
顺带体检：mesh 高度、材质槽数、材质是否仍指向修好的材质。
"""
import json

import unreal

# ---------------- 参数 ----------------
MESH = '/Game/MCP/Ganyu/Ganyu_UE'
MAP_JSON = r"e:\ue_proj\asset_test_UE55 (2)\asset_test_UE55\Saved\MCPAssets\MMD_Ganyu\rename_map.json"
BASE_JSON = r"e:\ue_proj\asset_test_UE55 (2)\asset_test_UE55\Saved\MCPAssets\MMD_Ganyu\baseline_pre_rename.json"
EXPECT_H = 159.241
DT_TOL = 1e-4
DOT_TOL = 0.9999
# --------------------------------------

mapping = json.load(open(MAP_JSON, encoding='utf-8'))
baseline = {}
for r in json.load(open(BASE_JSON, encoding='utf-8')):
    baseline[r['name']] = r
    baseline[r['name'].replace('.', '_')] = r      # UE 侧旧名把 . 变成 _（FBX 命名空间约定）

mesh = unreal.EditorAssetLibrary.load_asset(MESH)
sk = mesh.get_editor_property('skeleton')
rp = unreal.AnimPoseExtensions.get_reference_pose(sk)
L = unreal.AnimPoseSpaces.LOCAL
names = [str(n) for n in unreal.AnimPoseExtensions.get_bone_names(rp)]
nonascii = [n for n in names if any(ord(c) > 127 for c in n)]
print('mesh_h', round(mesh.get_bounds().box_extent.z * 2, 3), 'expect', EXPECT_H)
print('skeleton', sk.get_path_name(), 'bones', len(names), 'non_ascii', len(nonascii), nonascii[:5])
print('mats', len(mesh.get_editor_property('materials')),
      'mat0', mesh.get_editor_property('materials')[0].material_interface.get_name())

dt_max, dot_min, missing, pairs, worst = 0.0, 1.0, [], 0, None
for old, new in mapping.items():
    if new not in names:
        missing.append((old, new))
        continue
    b = baseline.get(old) or baseline.get(old.replace('.', '_'))
    if b is None:
        continue
    p = unreal.AnimPoseExtensions.get_bone_pose(rp, new, L)
    dt = max(abs(p.translation.x - b['t'][0]), abs(p.translation.y - b['t'][1]), abs(p.translation.z - b['t'][2]))
    dot = abs(p.rotation.x * b['r'][0] + p.rotation.y * b['r'][1]
              + p.rotation.z * b['r'][2] + p.rotation.w * b['r'][3])
    pairs += 1
    dt_max, worst = max(dt_max, dt), (worst if dt_max > dt else (old, new, dt))
    dot_min = min(dot_min, dot)
print('pairs_compared', pairs, 'missing_in_new', missing[:5], 'count', len(missing))
print('max_local_translation_delta_cm', round(dt_max, 6), 'worst', worst)
print('min_abs_quat_dot', round(dot_min, 6))
print('VERDICT',
      'PASS' if (pairs > 0 and not missing and not nonascii and dt_max < DT_TOL and dot_min > DOT_TOL) else 'CHECK')
print('EQUIV_DONE')
