---
name: unreal-cloth-physics-authoring
description: "UE5（UnrealMCP）里给带裙摆的骨骼网格做「骨物理 + 布料」的 Skill：运动由谁造的分工表（AnimDynamics 造摆动 / 布料做面内褶皱与碰撞避让 / 腰带挂饰流苏必须物理化、需要被身体挡开的附属链走 PhysicsAsset + AnimGraphNode_RigidBody 路线）、legacy 布料（UClothingAssetCommon）的语义要点（MaxDistance 掩码 = 允许离开蒙皮姿势的距离、掩码 <0.1 的粒子被 Chaos 判为 kinematic 且 InvM=0、全 0 掩码等价蒙皮、球形约束只拉不回推、权重图缺失时 anim drive 取 Low、tether 由掩码派生、掩码有三份副本——LOD PointWeightMaps / PhysicalMeshData.WeightMaps / 渲染分段 static alpha，只有 ApplyParameterMasks + SkeletalMesh::Build 才让前三者一致生效）、可脚本化的掩码斜坡生成法（腰部置 0 做锚点、往下线性升到 N cm）、AnimDynamics 稳定化的三个真实来源（关节限制被输入姿势持续违反 = 抖动；per-body 定义不齐导致该骨无碰撞球 = 穿身体；PhysicsAsset 胶囊过肥 = 把布料顶鼓）、角度范围 ≥360° = 引擎跳过该约束（自由）vs 范围置 0 = 锁死在 0 旋转（刚体）、PA + RigidBody 路线的判据（`FKSphylElem` 长轴是形状自己的局部 Z，引擎按顶点包围盒把该轴旋到 X/Y ⇒ 算碰撞几何必须读到形状 `Rotation`，否则穿模量错一个数量级；引擎自动生成的胶囊按包围盒「虚胖」、会把模拟链顶出去；`gap = 线段距离 − (r链 + r挡块)` 为负 = 接触穿插、身体基线为 0 而链仍在动 = 链自身在摆、−0.3cm 且逐帧不变的稳态轻压 ≠ 深穿插；只让必要骨骼参与碰撞；链节/叶骨的胶囊方向要用自身在父骨空间的偏移反推；不是 UPROPERTY 的引擎数据（如 `CollisionDisableTable`）只能靠命令改）、以及不靠截图的自证表（PIE 里 ClothingSimulationInteractor 的动态/运动学粒子计数、apply_cloth_masks 回报的 pinned 计数与两套 skinned/cloth 映射、reflect_probe 属性路径读烘焙数据与 PhysicsAsset 胶囊）。触发场景：准备调用以下任一 MCP 工具前 MUST 加载本 Skill —— set_object_property / reflect_probe（读写布料配置、掩码数组、PhysicsAsset 形体这类无 UFUNCTION 的属性）/ apply_cloth_masks / set_physics_asset_collision / list_physics_asset_bodies / add_physics_asset_body / add_physics_asset_constraint / remove_physics_asset_body / remove_physics_asset_constraint / set_blueprint_node_property（Node.PhysicsBodyDefinitions、SphericalLimits、GravityScale、Linear/AngularDampingOverride、NumSolverIterationsPreUpdate/PostUpdate、bDoUpdate）/ compile_blueprint / add_blueprint_node_by_class / delete_blueprint_nodes / connect_blueprint_pins / start_pie / stop_pie；以及任何涉及「裙子或裙摆不动、太硬、摆得不够、太抖、被撑大、掉到地上、穿模、捏皱成纸团、布料完全不动、掩码到底怎么刷、cloth 参数怎么写、AnimDynamics 关节限制该给多少、per-body 碰撞球、PhysicsAsset 胶囊收肥、挂饰/发梢要被身体挡开、实体碰撞不穿模、PA 上的碰撞对怎么关、只让部分骨参与碰撞、负间隙量测、挂在链上的抖到底是谁造成的、布料该不该删/该不该留」的任务。材质本身走 unreal-material-authoring，蓝图图结构与节点寻址走 unreal-blueprint-authoring，外部文件导入与骨骼网格替换走 unreal-asset-pipeline。"
metadata:
  version: "1.0.0"
  upstream: unreal-blueprint-authoring
  downstream: ~
---

# UnrealMCP 服装物理（裙摆 / 挂饰）Skill

适用：本仓库 `Plugins/UnrealMCP`。对象是**带裙摆的骨骼网格**（legacy 布料 = `UClothingAssetCommon`，非 ChaosClothAsset）。
本文只写**长期有效的做法、判据与引擎语义**。

> 一句话判据：**布料的"目标"是蒙皮姿势**。骨不动 ⇒ 布料只能表现出重力垂坠（≈蒙皮，看不出差别）；
> 要先有"会动的骨"，布料才有意义。裙子要动，先回答"骨靠什么动"。

---

## 一、先定分工：运动由谁造

| 需求 | 用什么 | 为什么 |
|---|---|---|
| 裙摆**摆动/滞后**（骨级别的二次运动） | AnimDynamics（骨物理）或**烘焙轨道** | 网格靠蒙皮跟随，便宜可控；不需要任何布料烘焙数据 |
| 面内**褶皱/自碰撞/被腿顶开/贴腿** | 布料 | 它解的是面片；骨物理没有面内形变，也没有碰撞 |
| 裙子下半"完全不动" | 先查**谁在驱动骨** | 若裙摆骨不在重定向链里（源没有对应骨），它的目标永远是静止姿势 |
| 腰带**挂饰/流苏**（`tassel_*` 之类的附属链）"太硬" | 同样上 AnimDynamics | 它们通常没被任何节点驱动＝纯蒙皮刚性跟随；挂饰要会摆只能物理化（或烘焙），布料帮不上 |
| 附属链**要被身体挡开**（真碰撞：被大腿/裙摆顶住、不被穿过） | **PhysicsAsset + `AnimGraphNode_RigidBody`**（`unreal-cloth-physics-case-chain-rigidbody`） | AD 完全不认识 PhysicsAsset 与世界几何（§三.4）；布料会与 PA 胶囊求交，但那是解面片，骨链的二次运动+碰撞只能走 PA 路线 |

- 判据：`AnimDynamics` 能用链的**输入姿势 + 关节限制 + 碰撞球**给出可见运动，**不需要** 掩码/权重烘焙；
  布料要发挥作用则必须先有"会动的目标 + 正确的 MaxDistance 掩码"。
- 组合（产品形态）：骨物理造运动 → 布料在其上叠加褶皱与避让。两者**不冲突**：布料的约束都相对"当前蒙皮姿势"，骨动了目标就动。

---

## 二、legacy 布料（ClothingAssetCommon）的语义要点

1. **MaxDistance 掩码 = 每个顶点的"允许离开蒙皮位置的距离"**。
   `0` ⇒ 该粒子被球形约束钉在蒙皮位置；Chaos 还用它判定 kinematic：**掩码 `< 0.1` 的粒子 InvM=0**
   （`ChaosClothingSimulationCloth.cpp` 里 `KinematicDistanceThreshold = 0.1`）。
   → **全 0 掩码 = 一整块 rigid，等价蒙皮**（这是"布料完全不动"的第一嫌疑，先查它）。
2. **球形（max distance）约束只是"上限"**：只在粒子超出半径时把它拉回，**没有任何力把布料推回身体**。
   所以半径大 = 鼓/掉地上，半径小 = 贴身；想要"贴身 + 可动"，半径必须小（几 cm 量级）。
3. **anim drive**：权重掩码缺失时取 `Low`（`PBDWeightMap.h` 的 `operator FSolverReal() = GetLow()`），
   所以 `{Low:0, High:1}` 在没刷权重图时**等于关掉**；`{Low>0}` 才是"往动画姿势拽"（=变硬/冻住）。
4. **tethers（长程约束）由掩码派生**（kinematic 判定同上）⇒ 掩码一改必须重建，否则要么没 tether、要么 tether 全错。
5. **掩码有两份存储**：LOD 级 `PointWeightMaps`（编辑器侧源，`bEnabled`）与物理网格 `PhysicalMeshData.WeightMaps`（**模拟读的那份**）。
   `FClothLODDataCommon::PushWeightsToMesh()` 用前者覆盖后者（只同步 `bEnabled` 的）。
6. **渲染是第三份数据**：`FSkelMeshRenderSection::ClothMappingDataLODs[bias][i].SourceMeshVertIndices[3]`
   = static alpha（`0xFFFF` 只蒙皮 / `0` 只布料），由 `ComputeVertexContributions` 在应用掩码时写。
   → 只改模拟侧/导入模型、不改 render data，**画面上什么都不会变**；render data 由 `USkeletalMesh::Build()` 重建。
7. `UClothingAssetCommon::ApplyParameterMasks(bUpdateFixedVertData=true, bInvalidateDerivedDataCache=true)`
   是"掩码生效"的入口（推权重 + 重建 tether + 重算分段贡献），**不是 UFUNCTION** ⇒ 只能从 C++ 或 Cloth 绘制 UI 触发。
8. **掩码生成法（可脚本化）**：按顶点到腰的距离做斜坡 —— 腰部一段置 0（做锚点，跟随身体），往下线性升到 N cm。
   全 0 = 硬化、全 >0 = 没有锚点（会掉），两者都不可用。
9. **布料当"碰撞层"的取舍**：贴身小球半径 + 高刚度 ⇒ 保形 + 避让（推荐，接近"蒙皮+碰撞"）；
   低刚度 + 大半径 ⇒ 褶皱多但容易压缩屈曲，观感上像"捏皱的纸团"。

---

## 三、AnimDynamics 稳定化的三个真实来源（都实测过）

1. **关节限制被输入姿势持续违反 → 每帧硬投影 → 抖动**。
   特征：`IterationCount` 提高、阻尼加大、关 `PlanarLimit`、改胶囊尺寸**全都无效**。
   做法：**把角度范围放到"自由"** —— 引擎对 **≥360° 的范围直接跳过该约束**（`AnimNode_AnimDynamics.cpp` 的
   `ConstrainAngularRange` 前注释："any limit with 360+ degree range is ignored and left free"），
   即 `AngularLimitsMin/Max = ∓360`；线性范围保持 ±1cm。
   ⚠️ **反例（踩过）**：`Linear*/Angular X LimitType = Limited` 且范围**置 0 不是"自由"而是"锁死在 0 旋转"** ——
   抖是停了，但链变成刚体（现象是"摆得不够/太硬"，很容易被误当成"修好了"）。
   参考节点（已调好的头发）用的是 ±6° 且不抖：短链、输入姿势本来就落在范围内；**长链（裙摆）别照抄这个窄范围**。
   **残留的"微弱抖"另有两个旋钮**（限制被持续违反时的每步硬投影）：
   `bOverrideAngularBias` + `AngularBiasOverride`（默认取常量 `AnimPhysicsConstants::JointBiasFactor = 0.3`，
   `AnimPhysicsSolver.h:53`；**调小 = 纠正更柔和**，本项目挂饰用 0.12）与迭代次数（挂饰 40/20）。
   抖动也常来自"球面接触反复开合"：把 R 的余量做大（`最小间距 − 2cm` 以上）比调偏置更直接。
2. **per-body 定义不齐 ⇒ 该骨没有碰撞球**（半径落回节点级 0）⇒ 链直接穿身体，而且改 PhysicalAsset 胶囊尺寸永远"没效果"。
   做法：`PhysicsBodyDefinitions` 的元素数 = 链上骨数（后裙摆 9、前裙摆 6），每条 `SphereCollisionRadius 3~6`。
   ⚠️ **但 per-body 数据只在"名字已被回填"时才生效**：`BoundBone.BoneName` 为空（从未经过编辑器属性编辑的资产，
   例如只被脚本写过）⇒ `InitPhysics`（`AnimNode_AnimDynamics.cpp:705`→`:523-558`）每帧把**整表重建为默认值**
   （盒体 10³ / 半径 10 / 约束全 0），作者调参被静默丢弃；名字被 `PostEditChangeProperty` 回填
   （`AnimGraphNode_AnimDynamics.cpp:210`）后才用作者值。调 per-body 半径"没效果"时先查这一列。
3. **PhysicsAsset 胶囊的尺寸/多余条目会把布料顶出去**（布料会与**自己的骨骼胶囊**求交）：
   过肥的 `lower_body`（22.8cm）会把裙子顶到 23cm 之外；裙摆骨自己的胶囊（4~4.6cm）会让布料躲开自身骨骼。
   做法：裙摆骨胶囊关碰撞，身体胶囊收到真实围度（thigh ~6.5 / calf ~5.5 / 胯 `lower_body` ~12）。
4. **AD 不认识骨骼胶囊，也不认识世界几何** —— 想让挂饰/裙摆"被大腿挡住"，加 PhysicsAsset 是没用的
   （`AnimNode_AnimDynamics.cpp` 里没有任何 `PhysicsAsset`/世界碰撞引用）。要挡就用 AD 自己的解析形状：
   - `bUseSphericalLimits` + `SphericalLimits`（`DrivingBone` / `SphereLocalOffset` / `LimitRadius` / `LimitType`）：
     球心 = `驱动骨变换 × SphereLocalOffset`（偏移在骨局部空间，`:990-1000`）；
     `LimitType = Outer` = 把进入球内的 body 推出去（`:1007-1008`），球随驱动骨移动 ⇒ 挡块跟着腿走；
   - `bUsePlanarLimit` + `PlanarLimits`（驱动骨 + 平面变换）当"挡板"（`:968-982`）。
   **半径怎么取（两条实测规律）**：
   - 量**整条链所有 body** 到目标骨**线段**的最小距离（不是只量链首骨、也不是到骨原点）：
     `get_socket_transform('thigh_l')` / `('calf_l')` 取髋/膝两点，逐 body 算点到线段距离最小者
     （本项目 `tassel_01` 全链 8.51~14.85、`tassel_02` 8.74~26.97 ⇒ 最小 8.5）。
   - **R = 最小静止间距 − 2cm**（不是"尽可能大"）。R 太贴近静止位置会让 body 每帧越过球面 ⇒
     one-sided 限制反复开关 ⇒ **抖动**（现象：挂饰高频颤，越贴近越明显）。R 也不能小于被挡部位的视觉半径，
     否则照样穿；二者冲突时（静止余量本来就只有 ~1.5cm）**优先削内摆（角度收窄，如 ±30°→±15°），球只做兜底**。
   **阻尼方向**：求解器是 `动量 *= pow(1 − Damping, ΔT)`（`AnimPhysicsSolver.cpp:847-851`）⇒
   **Damping 越大越强**。改小 = 减弱（会放大抖动）；本项目挂饰用 0.98，默认 0.7（很弱）。
   抖动还压不住就加迭代次数（挂饰用 30/10）——限制集每步收敛得更好。
   ⚠️ **只在关节处放一个球基本没用**：挂饰是沿大腿**中段**贴上去的，单球只罩住髋关节 ⇒ 看起来"完全没改善"。
   正确做法是**沿骨放一串球**：`SphereLocalOffset` 在**驱动骨局部空间**（`A * B` = A 先 B 后，
   `TransformNonVectorized.h:1296` 的推导 ⇒ 骨变换是外层），所以先算
   `dir_local = unrotate(大腿世界旋转, (膝世界位置 − 髋世界位置).normalized())`，再沿它布球。
   - **AD 没有胶囊体限制**（`AnimPhysSolver.h` 里形状只有 `FAnimPhysShape::MakeBox`；限制只有球 Inner/Outer、
     平面、角度/线性）⇒ 想要胶囊就用**密集球串近似**，并按下式保证"腰"够粗：
     `腰半径 = sqrt(R² − (间距/2)²)`。让腰半径 ≥ 目标半径（大腿 ~6.5~7cm）即可反推最大间距
     （R=7.5 时最大约 5.4cm；本项目取 **5cm**，腰 = 7.07cm，每腿 9 个球）。间距 = 骨长的 ⅓（如 13.3cm）会留下
     只有 3.5cm 的腰 ⇒ 挂饰从那挤进去，等于没挡。
   ⇒ **优先"软摆 + 挡块"，不要用僵硬防穿模**：僵硬会把该动的部位一起冻住。

### 初始 per-body 参数（偏硬、抗穿模；新链从这里起步）

下面是本项目的起点值（引擎默认构造的值，已写进角色资产并按链回填了名字）：

| 字段 | 初始值 | 为什么 |
|---|---|---|
| `PhysicsBodyDefinitions` 元素数 | = 链上骨数 | body 与骨一一对应 |
| `BoxExtents` | `(10,10,10)` | 每节 10cm 立方包裹盒：接触体积大 ⇒ **不容易穿模**（代价是形变偏"硬"） |
| `LocalJointOffset` | `(0,0,0)` | 关节贴在骨上，不做偏移 |
| `CollisionType` | `CoM` | 碰撞参考取质心 |
| `SphereCollisionRadius` | `10` | `CoM` 下不参与；保留 10 便于之后切 `CustomSphere` |
| `ConstraintSetup.LinearX/Y/ZLimitType` | `Limited` + `LinearAxesMin/Max = [0,0,0]` | 线性全锁：链节不脱位、不塌 |
| `ConstraintSetup.AngularConstraintType` | `Angular` | 逐轴角限 |
| `ConstraintSetup.AngularLimitsMin/Max` | `[0,0,0]` | 角限 0 = 每节咬死在输入姿势上 —— **"硬"的主要来源**，也是抗穿模的关键 |
| `TwistAxis` / `AngularTargetAxis` | `AxisX` / `AxisX` | 沿骨轴 |
| `ConeAngle` | `0` | 不用锥形限制 |

要点：

- 性格是"**硬**"：几乎不产生二次摆动，换来**不易穿模**（头发/裙摆/挂饰贴住身体）。适合作为起点，尤其是链长、
  反复穿模、或只想先"别穿"的部位。
- **按链单独放开前先量"静止余量"**（本项目实测结论）：腰挂饰 `tassel_*` 静止时到腿轴只有 ~8.5cm、
  腿表面 ~7cm ⇒ 余量 ~1.5cm。把它放开到 ±30°（乃至 ±15°）都会在行走时与大腿互穿，加球串挡又引入
  接触抖动（见 §三.4 的腰径与半径规则），最后**复原为偏硬的默认值**才算稳。
  教训：**余量小的部位不适合"靠放开角度换摆动感"**；要么先解决几何（骨/蒙皮离腿太近），
   要么改用布料（布料才会与 PhysicsAsset 胶囊求交）。
- 余量充足（>3~4cm）的附属链才可以按上面 ±30° + 线性 ±1.5cm 起步；一次只动一类链，便于归因。
- 要摆动感再按下面的稳定基线放开（角度 ±6° 或 ≥360°、线性 ±1cm、球半径按部位收到 3~6）。**别把"用 0 治抖"当修好**：
  0 是锁死，不是自由，见 §三.1 的 ⚠️。
- **写入方式**：`PhysicsBodyDefinitions` 是数组，python 侧 `set_editor_property` 对它是**静默无效**的；
  要用 `set_blueprint_node_property(property_name="Node.PhysicsBodyDefinitions", property_value=[…])`
  —— 点号路径 + **引擎属性原名**（snake 拼法会被 `path_segment_not_found` 拒）。
- **写完必须回填名字**：用 `set_object_property` 对该节点写一个无害同值属性（如 `Node.GravityScale` = 当前值），
  它会触发 `PostEditChangeProperty` → `ValidateChainPhysicsBodyDefinitions`，由引擎按链写 `BoundBone.BoneName`。
  回填后 `node.BoundBone`/`ChainEnd` 与 body 首/末一致，运行时不再整表重建，**蓝图里的这套值才是生效值**。
- 逐步确认靠 `reflect_probe(property="Node")` 读回（写入不抛异常 ≠ 生效）。

**想要摆动感时用的稳定基线**（照抄已调好的参考节点）：`NumSolverIterationsPreUpdate/PostUpdate = 20/5`、`GravityScale 0.5~0.7`、
**角度自由（±360）**、线性 ±1cm、per-body 齐、球碰撞半径 3~6、`Linear/AngularDampingOverride 0.98`、`bUsePlanarLimit=false`。

**相关源码位置**：约束建立 `AnimNode_AnimDynamics.cpp:948-966`（Angular/Cone 分支）、关节限制语义同上注释、
`AngularBias` 只影响**限制被违反时的纠正力度**（限制自由时不生效，不是"刚性"旋钮）。

---

## 四、自证手段（不靠截图也能有数据）

| 想知道 | 怎么读 |
|---|---|
| 掩码是否真的进了模拟 | PIE 里 `ClothingSimulationInteractor.get_num_dynamic_particles()/get_num_kinematic_particles()`（应与掩码的 非0/0 计数一致） |
| 掩码/tether/渲染分段是否生效 | `apply_cloth_masks`（**参数名是 `object_path`，不是 `asset_path`**）回报：`max_distance_pinned_count`、tether 批数、`sections`（导入模型）与 `render_sections`（渲染）两套 skinned/cloth 计数 |
| 烘焙数据本体 | `reflect_probe` 的属性路径（如 `LodData[0].PhysicalMeshData.WeightMaps[1].Values`） |
| PhysicsAsset 胶囊（含**形状轴**，算几何必须） | `reflect_probe` 读 `SkeletalBodySetups[i].AggGeom` —— `SphylElems[0].Radius/Length/Center/`**`Rotation`**；只拿半径+长度算穿模量会错一个数量级（见 `unreal-cloth-physics-case-chain-rigidbody` §6.2） |
| 某个 body 现在与谁不碰 | `list_physics_asset_bodies` 的 `collision_disabled_with`（禁用表本体不是 UPROPERTY，反射读不到） |
| AnimDynamics 参数 | `reflect_probe(target=<ABP>, node_id=..., property='Node')` 读整坨，和**已调好的参考节点**逐项对照 |
| 编辑器 world 里布料不跑 | interactor 存在但计数全 0 是正常的；要读数必须 PIE |

---

## 五、纪律

- **一次只动一个子系统**（布料 / 骨物理 / PhysicsAsset），否则"没变化"时无法归因。
- **验收要有运动输入**：静止姿势下布料的可视结果 ≈ 蒙皮，看不出差别；走动/转身/被腿撞才看得出。
- 观感一律由用户在编辑器视口判定；数据只用来排除"根本没生效"。
- 掩码/权重这类烘焙数据用**脚本生成 + 落盘**（可重跑、可回归），不要手刷。
- 碰撞类改动的验收靠**负间隙量测**（见 `unreal-cloth-physics-case-chain-rigidbody` §6.4），不要靠"看着像穿了"下结论。

---

> **这一节已拆成独立 skill**：`unreal-cloth-physics-case-chain-rigidbody`（附属链做真碰撞的完整配方）。
> **这一节已拆成独立 skill**：`unreal-cloth-physics-case-ragdoll`（整具骨架变 ragdoll 的配方）。
