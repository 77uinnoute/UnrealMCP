---
name: unreal-video-capture
description: "UE5（UnrealMCP）里把「效果动起来的样子」拿到手的方法。**首选：外部逐窗口抓帧器**（`PrintWindow(PW_RENDERFULLCONTENT)` 只抓 UE 窗口 —— 被别的窗口盖住、没有焦点、不置顶都能抓到；配 `inject_key` 驱动游戏 ⇒ 全自动录真 PIE gameplay，实测 23–29 fps）——因为引擎内**没有可脚本化的实时录屏**（`StartMovieCapture` 已弃用；legacy `UMovieSceneCapture` 的类在 python 里有但**没有开始入口**，`LevelEditorPlaySettings` 未暴露；桌面抓取 `ddagrab` 录的是整个桌面；MRQ 的 PIE 会话不接管关卡 GameMode 且会丢运行时状态）。**淘汰/备用**：Movie Render Queue 帧序列（附录 A：插件启用、相机/LevelSequence/相机切轨/job 的 API 与字段名、自证帧在动、语义边界=无跨帧 TAA）、定格相位取样（附录 B：Period 极大 + Phase 定值）。另含判据纪律：定格帧 ≠ 实机观感（TAA 跨帧累积 + 内建节点冻不住）、结论必须来自同一条渲染路径、`take_screenshot` 的 PNG 是线性值且带 alpha（差分前要 `format=gray`，否则全黑）、自动曝光会让「每一段不一样」。触发场景：要从 UE 出视频/帧序列、要真·实时 gameplay 录像、想按指定相位做确定性对照、`unreal.MoviePipeline*` 类在 python 里不存在、MRQ 渲染进度长时间停在 0、判断某个伪影是资产问题还是时间性渲染问题（TAA）、发现「同一个效果第 1 刀明显、第 2 刀变弱/发糊/退化成一堆干净弧线」、要给展示场景锁曝光、`ffmpeg ddagrab` 录到了别的窗口、UE 窗口最小化后抓到黑帧，或在 `execute_python_*` 里调用 MoviePipelineQueueSubsystem / LevelSequence / CameraActor / PostProcessVolume 之前。"
metadata:
  version: "2.0.0"
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
| **真·实时 gameplay 录像（含 UI / TAA / 真 GameMode）** | **外部逐窗口抓帧器（§一）** | 引擎内那几条（§三）：弃用的 `StartMovieCapture`、不可脚本化的 legacy capture、桌面抓取（`ddagrab` 抓整个桌面）、MRQ（不接管关卡 GameMode） |
| **效果动起来是什么样**（形状 / 时序 / 层次；可逐帧比对、离线、可出 EXR） | 附录 A：MRQ 帧序列 | 逐次 `take_screenshot` —— 一次一张、相位不可控、会撞空档 |
| **指定相位的确定性对照**（第 k 帧长什么样） | 附录 B：材质定格相位（Period 极大 + Phase）+ 截图 | 让动画跑着抓 —— 抓到的相位是随机的 |
| **某个伪影是资产问题还是渲染问题** | §二.1：同姿势换抗锯齿法各抓一张 | 只看一张就下结论 |

**纪律：结论必须来自同一条渲染路径。** 编辑器视口 / PIE / MRQ 是**三条不同的渲染路径**（AA 与曝光设置、是否跨帧累积、是否离线全渲都不同），
别拿一条的结果推断另一条。要跨路径对比，先让编辑器视口 pilot 到同一台相机（`pilot_level_actor`），再比画面。

---

## 一、真·实时录像：外部逐窗口抓帧器

**要录"我的游戏这一段到底什么样"，就用这个**（不用 MRQ、不用改 GameMode、不置顶、不抢焦点）。

### 1.1 为什么是"抓窗口"而不是"抓桌面"

- `ffmpeg -f lavfi -i ddagrab`（桌面复制）确实能录（实测 1920×1080@30 可用），但**录的是整个桌面**：谁在最上层就录谁（实测被一个前台聊天窗口盖住，就把那个窗口录了进去）。要干净只能把游戏窗口置顶 —— 属于接管别人的桌面，**别默认这么干**。
- **`PrintWindow(hwnd, hdc, PW_RENDERFULLCONTENT=2)`** 向 DWM 要**该窗口自己的合成结果** ⇒ 被盖住 / 无焦点 / 不置顶都能抓到内容。
  **D3D/DWM 窗口必须带 `PW_RENDERFULLCONTENT`**，否则拿到全黑（普通 BitBlt 不行）。

### 1.2 抓帧器（约 40 行，PowerShell + `Add-Type` C#）

`GetWindowRect`（尺寸）→ `new Bitmap(w,h)` → `Graphics.GetHdc()` → `PrintWindow(hwnd, hdc, 2)` → `ReleaseHdc` → `Bitmap.Save(..., ImageFormat.Jpeg)`；每帧按目标帧率 sleep，循环 N 秒；结束时打印**实测帧率**。

- **实测（编辑器窗口 1936×1048）**：**23–29 fps**（JPEG）。**用 JPEG，不要 PNG** —— 同尺寸 PNG 只有 ~3 fps，差一个量级。
- 踩坑：C# 里是 **`Graphics.GetHdc()`**（写成 `GetHDC` 编译不过）；**窗口不能最小化**（最小化后抓到黑帧/垃圾）；窗口在屏幕上的位置会变，**要裁剪到目标视口**（编辑器里视口只占一块，且视口顶部还有一排 overlay 小图标，裁剪起点往下让开 ~40 px）。

### 1.3 一条完整片子的流程

1. 起 PIE（`start_pie`），必要时把**相机**调成看得清的距离（只动视点，别钉被观察对象的状态 —— 录的就是出厂行为）；
2. 后台起抓帧器：`powershell -NoProfile -File <skill>\scripts\ue_window_grab.ps1 -Seconds 40 -Fps 30 -OutDir Saved\rec`（用 `run_in_background`；`-Seconds` 给足，两次 MCP 调用之间可能隔几十秒）；
3. 期间用 `inject_key(key, event)` 编一套表演（按下的键跨调用有效；成对的"松开+按下"合并到一次响应里更紧凑）；
4. 合片：`<skill>\scripts\frames2video.bat out.mp4 <实测帧率> Saved\rec f` —— **用抓帧器打印的实测帧率**，不要用你请求的那个 fps；
5. 总览图：`<skill>\scripts\tile_images.bat sheet.png Saved\rec <每 N 帧> <格数> f "crop=W:H:X:Y"`（裁窗怎么定见 §一.2 的坑；不传 crop 就是整帧）。

实测产物一次：1040×590、29 fps、41 s 的 PIE 录像（直行 → 90° 转 → 180° 转 → 停），含撞墙被挡的真实过程。

---

## 二、判据与两个常见坑

### 2.1 定格帧 ≠ 实机观感（两条独立原因）

1. **冻结只冻住你冻的那条链**：用 `Period` 冻结时只有走 `frac(Time/Period + Phase)` 的部分被冻住，而**引擎内建节点内部直接用 `Time × Speed`**（`MaterialExpressionRotator` 最常见）⇒ 定格时它还在动。
2. **TAA / TSR 是跨帧累积器**：动起来时把最近约 10 帧的姿势叠在一起（细带被拖宽、更亮、带尾迹），定格时收敛 ⇒ **静止帧永远比实机"瘦"**。

**判别法**：同一定格姿势**连截两张** —— 某块区域（粒子、旋转 UV）在变而轮廓不变 = 第 1 条；两张完全一致、但跑起来不一样 = 第 2 条。
**看单帧真值**：`SystemLibrary.execute_console_command(world, 'r.AntiAliasingMethod <0|1|2|4>')`（None/FXAA/TAA/TSR；**会话级、不落 ini，改完改回**）。

### 2.2 自动曝光会让"同一效果的每一段都不一样"

- **症状**：第 1 段明显更强，第 2 段变弱/发糊/退化成干净弧线。**先排除材质**：若每层都是 `frac(Time/Period+Phase)` 的纯函数且 Period/Phase 一致，两段的材质输出逐帧相同 ⇒ 差异只能来自渲染侧随时间变化的状态。
- **机制**：效果的空白期把眼睛适应（eye adaptation）的曝光顶高 ⇒ 第 1 段在高曝光下渲染；随后向整周期均值收敛 ⇒ 变暗。**它还会骗过定格帧**（定格前若刚把姿态设成"空相位"（全黑），曝光被顶到最高，那一张显著偏亮、细节虚多）。
- **修**：给关卡放 **unbound 的 PostProcessVolume** 把曝光锁死（`override_auto_exposure_min_brightness/max_brightness` 都打开、`auto_exposure_min_brightness == max_brightness`，顺手 `bloom_intensity`）；**PP 体积要存进关卡**。EV100 语义：**值越低越亮**，定标只能靠眼睛。会话级 cvar（`r.EyeAdaptation.SpeedUp/SpeedDown 0`）只用于临时验证，**别当修复留下**。
- **纪律**：判亮度/暗部细节**不要用定格帧**；展示/演示场景默认就该锁曝光（实时录像同样吃这条）。

### 2.3 两种 PNG 的编码不一样，看的方式也不一样

- `take_screenshot` 落盘的 PNG 存的是**线性值**（按 sRGB 打开偏黑，要先 linear→sRGB）**且带 alpha** ⇒ 做差分/叠加前先 `format=gray`（或 `format=rgb24`），否则 `blend=all_mode=difference` 出来**全黑**（A 通道差 = 0 ⇒ alpha 0 ⇒ 透明），会被读成"两张图一模一样"（实测踩过，因此误判某个动画没生效）。
- MRQ 出的帧是**显示参考（sRGB）成品**，**按正常亮度直接看，不要再提亮**（提亮会发灰发白，被误判成过曝）。
- 判不准时以逐帧统计的数值为准（见附录 A 的 `scripts/frame_stats.py`）。

---

## 三、引擎内那几条路为什么不行（都实测过）

- `StartMovieCapture` / `StopMovieCapture`：Matinee 遗留命令，UE5 已弃用（社区一致口径：别用）。
- **legacy `UMovieSceneCapture` 无法从脚本启动**：python 里**有**这些类（`MovieSceneCapture` / `MovieSceneCaptureSettings` / `VideoCaptureProtocol` / `MovieSceneCaptureProtocolState` / `MovieSceneCaptureEnvironment`），但 `dir(unreal.MovieSceneCapture)` 里只有 `settings` / `get_image_capture_protocol(…)` / `set_image_capture_protocol_type(…)` / `use_separate_process` / `close_editor_when_capture_starts(…)` …（括号的三个是**可调用成员**，其余是字段），**没有任何"开始录制"的入口**；`LevelEditorPlaySettings` 也没暴露（`dir(unreal)` 里搜 `PlaySettings` 为空）⇒ 只能人手点 UI。
- **桌面抓取（`ddagrab`）**：见 §一.1 —— 录整个桌面，需要置顶才干净。
- **MRQ**：见附录 A.4 的两条阻塞（不接管关卡 GameMode、PIE 世界是编辑器副本且运行时状态会丢）。

---

## 附录 A（淘汰/备用）：MRQ 帧序列

> 现在只在需要「**离线、可逐帧比对、可出 EXR 的确定性帧序列**」时用。要录实机 → 回 §一。

**A.1 启用插件**：`Engine/Plugins/MovieScene/MovieRenderPipeline/MovieRenderPipeline.uplugin` 的 `EnabledByDefault` 是 `false`。
症状：python 里 `unreal.MoviePipeline*` 全不存在（`dir(unreal)` 搜不到），而老版 `MovieSceneCapture*` 反而在 —— 别误判成"UE 没这个能力"。
做法：`.uproject` 的 `Plugins` 里加 `{ "Name": "MovieRenderPipeline", "Enabled": true }` → **重启编辑器**（插件只在启动时加载；先确认 `unreal.EditorLoadingAndSavingUtils.get_dirty_map_packages()` 为空；重启后会落在上次打开的图，显式 `unreal.LevelEditorSubsystem.load_level(…)` 切回）。

**A.2 建管线（顺序固定）**

- **相机**用 `unreal.CameraActor`（直接有 `field_of_view`，省掉 filmback 换算），别用 CineCameraActor。
- **LevelSequence**：`ls.set_display_rate(unreal.FrameRate(30,1))` + `set_playback_start_seconds/end_seconds`。
- **相机切轨**（三个易错点）：`SE.add_track(ls, unreal.MovieSceneCameraCutTrack)`（**不存在** `SE.add_master_track(…)` 这个名字）；`sec.set_range(0, N)` 收 **int 帧号**不收 FrameNumber；`sec.set_camera_binding_id(ls.get_binding_id(ls.add_possessable(cam)))`。**重跑前先查再建**（`unreal.MovieSceneSequenceExtensions.find_tracks_by_type(…)` / `unreal.MovieSceneTrackExtensions.get_sections(…)` / `unreal.MovieSceneSequenceExtensions.get_possessables(…)` —— 属主是**引擎的 MovieScene script extensions**，不是本插件的命令），否则叠出第二条切轨/第二个绑定。没有切轨 MRQ 不知道用哪台相机。
- **job**：`queue.delete_all_jobs()` → `allocate_new_job(unreal.MoviePipelineExecutorJob)` → 设 `job_name` / `sequence` / `map`（`SoftObjectPath`）→ `MoviePipelineEditorLibrary.ensure_job_has_default_settings(job)`（默认给 **JPG** + DeferredPass + OutputSetting）；要无损就 remove 掉 `*_JPG` 再 `find_or_add_setting_by_class(unreal.MoviePipelineImageSequenceOutput_PNG)`。

**`MoviePipelineOutputSetting` 的真实字段名**（踩过的）：

| 意图 | 字段 | 备注 |
|---|---|---|
| 输出目录 | `output_directory` | 收 `unreal.DirectoryPath` |
| 文件名 | `file_name_format` | 用 `f{frame_number}`；配 `zero_pad_frame_numbers` |
| 分辨率 | `output_resolution` | 收 `unreal.IntPoint` |
| 自定义帧率 | `use_custom_frame_rate` + **`output_frame_rate`** | **不叫 `custom_frame_rate`** |
| 帧步长 | `output_frame_step` | 1 = 逐帧 |
| 自定范围 | `use_custom_playback_range` + `custom_start_frame`/`custom_end_frame` | 不给就用 sequence 的范围 |

**A.3 渲染**：`sub.render_queue_with_executor(unreal.MoviePipelinePIEExecutor)`（传**类**不是实例；同族还有 `MoviePipelineInProcessExecutor`）。
返回是**异步**的（`is_rendering()` 下一帧才起，随后 `pie_running` 变 `True`）；**首次渲染要编 shader、进度会长时间停在 0** ⇒ **可靠判据是输出目录里的文件数**。
**从外面注入输入的窗口很窄**：实测 3 s（90 帧）的 job **不到 10 s** 就渲完，随后的 `inject_key` 落在渲染之后、帧里毫无反应；12 s（360 帧）才有余量。渲染完清帧：360 帧 PNG ≈ **618 MB**。
**job 活在内存队列里、不落盘** ⇒ 重启编辑器要重建；重跑前清输出目录（旧帧留着会让"渲染没起来"看起来像成功）。

**A.4 语义边界（别搞错它证明了什么）**

- MRQ **每帧全新渲染、不做跨帧 TAA 累积**（`MoviePipelineAntiAliasingSetting` 的 spatial/temporal sample 是**帧内**超采样）⇒ **出片 = 设计真相 ≠ 实机观感**；要"实机那种拖影"得开 `MoviePipelineCameraSetting` 的快门角/运动模糊。
- 反过来很有用：**MRQ 帧里伪影消失 ⇒ 那个伪影是时间性的（TAA）**（配合 §二.1 的关时间累积法判"资产问题还是渲染问题"）。
- **PIE 会话不接管关卡 GameMode**：`MoviePipelinePIEExecutor.cpp:167` 只在 job 里存在 **Movie Graph** 的 `MovieGraphGlobalGameOverridesNode` 时才设 `Params.GameModeOverride` ⇒ 经典 job 用**项目默认** GameMode。实测：同一张图，正常 PIE 的 pawn 是关卡 WorldSettings 指定的类，MRQ 的 PIE 世界里 `unreal.GameplayStatics.get_player_pawn(world, 0)` 拿到的是 **`DefaultPawn`**。⇒ 要么走 Movie Graph、要么临时改项目默认 GameMode、要么让被测对象**由 LevelSequence 自己驱动**。
- **PIE 世界是编辑器世界的副本 ⇒ 运行时生成的状态不保证过来**：实测 ISM 实例复制过来了，而一张 `CreateTransient` + `UpdateTextureRegions` 的**运行时贴图丢了**（画面变成"物件站在空底上"，且不报错）。

**A.5 自证帧真的在动**（静态渲染下 `Time` 是否推进）

- **各帧文件大小两两不同**；全都一样 ⇒ `Time` 没走、出来是一堆重复帧。
- **周期自检**：`帧数 / 帧率 == 材质的 Period`（例：Period 1.7 s、30 fps ⇒ 51 帧/周期）。
- **拼图格子 ≥640 px 宽**：把亮而细的特效缩进 320 px 缩略图，亮度会被黑底平均掉、变成一条发暗的灰管 —— 那时你判的是拼图 artefact，不是渲染结果。

**A.6 脚本**（`scripts/`，改顶部常量复跑；只读目录/内存队列，不改资产）

| 脚本 | 跑在哪 | 做什么 |
|---|---|---|
| `mrq_render.py` | 编辑器 python（`execute_python_file`） | 清输出目录 → 建 job → 出 PNG 帧序列；顶部改 `SEQUENCE`/`LEVEL`/`OUT_DIR`/`RESOLUTION`/`FPS` |
| `wait_mrq_frames.py` | 宿主 python | 盯输出目录帧数等渲染结束（进度文本不可靠） |
| `frame_sheet.py` | 宿主 python | 帧拼成带帧号的相位图（格子默认 640×360） |
| `frame_stats.py` | 宿主 python | 逐帧客观统计：亮区均值 RGB / `B−R` / `near_white` / `px>0.9` / 亮像素数 |

---

## 附录 B（备用）：定格相位取样

材质停在指定相位，`t = clamp(frac(Time/Period + Phase) / Duty)`，取 `Period = 1e9`（压掉 Time 项）+ `Phase = p`（`frac(p)` 恒定 ⇒ 姿态确定）。

- **只设 `Phase` 不设大 `Period` 是没用的**：`Time` 仍在走，那批"相位扫描"实际是随机时刻（结果毫无规律、还会撞空档）。
- **采样点**：`p_k = Duty · k/(n-1)` ⇒ `t = k/(n-1)`，均匀铺满动作段（跳过 `t>1` 的空档）。
- **只用于相位定位**，不能判粗细/亮度（§二.1、§二.2）；判观感要么跑起来看，要么先关时间累积。
- 引擎内建节点（`Rotator` 等）**冻不住**，定格帧里它带 TAA 残影（例：旋转 UV 的同心环）。
