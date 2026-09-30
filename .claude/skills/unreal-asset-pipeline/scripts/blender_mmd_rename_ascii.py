"""MMD(PMX) -> UE FBX，并把骨名标准化成 ASCII（Blender 直接执行，不走 MCP bridge）。

**为什么**：非 ASCII 骨名在 UE 里能跑（316 骨保留 314 个原名，见 SKILL §五），但会带来
"按名字识别"的一整类摩擦：`apply_auto_generated_retarget_definition()` 直接失败（0 条链）、
脚本源码里不能写骨名（MCP 桥 4KB 分块解码会炸）、全角/半角与 FName 大小写不敏感的坑。
标准化后官方人形模板可用（实测自动生成 20 条链），脚本也能直接写骨名。
**代价**：与 MMD/VMD 动作库脱钩（VMD 按名匹配）。只有确定不用 MMD 动作库时才做。

做五件事：
  1) D 变形骨权重并到 FK 骨（UE 无付与机制）
  2) 骨名 -> ASCII；同步 vertex group（Blender 改 bone.name 会自动改名对应顶点组，实测确认）
     + mmd_tools 里按字符串存的骨引用（additional_transform_bone / ik_target 等）
  3) dump「旧名→新名」映射表 + 新骨表（供 UE 侧重建 rig、以及改名前后逐骨等价性比对）
  4) x100 烤进顶点/骨骼数据 + global_scale=0.01 导出（骨架局部平移 cm、无内嵌 scale）
  5) 旧 FBX 备份（等价性基准；真正的"改名前"原件请提前另存）

命名约定（核心 + 模式规则见 CORE / IK_CORE / PATTERNS / FINGERS）：
  root(FBX armature 节点名) / ctrl_origin / master / center(_02) / groove(_02) / pelvis / lower_body
  spine_01..03 / neck_01 / head / eyes / eye_l|r / tooth_upper|lower
  clavicle_(p|c)_l|r / clavicle_l|r / upperarm(_twist_*) / lowerarm(_twist_*) / hand(_dummy)_l|r
  thumb/index/middle/ring/pinky_01..03|tip / thigh|calf|foot|ball(_deform)_l|r
  hair_* / ahoge_* / skirt_front_* / skirt_back_* / tassel_01|02_* / ik_foot* / ik_toe_l|r / breast_*
**换模型时**：把新模型的骨名补进 CORE/PATTERNS（脚本会 `assert` 未覆盖项并全部打印，不会静默漏改）。

验收（改完在 UE 侧跑）：
  dump_skeleton_baseline.py（改名前）→ 导入 → verify_bone_rename_equiv.py
  期望：非 ASCII 骨名 0 根；逐骨 LOCAL 平移差 < 1e-4 cm、四元数 |dot| > 0.9999。
"""
import json
import os
import re
import shutil
import unicodedata

import bpy

# ---------------- 参数 ----------------
SRC = r"C:\path\to\model.pmx"
OUT = r"e:\ue_proj\asset_test_UE55 (2)\asset_test_UE55\Saved\MCPAssets\MMD_<Model>"
NAME = "Model_UE"                      # 产物 <NAME>.fbx / <NAME>.fbm/，UE 资产名 = 文件名
ARMATURE_NAME = "root"                 # 骨架对象名 -> UE 骨架的根骨名（保持 ASCII）
IMPORT_SCALE = 0.08                    # mmd_tools 默认：PMX -> Blender 米
GLOBAL_SCALE = 0.01                    # x100 数据 + 0.01 => UE 得到真实 cm（见《UE资产导入》§五）
# --------------------------------------

FBX = os.path.join(OUT, NAME + ".fbx")
FBM = os.path.join(OUT, NAME + ".fbm")
MAP_JSON = os.path.join(OUT, "rename_map.json")
NEWBONES_JSON = os.path.join(OUT, "ue_bones_ascii.json")
NEWBONES_TSV = os.path.join(OUT, "ue_bones_ascii.tsv")

CORE = {
    '操作中心': 'ctrl_origin', '全ての親': 'master', 'センター': 'center', 'センター2': 'center_02',
    'グルーブ': 'groove', 'グルーブ2': 'groove_02', '腰': 'pelvis', '下半身': 'lower_body',
    '足': 'thigh', 'ひざ': 'calf', '足首': 'foot', 'つま先': 'ball',
    '足D': 'thigh_deform', 'ひざD': 'calf_deform', '足首D': 'foot_deform', '足先EX': 'ball_deform',
    '上半身': 'spine_01', '上半身3': 'spine_02', '上半身2': 'spine_03', '首': 'neck_01', '頭': 'head',
    '下齿': 'tooth_lower', '上齿': 'tooth_upper', '両目': 'eyes', '目': 'eye',
    '肩P': 'clavicle_p', '肩C': 'clavicle_c', '肩': 'clavicle',
    '腕捩': 'upperarm_twist', '腕': 'upperarm', 'ひじ': 'lowerarm',
    '手捩': 'lowerarm_twist', '手首': 'hand', 'ダミー': 'hand_dummy',
    'Weapon': 'weapon',
    'おっぱい調整': 'breast_ctrl', '胸上2': 'breast_upper_02', '胸上': 'breast_upper',
    '胸下先': 'breast_lower_tip', '胸下': 'breast_lower', '胸先': 'breast_tip',
}
IK_CORE = {'足IK親': 'ik_foot_parent', '足IK': 'ik_foot', 'つま先IK': 'ik_toe'}
PATTERNS = [
    (r'^发_(\d+)_1$', 'hair_%02d'),
    (r'^アホ毛_(\d+)_1$', 'ahoge_%02d'),
    (r'^前摆_(\d+)_(\d+)$', 'skirt_front_%02d_%02d'),
    (r'^后摆_(\d+)_(\d+)$', 'skirt_back_%02d_%02d'),
    (r'^流苏1_(\d+)_1$', 'tassel_01_%02d'),
    (r'^流苏2_(\d+)_1$', 'tassel_02_%02d'),
    (r'^腕捩(\d)$', 'upperarm_twist_%02d'),
    (r'^手捩(\d)$', 'lowerarm_twist_%02d'),
]
FINGERS = {
    '親指': ('thumb', {'0': '01', '1': '02', '2': '03', '先': 'tip'}),   # 拇指多一节
    '人指': ('index', {'1': '01', '2': '02', '3': '03', '先': 'tip'}),
    '中指': ('middle', {'1': '01', '2': '02', '3': '03', '先': 'tip'}),
    '薬指': ('ring', {'1': '01', '2': '02', '3': '03', '先': 'tip'}),
    '小指': ('pinky', {'1': '01', '2': '02', '3': '03', '先': 'tip'}),
}


def norm(s):
    """匹配用归一化：全角 -> 半角（ＩＫ -> IK、親指０ -> 親指0）。输出名不经过它。"""
    return unicodedata.normalize('NFKC', s)


def map_one(raw):
    """Blender 原名（含 .L/.R 与 _dummy_/_shadow_ 前缀）-> ASCII 新名。None = 未覆盖。"""
    key = norm(raw)
    prefix = ''
    for p in ('_dummy_', '_shadow_'):
        if key.startswith(p):
            prefix, key = p, key[len(p):]
            break
    side = ''
    m = re.match(r'^(.*)\.([LR])$', key)
    if m:
        key, side = m.group(1), '_' + m.group(2).lower()
    elif re.match(r'^Weapon[LR]$', key):           # MMD 的 WeaponL/R 没有点
        side, key = '_' + key[-1].lower(), 'Weapon'
    if key in CORE:
        return prefix + CORE[key] + side
    if key in IK_CORE:
        return prefix + IK_CORE[key] + side
    for lead, (eng, nums) in FINGERS.items():
        if key.startswith(lead) and key[len(lead):] in nums:
            return prefix + eng + '_' + nums[key[len(lead):]] + side
    for pat, tmpl in PATTERNS:
        mt = re.match(pat, key)
        if mt:
            return prefix + (tmpl % tuple(int(g) for g in mt.groups())) + side
    return None


bpy.ops.wm.read_homefile(use_empty=True)
bpy.ops.preferences.addon_enable(module="bl_ext.user_default.mmd_tools")
bpy.ops.mmd_tools.import_model(filepath=SRC, scale=IMPORT_SCALE)

arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
mesh = next(o for o in bpy.data.objects if o.type == 'MESH' and len(o.data.vertices) > 1000)
arm.name = ARMATURE_NAME
print('ACTIONS', len(bpy.data.actions), '(必须为 0：Blender Action 的 f-curve 路径是字符串，改名会失配)')

# ---- 1) D 骨权重 -> FK ----
DMAP = {}
for s in ('L', 'R'):
    DMAP['足D.' + s] = '足.' + s
    DMAP['ひざD.' + s] = 'ひざ.' + s
    DMAP['足首D.' + s] = '足首.' + s
    DMAP['足先EX.' + s] = 'つま先.' + s
vg = mesh.vertex_groups
name2g = {g.name: g for g in vg}
new_w = {}
for i, v in enumerate(mesh.data.vertices):
    d = {}
    for e in v.groups:
        src_name = next((g.name for g in vg if g.index == e.group), None)
        tgt = DMAP.get(src_name, src_name)
        d[tgt] = d.get(tgt, 0.0) + e.weight
    new_w[i] = d
for src in DMAP:
    name2g[src].remove(list(range(len(mesh.data.vertices))))
for i, d in new_w.items():
    for n, w in d.items():
        if name2g.get(n) and w > 0.0:
            name2g[n].add([i], w, 'ADD')


def wsum(name):
    g = name2g.get(name)
    if g is None:
        return None
    idx = g.index
    return round(sum(e.weight for v in mesh.data.vertices for e in v.groups if e.group == idx), 1)


print('FK_WEIGHT_SUMS', {DMAP[k]: wsum(DMAP[k]) for k in DMAP})   # 权重守恒：改名/合并前后应相等

# ---- 2) 映射 + 校验（未覆盖 / FName 大小写冲突都要 fail fast）----
old_names = [b.name for b in arm.data.bones]
mapping, unmapped = {}, []
for n in old_names:
    nn = map_one(n)
    (mapping.__setitem__(n, nn) if nn else unmapped.append(n))
low, dups = {}, []
for old, new in mapping.items():
    if new.lower() in low:
        dups.append((new, low[new.lower()], old))
    low[new.lower()] = old
print('BONES', len(old_names), 'MAPPED', len(mapping), 'UNMAPPED', unmapped, 'DUPES', dups)
assert not unmapped, '有骨没覆盖到映射规则：补 CORE/PATTERNS'
assert not dups, '映射后大小写不敏感重名（UE FName 会冲突）'

# ---- 3) 改名（顶点组由 Blender 随骨名自动改名）+ mmd 字符串引用 ----
REF_KEYS = ('additional_transform_bone', 'ik_target', 'ik_target_bone', 'target_bone', 'bone')
for b in arm.data.bones:
    if b.name in mapping:
        b.name = mapping[b.name]
fixed = 0
for pb in arm.pose.bones:
    d = pb.get('mmd_bone')
    if isinstance(d, dict):
        for k in list(d.keys()):
            if k in REF_KEYS and isinstance(d[k], str) and d[k] in mapping:
                d[k] = mapping[d[k]]
                fixed += 1
print('FIXED_MMD_REFS', fixed)
bone_set, group_set = {b.name for b in arm.data.bones}, {g.name for g in mesh.vertex_groups}
print('BONES_WITHOUT_GROUP', len(bone_set - group_set), sorted(bone_set - group_set)[:6])
print('GROUPS_WITHOUT_BONE', sorted(group_set - bone_set)[:6])   # 只剩 mmd_edge_scale/mmd_vertex_order 就正常

# ---- 4) dump 映射表 + 新骨表 ----
with open(MAP_JSON, 'w', encoding='utf-8') as f:
    json.dump(mapping, f, ensure_ascii=True, indent=1)
rows = [{'i': i, 'name': b.name, 'parent': b.parent.name if b.parent else '',
         'p': [round(v, 4) for v in b.head_local]} for i, b in enumerate(arm.data.bones)]
with open(NEWBONES_JSON, 'w', encoding='utf-8') as f:
    json.dump(rows, f, ensure_ascii=True, indent=1)
with open(NEWBONES_TSV, 'w', encoding='utf-8') as f:
    for r in rows:
        f.write("%d\t%s\t%s\t%s\n" % (r['i'], r['name'], r['parent'], r['p']))
print('DUMPED', MAP_JSON, NEWBONES_JSON, len(rows))

# ---- 5) x100 烤数据 + 贴图聚拢 + 备份 + 导出 ----
# 带 shape key（morph）时，**key_blocks 与 mesh.vertices 都要缩**：只缩 mesh.vertices 会让导出的
# morph delta = shape(米) − base(厘米) ≈ 整个身高 ⇒ UE 侧任何 morph 权重 > 0 都把网格撕开。
for _kb in (mesh.data.shape_keys.key_blocks if mesh.data.shape_keys else []):
    for _d in _kb.data:
        _d.co = _d.co * 100.0
for v in mesh.data.vertices:
    v.co = v.co * 100.0
bpy.context.view_layer.objects.active = arm
bpy.ops.object.mode_set(mode='EDIT')
for eb in arm.data.edit_bones:
    eb.head = eb.head * 100.0
    eb.tail = eb.tail * 100.0
bpy.ops.object.mode_set(mode='OBJECT')
for o in (arm, mesh):
    o.scale = (1.0, 1.0, 1.0)
mesh.matrix_parent_inverse.identity()
print('BAKED_CM', [round(v, 2) for v in mesh.dimensions])

os.makedirs(FBM, exist_ok=True)
for i in bpy.data.images:
    src = bpy.path.abspath(i.filepath) if i.filepath else ""
    if src and os.path.exists(src):
        dst = os.path.join(FBM, os.path.basename(src))
        if os.path.abspath(src) != os.path.abspath(dst):
            shutil.copyfile(src, dst)
        i.filepath = dst

bak = os.path.join(OUT, NAME + "_preprename.fbx")
if os.path.exists(FBX) and not os.path.exists(bak):
    shutil.copyfile(FBX, bak)
    print('BACKUP', bak)

bpy.ops.object.select_all(action='DESELECT')
arm.select_set(True)
mesh.select_set(True)
bpy.context.view_layer.objects.active = arm
print('EXPORT', bpy.ops.export_scene.fbx(
    filepath=FBX, use_selection=True, object_types={'ARMATURE', 'MESH'},
    add_leaf_bones=False, use_armature_deform_only=False, embed_textures=False,
    path_mode='COPY', bake_anim=False, mesh_smooth_type='FACE',
    axis_forward='-Z', axis_up='Y', apply_unit_scale=False, global_scale=GLOBAL_SCALE),
    os.path.getsize(FBX))
print('DONE', FBX)
