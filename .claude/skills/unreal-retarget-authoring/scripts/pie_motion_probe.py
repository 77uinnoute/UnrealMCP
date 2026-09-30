"""PIE 采样探针：起/停 PIE + 按 actor label 采骨世界坐标（"到底动没动"）。

用法（两次工具调用）：
  1) `execute_python_file(本脚本, ACTION='start')` 起 PIE
  2) 再跑两次 `ACTION='sample'`：两次读到的值**不同**才算在动；**逐字节相同 = 冻住**（多为 rest）
  3) 收工 `ACTION='stop'`

判读：
  * 目标全部等于 rest（如 左踝 z = actor_z + 9）且两次相同 -> 节点没喂出东西：
    先确认源在动（源帧两次不同），再按 重定向 SKILL §三 的顺序查运行时外壳（game world / 组件父链 / anim 类 / 重建 ABP）。
  * 源也不动 -> 源网格动画没播（ANIMATION_SINGLE_NODE + animation_data.anim_to_play）。
"""
import unreal

# ---------------- 参数 ----------------
ACTION = 'sample'          # 'start' | 'stop' | 'sample'
SAMPLES = [                # (actor label, [骨名...])
    ('RT_Target_Ganyu', ['\u8170', '\u8db3\u9996_L', '\u3072\u3056_L', '\u982d']),
    ('RT_Target_MMD', ['pelvis', 'foot_l', 'head']),
    ('RT_Source_Manny', ['pelvis', 'foot_l', 'head']),
]
# --------------------------------------

if ACTION == 'start':
    print('BEGIN_PLAY', unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).editor_request_begin_play())
elif ACTION == 'stop':
    # 注意：LevelEditorSubsystem 没有 editor_end_play
    print('END_PLAY', unreal.EditorLevelLibrary.editor_end_play())
else:
    UES = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    gw = UES.get_game_world()
    print('game_world', gw)
    actors = unreal.GameplayStatics.get_all_actors_of_class(gw, unreal.SkeletalMeshActor)
    for label, bones in SAMPLES:
        a = next((x for x in actors if x.get_actor_label() == label), None)
        if a is None:
            print('MISS', label)
            continue
        comp = a.get_components_by_class(unreal.SkeletalMeshComponent)[0]
        ai = comp.get_anim_instance()
        row = []
        for b in bones:
            t = comp.get_socket_transform(unreal.Name(b)).translation
            row.append('%s=(%.1f,%.1f,%.1f)' % (b, t.x, t.y, t.z))
        print('POSE', label, '| anim_inst=', ai.get_class().get_name() if ai else None, '|', ' '.join(row))
print('PIE_PROBE_DONE')
