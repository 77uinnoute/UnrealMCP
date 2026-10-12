---
name: unreal-video-capture
description: "UE5（UnrealMCP）里把「效果动起来的样子」拿到手的方法：为什么逐次 take_screenshot 采样不可用（一次一张、相位不可控、撞空档），为什么**定格相位帧 ≠ 实机观感**（TAA/TSR 是跨帧累积器；且引擎内建节点直接用 Time，冻不住），Movie Render Queue（MRQ）的启用步骤（插件默认未启用，改 .uproject + 重启编辑器）、用 python 建「相机 → LevelSequence → 相机切轨 → MRQ job → PNG 帧序列」全链路的正确 API 名与字段名、MRQ 每帧全新渲染的语义边界（出片是设计真相、不是实机观感）、定格相位的确定性取样法（Period 极大 + Phase 定值）与它的失效边界、以及用帧文件大小差异自证「Time 真的在推进」+ ffmpeg 编码。触发场景：需要从 UE 出视频/帧序列、需要看到效果连起来的样子、想按指定相位做确定性对照、`unreal.MoviePipeline*` 类在 python 里不存在、MRQ 渲染进度长时间停在 0、要判断某个伪影是资产问题还是时间性渲染问题（TAA）、发现「同一个效果第 1 刀明显、第 2 刀变弱/发糊/退化成一堆干净弧线」、要给展示场景锁曝光，或在 `execute_python_*` 里调用 MoviePipelineQueueSubsystem / LevelSequence / CameraActor / PostProcessVolume 之前。"
metadata:
  version: "1.0.0"
  upstream: ~
  downstream: ~
---

# UnrealMCP 出片与抓帧 Skill

适用：本仓库 `Plugins/UnrealMCP`（bridge `127.0.0.1:55557`，MCP server 名 `unrealMCP`）。
目标：把「效果动起来是什么样」**可复现地**拿到手——视频或帧序列——并知道每种手段的观感边界。

> 本文只写**可复用的做法与判据**；不收录具体配方。

**环境注意**：改 `Plugins/UnrealMCP/Content/Python/**` 后需重启 unrealMCP server；改插件 C++ 需 `Build_UnrealMCP.bat` + 重启编辑器。

---

## 〇、先选方法（判据）

| 你要回答的问题 | 用 | 不要 |
|---|---|---|
| **动起来是什么样**（形状 / 时序 / 层次） | MRQ 出帧序列或视频（§三） | 逐次 `take_screenshot` —— 一次一张、相位不可控、会撞空档 |
| **指定相位的确定性对照**（第 k 帧长什么样） | 材质定格相位（§六）+ 截图 | 让动画跑着抓 —— 抓到的相位是随机的 |
| **玩家实机看到的样子** | PIE + 人工看，或对 MRQ 开快门/运动模糊（§五） | 拿 MRQ 出片当"实机" —— 它没有跨帧累积 |
| **某个伪影是资产问题还是渲染问题** | 同一姿势用不同抗锯齿方法各抓一张（§二） | 只看一张就下结论 |

**纪律：结论必须来自同一条渲染路径。** 编辑器视口 / PIE / MRQ 是**三条不同的渲染路径**（AA 与曝光设置、是否跨帧累积、是否离线全渲都不同），
别拿一条的结果推断另一条。要跨路径对比，先让编辑器视口 pilot 到同一台相机（`pilot_level_actor`），再比画面。

---

## 一、为什么"定格一帧"和"动起来"不一样

两条独立原因，**都会**让静止帧与实机观感分叉：

1. **冻结只冻住你冻的那条链**。用 `Period` 冻结时，只有走 `frac(Time/Period + Phase)` 的部分被冻住；
   **引擎内建节点内部直接用 `Time × Speed`**（`MaterialExpressionRotator` 是最常见的一个），压根不经过 `Period` ⇒ 定格时它还在动。
2. **TAA / TSR 是跨帧累积器**。动起来时它把最近约 10 帧的姿势**叠**在一起：细带被沿运动方向拖开 ⇒ **更宽、更亮、带尾迹**；
   定格时它收敛到静止姿势 ⇒ **更细、更锐、更干净**。所以静止帧永远比实机"瘦"。

**判据（区分是 1 还是 2）**：同一定格姿势**连截两张**。若某块区域（粒子、旋转的 UV）在变而轮廓不变 → 是第 1 条（那部分没冻住）；
若两张完全一致、但跑起来就是不一样 → 是第 2 条（时间累积）。

**关掉时间累积看单帧真值**：`r.AntiAliasingMethod` → `0`=None / `1`=FXAA / `2`=TAA / `4`=TSR(UE5 默认)。
在 PIE 里用 `SystemLibrary.execute_console_command(world, 'r.AntiAliasingMethod 1')`。这条是**本会话生效、不落 ini**，改完记得改回来。

---

## 二、MRQ 前置：插件默认没启用

引擎里 `Engine/Plugins/MovieScene/MovieRenderPipeline/MovieRenderPipeline.uplugin` 的 `"EnabledByDefault": false`。

**症状**：python 里 `unreal.MoviePipelineQueueSubsystem`、`unreal.MoviePipelineOutputSetting`、`unreal.MoviePipelinePIEExecutor` 等
`MoviePipeline*` 类**全都不存在**（`dir(unreal)` 里搜 `MoviePipeline` 为空）—— 老版 `unreal.MovieSceneCapture*` 类反而在，别误判成"UE 没有这个能力"。

**做法**：工程 `.uproject` 的 `Plugins` 数组里加

```json
{ "Name": "MovieRenderPipeline", "Enabled": true }
```

然后**重启编辑器**（插件只在启动时加载）。本仓库重启三步（脚本是唯一出处，别手敲）：

```
Plugins\UnrealMCP\Editor.bat stop
Plugins\UnrealMCP\Editor.bat start
Plugins\UnrealMCP\Build\wait_bridge.bat
```

- 改 `.uproject` 前先确认 `get_dirty_map_packages()` 为空（重启丢未保存改动）。
- **重启后编辑器不会回到你原来的关卡**（会落在上次打开的那张图）⇒ 显式 `LevelEditorSubsystem.load_level(...)` 切回去。

---

## 三、建 MRQ 管线（顺序固定）

### 3.1 相机

用 `unreal.CameraActor` 而不是 `unreal.CineCameraActor`：前者直接有 `field_of_view`，省掉 filmback/焦距换算。

```python
cam = EditorActorSubsystem.spawn_actor_from_class(unreal.CameraActor, unreal.Vector(-500,0,200), unreal.Rotator(0,0,0))
cam.get_component_by_class(unreal.CameraComponent).set_field_of_view(90.0)
```

### 3.2 LevelSequence

```python
ls = AssetToolsHelpers.get_asset_tools().create_asset('LS_X', '/Game/Dir', unreal.LevelSequence, unreal.LevelSequenceFactoryNew())
ls.set_display_rate(unreal.FrameRate(30, 1))
ls.set_playback_start_seconds(0.0); ls.set_playback_end_seconds(3.4)
```

### 3.3 相机切轨（master track）——三个易错点

```python
SE = unreal.MovieSceneSequenceExtensions
cut  = SE.add_track(ls, unreal.MovieSceneCameraCutTrack)   # 没有 add_master_track 这个名字
sec  = cut.add_section()
sec.set_range(0, 102)                                      # 收 int 帧号，不收 FrameNumber
cam_bind = ls.add_possessable(cam)
sec.set_camera_binding_id(ls.get_binding_id(cam_bind))
```

- **幂等**：重跑前用 `SE.find_tracks_by_type(ls, ...)` / `cut.get_sections()` / `ls.get_possessables()` 先查再建，
  否则会叠出第二条切轨 / 第二个绑定。
- 没有相机切轨，MRQ 不知道从哪台相机渲。

### 3.4 MRQ job

```python
sub = unreal.get_editor_subsystem(unreal.MoviePipelineQueueSubsystem)
q = sub.get_queue(); q.delete_all_jobs()
job = q.allocate_new_job(unreal.MoviePipelineExecutorJob)
job.set_editor_property('job_name', 'X')
job.set_editor_property('sequence', unreal.SoftObjectPath('/Game/Dir/LS_X.LS_X'))
job.set_editor_property('map',      unreal.SoftObjectPath('/Game/Dir/L_X.L_X'))
unreal.MoviePipelineEditorLibrary.ensure_job_has_default_settings(job)
cfg = job.get_configuration()
```

`ensure_job_has_default_settings` 给的是 **JPG 输出** + `MoviePipelineDeferredPassBase` + `MoviePipelineOutputSetting`。
要无损帧就换掉：

```python
for s in cfg.get_all_settings():
    if s.get_class().get_name().endswith('_JPG'): cfg.remove_setting(s)
cfg.find_or_add_setting_by_class(unreal.MoviePipelineImageSequenceOutput_PNG)
```

**`MoviePipelineOutputSetting` 的真实字段名**（踩过的）：

| 意图 | 字段 | 备注 |
|---|---|---|
| 输出目录 | `output_directory` | 收 `unreal.DirectoryPath` |
| 文件名 | `file_name_format` | 用 `f{frame_number}`；配 `zero_pad_frame_numbers` |
| 分辨率 | `output_resolution` | 收 `unreal.IntPoint` |
| 自定义帧率 | `use_custom_frame_rate` + **`output_frame_rate`** | **不叫 `custom_frame_rate`** |
| 帧步长 | `output_frame_step` | 1 = 逐帧 |
| 自定范围 | `use_custom_playback_range` + `custom_start_frame` / `custom_end_frame` | 不给就用 sequence 的范围 |

### 3.5 渲染

```python
sub.render_queue_with_executor(unreal.MoviePipelinePIEExecutor)   # 传【类】，不是实例
```

- 同族还有 `MoviePipelineInProcessExecutor`。
- **返回是异步的**：调完 `is_rendering()` 可能还是 `False`（下一帧才起），随后 `pie_running` 会变 `True`。
- 进度看 `sub.is_rendering()` 与 job 的 `get_status_message()` / `get_status_progress()`。
- **首次渲染要编 shader，进度会长时间停在 `0.0`** ⇒ 别据此判失败。**可靠判据是输出目录里的文件数**。

---

## 四、自证：帧真的在动

MRQ 渲染的帧是**静态渲染**，材质里用 `Time` 驱动的部分是否推进，必须证明：

- **判据：各帧文件大小两两不同**（内容不同 ⇒ 压缩结果不同）。全都一样 ⇒ `Time` 没走，出来的是一堆重复帧。
- **周期自检**：`帧数 / 帧率 == 材质的 Period`。
  例：`Period = 1.7 s`、30 fps ⇒ 一个周期 **51 帧**；102 帧正好两个周期，且"有内容/全黑"的段也会按 51 帧重复。
- **逐帧客观统计**用 `scripts/frame_stats.py`：亮区均值 RGB、`B−R` 色相偏移、`near_white` 过曝占比、`px>0.9`、亮像素数。
  它一次回答三件事 —— 哪几帧有内容/哪几帧是空档、整条效果实际读成什么颜色、以及**是不是被曝光烧白了**
  （`mean` 停在 (150,170,190) 一带而 `near_white ≈ 0.4` ⇒ 渲染侧过曝，先查场景曝光，别急着改材质）。
- **拼图**用 `scripts/frame_sheet.py`；**格子要 ≥640 px 宽**：把亮而细的特效缩进 320 px 缩略图，
  亮度会被黑底平均掉、变成一条发暗的灰管 —— 那时你判的是拼图 artefact，不是渲染结果。
- **两路 PNG 的编码不一样，看的方式也不一样**（踩过两次）：
  - MRQ 出的帧是**显示参考（sRGB）成品**，**按正常亮度直接看，不要再做 linear→sRGB 提亮**（提亮会发灰发白，被误判成过曝）；
  - `take_screenshot` 落盘的 `.png` 存的是**线性值**，按 sRGB 打开**偏黑**，要先做一次 linear→sRGB 才判得了形态。
  - 判据：同一帧两种看法比，**哪个"暗部还有层次、亮部没过曝"就用哪个**；拿不准时以 `frame_stats.py` 的数值为准。

**编码视频**（本机 `ffmpeg` 在 PATH 里）：

```
ffmpeg -y -framerate 30 -i "<dir>/f%04d.png" -c:v libx264 -pix_fmt yuv420p -crf 18 out.mp4
```

`ffprobe -show_entries stream=nb_frames,r_frame_rate -show_entries format=duration,size` 复核（帧数/帧率/时长要对得上）。

---

## 五、MRQ 出片的语义边界（别搞错它证明了什么）

- MRQ **每帧都是全新渲染，不做跨帧 TAA 累积**。`MoviePipelineAntiAliasingSetting` 的 spatial/temporal sample 是**帧内超采样**，
  不是运动拖影。
- ⇒ **出片 = 设计真相**（形状、时序、层次、消散都是准的）；**≠ 玩家实机观感**（实机有 TAA 拖影）。
- 想要"实机那种拖影"，得开 **`MoviePipelineCameraSetting` 的快门角 + 运动模糊 / 时序采样**（物理正确的版本）。
- 反过来这条很有用：**MRQ 帧里若某个伪影消失了，说明那个伪影是时间性的（TAA），不是资产本身的问题**。
  判别「丑东西是资产还是渲染」就靠这个：同一姿势用 §一 的「关时间累积」法再抓一张 —— 伪影还在 = 资产问题；没了 = 时间性问题。

---

## 六、定格相位：确定性取样

材质停在指定相位，`t = clamp(frac(Time/Period + Phase) / Duty)`，取

```
Period = 1e9   （把 Time 项压掉）
Phase  = p     （frac(p) 恒定 ⇒ 姿态确定）
```

- **只设 `Phase` 不设大 `Period` 是没用的**：`Time` 仍在走，`frac(Time/Period + Phase)` 照样推进 ——
  那批"相位扫描"实际是随机时刻（表现为采样结果毫无规律，还会撞到空档）。
- **采样点选法**：`p_k = Duty · k/(n-1)` ⇒ `t = k/(n-1)`，均匀铺满整个动作段（跳过 `t>1` 的空档）。
- **定格只用于相位定位**，不能拿来判断粗细 / 亮度（原因见 §一）。要判断观感，要么跑起来看（MRQ / PIE），要么先把时间累积关掉。
- 引擎内建节点（`Rotator` 等）**冻不住**（§一.1），定格帧里它会带着 TAA 残影（例：旋转 UV 的同心环）。看不惯就关时间累积，或调掉那个节点的转速。

---

## 七、执行与落点

- 长脚本写 `Saved/MCPScripts/*.py` → `execute_python_file`；中途 `print()` 会随回包回来。
- 现成一份：本 skill 的 `scripts/mrq_render.py`（建 job + 开渲；改顶部 `SEQUENCE` / `LEVEL` / `OUT_DIR` 即可复跑）。
- **编排纪律**：`execute_python_command` / `execute_python_file` 都**同步**且跑在 GameThread 上，
  "每帧截一张图"这种编排在工具面上做不到（一次调用一张图），所以**连续画面的编排必须交给一次 python 调用里完成** —— MRQ 正好满足。
- MRQ job 活在**内存队列**里，不落盘；重启编辑器后要重建（脚本就是为这个准备的）。

---

## 八、自动曝光：让"每一刀都不一样"，也会骗过定格帧

**症状**：同一个时间驱动的效果，**第 1 个周期明显更强、第 2 个周期变弱 / 发糊 / 退化成一堆干净弧线**（细节全没了）。

### 8.1 先排除材质（这一步能省掉一整轮瞎调）

只要效果的每一层都是 `frac(Time/Period + Phase)` 的纯函数、且各层的 `Period` / `Phase` 一致，
**第 1 与第 2 周期的材质输出逐帧完全相同** ⇒ 差异**不可能**来自材质/资产，只能来自**渲染侧随时间变化的状态**。

### 8.2 两步定位

1. **在 MRQ 出片里也复现吗？** MRQ 每帧全新渲染、**没有跨帧 TAA 累积**（§五）。
   - **也复现** ⇒ **不是 TAA**，查曝光（本节）。
   - **只在实时/PIE 里复现** ⇒ 是时间累积，见 §一。
2. **同帧号跨周期比亮度**（`f0009` vs `f0063` 之类）。实测过一次：两周期峰值差 **1.7 倍**（第 2 刀只有 58%）= 实锤。

### 8.3 机制

绝大多数特效都有"空白期"（实测：效果只占 55% 周期，45% 近乎全黑）。
空白期把**眼睛适应（eye adaptation）**的曝光顶高 ⇒
**第 1 刀是在高曝光下渲染的（亮、细节都在）**；随后曝光向整周期平均值收敛 ⇒ 第 2 刀变暗；稳态停在低曝光 ⇒ 只剩最亮的几何带。

**它还会骗过定格帧**：如果定格前刚把姿态设成"空相位"（画面全黑），曝光会被顶到最高，
那一张定格图会**显著偏亮、细节虚多**，与玩家看到的稳态完全不是一回事 —— 这是"定格帧看起来比实机好"的第二个来源（第一个是 §一 的 TAA）。

### 8.4 修法：给关卡放一个 unbound 的 PostProcessVolume，把曝光锁死

```python
vol = EditorActorSubsystem.spawn_actor_from_class(unreal.PostProcessVolume, loc, rot)
vol.set_editor_property('unbound', True)
st = vol.get_editor_property('settings')
st.set_editor_property('override_auto_exposure_min_brightness', True)
st.set_editor_property('auto_exposure_min_brightness', EV)
st.set_editor_property('override_auto_exposure_max_brightness', True)
st.set_editor_property('auto_exposure_max_brightness', EV)   # min == max ⇒ 锁死
st.set_editor_property('override_bloom_intensity', True)
st.set_editor_property('bloom_intensity', 0.7)
vol.set_editor_property('settings', st)
```

- **EV100 语义：值越低越亮**（实测：`0` 偏暗、`-4` 明显更亮）。定标只能靠眼睛，一次一档。
- **PP 体积必须存进关卡**（`save_current_level`）——否则 MRQ 从磁盘加载关卡时看不到它。
- 会话级 cvar 版**只用于临时验证、且不落 ini**：`r.EyeAdaptation.SpeedUp 0` + `SpeedDown 0` 冻结曝光，恢复用 `3` / `1`（UE 默认）。
  **别把它当修复留下** —— 冻结会把画面按死在某个曝光值上、细节全被压掉（比原问题更糟）。

### 8.5 验收（闭环）

改完**重渲 MRQ**，比同帧号跨周期的亮度，应落在几个百分点以内。
实测：修前 `f0009=0.0649` vs `f0063=0.0378`（58%）；加锁后 `0.0592` vs `0.0599`（101%）。

### 8.6 纪律

- **判亮度 / 判暗部细节，不要用定格帧**（§一 + 本节），要用"跑起来抓"或 MRQ 出片。
- **展示/演示场景默认就该锁曝光** —— 黑底特效场景的自动曝光必然被空白期牵着走。

---

## 九、本 skill 的 `scripts/`（改顶部常量即可复跑）

一轮完整出片 = 下面四步各跑一次，**前一步的产物是后一步的输入**：

| 脚本 | 跑在哪 | 做什么 |
|---|---|---|
| `scripts/mrq_render.py` | **编辑器** python（`execute_python_file`） | 清输出目录 → 建 MRQ job → 出 PNG 帧序列；顶部改 `SEQUENCE` / `LEVEL` / `OUT_DIR` / `RESOLUTION` / `FPS` |
| `scripts/wait_mrq_frames.py` | **宿主** python | 盯输出目录的帧数等渲染结束（进度文本不可靠，见 §3.5） |
| `scripts/frame_sheet.py` | **宿主** python | 把帧拼成带帧号的相位图（默认取一个周期铺满 15 格），格子默认 640×360 |
| `scripts/frame_stats.py` | **宿主** python | 逐帧客观统计：亮区均值 RGB / `B−R` / `near_white` / `px>0.9` / 亮像素数 |

```bash
# 编辑器侧
execute_python_file(file_path="<...>/scripts/mrq_render.py")
# 宿主侧（本机 python 带 PIL + numpy）
python scripts/wait_mrq_frames.py E:\out\seq 102
python scripts/frame_sheet.py     E:\out\seq E:\out\sheet.png
python scripts/frame_stats.py     E:\out\seq
ffmpeg -y -framerate 30 -i "E:\out\seq\f%04d.png" -c:v libx264 -pix_fmt yuv420p -crf 18 out.mp4
```

四个脚本都只读目录/内存队列，**不改资产**；`mrq_render.py` 里那次 `shutil.rmtree` 是刻意的
（旧帧留着会让"渲染没起来"看起来像成功），所以 `OUT_DIR` 一定要指向专用目录。

