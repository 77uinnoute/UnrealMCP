---
name: unreal-particle-authoring
description: "UE5（UnrealMCP）里用 MCP 工具 + python 脚本程序化建/改 Cascade（UParticleSystem）粒子系统的方法：emitter/module 结构编辑的固定顺序与继承清理、模块属性与 Distribution 的写入规则与命名陷阱、TypeDataMesh / Light / Orbit / SizeScaleBySpeed 的实测行为、粒子材质配套要点（additive 的 Opacity 语义、mesh override material、贴地件 DepthFade）、程序化图案的抗锯齿纪律、把成品挂到关卡角色的方式、逐步回读与验收纪律。触发场景：准备调用以下任一 MCP 工具前 MUST 加载本 Skill —— create_particle_system / add_particle_emitter / remove_particle_emitter / add_particle_module / remove_particle_module / set_particle_module_property / set_particle_distribution / get_particle_module / list_particle_emitters / list_particle_modules / set_particle_lod_count / set_particle_lod_distance / copy_particle_lod；以及任何涉及「用 MCP 建/改 UE 粒子系统、Cascade emitter 与 module、分布曲线（constant/uniform/curve）、Sprite/Mesh/Beam/Ribbon emitter、光环/法阵/光柱/buff 特效、粒子挂到角色（AEmitter）、UParticleSystem 结构与持久化」的任务。粒子材质本身走 unreal-material-authoring Skill。"
metadata:
  version: "1.0.0"
  upstream: unreal-material-authoring
  downstream: ~
---

# UnrealMCP 粒子（Cascade）编写 Skill

适用：UE 5.5 + 本仓库 `Plugins/UnrealMCP`（bridge `127.0.0.1:55557`，MCP server 名 `unrealMCP`）。
目标：用 MCP 工具 + python 脚本**可重复、可验证**地建/改 Cascade 粒子系统，并把成品挂到关卡角色。

> 本文只写**可复用的做法与接口**；不收录具体配方与工具缺陷。

**环境注意**：改了 `Plugins/UnrealMCP/Content/Python/**`（含工具描述）后需**重启 unrealMCP server**；改插件 C++ 需编译 + 重启编辑器。**粒子材质**（Unlit/Additive、Custom HLSL、混合模式、DepthFade）走 `unreal-material-authoring` Skill。

---

## 〇、范围与可用路径

- 覆盖 **Cascade（`UParticleSystem`）**，不含 Niagara（本工具面没有 Niagara 入口）。
- 粒子这边 python 侧**没有** `MaterialEditingLibrary` 那样的编辑库（`Emitters`/`LODLevels`/`Modules` 都是 instanced 且无 `EditAnywhere`，`get_editor_property` 读不到），所以：

| 用途 | 路径 |
|---|---|
| 单个操作 / 需要结构化错误 | MCP 工具（13 个，命令名 = 工具名） |
| 批量建/配（几十次写入） | 脚本内 `unreal.UnrealMCPPythonAPI.execute_mcp_command` **回环**（同步派发，无 TCP、无死锁） |
| 工具写不进去的属性（结构体/数组） | `unreal.load_object` 拿模块 subobject → `set_editor_property`（见 §3） |

结构编辑（emitter / LOD / module 的增删改）一律走上表路径：**不要**绕过内核手改 instanced 数组（`Emitters` / `LODLevels` / `Modules`）。

```python
import json, unreal
PS = "/Game/Particles/PS_X.PS_X"

def bridge(cmd, **params):
    return json.loads(unreal.UnrealMCPPythonAPI.execute_mcp_command(cmd, json.dumps(params)))

print(bridge("list_particle_emitters", asset_path=PS)["result"]["emitter_count"])
```

脚本写到 `Saved/MCPScripts/*.py` → `execute_python_file(file_path=..., deferred=True)`（长脚本别用 inline `execute_python_command`）。生成 HLSL/长字符串一律用 **f-string**，`%` 格式化在多层嵌套里极易 hand-format 出错。

---

## 一、引擎事实（先记住，能省掉整轮试错）

| 事实 | 出处 |
|---|---|
| emitter / LOD / module 存在 instanced 数组，`UPROPERTY(instanced)` 且无 `EditAnywhere` → 结构编辑只能走本内核 | `ParticleSystem.h:192`、`ParticleEmitter.h:179`、`ParticleLODLevel.h:34-51` |
| 新建 emitter 会**从邻居 LOD 复制模块清单**（`GenerateFromLODLevel`）→ 新 emitter 一定带上一堆不想要的模块，必须显式清理 | `UnrealMCPParticleOps.cpp:1448-1502` |
| Sprite emitter 必须有 `RequiredModule->Material`，否则不渲染 | `UnrealMCPParticleOps.cpp:1502` 附近 |
| 短的 module/emitter 类名会被容错解析（"Velocity"→`ParticleModuleVelocity`、"Sprite"→`ParticleSpriteEmitter`），解析失败会回 candidates | `UnrealMCPParticleOps.cpp:320-483` |
| 工程里**只有 `ParticleSpriteEmitter` 这一族可加**；Mesh / Beam / Ribbon / AnimTrail 都是"给 sprite emitter 加 TypeData 模块"实现的 | `TypeData/` 目录只有 AnimTrail/Base/Beam2/Gpu/Mesh/Ribbon；**没有 TypeDataLight** |
| 光 emitter = sprite emitter + `UParticleModuleLight`；想只出光不出 sprite：`Required.bUseMaxDrawCount=True` 且 `MaxDrawCount=0`（`IsDynamicDataRequired` 直接返回 false，不产 render data，光照照常） | `ParticleEmitterInstances.cpp:2710-2722` |
| Orbit 的旋转速率属性名是 **`RotationRateAmount`**（不是 `RotationRate`）；且 `RotationRateOptions` 默认只 `ProcessDuringSpawn` → 不改成 Update 就完全不转 | `ParticleModuleOrbit.h:17-63` + 回读实测 |
| `LocationPrimitiveCylinder`：`StartRadius`/`StartHeight`/`HeightAxis` + 基类 `Positive_X..Negative_Z`/`SurfaceOnly`/`Velocity`/`StartLocation`；把 `StartHeight=0` 就是"平地圆盘散布"（不依赖轴开关语义） | `ParticleModuleLocationPrimitiveCylinder.h`、`...PrimitiveBase.h` |
| `SizeScaleBySpeed`：`SpeedScale` / `MaxScale`（都是 `FVector2D`），配合 `ScreenAlignment=PSA_Velocity(3)` 做速度拉伸拖尾 | `ParticleModuleSizeScaleBySpeed.h:21-27` |
| `ColorOverLife`：`ColorOverLife`(vector) + `AlphaOverLife`(float) + `bClampAlpha` | `ParticleModuleColorOverLife.h:24-31` |
| `UParticleModuleLight`：`SpawnFraction` / `bUseInverseSquaredFalloff` / `bHighQualityLights` / `bAffectsTranslucency` + `ColorScaleOverLife`/`BrightnessOverLife`/`RadiusScale`/`LightExponent`；光半径 ≈ 粒子 Size × RadiusScale | `ParticleModuleLight.h` + 回读 |
| 材质 additive 时 **Opacity 会乘进输出** → 粒子材质的 `ParticleColor.A` 必须接到 `Opacity`，否则 `AlphaOverLife` 曲线完全无效 | Epic Material Blend Modes 文档 + 实测 |

---

## 二、结构编辑配方（顺序固定，别跳）

1. **写前关编辑器**：`close_asset_editors(asset_path=...)`。资产在粒子编辑器里打开时任何写都被拒（`particle_editor_open`）；
   赶时间可以在该写命令上加 `auto_close=True`（所有粒子写工具都支持），它先关掉该资产的编辑器再继续，响应里回 `closed_editors`。
2. `create_particle_system(name, folder="/Game/Particles")` → **已自带一个可渲染的 sprite emitter**（Required + Spawn + Lifetime + Size + Velocity + ColorOverLife）。不要从空系统手搭。
3. `add_particle_emitter(asset_path, emitter_class="Sprite", lod_count=N, insert_index=?)`：LOD0 先建并用 `SetToSensibleDefaults`，再生成更高 LOD（否则高 LOD 是空清单）。
   emitter 默认都叫 `Particle Emitter`，多 emitter 时立刻 `set_particle_emitter_name(asset_path, emitter_index, "Rune")` 命名，后面才能按名对话。
4. `add_particle_module(asset_path, emitter_index, module_class, slot=?, insert_index=?)`，`slot ∈ modules | required | type_data | spawn | event_generator`；`insert_index=-1` = 追加，位置会影响 Cascade 求值顺序（Location 在 Velocity 前、SizeMultiplyLife 在 Size 后）。
   - **一次加一组**：`module_classes=["Velocity","Size","Color"]`（按序插入，全有或全无）；**从别的 emitter 抄一套**：`copy_modules_from_emitter=<index>`（跳过槽位模块）。三个形状互斥。
   - **顺序写错了不用删了重加**：`move_particle_module(asset_path, emitter_index, module_class|module_index, to_index)`，响应回 `from_index` / `to_index` / `module_order`；
     槽位模块（required/spawn/type_data/event_generator）不可移动。注意改顺序会改变视觉效果。
5. `remove_particle_module(...)` 只对 **modules** 槽的普通模块有效（slot 模块删不掉，改它的属性即可）。
6. 需要同类变体：`duplicate_particle_emitter(asset_path, emitter_index, name=?)` —— 连 LOD 与模块（含数值）一起复制成新 emitter，模块在各级 LOD 间仍是共享的；
   复制体可以独立编辑（改复制体不影响源）。LOD 级开关用 `set_particle_lod_enabled(..., lod_index, enabled)`（LOD 0 不能停用）。
7. 收尾：`list_particle_modules` 逐 emitter 回读结构 → 保存。

**新 emitter 的继承清理清单**（按用途删）：
- 地面件 / 光件：删 `Velocity`（否则整块盘子飞走）
- 任何 sprite emitter：删 `TypeDataMesh`（会从上一个 mesh emitter 继承过来）
- 非拖尾件：删 `SizeScaleBySpeed`；非环绕件：删 `Orbit`；光件只留一个 `Light`

---

## 三、属性与 Distribution 的写入规则

**先读后写**：`get_particle_module(asset_path, emitter_index, module_class|slot, lod_index)` 返回 `properties` / `property_details`（真实 C++ 名 + 类型 + 值）与 `distributions`（`property_name`/`kind`/`constants`/`min_max`/`keys`/`sampled_value`）。**照头文件猜属性名会翻车**（Orbit 的 `RotationRateAmount`、DepthFade 的 pin 名是 `Opacity`）。

- 标量 / 布尔 / 枚举 / 对象引用：`set_particle_module_property(..., property_name, property_value)`；对象属性传资产路径字符串（`Material`、`Mesh`）。
- **结构体属性**（Orbit 的 `OffsetOptions` / `RotationRateOptions`）：值传对象字典 `{"bProcessDuringUpdate": true, "bProcessDuringSpawn": false}`（也可按声明顺序传数组）。响应里结构体值以 JSON 对象回读。
- **结构体数组**（Spawn 的 `BurstList`）：用 `set_particle_bursts(asset_path, emitter_index, bursts=[{"count":30,"time":0.0,"count_low"?}])`，整体替换（空数组 = 清空）。
- 属性别名与单位：`RotationRate` 会解析到 `RotationRateAmount`（响应回 `resolved_from` / `property_name`），可定单位的属性回 `units` / `hint`（如 turn/秒）。
- **响应体积**：写命令默认 `detail="summary"`（不吐 `emitters` 全量树）；需要回读结构就传 `detail="full"`，或在 `fields=[...]` 里点名 `emitters`。
  只读命令可用 `include_modules=False` / `fields=[...]` 裁剪。要看某模块的一个属性：`get_particle_module` 的 `property_details`（含 `units` / `hint`）。
- 不确定某个命令名/参数名时，用自省工具 `list_mcp_commands`（可选 `category="particle"`）直接问 bridge：返回每条命令的 `params`（含 `required`）与 `flags`，比读源码或猜名字可靠。
- 结构体也没法表达时（罕见）：`get_particle_module` / `list_particle_emitters` 的 `object_path` 是官方兜底入口 —— `unreal.load_object(None, object_path)` + `set_editor_property`（必须用 C++ 原名）。
- 分布：`set_particle_distribution(..., property_name, kind, values|keys)`，`kind ∈ constant | uniform | constant_curve | uniform_curve`
  - float `uniform` → `[min, max]`；**vector `uniform` → 6 个值**（min xyz + max xyz）或 2 个（广播）；给 3 个会 `invalid_value`
  - 曲线 → `keys=[{"time": 0.0, "value": [...], "interp": "linear"}]`（float 曲线 value 用 1 元数组，vector 用 3 元）
- **结构体成员名用 C++ 原名**（snake_case 匹配不上）。
- LOD：`set_particle_lod_count` / `copy_particle_lod` / `set_particle_lod_distance`。

**常用模块短名**（slot 说明）：
`Required`(slot=required)、`Spawn`(slot=spawn)、`Lifetime`、`Size`、`SizeMultiplyLife`、`SizeScaleBySpeed`、`Velocity`、`Acceleration`、`Location`、`LocationPrimitiveCylinder`、`LocationPrimitiveSphere`、`ColorOverLife`、`Rotation`、`RotationRate`、`Orbit`、`MeshRotation`、`MeshRotationRate`、`TypeDataMesh`(slot=type_data)、`TypeDataBeam2`(type_data)、`TypeDataRibbon`(type_data)、`TypeDataAnimTrail`(type_data)、`Light`、`SubUV`。

---

## 四、粒子材质配套（细节见 unreal-material-authoring）

- 着色模型一律 `MSM_UNLIT`（Unlit），混合模式按需（`BLEND_ADDITIVE` / `BLEND_TRANSLUCENT`）；**Usage 要勾 `bUsedWithParticleSprites` 与 `bUsedWithInstancedStaticMeshes`**（否则粒子顶点工厂可能没被编译到）。
- additive 的标准接法：`mask × ParticleColor.RGB → EmissiveColor`，`mask × ParticleColor.A → Opacity`。
- sprite emitter 用 `Required.Material`；mesh emitter 用 `TypeDataMesh.Material` + **`bOverrideMaterial=True`**。**注意：给 `TypeDataMesh.Mesh` 赋值会重置 `Required.Material`（OnMeshChanged）→ 先设 Mesh，最后再写一次材质。**
- **贴地件**（地面法阵/冲击环等方形 plane）在地形上会被起伏半埋 → 边缘硬裁切、抖动。两手一起上：① 各 emitter 加 `Location.StartLocation=(0,0,10)` 抬离地面；② 材质串一个 **Depth Fade**（`FadeDistance` 2~3）让沉进地形处柔化。
- **程序化图案（Custom HLSL）的抗锯齿纪律**：
  - `float du = clamp(length(fwidth(uv)), 0.0004, 0.018);` **必须封顶**：不封顶时线宽 `max(下限, du*k)` 会随斜视角/逐帧的 `du` 变大而失控。
  - 角向/旋转特征用**整数**刻度数：`frac(ang*N/(2π))` 在 `atan2` 分支割线处才连续；**绝不要 `fwidth(angle)`**（分支割线处会爆）。
  - 尖角形状用**圆角距离** `length(max(ad,0)) + min(max(ad.x,ad.y),0) - r` 或**柔化 max** `0.5*(a+b+sqrt((a-b)²+k))`：硬盒距离 / 硬 `max` 在角点梯度断裂，抗锯齿恰好在角点失效。
  - 多条描边叠加时用 `max()` 合并而不是相加：additive 下相加 = 交点处 2× 亮斑。
  - 最后加**硬闸门**把不该有内容的区域强制为 0（例：`float disc = smoothstep(0.975, 0.945, r);` 乘在 return 上）。"结构上不可能亮"比"算得更准"可靠得多。
  - 每次改完 `get_material_compile_errors`（判当前状态**只看 `errors_by_feature_level`**）；写 `code` 会被服务端 lint 拦（失败则材质不变）。
- **预览的用途**：材质节点预览是**正视图 + 恒定像素足迹**，看不到斜视角/深度/地形类问题。所以出现视觉争议时，**先让用户发一张 Custom 节点预览图**，能立刻把问题分成"图案数学"还是"渲染/视角"。

---

## 五、把成品挂到关卡角色

**一条命令搞定**（2026-09-13 起）：

```
spawn_particle_actor(name="Emitter_Buff", template="/Game/Particles/PS_X",
                     location=[x,y,z], attach_to="BP_Character_C_0",
                     relative_location=[0,0,2], auto_activate=True)
```

它内部完成"生成 `AEmitter` → `ParticleSystemComponent.SetTemplate` → `AttachToActor(KeepWorldTransform)` → 相对变换 → 激活"，响应回
`actor` / `template` / `attached_to` / `relative_location` / `activated`；`template` 非粒子系统 → `not_particle_system`，`attach_to` 不存在 →
`actor_not_found` + `candidates`，两种失败都在生成前判定、不留空 Emitter。

需要更细的手工调整（例如改组件上的其它字段）时才回落到 python：

```python
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
world_char = eas.get_all_level_actors()          # 按 get_name()/get_actor_label() 找目标角色
emitter = eas.spawn_actor_from_class(unreal.Emitter, char.get_actor_location(), unreal.Rotator(0, 0, 0))
emitter.set_actor_label("Emitter_Buff")
comp = emitter.get_editor_property("particle_system_component")
comp.set_editor_property("template", unreal.EditorAssetLibrary.load_asset("/Game/Particles/PS_X"))
comp.set_editor_property("auto_activate", True)
emitter.attach_to_actor(char, "", unreal.AttachmentRule.KEEP_WORLD,
                        unreal.AttachmentRule.KEEP_WORLD, unreal.AttachmentRule.KEEP_WORLD, False)
emitter.set_actor_relative_location(unreal.Vector(0.0, 0.0, 2.0), False, False)
comp.activate(True)
unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level()
```

- 所有 emitter 的 `Required.bUseLocalSpace=True` → 特效跟着角色；各层的相对高度放在各自 `Location.StartLocation`（如光柱 `z=120`、胸口光 `z=95`）。
- **别用射线探地形做诊断**（python 侧拿不到可靠的 actor/component 几何量）。
- 改了资产内容不需要重启，关卡里的组件引用同一资产即时生效；**改 `Content/Python/**` 才需要重启 server**。

---

## 六、验证与验收纪律

1. **结构回读**：每批写入后 `list_particle_modules` 逐 emitter 打印模块清单；分布值用 `get_particle_module` 回读 `sampled_value` / `min_max` / `keys`（不要用"上一步应该成功了"）。
2. **材质**：`get_material_compile_errors`（异步，隔一次调用复查）。
3. **视觉验收归用户**：效果由用户在编辑器视口判断；**不要**用截图/像素分析代替（本仓库规则），也不要用视觉子 agent。
4. **定位方法**：给用户**逐层开关**（Custom code 里 `float xxxAmount = 1.0;` 一行一个），让他二分；同时问清**半径/范围/形状**。经验：只说"某处闪烁"时盲改容易连错几轮；"节点预览图 + 逐层开关 + 问清半径"是收敛最快的组合。
5. 编辑器**不 tick 粒子**时 `get_num_active_particles()` 恒为 0，这**不是**失败证据（视口 Realtime / PIE 才有）。

---

## 七、故障速查

