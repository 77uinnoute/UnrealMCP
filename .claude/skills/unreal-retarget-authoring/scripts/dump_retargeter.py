"""dump IK Retargeter 结构（只读）：root settings / chain settings / 两侧 rig+pose 现状。

做什么：把一个 IK Retargeter 的**真实结构原样打印出来**——root settings 的字段值、每条
chain settings 的**真实成员名与值**、两侧（SOURCE/TARGET）的 ik rig / preview mesh / 当前 pose 名。

为什么需要它：本 change 原稿假定的 `RetargetChainSettings` 字段名（chain_name / start_bone /
end_bone / goal_name / enable_fk / fk_rotation_mode / fk_translation_mode / enable_ik）**实测
全部不存在**（`get_editor_property` 报 `Failed to find property`）。字段名随引擎版本变，把它冻进
C++ 命令意味着每次纠正都要"编译 + 重启编辑器"，所以按仓库既定做法落成只读体检脚本。

跑在哪：编辑器 python（`execute_python_file`），全程只读、不写盘。

怎么用：改下面 `RTG` 常量 -> 跑 -> 看 `ROOT_FIELD/ROOT_DATA` 与 `CHAIN[n] DATA <name> = <value>` 行。

如何发现字段名（本脚本的职责，实测过两条路）：
  1. root settings 有 `export_text()` -> 取 UE 文本形式（形如 `(Key=Value,...)`）再按 `Key=Value` 解析；
  2. **`RetargetChainSettings` 实测没有 `export_text`**（它是 UObject 包装：`dir()` 里有
     `get_class` / `get_outer` / `modify` / `static_class`）-> 退回**反射**：取 `dir()` 的非 dunder
     **数据**成员（跳过可调用成员 = UObject 管道），逐个 `getattr` 打值。实测真实成员是
     `source_chain` / `target_chain` / `settings`（脚本**不硬编码**这些名字，靠 dir() 现取）。
**不硬编码任何未证实字段名**，也不凭猜补名字。

注意（PIE 纪律）：PIE 运行中引擎 python 的 `load_asset` / `get_controller` 对 IK Rig /
IK Retargeter 会**静默返回 None**；`None` MUST NOT 被解释成"资产不存在"——先 `stop_pie`
（用 `pie_running` 轮询确认已退出）再来跑本脚本。
"""
import re

import unreal

# ---------------- 参数 ----------------
RTG = '/Game/MCP/Ganyu/RTG_Ganyu'    # 目标 IK Retargeter（实测存在）
DUMP_ALL_CHAINS = True               # False = 只打第 0 条 chain + 总数
MAX_KV_LINES = 40                    # 每个结构体最多解析出多少键值（防刷屏）
MAX_DATA_MEMBERS = 24                # 每个对象最多打多少数据成员（防刷屏）

# 实测存在的 root settings 字段（其余字段靠 export_text / dir() 现场发现，别在这里猜）
ROOT_FIELDS = [
    'scale_horizontal', 'scale_vertical', 'rotation_alpha', 'translation_alpha',
    'blend_to_source', 'affect_ik_horizontal', 'affect_ik_vertical',
    'rotation_offset', 'translation_offset', 'blend_to_source_weights',
]
# --------------------------------------

KV = re.compile(r'([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(".*?"|[^,()]*)')
SIDES = ('SOURCE', 'TARGET')
_DIR_SEEN = set()


def struct_text(obj):
    """UE 文本形式；失败就如实回一个标记字符串，绝不让脚本炸掉。"""
    try:
        return str(obj.export_text())
    except Exception as exc:                     # noqa: BLE001 - 只报告，不抛
        return '<export_text failed: %s>' % exc


def data_members(obj):
    """dir() 的非 dunder 成员里**不可调用**的那些 = 数据字段（跳过 UObject 管道方法）。"""
    found = []
    for name in dir(obj):
        if name.startswith('_'):
            continue
        try:
            value = getattr(obj, name)
        except Exception:                        # noqa: BLE001 - 读不到就跳过
            continue
        if callable(value):
            continue
        found.append((name, value))
    return found[:MAX_DATA_MEMBERS]


def dump_struct(label, obj):
    text = struct_text(obj)
    if not text.startswith('<export_text failed'):
        print('%s TEXT %s' % (label, text))
        for key, value in KV.findall(text)[:MAX_KV_LINES]:
            print('%s KEY  %s = %s' % (label, key, value.strip()))

    # 成员名每个类只打一次（20 条 chain 同一个类，重复刷屏没意义）
    cls = type(obj).__name__
    if cls not in _DIR_SEEN:
        _DIR_SEEN.add(cls)
        print('%s DIR[%s] %s' % (label, cls,
                                 ','.join(sorted(m for m in dir(obj) if not m.startswith('_')))))
    for name, value in data_members(obj):
        print('%s DATA %s = %s' % (label, name, value))


def main(asset):
    # get_controller 是 IKRetargeterController 上的静态入口
    ctrl = unreal.IKRetargeterController.get_controller(asset)
    print('CTRL', ctrl)

    # 下列 getter MUST 显式传 source_or_target（少传报
    # `required argument 'source_or_target' not found`），所以两侧各调一次
    for side_name in SIDES:
        side = getattr(unreal.RetargetSourceOrTarget, side_name)
        print('SIDE %s ik_rig=%s' % (side_name, ctrl.get_ik_rig(side)))
        print('SIDE %s preview_mesh=%s' % (side_name, ctrl.get_preview_mesh(side)))
        print('SIDE %s current_pose_name=%s' % (side_name,
                                                ctrl.get_current_retarget_pose_name(side)))
        print('SIDE %s poses=%s' % (side_name,
                                    [str(p) for p in ctrl.get_retarget_poses(side)]))

    rs = ctrl.get_root_settings()
    print('ROOT_SETTINGS')
    for field in ROOT_FIELDS:
        print('ROOT_FIELD %s = %s' % (field, getattr(rs, field, '<missing>')))
    dump_struct('ROOT', rs)

    chains = ctrl.get_all_chain_settings()
    print('CHAIN_COUNT %d' % len(chains))
    for i, chain in enumerate(chains):
        if not DUMP_ALL_CHAINS and i > 0:
            break
        dump_struct('CHAIN[%d]' % i, chain)


UES = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
if UES.get_game_world() is not None:
    print('WARN_PIE_RUNNING load_asset/get_controller can return None while PIE runs; '
          'stop_pie first')

_ASSET = unreal.EditorAssetLibrary.load_asset(RTG)
print('ASSET %s -> %s' % (RTG, _ASSET))
if _ASSET is None:
    print('DUMP_RETARGETER_DONE asset_none (missing path, or PIE is running)')
else:
    main(_ASSET)
    print('DUMP_RETARGETER_DONE')
