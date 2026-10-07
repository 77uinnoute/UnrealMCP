---
name: unreal-retarget-authoring
description: "UE5（UnrealMCP）里把「A 骨架的动画搬到 B 骨架」这件事做对、并排查姿态偏差的 Skill：FK 通道（retarget chain）与 IK 通道（goal）/根通道（retarget root）/参考基准（retarget pose）的分工与对应旋钮、求解器实际的旋转/位移/缩放数学（含 translation_mode=None 导致整体上下起伏丢失）、非标准骨架（MMD/PMX、D 变形骨、骨轴 180° 翻转、命名与真实父链不一致、导入骨架的 track 单位=米且『世界向上』落在父骨局部 −Y）为什么自动重定向不可用而必须手工建链（链名照抄源、逐骨目视对应、末端关节不选、躯干必须连续包含中间骨）、离线烘焙与运行时（Retarget Pose From Mesh）的取舍与各自硬约束、烘焙后补通道（把源骨盆竖直起伏按身高比单通道移植回目标）、以及姿态偏差（踮脚、头/胸后仰、末端不跟随、走路没有上下起伏）的分相位数据化排查法（禁用『旋转量』指标，改用相对父骨局部旋转 + 与骨轴约定无关的几何量 + 视口）。触发场景：准备操作 IK Rig（链 start/end bone、add_new_goal、set_retarget_chain_goal、retarget pose 偏移）、IK Retargeter（auto_map_chains、auto_align_all_bones、chain settings 的 enable_fk/rotation_mode/translation_mode/enable_ik/blend_to_source、root settings 的 scale_horizontal/vertical）、批量重定向（IKRetargetBatchOperation.duplicate_and_retarget）、运行时节点 Retarget Pose From Mesh（IKRetargeterAsset / bUseAttachedParent / SourceMeshComponent）、写骨骼平移轨道（set_bone_track_keys / add_bone_track 的轴向与单位标定）、以及任何涉及『把 Mixamo/Mannequin/MMD 动画重定向到自定义骨架、自动重定向失败、链断、踮脚尖、抬不起脚、头/胸后仰、脚不贴地、烘焙结果没有上下起伏/跳跃高度丢失、烘焙还是运行时』的任务。外部文件导入走 unreal-asset-pipeline Skill。"
metadata:
  version: "1.1.0"
  upstream: unreal-asset-pipeline
  downstream: unreal-animation-authoring
---

# UnrealMCP 重定向（IK Rig / IK Retargeter）Skill

适用：本仓库 `Plugins/UnrealMCP`（bridge `127.0.0.1:55557`，MCP server 名 `unrealMCP`）。
目标：把源骨架动画稳定地搬到目标骨架，并用**分相位、抗约定干扰**的数据证明姿态对上了。

**环境注意**：改 `Content/Python/**` 需重启 unrealMCP server；改插件 C++ 需编译 + 重启编辑器；**PIE 运行中 `unreal.load_asset` 拿 rig/retargeter 可能返回 None** → 改 rig/retargeter 一律先停 PIE，改完再起 PIE 验证（处理器有缓存，热改不一定生效）。

---

## 〇、四个通道，别混

| 通道 | 载体 | 管什么 | 主要旋钮 |
|---|---|---|---|
| **FK** | IK Rig 的 **retarget chain**（start/end bone） | 骨到骨的旋转/位移搬运（姿态主体） | `enable_fk`、`rotation_mode`、`rotation_alpha`、`translation_mode`、`translation_alpha` |
| **IK** | IK Rig 的 **goal**（末端骨）+ 链的 IK 设置 | 末端（手/脚）位置贴合、pole vector | `enable_ik`、`blend_to_source`（**0 = IK 不干活**）、`static_offset`、`static_rotation_offset` |
| **根** | IK Rig 的 **retarget root**（"初始骨骼"，取胯部/腰上一级）+ retargeter 的 root settings | 根的位移/旋转/缩放 | `ScaleHorizontal/Vertical`、`TranslationAlpha`、`RotationAlpha`、`BlendToSource` |
| **基准** | 两侧的 **retarget pose**（存在各自 IK Rig 里） | 上面三者的"零点" | `rc.set_rotation_offset_for_retarget_pose_bone`、`auto_align_bones`、`reset_retarget_pose` |

一句话：**链 = FK 通道；goal = IK 通道；retarget root = 根通道；retarget pose = 三者的共同基准；`blend_to_source` = IK 是否真的干活。**

---

## 一、求解器实际在算什么（IKRetargetProcessor）

**每根骨（global/component 空间）**：
```cpp
RotationDelta = SourceCurrentRotation * SourceInitialRotation.Inverse();   // 源相对"源 retarget pose"的增量
OutRotation   = RotationDelta * TargetInitialRotation;                     // 施加到"目标 retarget pose"上
```
- ⚠️ **retarget pose 的旋转偏移是「独立 op」，不参与烘焙的默认通道**：它由 `FIKRetargetAdditivePoseOp`
  （`RetargetOps/RetargetPoseOp.cpp:66-127`）施加，受 `Settings.PoseToApply`（姿势名）+ `Settings.Alpha` 门控
  （`IKRetargetProcessor.cpp:63` 才把它读进 FK）。**op 栈里没有这个 op ⇒ 偏移对烘焙结果完全无效**。
  **判据（一行）**：改完偏移重烘一条 clip，量被改骨**及其子骨**的朝向增量 —— 子骨不动 / 增量远小于写的角度
  = 偏移没进通道，**别再调参数**。要修烘焙产物只能写进 clip 本身或 AnimGraph；运行时节点同理。
- **纯 roll 的度量要选对轴**：`R = W_anim * W_rest⁻¹`，`align = q_from_to(dr, da)`；
  `align⁻¹ * R` 是 swing-first（残余轴在 **rest 方向** `dr`），`R * align⁻¹` 的残余轴才在**动后方向** `da`。
  把 twist 投到错的轴上会让「纯 roll」读成≈0（本项目实测 11.7° 的 roll 只读出 0.4°，白跑数轮）。
- `SourceInitialRotation` / `TargetInitialRotation` 来自**两侧的 retarget pose，而不是 rest/ref pose**。"目标 retarget pose 没对齐"会变成**每帧恒定**的姿态偏差（踮脚、后仰）——这是 A 类问题的指纹。
- 位置：`translation_mode` = `None`（用目标 retarget pose 的局部偏移，刚性）/ `GloballyScaled` / `Absolute`；缩放 `OutScale = 源 + (目标初始 − 源初始)`。
  - **实测推论：`None`（默认）下目标骨的本地平移直接取目标 retarget pose 的偏移 = 常数** → 源动画里"骨盆带着全身上下起伏（bob）/跳跃高度"这类**整体竖直位移搬不过来**（本项目：54 个烤肉动画 pelvis 竖直范围全 0.00）。**烘焙和运行时用的是同一套 profile 语义，所以两条路一起丢**——"烘焙没有起伏"不是烘焙的锅，是通道语义（见 §四 补通道）。
  - **但这条只对"骨盆不是 retarget root"的 rig 成立**：retarget root 每帧被根通道写全局变换（`FRootRetargeter::DecodePose`），若 root 就是 `pelvis`，起伏**已经被搬到位**（`RTG_Ganyu` 属此列，实测产物 y 范围 6.3033 ≈ 源 6.679 × 比 0.9437）。**先按 §四 开头那条判据量一次，别默认移植。**
- 链上如何取源数据（`rotation_mode`）：`Interpolated`（按**链上归一化参量**采样源链，骨数不等也能分配）/ `OneToOne` / `OneToOneReversed` / `None`（整链刚性跟父，压 180° 翻转时用）。
  - 实测：**骨数相等时 `Interpolated` 与 `OneToOne` 输出逐字节相同**，改它没用；只有链的**骨集**变了才有变化。
  - **但骨数不等时 `OneToOne` 会丢源链的多余骨**（1:1 只配前 N 对）→ 丢掉的是源链**上段/远端的累积旋转**，症状出现在**远端**：本项目实测 MMD 脊柱只有 3 根（`spine_01..03`）而 Manny 源链是 `spine_01→spine_05`（5 根），用 `OneToOne` 时"肩不跟着臂走 + 头左右歪"（头世界朝向残 3.9°、head→eyes 侧向摆 0.79 cm），改成 **`INTERPOLATED` 后 head 世界偏差 0.0°、clavicle/upperarm/hand 与源逐值一致（8.7/19.3/31.7°）**。**判据**：逐骨比 `AnimPose WORLD` 旋转相对 F0 的偏差 vs 源同名骨（远端骨差得多 = 分配模式/链跨度问题，而不是 retarget pose 问题）；**修法优先级**：先看源链跨度（`IKRigController.get_retarget_chains()` 查源 rig）→ 目标链覆盖同一解剖区段 → `rotation_mode=INTERPOLATED`。
  - **腿链末端跟源保持一致**：源 `IK_Mannequin` 的 `LeftLeg` 是 `thigh_l→ball_l`（含脚尖）。本项目一度按另一模型的经验裁到 `foot_l`，改回 `ball_l|r` 后腿的每一项与源**逐值相同**（thigh 37.8° / calf 80.0° / foot 77.9° / ball 99.7°）。"末端别选 ball"这条只适用于该骨 rest 方向病态（如朝上 8 cm）的模型，**先量 `ball` 的 rest 朝向再决定**。
- 处理每条链前，求解器会把**整条源链重对齐到目标父链当前姿态**，所以"父链没重定向"不会累成 skew。
- 根：`FRootRetargeter::DecodePose` —— 位置按身高归一化 × `ScaleVertical/Horizontal` + offset + alpha；旋转 `ΔW_src × W_tgt_rest`。

---

## 二、非标准骨架（MMD/PMX 是重灾区）

**症状**：IK Rig 打开就提示"未找到模板 / 无法自动重定向"，或自动生成后**某些骨整段不动 / 姿态系统性偏**。

**根因清单**（本项目全踩过）：
1. **命名与真实父链不一致 → 链断**。自动生成只认"像人形命名"的连续路径，遇到未改名骨就停。实例：真实路径 `head ← neck_01 ← spine_04 ← 上半身1 ← spine_03 ← …`，`spine_04` 的父级是**没改名的** → 自动生成的 `Spine = spine_01→spine_03`（源是 5 根）、`Neck = neck_01`（源 2 根），**`spine_04` 完全不在任何链里** → 胸/头整段后仰（实测头相对 rest 偏 24.7° vs 源 10.0°）。
   修法：`set_retarget_chain_end_bone('Spine','spine_04')` 把中间骨包进链，再 `auto_map_chains` + `auto_align_all_bones`。
   **反面收益（实测）**：若把骨架**标准化成 UE 人形命名**（见 `unreal-asset-pipeline-case-bone-rename` 的 ASCII 标准化流程），`apply_auto_generated_retarget_definition()` 会**直接返回 True 并自动生成 20 条链**（Root / Spine / Neck / Head / 四肢 + 双手各 5 条手指链）——手工建链那一段就省了。标准化的唯一代价是与按名匹配的 MMD/VMD 动作库脱钩；不用 MMD 动作库时值得做（本次 Ganyu 就是先手工建 10 条链、后标准化重建成自动 20 条链）。**标准化后唯一要手工收的一处**：官方模板会把腿链末端定到 `ball_l|r`（脚尖），按 §二.2 改成 `foot_l|r` 防踮脚。
2. **末端关节别选**。实例：腿链含 `ball_l`，而该骨 rest 方向在本模型里**朝上 8cm**（`head z=0 → tail z=0.08`）→ 脚在链上的分配被拉歪，**脚的相位变化搬不过来**：源脚底倾角随相位 12.7°→42.8°，目标恒定 13–17° = 踮脚。
   修法：`set_retarget_chain_end_bone('LeftLeg','foot_l')`（腿链改为 `thigh→foot`）。修完残差 0.9°，脚底法线夹角 25.9°→**2.87°**。
3. **骨轴 180° 翻转**：MMD 部分骨（含骨架根）rest 轴与 UE 相反 → 根链 `rotation_mode=NONE` 压住。**但这条只对"骨盆父骨链在原点"的骨架有效**（见下一条）。
3b. **骨架内嵌 ×100 scale → 重定向输出整副骨架塌成一点**（本项目 Ganyu 事故，排查花了很久，记住指纹）：
   - **症状**：烘焙/运行时输出的姿态"碎成一团"——所有骨的世界坐标互相差 < 1–2 cm（`AnimPoseExtensions.get_anim_pose_at_frame` + `WORLD` 逐帧量，各帧还几乎不动）；或运行时节点输出恒为 rest。
   - **指纹**：某骨的**局部平移是米、世界坐标是厘米**。判据（只读，一条命令定性）：
     ```python
     L, W = AnimPoseSpaces.LOCAL, AnimPoseSpaces.WORLD
     lp = get_bone_pose(rp, 'センター', L).translation   # 内嵌 scale 时: -0.66  (米)
     wp = get_bone_pose(rp, 'センター', W).translation   # 同时: 66.24 (厘米)
     ```
     正常 cm 骨架两个都是厘米量级（Manny pelvis local ≈ 95.9）。**MMD 常见病因**：Blender 侧用"米制数据 + 让 UE 做 m→cm 转换"导出 → UE 把 ×100 放在**根骨/骨架节点**上，骨骼局部平移留在米制。
   - **为什么 Richie 没炸而 Ganyu 炸**：`FRootRetargeter::DecodePose` 写的是目标根骨的**全局**变换（按 `Target.InitialHeight` 归一化 × `ScaleVertical/Horizontal`，再由全局→局部换算）。内嵌 scale 时"全局→局部"算出的局部平移是**厘米量级**（Ganyu 腰 `-24.26`），而骨架运行时期待米（`-0.2426`）→ 根骨被顶 100× → 全身塌。Richie 侥幸：它的骨盆父骨 `root` **在原点**，且 root 链配 `ScaleVertical/Horizontal=0.01`，两者相乘恰好把厘米量级缩回米量级（烘焙 pelvis 局部 `-0.6369` 正确）。Ganyu 的骨盆父链在 66 cm 高（センター→グルーブ2 五根），把 root scale 压到 0.01 只会更烂 → **rig 侧无解**。
   - **修法（唯一干净解）**：模型侧重导一个**局部平移就是 cm 的骨架**（见 `unreal-asset-pipeline` §五：mmd_tools `scale=0.08` → 把 ×100 烤进 mesh 顶点与骨骼 `head/tail` → `export_scene.fbx(global_scale=0.01, apply_unit_scale=False)`），然后 rig 侧**全部用默认**（root scale 1.0、chain `rotation_mode`/`translation_mode` 默认）。同一条 Manny 走路动画，修前输出全塌，修后：腰 z≈86-89、双脚交替（左踝 27→11→27 / 右踝 13→23→12）、头 130-134 —— 一次过。
   - **别在坏骨架上做参数试错**（本项目白烧了 4 轮烘焙 × 4 个参数组合）：先量上面那**一个**局部平移判据，是米制就直接回模型侧重导。
4. **根链布局照抄"骨架根骨"**：Root 链要覆盖**无父骨的那根骨架根**（MMD 系可能是 `全ての親`/`root`），**不要把骨盆本尊放进任何链**（它只当 retarget root）。把骨盆放进 Root 链 + `translation_mode` 一改（如 `GloballyScaled`）就会把它的局部平移交给链解码器写坏（实测：骨盆局部平移被写成 1/100/全零三种坏法）。
4. **D 变形骨 / 付与骨**：UE 没有"付与"机制，皮肤挂在 `ひざD/足首D/…` 上就是死皮（腿不动）。这属于**权重问题**，必须在模型侧把权重并到 FK 骨上（见 `unreal-asset-pipeline`），重定向怎么调都救不了。
5. 手工建链的正确姿势（视频教程同款）：**链名照抄源骨架**；逐骨在大纲/搜索里选，对着视口核；躯干链要**连续覆盖从胯到上胸的所有中间骨**；末端（脚掌/脚尖、手指尖）按需留。
7. **MMD「捩り骨」在 UE 里静止 = 重定向的默认正确结果**（FBX 不携带 PMX 的「付与」）：本项目读 PMX 真值实测：`腕捩` 带 `軸固定`、`腕捩1/2/3` 的付与親是**在链上的 `腕捩`**（比率 0.25/0.50/0.75）、`ひじ` 的 parent 是 `腕捩`；`手捩`/`手捩1..3`/`手首` 同构。即这些旋转**由动画师手转 `腕捩`/`手捩` 产生**，重定向永远不会有这个量 ⇒ 静止就是正确的。
   ⚠️ **但「驱动它 = 错的」只对一种驱动方式成立**，判据只看**驱动源是谁**：源骨取它**已经继承的父骨**时才是二次旋转（本项目实测整条手臂整体位移、看着像"手臂位置错了"）；而**把关节过渡带挂到这些阶梯骨上、源骨取子肢骨**是另一件事（给"0% 跟随"的过渡带补分数跟随），配方见 `unreal-animation-authoring` skill §十。
   读 PMX 付与真值的方法：Blender 里 `pmx = __import__('bl_ext.user_default.mmd_tools.core.pmx', fromlist=['pmx'])` → `pmx.load(path)`（**没有** `Model(path)` / `read()` 这种形态）；`pmx.Bone` 的字段名是 `hasAdditionalRotate` / `hasAdditionalLocation` / `additionalTransform`（= (親 index, 率)）/ `axis` / `isRotatable` —— 写 `grant_parent` / `grant_weight` 会**静默取到 `None`**，会误判成"这个模型没有付与"。
6. **导入骨架的"平移通道轴向 + 单位"不是 UE 约定，写平移轨道前必须先标定**（本项目踩过，写错轴=白干且看起来"没效果"）：
   - 实测（MMD→Blender→FBX）：动画 track 的平移单位是**米**（pelvis rest 局部 `[0, -0.6486, 0]` ↔ 世界 `[0,0,64.856]` cm，比值 100），且**世界向上 = pelvis 父骨 `root`（rest 旋转 = 绕 X −90°）的局部 −Y**，不是局部 Z。
   - 标定法（只读，不猜；`rp` = 目标骨架 ref pose）：
     ```python
     W, L = AnimPoseSpaces.WORLD, AnimPoseSpaces.LOCAL
     R  = get_bone_pose(rp, parent_bone, W).rotation          # 父骨世界旋转（父骨是谁：用 W 空间位移差反推）
     vl = get_bone_pose(rp, bone, L).translation              # 子骨局部平移（父骨系）
     wd = get_bone_pose(rp, bone, W).translation - get_bone_pose(rp, parent_bone, W).translation
     # 解：wd ≈ R·vl × 单位比 → 哪个分量/符号是"世界 +Z"、单位比多少，一次看清
     ```
   - **别拿 `get_bone_pose_for_frame(anim, bone, f, True)` 的 comp 空间 z 判竖直**：本模型里那套 comp 空间给 pelvis 的 z 恒 ≈ 0（竖直根本没落在那），写错轴也"看不出错"。读数与写数必须**同空间同轴**。
   - 平移写回是**局部**（父骨系）：`set_bone_track_keys`（平移写在 `keys[].position=[x, y, z]`） 与 `get_bone_pose_for_frame(..., False)` 同空间，改哪个分量、哪个符号要按上面的标定来。

---

## 三、烘焙（离线）vs 运行时

| | 离线烘焙 | 运行时（`Retarget Pose From Mesh`） |
|---|---|---|
| 入口 | `IKRetargetBatchOperation.duplicate_and_retarget(asset_data_list, src_mesh, tgt_mesh, retargeter, search, replace, prefix, suffix, include_ref)` | AnimBP 节点 `AnimGraphNode_RetargetPoseFromMesh` |
| 输入 | 源 AnimSequence 列表（**必须是 `AssetData` 数组**，传资产对象会 `NativizeProperty` 报错） | 另一个 `SkeletalMeshComponent` 的当前姿态 |
| 产物 | 目标骨架的 AnimSequence（全骨轨 + 根运动），落在**源目录**（脚本再 `EA.rename_asset` 搬走） | 无资产，直接出 Pose |
| 硬约束 | 目标路径同名会 `rename` 失败 → 重烤要换新目录或先清旧 | ① 只在 **game world** 求值（编辑器世界里目标恒为 rest，我实测全 0≠坏）；② 源必须**先于本 anim instance tick**；③ 隐藏源网格要 `VisibilityBasedAnimTickOption=AlwaysTickPoseAndRefreshBones`；④ 字段名是 **`IKRetargeterAsset`**（写错就编译报"未指定重定向器资产"）；⑤ `SourceMeshComponent` 是 pin，默认靠 **`RetargetFrom = ParentSkeletalMeshComponent` 沿组件父链**找源（5.7 前那个布尔 `bUseAttachedParent` 已废弃、写它无效，见 §六）→ 目标 mesh 要挂在源 mesh **组件**下（跨 actor 时 `attach_component_to_component` 会重挂根组件，也能命中）；⑥ **ABP 必须在创建时就绑定目标骨架**（`AnimBlueprintFactory` 先 `set_editor_property('target_skeleton', skel)` 再 `create_asset`）——"先建空 ABP、加节点、编译、**事后再 `set target_skeleton` + 重编译**"的类在 PIE 里会**静默输出 rest pose**：编译 `BS_UpToDate`/无警告、节点属性与能用的 ABP 逐字段一致、anim instance 正常生成、`get_socket_transform` 就是不动。这种类也删不掉（`delete_asset` 恒 False）→ 直接**换个新名字重建**（本项目 `ABP_GanyuRT` → `ABP_GanyuRT2` 才通） |
| 取舍 | 自包含、Persona 可查、能进 BlendSpace/Montage、运行时零开销；源一变就要重烤、资产翻倍 | 源动画即插即用、可运行时调精调（`CustomRetargetProfile`）；每帧开销、双份骨骼/蒙皮、调试更绕 |

**烘焙默认会把"前进位移"写进 retarget root，而不是骨架根骨 ⇒ 根运动读数为 0**（实测，滑步的常见根因）：

- 重定向写的是**目标 retarget root**（本 rig = `pelvis`）的每帧变换，于是**整段走路的前进量落在 `pelvis` 的局部水平分量上**（实测 `pelvis` 局部 Z `0.41→230.21`），而骨架 bone 0（`root`）与其中间父骨（`ctrl_origin`/`master`）**全程静止**。
- 引擎的根运动只认**骨架 bone 0** ⇒ `get_total_root_motion()` 读出来是 **0**，`bEnableRootMotion` 打开也**没有任何东西可消费**；表现是"身体在 1 秒里前移 2.3 m 再跳回"= 滑步/漂移，而"根运动"却是 0 —— 用户口中的"动画和根运动对不上"就是这个。
- **判据（先量，改之前）**：比对**源的 `get_total_root_motion()`** 与产物的（源 `[0,247.79,0]` vs 产物 `[0,0,0]`），再看逐骨的**局部平移范围**是谁在长（`pelvis` 的哪个分量单调增长 = 位移在那）。
- **修法**：逐帧把 retarget root 局部水平分量的**增量**加到**骨架根骨**的局部平移上，并把 retarget root 的水平分量还原成首帧常量（摆动/竖直起伏保留），最后 `set_enable_root_motion(true)`。搬完 `get_total_root_motion()` 的前进分量应 ≈ 该动画的世界前进量、方向与源一致。
- ⚠️ **实测修正（MMD 导入骨架）：位移不一定落在 `pelvis` 上 —— 它可能落在骨盆上方的辅助骨（`ctrl_origin` / `master`），而骨盆带着一条等量反向的"补偿斜坡"把位移抵消掉。** 这种产物的骨盆世界位置**几乎不动、看着就是原地的**，但那是"父骨前进 × 骨盆局部反向"相消的结果。**只改骨盆 = 破坏这个相消**：辅助骨照样每循环飞 4.6 m（走）/ 10.9 m（跑），骨盆反而开始跟着漂（症状：**"相机比角色前进得快、松开按键角色飞过来"**），而且换来的骨盆轨道里还留着一条 47 cm 的**假前后摆动**（真实跑循环只有几 cm）。
  - **原地化的正确判据 = 整条链的 COMP 位移范围**，不是只看骨盆：逐帧列 `root → ctrl_origin → master → center → groove → pelvis` 的 `get_bone_pose(..., WORLD).translation` 范围（注意 `AnimPoseEvaluationOptions` 要显式构造，默认 `bIncorporateRootMotionIntoPose=True` 会把根运动并进姿态，判定时要关掉），谁的水平分量单调增长就是位移所在。
  - **修法**：冻结**承载位移那根骨**的水平分量（辅助骨通常只承载水平位移，**别碰竖直分量**——跳跃高度常常就在骨盆的 local y 上），再把骨盆那条补偿斜坡的**低频成分**一起去掉（`z_new = z[0] + (z - 移动平均)`，保留高频 sway）。验收 = 每条骨的水平范围都是常数，而 bob / 跳跃高度 / 抬脚范围保持原值。
  - **轴映射随父骨 rest 旋转而变（本骨架实测）**：`ctrl_origin` 的父骨是 `root`（rest 单位阵）⇒ 局部 y == 组件 y（水平前后）；`pelvis` 的父骨带 −90° X ⇒ 局部 z ↔ 组件 y（前后）、局部 y ↔ 组件 z（上下，符号相反）。**写轨道前先按父骨 rest 旋转标定哪个局部分量是前后/上下**，否则冻错轴（要么留漂移，要么把跳跃高度冻没）。
  - 另外：模板第三人称的 `RootMotionMode = RootMotionFromMontagesOnly`（移动由代码驱动）⇒ 这种角色**要的就是原地剪辑**，不要把位移留在辅助骨上指望根运动兜底（`root` 骨没动，`get_total_root_motion()` 依旧是 0）。
- **自证别拿合成量**：我第一版拿 `pelvis` 的**世界**位移判"位移有没有搬走"——搬运后它当然还是那么多（root 带着它）⇒ 判据误判、脚本自动翻符号，把动画写成**倒着走**。要量**被写的那根骨**（根骨的世界位移）+ 被搬运对象的**局部**分量。
- **"脚落地"的判据用速度剖面**：支撑相脚的世界速度应接近 0（相对 pelvis 均速设阈值，如 <20%，并数帧数），源/产物应一致（实测 7/58 帧、抬脚 21 cm）。**别用"最低 z 附近的时间窗"**——它会把摆动相一起圈进来，得到假的"支撑相漂移"。

**两条路用同一个求解器 → 观感应当一致**。所以"运行时好看、烘焙难看"通常是**烘焙时叠了额外处理**（见 §五）；唯一的例外是**位移通道刚性导致的整体起伏缺失/移植**（见 §四，两条路都会缺，且只能离线补）。

**"两条路互为对照"是最省时间的差分法**（本项目就是这么定位的）：同一条源动画、同一个 retargeter，
- 烘焙塌成一点 + 运行时也冻 → **资产/骨架问题**（先量 §二.3b 的局部平移判据）；
- 烘焙正确 + 运行时冻在 rest → **运行时外壳问题**（按 ① game world ② 目标 mesh 的**组件父链**是否通到源网格组件 ③ PIE 里 `comp.get_anim_instance()` 是否真的生成了目标 ABP 类 ④ 节点 `IKRetargeterAsset` ⑤ **换个新名字重建 ABP** 的顺序查，见 §三 硬约束⑥）。
- **PIE 运行中不要碰 rig/retargeter**：`EA.load_asset`/`get_controller` 会返回 None（实测全 None），改 rig 前先 `unreal.EditorLevelLibrary.editor_end_play()`（`LevelEditorSubsystem` 没有 `editor_end_play`，别猜）。

**本仓库现成的两个可跑参考（都验证过，改东西时照抄布局）**——关卡 `ThirdPersonMap`：

| | 源 actor / 组件 | 目标 actor / 组件 | 目标骨架资产 | 重定向器 | ABP |
|---|---|---|---|---|---|
| Richie（早期） | `RT_Source_Manny`（SkeletalMeshActor + `SKM_Manny` + 单节点动画 `test_run_f`） | `RT_Target_MMD`（挂在源 mesh 组件下，`KEEP_WORLD`） | `/Game/MCP/Richie/Richie_UE` | `RTG_Richie`（源 `IK_Mannequin`，root scale 0.01，Root 链=`root` NONE/GloballyScaled） | `ABP_RichieRT` |
| Ganyu（**ASCII 标准化版**，当前） | 同上，复用同一个源 actor | `RT_Target_Ganyu`（源 mesh 组件下，偏移 (0,400,0)） | `/Game/MCP/Ganyu/Ganyu_UE`（159.24 cm，无内嵌 scale，D 骨权重已并，**316 骨全 ASCII**） | `RTG_Ganyu`（源 `IK_Mannequin`，root scale 1.0，**auto-gen 的 20 条链**；`Spine/Neck/Head` 用 `INTERPOLATED`、腿链 `thigh→ball`，retarget root=pelvis） | `ABP_GanyuRT3`（**注意是 3**；`ABP_GanyuRT`/`ABP_GanyuRT2` 是换过 skeleton 的废类，其中 1 号至今删不掉） |

配套脚本 = **本 skill 的 `scripts/`**（都是"改顶部参数即可重跑"的模板，源码**纯 ASCII**：非 ASCII 骨名走 `dump_skeleton_bones.py` 落盘的 JSON 按下标取，原因见该脚本注释）：

| 脚本 | 做什么 | 什么时候用 |
|---|---|---|
| `dump_skeleton_bones.py` | 骨名+世界坐标 dump 成 JSON/TSV | 规划链之前；给其它脚本提供 ASCII 骨名表 |
| `build_ik_rig.py` | 建 IK Rig：set mesh → 按**下标**建 10 条链 → set retarget root → 回读验证 → 存盘 | 新目标骨架 |
| `build_retargeter.py` | 建/改 retargeter：绑两侧 rig → preview mesh → `auto_map_chains` → `auto_align_all_bones(TARGET)` → root scale（干净骨架=1.0）→ 存盘 → 回读映射 | 新重定向器；链改了以后 |
| `build_runtime_retarget_abp.py` | **工厂带 skeleton** 建新 ABP → 加 RetargetPoseFromMesh → 写 `IKRetargeterAsset`（源网格模式用 5.7 的默认 `RetargetFrom`）→ Pose→Root → 编译断言 → 换关卡 actor 的 `anim_class` | 建/重建运行时 ABP（必须换新名） |
| `spawn_rt_target_actor.py` | spawn 目标 actor → 设 mesh/anim_class → 挂到源 mesh **组件**下（KEEP_WORLD）→ 存关卡 | 摆 PIE 演示 |
| `bake_diff_probe.py` | 批量烘一条源动画 → 逐帧量世界坐标（+ 源侧对照） | **每次改 rig 后的第一探针**（差分法） |
| `pie_motion_probe.py` | `ACTION='start'/'sample'/'stop'`：起停 PIE + 按 label 采骨坐标两次对比 | 最终确认"到底动没动" |
| `dump_retargeter.py` | 读 IK Retargeter 真实结构：两侧 rig/preview mesh/pose 名 + root settings 实测字段 + 每条 chain settings 的**真实数据成员名与值**（root settings 走 `export_text`；chain settings 无 `export_text`，走 `dir()` + 反射，见 §六.1） | 要查 chain/root 的**真实字段名**时（原稿假定的名字实测不存在） |
| `probe_retarget.py` | 派生判据：链数 / **两侧配对**（逐条比 `source_chain` 与 `target_chain`）/ root settings 非默认值（`scale_horizontal`/`scale_vertical`/`blend_to_source` 非 1/1/0）/ 两侧 pose 名 / 本次拿不到什么 → 一行 `PROBE_RESULT: PASS\|CHECK` | 改完 retargeter 后的一次性体检 |

（这些是从一次真实落地里长出来的：现场还用过一套同逻辑的一次性脚本（建 rig / 重烘 / 建 ABP / 烘干净版各一份），它们随会话清掉了——要用就直接照本案例 `scripts/` 里的模板改参数。）



> **两个案例已拆成独立 skill**：
> 原 §三.1（把运行时重定向装成一个受控角色）→ `unreal-retarget-case-runtime-carrier`；
> 原 §四（烘焙后补竖直通道：把源的整体上下起伏搬到目标）→ `unreal-retarget-case-bake-vertical-channel`。
> 因为原 §四 整节搬走，本文的编号从 §三 直接跳到 §五（不是漏了一节）。

## 五、姿态偏差的排查法（重点：指标选错就永远查不出来）

**禁用**："骨世界旋转量"（`|Δ from rest|`）。绕错轴转同样角度也能全绿——本项目就是这么把"看着更差"验成"误差 0.00"的。

**用**：
1. **相对父骨的局部旋转差**（哪一个关节在偏）。
2. **与骨轴约定无关的几何量**：如**脚底法线相对地面**。
   ```python
   # rest 时把世界"下"转到该骨局部系里（与骨轴/roll约定无关）
   down_local = qrot(qinv(W_rest_world), (0, 0, -1))
   # 动画时同一个局部向量转回世界 = 当前脚底朝向
   sole_world = qrot(W_anim_world, down_local)
   tilt = angle(sole_world, (0, 0, -1))          # 与地面法线的夹角
   ```
   对源与目标各算一遍，比 `tilt` 差；**再取多个相位**看它是"恒定"还是"随相位变化"。
3. **视口**（最终验收永远是用户的眼睛）。

**分相位测量法**（可重复、不用猜相位）：
- 起 PIE → 源网格 `set_play_rate(0)` + `set_position(t)` 冻结到确定相位 → **下一次工具调用**再采样（同一次调用里采样会读到上一帧，目标还没更新）。
- 取"两脚反相"的相位（如 0.5s：左脚平放、右脚蹬地）→ 一次采样同时看到"常量偏置"和"相位跟随"两件事。

**判读表**：

| 观察 | 结论 | 改哪 |
|---|---|---|
| 偏差**恒定**、各相位一样 | retarget pose（目标）没对齐 | `rc.set_rotation_offset_for_retarget_pose_bone(bone, Quat(Δ), TARGET)`：取"源几何量→目标几何量"的最小弧旋转，按 `D_local = qinv(W_bone) * D_world * W_bone` 换算后写入；写完**重测其余相位**（只治常量） |
| 偏差**随相位变**、目标变化幅度被压平 | 链上分配错（末端骨/中间骨不在链里） | 改链 `start/end bone`（把该骨纳入或排除），再 `auto_map_chains`+`auto_align_all_bones` |
| 单侧肢体整段不动 | 该骨不在任何链里（或权重不在该骨） | 查 `get_retarget_chains()` 的 start/end + 权重归属 |
| 根/整体歪、躺平、缩放爆 | 根通道 | `rotation_mode=NONE`（180° 翻转）、root `ScaleHorizontal/Vertical`、`retarget_root` 选胯部 |
| 末端不贴地/手不贴 | IK 通道没开 | 该链 `blend_to_source` > 0（配 goal 骨），必要时 `static_rotation_offset` 校脚跟朝向 |
| 整身/跳跃**完全没有上下起伏**（各相位都平） | FK 位移通道刚性（`translation_mode=None`），源的整体竖直位移没通道可走 | `unreal-retarget-case-bake-vertical-channel` 的单通道移植源骨盆竖直位移（改 `translation_mode` 会连带把源位移全搬来，容易过冲，优先移植） |

**不要自研 FK 传递**。项目里试过 `W_tgt = W_src × (W_src_rest⁻¹ × W_tgt_rest)` 逐骨写回 track：结构上和引擎一致，但 ① 参考姿势用错了（用了 rest 而非 retarget pose，把对齐残差灌进每一帧）；② 链上分配按骨序号硬映射而非按链参量 → 躯干被**过约束**（每根骨都被钉到源的世界角度），看着"散"。正确做法永远是：**改 retarget pose / 改链 / 开 IK**，然后重跑官方批处理。

---

## 六、API 速查（实测可用形态）

```python
EA  = unreal.EditorAssetLibrary
lib = unreal.UnrealMCPBlueprintGraphLibrary        # AnimBP 图操作（见 unreal-blueprint-authoring）
c   = unreal.IKRigController.get_controller(EA.load_asset('/Game/.../IK_X'))
rc  = unreal.IKRetargeterController.get_controller(EA.load_asset('/Game/.../RTG_X'))
T   = unreal.RetargetSourceOrTarget.TARGET
```

| 目的 | 调用 |
|---|---|
| 看/改链跨度 | `c.get_retarget_chains()`（只给 chain_name/ik_goal_name）、`c.get_retarget_chain_start_bone(ch)` / `..._end_bone(ch)`（**直接回骨名，比读 `BoneReference` 结构可靠——后者 python 侧打印成 `{}`**）、`c.set_retarget_chain_start_bone(ch, bone)` / `..._end_bone(ch, bone)`、`c.get_retarget_root()` / `c.set_retarget_root(bone)` |
| **建链** | `c.add_retarget_chain(chain_name, start_bone_name, end_bone_name, goal_name)` —— **4 个参数**（`goal_name` 传 `''` 即可；少传一个会 `TypeError: required argument 'goal_name'`）。返回**引擎唯一化后的链名**（可能被改写，如 `Head`→`head`）——后续一律用返回值/`get_retarget_chains()` 里的名字 |
| 根骨（IK solver 用） | `c.get_root_bone(solver_index)` / `c.set_root_bone(bone, solver_index)` —— 与 retarget root 是两回事；没有 solver 时调它会打 `LogIKRigEditor: Warning: Root bone not set. Invalid solver index, 0.`（无害）。retarget root 走 `set_retarget_root`（**单参数**） |
| 读 rig 的参考姿势 | `c.get_ref_pose_transform_of_bone(bone)` —— 回**世界 cm**（含 scale 影响）；用来判断"rig 里的姿势数据对不对"与"骨架有没有内嵌 scale"（§二.3b） |
| 建/改 goal | `c.add_new_goal(goal_name, bone_name)`（**先 name 后 bone**）、`c.get_all_goals()`、`c.get_bone_for_goal(goal_name)`、`c.set_retarget_chain_goal(ch, goal)`、`c.is_goal_connected_to_any_solver(goal)` |
| 链设置 | `rc.get_all_chain_settings()` → **每项只有 3 个数据成员（实测）**：`source_chain` / `target_chain`（链名，本次 20/20 同名）+ `settings`（`TargetChainSettings`）→ 取 `.get_editor_property('settings')` / 取 `fk`、`ik` 子结构（实测键名：`fk` = `enable_fk` / `rotation_mode` / `rotation_alpha` / `translation_mode` / `translation_alpha` / `pole_vector_matching` / `pole_vector_maintain_offset` / `pole_vector_offset`，`ik` = `enable_ik` / `blend_to_source` / `blend_to_source_weights` / `static_offset` / `static_local_offset` / `static_rotation_offset` / `scale_vertical` / `extension` / `affected_by_ik_warping`，另有 `speed_planting`）改完写回 → **`rc.set_retarget_chain_settings(chain_name, inner_settings)`（传内层 settings！传外层链结构会报 parameter 转换失败）**。原稿的 `chain_name` / `start_bone` / `goal_name` / `fk_rotation_mode` / `fk_translation_mode` **不存在**（`RetargetChainSettings` 也没有 `export_text`，字段名走 `.codemaker/skills/UE重定向/scripts/dump_retargeter.py` 现取） |
| 根设置 | `rs = rc.get_root_settings()` 改 `scale_horizontal/vertical`、`translation_alpha`、`rotation_alpha` → `rc.set_root_settings(rs)` |
| 对齐/映射 | `rc.auto_map_chains(unreal.AutoMapChainType.EXACT, True)`、`rc.auto_align_all_bones(T)` |
| retarget pose | `rc.set_rotation_offset_for_retarget_pose_bone(bone, unreal.Quat(x,y,z,w), T)`；读回 `rc.get_rotation_offset_for_retarget_pose_bone(bone, T)` **返回 Quat（不可迭代，别 `for` 它）**；`rc.reset_retarget_pose(bones_to_reset, pose_name, source_or_target)`（**3 参数**，`bones_to_reset` 是数组）；`rc.get_current_retarget_pose(T)` 回 `IKRetargetPose` 结构（**不能喂给 AnimPose 求值**，要看数据得走 `get_ref_pose_transform_of_bone`）；`rc.get_current_retarget_pose_name(T)` → 通常 `"Default Pose"`；`rc.get_preview_mesh(st)` / `rc.set_preview_mesh(source_or_target, mesh)`（**2 参数**） |
| retargeter ↔ rig | `rc.set_ik_rig(SOURCE/TARGET, rig)`（void）；`rtg.has_source_ik_rig()/has_target_ik_rig()`；IK Rig 资产路径可以从参考实现反查：`AR.get_dependencies('/Game/.../RTG_X', unreal.AssetRegistryDependencyOptions(True, True, True))` |
| 写平移轨道 | `bridge('set_bone_track_keys', asset_path=pkg, bone_name=b, keys=[{'time':t,'position':[x,y,z],'rotation':[..],'scale':[1,1,1]}])`；**先按 §二.6 标定轴与单位**；读回用 `get_bone_pose_for_frame(anim, b, f, False)`（与写入同空间） |
| 批量烘焙 | `unreal.IKRetargetBatchOperation.duplicate_and_retarget(asset_data_list, src_mesh, tgt_mesh, retargeter, '', '', '', '_Suffix', False)`，`asset_data_list = [d for d in AR.get_assets_by_path(dir, True) if ...]` |
| 运行时节点 | `lib.add_node_by_class(graph, 'AnimGraphNode_RetargetPoseFromMesh', x, y)` → `inner = node.get_editor_property('node')`；写 `inner.set_editor_property('ik_retargeter_asset', rtr)` → `node.set_editor_property('node', inner)` → `lib.connect_pins(node, 'Pose', root.node, 'Result')` → `lib.compile_blueprint_checked(bp)`。**源网格选择在 5.7 是 `RetargetFrom`（`ERetargetSourceMode`，默认 `ParentSkeletalMeshComponent` = 沿 attach 父链找第一个 `SkeletalMeshComponent`）**；老布尔 `bUseAttachedParent` 已降级为 `bUseAttachedParent_DEPRECATED`（`UPROPERTY()`，只在 `PostSerialize` 读旧资产时消费一次）⇒ **写它不改变运行时行为**，别按它写脚本，要靠布局满足默认语义。**ABP 必须建时就带 skeleton**（见 §三 硬约束⑥） |

**其它实测坑**：
- 写组件的 `anim_class` 必须 `unreal.load_class(None, '/Game/.../ABP_X.ABP_X_C')`；用 `EA.load_asset` 得到 AnimBlueprint 对象，写进 `UClass` 属性会被**清空**。
- MCP 工具 `set_component_property` 用 **C++ 属性名**（`SkeletalMeshAsset`/`AnimationMode`/`AnimToPlay`/`AnimClass`），snake_case 会提示 "Did you mean:"。
- **`spawn_blueprint_actor` 同名已存在 = 引擎 Fatal，直接崩编辑器**（`LevelActor.cpp:574`，`Required_Fatal`）→ 换唯一名；要重建先 `delete_actor`。
- 编辑器世界里运行时重定向节点目标恒为 rest；**必须 PIE**。停 PIE 用 `unreal.EditorLevelLibrary.editor_end_play()`（`LevelEditorSubsystem` 只有 `editor_request_end_play`，没有 `editor_end_play`）。
- 源网格要 `SkeletalMeshComponent`：`set_play_rate(0)` + `set_position(t)` 可定相；`set_animation_mode(ANIMATION_SINGLE_NODE)` + `animation_data.anim_to_play`（python 无直接 `anim_to_play` 属性，走 `animation_data` 结构）。
- **`lib.get_graph_nodes(g).nodes` 回的是 `node_info` 结构，不是节点对象**：读属性/引脚/内部 struct 都要先 `.node`（`n.node` → `UAnimGraphNode_*` → `get_editor_property('node')` → `FAnimNode_*`）。少一层就会得到 `Failed to find property 'ik_retargeter_asset' on 'AnimGraphNode_...'` 这种"看着像属性名写错"的报错。
- **`unreal.IKRetargetProcessor` 没有暴露给 python**（不能手工 `initialize` 来做差分）；**`unreal.ObjectTools` 也没有**。
- **删不掉的资产**：`delete_asset` / `delete_loaded_asset` / `safe_delete_asset` 都会对某些 AnimBlueprint（尤其换过 skeleton 的）恒返回 False，且 `list_asset_blockers` 报空。**不要在这上面耗时间**：换个新名字重建（本项目 `ABP_GanyuRT` → `ABP_GanyuRT2`），旧资产留到重启编辑器后再删。同理，删 mesh/skeleton/physics 之前要先 `destroy_actor` 掉所有引用它们的关卡角色实例。

### 六.1 只读体检：`dump_retargeter.py` / `probe_retarget.py`（为什么不做成 bridge 命令）

`scripts/` 里两个**只读**脚本回答"这个 IK Retargeter 到底建成了什么"，改完 rig/retargeter 后先跑它们，再谈姿态：

| 脚本 | 做什么 | 判据 / 产出 |
|---|---|---|
| `dump_retargeter.py` | 取控制器后打印两侧（SOURCE/TARGET）的 ik rig / preview mesh / 当前 retarget pose 名、root settings 全部实测字段（`export_text` 原文 + 解析键名），以及**每条 chain settings 的真实数据成员名与值**（实测 `source_chain` / `target_chain` / `settings`；该类无 `export_text`，走 `dir()` + 反射） | 只给"观测到什么"，不判定 |
| `probe_retarget.py` | 链数、**两侧逐条配对**（`source_chain` vs `target_chain`）、root settings 非默认值（`scale_horizontal` / `scale_vertical` / `blend_to_source` 不是 1/1/0 的情况）、两侧当前 pose 名、"哪些信息本次拿不到" | 一行 `PROBE_RESULT: PASS\|CHECK`（任一条不成立就 `CHECK` 并列出原因） |

**为什么不做成 bridge 命令**（原拟的 `retarget_probe` / `retargeter_dump` 已按 design 决策 8 砍掉）：

1. **python 今天就能做到**——这不是能力缺口，只是便利。一次 `execute_python_file` 即可，做成命令的增量价值接近零。
2. **字段名随引擎版本变，而且原稿假定的顶层名字全部不存在**：`RetargetChainSettings` 上的 `chain_name` / `start_bone` / `end_bone` / `goal_name` / `enable_fk` / `fk_rotation_mode` / `fk_translation_mode` / `enable_ik` 一律报 `Failed to find property`（`enable_fk` / `enable_ik` 只在 `settings.fk` / `settings.ik` 里，链的旋转/位移模式叫 `rotation_mode` / `translation_mode` 而非 `fk_*`）。把这套名字冻进 C++，每次纠正都要"编译 + 重启编辑器"。
3. **`initialized` 一类判据本就是启发式**（引擎未暴露 `unreal.IKRetargetProcessor`），只能由可观测证据推导、需要迭代；而写命令不能建在半可靠判断上（决策 2）。
4. **仓库既定做法**就是"一次性采集 / 体检"放 skill 脚本：`dump_skeleton_bones.py` 与 asset-pipeline 侧的 `check_imported_skeleton.py` 都是这个形态。

**怎么发现真实字段名**（脚本职责，不猜；两条路都实测过）：① root settings 有 `root_settings.export_text()` → 取 UE 文本形式（`(Key=Value,...)`）再解析键名；② **`RetargetChainSettings` 实测没有 `export_text`**（它是 UObject 包装，`dir()` 里是 `get_class` / `modify` / `static_class` 这类管道）→ 退回反射：取 `dir()` 里**非 dunder 且不可调用**的成员（= 数据字段）逐个取值。实测真实成员是 `source_chain` / `target_chain` / `settings`。**正文与脚本都不写未证实的字段名**，脚本也不硬编码这三个（靠 `dir()` 现取）。

**PIE 调用纪律（重点）**：PIE 运行中，引擎 python 的 `unreal.load_asset` / `unreal.IKRetargeterController.get_controller` 对 IK Rig / IK Retargeter 会**静默返回 None**。这个 `None` **MUST NOT** 被当成"资产不存在"；需要编辑器侧资产时先 `stop_pie`，再用 `stop_pie` 返回的 `pie_running` 轮询确认已退出（PIE 中取姿态还会拿到假答案/rest，见 §三 硬约束①）。

**实测（本机 `/Game/MCP/Ganyu/RTG_Ganyu`，真实 MCP 工具 `execute_python_file`）**：两脚本都跑通 —— `dump_retargeter.py` 打出 20 条链的 `source_chain` / `target_chain` / `settings`（两侧 20/20 同名）与 root settings 全字段；`probe_retarget.py` 回 `PROBE_RESULT: PASS | chains=20 | pair=20/20 | ok`。**这也顺手证明"发现字段名"这条路走得通**：第一次跑时 `export_text` 在 chain settings 上直接失败，退回 `dir()` 才拿到真名。

bridge 侧相关能力（已存在的工具名）：`start_pie` / `stop_pie` / `get_actor_pose` 管 PIE 生命周期与运行时姿态数值；`python_api_index` / `python_api_doc` 查**引擎 python API 层**的签名——本节脚本用的那些控制器方法正是这一层，`list_mcp_commands` 看不到它们（LAYER BOUNDARY）。

---

## 七、验收纪律

1. **指标必须对轴敏感**：局部旋转差 + 几何量（脚底/手/头顶的世界朝向与位置）+ 视口；**永不用"旋转量"**。
2. **验证必须落在你写的那根轴上**：写了竖直分量，就量"该骨在**该局部轴**上的位移范围"（本项目：`pelvis` 局部 y 范围），并与 `源竖直范围 × 高度比` 对比（54/54 相等；例：jump 0.873 m、hit 0.395 m、walk 0.0396 m）。**别拿别的骨/别的轴冒充**——本项目把根骨 `8_arm` 的**前向**位移 4.38 当成"走路 bob 已修好"上报，属于假绿。
3. **至少两个反相相位**：区分"常量偏置"（retarget pose）与"分配错误"（链）。
4. **写后重测**：`rc.set_rotation_offset_for_retarget_pose_bone` 写完要**重起 PIE** 再量（处理器缓存），并回读偏移确认落盘（`EA.save_asset(rtg)`）。
5. **改 rig/retargeter 前先停 PIE**；改完保存 assets。
6. **不要留下创可贴**：常量偏移若只是掩盖链问题，链修好后把偏移 `reset` 回 identity。
7. **不要自研 FK 传递**；也不要指望 `rotation_mode` 在等骨数链上改变结果。位移缺失优先用 `unreal-retarget-case-bake-vertical-channel` 的单通道移植，而不是试 `translation_mode` 全量开关。
8. **两条路一致性**：烘焙版若比运行时版差，先查"烘焙时是不是叠了额外处理"，而不是怀疑求解器。
9. 用户视口是最终验收；数据是用来**定位**的，不是用来宣布成功的。
10. **先判"资产/骨架"还是"参数"，再动参数**：一次烘焙的输出如果**整副骨架塌成一点**（各骨世界坐标互差 <1–2 cm）或**各帧几乎不变**，那读的是坏数据，调 `rotation_mode`/`translation_mode`/root scale 全是白工（本项目白烧 4 轮）。先量 §二.3b 的一条局部平移判据。
11. **"干净骨架"的验收指标（一次过就是一次过）**：同一条源动画烘焙后，`AnimPoseExtensions` 逐帧量这些量——腰/骨盆 z 在身高量级（Ganyu 86–89 cm）、左右踝**交替**（27→11→27 / 13→23→12）、膝 ~50、头 ~130–134、根位移随源线性推进。**这些全对才叫骨架与 rig 对了**，剩下才谈姿态细节。
12. **运行时外壳不要修补，直接重建**：ABP 只要动过 skeleton / 重编译过又不动，就换新名重建（§三⑥）；`delete_asset` 删不掉不是你的错，别耗时间。
13. **每改一次 rig/retargeter 都留一条可复算的"烘焙差分"**：烘焙是同步、可重复、无 tick 顺序干扰的探针；运行时只用来最终确认。**两者用同一个求解器**，因此"一个通一个不通"永远指向各自的外壳（烘焙的 profile / 运行时的类与组件链），不指向求解器。
