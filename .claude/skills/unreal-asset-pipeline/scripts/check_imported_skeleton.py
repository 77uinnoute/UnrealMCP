"""导入后自检：网格尺寸 + 骨架"有没有内嵌 scale"（只读，编辑器 python 跑）。

用 execute_python_file 跑本脚本（`Content/Python` 之外也行，传绝对路径）。

判据（SKILL §五）：
  * mesh 高度 ≈ 角色真实身高（cm）
  * 任取一根骨：LOCAL 平移与世界坐标**同为厘米量级** => 干净骨架（可安全做重定向/写平移轨道）
    LOCAL 是米、WORLD 是厘米 => 骨架内嵌了 x100 scale（来自"米制数据 + 让 UE 做 m->cm 转换"的导出），
    重定向会整副骨架塌成一点（UE重定向 SKILL §二.3b）——回 Blender 用 blender_pmx_to_ue_fbx.py 重导。
"""
import unreal

# ---------------- 参数 ----------------
MESHES = ['/Game/MCP/Ganyu/Ganyu_UE']
# 每套骨架给一根"能定性"的骨（MMD 常用 センター / 腰；UE 系 pelvis / spine_02）
PROBE = {'/Game/MCP/Ganyu/Ganyu_UE': ['\u30bb\u30f3\u30bf\u30fc', '\u8170', '\u4e0a\u534a\u8eab']}
EXPECT_H = {'/Game/MCP/Ganyu/Ganyu_UE': 159.24}   # 期望高度 cm（Blender 实测 x100）
# --------------------------------------

L = unreal.AnimPoseSpaces.LOCAL
W = unreal.AnimPoseSpaces.WORLD

for path in MESHES:
    mesh = unreal.EditorAssetLibrary.load_asset(path)
    print('=====', path, mesh)
    if mesh is None:
        print('  MISSING')
        continue
    sk = mesh.get_editor_property('skeleton')
    rp = unreal.AnimPoseExtensions.get_reference_pose(sk)
    h = round(mesh.get_bounds().box_extent.z * 2, 3)
    exp = EXPECT_H.get(path)
    print('  mesh_h', h, 'expect', exp, 'OK' if exp is None or abs(h - exp) < 1.0 else 'MISMATCH')
    print('  skeleton', sk.get_path_name(),
          'bones', len(unreal.AnimPoseExtensions.get_bone_names(rp)),
          'mats', len(mesh.get_editor_property('materials')))
    for b in PROBE.get(path, ['pelvis']):
        lp = unreal.AnimPoseExtensions.get_bone_pose(rp, b, L).translation
        wp = unreal.AnimPoseExtensions.get_bone_pose(rp, b, W).translation
        lm = max(abs(lp.x), abs(lp.y), abs(lp.z))
        # 探针骨要选躯干中段（离原点够远）；局部量级 < 1 而世界是厘米 => 内嵌 scale
        verdict = 'CLEAN_CM' if lm > 1.0 else 'EMBEDDED_x100_SCALE'
        print('  bone %s local=%s world=%s -> %s' % (
            b, [round(v, 3) for v in (lp.x, lp.y, lp.z)], [round(v, 3) for v in (wp.x, wp.y, wp.z)], verdict))
print('CHECK_DONE')
