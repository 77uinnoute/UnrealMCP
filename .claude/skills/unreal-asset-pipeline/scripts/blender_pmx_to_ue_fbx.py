"""PMX -> UE-ready FBX（干净 cm 骨架），在 Blender 里跑（--background --python）。

用途：MMD(PMX) 模型导进 UE，且拿到的骨架**局部平移就是 cm、根骨内嵌 scale=0**
（重定向/写平移轨道的前提，见 UE重定向 SKILL §二.3b）。

做三件事：
  1. mmd_tools 导入 PMX（scale=0.08，模型约 1.6 m）
  2. 把 MMD "付与/D 变形骨" 的权重并到 FK 骨（UE 没有付与机制，否则腿不动）
  3. 把 x100 烤进 mesh 顶点与骨骼 head/tail，再 global_scale=0.01 导出
     => UE 里 mesh 高度=真实 cm，且骨局部平移是 cm（无内嵌 scale）

用法：
    "D:\\blender-5.2.0-windows-x64\\blender-5.2.0-windows-x64\\blender.exe" \
        --background --python <本脚本绝对路径>

验收（导入 UE 后跑 check_imported_skeleton.py）：
    mesh_h ≈ 身高(cm)；骨局部平移与世界坐标**同为厘米量级**
本例实测：mesh 159.24 cm、センター local -66.15 / world 66.24、腰 local -24.26 / world 90.50
"""
import os
import shutil

import bpy

# ---------------- 参数 ----------------
SRC = r"C:\Users\aishengmin\Downloads\ganyu_by_原神_7381ccd84ee8763ce63b3ad638e1c49b\甘雨.pmx"
OUT = r"e:\ue_proj\asset_test_UE55 (2)\asset_test_UE55\Saved\MCPAssets\MMD_Ganyu"
NAME = "Ganyu_UE"                       # 产物 <NAME>.fbx / <NAME>.fbm/，UE 资产名 = 文件名
IMPORT_SCALE = 0.08                     # mmd_tools 默认：PMX -> Blender 米
GLOBAL_SCALE = 0.01                     # x100 数据 + 0.01 => UE(数值x100) 得到真实 cm
# --------------------------------------

FBX = os.path.join(OUT, NAME + ".fbx")
FBM = os.path.join(OUT, NAME + ".fbm")


def side_map():
    """MMD D 变形骨 -> FK 骨（UE 无付与：权重留在 *D 上 = 死皮）。"""
    m = {}
    for s in ("L", "R"):
        m["\u8db3D." + s] = "\u8db3." + s              # 足D  -> 足
        m["\u3072\u3056D." + s] = "\u3072\u3056." + s  # ひざD -> ひざ
        m["\u8db3\u9996D." + s] = "\u8db3\u9996." + s  # 足首D -> 足首
        m["\u8db3\u5148EX." + s] = "\u3064\u307e\u5148." + s  # 足先EX -> つま先
    return m


bpy.ops.wm.read_homefile(use_empty=True)
bpy.ops.preferences.addon_enable(module="bl_ext.user_default.mmd_tools")
bpy.ops.mmd_tools.import_model(filepath=SRC, scale=IMPORT_SCALE)

arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
mesh = next(o for o in bpy.data.objects if o.type == 'MESH' and len(o.data.vertices) > 1000)
print("DIMS_M", [round(v, 4) for v in mesh.dimensions], "BONES", len(arm.data.bones))

# ---- 1) D 骨权重 -> FK 骨（总权重守恒，源组清零）----
MAP = side_map()
vg = mesh.vertex_groups
name2g = {g.name: g for g in vg}
missing = [k for k in MAP if k not in name2g or MAP[k] not in name2g]
print("MAPPING_MISSING", missing)


def wsum(name):
    g = name2g.get(name)
    if g is None:
        return None
    idx = g.index
    return round(sum(e.weight for v in mesh.data.vertices for e in v.groups if e.group == idx), 1)


print("W_BEFORE_FK", {MAP[k]: wsum(MAP[k]) for k in MAP})
new_w = {}
for i, v in enumerate(mesh.data.vertices):
    d = {}
    for e in v.groups:
        src_name = next((g.name for g in vg if g.index == e.group), None)
        tgt = MAP.get(src_name, src_name)
        d[tgt] = d.get(tgt, 0.0) + e.weight
    new_w[i] = d
for src in MAP:
    name2g[src].remove(list(range(len(mesh.data.vertices))))
for i, d in new_w.items():
    for n, w in d.items():
        g = name2g.get(n)
        if g and w > 0.0:
            g.add([i], w, 'ADD')
print("W_AFTER_FK", {MAP[k]: wsum(MAP[k]) for k in MAP}, "W_AFTER_D", {k: wsum(k) for k in MAP})

# ---- 2) x100 烤进数据（角色尺寸交给 UE 的单位换算）----
# ⚠️ 带 shape key（morph）时，**key_blocks 与 mesh.vertices 都要缩**。
#    只缩 mesh.vertices 会让导出的 morph delta = shape(米) − base(厘米) ≈ 整个身高：
#    UE 侧一旦任何 morph 权重 > 0，网格就被撕开（实测：每个 morph 平均把顶点推 106，而角色只有 159 高）。
#    正确做法实测通过（Blender 5.2）：两块都 ×100，导出后回导量 delta/体型 = 3.8%。
for _kb in (mesh.data.shape_keys.key_blocks if mesh.data.shape_keys else []):
    for _d in _kb.data:
        _d.co = _d.co * 100.0
for v in mesh.data.vertices:
    v.co = v.co * 100.0

# 自查：任一脸部 morph 的位移不得超过体型的 5%（判据与单位无关）。
# 这一条就是上面那个坑的守门员 —— 只在 UE 里"打上 morph 标记才发现角色炸了"太晚。
if mesh.data.shape_keys and len(mesh.data.shape_keys.key_blocks) > 1:
    _base = [d.co.copy() for d in mesh.data.shape_keys.key_blocks[0].data]
    _body = max(max(p[i] for p in _base) - min(p[i] for p in _base) for i in range(3))
    _worst = 0.0
    for _kb in list(mesh.data.shape_keys.key_blocks)[1:]:
        for _i, _d in enumerate(_kb.data):
            _worst = max(_worst, (_d.co - _base[_i]).length)
    _ratio = _worst / _body if _body else 0.0
    print("MORPH_DELTA_CHECK worst=%.3f body=%.3f ratio=%.4f %s"
          % (_worst, _body, _ratio, "PASS" if _ratio <= 0.05 else "FAIL"))
bpy.context.view_layer.objects.active = arm
bpy.ops.object.mode_set(mode='EDIT')
for eb in arm.data.edit_bones:
    eb.head = eb.head * 100.0
    eb.tail = eb.tail * 100.0
bpy.ops.object.mode_set(mode='OBJECT')
for o in (arm, mesh):
    o.scale = (1.0, 1.0, 1.0)
mesh.matrix_parent_inverse.identity()
print("BAKED_CM", [round(v, 2) for v in mesh.dimensions],
      "upper_body_head_local", [round(v, 3) for v in arm.data.bones['\u4e0a\u534a\u8eab'].head_local])

# ---- 3) 贴图聚到 <NAME>.fbm/ 再导出 ----
os.makedirs(FBM, exist_ok=True)
for i in bpy.data.images:
    src = bpy.path.abspath(i.filepath) if i.filepath else ""
    if src and os.path.exists(src):
        dst = os.path.join(FBM, os.path.basename(src))
        if os.path.abspath(src) != os.path.abspath(dst):
            shutil.copyfile(src, dst)
        i.filepath = dst
print("FBM", sorted(os.listdir(FBM)))

bpy.ops.object.select_all(action='DESELECT')
arm.select_set(True)
mesh.select_set(True)
bpy.context.view_layer.objects.active = arm
print("EXPORT", bpy.ops.export_scene.fbx(
    filepath=FBX, use_selection=True, object_types={'ARMATURE', 'MESH'},
    add_leaf_bones=False, use_armature_deform_only=False, embed_textures=False,
    path_mode='COPY', bake_anim=False, mesh_smooth_type='FACE',
    axis_forward='-Z', axis_up='Y', apply_unit_scale=False, global_scale=GLOBAL_SCALE),
    os.path.getsize(FBX))
print("DONE", FBX)
