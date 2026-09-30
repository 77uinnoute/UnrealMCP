"""IK Retargeter 派生判据探针（只读）：链数 / 两侧配对 / root settings 非默认值 / 两侧 pose 名。

做什么：从**实测确认过**的调用形态与成员名里推导一组判据，并打一行 `PROBE_RESULT: PASS|CHECK` 汇总。

跑在哪：编辑器 python（`execute_python_file`），只读、不写盘。怎么用：改下面 `RTG` 常量 -> 跑。

判据（只输出"能观测到什么"，**不**假装等价引擎内部的 `initialized`——那是启发式）：
  PASS  = 资产与控制器都拿到 + 链数 > 0 + 两侧当前 retarget pose 名都读到 + 每条链两侧链名配得上
          + root settings 无"非默认值"提示。
  CHECK = 任一条不成立，逐条列出原因。

实测确认的成员名（`dump_retargeter.py` 跑出来的，先前的原稿名字不存在）：
  - 每条 chain setting 的数据成员只有 3 个：`source_chain` / `target_chain`（名字）+ `settings`
    （`TargetChainSettings`，内含 `fk{enable_fk,rotation_mode,rotation_alpha,translation_mode,
    translation_alpha,pole_vector_*}` / `ik{enable_ik,blend_to_source,static_*,scale_vertical,
    extension}` / `speed_planting{...}`）。原稿的 `chain_name` / `start_bone` / `goal_name` /
    `fk_rotation_mode` / `fk_translation_mode` **都不存在**。
  - 本脚本只按这 3 个成员做**配对**判据；嵌套 `fk`/`ik` 的逐链语义不在本探针范围内（去看 dump）。

拿不到的信息（如实标注，不编）：
  - `initialized` 一类"引擎内部是否真的初始化成功"的判据**不可得**（引擎未暴露 `IKRetargetProcessor`），
    本脚本只给可观测证据。

注意（PIE 纪律）：PIE 运行中 `load_asset` / `get_controller` 会**静默返回 None**；`None` MUST
NOT 被解释成"资产不存在"——先 `stop_pie`（轮询 `pie_running` 确认已退出）再跑本脚本。
"""
import unreal

# ---------------- 参数 ----------------
RTG = '/Game/MCP/Ganyu/RTG_Ganyu'
EXPECTED_CHAINS = 20        # 实测该资产一次读到 20 条；不等只作提示，不单独判 CHECK
SCALE_EPS = 1e-4            # root scale 的默认值是 1.0（干净骨架）；MMD 内嵌 scale 的 rig 会是 0.01
# --------------------------------------


def fmt(value):
    try:
        return '%.4g' % float(value)
    except (TypeError, ValueError):
        return str(value)


reasons = []
UES = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
in_pie = UES.get_game_world() is not None
print('PIE_RUNNING', in_pie)
if in_pie:
    reasons.append('pie_running: asset getters can return None, stop_pie first')

asset = unreal.EditorAssetLibrary.load_asset(RTG)
print('ASSET', RTG, '->', asset)

ctrl = None
if asset is None:
    reasons.append('asset_is_none: PIE running, or the path does not exist (do not read as '
                   '"asset missing" before ruling PIE out)')
else:
    ctrl = unreal.IKRetargeterController.get_controller(asset)
    print('CTRL', ctrl)
    if ctrl is None:
        reasons.append('controller_is_none')

pair_ok = 'n/a'
if ctrl is not None:
    pose_names = {}
    for side_name in ('SOURCE', 'TARGET'):
        side = getattr(unreal.RetargetSourceOrTarget, side_name)
        # 这些 getter MUST 显式传 source_or_target
        pose_names[side_name] = str(ctrl.get_current_retarget_pose_name(side))
        print('SIDE %s ik_rig=%s preview_mesh=%s current_pose_name=%s poses=%s'
              % (side_name, ctrl.get_ik_rig(side), ctrl.get_preview_mesh(side),
                 pose_names[side_name], [str(p) for p in ctrl.get_retarget_poses(side)]))
    print('POSE_NAME SOURCE=%s TARGET=%s' % (pose_names.get('SOURCE'), pose_names.get('TARGET')))
    if not all(pose_names.values()) or 'None' in pose_names.values():
        reasons.append('current_pose_name unreadable on one side')

    rs = ctrl.get_root_settings()
    scale_h = getattr(rs, 'scale_horizontal', None)
    scale_v = getattr(rs, 'scale_vertical', None)
    blend = getattr(rs, 'blend_to_source', None)
    rot_a = getattr(rs, 'rotation_alpha', None)
    tra_a = getattr(rs, 'translation_alpha', None)
    print('ROOT scale_horizontal=%s scale_vertical=%s blend_to_source=%s rotation_alpha=%s '
          'translation_alpha=%s' % (fmt(scale_h), fmt(scale_v), fmt(blend), fmt(rot_a), fmt(tra_a)))
    for name, value, default in (('scale_horizontal', scale_h, 1.0),
                                 ('scale_vertical', scale_v, 1.0),
                                 ('blend_to_source', blend, 0.0),
                                 ('rotation_alpha', rot_a, 1.0),
                                 ('translation_alpha', tra_a, 1.0)):
        if value is None:
            reasons.append('root.%s unreadable' % name)
            continue
        if abs(float(value) - default) > SCALE_EPS:
            reasons.append('root.%s=%s (non-default, expected %s)' % (name, fmt(value), fmt(default)))
            print('ROOT_NONDEFAULT %s=%s (expected %s)' % (name, fmt(value), fmt(default)))

    chains = ctrl.get_all_chain_settings()
    print('CHAIN_COUNT %d' % len(chains))
    if len(chains) == 0:
        reasons.append('chain_settings_count_is_zero')
    elif len(chains) != EXPECTED_CHAINS:
        print('CHAIN_COUNT_NOTE %d != expected %d (informational)' % (len(chains), EXPECTED_CHAINS))

    # 两侧配对：实测每条 chain setting 的数据成员是 source_chain / target_chain（名字）。
    # 用 getattr 兜底，成员被改名时降级成 unknown 而不是崩。
    matched = 0
    unknown = 0
    for i, chain in enumerate(chains):
        src = getattr(chain, 'source_chain', None)
        tgt = getattr(chain, 'target_chain', None)
        if src is None or tgt is None:
            unknown += 1
            continue
        same = str(src) == str(tgt)
        matched += 1 if same else 0
        if not same:
            print('PAIR_MISMATCH[%d] source=%s target=%s' % (i, src, tgt))
    pair_ok = '%d/%d' % (matched, len(chains))
    print('PAIR_OK %s' % pair_ok)
    if unknown:
        reasons.append('%d chain settings have no readable source_chain/target_chain' % unknown)
    if matched != len(chains):
        reasons.append('chain pairing incomplete: %s match' % pair_ok)

    print('UNAVAILABLE initialized-equivalent: the engine does not expose IKRetargetProcessor, so '
          '"is the processor really initialised" stays a heuristic; per-chain nested fk/ik '
          'semantics are out of scope here (dump_retargeter.py prints settings.* instead)')

verdict = 'PASS' if not reasons else 'CHECK'
print('PROBE_RESULT: %s | chains=%s | pair=%s | %s'
      % (verdict, 'n/a' if ctrl is None else len(ctrl.get_all_chain_settings()), pair_ok,
         'ok' if not reasons else '; '.join(reasons)))
print('PROBE_RETARGET_DONE')
