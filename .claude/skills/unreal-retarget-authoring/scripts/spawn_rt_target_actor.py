"""摆运行时重定向 targets：spawn SkeletalMeshActor -> 设 mesh/anim_class -> 挂到源 mesh 组件下 -> 存关卡。

要点（重定向 SKILL §三 硬约束⑤）：节点靠 `bUseAttachedParent` 沿**组件父链**找源，
所以目标 actor 的根组件必须挂到**源 actor 的 SkeletalMeshComponent** 上（KEEP_WORLD 保偏移）。
`spawn_blueprint_actor` 同名会 Fatal 崩编辑器 —— 本脚本用 `spawn_actor_from_class`（SkeletalMeshActor），
并显式检查同名已存在就跳过 spawn（只改属性）。
"""
import unreal

# ---------------- 参数 ----------------
SOURCE_LABEL = 'RT_Source_Manny'
TARGET_LABEL = 'RT_Target_Ganyu'
TARGET_MESH = '/Game/MCP/Ganyu/Ganyu_UE'
TARGET_ABP_CLASS = '/Game/Blueprints/ABP_GanyuRT2.ABP_GanyuRT2_C'
OFFSET = (0.0, 400.0, 0.0)        # 相对源 actor 的偏移（避免和别的 target 重叠）
# --------------------------------------

EA = unreal.EditorAssetLibrary
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
w = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
actors = unreal.GameplayStatics.get_all_actors_of_class(w, unreal.SkeletalMeshActor)

src = next((a for a in actors if a.get_actor_label() == SOURCE_LABEL), None)
if src is None:
    raise SystemExit('找不到源 actor: ' + SOURCE_LABEL)
c1 = src.get_components_by_class(unreal.SkeletalMeshComponent)[0]
print('src', src.get_actor_label(), c1.get_editor_property('skeletal_mesh_asset').get_name())

tgt = next((a for a in actors if a.get_actor_label() == TARGET_LABEL), None)
if tgt is None:
    loc = src.get_actor_location() + unreal.Vector(*OFFSET)
    tgt = eas.spawn_actor_from_class(unreal.SkeletalMeshActor, loc, src.get_actor_rotation())
    tgt.set_actor_label(TARGET_LABEL)
else:
    print('target 已存在，只改属性')

c2 = tgt.get_components_by_class(unreal.SkeletalMeshComponent)[0]
c2.set_editor_property('skeletal_mesh_asset', EA.load_asset(TARGET_MESH))
c2.set_editor_property('animation_mode', unreal.AnimationMode.ANIMATION_BLUEPRINT)
c2.set_editor_property('anim_class', unreal.load_class(None, TARGET_ABP_CLASS))
print('target', tgt.get_actor_label(), c2.get_editor_property('skeletal_mesh_asset').get_name(),
      c2.get_editor_property('anim_class').get_name())

tgt.attach_to_component(c1, 'None', unreal.AttachmentRule.KEEP_WORLD, unreal.AttachmentRule.KEEP_WORLD,
                        unreal.AttachmentRule.KEEP_WORLD, False)
print('attach_parent', c2.get_attach_parent().get_name() if c2.get_attach_parent() else None,
      '| rel_loc', c2.get_editor_property('relative_location'))
print('save_level', unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level())
print('SPAWN_DONE（编辑器世界里目标恒为 rest，必须 PIE 才动）')
