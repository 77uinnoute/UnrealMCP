---
name: unreal-asset-pipeline
description: "UE5（UnrealMCP）里把「外部文件 → 工程资产」这件事做对的 Skill：为什么外部文件导入必须走 import_assets（force_legacy 关 Interchange + readback，手搓 AssetImportTask 会直接崩编辑器）、替换/重导入已有资产（同名覆盖、保持 Skeleton 绑定与材质重链）的正确顺序、导入后能用来自证的硬证据（临时目录导入 + rest 姿势逐骨比对、动画/材质槽/骨数核对、权重类改动无 python 读接口时的验收路径）、导入骨架的 track 平移单位与竖直轴不遵循 UE 约定（实测 MMD 版：单位=米、世界向上=父骨局部 −Y）以及写平移轨道前必须先标定、资产生命周期（safe_delete_asset / duplicate_asset_safe / list_asset_blockers / 别在同一 job 删了再同名重建）、Blender→UE 的交接判据（别信上次导出参数、只信导入后比对）、以及执行模型（`execute_python_*` 一律同步、重活拆成短批、没有 deferred/job 模式）。触发场景：准备调用 import_assets / import_texture / import_skeletal_mesh / import_animation / set_asset_properties / get_asset_properties / safe_delete_asset / duplicate_asset_safe / list_asset_blockers / list_mcp_commands / execute_python_file（涉及导入、覆盖、批量落盘时）之前；以及任何涉及「把 FBX/贴图/外部文件导进 UE、替换或重导入已有资产、覆盖导出后重新导入、导入崩编辑器、资产没落盘、删资产、导入骨架的平移通道该写哪根轴/单位是多少」的任务。"
metadata:
  version: "1.1.0"
  upstream: ~
  downstream: unreal-material-authoring
---

# UnrealMCP 资产导入与外部文件管线 Skill

适用：本仓库 `Plugins/UnrealMCP`（bridge `127.0.0.1:55557`，MCP server 名 `unrealMCP`）。
目标：外部文件（FBX/贴图/文本）进工程、替换已有资产、批量落盘/删除时，**不崩编辑器、结果可自证**。

> 本文只写**可复用的做法与接口**；不收录具体配方。

**环境注意**：改了 `Plugins/UnrealMCP/Content/Python/**`（含工具描述、本 Skill 指向的脚本）后需**重启 unrealMCP server**；改插件 C++ 需 `Build_UnrealMCP.bat` + 重启编辑器。

---

## 〇、先问"有没有现成工具"

写 python 之前先自省：`list_mcp_commands`（可选 `category`）→ 返回每条命令的 `params`（含 `required`）与 `flags`（`loopback_forbidden` / `mutates_graph` / `persist_after_success` / `python_execution`）。已有工具能做的，**不要**在 `execute_python_*` 里手搓：

| 需求 | 用工具 | 不要 |
|---|---|---|
| 导入外部文件 | `import_assets` | `unreal.AssetImportTask` + `asset_tools.import_asset_tasks`（**崩编辑器**） |
| 建资产（名称可能已存在） | `create_asset_safe`（存在即回 `asset_exists`，`recreate=True` 静默存删） | `AssetToolsHelpers.create_asset`（同名包**弹模态框、冻结整条 bridge**） |
| 资产属性（贴图 srgb/压缩/寻址、材质参数等） | `set_asset_properties` | 手写 `set_editor_property` + 自己 save |
| 读资产自身属性 | `get_asset_properties` | — |
| 删资产 | `safe_delete_asset`(+`list_asset_blockers`) | `EditorAssetLibrary.delete_asset`（无注册表通知、会留残留） |
| 复制资产 | `duplicate_asset_safe`（默认落在源目录、名字 `<源>_Copy`；`overwrite=true` 静默先删） | 手搓 `StaticDuplicateObject`（**不做蓝图/RigVM 类特有的 fixup**：副本当次会话能编译能存盘，**重载后编译失败** —— 实测 ControlRig 副本里的 spline 节点丢 `ScriptStruct`） |
| 改资产路径 / 搬目录 | `move_asset` / `move_directory` | `EditorAssetLibrary.rename_asset`（引用者不落盘、又不留 redirector → 重启即断，见 §九） |

外部文件导入唯一入口是这套工具（**不要**手搓 `AssetImportTask`，会崩编辑器）：贴图走 `import_texture`、骨骼网格走 `import_skeletal_mesh`、动画走 `import_animation`（`skeleton_path` 必填），**其它类型与混合批处理**才用 `import_assets`。三条类型化命令与 `import_assets` 共用同一份导入核心，所以 force_legacy 门禁、模态抑制、逐文件结果、companion_assets / save_failures 两张清单全部同形。`folder` 里出现"某类型专属参数"就说明该换工具了（例：`import_assets` 收到 `skeleton_path` 会结构化拒绝，不会静默忽略）。

**脚本层（`execute_python_*` 内）的边界**——决定"工具逻辑能不能被复用"：

- 编辑器 sys.path 上**有** `Plugins/UnrealMCP/Content/Python`（含 `tools/`）⇒ 脚本可以 `import` 其中的**纯模块**。
- 但编辑器 python **没有 `mcp`** ⇒ 依赖 `mcp.server.fastmcp` 的工具模块（`tools/ue_safe_api.py` 等）**在脚本里 import 会失败**。它们的**能力**不受影响：那批实现已搬成 bridge 命令，脚本用 `unreal.UnrealMCPPythonAPI.execute_mcp_command("create_asset_safe", …)` 直接调（工具层只是同名薄转发）；只有纯 python 实现的工具（如 `preview_material_expression`）才必须走 MCP 工具面。
- 因此脚本里建/删资产优先调 **`create_asset_safe` / `delete_asset_safe`**（无模态框、有 blocker 语义）；退路才是 python API，此时**用唯一名**，或先 `does_asset_exist` → 删除 → **回读** `does_asset_exist` 再建 —— 对"可能已存在的同名包"直接 `create_asset` = 弹窗风险。

---

## 一、`import_assets`（唯一入口）

```
import_assets(paths=[绝对路径...], destination_path="/Game/...", force_legacy=true, replace_existing=false)
```

| 参数 | 语义 |
|---|---|
| `paths` | 磁盘绝对路径列表（可多个） |
| `destination_path` | 落到的内容目录；**资产名 = 文件名**（改名就是新建，不是替换） |
| `force_legacy`（默认 true） | 对请求的扩展名关掉 Interchange feature flag，**并 readback 校验**；未生效则拒绝导入（`cvar_override_failed`），一个文件都不动 |
| `replace_existing`（默认 false） | true = 覆盖同名资产；false 且同名存在 → `asset_exists` |

- 扩展名 → flag 映射：`.fbx → Interchange.FeatureFlags.Import.FBX`（其余见插件 `GetInterchangeExtensionFlags()`）。没有对应 flag 的扩展名 → `unsupported_extension`，`force_legacy` 无法兑现。
- **会话级副作用（刻意设计）**：flag 关掉后本会话不再恢复，之后同扩展名的导入继续走 legacy importer；响应里的 `cvar_overrides` 记录改了哪些。
- 返回：每个文件一条结果（形状固定），别只看整体 `status`；另外两个清单决定"这次导入真的留下了什么"：
  - `companion_assets[]`：本次导入在目标目录里**额外产出**的资产（Skeleton / PhysicsAsset / 自动材质），逐条带 `saved` —— 它们由导入任务之外的路径产生，曾经只留在内存（重启后主资产的引用就断）。
  - `save_failures[]`（顶层 `companion_save_failed_count`）：companion 没落盘**不会**把 `imported` 翻成 false，只看 `imported` 会漏。

---

## 二、替换/重导入已有资产

顺序（任一步失败就停，别继续往下覆盖）：

1. **备份源文件**：覆盖导出前 `copy` 一份到同目录（例：`Richie_UE_pre_fix.fbx`）。UE 侧不留"改名备份资产"，靠文件备份回滚。
2. **同名覆盖导出源文件**（Blender/FBX 侧），参数见 §5。
3. `import_assets(paths=[同名文件], destination_path=原目录, replace_existing=true)`，**然后读回包里的
   `equivalence{}`**（`replace_existing=true` 时默认给）：它把替换前后的 `height_cm` / `bounds_*` /
   `sphere_radius` / `slot_count` / `morph_count` / `skeleton_path` / `physics_asset_path` 各取一次快照，
   逐项给 `*_same`，再给 `bounds_delta{extent_ratio, sphere_radius_ratio, height_ratio}` 与 `warnings[]`。
   判据：比值偏离 1 超过 5% = warning；比值 <1/50 或 >50 倍 = **error**（单位重复折算的形态，实测过一次
   `global_scale` 被折算两次 → 整模型缩成 1/100，而回包仍是 `imported: true`）。
   没有同名资产时 `before: null`；同名资产读不出来时 `before_unavailable` warning（PIE 会拒某些类型）。
   **不要只看 `imported`**：导入成功与"资产还是原来那个东西"是两件事。
4. **立刻核对绑定**（§4）：SkeletalMesh 的 `skeleton` 必须仍指向原 Skeleton —— 绑错/新建骨架会让所有动画失效。
5. **覆盖导入不更新骨架**：`import_assets(replace_existing=true)` 只替换网格，**Skeleton / PhysicsAsset 保留第一次导入的数据**；要换骨架**先按 physics→skeleton→mesh 顺序删干净**再导（见 `unreal-asset-pipeline-case-bone-rename` 第 3 步的 `purge_skeletal_assets.py`）。
   - **"MMD 角色整份重导、且要求动画/ABP 继续可用"这一整套配方**（为什么网格与骨架必须同门、编辑器关闭时的文件级换骨架、重编 ABP、绕序/UV 错配与单位三个必查缺陷）：见案例 skill `unreal-asset-pipeline-case-mmd-reimport-same-pair`。
6. **导入选项（morph 之类）在覆盖导入时取自"网格资产自带的 import data"，不是任务参数**：`bImportMorphTargets` 是 `config` 属性、引擎基础 ini 里就是关的（首次导入靠给任务挂 `Options`，工具已做）；destination 已有同名网格时改的是**网格自己**那份 import data —— 打在 `"<mesh 对象路径>:FbxSkeletalMeshImportData_0"` 的 `bImportMorphTargets` 上（位域，python 的 `get_editor_property` 读不到它，用 `reflect_probe` / `set_object_property`），改完再重导。判据只有一条：重导后 `get_all_morph_target_names()` 的条数变了。
7. **要保住成品材质，只能就地重导**（`destination_path` = 资产原目录）：导到新目录时导入器按 FBX 里的材质名**在新目录新建占位材质**（各 1 个节点），不会去原目录捡同名材质。
8. 核对材质槽与资产总数没变（同名的材质/贴图按名字复用，不应产生新资产）；**复验要"槽路径 + 每槽节点数"逐条比，只比槽数会漏**。
6. 需要时再 `set_asset_properties` 收尾；写命令成功即落盘。

---

## 三、先导到临时目录做"等价性证明"

覆盖真资产前，**先导入到一个临时目录**（如 `/Game/MCP/<X>Check`）再做比对，确认"只改了我改的东西"：

- 骨架：`AnimPoseExtensions.get_reference_pose(skeleton)` 逐骨比 `translation/rotation`（LOCAL/WORLD 皆可，同一空间即可）。绑定姿势逐骨 **dT/dR ≈ 0** 才说明导出参数与上一次等价 → 骨架不会变 → 已有动画仍有效。
- 骨数：`get_bone_names(reference_pose)` 的长度（可用 `unreal.AnimPoseExtensions.get_bone_names`）。
- 网格/材质：资产清单与材质槽路径逐条比。
- 动画：`AnimationLibrary.get_num_frames` / `AnimationLibrary.get_animation_track_names` 长度 / `enable_root_motion`。
- 收尾删临时资产走 `safe_delete_asset`（只删无引用的，别在同一 job 里同名重建）。

**权重类改动没有 python 读接口**（`SkeletalMesh` 不暴露 skin weights）：自证只能到"源头 + 导入等价"为止 —— 源头用 Blender 侧 vertex group 普查（改名/迁移后的顶点数与权重和），UE 侧证明"骨架没变、动画没坏"，最终形态由用户看视口验收。

**同机位 A/B 是唯一能证明"改了权重/几何的导出真的到了视口"的手段**：`take_screenshot` 回包带 `camera{location, rotation, fov, viewport_size}`，**两次截图的 `camera` 相等才可比**；不等就说明视口被挪过，此时差异不可归因于改动（实测发生过一次，差异全被相机位移吃掉）。截图本身仍不是验收结论。
- 要拍的不是关卡、而是某个资产编辑器自己的预览视口（Persona / 材质编辑器 / 预览视口）时用 `source='asset_editor'`，可配 `asset_path` 先把那个资产编辑器聚焦再拍（未打开回 `editor_not_open` + 退路，**不代开**）；`source='level_viewport'` **永远只拍关卡视口**（活动视口仍是关卡视口，拍 Persona 会拍到关卡画面），`source='pie'` 才是含 UMG 的 PIE 视口。
  - **UE 导入时会归一化蒙皮权重** ⇒ 「权重和 ≠ 1」**不是缺陷通道**：本项目实测把 24595/37757 个和≠1 的顶点（最大 2.0）改成 sum=1 后重导，`height_cm`/`bounds`/槽位/morph 全等且**视口零变化**。改权重和 = 空操作，别把它当病因。
  - **要证明"改权重真的会到视口"，做一次夸张自证**（比反复微调省时间）：挑一小组顶点 → 权重 100% 绑到另一根骨 **且**几何外移几 cm → 导出重导 → 同机位前后截图对比。本项目实测 362 个顶点这么改后肘部出现明显台阶、还原后消失，证明「Blender → FBX → 就地重导 → 视口」链路是通的。**没有这条自证，后面每一次"没效果"都无法区分"改动无效"与"链路没通"。**

---

## 四、`get_asset_properties` 能核对的硬字段

`get_asset_properties(asset_path, asset_name)` 回资产自身反射属性；SkeletalMesh 至少核对：

- `skeleton`（必须是原 Skeleton 对象路径）
- `materials`（槽位清单，逐条路径）
- 动画资产：帧数/长度/轨道数/`enable_root_motion`/`get_root_motion_at_time` 采样

**网格健康另有专用体检**：`inspect_skeletal_mesh(asset_path)` —— 一次拿到

- `skeleton_consistent`：网格根骨能否在骨架里按名字找到。**为 false 时引擎不建 AnimInstance**，角色表现为参考姿势/T 字，而编译、引用扫描、导入回执全都不报错（唯一线索是 `LogAnimation` 的 Verbose 一行 `… : Missing joint on skeleton`）。角色"T 字/不动"先看这一条，再去看 AnimGraph。
- `root_bone{name,scale,translation}` / `bone_count`：**导入期会把节点名里的 `.` 换成 `_`**（`root.001` → `root_001`），所以"Blender 里对象名被加了 `.00x` 后缀"会静默改 UE 侧骨名，进而让新网格与既有骨架对不上。判据是把网格/骨架两侧的 `bone[0]` 与骨名集合都读出来比。
- `has_embedded_scale` / `bone_local_unit`：骨架有没有内嵌 ×100（`meters` vs `cm`）。
- `slots_without_texture`：白膜签名（BaseColor 没有贴图支撑的槽）。

---

## 五、外部文件事实（导入器侧）

- **导入侧的判据只有一条：rest 姿势逐骨比对**（§三）；"导出时看着对"不算。
- **UE 读到的 cm = FBX 里写的数值 × 100**（`FbxMainImport.cpp:1558` 的 `bConvertSceneUnit`：FBX 声明米 → `FbxSystemUnit::cm.ConvertScene` 全场景 ×100）。
  `apply_unit_scale` / `scale_length` / 单位系统那套**对数值的影响不可靠**，只信导入后实测。
- **骨架有没有内嵌 scale，只能靠"一根骨的局部平移量级"判**（`get_reference_pose` + `get_bone_pose(..., LOCAL)`，见 `unreal-retarget-authoring` §二.6）；
  `mesh.get_bounds()` 高度作交叉验证。动画-only 导入**必须**带 `skeleton_path`。
- **"上次的导出参数"不可信**：Blender 的 `operator_properties_last('export_scene.fbx')` 可能回默认值，与真实导出不符。
- **Blender 侧怎么产出这个文件**（mmd_tools 参数、×100 烘焙与 `global_scale` 标定、D 变形骨权重、shape key 两块都要缩、VMD → 动画 FBX 四条纪律、导出参数表）
  已拆成独立 skill：`blender-case-mmd-pipeline`；Blender MCP 用法与 bpy 纪律见 `blender-authoring`。

**本 skill 的 `scripts/`（都是"改顶部参数即可重跑"的模板，都跑在编辑器 python 侧）**：

| 脚本 | 做什么 |
|---|---|
| `dump_skeleton_baseline.py` | **改名前**：旧骨架逐骨 rest 姿势（LOCAL t/r）落盘，作为等价性比对基准 |
| `verify_bone_rename_equiv.py` | 改完导入后：用 `rename_map.json` 逐骨比对"只改名没动数据"，顺带体检高度/材质槽；给出 `PASS/CHECK` |
| `check_imported_skeleton.py` | 导入后自检：mesh 高度对不对 + **骨架有没有内嵌 ×100 scale**（局部平移量级判据），给出 `CLEAN_CM` / `EMBEDDED_x100_SCALE` 结论 |
| `purge_skeletal_assets.py` | 彻底删一套 mesh/skeleton/physics（先干掉引用它们的关卡 actor → 按序 `delete_asset` → 清残留 `.uasset` → `scan_paths_synchronous`），供"重导换骨架"用 |

> Blender CLI 侧的三个脚本（`blender_pmx_to_ue_fbx.py` / `blender_mmd_rename_ascii.py` / `blender_vmd_to_ue_anim_fbx.py`）
> 已随案例搬到 `blender-case-mmd-pipeline/scripts/`。
> **这一节已拆成独立 skill**：`unreal-asset-pipeline-case-bone-rename`（骨名 ASCII 标准化）。
> **这一节已拆成独立 skill**：`unreal-animation-case-joint-transition-band`（关节过渡带的权重侧做法）。
> **反方向（从第三方 UE 游戏的 cooked 包往外拆资产）**：容器解包、缺 `.usmap` 时从能跑的游戏 dump、贴图 PNG / JSON 导出，见案例 skill `unreal-asset-pipeline-case-thirdparty-game-extraction`。

## 六、执行模型：一律同步（没有 deferred/job）

- `execute_python_command` / `execute_python_file` **只有同步一种模式**（原先的 deferred 参数与配套的轮询工具已从工具面、命令面、C++ 一并移除）。调用的结果（含结构化错误）**在本次回包里返回**，返回后编辑器立刻能继续响应其它命令。
- 因此**一次调用必须能在客户端工具超时（≈90s）内跑完**：重活（几十个资产的导入/重编/写盘）拆成**多次短、幂等、可重跑**的调用；只是"慢一点"就用 `timeout=<秒>`（bridge 侧 5–600s）拉长等待。
- 拆批的粒度判据：一批的耗时估算要明显低于 90s（例如每批 20–30 个资产），批与批之间用回包里的清单核对（`imported_count` / `failed_count`），不要靠"应该成功了"。
- 编辑器侧**没有后台**：调用跑在 GameThread 上，期间编辑器不响应别的命令（也别在 python 里 `sleep` / 轮询）。终端侧（Blender CLI、编译、下载）才用后台任务。

### 附：样例脚本（`scripts/` 之外的临时脚本）怎么落

长脚本写到 `Saved/MCPScripts/*.py` → `execute_python_file(file_path=..., timeout=...)` 同步跑；脚本内 `print()` 的中间状态会随回包一起回来。

---

## 七、生命周期与错误码

- 写命令成功即落盘；仍建议 `does_asset_exist` + 磁盘 mtime 复核。
- `safe_delete_asset`：`deleted` 只在盘上文件与内存对象都消失时为 true；失败给 `detail`/`blockers`。删之前被引用问 `list_asset_blockers`。
  删前还有一道 **World Partition 守卫**：① referencer 里出现 `/__ExternalActors__/` 或 `/__ExternalObjects__/` 包
  ⇒ `world_partition_referenced`；② 任一已加载世界的 `UWorldPartition` 已初始化（= 正开着 WP 关卡）⇒ `world_partition_open`。
  两种情况**什么都不删**（内存与包文件都不动），响应给 `world_partition_open`、`workarounds`（Fix Up Redirectors / 关编辑器删磁盘 / 换非 WP 关卡）
  与 `detail`；`force=true` **不豁免**（force 只断引用，崩的是随后的 GC —— `WorldPartitionSubsystem.cpp:507` 的断言）。
  即：**开着 WP 关卡时这条命令整体不可用**，脚本里要把它放到换关卡/收尾阶段。
- **`force=true` 且有 blockers 时先跑只读前置探测**，命中即拒绝并**保持零改动**（不断引用、不标垃圾、不写盘）：响应为 `{deleted:false, reason:"would_fail_*", detail:"什么都没动", workarounds:[...]}`、`detached: []`。三类命中：目标包文件只读 / 该资产仍开着资产编辑器 / **存在够不到的活引用者**（`unreachable_references[]`：引用者的包落在"本命令能清空的范围"（`blockers` + 目标包）之外，且不属于缩略图那类瞬态包）。判据取**真实内存引用图**，不是文件锁 —— 加载中的包本来就映射着 `.uasset`，"文件被占"是删除前的正常状态，用文件锁当判据会把 `force` 变成空操作。
- **`dry_run=true`（只读）**：给 `would_delete` / `would_fail`(+`would_fail_reason`/`would_fail_detail`) / `would_clear[]`（每项含 referencer 路径与将被清空的属性**路径**，形如 `would_clear_property:<属性路径>`，能看到 AnimGraph 内部路径），且 `detached` 恒为 `[]`（计划不进 `detached`）。它**穿透 World Partition 前端检测**（该检测针对会崩编辑器的真删除，干跑不删任何东西）⇒ 在"WP 关卡常开"的工程里这是唯一还能给出答案的形态。
- **`force=true`** 通过探测后忽略 `blockers`、先断开引用再删，`detached[]` 列出被断开的引用者：关卡内以该资产为类的 actor 实例、资产自身指向目标的引用、以及**嵌套在结构体与结构体数组里的对象引用**（例如蒙太奇段的 `CompositeSections[i].LinkedSequence` / `AnimSegments[j].AnimReference`）—— 走查不递归进 struct 时这些清不掉，会出现"删除成功但引用者仍指向已删资产"。
- **所有未删成功的结局都带 `workarounds[]`**（关编辑器后同名覆盖 / 删磁盘包文件再重启、先把引用者换走或改名、关掉该资产的编辑器后重试），不要只回一个裸 `false`。
- 目标本身是 `UObjectRedirector` 时改走引擎 `IAssetTools::FixupReferencers`，用 `redirection_fixed` 与 `redirector_path`（`none` / `engine_fixup` / `plugin_fallback`）说明实际走的是哪条。
- **走引擎 fixup 那条路会弹一个不可抑制的模态窗，而且工作其实在弹窗前就做完了**：`FixupReferencers` 结尾**无条件**弹 `SFixupRedirectorsReport`（`SModalEditorDialog<bool>`，引擎 `AssetFixUpRedirectors.cpp:938-939`，两种 fixup 模式都会走到），它占住 GameThread 直到有人点确认 ⇒ 回执必然超时丢失、整条 bridge 冻住，而引擎侧**已经把引用者包载入、改写并存盘完毕**（实测夹具：一次调用挂了 **139.31s**，其中真正的工作只有 **0.23s**，其余全在等人点）。它**不经过** `FCoreDelegates::ModalMessageDialog` ⇒ unattended 守卫与弹框策略**都拦不住**；`dry_run` 也不覆盖这条分支（实测 `dry_run=true` 仍改了引用者、删了 redirector）。做法：① 不要把这条路径排进批处理或指望回执；② 要证据就先落盘起止时间/journal，再从 `Saved/Logs/*.log` 的时间窗取回读数（日志里的 `Saving Map:`/`LogSavePackage` 行会精确标出工作段与等待段的分界）。
- **`duplicate_asset_safe` 的验收判据是「重启后还能编译/还能用」**：它走引擎自己的复制路径（`IAssetTools::DuplicateAsset`，Content Browser 用的那条），蓝图/RigVM 类因此拿到类特有的 fixup；那条路唯一的风险是 `CanCreateAsset` 会 **fully-load 目标包**（脏包会弹模态），所以**目标包必须先判空**再调。存在性探测用 `FindAsset` 而**不是** `DoesAssetExist`（后者对"文件刚被删掉的包"会留 phantom 条目 ⇒ 误报 `asset_exists`；`FindAsset` 拒绝解析没有包文件的注册表条目）。`overwrite=true` 静默先删，删不掉回 `delete_pending` + 内嵌 `delete_result`。校验副本时**当次会话能编译不算数 —— 重启编辑器再编译一次**，那才是区分它和 `StaticDuplicateObject` 的判据。
- **探针夹具的"建"与"删"不要放在同一会话**：在同一个 session 里把刚建的资产删掉（尤其再同名重建），会留下 redirector / 坏包（日志里表现为 `<pkg>.uasset: Error opening file` 与 `寻找对象"ObjectRedirector …"失败`），随后**内容浏览器/TypedElement 刷新时会 AV 崩编辑器** —— 崩溃栈在 GameThread 的 TypedElement/UI 路径上，应用侧只看到"命令都返回成功了"。`safe_delete_asset(force=True)` 的 `force` 只绕过**引用检查**，绕不过悬空引用。做法：探针换新目录名建、删除留到下次会话；纯文件备份放 `Saved/`（不参与资产注册表）可随时删。批量删完也不要再接重活，先查 redirector 再继续。**清理顺序很关键**：要清掉一个 redirector，必须**先处理 redirector、再删它的引用者** —— 反过来（先删引用者）会留下"引用者已死"的孤儿 redirector；**如果紧接着在同一个脚本 / 同一轮派发里触发 fixup，编辑器会在引擎 AssetTools 里 AV 崩**（实测栈：`FAssetFixUpRedirectors::ExecuteFixUp` 的删除段 → `ObjectTools::DeleteObjects`）。**只要间隔开就不会崩**（实测：同一序列拆成两次 MCP 调用、编辑器跑过 tick、GC 收掉待删包 ⇒ 正常完成）—— 所以纪律是"删完别在同一 tick 里接着修"，不是"别删引用者"。
- **不要用「删除 + 改名替换」的方式替换被引用的资产**：引用会**静默断掉** —— 本项目实测替换 `Fei_Idle` 后，AnimBP 的 `AnimGraphNode_SequencePlayer.Sequence` 与 BlendSpace 的 `FBlendSample.Animation` 都变成 `None`，而命令回执、`save_asset`、甚至 `compile_blueprint` 的 `BS_UpToDate` **全都不报错**（BS 侧只在编辑器 log 里出一句"混合空间 X 拥有一个无/无效动画的样本"）。替换后**逐项回读引用者**：AnimBP 序列播放器 / BlendSpace 样本 / 蒙太奇 / 关卡 Actor 的组件。要做"换一份数据"，优先**同名覆盖重导**（网格）或直接改资产内容，而不是删了重建。
- **不要在同一 job 里删掉刚创建的资产再同名重建**。要重来就换名/换目录，删除留到编辑器关闭后走文件系统。
- 常见错误码：

| 码 | 触发 |
|---|---|
| `unsupported_extension` | 该扩展名没有 Interchange flag，`force_legacy` 无法兑现（响应带受支持清单） |
| `cvar_override_failed` | flag 关不掉（readback 仍开），**拒绝导入**，什么都没做 |
| `asset_exists` | 同名存在且 `replace_existing=false` |
| `load_failed` / `invalid_params` | 路径不可读 / 参数非法 |
| `load_failed_in_pie` | **PIE 运行中**、该路径在资产注册表里、但加载不出来（部分资产类型 play mode 不加载）⇒ 先 `stop_pie` 再读。与"资产真的不存在"（`load_failed`）是两个码，别混。每条命令的回包都带 `editor_state{pie_running, simulating_in_editor, world, level_name}`，判断环境不用再单独问一次 |

---

## 八、工具速查

| 工具 | 用途 | 关键点 |
|---|---|---|
| `import_texture` | 导入贴图（png/tga/jpg/jpeg/bmp/exr/hdr/psd/dds）**并一次把属性配好** | `srgb` / `compression` / `compression_quality` / `lod_group` / `no_alpha` / `mip_gen` / `filter`（**只写显式传入的项**，逐项 before→after 回读在 `results[i].properties`） |
| `import_skeletal_mesh` | 导入骨骼网格（fbx/obj） | `skeleton_path`（省略=新建）/ `create_physics_asset` / `physics_asset` / `import_morph_targets`；companion（骨架/物理资产/材质）照旧落 `companion_assets` |
| `import_animation` | 把动画导到**既有骨架**上 | `skeleton_path` **必填**（缺 → `missing_skeleton`，不导入任何东西）；`import_mesh` / `import_morph_targets` / `override_animation_name` / `frame_rate` |
| `import_assets` | 基础入口：**其它类型 + 混合批处理** | `paths` / `destination_path` / `force_legacy` / `replace_existing` / `inspect_materials`。**不再接受** `skeleton_path` / `import_mesh`（传了返回 `unsupported_parameter` + `moved_to: import_animation`，且不导入任何文件） |
| `set_asset_properties` | 资产属性批量写 | 逐项 `applied`/`failed`，不回滚；先读再写 |
| `get_asset_properties` | 读资产自身反射属性 | 核对 `skeleton` / `materials` |
| `safe_delete_asset` | 删资产（带注册表通知） | 被引用会拒；`blockers` 见 `list_asset_blockers`；开着 WP 关卡时整体拒（`world_partition_open`/`world_partition_referenced`，`force` 不豁免） |
| `list_asset_blockers` | 查引用者 | 删之前先跑 |
| `list_broken_references` | 查**我引用谁而它已经不在**（反查） | `scope` + `kinds`（blendspace / anim_blueprint / anim_montage / skeletal_mesh_comp）；默认只扫已加载资产（`load_missing=true` 才加载）；删/改名/替换之后**先跑它**再看别的 |
| `move_asset` | 改名 / 挪目录（唯一入口） | 默认 `update_referencers=true` + 改名**前**刷新注册表；引用者逐个重存盘；加载不到的（地图包）报 `unstorable[]` 并由引擎留 redirector；`dry_run=true` 只报告；`answer_dialogs=true` 把引擎确认框答成"继续"并记进 `auto_answered_dialogs[]` |
| `move_directory` | 整目录（含子目录）搬 | 目录**外**引用者按 `move_asset` 同规则处理；目标目录已存在即拒（不合并）；`assets[]` 是逐资产结果；`answer_dialogs` 同 `move_asset` |
| `list_mcp_commands` | 自省命令表 | 写 python 前先跑 |
| `execute_python_file` | 落盘脚本（>30 行） | 同步；重活拆批，慢活用 `timeout<=600` |
| `inspect_skeletal_mesh` | 网格健康体检（只读） | `skeleton_consistent`（false ⇒ 引擎不建 AnimInstance、角色 T 字且不报错）、`root_bone`/`bone_count`、`has_embedded_scale`/`bone_local_unit`、`slots_without_texture` |
| `take_screenshot` | 截图排查（**不作验收结论**） | `source='level_viewport'`(默认) / `'pie'`(含 UMG) / `'asset_editor'`(+`asset_path` 聚焦该资产编辑器，未打开回 `editor_not_open`+退路)；回包带 `camera`，**同机位两次才可比** |

---

## 九、改资产路径：只用 `move_asset` / `move_directory`

改资产路径只用 `move_asset` / `move_directory`；**不要**在 `execute_python_*` 里直调 `EditorAssetLibrary.rename_asset`（引用者不落盘、不留 redirector）。

- 命令做的事：改名前按 `search_paths`（默认 `/Game`）刷新注册表 → 用引擎自己的改名的路径移动 → 引用者 = 注册表命中 ∪ 已加载包里指向目标的硬引用 → 能加载的**无条件**重存盘（引用者被修好后**不脏**，dirty-only 会漏）→ 加载不到的（地图包）不加载，报 `unstorable[]`，引擎在旧路径留 redirector。
- 看这几个字段判结果：`referencers_saved[]` / `referencers_failed[]` / `unstorable[]` / `redirector_left` / `old_path_resolves` / `on_disk_after`。
- 地图引用者只有 redirector 兜底：`redirector_left=true` 时旧路径仍可解析，但**没有被重写**；后续若做 "Fix Up Redirectors" 而地图未加载/未存盘，仍可能断。需要地图引用者干净，得先把地图加载并保存。
- 移动/改名**不做覆盖**：目标已存在直接拒（`destination_exists`），没有 `force` / `replace_existing`。redirector 占着旧路径时**同名新建也会被拒** —— 要复用旧名先删 redirector。
- **redirector 记的是路径快照**：它只记住创建时的目标路径。**只在引用者可见（注册表命中或已加载）时才能保护它**；引用者既不在注册表也没加载时无法发现，这是本机制的边界。
- **弹框策略**：这两条命令底下会走引擎自己的改名路径（`UEditorAssetSubsystem::RenameLoadedAsset` → `IAssetTools::RenameAssets`）。该路径**可能**弹确认框（CDO 引用一类，来自另一项目的实测记录；**本工程用"蓝图 CDO 持有被改名资产的类引用"复现过一例，未触发** ⇒ 触发条件尚不明确），而模态框占住 GameThread ⇒ 整条 bridge 冻到有人手点。所以每次调用前先定策略，回执里用 `dialog_policy` 报出来：
  - 默认 `unattended_defaults`：确认框返回**引擎默认值**、不建窗口，**保证不冻**；但确认框的默认值通常是"中止"，于是改名失败 ⇒ 回 `rename_failed` + `hint`。此时**"没冻"不等于"成功"**，按失败处理。
  - `answer_dialogs=true`：答成"继续"，每次回答（类型/标题/正文/答案）都在 `auto_answered_dialogs[]` 里。**这是真决定** —— 若弹出的是签出提示，等于替你签出，所以默认不开。`rename_failed` 时优先拿它重跑看被答了什么。
  - 编辑器自身以 `-unattended` 启动时该机制不生效，`dialog_policy_note` 会说明。
