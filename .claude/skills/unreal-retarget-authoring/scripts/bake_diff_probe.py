"""烘焙差分探针：批量烘焙一条源动画 -> 逐帧量世界坐标（判断 rig/骨架到底行不行）。

为什么先做它（重定向 SKILL §三"差分法"）：烘焙是同步、可重复、没有 tick 顺序干扰的探针；
它和运行时用**同一个求解器**。烘焙塌 = 资产/骨架问题（先查 SKILL §二.3b 的内嵌 scale），
烘焙对、运行时不动的 = 运行时外壳问题（ABP 类/组件链）。

健康指标（干净 cm 骨架 + Manny 走路）：
  腰/骨盆 z 在身高量级；左右踝**交替**；膝 ~身高/3；头 ~总高的 0.85；根位移随源线性推进。
  塌陷指纹：所有骨世界坐标互差 < 1–2 cm，或各帧几乎不变。

注意：`duplicate_and_retarget` 的 asset 列表必须是 **AssetData 数组**；
产物落在 `/Game`（不是源目录），同名会加后缀 1/2/3…（重烤前先删旧的或换 SUFFIX）。
"""
import json

import unreal

# ---------------- 参数 ----------------
SRC_ANIM = '/Game/Characters/Mannequins/Animations/Manny/MM_Walk_Fwd.MM_Walk_Fwd'
SRC_MESH = '/Game/Characters/Mannequins/Meshes/SKM_Manny'
TGT_MESH = '/Game/MCP/Ganyu/Ganyu_UE'
RTG = '/Game/MCP/Ganyu/RTG_Ganyu'
SUFFIX = '_Ganyu'
BONES_JSON = r"e:\ue_proj\asset_test_UE55 (2)\asset_test_UE55\Saved\MCPAssets\MMD_Ganyu\ue_bones.json"
BONE_IDX = [7, 11, 20, 10, 213, 164]      # 腰 / 左踝 / 右踝 / 左膝 / 左腕 / 头
SRC_BONES = ['pelvis', 'foot_l', 'foot_r', 'calf_l', 'hand_l', 'head']
# --------------------------------------

EA = unreal.EditorAssetLibrary
AR = unreal.AssetRegistryHelpers.get_asset_registry()
N = {r['i']: r['name'] for r in json.load(open(BONES_JSON, encoding='utf-8'))}

ad = AR.get_asset_by_object_path(SRC_ANIM)
res = unreal.IKRetargetBatchOperation.duplicate_and_retarget(
    [ad], EA.load_asset(SRC_MESH), EA.load_asset(TGT_MESH), EA.load_asset(RTG),
    '', '', '', SUFFIX, False)
print('BAKE', [str(a.package_name) for a in res])
anim = EA.load_asset(str(res[0].package_name) + '.' + str(res[0].asset_name))
n = unreal.AnimationLibrary.get_num_frames(anim)
print('frames', n)

opts = unreal.AnimPoseEvaluationOptions()
tgt_names = [N[i] for i in BONE_IDX]
for fi in (0, n // 4, n // 2, (3 * n) // 4, n - 1):
    pose = unreal.AnimPoseExtensions.get_anim_pose_at_frame(anim, fi, opts)
    row = []
    for b in tgt_names:
        t = unreal.AnimPoseExtensions.get_bone_pose(pose, b, unreal.AnimPoseSpaces.WORLD).translation
        row.append('%s=(%.2f,%.2f,%.2f)' % (b, t.x, t.y, t.z))
    print('TGT_F%d' % fi, '|', ' '.join(row))

src = EA.load_asset(SRC_ANIM)
for fi in (0, n // 2, n - 1):
    pose = unreal.AnimPoseExtensions.get_anim_pose_at_frame(src, fi, opts)
    row = []
    for b in SRC_BONES:
        t = unreal.AnimPoseExtensions.get_bone_pose(pose, b, unreal.AnimPoseSpaces.WORLD).translation
        row.append('%s=(%.2f,%.2f,%.2f)' % (b, t.x, t.y, t.z))
    print('SRC_F%d' % fi, '|', ' '.join(row))
print('BAKE_DIFF_DONE')
