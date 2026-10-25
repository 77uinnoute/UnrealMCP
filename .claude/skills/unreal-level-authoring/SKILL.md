---
name: unreal-level-authoring
description: "UE5（UnrealMCP）里用 MCP 工具程序化新建/保存地图、给地图补天空与光照、往关卡里放 actor 的方法：新建地图的唯一入口与它会把编辑器当前关卡切走的副作用（先查 dirty map 避免模态框）、**新建地图后必须补一套天空/光照 actor（SkyLight + DirectionalLight + SkyAtmosphere，可选 VolumetricCloud / ExponentialHeightFog），否则视口与 PIE 都是黑的**、这套 actor 可直接照抄的起手变换值、spawn_actor 与 spawn_blueprint_actor 的取舍与重名拒绝语义、关卡里放 UMG 入口 Actor 时 BeginPlay 早于 PlayerController 的坑、不靠截图的存在性断言与存盘收口，以及 **PIE 期怎么可靠判「资产在不在」（EditorScriptingUtilities 那族在 play 模式下拒答并把拒答编码成 false / None，要改用 asset_status 或 unreal.load_asset）**。触发场景：准备调用 get_actors_in_level / spawn_actor / spawn_blueprint_actor / set_actor_property / get_actor_properties / asset_status / delete_actor / start_pie / stop_pie / add_widget_to_viewport，或在 `execute_python_*` 里调用 LevelEditorSubsystem.new_level / save_current_level / EditorLoadingAndSavingUtils.* 之前；以及任何涉及「新建一张地图/关卡、地图打开是黑的、进 PIE 一片黑、PIE 起不来（`pie_blocked_by_compile_errors`）、PIE 期间判资产存在性只拿到 false / None、给新地图补天空光/平行光/大气/雾、往关卡里摆 actor、关卡怎么存盘」的任务。"
metadata:
  version: "1.0.0"
  upstream: ~
  downstream: ~
---

# UnrealMCP 关卡与光照 Skill

适用：本仓库 `Plugins/UnrealMCP`（bridge `127.0.0.1:55557`，MCP server 名 `unrealMCP`）。
目标：用 MCP 建出一张**能看**的地图：新建 → 补天空/光照 → 摆 actor → 存盘，且每一步都可回读自证。

**环境注意**：改 `Plugins/UnrealMCP/Content/Python/**`（含工具描述）后需**重启 unrealMCP server**；改插件 C++ 则需 `Build_UnrealMCP.bat` + 重启编辑器（`Editor.bat start`）。引擎目录只读。

---

## 〇、第一纪律：新建地图后必须补一套天空/光照 actor

**引擎事实**：空地图里**没有任何光源**。`LevelEditorSubsystem.new_level()` 造出来的地图只有这些系统 actor（`get_actors_in_level` 实测原文：`WorldSettings` / `Brush_0`(class `Brush`，默认地板) / `DefaultPhysicsVolume_0` / `GameplayDebuggerPlayerManager_0` / `ChaosDebugDrawActor`(class 报作 `Actor`) / `AbstractNavData-Default`）—— 没有任何 Light。所以：

> **任何"新建地图"的流程，在离开这一步之前都必须把天空与光照 actor 建出来**，否则编辑器视口与 PIE 都是全黑（并且不会有任何报错提示你）。

一套够用的最小集合（实测可用，按"Basic/光照"模板的默认值起手）：

| actor 类名 | 作用 | 起手变换 |
|---|---|---|
| `SkyLight` | 环境光（天空光），给暗部补光 | location `(0,0,0)`，rotation `(0,0,0)`，scale `1` |
| `DirectionalLight` | 主光/太阳 | location `(0,0,0)`，rotation `(-31, -14, -105)`，scale `2.5` |
| `SkyAtmosphere` | 天空盒与大气散射 | location `(0,0,0)`，scale `1` |
| `VolumetricCloud`（可选） | 体积云 | location `(0,0,0)`，scale `1` |
| `ExponentialHeightFog`（可选） | 高度雾，给远景层次 | location `(0,0,0)`，scale `1` |

建法（逐个调用，`name` 必须唯一）：

```
spawn_actor(name="SkyLight_0",        type="SkyLight",           location=[0,0,0])
spawn_actor(name="DirectionalLight_0", type="DirectionalLight",   location=[0,0,0],
            rotation=[-31, -14, -105])
spawn_actor(name="SkyAtmosphere_0",   type="SkyAtmosphere",      location=[0,0,0])
spawn_actor(name="VolumetricCloud_0", type="VolumetricCloud",    location=[0,0,0])
spawn_actor(name="ExponentialHeightFog_0", type="ExponentialHeightFog", location=[0,0,0])
```

- `type` 收的是 **AActor 子类名**（`SkyLight` / `DirectionalLight` / `SkyAtmosphere` / `VolumetricCloud` / `ExponentialHeightFog` / `PointLight` / `SpotLight` / `CameraActor` / `StaticMeshActor` …）。
- **判据**：`get_actors_in_level` 里逐类都在（回读，不看截图）；**效果（亮不亮、好不好看）由用户在编辑器视口判断** —— 本仓库规则禁止用截图/渲染画面当验收。
- 建完**要存关卡**（见 §二），否则 PIE 起来还是黑的。

### 视口/PIE 全黑的排查顺序

1. 地图里有没有上面那套 actor（`get_actors_in_level` 直接看）；
2. `DirectionalLight` 的 rotation 是不是被摆成了朝下/朝外（`(-31,-14,-105)` 是正对场景的常规值）、强度是不是 0；
3. `SkyLight` 是否还在用需要烘焙的静态光照且光照数据未重建（新地图必然没烘过，`LIGHTING NEEDS TO BE REBUILT` 只影响静态光，动态/可移动光仍然亮 —— 只影响观感细节，不至于全黑）；
4. `SkyAtmosphere` 缺失时天空会是纯黑背景（地平线没有渐变）；
5. 相机是不是在 `Brush` 地板下方。

---

## 一、新建地图：唯一入口与副作用

MCP 命令层没有"新建地图"命令，用编辑器 python：

```python
les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
dirty = unreal.EditorLoadingAndSavingUtils.get_dirty_map_packages()
if not dirty:                                  # 有未保存的地图就先别建，避免弹模态
    created = les.new_level("/Game/UI/Inventory/L_InventoryDemo")   # 返回 bool
```

- **先查 `get_dirty_map_packages()`**：用户当前正在编辑的地图若有未保存改动，切关卡会打断他的工作，也可能弹确认框（模态框会冻住整条 bridge）。
- **副作用：`new_level` 会把编辑器的"当前关卡"切走**。建完要提醒用户当前关卡已变，或在流程结束时切回。
- `new_level` 之后 `get_current_level().get_path_name()` 会回 `<新包名>.新关卡名:PersistentLevel`，用它确认真的切过去了。
- 存盘：`les.save_current_level()`（回 bool）+ 批末 `unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)`。
- 资产存在性用 `unreal.EditorAssetLibrary.does_asset_exist("/Game/Dir/L_X")` 判；`.umap` 也是资产，走同一个 API。

### 1b. World Partition 地图：改动要**显式存**，而且脏在别处

**实测（WP 关卡）**：摆进去的 actor **不在 `.umap` 里**，各自一个 **external actor 包**：

```python
print(unreal.EditorLoadingAndSavingUtils.get_dirty_map_packages())
# ['/Game/__ExternalActors__/test_WP/3/FK/S47KK7H6S5XMCNZ856EYBH',
#  '/Game/__ExternalActors__/test_WP/8/1F/DMLYU5KQ6QK0O900UO8YB9']
```

- ⇒ **"命令返回 success" 不代表改动进了关卡资产**。`les.save_current_level()` + `unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)` 之后脏表才会变 `[]`。
- **自证落在哪**：查 `Content/<Map>.umap` **和** `Content/__ExternalActors__/<Map>/**` 的 mtime（实测存盘时两者同一秒被写）。只看 `.umap` 会误判成"什么都没变"。
- **未保存的 spawn 会消失**：实测两次"刚摆好还在、过一会儿 `get_all_level_actors()` 里没了"（未落盘的 WP actor 被流送单元格重载回收；具体触发点是 PIE 结束还是别的还没定案）。**别把"没落盘"当"只是没写盘而已"** —— 在 WP 关卡里它同时意味着"随时会丢，而且不报错"。
- 换关卡前先 `get_dirty_map_packages()`（见 §一）：WP 的脏包是 external actor 包，**不要**用"`.umap` 没变"推断"没有未保存改动"。
- **actor 的"实例属性"可能被 WP 重载回退**：改在**实例**上的属性/变换，当场回读正确、也存过盘，但切关卡或单元格重载后**可能变回磁盘上的旧值**。
  ⇒ **要持久就改资产侧**：类的 CDO（`set_blueprint_property`，新 spawn 的实例天然继承）或组件模板；只有确实只改某一个实例时才动实例，并把它当**随时会回退**的状态用。
  ⇒ **验收判据**：实例级改动改完**切一次关卡再回读**才发现得了；只看"命令回 success + 当场回读"会假绿。


---

## 二、摆 actor：`spawn_actor` vs `spawn_blueprint_actor`

| 需求 | 用 | 关键点 |
|---|---|---|
| 摆引擎类 actor（光源、雾、相机、StaticMeshActor…） | `spawn_actor(name, type, location, rotation, scale?)` | `type` 是 AActor 子类名；`name` 必须唯一，会在当前关卡里新建 |
| 摆蓝图 actor | `spawn_blueprint_actor(blueprint_name, actor_name, location, rotation, scale?)` | 回执里 `class` 带 `_C` 后缀（`BP_InventoryHost_C`）；`scale` 由命令在 spawn 之后补写（蓝图根组件来自 SCS，spawn 变换里的 scale 会被引擎重建掉）——**不用再补一次 `set_actor_transform`** |
| 改已存在 actor 的属性 | `set_actor_property` / `set_actor_transform` / `set_actor_location_safe` | 按类走结构化读写，别手搓指针 |
| 改**组件上的**属性（灯强度、相机 FOV、组件可见性、网格） | `set_actor_property(property_name='<组件>.<属性>')`，如 `LightComponent0.Intensity` | 组件选择器 = 组件对象名**或**组件类名（`U` 前缀/`Component` 后缀可省），所以 `LightComponent.Intensity` 也能命中灯的 `DirectionalLightComponent`。`set_component_property` **不是**替代品：它写蓝图的 SCS 模板，对关卡里已放置的 actor 无效。失败回包里带 `components`（该 actor 的组件清单）与 `component_property_paths`（每条可用点号路径），照着写即可；成功回包带 `component` 与 `property_value_after` |
| 改 actor **自己持有的结构体/对象里的成员**（关卡后处理的 `FPostProcessSettings`、组件的 `RootComponent.RelativeLocation`…） | 同一个 `set_actor_property`，`property_name` 直接写路径：`Settings.bOverride_AutoExposureMethod` / `Settings.AutoExposureMethod` / `RootComponent.RelativeLocation` / `LodData[0].PhysicalMeshData.Values` | 两种读法**同时支持，顺序固定**：先当**组件选择器**（`<组件>.<属性>`，既有语义，不会改道），组件没命中才当**属性路径**（段用 `.` 分隔，`[i]` 数组下标、`[key]` map 键，中途可以是 struct / object / array / map；与 `reflect_probe` / `set_object_property` **共用同一份解析器**，所以一条路径可以先 probe 再写）。成功回包在 `property_value_after` 之外带 `resolved_property`（叶子真名）。路径第 1 段解析成功、后面失败 ⇒ 回 `unknown_property` + `path_prefix` + 该层全字段 `candidates`（**不是** `component_not_found`）；两路都不成立 ⇒ `component_not_found` + `components` + `path_error`/`path_candidates`。**注意**：`reflect_probe` 的 `target` **不认关卡 actor**（只认资产/类/子对象），所以这种写法要靠 `set_actor_property` 自己的 `property_value_after` 回读自证；位域成员（`bOverride_*`）python 也读不到 |
| 删 actor | `delete_actor(name, flush?)` | **`Destroy()` 只标 pending-kill**：立刻退出所有 actor 清单，但**名字要到 GC 才释放** ⇒ 紧接着同名 `spawn_*` 是引擎 Fatal，且存盘会把它写回来。回执带 `pending_kill` + `hint`；`flush=true` 会**无条件 GC** 并用引擎同款判据复验（`pending_kill=false` = 名字已释放、`flushed=true` 表示 GC 真跑过），但**同名重建仍要等下一次调用**（见下条） |

- **同名重建的 recipe（实测定案）**：`delete_actor` → `unreal.SystemLibrary.collect_garbage()`（或 `delete_actor(flush=true)`，它现在会真的 GC）→ **另一次 MCP 调用**里再 spawn 同名。两个前提缺一不可：① **GC 真的跑过**（没跑的话名字一直被占，连存盘都会把这个 actor 写回去）；② **重建发生在"新的一帧"** —— 即另一次 MCP 调用；同一个 `execute_python_file` 脚本里删完立刻重建，**即使 GC 跑过也撞引擎 Fatal**（`LevelActor.cpp:585` "Cannot generate unique name"）。
- **一轮（同一脚本）内重建同名：可以，靠"给僵尸改名"**（2026-10-12 实测通过）。判据只有一条 —— `ULevel::SpawnActor` 查 `StaticFindObjectFast(nullptr, Level, Name)`（`LevelActor.cpp:564`），即**名字是否被对象图里仍存在的对象占着**；而 `Destroy()` 后对象通常**不会**被 GC 收掉（python 局部变量、undo 事务都持有引用），所以名字一直占着 ⇒ 默认 `FActorSpawnParameters::NameMode == Required_Fatal`（`World.cpp:428`）的 spawn（脚本里手写的、`EditorLevelLibrary.spawn_actor_from_class` 一族）**直接 Fatal 崩编辑器**。把僵尸**改名挪开**就当场释放名字，与 GC、帧都无关：
  ```python
  bridge("delete_actor", name=NAME, flush=True)                 # 或 actor.destroy_actor()
  zombie = unreal.find_object(None, level_path + "." + NAME)     # 仍在对象图里
  zombie.rename(NAME + "_Dead")                                  # 名字当场释放
  bridge("spawn_actor", name=NAME, type="...")                   # 同脚本内立刻建，成功
  ```
  改名**不等于复活**：对象仍是 pending-kill，不回 actor 清单、不参与存盘。未实测的两条同效路径：① 反向做（临时名建 → 删旧 → 新 actor 改名成目标名，`UObject::Rename` 不走 spawn 的唯一性检查）；② 让 spawn 走引擎 `NameMode::Requested`（`LevelActor.cpp:569-571` 自动加 `_2` 后缀），代价是拿不到精确名字。
- **误用不会再崩编辑器**：三个 spawn 命令现在先按引擎同款判据（`StaticFindObjectFast(nullptr, Level, Name)`）检查名字，被占就回 **`name_taken` + `hint`**（讲清上面那套 recipe），并给 `SpawnActor` 传 `NameMode = Required_ErrorAndReturnNull` 兜底 —— 实测**同一调用内**再 spawn 同名只是被拒，编辑器照常。（这一条是"能查就别崩"的落地：以前这个形状直接 Fatal。）
- **别用 `get_all_level_actors()` 的 label 判断"东西还在不在"来清理**：对蓝图实例它可能取不到 label ⇒ 清理静默不生效、实例还活着 ⇒ 后续同名 spawn 直接 Fatal（本轮踩过）。
- **删过的 actor 不能再按名字删第二次**：它已从 actor 清单里消失，第二次 `delete_actor` 直接回 `Actor not found`（回执里没有 `pending_kill`），而名字仍在对象图里占着 ⇒ 要回收只能靠 GC（`flush=true`，或 python 侧 `unreal.SystemLibrary.collect_garbage()`），不能靠"再删一遍"。

- **`name` 重名会被拒**（不会静默改名或复制）——批量摆件时用带序号的唯一名。
- `get_actors_in_level` 回**一次**全量（`{"actors": [...], "count": N}`，每个带 name/class/location/rotation/scale），比逐个查便宜；**断言"某个 actor 在不在"就用它**。
- `spawn_*` 只关卡，**不落盘**：要留下来必须自己存关卡（§一末）。
- **写关卡的命令现在会留下痕迹了**：`spawn_actor` / `create_actor` / `spawn_particle_actor` / `spawn_blueprint_actor` / `delete_actor` / `set_actor_transform` / `set_actor_property` / `set_actor_location_safe` / `set_actor_custom_depth_safe` 每条 = **一个 undo 步**（`Ctrl+Z`，或 `execute_console_command(world, 'TRANSACTION UNDO')`），并且会把关卡包**标脏**（`get_dirty_map_packages()` 从此不再为空）⇒ 关编辑器/自动保存会真的问你要不要存，不会再出现"回包 success、关卡却干净、改动静默消失"。**但标脏 ≠ 落盘**：仍要显式存（§一末）。
- **脚本里读写"包脏不脏"一律走 `unreal_mcp_compat`**（编辑器 python 直接 `import unreal_mcp_compat`，`Content/Python` 已在 sys.path）：`is_package_dirty` / `set_package_dirty` / `dirty_packages` / `engine_version` / `class_available`。**别自己写 `hasattr` 探测** —— 引擎版本差异（5.5 的 `unreal.Package` 既没有 `set_dirty_flag` 也没有 `is_dirty`，`modify()` 只能**置**脏、5.5 没有任何 python API 能**清**脏）集中在那个文件里，新版本来了改那里。
- **PIE 运行中写关卡 = 白写**：`spawn_*` / `delete_actor` / `set_actor_transform` / `set_actor_property` / `set_actor_location_safe` / `set_actor_custom_depth_safe` 作用在**编辑器世界**，而 PIE 渲染的是它自己的副本 ⇒ 命令回 success、编辑器世界回读也对，**PIE 里画面完全不变**（两轮 A/B 截图会几乎逐字节相同）。这些命令现在会在 `pie_running=true` 时于回包里附 `warning` —— 看到它就**别拿改动后的画面做判断**：先 `stop_pie` → 改 → 再 `start_pie`。
- 任何命令收到**未声明的参数**（例如给 `take_screenshot` 传 `width`）会在回包里带 `undeclared_params` + `undeclared_params_hint`：那是"这个命令不认这个参数"，不是生效了。

### 关卡里放 UMG 入口 Actor 的坑

关卡 Actor 的 `BeginPlay` **早于 PlayerController 可用**：那时 `GetPlayerController(0)` 可能是 `None`，用它当 owning player 建出来的 `UserWidget` 即使 `AddToViewport()` 也不会真的进视口（`IsInViewport()` 为假，且不报错）。

- 可行形态：`BeginPlay → Delay(0.25) → GetPlayerController(0) → EnableInput`；控件留到**按键时**才 `CreateWidget(owner=PC) + AddToViewport`。
- 造型上像"纯取值"却带 `execute` 引脚的 K2 节点（`WidgetBlueprintLibrary.Create`、`GameplayStatics.Spawn*`、`KismetSystemLibrary.Delay`）**必须显式串 exec**，否则返回值恒为 null 而编译 0 错误。
- 界面本身的搭建与点击链路细节看 `UE-UMG编写` skill。

---

## 三、让关卡用上自己的 GameMode / PlayerController

新建地图默认用工程默认 GameMode。要给某张关卡定制（例如换成"带 UI 开关 + 鼠标指针控制"的 PlayerController），三步都能用命令做，**判据是 PIE 里读到的 PlayerController 类名**：

**第 0 步（必做）：先可靠地读出这张关卡现在就有什么，并记下来。** 覆盖 `DefaultGameMode` 会**立刻改变玩什么**（默认 pawn、HUD、GameState 全跟着换），而"这张关卡原来用的是哪个 GameMode"只有两个可信来源：

```python
world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
print(world.get_world_settings().get_editor_property("default_game_mode"))   # 真值（None = 走工程默认）
```

- **`get_actor_properties` 现在也能读到类属性**：详细变体的 `properties` 含对象/类族（值 = 资产路径字符串，如 `DefaultGameMode`），没做 JSON 序列化的族（结构体/容器/FName/FText）会列在 `omitted_properties` 里，`properties_count + omitted_count == reflected_property_count`。**但"能读到"不等于"可以只看它"**：判据仍是 python 的 `world.get_world_settings().get_editor_property("default_game_mode")`（真值，`None` = 走工程默认）。历史上这条踩坑真的出过事故：工具当年不列类属性，"没列出来"被读成"没设置"，把某关卡原本的甘雨 GameMode（pawn = 甘雨角色）覆盖成模板 GM（pawn = `DefaultPawn`），用户当场发现角色不能控了。事后靠引擎日志才定案：`Saved/Logs/*.log` 里搜 `LogLoad: Game class is '...'` 能看出**每一轮 PIE 用的是哪个 GM**（改前/改后/修好后三条都在，是最好的回滚依据）。所以：**改动前记原值，出了问题去日志里查原值。**
- `EditorLevelLibrary.get_all_level_actors()` **不返回 `WorldSettings`**（`get_actors_in_level` 返回），所以定位 WorldSettings 用后者/用 `world.get_world_settings()`。

1. **建蓝图**：`create_blueprint(name="BP_XPlayerController", parent_class="PlayerController")`，以及**覆盖用**的 GM —— **父类必须选"这张关卡现在用的那个 GM"**，而不是 `GameModeBase`：`create_blueprint(name="BP_XGameMode", parent_class="<原 GM 资产路径>")`。这样默认 pawn / GameState / HUD 全部按继承保留，只多一个 PC 覆盖；反过来用 `GameModeBase` 就等于把这张关卡的玩法换成默认玩法。
   （`create_blueprint` 只能落在 `/Game/Blueprints/`，之后用 `move_asset` 搬到目标目录；`move_asset` 回 `redirector_left: false` 说明没留 redirector。）
2. **GM 指向 PC**：`set_blueprint_property(blueprint_name=<GM>, property_name="PlayerControllerClass", property_value="<PC 资产路径>_C")` —— 回执给 `property_value_before / property_value_after`（实测 `before = /Script/Engine.PlayerController`，`after` 就是新类），可自证。
3. **关卡覆盖 GM**：`set_actor_property(name=<WorldSettings actor 名>, property_name="default_game_mode", property_value="<GM 资产路径>_C")`。
   **回执不可信**：`set_actor_property` 只给 `success` + 该 actor 的属性 dump，即使 dump 现在含 `DefaultGameMode`（路径字符串）也是**写入后**的值（新增的 `property_value_after` 同样只是那个字段的值，不证明引擎真的改了游戏模式）。判据只有两个 —— python 回读 `world.get_world_settings().get_editor_property("default_game_mode")`，以及存盘后**下一轮 PIE 的 `LogLoad: Game class is '...'`**。
4. **存盘**：改 WorldSettings 只改内存，`LevelEditorSubsystem.save_current_level()` 之后才算落到关卡资产（用 `.umap` 的 mtime 自证）。
5. **判据**：`start_pie` 之后读
   - `unreal.GameplayStatics.get_player_controller(world, 0).get_class().get_name() == "<PC>_C"` ⇒ 整条链生效；
   - **`unreal.GameplayStatics.get_player_pawn(world, 0).get_class().get_name()` 与接入前一致** ⇒ 原关卡控制的角色没被换掉（这条比 PC 那条更重要：换错 GM 时 PC 是对的角色才是错的）。`pc.get_editor_property("pawn")` 读不了（protected），用 `get_player_pawn(world, 0)`。
6. **PC 蓝图的 CDO 属性可以直接写**：`set_blueprint_property(blueprint_name=<PC>, property_name="bShowMouseCursor", property_value="True")` 实测 `false → true` 生效。但"只在某个界面打开时显示光标"这类要求必须写进运行时逻辑，不能只改 CDO。

### UMG 面板打开时"不许转视角、不许动角色"

在 `ShowPanel()` 尾部加 `SetIgnoreMoveInput(true)` + `SetIgnoreLookInput(true)`，`HidePanel()` 尾部加 `ResetIgnoreMoveInput()` + `ResetIgnoreLookInput()`（`Controller.h:357/372/361/376`，四个都 `BlueprintCallable`；`self` 引脚接图上的「自引用」节点即可，与既有 `Create`/`SetInputMode` 的接法一致）：

- **隐藏侧要用 `Reset*`，不要用 `Set*(false)`**：`Set*` 是**堆叠**语义（连续两次 `true` 需要两次 `false` 才解开），而 `Reset*` 幂等 —— 万一 `ShowPanel` 被连调两次，一次 `HidePanel` 也能清干净。
- **别改成 `SetInputMode_UIOnlyEx`**：UIOnly 会把键盘也交给 UI，用来开关面板的那个键（`I`）就再也到不了 PlayerController 的输入组件 —— 等于拆掉自己的关闭键。要"UI 可点 + 键还灵 + 游戏输入被屏蔽"，就是 `GameAndUI` + 忽略 move/look 这一组。
- 判据（无需模拟输入）：`pc.is_move_input_ignored()` / `pc.is_look_input_ignored()`（都 `BlueprintCallable`）在显示态为真、隐藏态为假。

- 改 GameMode 之前先确认**当前关卡就是目标关卡**（`LevelEditorSubsystem.get_current_level()`）：`new_level` 会把编辑器的当前关卡切走。
- 关卡里的开关蓝图放在哪：**优先放在 PlayerController 蓝图**（它自带输入组件、且 `bShowMouseCursor` 这类属性只有它自己能设），细节看 `UE-UMG编写` skill §八.7。

---

## 四、PIE 相关

- `start_pie` / `stop_pie` 都是**只发请求、立刻返回**：PIE 在后面的帧才起来。**不要在同一次派发里等它**（脚本占着 GameThread，PIE 永远起不来）—— 拆成"这次派发 start_pie →（真实时间过去）→ 下一次派发断言 → 再一次派发清理"。
- **PIE 起不来先看 `pie_blocked_by_compile_errors`**：只要有任何蓝图处于 `BS_Error`，引擎就**拒绝启动 PIE 并弹模态框**问"是否强制"；插件里**没有任何全局模态抑制**（唯一设 `GIsRunningUnattendedScript` 的地方在 asset 域），所以从外面看就是"静默什么都不做"（`pie_running` 永远 false、日志里也没有别的线索）。`start_pie` 现在会先做这个预检并回 `success:false` + `compile_error_blueprints[]`（含资产路径与节点错误）+ `log_errors_tail[]`（`LogBlueprint: Error` / `[AssetLog]` 行 —— **RigVM/自定义编译器的真实错误文本只在日志里**，节点自带的 `ErrorMsg` 很短）；确要强起传 `force=true`。
- PIE 运行期间不要删夹具资产（`safe_delete_asset` 有 `pie_running` 守卫，整调用拒），清理一律放 `stop_pie` 之后。
- PIE 期间 `unreal.EditorAssetLibrary.*`（`does_asset_exist` / `load_asset` / `list_assets` 等**全部入口**）**一律拒答，并把拒答编码成假值**（`false` / `None` / 空数组）。根因：`UEditorAssetSubsystem` 的每个入口都以 `if (!EditorScriptingHelpers::CheckIfInEditorAndPIE() || !EnsureAssetsLoaded()) return <中性值>;` 开头，而前者 = `IsInEditorAndNotPlaying()`，在 `GEditor->PlayWorld` 非空时恒为 false。⇒ 它的 `false` **区分不了**"资产没了"和"我不能回答"。
  - **别拿它当存在性门禁**：`if not does_asset_exist(p): create_asset(p)` 会走进创建分支 ⇒ 引擎弹**重名模态框** ⇒ GameThread 被占，连只读命令都超时，必须人工关框。
  - **正解（按用途选）**：判存在性 → 命令 **`asset_status`**（回 `in_memory` / `on_disk` / `registry` / `referencers` 四路，无 PIE 闸，python 回环里也能调 —— 它存在的意义就是区分"没加载"与"没了"）；拿对象 → `unreal.load_asset` / `unreal.load_object`（python 层 `PyCore` 走裸 `LoadObject`，同样无闸）；建/删 → `create_asset_safe` / `safe_delete_asset`。
  - 两个 guard 的日志行为不同：`CheckIfInEditorAndPIE()` 失败会写 `LogEditorScripting: Error: The Editor is currently in a play mode.`，而 `EnsureAssetsLoaded()` 失败**静默** ⇒ 日志里没有那行 Error **不能**证明与 PIE 无关。

---

## 五、验收骨架

```python
import json, unreal
API = unreal.UnrealMCPPythonAPI
FAIL = []

def actors():
    r = json.loads(API.execute_mcp_command("get_actors_in_level", "{}"))
    return (r.get("result") or {}).get("actors") or []

def check(ok, msg, extra=None):
    print(("PASS: " if ok else "FAIL: ") + msg + ("" if ok or extra is None else " | " + str(extra)[:300]))
    if not ok: FAIL.append(msg)

a = actors()
classes = {x["class"] for x in a}
for need in ("SkyLight", "DirectionalLight", "SkyAtmosphere"):
    check(need in classes, "新地图已补 %s" % need, sorted(classes))
check(any(x["class"] == "DirectionalLight"
          and abs(x["rotation"][0] - (-31)) < 1 for x in a), "主光朝向是常规起手值")
print("RESULT: " + json.dumps({"count": len(a), "failures": FAIL}, ensure_ascii=False))
```

- 断言优先用**存在性 / 变换 / 属性回读**；亮度与观感现在**允许用 `take_screenshot`**（2026-10-08 起）作为验收/定位手段，与结构化回读一起写进结论。
- 存盘后用 `.umap` 的 mtime 复核真的写盘，别拿"命令返回 success"当证据。
