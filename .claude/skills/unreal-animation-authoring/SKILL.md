---
name: unreal-animation-authoring
description: "UE5（UnrealMCP）里用 MCP 工具 + python 脚本程序化建/改动画资产的方法：序列（UAnimSequence）为什么必须走 C++ 命令、数据模型取帧率陷阱与帧率兼容规则、骨骼轨道/曲线/Notify/NotifyTrack/SyncMarker 的写入顺序与回读、压缩与派生数据同步等待、蒙太奇（UAnimMontage）section/slot/segment/notify/branching point/blend 的结构编辑纪律与「结构写先关编辑器」规则、编辑器会话与预览控制（打开 Persona、预览时间、播放/停止、跳段）、NAME_None 序列化陷阱、写入自证字段与验收纪律；以及「复刻官方模板 AnimBP（整份复制 → 换骨架 → 逐个换动画引用）」、动画状态机/状态/转移内层图的 python 取法（`unreal.load_object`）、BlendSpace 的三条硬边界（`Skeleton` EditConst / `BlendParameters` 定长数组的两种写法 / 新建资产没有运行时 triangulation ⇒ T 字，用 finalize_blend_space 一条命令收口）。触发场景：准备调用以下任一 MCP 工具前 MUST 加载本 Skill —— create_anim_sequence / create_anim_sequence_from_pose / add_bone_track / set_bone_track_keys / add_curve / set_curve_keys / add_notify / add_notify_state / add_notify_track / add_sync_marker / set_animation_frame_rate / compress_animation / create_montage_from_animation / create_empty_montage / duplicate_montage / add_section / remove_section / rename_section / add_slot_track / add_anim_segment / set_segment_play_rate / add_branching_point / set_blend_in / set_blend_out / open_animation_editor / open_montage_editor / refresh_montage_editor / set_preview_time / play_preview / stop_preview / jump_to_section / set_asset_properties（写 BlendSpace）/ finalize_blend_space（补 BlendSpace 运行时数据）；以及任何涉及「用 MCP 建/改 UE 动画序列或蒙太奇、骨骼动画数据、动画曲线、AnimNotify、同步标记、蒙太奇段落与槽位、动画编辑器预览、压缩动画、动画资产验收、复刻第三人称模板 locomotion 状态机、BlendSpace 混合空间（样本/轴参数/运行时数据/样本变「无动画」=引用断了）、走跑切换或 T 字姿势排查」的任务。材质与粒子不在这里：走 unreal-material-authoring / unreal-particle-authoring。"
metadata:
  version: "1.0.0"
  upstream: unreal-blueprint-authoring
  downstream: unreal-particle-authoring
---

# UnrealMCP 动画（序列 + 蒙太奇）编写 Skill

适用：本仓库 `Plugins/UnrealMCP`（bridge `127.0.0.1:55557`，MCP server 名 `unrealMCP`）。
目标：用 MCP 工具 + python 脚本**可重复、可验证**地建/改动画序列与蒙太奇。

**环境注意**：改了 `Plugins/UnrealMCP/Content/Python/**`（含工具描述、脚本）后需**重启 unrealMCP server**；改插件 C++ 需 `Build_UnrealMCP.bat` 编译 + 重启编辑器（`Editor.bat start`，或 `Build/start_editor.bat`）。

---

## 〇、为什么必须有这套命令（python 侧做不到）

| 数据 | 存储位置 | python 能否直接写 |
|---|---|---|
| 序列的骨骼轨道 / 帧率 / 长度 | `IAnimationDataModel` / `IAnimationDataController`（`#if WITH_EDITOR` 纯 C++ 接口，不是 UFUNCTION） | **不能**（`get_editor_property` 只给只读副本） |
| 序列的**曲线**（含 morph 曲线） | 同上，但控制器侧有可实例化的 UCLASS | **能**，用 `unreal.AnimSequencerController`（见 §2.7；类选错会 Assert 崩编辑器） |
| 蒙太奇 section / slot / segment | `CompositeSections` / `SlotAnimTracks[].AnimTrack.AnimSegments` | **不能**（instanced 结构、无蓝图接口） |
| Notify / NotifyTrack / SyncMarker | `Notifies` / `AnimNotifyTracks` / `AuthoredSyncMarkers` | 只能读（`Notifies` 是 `UPROPERTY` 但数组写没有蓝图入口） |
| 编辑器预览（Persona 时间/播放/跳段） | `UAnimPreviewInstance`（AnimGraph 模块，编辑器独占） | **不能**（`UAssetEditorSubsystem` 只知道编辑器实例，拿不到预览） |

所以动画的一切写入与预览控制都是本仓库的自建命令（category `anim_sequence` / `anim_montage`）。

三种用法：

| 用途 | 路径 |
|---|---|
| 单个操作 / 需要结构化错误 | MCP 工具（命令名 = 工具名） |
| 批量建/配（几十次写入） | 脚本内 `unreal.UnrealMCPPythonAPI.execute_mcp_command` **回环**（同步派发，无 TCP、无死锁） |
| 独立校验 | `unreal.AnimationLibrary`（`get_sequence_length()` / `get_num_frames()` / `get_animation_track_names()` / `get_animation_notify_events()` / `get_bone_pose_for_time()` …）+ `unreal.load_asset(...)` 直读属性 |

```python
import json, unreal

def bridge(cmd, **params):
    raw = unreal.UnrealMCPPythonAPI.execute_mcp_command(cmd, json.dumps({"type": cmd, "params": params}))
    return json.loads(raw)["result"]

print(bridge("get_animation_length", asset_path="/Game/Anims/MM_Idle")["length"])
```

脚本写到 `Content/Python/scripts/animation/*.py` → `execute_python_file(file_path=..., timeout=...)`（长脚本一律走文件，别用 inline 长字符串；命令一律同步，重活拆成多次短调用）。脚本的 bridge 包装要 `except RuntimeError` 兜住并记成结构化失败，避免一条失败打断整轮。

---

## 一、引擎语义与接口事实（带源码出处）

| 事实 | 出处 |
|---|---|
| **`UAnimSequenceBase::GetSamplingFrameRate()` 返回的是「项目默认帧率」**，不是该资产的帧率 → 用它换算帧/秒会错（60fps 的序列被当 30fps）；读帧率/帧数必须走数据模型 | `AnimSequenceBase.cpp` |
| `set_animation_frame_rate` 只接受**当前帧率的整数倍或整除数**（`IsMultipleOf`/`IsFactorOf`）；否则控制器 `ReportErrorf`，命令侧预检后返回 `incompatible_frame_rate` + `candidates`（current、×2、×4、÷2、÷4） | `IAnimationDataController::SetFrameRate` |
| 改帧率必须让控制器**走事务**（不能传 "不要事务"） | `AnimDataController.cpp` |
| 曲线写入要 `FAnimationCurveIdentifier(曲线名, RCT_Float)` + `AACF_DefaultCurve` 标志；曲线名不存在时先 `AddCurve` 再写 keys | `AnimDataController.h` |
| 引擎没有 `UAnimSequenceBase::AddNotify`；做法是 `Notifies.AddDefaulted_GetRef()` + `Link(资产, 时间)` + `TriggerTimeOffset = GetTriggerTimeOffsetForType(...)` → 再 `RefreshCacheData()`；返回索引要用新增项自己的 `Guid` 反查 | `AnimSequenceBase.cpp` |
| `UAnimNotify` / `UAnimNotifyState` 在 5.5 是**抽象类**，实例化会崩 → 命令侧显式拒绝抽象类 | `AnimNotify.h`/`AnimNotifyState.h` |
| 引擎的 NotifyTrack **有名字**（`FAnimNotifyTrack::TrackName`），不是隐式索引；改名要真改 `AnimNotifyTracks[i].TrackName` | `AnimSequenceBase.h` |
| SyncMarker 存在 `UAnimSequence::AuthoredSyncMarkers`：改完要 `SortSyncMarkers()` + `RefreshSyncMarkerDataFromAuthored()`，否则读回还是旧的 | `AnimSequence.h` |
| `FAnimTrack::ValidateSegmentTimes()` **没有 ENGINE_API** → 蒙太奇结构写的收尾只能做 `Montage->SetCompositeLength()` + `UpdateLinkableElements()` + `RefreshCacheData()` | `AnimCompositeBase.h` |
| **蒙太奇的段拖不动是设计，不是 bug**：`FAnimSegment::StartPos` 是 `UPROPERTY(VisibleAnywhere, DisplayName="Starting Position")`（只读）；且 `FAnimTrack::CollapseAnimSegments()` 排序后**把第一个段的 `StartPos` 直接置 0**、其余段贴在前一段尾部 ⇒ 单段蒙太奇里唯一的段永远从 0 开始，编辑器里拖它会被 collapse 拉回 | `AnimCompositeBase.h:104`、`AnimCompositeBase.cpp:505` |
| 蒙太奇的播放起点由**播放方**决定：`Montage_Play(..., InTimeToStartMontageAt)` → `NewInstance->SetPosition(Clamp(t, 0, MontageLength))`；`UAnimMontage::CreateSlotAnimationAsDynamicMontage` 的同名参数**已废弃不用**（注释明写） | `AnimInstance.cpp:2441`、`AnimMontage.cpp:3059` |
| `ACharacter::PlayAnimMontage(Montage, InPlayRate, StartSectionName)`（= 蓝图 `Play Anim Montage`）= `Montage_Play` + `JumpToSection`，**没有起始时间参数** | `Character.cpp` |
| 字段是 `NAME_None` 时 `FName::ToString()` 会输出字面量 `"None"` → 所有 section 链接 / sync group / track name / slot group 都序列化为**空串** | `NameTypes.h` |
| 蒙太奇**没有**「notify 关联到 section」的存储字段：`linked_section_name` 是**按触发时间推导**的，所以「link to section」的语义 = 把 notify 移到该 section 起始时间 | `AnimMontage.h` + 参考实现 |
| 蒙太奇的 BranchingPoint 就是 `MontageTickType == EMontageNotifyTickType::BranchingPoint` 的 notify（旧 `FBranchingPoint` 已废弃） | `AnimTypes.h` |
| `ExtractRootMotionFromTrackRange(0, t)` 是蒙太奇根运动的正确取法（按 segment 累加由引擎负责） | `AnimMontage.h` |
| 压缩是派生数据：`CacheDerivedDataForCurrentPlatform()` + `WaitOnExistingCompression(true)` 后 `IsCompressedDataValid()` 才算完成 | `AnimSequence.h` |
| Persona 打开**蒙太奇**标签时预览**默认就在播放**；跳段后预览代理时间滞后一 tick ⇒ 回读前需同步 | `AnimMontage.h` |
| 结构写（增删 section/slot/segment）前先关该资产编辑器（响应 `editor_closed` / `closed_editor_count`） | 设计约定 |

---

## 二、序列（`UAnimSequence`）

### 2.1 读取（都是只读命令）

| 用途 | 命令 |
|---|---|
| 找资产 | `list_anim_sequences`（只走资产注册表元数据，**不 load**）、`search_animations`、`find_animations_for_skeleton`、`get_anim_sequence_info`、`anim_self_check` |
| 长度/帧率/帧数/骨骼 | `get_animation_length`、`get_animation_frame_rate`、`get_animation_frame_count`、`get_animation_skeleton`、`get_rate_scale`、`get_animated_bones` |
| 采样根运动 | `get_bone_transform_at_time`、`get_bone_transform_at_frame`、`get_pose_at_time`、`get_pose_at_frame`、`get_root_motion_at_time`、`get_total_root_motion` |
| 曲线 | `list_curves`、`get_curve_info`、`get_curve_value_at_time`、`get_curve_keyframes` |
| Notify / 轨道 / 标记 | `list_notifies`、`get_notify_info`、`list_notify_tracks`、`get_notify_track_count`、`list_sync_markers` |
| 加性 / 根运动设置 / 压缩 | `get_additive_anim_type`、`get_additive_base_pose`、`get_enable_root_motion`、`get_root_motion_root_lock`、`get_force_root_lock`、`get_compression_info`、`get_source_files`、`export_animation_to_json` |

`get_animation_frame_rate` 返回的才是资产自己的采样帧率；`get_animation_frame_count` 是数据模型帧数（采样 key 数 = 帧数 + 1，因为多一个 T0 key）。用 `unreal.AnimationLibrary.get_num_frames()` 交叉验证时记得这个差 1。

### 2.2 创建与帧率

1. `create_anim_sequence(name, folder="/Game/MCPTests", skeleton=..., frame_rate=30, duration=1)`；要「参考姿势逐骨骼打满 key」用 `create_anim_sequence_from_pose`（同参数）。
2. 改帧率：`set_animation_frame_rate(asset_path, frame_rate=60)`。
   - **先看清当前帧率**（`get_animation_frame_rate`）：只接受整数倍/整除数关系，否则 `incompatible_frame_rate`（带 `candidates`）。
   - 改完回读 `get_animation_frame_rate` 与 `get_animation_frame_count`，并确认 `get_animation_length` 没变（帧率变了、时长不该变）。
3. `set_rate_scale(asset_path, rate_scale=1.5)` 是播放速率，不是采样帧率，别混。

### 2.3 骨骼轨道与曲线

1. `add_bone_track(asset_path, bone_name)` → 空轨道。
2. `set_bone_track_keys(asset_path, bone_name, keys=[{time, position:[x,y,z], rotation:[roll,pitch,yaw] 或 [x,y,z,w], scale:[x,y,z]}])` → 稀疏写入；key 的 `interp`/切线可选。
3. 回读：`get_animated_bones`（轨道清单）、`get_bone_transform_at_time`（按时间采样，验证稀疏 key 的插值结果）。
4. 曲线：`add_curve(asset_path, curve_name)` → `set_curve_keys(asset_path, curve_name, keys=[{time, value, arrive_tangent, leave_tangent, interp}])`，或单点追加 `add_curve_key(asset_path, curve_name, time=..., value=...)`；删除 `remove_curve`（整条）/`remove_bone_track`（整轨）。
5. 曲线写完必须回读 `get_curve_info`（key 数）+ `get_curve_value_at_time`（中间点插值，验证的是曲线本身不是写入回显）。

### 2.4 Notify / NotifyTrack / SyncMarker

1. 加 notify：`add_notify(asset_path, time=0.5, notify_class="AnimNotify_PlaySound", notify_name="FootStep")`；状态机用 `add_notify_state(..., duration=0.2, notify_class="AnimNotifyState_TimedParticleEffect")`。
   - **不要**传 `UAnimNotify` / `UAnimNotifyState` 本体（抽象类，会被拒）；短类名可容错解析，失败会回 `candidates`。
   - 返回 `notify_index`：它是数组索引，插入/删除后会漂移 → **后续按名字操作**。
2. 改名/改属性：`set_notify_name`、`set_notify_trigger_time`、`set_notify_duration`（仅 notify state，否则 `not_a_notify_state`）、`set_notify_track`、`set_notify_color`、`set_notify_trigger_chance`、`set_notify_trigger_on_server`、`set_notify_trigger_on_follower`、`set_notify_trigger_weight_threshold`、`set_notify_lod_filter`（`value` 形如 `{"filter_type":"lod","lod":1}`）。
   - `set_notify_name` 传旧名时会失效：改名后**旧名必须解析不到**（验收脚本就是这么断言的）。
3. 轨道：`add_notify_track(asset_path, name=..., color=...)`、`rename_notify_track(asset_path, track_name=旧名 / track_index=..., name=新名)`、`remove_notify_track`（**只剩一条时报 `last_notify_track`**，删轨后其上的 notify 落到 track 0）。
4. 同步标记：`add_sync_marker(asset_path, marker_name="FootL", time=0.2)`、`set_sync_marker_time`、`set_sync_marker_time_by_name`、`remove_sync_marker`（名字 + `time` 消歧；同名多个会报 `ambiguous_notify` 语义的歧义错误）。
5. `remove_notify` 用 `notify_index` 或 `notify_name`（可加 `time` 消歧）+ `guid`。

### 2.5 加性 / 根运动 / 压缩

- 加性：`set_additive_anim_type(asset_path, type="LocalSpace")`、`set_additive_base_pose(asset_path, base_pose="")`（空串 = 参考姿势）。
- 根运动：`set_enable_root_motion(asset_path, enable=True)`、`set_root_motion_root_lock(asset_path, lock_type="RefPose")`、`set_force_root_lock(asset_path, force=True)`。
- 压缩：`set_compression_scheme(asset_path, compression_scheme="/Game/.../UCS_X")` → `compress_animation(asset_path)`。
  - `compress_animation` **同步等待**派生数据（不是「已开始」），成功后回 `compressed_data_valid` + `compressed_size`；`compressed_data_valid=false` 视为失败（`compression_failed`）。之后 `get_compression_info` 交叉验证压缩比。

### 2.6 资产生命周期纪律（删了再建、以及别的脚本的产物）

五条长期纪律（违反会留下加载器失败标记，或让一次失败的删除连锁污染整轮）：

1. **不要对"文件已不存在的路径"做加载式解析**：加载缺失的包会在引擎加载器里留下永久的失败标记。自己写脚本时同理：**别用** `unreal.load_asset(已删路径)` 去"验证删干净了"，用"随后的同名创建是否成功"判定（残留会回 `asset_exists`）。
2. **删除走 `safe_delete_asset`**（响应带 `registry_notified`）：删除后**立即** `does_asset_exist` 仍可能是 True —— 注册表的**磁盘侧**条目要等编辑器自己扫描（一帧）才消失，属正常。
3. **创建/复制动画资产走直连工厂（`CreateAssetDirect`）**，不走 `IAssetTools::CreateAsset` / `DuplicateAsset`（后者会经 `CanCreateAsset` → `UPackageTools::HandleFullyLoadingPackages` 对刚被删掉文件的包做同步 fully-load）。
4. **一个 job 内「删掉刚创建的资产 + 同名重建」可行**；但一个 job 别串多个重资产脚本，验收 runner 一次只跑一个脚本。
5. **测试脚本不要拿别的脚本的产物当输入**：`find_animations_for_skeleton` 会把 `/Game/MCPTests/` 下的产物一起返回，而蒙太奇对动画是**硬引用**，被引用者的 `safe_delete_asset` 会回 `blockers`。产物目录要显式排除。

### 2.7 在 python 里直接写曲线（含 morph / 表情曲线）

除命令面（§2.3 的 `add_curve` / `set_curve_keys`）之外，python 脚本也能写曲线 —— **前提是控制器类与资产的数据模型匹配**：

| 资产的数据模型 | 用哪个控制器 |
|---|---|
| `AnimationSequencerDataModel`（本工程默认，`/Script/AnimationData.*` 出现在 `UAnimSequenceBase` 的 construct classes 里） | `unreal.AnimSequencerController` |
| legacy `AnimDataModel` | `unreal.AnimDataController` |

**类选错 = Assert 关掉编辑器**：legacy 控制器内部是 `Cast<UAnimDataModel>`，对 sequencer 数据模型会 fatal（`Casts.cpp` 的 modal assert）。所以**动手前先 `model.get_class().get_name()`**。

```python
import unreal

seq   = unreal.load_asset("/Game/.../MySeq")
model = seq.get_editor_property("data_model_interface")     # 先确认 get_class().get_name()
ctrl  = unreal.new_object(unreal.AnimSequencerController)    # 与上面那行匹配的类
ctrl.set_model(model)

ci = unreal.AnimationCurveIdentifierExtensions.get_curve_identifier(
    seq.get_editor_property("skeleton"), unreal.Name("curve_name"),
    unreal.RawCurveTrackTypes.RCT_FLOAT)

ctrl.open_bracket(unreal.Text("python curves"), True)
ctrl.add_curve(ci, 4, True)                                  # 4 = AACF_DefaultCurve
ctrl.set_curve_keys(ci, [unreal.RichCurveKey(time=t, value=v), ...], True)
ctrl.close_bracket(True)
unreal.EditorAssetLibrary.save_asset("/Game/.../MySeq", only_if_is_dirty=False)
```

- **曲线标识符只能走 `AnimationCurveIdentifierExtensions.get_curve_identifier(skeleton, name, RCT_FLOAT)`**：`unreal.AnimationCurveIdentifier(name, type)` 这个构造器在 python 侧不可调（`call() takes at most 0 arguments (2 given)`）。
- **morph 曲线就是 float 曲线**（数据模型里只有 Float / Transform 两类曲线），**曲线名 = morph target 名**；数据源按名与 `mesh.get_all_morph_target_names()` 求交（名字对不上的就是该模型没有的 morph，本该丢）。
- 一批曲线包在一个 bracket 里，最后统一 `save_asset`；写完回读 `model.get_number_of_float_curves()` 与帧数（帧数不该被写曲线改掉）。
- **写完曲线还不算完：morph 曲线要能被识别成 morph，必须给"曲线名"打上 morph 标记**。标记只存在两处：**骨架的曲线元数据**（`USkeleton` 上的 `UAnimCurveMetaData`）与**网格自带的同款 user data**（网格那份是 override 层）。曲线自己在求值时**不带 flag**，所以"曲线有键、值也对，脸就是不动"就是这个标记缺失。
  - 用 `set_curve_metadata_flags(asset_path=<骨架/网格/序列>, curve_names=[...], morphtarget=True)`（命令）；逐条回读 `curves[]` 里的 `morphtarget` 与 `metadata_count`。
  - 那张 map **不能用 `set_object_property` 写**（`property_not_writable`，引擎托管），别浪费轮次去试。
  - 打完标记要**重开资产 / 重启编辑器**再验（`FAnimInstanceProxy` 用的 `RequiredBones->GetCurveFlags()` 是 BoneContainer 初始化时缓存的）。
  - Persona 的曲线列表里那个 "Morph Target" 列读的也是**骨架元数据**，可以拿它当"标记有没有落上"的目视判据。
- **验证不看视口**：`AnimPoseExtensions.get_anim_pose_at_frame(anim, frame, unreal.AnimPoseEvaluationOptions())` 拿到的 pose 里就有这些曲线 —— `pose.get_curve_names()` / `pose.get_curve_weight(unreal.Name("..."))`。多取几个帧看权重是否在变（读的是运行时同一份数据）。`AnimPose` 只暴露这两个曲线读法。
- 想让 morph 曲线真的驱动 morph target，**骨架**上还要有同名曲线的 `FCurveMetaData.Type.bMorphtarget`；这份元数据 python 读不到（`UAnimCurveMetaData` 未暴露），用 `reflect_probe` 读 `AssetUserData[i]` 再进 `CurveMetaData`，写用 `set_curve_metadata_flags`（见上一条）。

---

## 三、蒙太奇（`UAnimMontage`）

### 3.1 发现与创建

- 发现：`list_montages`（注册表元数据，不 load）、`find_montages_for_skeleton`、`find_montages_using_animation`（先走注册表 referencers，新资产依赖数据未就绪时退化为有上限的 load 扫描，响应里的 `scan_strategy` 说明走了哪条路）、`get_montage_info`、`get_montage_length`、`get_montage_skeleton`。
- 创建：`create_montage_from_animation(anim_path, name, folder)`、`create_empty_montage(name, skeleton, folder)`、`duplicate_montage(asset_path, name, folder)`。
  - 创建结果**一定有 `Default` section**（`UAnimMontageFactory::EnsureStartingSection` 语义）；空蒙太奇也自带 `DefaultSlot` 槽位，所以 `add_anim_segment` 一定有地方写。
  - 从动画创建后 `get_montage_length` 应等于源动画长度（±0.001），section 列表含 `Default`。

### 3.2 Section

| 用途 | 命令 |
|---|---|
| 读 | `list_sections`、`get_section_info`、`get_section_index_at_time`、`get_section_name_at_time`、`get_section_length`、`get_next_section`、`get_all_section_links` |
| 写 | `add_section`、`remove_section`、`rename_section`、`set_section_start_time`、`set_next_section`、`set_section_loop`、`clear_section_link` |

- 寻址：`section_name` **优先**，`section_index` 兜底；列表始终按时间排序（`add_section`/`set_section_start_time` 之后索引会变，所以写入响应会回读**新**索引）。
- `remove_section`：只剩一个时报 `last_section`；删掉后指向它的 next 链接被清空（`get_all_section_links` 里不该再出现那个名字）。
- `rename_section`：所有指向旧名的链接跟着改；改名后旧名解析不到。
- `set_section_loop(loop=True)` = 把 next 指向自己（`get_next_section` 的 `is_looping` 为真）；`clear_section_link` = 清 next。

### 3.3 SlotTrack 与 AnimSegment

| 用途 | 命令 |
|---|---|
| 槽位 | `list_slot_tracks`、`get_slot_track_info`、`add_slot_track`、`remove_slot_track`、`set_slot_name`、`get_all_used_slot_names`（蒙太奇用到的 + 骨骼定义的） |
| 段 | `list_anim_segments`、`get_anim_segment_info`、`add_anim_segment`、`remove_anim_segment`、`set_segment_start_time`、`set_segment_play_rate`、`set_segment_start_position`、`set_segment_end_position`、`set_segment_loop_count` |

- 槽位寻址：`slot_name` 优先 / `track_index` 兜底；只剩一条槽位时 `remove_slot_track` 报 `last_slot_track`。
- 段字段语义（容易混）：`start_time` = 段在**蒙太奇时间轴**上的位置（内部 `FAnimSegment::StartPos`）；`start_position`/`end_position` = 段取用的**动画内部**时间（`AnimStartTime`/`AnimEndTime`）；`play_rate` = 段播放速率（`AnimPlayRate`，负值倒放，0 被拒）；`loop_count` = 段内循环次数（≥1）。
- `add_anim_segment(asset_path, slot_name="DefaultSlot", anim_path=..., start_time=..., play_rate=..., loop_count=...)`：三者省略时用引擎默认（0 / 1 / 1）；引用走 `SetAnimReference(anim, /*bInitialize=*/true)`，所以默认是「整段动画、1 倍速」。加性类型不匹配、空动画、把蒙太奇塞进蒙太奇都会被拒（`invalid_value`）。
- 增删段后蒙太奇长度会重算（`length` 回读会变），这是正确的：长度 = 最长槽位轨长度。

### 3.4 Notify 与 BranchingPoint

- **notify 命令是序列/蒙太奇共用的**（注册表命令名唯一，按资产类型分派）：`list_notifies`、`add_notify`、`add_notify_state`、`remove_notify`、`set_notify_trigger_time`、`get_root_motion_at_time`。传蒙太奇时响应带 `is_montage: true`，且 notify 条目**额外**有 `linked_section_name`（推导）与 `is_branching_point`。
- 蒙太奇专属：`set_notify_link_to_section(asset_path, section="Attack", notify_index=0 / notify_name=...)`（= 把 notify 移到该 section 起始时间）、`list_branching_points`、`add_branching_point(name, trigger_time, track_index=0)`、`remove_branching_point(branching_point_index)`、`is_branching_point_at_time(time)`。
- `branching_point_index` 是「分支点序号」，`notify_index` 是「notify 数组索引」；`remove_branching_point` **只**删分支点，同位置普通 notify 不动（验收脚本会断言 notify 计数只减 1）。
- **要一个「能拖的播放起点」就把 notify 当把手**（段本身被引擎钉死，见 §一）：在目标时刻放一条普通 notify（约定名 `Start`），播放方在 `Montage_Play` **之前**扫 `Montage->Notifies` 取同名 notify 的 `GetTriggerTime()`（找不到回退 0），把它当 `InTimeToStartMontageAt` 传下去；改起点 = 在编辑器里拖那条 notify（脚本等价物 `set_notify_trigger_time`）。`Montage_Play` 在蓝图侧是 `Play Anim Montage`（无起始时间参数，见 §一）⇒ 蓝图要用就把「读把手时间 + `Montage_Play`」包成一个 `BlueprintCallable`。
  验收：把手在 1.0s 时 PIE 里 `Montage_GetPosition()` 读到 1.0（该函数 `BlueprintPure`，python 可读）。

### 3.5 Blend 与根运动

- 读：`get_blend_settings`（blend_in/out 的 time/option/mode + `blend_out_trigger_time` + `enable_auto_blend_out`）。
- 写：`set_blend_in(blend_time=, blend_option=)`、`set_blend_out(...)`、`set_blend_out_trigger_time(trigger_time)`（**负值合法**，引擎语义是「改用 blend_out time」，默认 -1）。
- 合法 `blend_option`：Linear / Cubic / HermiteCubic / Sinusoidal / QuadraticInOut / CubicInOut / QuarticInOut / QuinticInOut / CircularIn / CircularOut / CircularInOut / ExpIn / ExpOut / ExpInOut / Custom；写错报 `invalid_blend_option` + 全量 `candidates`，且**设置不变**。
- 根运动：`get_enable_root_motion_translation` / `set_enable_root_motion_translation(enable=)`、`get_enable_root_motion_rotation` / `set_enable_root_motion_rotation(enable=)`；采样用 `get_root_motion_at_time(time)`（对蒙太奇等价于 `ExtractRootMotionFromTrackRange(0, t)`）。

### 3.6 结构写 = 先关编辑器

下面这些命令是**结构写**，会在写入前关掉该资产的 Persona 编辑器并在响应里带 `editor_closed: true` + `closed_editor_count`：

`add_section` / `remove_section` / `rename_section` / `add_slot_track` / `remove_slot_track` / `add_anim_segment` / `remove_anim_segment` / `add_branching_point` / `remove_branching_point`

原因：Persona 预览里持有指向 `SlotAnimTracks` / `AnimSegments` / `CompositeSections` 的原始指针与索引，结构一变就悬垂。非结构写（section 时间/next/loop、slot 改名、段字段、notify、blend）不动结构，所以不关编辑器。

---

## 四、编辑器会话与预览

| 用途 | 命令 |
|---|---|
| 打开 | `open_animation_editor(asset_path)`（序列或蒙太奇）、`open_montage_editor(asset_path)`（仅蒙太奇，否则 `asset_not_anim_montage`） |
| 刷新 | `refresh_montage_editor(asset_path)` = 关标签再开（重读资产） |
| 预览时间 | `set_preview_time(asset_path, time)`（序列 `SetPosition`，蒙太奇 `MontagePreview_JumpToPosition`，同时移动「预览起始 section」） |
| 播放/停止 | `play_preview(asset_path, loop=?, play_rate=?)`、`stop_preview(asset_path)`（`loop`/`play_rate` 只对序列生效；蒙太奇预览由自己的 section 循环设置驱动） |
| 跳段 | `jump_to_section(asset_path, section="Attack")` |

- **这些命令改的是编辑器显示，不是资产** → 它们不发事务、**不保存**（响应里**没有** `saved`），验收时应该断言「没有 saved」。
- 没有会话时一律 `editor_not_open`，消息里点名先调 `open_animation_editor` / `open_montage_editor`，**不会**静默成功。
- 会话按 `GetCurrentAsset()` 匹配本资产，不抢占别的编辑器预览。
- `jump_to_section` 保留调用前的播放态（暂停→仍暂停、播放→仍播放）；跳段后读回前内核同步代理时间。
- 收尾：`close_asset_editors(asset_path=...)`（editor 域命令）关标签；结构写会自动关。
- 效果验收**由用户看视口**：截图不作为验收证据（仓库规则）。

---

## 五、工具全表

### 5.1 序列：读

| 命令 | 说明 |
|---|---|
| `anim_self_check` | 域存活探针（`registered_anim_commands`） |
| `list_anim_sequences` | 按文件夹/骨骼列出序列（注册表元数据，不 load） |
| `search_animations` | 名字片段/通配搜序列 |
| `find_animations_for_skeleton` | 某骨骼下的全部序列 |
| `get_anim_sequence_info` | 单资产汇总（长度/帧率/帧数/轨道数/notify 数…） |
| `get_animation_length` | 时长（秒） |
| `get_animation_frame_rate` | 采样帧率（**数据模型**值） |
| `get_animation_frame_count` | 数据模型帧数 |
| `get_animation_skeleton` | 所属骨骼（字段名是 `skeleton`） |
| `get_rate_scale` | 播放速率倍率 |
| `get_animated_bones` | 有轨道的骨骼清单 |
| `get_bone_transform_at_time` | 按时间采样某骨骼 |
| `get_bone_transform_at_frame` | 按帧采样某骨骼 |
| `get_pose_at_time` | 整姿态采样（时间） |
| `get_pose_at_frame` | 整姿态采样（帧） |
| `get_root_motion_at_time` | 从 0 到该时刻的根运动（序列/蒙太奇共用） |
| `get_total_root_motion` | 整段根运动 |
| `list_curves` | 曲线清单 |
| `get_curve_info` | 单曲线（key 数/类型） |
| `get_curve_value_at_time` | 曲线在某时刻的值 |
| `get_curve_keyframes` | 曲线 key 明细 |
| `list_notifies` | notify 清单（序列/蒙太奇共用） |
| `get_notify_info` | 单个 notify |
| `list_notify_tracks` | 通知轨道 |
| `get_notify_track_count` | 轨道数 |
| `list_sync_markers` | 同步标记 |
| `get_additive_anim_type` | 加性类型 |
| `get_additive_base_pose` | 加性基础姿势 |
| `get_enable_root_motion` | 是否提取根运动 |
| `get_root_motion_root_lock` | 根锁模式 |
| `get_force_root_lock` | 是否强制根锁 |
| `get_compression_info` | 压缩状态（scheme/大小/比） |
| `get_source_files` | 源文件（FBX/导入信息） |
| `export_animation_to_json` | 导出资产文档 JSON |
| `open_animation_editor` | 打开 Persona（见第四节） |
| `set_preview_time` | 移动预览时间（序列/蒙太奇共用） |
| `play_preview` | 开始/继续预览（共用） |
| `stop_preview` | 预览停止 |

### 5.2 序列：写

| 命令 | 说明 |
|---|---|
| `create_anim_sequence` | 建序列（name/folder/skeleton/frame_rate/duration/tracks） |
| `create_anim_sequence_from_pose` | 建序列并用参考姿势打满 key |
| `set_animation_frame_rate` | 改采样帧率（兼容性预检） |
| `set_rate_scale` | 改播放速率倍率 |
| `add_bone_track` | 加空骨骼轨道 |
| `remove_bone_track` | 删骨骼轨道 |
| `set_bone_track_keys` | 写骨骼轨道 key（稀疏/烤制） |
| `add_curve` | 加曲线 |
| `remove_curve` | 删曲线 |
| `set_curve_keys` | 覆盖写曲线 key |
| `add_curve_key` | 追加一个曲线 key |
| `add_notify` | 加即时 notify |
| `add_notify_state` | 加带时长 notify state |
| `remove_notify` | 删 notify |
| `set_notify_trigger_time` | 改触发时间 |
| `set_notify_duration` | 改时长（仅 state） |
| `set_notify_track` | 改所属通知轨道 |
| `set_notify_name` | 改名 |
| `set_notify_color` | 改颜色 |
| `set_notify_trigger_chance` | 改触发概率 |
| `set_notify_trigger_on_server` | 仅服务器触发 |
| `set_notify_trigger_on_follower` | 仅 follower 触发 |
| `set_notify_trigger_weight_threshold` | 权重阈值 |
| `set_notify_lod_filter` | LOD 过滤（none/lod + 等级） |
| `add_notify_track` | 加通知轨道 |
| `rename_notify_track` | 重命名通知轨道 |
| `remove_notify_track` | 删通知轨道（保护最后一条） |
| `add_sync_marker` | 加同步标记 |
| `remove_sync_marker` | 删同步标记 |
| `set_sync_marker_time` | 改标记时间（按索引） |
| `set_sync_marker_time_by_name` | 改标记时间（按名字） |
| `set_additive_anim_type` | 设置加性类型 |
| `set_additive_base_pose` | 设置加性基础姿势 |
| `set_enable_root_motion` | 开关根运动提取 |
| `set_root_motion_root_lock` | 设置根锁模式 |
| `set_force_root_lock` | 开关强制根锁 |
| `set_compression_scheme` | 指定骨骼压缩设置 |
| `compress_animation` | 同步压缩并回读结果 |
| `set_blend_space_samples` | 整表写 BlendSpace 样本 + 自动 finalize + 回读自证（见 `unreal-animation-case-template-abp` 的 BlendSpace 那节） |

### 5.3 蒙太奇

| 命令 | 说明 |
|---|---|
| `list_montages` | 列出蒙太奇（注册表元数据） |
| `get_montage_info` | 汇总 + sections + slot_tracks + blend |
| `find_montages_for_skeleton` | 某骨骼下的蒙太奇 |
| `find_montages_using_animation` | 用了某动画的蒙太奇（`scan_strategy`） |
| `get_montage_length` | 时长 |
| `get_montage_skeleton` | 所属骨骼 |
| `create_montage_from_animation` | 从动画建蒙太奇（含 Default section） |
| `create_empty_montage` | 建空蒙太奇（Default section + DefaultSlot） |
| `duplicate_montage` | 复制蒙太奇 |
| `list_sections` | section 清单（按时间） |
| `get_section_info` | 单 section |
| `get_section_index_at_time` | 某时刻所在 section 索引 |
| `get_section_name_at_time` | 某时刻所在 section 名 |
| `get_section_length` | section 长度 |
| `get_next_section` | next 链接 |
| `get_all_section_links` | 全部链接对 |
| `add_section` | 加 section（自动按时间重排） |
| `remove_section` | 删 section（保护最后一个） |
| `rename_section` | 改名（链接跟随） |
| `set_section_start_time` | 改起始时间 |
| `set_next_section` | 设 next（空串 = 清除） |
| `set_section_loop` | 设/取消自身循环 |
| `clear_section_link` | 清 next 链接 |
| `list_slot_tracks` | 槽位轨清单 |
| `get_slot_track_info` | 单槽位轨 |
| `add_slot_track` | 加槽位轨 |
| `remove_slot_track` | 删槽位轨（保护最后一条） |
| `set_slot_name` | 槽位改名 |
| `get_all_used_slot_names` | 用到的 + 骨骼定义的 slot 名 |
| `list_anim_segments` | 某槽位轨的段清单 |
| `get_anim_segment_info` | 单个段 |
| `add_anim_segment` | 加段 |
| `remove_anim_segment` | 删段 |
| `set_segment_start_time` | 段在蒙太奇上的位置 |
| `set_segment_play_rate` | 段播放速率 |
| `set_segment_start_position` | 段取用动画的起始时间 |
| `set_segment_end_position` | 段取用动画的结束时间 |
| `set_segment_loop_count` | 段内循环次数 |
| `set_notify_link_to_section` | 把 notify 移到 section 起始 |
| `list_branching_points` | 分支点清单 |
| `add_branching_point` | 加分支点 |
| `remove_branching_point` | 删分支点（只删分支点） |
| `is_branching_point_at_time` | 某时刻是否分支点 |
| `set_blend_in` | 设置 blend in |
| `set_blend_out` | 设置 blend out |
| `get_blend_settings` | 读 blend 设置 |
| `set_blend_out_trigger_time` | 设置 blend out 触发时间（可负） |
| `get_enable_root_motion_translation` | 读平移根运动开关 |
| `set_enable_root_motion_translation` | 写平移根运动开关 |
| `get_enable_root_motion_rotation` | 读旋转根运动开关 |
| `set_enable_root_motion_rotation` | 写旋转根运动开关 |
| `open_montage_editor` | 打开蒙太奇编辑器 |
| `refresh_montage_editor` | 关+开（重读资产） |
| `jump_to_section` | 预览跳段 |

> 提醒：`anim_montage` 里**没有**单独的 notify 命令——`list_notifies` / `add_notify` / `add_notify_state` / `remove_notify` / `set_notify_trigger_time` / `get_root_motion_at_time` 归在 `anim_sequence` 里但**同样接受蒙太奇**（回包 `is_montage: true`）。

---

## 六、错误码

| 码 | 触发 |
|---|---|
| `asset_not_found` | 路径解析不到资产 |
| `asset_not_anim_sequence` | 传了非动画资产（或 composite 用在只支持序列/蒙太奇的地方） |
| `asset_not_anim_montage` | 蒙太奇专属命令收到非蒙太奇 |
| `asset_not_skeleton` | 需要骨骼资产却给了别的 |
| `asset_exists` | 创建/复制时同名资产已存在（**先 `safe_delete_asset` 或换名**） |
| `create_failed` | 工厂没有产出资产（创建后加载不回来） |
| `invalid_params` | 缺必填参数 / 类型不对 / 抽象 notify 类 |
| `invalid_value` | 数值非法（0 播放速率、负时间、越界时间、loop_count < 1…） |
| `unknown_value` | 枚举字符串不认识 |
| `unsupported_property` | 该资产类型不支持这个属性（响应带 `available_fields`） |
| `bone_not_found` / `bone_track_not_found` / `bone_track_exists` | 骨骼轨道相关 |
| `curve_not_found` / `curve_exists` | 曲线相关 |
| `notify_not_found` / `ambiguous_notify` | notify 找不到 / 名字命中多个（带 `candidates`，用 `time` 消歧） |
| `notify_track_not_found` / `last_notify_track` / `notify_track_name_taken` | 通知轨道相关 |
| `not_a_notify_state` | 对即时 notify 设时长 |
| `sync_marker_not_found` | 同步标记找不到 |
| `incompatible_frame_rate` | 新帧率不是当前帧率的整数倍/整除数（带 `candidates`） |
| `compression_failed` | 压缩后派生数据仍无效 |
| `section_not_found` / `section_name_taken` / `last_section` | section 相关 |
| `slot_track_not_found` / `slot_name_taken` / `last_slot_track` | 槽位轨相关 |
| `segment_not_found` | 段索引越界 |
| `branching_point_not_found` | 分支点序号越界 |
| `invalid_blend_option` | blend option 不认识（带全量 `candidates`） |
| `editor_not_open` | 编辑器会话命令没有可用预览（先 `open_animation_editor` / `open_montage_editor`） |
| write_failed | 引擎层写入失败（数组/控制器拒绝） |

失败一律是结构化回包（`success: false` + `error_code` + 说明），命名字符串类还会带 `candidates` 或 `available_fields`。

---

## 七、验收纪律

1. **写入必须回读**：每个写命令都返回它写后的读回字段（例：`add_notify` → `notify` 对象 + `notify_count`；`set_blend_in` → `blend_time`/`blend_option` + `blend`）。断言要用**读命令**再读一遍，而不是相信写入回显。
2. **交叉验证**：长度用 `unreal.AnimationLibrary.get_sequence_length()`、姿态用 `get_bone_pose_for_time()`、notify 用 `get_animation_notify_events()` 独立再算一遍。
3. **失败路径必须留痕**：反例（删唯一 section / 删唯一槽位 / 非法 blend option / 越界时间 / 抽象 notify 类 / 0 播放速率）要求同时满足「结构化 error_code」+「前后计数器不变」。
4. **持久化证据**：写成功回包里 `saved: true`；`.uasset` 的 mtime 应随写入前移；包应 `is_dirty() == False`。
   **批量写要显式关落盘**：本域写命令默认"成功即落盘"，同一条序列/蒙太奇上几十次写就是几十次整包写盘（编辑器卡顿、同步 job 期间整条 MCP 通道被占住）。
   写命令都接受可选参数 `persist`：批内传 `false`（编辑进内存与撤销栈照常、**不写盘**），批末用
   `unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)` 落一次盘；回执里 `saved` 说明本次到底写没写盘、
   `persist_requested` 回显你要的是什么。注意 `does_asset_exist` **不是**"盘上有文件"的证据（新建资产在创建时就已注册，随即答 true）。
5. **脚本幂等**：自己的产物先 `safe_delete_asset` + `unreal.SystemLibrary.collect_garbage()`，并断言内存里没有残留对象。
6. 产物统一放 `/Game/MCPTests/`；输入资产（别人的序列/骨骼）只读，不写不存。
7. 脚本落在 `Content/Python/scripts/animation/`（`test_anim_sequence_reads.py` / `test_anim_sequence_writes.py` / `test_anim_montage_edits.py` / `test_anim_editor_session.py`），用 `execute_python_file(deferred=True)` 跑，末尾打印 `*_RESULT: OK|FAIL`；结果记录进 `TestResults/`。

---

## 八、写入自证字段速查

| 写命令组 | 回包里可用来自证的字段 |
|---|---|
| 创建（序列/蒙太奇） | `created`、`asset_path`、`saved`、`length`、`sections`（蒙太奇） |
| 帧率/速率 | `frame_rate`、`frame_count`、`length`、`rate_scale` |
| 骨骼轨道/曲线 | `bone_track_count`、`key_count`、`curve_count`、`value`（采样值） |
| notify 族 | `notify`（含 `trigger_time`/`duration`/`track_index`/`guid`/`linked_section_name`/`is_branching_point`）、`notify_count` |
| 通知轨道/同步标记 | `track_index`/`track_name`/`marker_index`/`marker_count` |
| section 族 | `section`（`section_name`/`start_time`/`end_time`/`length`/`next_section_name`/`is_looping`）、`section_count`、`editor_closed` |
| slot/segment 族 | `slot_track`、`segment`（`start_time`/`end_time`/`start_position`/`end_position`/`play_rate`/`loop_count`）、`segment_count`、`editor_closed` |
| blend/根运动 | `blend`（in/out 全部字段）、`enable_root_motion_*` |
| 编辑器会话 | `opened`/`refreshed`、`editor_open`、`preview_available`、`preview_time`、`playing`、`current_section_name`（**没有** `saved`，这些命令不碰资产） |

---

> **这一节已拆成独立 skill**：`unreal-animation-case-template-abp`（复刻官方模板 ABP 与 BlendSpace）。
> **这一节已拆成独立 skill**：`unreal-animation-case-joint-transition-band`（关节过渡带：图侧配方 + 权重侧做法）。
