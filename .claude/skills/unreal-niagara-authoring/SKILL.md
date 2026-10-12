---
name: unreal-niagara-authoring
description: UE5（UnrealMCP）里用 MCP 工具 + python 脚本程序化建/改 Niagara 的方法：系统与发射器的生命周期（建资产为何必须走引擎初始化）、编译诊断的读法（per-script 状态 + errors/warnings 带 script_path 与 node_guid）、用户参数的「User. 前缀与类型必须一致」语义、stack 模块的 stage 纪律（SpawnRate 属 EmitterUpdate，放错 stage 只报 "System is invalid after compilation"）、scratch-pad 模块（Custom HLSL 与 Map Get/Map Set）的建/声明/连线顺序与两套 namespace（Module.X 输入、Particles.X 输出）、pin 幂等规则与重复 pin 会毁掉编译、GPU-only DataInterface（Grid2D/RenderTarget2D）必须改 Sim Target、以及「写完回读 + compile_niagara_system 自证」的验收纪律。触发场景：准备调用以下任一 MCP 工具前 MUST 加载本 Skill —— create_niagara_system / duplicate_niagara_system / list_niagara_emitters / add_niagara_emitter / remove_niagara_emitter / add_niagara_renderer / remove_niagara_renderer / set_niagara_renderer_material / compile_niagara_system / get_niagara_parameters / set_niagara_parameter / list_niagara_grid_attributes / read_render_target_stats / list_niagara_modules / add_niagara_module / set_niagara_module_enabled / create_niagara_scratch_pad_module / list_niagara_scratch_pad_nodes / set_niagara_custom_hlsl / set_niagara_scratch_pad_pins / remove_niagara_scratch_pad_node / remove_niagara_scratch_pad_pin / connect_niagara_scratch_pad_pins / spawn_niagara_actor / set_niagara_component_parameter；以及任何涉及「用 MCP 建/改 UE Niagara 系统、往 stack 里加模块、写 Custom HLSL/ScratchPad、给发射器加 GPU 仿真阶段、把 Niagara 系统挂到关卡 Actor、读编译报错到底哪条脚本/哪个节点」的任务。
---

# Niagara 编程式建改（UnrealMCP）

## 0. 一句话模型

Niagara 是**双层**结构：用户看到的 Stack（System Spawn/Update、Emitter Spawn/Update、Particle Spawn/Update）是 `UNiagaraGraph` 里一串用 parameter-map 引脚串起来的 `UNiagaraNodeFunctionCall`。**唯一安全的写法是走引擎自己的 stack 工具**（`FNiagaraStackGraphUtilities::AddScriptModuleToStack` 等），手搓 `AddNode` + `AllocateDefaultPins` 会绕开 parameter-map 接线，编辑器打开时 stack 是空的、编译签名直接崩。

本工具面已按这条纪律实现：`add_niagara_module` / `create_niagara_scratch_pad_module` 都经引擎函数落栈，`set_niagara_scratch_pad_pins` 通过导出的 schema 造 pin。

## 1. 从零到能编译的最小流程

```python
S = "/Game/VFX/NS_ShallowWater"

create_niagara_system(name="NS_ShallowWater", folder="/Game/VFX")      # 空系统（带 SystemSpawn/Update 图与 SystemUpdate 模块）
add_niagara_emitter(asset_path=S, emitter_name="Water")                 # 空发射器
list_niagara_modules(asset_path=S, emitter_name="Water")                # 先看有没有默认模块；空发射器一般什么都没有
compile_niagara_system(asset_path=S)                                    # 先拿一次基线：compile_status / errors / warnings
```

`list_niagara_emitters` 给出的 `id` 与 `name` 都能当 `emitter_name` 传。
其余生命周期命令：`create_niagara_system`（建空系统，必须有 SystemSpawn/Update 图才算初始化好）、
`duplicate_niagara_system`（复制一份再改，比从零省事）、`remove_niagara_emitter`（摘发射器）。
**空发射器没有渲染器 ⇒ 系统能编译但看不见东西**：渲染器用 `add_niagara_renderer` 加（`renderer_type` 取
`Sprite` / `Mesh` / `Ribbon` / `Light` / `Decal` / `Component`）、`remove_niagara_renderer` 摘（`renderer_index`
就是 `list_niagara_emitters[*].renderers` 的顺序），加的时候可以顺手 `material_path=…` 绑材质；
渲染器没有材质同样画不出东西。

## 2. stage 是硬约束，不是建议

| 模块 | 正确 stage |
|---|---|
| `InitializeParticle` | `ParticleSpawn` |
| `SpawnRate` / `SpawnBurstInstantaneous` / `EmitterState` | `EmitterUpdate` |
| `ParticleState` / `SolveForcesAndVelocity` / `ScaleColor` 等每帧更新 | `ParticleUpdate` |

放错 stage 时编译器**只会**给一句 `System is invalid after compilation`，不指认是谁。所以：加完模块就 `compile_niagara_system`，报错先怀疑 stage，用 `list_niagara_modules(asset_path, emitter_name=<同上>)` 看每个模块的 `stage` 对不对。

`list_niagara_modules` 的 `order` 字段是**真实执行顺序**（沿 stage 的 parameter-map 链从 output 节点倒推），不是图上的 X/Y 坐标顺序——判断「谁先跑」用它，别用编辑器里的摆放位置。`order == -1` 表示该节点不在任何 stage 链上（例如嵌套调用）。

```python
add_niagara_module(asset_path=S, emitter_name="Water", stage="EmitterUpdate",
                   module_asset_path="/Niagara/Modules/Emitter/SpawnRate.SpawnRate")
add_niagara_module(asset_path=S, emitter_name="Water", stage="ParticleUpdate",
                   module_asset_path="/Niagara/Modules/Update/Forces/SolveForcesAndVelocity.SolveForcesAndVelocity")
compile_niagara_system(asset_path=S)      # -> errors[] 里每条带 script_path + node_guid
```

模块脚本路径必须是 **Module 用途**的 `UNiagaraScript`；传了其它用途（Function/DynamicInput）会拿到 `not_a_module_script`，传错路径是 `module_script_not_found`。

**加错了就摘**：`remove_niagara_module` 会把模块连同 stack 为它建的 override 节点一起摘掉
（引擎的 `RemoveModuleFromStack` 未导出，这里靠沿 parameter-map 链倒推 + "override 节点上的 pin 全部点名这个模块"来认领），
回执 `removed_nodes` 列出实际摘掉的每个节点、`modules` 给出摘后的 stage、`errors` 给出编译诊断——**看这三样判成败**。
只想临时关掉就用 `set_niagara_module_enabled`。

**发射器级开关**（决定某些特效能不能跑）用 `set_niagara_emitter_properties`：`sim_target="GPUComputeSim"`
是 Grid2D / RenderTarget2D 这类 GPU-only 数据接口的前提；另外 `local_space / determinism / random_seed /
interpolated_spawning / pre_allocation_count` 也可写。
可见性用 `set_niagara_renderer_material` 绑材质（`renderer_index` 选第几个、`material_path` 给材质；材料属性按"可编辑的材质类型属性"自动找，
Sprite/Mesh/Ribbon/Decal 通吃；材质的类名可从 `list_niagara_emitters[*].renderers` 看到）。

**GPU 逐格计算用 `add_niagara_simulation_stage`**（浅水 SWE、PBF、Grid2D 写回这类）：它建 stage 对象 + 它的脚本 +
stage 的图 output，stage 的 identity 由脚本的 usage id（= stage 的 merge id）承担，编译器按它回读。
`iteration_source` 选 `Particles` / `DataInterface` / `DirectSet`；配合 `sim_target="GPUComputeSim"` 才跑得起来。
回读看 `emitter.simulation_stage_names` / `simulation_stage_count`；stage 的 stack 条目要在资产编辑器重新打开时刷新出来。

**模拟阶段本身就是一个可以挂模块的阶段**：`add_niagara_module` / `create_niagara_scratch_pad_module` 的 `stage`
直接写 stage 的名字（`"SWE Sweep"`）或它的序号（`"0"`），`list_niagara_modules(..., stage="SWE Sweep")`
就会把这上面排的模块单独列出来（`stage` 字段回的是阶段名，不再是 `ParticleSimulationStage`）。

### 2.1 在模拟阶段里做逐格求解（Grid2D，实测可编的整套写法）

每格一次、要读邻居的求解（浅水 SWE、PBF 的密度步）都长这样：

```python
set_niagara_emitter_properties(emitter_name="WaterGrid", sim_target="GPUComputeSim")
set_niagara_data_interface_properties(param_name="WaterGrid", data_interface_type="Grid2DCollection")
add_niagara_simulation_stage(emitter_name="WaterGrid", stage_name="SWE Sweep", iteration_source="DataInterface")
set_niagara_simulation_stage(emitter_name="WaterGrid", stage="SWE Sweep",
                             data_interface_parameter="User.WaterGrid", num_iterations=1)
r = create_niagara_scratch_pad_module(emitter_name="WaterGrid", stage="SWE Sweep", module_name="SWESweep")
```

然后三件事，一条都不能省：

1. **声明一个指向 DI 的读 pin**（模块级输入，名字是参数表里的**全名**、类型是 DI 类名）：
   `set_niagara_scratch_pad_pins(input_pins=[{"name": "User.WaterGrid", "type": "Grid2DCollection"}])`。
   这个 pin 落在 Map Get 上，作用是把 DI 变量**带进发射器参数表** —— 没有它，编译器找不到迭代源
   （`HandleSimStageSetupAndTeardown` 按全名在"图里出现过的变量"里精确查找），报
   `变量 User.WaterGrid 编译时在引用的图表中缺失`。
2. **stage 的绑定名必须是同一个全名**：`set_niagara_simulation_stage(data_interface_parameter="User.WaterGrid")`
   写成短名（`"WaterGrid"`）编译期同样报"变量缺失"，因为 `GetStackContextReplacementName()` 直接返回绑定名。
3. **状态与发布通道都声明成 StackContext pin**：读 `input_pins=[{"name":"User.WaterGrid.H","type":"float"}]`、
   写 `output_pins=[{"name":"User.WaterGrid.H","type":"float"}]`（`User.WaterGrid.<属性名>`），
   有 pin 的属性才存在。

HLSL 正文里 `Grid` 就是那个 DI 输入 pin 的变量名，调用形态照抄引擎：

```hlsl
int X, Y;
Grid.ExecutionIndexToGridIndex(X, Y);                                  // 本格坐标
float Hp;
Grid.GetPreviousFloatValue<Attribute="H">(X + 1, Y, Hp);               // 邻居/上一执行阶段的状态（顶点法 curl 用 X-1/X+1 等）
Grid.SetFloatValue<Attribute="H">(X, Y, Hp + 0.1);                     // 写回本格
```

- 读一律读**上一执行阶段**的值（`GetPrevious*`）：这正是显式时间积分要的"本步开始时的状态"。
- vec2/vec3/vec4 对应 `GetPreviousVector2DValue` / `GetPreviousVectorValue` / `GetPreviousVector4Value`，
  写对应 `SetVector2DValue` / `SetVectorValue` / `SetVector4Value`，都带 `<Attribute="…">`。
- 只有"写 pin 声明过、或图里引用过"的属性才可用；写一个新属性前先声明它的写 pin。
- **HLSL 正文里赋值的名字，就是输出引脚名**（编译器按引脚名声明变量）。引脚叫 `H` 而正文写 `HOut`
  时，`compile_niagara_system` 仍回 `success`，坏在后面的 GPU shader 编译上。
- **判据必须多看一步**：`compile_niagara_system` 的 `success` 只代表脚本翻译过了，
  **不代表 GPU shader 编过**（后者在实例化时发生）。所以改完先 `compile_niagara_system`，
  再 `start_pie` → 读 `Saved/Logs/<proj>.log` 里的 `LogNiagaraShaderCompiler`（出现
  `There were issues for job "…/ParticleGPUComputeScript/FNiagaraShader/N"` 就是没编过）→ `stop_pie`。
  常见 shader 期错误只有两句话有用：`use of undeclared identifier '<引脚名>'`（见上一条）与
  片元端的 `Component selction_result … has no valid offset`（position/LWC 引脚）。
- 迭代源找不到时 dispatch 形状会**静默**退回 OneD，不报错；`变量 <名> 编译时在引用的图表中缺失` 才是它的正面信号。

### 2.2 两阶段（预测/校正）怎么切

要 SSPRK2 这类需要"中间态"的多步积分时，用两个模拟阶段，并且**状态与中间量分别具名**，别指望
`GetPrevious*` 在同一帧跨阶段读到你要的那一份：

| 阶段 | 读 | 写 |
|---|---|---|
| `… Predict` | 状态 `H/HU/HV`（上一帧的 U^n） | `H0/HU0/HV0`（U^n 留档）+ `HStar/HUStar/HVStar`（U^n + dt·L(U^n)） |
| `… Correct` | `H0/HU0/HV0` + `HStar/…` | 状态 `H/HU/HV` = 0.5·U^n + 0.5·(U* + dt·L(U*)) |

两个阶段各挂一个 scratch 模块（同一个发射器、同一个 DI），属性总数要够：`NumAttributes` 用
`set_niagara_data_interface_properties` 抬高（本机 SWE 用了 15 个属性 ⇒ 设 16）。
阶段顺序＝创建顺序，改名用 `set_niagara_simulation_stage(..., stage_name=…)`。

### 2.3 发射器要先"活着"，网格才有人算

`add_niagara_emitter` 建的空发射器**没有任何 EmitterState / SpawnRate** ⇒ 它一次性就结束、
`NiagaraComponent.is_active()` 为 False、模拟阶段一帧都不跑（表现：RT/属性全是初值，画面全黑）。
自证的顺序是：`start_pie` → 在 PIE world 里查组件 `is_active()` → 再看数据。

```python
add_niagara_module(stage="EmitterUpdate", module_asset_path="/Niagara/Modules/Emitter/EmitterState.EmitterState")
add_niagara_module(stage="EmitterUpdate", module_asset_path="/Niagara/Modules/Emitter/SpawnRate.SpawnRate")
set_niagara_module_input(module=<SpawnRate guid>, input_name="SpawnRate", value="1024")
```

`EmitterState` 的 `Loop Behavior` 是**枚举**输入，按成员名写即可（数值也可以）：

```python
set_niagara_module_input(module=<EmitterState guid>, input_name="Loop Behavior", value="Infinite")
```

### 2.4 让网格变成看得见的画：Grid2D → RenderTarget → 材质

网格数据只有在 RenderTarget 里才可能被材质采样，链路四步：

1. RT 资产：`create_asset_safe(asset_name="RT_WaterGrid", package_path="/Game/VFX", asset_class="TextureRenderTarget2D")`
   （工厂按 `GetSupportedClass()` 反查，回执里有 `factory` / `factory_source`）。
2. **UObject 类型用户参数**（`set_niagara_parameter` 的 `param_type` 可以是 `TextureRenderTarget`，
   `value` 给资产路径）：`set_niagara_parameter(param_name="WaterRT", param_type="TextureRenderTarget", value="/Game/VFX/RT_WaterGrid")`
3. 绑到网格 DI：`set_object_property(target="…:NiagaraDataInterfaceGrid2DCollection_0",
   property_name="RenderTargetUserParameter.Parameter.Name", property_value="User.WaterRT")`
4. 材质：`TextureObjectParameter`/`TextureSample` 采这张 RT。**别按 UV 0..1 采整张**：引擎会把 RT 改成
   "每属性一个 tile"（属性 i 落在 x∈[i·NumCells,(i+1)·NumCells)），只想要一个属性就采
   `UV * (1/NumTiles.X, 1) + (tile/NumTiles.X, 0)`。
   材质工具的 UV 输入引脚名是 **`UVs`**（写 `Coordinates` 会 `connect` 回 False，静默没连上）。
5. **tile 号靠读属性表，不靠猜**：`list_niagara_grid_attributes` 回引擎 `FindAttributes` 的属性清单
   （每个属性的 `name` / `type` / `first_slice` / `channels`）与 `slot_count`（`asset_path` + `param_name`）。
   **RT tile 号 = `first_slice - slot_count`**（引擎的偏移从 `NumAttributes` 起算，槽位比属性多 1 个也正常）。
   顺序既不等于用户参数表顺序，也不等于 scratch 引脚的书写顺序，别数引脚。
   ⚠️ **没有实例在模拟时 RT 里是引擎给 grid 设的清屏色 `(0.5,0,0,0)`** —— 此时读到的/预览到的都不是数据；
   要读就先让实例跑起来（`start_pie`；PIE 结束后 RT 保留最后一帧）。
6. **"RT 里到底有什么"用 `read_render_target_stats` 读**（`asset_path` + 可选 `tile_width`/`sample_step`/`png_path`）：
   它取 GPU 纹理本身按 `EPixelFormat` 解码成**浮点**，逐 tile 回 `min`/`max`/`mean`/`nonzero` 数组与
   `size_x`/`size_y`/`format`/`tiles_x`/`tiles_y`/`tile_count`。tile 索引与第 5 条同一套（行优先，tile = `first_slice - slot_count`），
   `nonzero > 0` 就是"这个 tile 有数据"的判据（清屏色那一片会是 `mean ≈ 0.5`、`min == max`）。
   **不要再走这两条 python 路**：`unreal.RenderingLibrary.read_render_target_pixel` 对 `RTF_R16F` 恒回
   `(255,0,0,255)`（与内容无关），`unreal.RenderingLibrary.export_render_target` 对 R16F 写的是 **16-bit** PNG
   （按 8-bit 解会得到全零 tile）、对 `RTF_RGBA16F` 写的是**不可解码**的文件。
   只在没有 bridge（编辑器/python 都不可用）时才退回脚本 `scripts/dump_grid_rt_tiles.py`（导出 + 自己解 16-bit PNG）。

**stage 建完要改就用 `set_niagara_simulation_stage`**（`enabled` / `iteration_source` / `num_iterations` /
`data_interface_parameter` / 改名）。**先别急着扩命令**：stage 是普通子对象，`reflect_probe` + `set_object_property`
按路径就能写大部分属性（实测能写 `SimulationStageName`、`IterationSource`、`DataInterface.BoundVariable`，
连 `NumIterations.DefaultValue` 都能写——**但要按字节数组形状** `[16,0,0,0]`，给数字会静默无操作）。
两类要注意（`MCPPropertyReflector::IsWritable` 的规则）：
- **非嵌套路径 + 缺 `EditAnywhere`** → 回 `writable_detail: engine_managed` 拒写（`bEnabled` 就是这种，
  它恰好是位域但不是位域被拒）；想绕就得把叶子写进**嵌套路径**（反射器对这一路显式豁免）。
- **形状必须与 probe 报告的一致**（标量 / 数组 / 字段对象 / 资产路径）；形状错会明确回
  `failed[0].error_code = type_mismatch` 与 `supported_shapes`（如字节块 `DefaultValue` 必须写 `[16,0,0,0]`）。
  **读数陷阱**：逐属性结果在 `result.data` 里（`writes`/`written_count`/`failed`/`failed_count`），
  最外层 envelope 的 `success: true` 只表示"调用跑通"——**判据是 `failed_count == 0`**，否则会把"没写进去"看成成功。
（`set_object_property` 的入参是 `target` / `property_name` / `property_value`，用错名字只会得到 `invalid_params`。）

**stock 模块的参数用 `set_niagara_module_input` 改**（这是 stack UI 自己的写法，落到模块的 rapid iteration 参数）：

```python
mods = list_niagara_modules(asset_path=S, emitter_name="Water")
rate = next(m for m in mods["modules"] if m["name"] == "SpawnRate")
set_niagara_module_input(asset_path=S, emitter_name="Water", module=rate["guid"],
                         input_name="SpawnRate", value="4096")     # 回读 value + rapid_iteration_parameter
```

- 输入名给 stack 上显示的名字（`SpawnRate`）或 `Module.SpawnRate` 都行；名字错会回 `module_input_not_found` 且列出该模块全部输入名。
- 类型以模块签名为准：给错 `param_type` 直接 `parameter_type_mismatch`；DataInterface 类输入会被 `unsupported_parameter_type` 拒绝。
- 回读取自脚本的 rapid iteration 存储（`rapid_iteration_parameter` 给出完整参数名），不是把请求原样回显。

**DataInterface 参数（GPU 面）走 `set_niagara_data_interface_properties`**——`set_niagara_parameter` 写不了它们（DI 是对象不是数值）：

```python
# 先读它能接受什么（properties 省略 = 只读），再用真名写
r = set_niagara_data_interface_properties(asset_path=S, param_name="WaterGrid",
                                          data_interface_type="Grid2DCollection")
set_niagara_data_interface_properties(asset_path=S, param_name="WaterGrid",
                                      properties={"ResolutionMaxAxis": 256, "CellSize": 12.5})
```

- 参数不存在时给 `data_interface_type` 就会建（类名可写规范短名 `Grid2DCollection` / `RenderTarget2D` / `ArrayFloat3`，
  也可写完整类路径）；不给类型且参数不存在 → `parameter_not_found` + 现有参数列表。
- 属性名写错 → `data_interface_property_not_found` + **可写属性清单**（`editable_properties` 字段任何时候都会回），别猜名字。
- 写进去的值从对象读回来（`properties[].value`）；标量参数（`User.CFL` 那种）用它会被 `not_a_data_interface_parameter` 挡住。

## 3. 用户参数：前缀与类型

系统级参数都住在 `UNiagaraSystem.ExposedParameters` 里，名字带 `User.` 前缀：

```python
get_niagara_parameters(asset_path=S)                 # [{name:"User.Gravity", type:"vec3", value:"(X=0,Y=0,Z=-980)"}]

set_niagara_parameter(asset_path=S, param_name="Gravity", value="(X=0,Y=0,Z=-980)")      # "User.Gravity" 等价
set_niagara_parameter(asset_path=S, param_name="CFL", value="0.4", param_type="float", add_if_missing=True)
```

- `value` 用**引擎导出格式**（`"(X=0,Y=0,Z=-980)"` / `"(R=1,G=0,B=0,A=1)"` / `"true"` / `"12.5"`），`get_niagara_parameters` 读出来的值可以直接喂回去。
- 参数已存在时**以实际类型为准**：类型不一致会 `parameter_type_mismatch`（回执里给出真实类型），不静默改布局。
- DataInterface 类型（Grid2DCollection / ArrayFloat3 / RenderTarget2D…）不能用 `set_niagara_parameter` 写（`unsupported_parameter_type`）；它们只能从蓝图/组件侧写进去。
- 每个写命令成功即保存（`saved`）。批处理时传 `persist=False`，最后一条不带 `persist` 时再落盘，或对整包用一次保存命令收口。

## 4. ScratchPad + Custom HLSL：物理计算的唯一入口

水体 SWE / PBF 这类要自己算 HLSL 的必须走 scratch-pad 模块——它是发射器内部的一个 `UNiagaraScript`，工具面负责建它、往 stack 挂它、并钻进它的图里加节点/声明 pin/连线。

```python
r = create_niagara_scratch_pad_module(asset_path=S, emitter_name="Water", stage="ParticleUpdate",
                                      module_name="SWEStep")
MOD = r["module_guid"]                     # 后续所有 scratch 操作都用它当 `module`

# 1) 声明模块接口：输入 -> Map Get 的 "Module.X" 读 pin；输出 -> Map Set 的 "Particles.X" 写 pin
set_niagara_scratch_pad_pins(asset_path=S, emitter_name="Water", module=MOD,
                             input_pins=[{"name": "Grid", "type": "Grid2D"},
                                         {"name": "CFL", "type": "float"}],
                             output_pins=[{"name": "Height", "type": "float"}])

# 2) 加一个 Custom HLSL 节点（不给 node_guid 就是新建）
h = set_niagara_custom_hlsl(asset_path=S, emitter_name="Water", module=MOD,
                            custom_hlsl_code="""
int2 Idx = int2(0, 0);
float Cs = 0.0;
// 读 Grid / CFL，算完写 Height
Height = Cs;
""")
HLSL = h["node_guid"]

# 3) 给这个节点声明打字过的 pin（名字就是 HLSL 里用的变量名）
set_niagara_scratch_pad_pins(asset_path=S, emitter_name="Water", module=MOD, node_guid=HLSL,
                             input_pins=[{"name": "Grid", "type": "Grid2D"},
                                         {"name": "CFL", "type": "float"}],
                             output_pins=[{"name": "Height", "type": "float"}])

# 4) 连线：Map Get -> HLSL -> Map Set（先 list 出真实的 guid 与 pin 名）
nodes = list_niagara_scratch_pad_nodes(asset_path=S, emitter_name="Water", module=MOD)
mapget = next(n["guid"] for n in nodes["nodes"] if n["type"] == "MapGet")
mapset = next(n["guid"] for n in nodes["nodes"] if n["type"] == "MapSet")
for pin in ("Module.Grid", "Module.CFL"):        # 参数 map pin 名自带 namespace
    connect_niagara_scratch_pad_pins(asset_path=S, emitter_name="Water", module=MOD,
                                     from_node=mapget, from_pin=pin, to_node=HLSL, to_pin=pin.split(".")[1])
connect_niagara_scratch_pad_pins(asset_path=S, emitter_name="Water", module=MOD,
                                 from_node=HLSL, from_pin="Height", to_node=mapset, to_pin="Particles.Height")

compile_niagara_system(asset_path=S)             # 失败时 errors[] 指到具体 script_path + node_guid
```

所有连线都走 `connect_niagara_scratch_pad_pins`（`from_node` / `from_pin` → `to_node` / `to_pin`；
`output_name` 不传即取第一个输出），它连完会回读该节点的 pins —— 判据是 `connected`，不是"调用了没报错"。

要换 HLSL 正文时，把 `node_guid=HLSL` 再传一次给 `set_niagara_custom_hlsl` 即可（不新增节点）。

**四条硬边界**

1. **Custom HLSL 节点禁用 position（LWC）引脚**：给它的输入或输出声明 `position` 类型会被直接拒绝
   （`unsupported_hlsl_pin_type`）——一旦放进去，VM 编译必挂：
   `Component selction_result of variable selction_result has no valid offset`。
   **注意连接这一步不是最终判据**：把 vec3 输出接进需要转换的参数时，回执仍是 `connected: true`，
   错误要到 `compile_niagara_system` 才出现，而且报的是 `... selction_result has no valid offset`
   （`node_guid` 全 0，指不到人）。连线回执会额外带 `inserted_conversion_nodes`——实测这个场景它为空，
   所以它只是"看到就警惕"的信号，**判据只有 compile 的 `errors` 是否为 0**。
   实测干净可编译的是普通属性：`Particles.WaveHeight`(float) / `Particles.WaveNormal`(vec3) / `Particles.Foam`(float)；
   `Particles.Position` / `Particles.Velocity` 这类有炸掉的样本。
   排错用 `set_niagara_module_enabled(..., enabled=false)` 二分：停用后编译通过 ⇒ 问题就在你这个模块图里。
   想让粒子动，现阶段只能写普通属性、由材质/后续 GPU 阶段消费；真正的位移要等仿真阶段能力。
2. **重复 pin 会毁编译**：`set_niagara_scratch_pad_pins` 幂等（同名 pin 复用），但如果你用别的路径在同一 Map Get/Map Set 上造了同名 pin，两个同名且都连上的写 pin 会让系统变成 invalid。改之前先 `list_niagara_scratch_pad_nodes` 看现状，别反复声明。
3. **pin 名是 namespace**：模块输入在 Map Get 上是 `Module.<名字>`，输出在 Map Set 上是 `Particles.<名字>`（工具面在声明时替你补，读回来时是完整名）。HLSL 节点内部的 pin 名是**裸名**——写成 `Particles.WaveHeight` 的话，正文里的 `WaveHeight` 就不再对应任何 pin。
4. **Grid2D / Grid3DCollection / RenderTarget2D 是 GPU-only**：CPU（默认）发射器上出现这些接口，编译只给一句 `System is invalid after compilation`。要么把这些计算搬到 GPU 仿真阶段的发射器上（Sim Target 改 GPUComputeSim，改法是发射器自身属性/蓝图侧，不在本工具面），要么换 CPU 可行的数据结构（DataInterfaceArray 系列）。

**改错就拆**：pin 声明错了、节点建错了用 `remove_niagara_scratch_pad_pin` / `remove_niagara_scratch_pad_node`
（前者对 Custom HLSL 会重建签名，所以该 pin 真的会从编译里消失；模块的 Input/Output 节点受保护）。
没有它们时只能把 HLSL 正文改成注释把节点"废掉"，图会越来越脏。

## 5. 关卡 Actor 与实例参数

```python
spawn_niagara_actor(system_path=S, actor_name="NS_Water_Pool",
                    location={"x": 0, "y": 0, "z": 0}, auto_activate=True)

set_niagara_component_parameter(actor_name="NS_Water_Pool", param_name="Gravity",
                                param_type="vec3", value="(X=0,Y=0,Z=-980)")
```

- 名字被占用直接拒（`name_taken`），不会让引擎断言。
- 关卡被标脏但**不保存**：摆好之后自己存关卡。
- 组件参数写在组件自己的 override，不影响系统资产里的默认值（这正是「实例级覆盖」的语义），回执里的 `value` 是从组件读回来的。

## 6. 自证清单（每次改完都跑）

| 断言 | 依据 |
|---|---|
| 模块真进 stack 了 | `list_niagara_modules` 里有它的 `guid` / `name` / 正确 `stage` |
| 编译通过 | `compile_niagara_system` 回 `compile_status == "success"`、`errors == []` |
| 参数写进去了 | `set_niagara_parameter` / `set_niagara_component_parameter` 回执里的 `value`（回读值）符合预期 |
| scratch 图对了 | `list_niagara_scratch_pad_nodes` 的 `nodes` + `pins`（含 `connected`） |
| HLSL 落盘了 | `set_niagara_custom_hlsl` 回执的 `custom_hlsl_code` 就是你写的那段（不一致会报 `readback_mismatch`） |
| Actor 起效 | `spawn_niagara_actor` 回 `actor_path` / `component_path`，组件参数回读一致 |

**截图可用于验收**（2026-10-08 起允许）：`take_screenshot`（含 PIE 内）能补"形状/观感/遮挡"这类回读证明不了的部分；
但**能不能跑**仍然只认结构化证据 —— 上面的回读 + `compile_niagara_system` + PIE 日志里的
`LogNiagaraShaderCompiler` / `MessageLog`（截图看不出这两类错）。

## 7. 常见错误码速查

| error_code | 含义 / 处置 |
|---|---|
| `emitter_not_found` | `emitter_name` 写错；`candidates` 里是真实名字 |
| `unknown_stage` | stage 拼错；合法值见 `candidates` |
| `stage_output_not_found` | 该 stage 在图上没有 output 节点（通常是系统空图/脚本未初始化） |
| `module_script_not_found` / `not_a_module_script` | 模块脚本路径不对，或者传的不是 Module 用途脚本 |
| `parameter_not_found` | 参数不存在且 `add_if_missing=False` |
| `parameter_type_mismatch` | 传的 `param_type` 与真实类型不符（回执含真实类型） |
| `unsupported_parameter_type` | DataInterface / UObject 参数不能这样写 |
| `scratch_pad_not_found` | `module` 传的 guid/名字不对（用 `list_niagara_modules` 取） |
| `pin_not_found` | scratch 图里没这个 pin；`candidates` 列出目标节点的全部 pin（`in:`/`out:` 前缀） |
| `unsupported_hlsl_pin_type` | Custom HLSL 节点上声明了 position 引脚（会打死 VM 编译），换 vec3 |
| `structural_node` / `structural_pin` | 想删模块的 Input/Output 节点或 parameter-map 链引脚，被拒 |
| 连线被 schema 拒绝 | 回执 error_code 为 connect_failed（类型不符），connected=false |
| `readback_mismatch` | 写进去的值读回来不一样（HLSL 正文被引擎规整过时也可能命中） |
| `name_taken` | 关卡里已有同名 Actor |

改完 `Content/Python/**` 要重启 unrealMCP server；改的是插件 C++ 则先编译插件再重启编辑器。
