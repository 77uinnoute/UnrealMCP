"""PMX 模型 + VMD 动作 -> UE 可直导的「只带动画」FBX（Blender 直接执行）。

用途：把 MMD 的动作（VMD）搬进 UE 已有骨架。本脚本只产**动画**，不产网格，骨名与 UE 侧
`Ganyu_UE_Skeleton`（316 骨、ASCII、局部平移 cm、无内嵌 scale）逐名一致，所以 UE 侧可以直接
`import_assets` 到同一目录，让动画绑到既有骨架，不必走重定向。

做六件事：
  1) mmd_tools 导入 PMX（scale=0.08）
  2) 导入 VMD（**用原始文件，不要改名**：mmd_tools 匹配骨用的是 PMX 原始名 (`name_j`)，
     而它导入模型时会把 `右足` 显示成 `足.R`；把 VMD 改成显示名反而一条都对不上。
     实测：原始 VMD 的 `右足` 命中，改写成 `足.R` 的版本全部 "not found bone"）
  3) nla.bake(visual_keying) 把 IK 解算结果烤成 FK 通道（MMD 腿是 IK 驱动的：足IK 有键、
     show/ik 帧为 0 表示全程启用；不烤就进 UE 是一堆无效键）
  4) 骨名按 `blender_mmd_rename_ascii.py` 的规则改成 ASCII（含 _dummy_/_shadow_ 前缀与 .L/.R 后缀）
  5) 单位：沿模型侧同一套配方 —— 骨 head/tail ×100、**动作的 location 通道也 ×100**（漏了这一步
     动作距离会只剩 1/100），再 global_scale=0.01 导出
  6) 只导出骨架（object_types={'ARMATURE'}），删掉网格

验收：打印 帧范围 / 骨数 / 未改名骨（应为空或只剩琳奈专属）/ センター location 范围（×100 前后）
      / 导出文件大小。UE 侧再核对 AnimSequence 是否绑到 Ganyu_UE_Skeleton。
"""

import json
import os
import re
import unicodedata

import bpy

# ---------------- 参数 ----------------
SRC_PMX = r"C:\Users\aishengmin\Downloads\ganyu_by_原神_7381ccd84ee8763ce63b3ad638e1c49b\甘雨.pmx"
SRC_VMD = (r"C:\Users\aishengmin\Downloads\Catch Me If You Can_by_S师傅大火快炒_68e442aa878ada673d2959fb9fd721c8"
           r"\【琳奈用】Catch Me If You Can.vmd")
# 表情轨（morph）单独一个 VMD：它是 shape key 动画，**挂在网格上**，所以导出时必须带上网格，
# 否则 FBX 里没有 blendshape 通道、UE 也就没有 morph 曲线可导。
SRC_VMD_MORPH = (r"C:\Users\aishengmin\Downloads\Catch Me If You Can_by_S师傅大火快炒_68e442aa878ada673d2959fb9fd721c8"
                 r"\表情.vmd")
OUT = r"e:\ue_proj\asset_test_UE55 (2)\asset_test_UE55\Saved\MCPAssets\MMD_Ganyu"
NAME = "CatchMeIfYouCan_Ganyu"          # 产物 <NAME>.fbx；UE 侧资产名 = 文件名
ARMATURE_NAME = "root"                  # 骨架对象名 -> UE 骨架的根骨名（保持 ASCII）
IMPORT_SCALE = 0.08                     # mmd_tools：PMX -> Blender 米（与模型侧一致）
GLOBAL_SCALE = 0.01                     # x100 数据 + 0.01 => UE 得到真实 cm（见《UE资产导入》§五）
FPS = 30
# --------------------------------------

FBX = os.path.join(OUT, NAME + ".fbx")
REPORT = os.path.join(OUT, NAME + "_report.json")

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
    '親指': ('thumb', {'0': '01', '1': '02', '2': '03', '先': 'tip'}),
    '人指': ('index', {'1': '01', '2': '02', '3': '03', '先': 'tip'}),
    '中指': ('middle', {'1': '01', '2': '02', '3': '03', '先': 'tip'}),
    '薬指': ('ring', {'1': '01', '2': '02', '3': '03', '先': 'tip'}),
    '小指': ('pinky', {'1': '01', '2': '02', '3': '03', '先': 'tip'}),
}


def norm(s):
    return unicodedata.normalize('NFKC', s)


def map_one(raw):
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
    elif re.match(r'^Weapon[LR]$', key):
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


# ---- 1) 导入模型 ----
bpy.ops.wm.read_homefile(use_empty=True)
bpy.ops.preferences.addon_enable(module="bl_ext.user_default.mmd_tools")
bpy.ops.mmd_tools.import_model(filepath=SRC_PMX, scale=IMPORT_SCALE)
arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
mesh = next(o for o in bpy.data.objects if o.type == 'MESH' and len(o.data.vertices) > 1000)
arm.name = ARMATURE_NAME
print("MODEL bones=%d mesh_verts=%d actions_before=%d"
      % (len(arm.data.bones), len(mesh.data.vertices), len(bpy.data.actions)))

# ---- 2) 表情 VMD（shape key 动画）—— 必须先导 ----
# 顺序敏感：实测「先导动作 VMD、再导表情」会让 shape key 曲线整批丢失（管线那轮打印 animated=0），
# 而单独导（隔离探针）时 mmd_tools 会把 表情_facial 正常挂到 mesh.shape_keys.animation_data（28 条）。
def sk_info():
    ad = mesh.data.shape_keys.animation_data if mesh.data.shape_keys else None
    if not ad or not ad.action:
        return "<none>", 0
    n = len([fc for fc in ad.action.fcurves if 'key_blocks[' in fc.data_path])
    return ad.action.name, n


rm = bpy.ops.mmd_tools.import_vmd(filepath=SRC_VMD_MORPH, scale=IMPORT_SCALE, margin=0,
                                  create_new_action=False, update_scene_settings=False)
n_sk = len(mesh.data.shape_keys.key_blocks) if mesh.data.shape_keys else 0
print("MORPH_IMPORT %s shape_keys=%d action=%s fcurves=%d" % ((rm, n_sk) + sk_info()))

# ---- 3) 动作 VMD（骨轨）----
scene = bpy.context.scene
scene.render.fps = FPS
bpy.ops.object.select_all(action='DESELECT')
arm.select_set(True)
bpy.context.view_layer.objects.active = arm
r = bpy.ops.mmd_tools.import_vmd(filepath=SRC_VMD, scale=IMPORT_SCALE, margin=0,
                                 create_new_action=True, update_scene_settings=True)
print("VMD_IMPORT %s actions=%d range=%d..%d"
      % (r, len(bpy.data.actions), scene.frame_start, scene.frame_end))

act = arm.animation_data.action if arm.animation_data else None
print("ACTION %s fcurves=%d" % (act.name if act else None, len(act.fcurves) if act else 0))
print("AFTER_MOTION shape_key_action=%s fcurves=%d" % sk_info())

# 动作 VMD 的导入会**清掉 mesh shape keys 上的 action 挂载**（datablock 仍在 bpy.data.actions 里，
# 实测 actions=2、但 shape_key_action=<none>）；重新挂回去，否则导出的 FBX 里没有 blendshape 动画。
face_act = None
for a in bpy.data.actions:
    if any('key_blocks[' in fc.data_path for fc in a.fcurves):
        face_act = a
        break
if face_act and mesh.data.shape_keys:
    if not mesh.data.shape_keys.animation_data:
        mesh.data.shape_keys.animation_data_create()
    mesh.data.shape_keys.animation_data.action = face_act
print("FACE_RELINK action=%s -> shape_key_action=%s fcurves=%d"
      % ((face_act.name if face_act else None,) + sk_info()))

# ---- 3) 把 IK 解算结果烤成 FK（visual keying） ----
bpy.ops.object.mode_set(mode='POSE')
bpy.ops.pose.select_all(action='SELECT')
bpy.ops.nla.bake(frame_start=scene.frame_start, frame_end=scene.frame_end, step=1,
                 only_selected=False, visual_keying=True, clear_constraints=True,
                 clear_parents=False, use_current_action=True, bake_types={'POSE'})
bpy.ops.object.mode_set(mode='OBJECT')
act = arm.animation_data.action
print("BAKED action=%s fcurves=%d" % (act.name, len(act.fcurves)))

# ---- 4) 骨名 -> ASCII ----
unmapped = []
rename_map = {}
for b in arm.data.bones:
    new = map_one(b.name)
    if new is None:
        unmapped.append(b.name)
        continue
    rename_map[b.name] = new
# 先缓存父名，再改（改父名会断掉按名查找）
parents = {b.name: (b.parent.name if b.parent else None) for b in arm.data.bones}
for old, new in rename_map.items():
    arm.data.bones[old].name = new
print("RENAMED %d / 未覆盖=%d %s" % (len(rename_map), len(unmapped), unmapped))

# ---- 5) 单位：x100 进骨与**动作通道** ----
bpy.ops.object.mode_set(mode='EDIT')
n_eb = 0
for eb in arm.data.edit_bones:
    eb.head = eb.head * 100.0
    eb.tail = eb.tail * 100.0
    n_eb += 1
bpy.ops.object.mode_set(mode='OBJECT')
n_loc = 0
for fc in act.fcurves:
    if fc.data_path.endswith('.location'):
        for kp in fc.keyframe_points:
            kp.co.y *= 100.0
            kp.handle_left.y *= 100.0
            kp.handle_right.y *= 100.0
        fc.update()
        n_loc += 1
print("SCALED edit_bones=%d location_fcurves=%d" % (n_eb, n_loc))

# 网格也 x100（与骨一致）。有 shape key 时权威数据在 key_blocks 里，逐块缩；
# 不能两边都缩 —— 无 shape key 时 vertices 就是那份数据，缩两次会变大 100 倍。
if mesh.data.shape_keys:
    n_sk = 0
    for kb_ in mesh.data.shape_keys.key_blocks:
        for d in kb_.data:
            d.co *= 100.0
        n_sk += 1
    print("SCALED_MESH shape_key_blocks=%d" % n_sk)
else:
    for v in mesh.data.vertices:
        v.co *= 100.0
    print("SCALED_MESH verts=%d (no shape keys)" % len(mesh.data.vertices))


def loc_range(fc_prefix):
    out = {}
    for fc in act.fcurves:
        if fc.data_path.endswith('.location'):
            vals = [kp.co.y for kp in fc.keyframe_points]
            if vals:
                out[fc.data_path] = (round(min(vals), 3), round(max(vals), 3))
    return out


print("LOC_RANGE sample %s" % list(loc_range('').items())[:3])

# ---- 6) 导出骨架 + 网格（网格必须留：shape key 动画挂在它上面，UE 侧 morph 曲线靠它解析）----
bpy.ops.object.select_all(action='DESELECT')
arm.select_set(True)
mesh.select_set(True)
bpy.context.view_layer.objects.active = arm
res = bpy.ops.export_scene.fbx(
    filepath=FBX, object_types={'ARMATURE', 'MESH'}, use_selection=True,
    # bake_anim 必须开：关着时导出器只写一个空的 AnimStack，**一条 bone 曲线都不写**
    # （实测：FBX 里 AnimStack=1、AnimCurve=0），打开后按 30fps 逐帧采样已烤好的 FK。
    bake_anim=True, bake_anim_use_all_bones=True, bake_anim_force_startend_keying=True,
    bake_anim_simplify_factor=0.0, bake_anim_step=1.0, bake_anim_use_nla_strips=False,
    add_leaf_bones=False, use_mesh_modifiers=False,
    axis_forward='-Z', axis_up='Y', apply_unit_scale=False, global_scale=GLOBAL_SCALE,
    path_mode='AUTO', embed_textures=False)
print("EXPORT %s exists=%s size=%s"
      % (res, os.path.exists(FBX), os.path.getsize(FBX) if os.path.exists(FBX) else -1))

# ---- 自查：FBX 里到底有没有动画曲线 / blendshape 通道（不看这个就会把空导出当成功） ----
probe = open(FBX, "rb").read()
counts = {k: probe.count(k.encode()) for k in
          ("AnimCurveNode", "AnimationCurve", "AnimStack", "BlendShapeChannel", "Shape")}
print("FBX_ANIM %s" % counts)
if counts["AnimCurveNode"] == 0 and counts["AnimationCurve"] == 0:
    print("FBX_ANIM_WARNING: no animation curves in the export")
if counts["BlendShapeChannel"] == 0:
    print("FBX_ANIM_WARNING: no blendshape channels in the export (morph curves will be missing)")

json.dump({"fbx": FBX, "bones": len(arm.data.bones), "renamed": len(rename_map),
           "unmapped": unmapped, "fcurves": len(act.fcurves),
           "frames": [scene.frame_start, scene.frame_end], "fps": FPS},
          open(REPORT, "w"), ensure_ascii=True, indent=1)
print("REPORT %s" % REPORT)
