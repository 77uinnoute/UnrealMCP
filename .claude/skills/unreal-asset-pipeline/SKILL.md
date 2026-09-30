---
name: unreal-asset-pipeline
description: "UE5（UnrealMCP）里把「外部文件 → 工程资产」这件事做对的 Skill：为什么外部文件导入必须走 import_assets（force_legacy 关 Interchange + readback，手搓 AssetImportTask 会直接崩编辑器）、替换/重导入已有资产（同名覆盖、保持 Skeleton 绑定与材质重链）的正确顺序、导入后能用来自证的硬证据（临时目录导入 + rest 姿势逐骨比对、动画/材质槽/骨数核对、权重类改动无 python 读接口时的验收路径）、导入骨架的 track 平移单位与竖直轴不遵循 UE 约定（实测 MMD 版：单位=米、世界向上=父骨局部 −Y）以及写平移轨道前必须先标定、资产生命周期（safe_delete_asset / list_asset_blockers / 别在同一 job 删了再同名重建）、Blender→UE 的 FBX 导出参数与「别猜参数、用比对反证」的做法、以及 deferred python job 的边界（默认 sync、只有超过客户端 ~90s 才 deferred、job 文件只在结束时出现）。触发场景：准备调用 import_assets / import_texture / import_skeletal_mesh / import_animation / set_asset_properties / get_asset_properties / safe_delete_asset / list_asset_blockers / list_mcp_commands / execute_python_file（涉及导入、覆盖、批量落盘时）/ poll_python_job 之前；以及任何涉及「把 FBX/贴图/外部文件导进 UE、替换或重导入已有资产、覆盖导出后重新导入、导入崩编辑器、资产没落盘、删资产、判断某个操作该不该用 deferred、导入骨架的平移通道该写哪根轴/单位是多少」的任务。"
metadata:
  version: "1.1.0"
  upstream: ~
  downstream: unreal-material-authoring
---

# UnrealMCP 资产导入与外部文件管线 Skill

适用：UE 5.5 + 本仓库 `Plugins/UnrealMCP`（bridge `127.0.0.1:55557`，MCP server 名 `unrealMCP`）。
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
- 返回：每个文件一条结果（形状固定），别只看整体 status。

---

## 二、替换/重导入已有资产

顺序（任一步失败就停，别继续往下覆盖）：

1. **备份源文件**：覆盖导出前 `copy` 一份到同目录（例：`Richie_UE_pre_fix.fbx`）。UE 侧不留"改名备份资产"，靠文件备份回滚。
2. **同名覆盖导出源文件**（Blender/FBX 侧），参数见 §5。
3. `import_assets(paths=[同名文件], destination_path=原目录, replace_existing=true)`。
4. **立刻核对绑定**（§4）：SkeletalMesh 的 `skeleton` 必须仍指向原 Skeleton —— 绑错/新建骨架会让所有动画失效。
5. **覆盖导入不更新骨架**：`import_assets(replace_existing=true)` 只替换网格，**Skeleton / PhysicsAsset 保留第一次导入的数据**；要换骨架**先按 physics→skeleton→mesh 顺序删干净**再导（见 §5.1 第 3 步的 `purge_skeletal_assets.py`）。
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

---

## 四、`get_asset_properties` 能核对的硬字段

`get_asset_properties(asset_path, asset_name)` 回资产自身反射属性；SkeletalMesh 至少核对：

- `skeleton`（必须是原 Skeleton 对象路径）
- `materials`（槽位清单，逐条路径）
- 动画资产：帧数/长度/轨道数/`enable_root_motion`/`get_root_motion_at_time` 采样

---

## 五、外部文件事实（FBX / Blender 侧）

Blender 导出给 UE 的实测参数：`axis_forward='-Z'`、`axis_up='Y'`、`add_leaf_bones=False`、`use_armature_deform_only=False`、`use_mesh_modifiers=True`、`mesh_smooth_type='FACE'`、`path_mode='COPY'`、`embed_textures=True`、`bake_anim=False`、`object_types={'ARMATURE','MESH'}`。

- **不要去"猜上次的导出参数"**：`bpy.context.window_manager.operator_properties_last('export_scene.fbx')` 可能给回默认值，与真实导出不符。判据只有一条：**导入后 rest 姿势逐骨比对**（§3）。
- 单位：PMX/MMD 内部尺度 ≈ 8cm/单位、Blender 是米、UE 是 cm。导入 scale 与角色实际高度要实测记录。
- **MMD→UE 的单位必须用"黑盒标定"定下来，别推公式**。标定关系（Blender 5.2 + UE 5.5，`axis_forward=-Z/axis_up=Y`、`add_leaf_bones=False`）：
  - **UE 读到的 cm = FBX 里写的数值 × 100**（`FbxMainImport.cpp:1558` 的 `bConvertSceneUnit`：FBX 声明米 → `FbxSystemUnit::cm.ConvertScene` 全场景 ×100）。`apply_unit_scale` / `scale_length` / 单位系统那套**对数值的影响不可靠**，只信导入后实测。
  - 标定姿势：改一个参数 → 导出 → `import_assets` 到**检查目录** → 量 `mesh.get_bounds()` 高度 + 一根骨的**局部平移**（`get_reference_pose(sk)` + `get_bone_pose(..., LOCAL)`，见 `unreal-retarget-authoring` §二.6）。**局部平移是判定"骨架有没有内嵌 scale"的唯一硬指标**。
  - **`global_scale` 是唯一可控的整体倍数**：配方 = mmd_tools `import_model(scale=0.08)` → 把 ×100 **烤进数据**（`v.co *= 100`、Edit Mode 里 `eb.head/tail *= 100`）→ `export_scene.fbx(..., global_scale=0.01, apply_unit_scale=False)` → UE 里**骨局部平移是 cm**。
- **导出前把 MMD 的"付与/D 变形骨"权重并到 FK 骨**（UE 没有付与机制，权重留在 `足D/ひざD/足首D/足先EX` 上 = 死皮，腿不动）：在 Blender 里按顶点把 `*D` 组的权重累加进对应 FK 组（`足D.L→足.L`、`ひざD.L→ひざ.L`、`足首D.L→足首.L`、`足先EX.L→つま先.L`），总权重守恒。**这是模型侧修复，重定向侧怎么调都救不了。**
- **带 shape key（morph）的网格，×100 必须同时打给 `key_blocks` 与 `mesh.vertices`**：只缩 `mesh.vertices` 时，Blender 的权威顶点数据仍在 key 块里（米），导出的 morph delta 变成 `shape(米) − base(厘米) ≈ 整个身高` —— UE 导入**不报错**，只在"有曲线真的驱动它"时才表现为**整个角色被撕开**。导出前必须自检：任一脸部 morph 的 `max|delta| / 体型 ≤ 5%`（判据与单位无关；实测正解 3.8%，坏解 99%）；本 skill 的两个模板脚本已内置该断言。
- **等价性证明要覆盖 morph 与槽序**：重导模前先导临时目录比对"高度 / morph **名序** / 材质槽**名序** / 骨架**骨名序** / 逐骨 LOCAL rest 平移"，全等再覆盖真网格。


**本 skill 的 `scripts/`（都是"改顶部参数即可重跑"的模板）**：

| 脚本 | 跑在哪 | 做什么 |
|---|---|---|
| `blender_pmx_to_ue_fbx.py` | Blender CLI（`--background --python`） | PMX→FBX 一站式：mmd_tools 导入(scale=0.08) → D 骨权重并到 FK 骨 → ×100 烤进顶点与骨骼 → `global_scale=0.01, apply_unit_scale=False` 干净导出（+ 贴图聚到 `<NAME>.fbm/`） |
| `blender_mmd_rename_ascii.py` | Blender CLI | 同上 + **骨名 ASCII 标准化**（映射表/模式规则、同步顶点组与 mmd 字符串引用、dump `rename_map.json` 与新骨表、备份改名前 FBX） |
| `dump_skeleton_baseline.py` | 编辑器 python（**改名前**） | 旧骨架逐骨 rest 姿势（LOCAL t/r）落盘，作为等价性比对基准 |
| `verify_bone_rename_equiv.py` | 编辑器 python（改完导入后） | 用 `rename_map.json` 逐骨比对"只改名没动数据"，顺带体检高度/材质槽；给出 `PASS/CHECK` |
| `check_imported_skeleton.py` | 编辑器 python | 导入后自检：mesh 高度对不对 + **骨架有没有内嵌 ×100 scale**（局部平量级判据，见上），给出 `CLEAN_CM` / `EMBEDDED_x100_SCALE` 结论 |
| `purge_skeletal_assets.py` | 编辑器 python | 彻底删一套 mesh/skeleton/physics（先干掉引用它们的关卡 actor → 按序 `delete_asset` → 清残留 `.uasset` → `scan_paths_synchronous`），供"重导换骨架"用 |
| `blender_vmd_to_ue_anim_fbx.py` | Blender CLI | PMX + VMD → **只带动画**的 FBX（骨名与 UE 侧 ASCII 骨架逐名一致，UE 侧 `import_assets` 后直接绑既有 `Skeleton`，不必走重定向）：mmd_tools 导入模型 → 导表情 VMD → 导动作 VMD → `nla.bake(visual_keying)` 烤 FK → 改名 ASCII → ×100 烤进骨骼与 location 通道 → `bake_anim=True` 导出 + FBX 自查 |

**VMD → 动画 FBX 的四条纪律**（`blender_vmd_to_ue_anim_fbx.py` 的现场结论）：

1. **骨名匹配用 PMX 原始名**：mmd_tools 导入 VMD 时是按 PMX 的 `name_j`（`右足`）去匹配的，而它导入模型时把骨显示成 `足.R` —— 所以 `SRC_VMD` 要**原始 VMD**，把 VMD 改写成显示名反而一条都对不上。
2. **先导表情 VMD，再导动作 VMD**：动作 VMD 的导入会**摘掉** `mesh.data.shape_keys.animation_data.action`（action 本体还在 `bpy.data.actions` 里）→ 之后按"含 `key_blocks[` 的 action"重新挂回 `animation_data`，否则导出的 FBX 里没有 blendshape 动画。
3. **MMD 腿是 IK 驱动，必须先烤**：`nla.bake(frame_start..frame_end, visual_keying=True, clear_constraints=True, use_current_action=True, bake_types={'POSE'})` 把 IK 解算结果落成 FK 通道，否则进 UE 是一堆无效键。
4. **导出必须 `bake_anim=True`**（关着时导出器只写一个空的 AnimStack，一条骨骼曲线都不写），且导出后自查 FBX 里的 `AnimCurveNode` / `AnimationCurve` / `BlendShapeChannel` 计数——不然会把空导出当成功。UE 侧导入动画要带 `skeleton_path`（动画-only 导入**必须**指定骨架），要连网格/morph target 一起就加 `import_mesh=true`。

### 5.1 骨名 ASCII 标准化（可选；决定不用 MMD/VMD 动作库时才做）

**收益**：`apply_auto_generated_retarget_definition()` 变得可用（自动建出 Root/Spine/Neck/Head/四肢与双手各 5 条手指链）；脚本源码里可以直接写骨名，不再需要"dump JSON + 按下标取"那套绕行（见 §八 的桥 4KB 分块解码限制）。

**代价**：与按名匹配的 MMD 动作库（VMD）脱钩；动作/曲线/骨骼引用等字符串接口都要跟着重建一次。

**流程（4 步，缺一不可）**
1. **先留基线**：`dump_skeleton_baseline.py`（旧骨架 rest 姿势）——否则改名后无法证明"只改了名字"。
2. **Blender 改名 + 导出**：`blender_mmd_rename_ascii.py`（映射表 + 模式规则；脚本会 `assert` 未覆盖项与 FName 大小写冲突，全部打印，不静默漏改）。
3. **换骨架**：`purge_skeletal_assets.py` 清 mesh/skeleton/physics → `import_assets` 新 FBX（**必须 purge**，否则踩 §二.5 的"覆盖导入不更新骨架"）。
4. **验等价**：`verify_bone_rename_equiv.py` → 期望 非 ASCII 骨名 0 根、Δt_max < 1e-4 cm、|quat_dot| > 0.9999。

**要点**
- **Blender 改 `bone.name` 会自动改名匹配的顶点组**（权重守恒）。但**要先确认**：改完打印 `BONES_WITHOUT_GROUP` / `GROUPS_WITHOUT_BONE`（后者只应剩 `mmd_edge_scale` / `mmd_vertex_order` 两个 mmd_tools 元数据组）。
- **UE 骨架的根骨名 = Blender 里"骨架对象名"**（FBX armature 节点）——不改它会留下唯一一根非 ASCII 骨。
- **映射必须做 FName 大小写不敏感去重**：`Head`/`head`、全角 `ＩＫ`/半角 `IK` 在 UE 里是同一个 FName；批量改名撞名会静默吃掉一根骨的权重/动画。
- **全角数字/字母要归一化后再匹配**：`親指０`（全角０）/`足ＩＫ`；脚本用 `unicodedata.normalize('NFKC', name)` 只做**匹配**，输出名自己生成。
- **`WeaponL/R` 这类没有 `.` 的名字**要单独处理（MMD 里只有左右，没有点）。
- **拇指多一节**：`親指０/１/２/先` → `thumb_01/02/03/tip`（其余手指是 `１/２/３/先` → `01/02/03/tip`），编号写错就会和 `親指１` 撞名。
- **`.L/.R` → `_l/_r`** 是同一批命名里的约定，别指望 FBX 的 `.`→`_` 自动转换给你一致性（它会把 `.L` 变成 `_L`，大小写与风格都不统一）。
- 改名只动**字符串**：Blender 的约束/父子是**指针**，改 `bone.name` 不会断；会断的是 Blender Action 的 f-curve 路径、驱动器、自定义属性里按名存的引用（mmd_tools 的 `additional_transform_bone` / `ik_target`）——脚本里断言 `len(bpy.data.actions) == 0` 并扫描 `REF_KEYS`。
- **导入骨架的"动画 track 平移单位 + 竖直轴"不遵循 UE 约定**：MMD 版里 track 平移单位是**米**，世界向上落在 pelvis 父骨 `root` 的**局部 −Y**（局部 Z 是水平）。任何写平移轨道（`set_bone_track_keys` / `add_bone_track`）之前先标定，别假设"局部 Z = 竖直"；标定法与实例见 `unreal-retarget-authoring` §二.6。
- 导出前先在 Blender 侧自查（命名、顶点组、朝向、权重和），UE 侧只做"是否等价"的比对。
- 纹理/材质：不 `embed_textures`（或 `import_materials=False`）会让槽位退化成空材质 → 看起来"导入成功但材质丢了"。

---

## 六、`deferred` 的边界（与 `execute_python_*` 工具描述一致）

- **默认 sync**。只有"确定超过客户端工具超时（≈90s）"才 `deferred=True`：几十个资产的批量写、重编译、重导入、批处理循环。
- 只是"慢一点"就用 `timeout=<秒>`（bridge 侧 5–600s）加宽 sync 等待，不要习惯性转 async。
- deferred 的代价：job 占着 GameThread 期间**整条 MCP 通道被冻结**（`poll_python_job` 也会被挡住）；没有进度、没有取消。
- 批量工作拆成**短、幂等、可重跑**的 job；一个 job 里别串多个重资产脚本。

---

## 七、生命周期与错误码

- 写命令成功即落盘；仍建议 `does_asset_exist` + 磁盘 mtime 复核。
- `safe_delete_asset`：`deleted` 只在盘上文件与内存对象都消失时为 true；失败给 `detail`/`blockers`。删之前被引用问 `list_asset_blockers`。
- **探针夹具的"建"与"删"不要放在同一会话**：在同一个 session 里把刚建的资产删掉（尤其再同名重建），会留下 redirector / 坏包（日志里表现为 `<pkg>.uasset: Error opening file` 与 `寻找对象"ObjectRedirector …"失败`），随后**内容浏览器/TypedElement 刷新时会 AV 崩编辑器** —— 崩溃栈在 GameThread 的 TypedElement/UI 路径上，应用侧只看到"命令都返回成功了"。`safe_delete_asset(force=True)` 的 `force` 只绕过**引用检查**，绕不过悬空引用。做法：探针换新目录名建、删除留到下次会话；纯文件备份放 `Saved/`（不参与资产注册表）可随时删。批量删完也不要再接重活，先查 redirector 再继续。
- **不要在同一 job 里删掉刚创建的资产再同名重建**。要重来就换名/换目录，删除留到编辑器关闭后走文件系统。
- 常见错误码：

| 码 | 触发 |
|---|---|
| `unsupported_extension` | 该扩展名没有 Interchange flag，`force_legacy` 无法兑现（响应带受支持清单） |
| `cvar_override_failed` | flag 关不掉（readback 仍开），**拒绝导入**，什么都没做 |
| `asset_exists` | 同名存在且 `replace_existing=false` |
| `load_failed` / `invalid_params` | 路径不可读 / 参数非法 |

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
| `safe_delete_asset` | 删资产（带注册表通知） | 被引用会拒；`blockers` 见 `list_asset_blockers` |
| `list_asset_blockers` | 查引用者 | 删之前先跑 |
| `move_asset` | 改名 / 挪目录（唯一入口） | 默认 `update_referencers=true` + 改名**前**刷新注册表；引用者逐个重存盘；加载不到的（地图包）报 `unstorable[]` 并由引擎留 redirector；`dry_run=true` 只报告 |
| `move_directory` | 整目录（含子目录）搬 | 目录**外**引用者按 `move_asset` 同规则处理；目标目录已存在即拒（不合并）；`assets[]` 是逐资产结果 |
| `list_mcp_commands` | 自省命令表 | 写 python 前先跑 |
| `execute_python_file` | 落盘脚本（>30 行） | 默认 sync；`deferred=True` 仅给超时活 |
| `poll_python_job` | 取 deferred 结果 | 会被长 job 挡住；`cleanup` 默认删文件 |

---

## 九、改资产路径：只用 `move_asset` / `move_directory`

改资产路径只用 `move_asset` / `move_directory`；**不要**在 `execute_python_*` 里直调 `EditorAssetLibrary.rename_asset`（引用者不落盘、不留 redirector）。

- 命令做的事：改名前按 `search_paths`（默认 `/Game`）刷新注册表 → 用引擎自己的改名的路径移动 → 引用者 = 注册表命中 ∪ 已加载包里指向目标的硬引用 → 能加载的**无条件**重存盘（引用者被修好后**不脏**，dirty-only 会漏）→ 加载不到的（地图包）不加载，报 `unstorable[]`，引擎在旧路径留 redirector。
- 看这几个字段判结果：`referencers_saved[]` / `referencers_failed[]` / `unstorable[]` / `redirector_left` / `old_path_resolves` / `on_disk_after`。
- 地图引用者只有 redirector 兜底：`redirector_left=true` 时旧路径仍可解析，但**没有被重写**；后续若做 "Fix Up Redirectors" 而地图未加载/未存盘，仍可能断。需要地图引用者干净，得先把地图加载并保存。
- 移动/改名**不做覆盖**：目标已存在直接拒（`destination_exists`），没有 `force` / `replace_existing`。redirector 占着旧路径时**同名新建也会被拒** —— 要复用旧名先删 redirector。
- **redirector 记的是路径快照**：它只记住创建时的目标路径。**只在引用者可见（注册表命中或已加载）时才能保护它**；引用者既不在注册表也没加载时无法发现，这是本机制的边界。
