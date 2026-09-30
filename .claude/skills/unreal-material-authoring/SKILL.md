---
name: unreal-material-authoring
description: "UE5（UnrealMCP）里用 MCP 工具 + python 脚本程序化建/改材质的方法：材质图的构建/连线/重建与状态回验流程、python 脚本内调用 bridge 命令（回环）的方式与命令表、Custom HLSL 的可用接口与写法（输入 pin、纹理输入、输出名规则）、单节点预览（preview_material_expression）的用法与语义边界、贴图等资产的导入与属性配置、逐步验证纪律。触发场景：准备调用以下任一 MCP 工具前 MUST 加载本 Skill —— create_material_expression / connect_material_pin / connect_material_expressions_safe / set_material_expression_property / set_asset_properties / wipe_material_graph / import_assets / add_blendable_to_post_volume / list_blendables / clear_blendables / scan_material_custom_nodes / get_material_graph / get_material_compile_errors / list_material_expressions / validate_custom_hlsl / preview_material_expression；以及任何涉及「用 MCP 改/建 UE 材质、材质图连线、后处理材质与描边、火焰效果、Custom HLSL、MaterialExpression、PostProcessVolume blendable、程序化 UV 平移动画、条带 UV、Custom 节点输入 pin、用 MCP 导入贴图或资产、看某个材质节点到底长什么样/掩码覆盖率/参数改动的效果」的任务。"
metadata:
  version: "1.5.0"
  upstream: ~
  downstream: ~
---

# UnrealMCP 材质编写 Skill

适用：UE 5.5 + 本仓库 `Plugins/UnrealMCP`（bridge `127.0.0.1:55557`，MCP server 名 `unrealMCP`）。
目标：用 MCP 工具 + python 脚本**可重复、可验证**地建/改材质（含后处理材质与 Custom HLSL）。

> 本文只写**可复用的做法与接口**；不收录具体配方、踩坑记录与工具缺陷。

**环境注意**：改了 `Plugins/UnrealMCP/Content/Python/**`（含工具描述）后需**重启 unrealMCP server**；改插件 C++ 则需编译 + 重启编辑器。

---

## 〇、材质图操作规范

### 0. 两条可用路径

| 用途 | MCP 工具族 | python API（`unreal.MaterialEditingLibrary.*`） |
|---|---|---|
| 建节点 | `create_material_expression(asset_path, expression_class, editor_x, editor_y, desc, code, inputs)` → 返回 `{name, desc, type, inputs, outputs}`（`inputs`/`outputs` 为**回读**结果，`code`/`inputs` 仅 Custom 可传） | `MEL.create_material_expression(mat, Class, x, y)` |
| 设属性 | `set_material_expression_property`（`code` / `default_value` / `texture` …） | `expr.set_editor_property(...)` |
| 连线 | `connect_material_expressions_safe(asset_path, source_name, target_name, input_name, source_output_name)` —— 源输出走 **`GetOutputs()` 虚函数**，动态输出节点（Add/Subtract、SceneTexture、Custom）也能当源；报错结构化（`output_not_found` 带 `available_outputs`、`input_not_found` 带 `available_inputs`） | `MEL.connect_material_expressions(源, 输出名, 目标, pin名)` —— 只读成员 `Outputs` 数组，需**精确**输出名 |
| 列节点（拿 name/desc/object_path） | `list_material_expressions(asset_path)`，`index` 仅诊断用，`object_path` 可给 python 直接 resolve 表达式对象；每项带 **`referenced`**（是否从材质属性引脚可达），顶层带 `unreferenced_count` | 无（`Material.expressions` 是 protected） |
| 读属性 / 读 pin 接线 | `get_material_expression_property(..., property="code"\|"inputs"\|...)`，`property="inputs"` 返回 `[{input_name, expression, output_index, connected}]` | 多数属性可读；pin 的 `Input` 是 protected，读不到 |
| 删节点（精确） | `delete_material_expressions(asset_path, [{...}])` 按 name/desc，逐项独立处理；顶层 `success` 反映子项（全失败为 false），部分失败给 `partial` + `failed_count`，逐项结果在 `results[]`（**必读**） | — |
| 整图 wipe | `wipe_material_graph(asset_path, max_iterations=32)` → `{iterations, deleted_total, remaining}`，`remaining` 必须为 0 | 循环 `delete_all_material_expressions` + GC（单遍只删一部分，见 §2） |
| 建资产 | `create_asset_safe(name, path, class, factory, recreate)`（无重命名模态框） | `AssetToolsHelpers.get_asset_tools().create_asset(...)` |
| 导入资产 | `import_assets(paths, destination_path, force_legacy=True, replace_existing=False)` → `{importer, imported_count, failed_count, results[], cvar_overrides[]}` | —（**禁止**手写 `AssetImportTask`，会崩编辑器，见 §二） |
| 写任意资产属性 | `set_asset_properties(asset_path, props)` → `{applied[], failed[], applied_count, failed_count}`（友好名 + 枚举名容错，见 §二） | `asset.set_editor_property(...)`（属性名拼错/受保护会直接抛异常） |

两条路径操作同一份内存图，可混用。**python 脚本里也能调这些命令**（bridge 回环，见 §8），所以"批量脚本"和"MCP 工具"并不互斥：批量建图走 python，需要结构化错误或逐节点精调时用工具，或两者在同一脚本里混用。

### 1. 改图前先关闭该资产的编辑器

```python
# agent 直接调工具
close_asset_editors(asset_path="/Game/MCP/Pond/M_Pond")

# 脚本内走 bridge 回环命令（同名）
import json, unreal
unreal.UnrealMCPPythonAPI.execute_mcp_command(
    "close_asset_editors", json.dumps({"asset_path": "/Game/MCP/Pond/M_Pond"}))
```
构建完成后需要时再打开。编辑器持有自己的图节点与撤销栈，关闭后再写图，改动能稳定落盘。

### 2. 重建流程（固定顺序）

**wipe → 建节点/连边 → 保存 → 回验 → 编译复查**

wipe 直接用工具，**不要**只调一次引擎的批量删除：

```python
wipe_material_graph(asset_path=MAT)          # → {iterations, deleted_total, remaining}
assert r["remaining"] == 0, r                 # remaining != 0 时工具返回 wipe_incomplete
```

原因：引擎的 `delete_all_material_expressions` 是**边遍历边 erase**，单遍只清掉一部分，所以需要"循环到空"。`wipe_material_graph` 内部就是"快照后逐个删 + GC"循环到 0，并把 `remaining` 回读给你，比脚本自己数轮次可靠。

只有走 python 批量脚本时才用等价循环（每轮都要 `collect_garbage`）：

```python
MEL = unreal.MaterialEditingLibrary
for i in range(12):
    MEL.delete_all_material_expressions(mat)
    unreal.SystemLibrary.collect_garbage()
    if MEL.get_num_material_expressions(mat) == 0:
        break
```

收尾断言：`get_num_material_expressions(mat) == 设计值`。本文的图层写法都能预先算出节点数（例：一个 Custom 节点承载 3 条鱼的整套逻辑 → 整图 49 节点），这是最直接的幂等校验。残留孤儿会占名字并虚高 `count`，`list_material_expressions` 的 `referenced: false` 与 `unreferenced_count` 可用来确认是否真的清干净。

### 3. 建节点 → 设属性 → 连线的命名约定

**MCP 工具路径**：先 `list_material_expressions` 拿到目标节点的 `name`/`desc`，再
 
```
connect_material_expressions_safe(asset_path, source_name="...", target_name="...",
                                 input_name="A", source_output_name="")
```
- `input_name`：C++ 成员名（`A` / `B` / `Alpha` / `ExponentIn` / …），`""` = 第一个输入；Custom 节点用声明时的 `input_name`
- `source_output_name`：`""` = 第一个输出；具名输出（如 SceneTexture 的 `Color`）显式传

**python API 路径**（批量脚本）：

```python
MEL.connect_material_expressions(源表达式, 输出名, 目标表达式, 输入pin名)   # 返回 bool
MEL.connect_material_property(表达式, 输出名, unreal.MaterialProperty.MP_BASE_COLOR)
```

需要**精确**输出名：

| 源节点 | 整向量输出名 |
|---|---|
| `TextureSample` | `"RGB"`（单通道 `"R"/"G"/"B"/"A"`） |
| `VectorParameter` | `""` |
| `TextureCoordinate` | `""` |
| 单输出节点（Add/Multiply/Lerp/Clamp/...） | `""` |

返回 False 时，把两端节点的类名、`get_outputs()` 名字、目标 pin 名一起打出来自查。

### 4. Custom 节点的输入 pin

**首选：一次调用把节点、`code`、引脚全建好**（MCP 工具）：

```python
create_material_expression(
    asset_path=MAT, expression_class="MaterialExpressionCustom", editor_x=-800, editor_y=0,
    desc="FishComposite",
    code=HLSL,
    inputs=["In_Base", "In_UV", "In_Time",
            "Tex_Fish0", "Tex_Fish1", "Tex_Fish2",
            "In_FishSpeeds", "In_FishHeightUV", "In_FishOpacity"],   # 顺序 = HLSL 参数顺序
)
# → 返回 inputs（回读引脚：input_name / expression / output_index / connected）
#   与 outputs（该节点合法输出名 [{output_index, name}]）
```

- 引脚**只有名字**：引擎 `FCustomInput` 只有 `InputName` 与 `Input`，引脚类型由上游连接决定，所以 `inputs` 不收类型字段。`inputs` 也接受 `[{"input_name": "In_Base"}, ...]` 形态。
- `code` / `inputs` **只对 `MaterialExpressionCustom` 有效**：其他类传了返回结构化错误且不建任何节点。
- 传了 `code` 时**先跑 HLSL lint 再建节点**：有 error 直接拒绝（`invalid_custom_hlsl`），图完全不变——不必"先建再校验再删"。

**python 路径**（批量脚本里建节点时等价可用）：

```python
node = MEL.create_material_expression(mat, unreal.MaterialExpressionCustom, x, y)
node.set_editor_property("code", HLSL)
node.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
node.set_editor_property("description", "FishComposite")

PIN_NAMES = ["In_Base", "In_UV", "In_Time",
             "Tex_Fish0", "Tex_Fish1", "Tex_Fish2",
             "In_FishSpeeds", "In_FishHeightUV", "In_FishOpacity"]
pins = []
for n in PIN_NAMES:                       # 顺序 = HLSL 参数顺序
    ci = unreal.CustomInput()
    ci.set_editor_property("input_name", n)   # 字段名是 input_name
    pins.append(ci)
node.set_editor_property("inputs", pins)

# 回读确认
print([i.get_editor_property("input_name") for i in node.get_editor_property("inputs")])
```

**纹理输入 pin**：用 `MaterialExpressionTextureObject`（设 `texture` 属性）接过去；HLSL 里直接
`Texture2DSample(texInput, texInputSampler, uv)` —— 引擎会为每个纹理输入额外提供 `<输入名>Sampler`（`HLSLMaterialTranslator.cpp:15338`），所以世界坐标 UV 与纹理采样可以同时存在。

- **建 pin 有两条路**：建节点时 `create_material_expression(..., inputs=[...])`（推荐），或 python 的 `set_editor_property("inputs", pins)`；**给已存在的节点加/删引脚用 `add_custom_input` / `remove_custom_input`**（单元素插入/删除，其余引脚的连线不动），改名用 `set_custom_input_name`（改名保连线）。三个都是"只动一个元素"，所以不会像整表赋值那样把连线抹掉。
- **加完 pin 必须逐个接线**，并回读确认：`get_material_expression_property(..., property="inputs")` 里每项的 `connected` 才是判据 —— **别去数 pin 的个数**。编译若报 `Custom material <节点> missing input N (X)`，含义是"**X 这个输入没接线**"（`N` 从 1 开始），不是引脚数组缺项；`get_material_compile_errors` 的 `error_details` 已经把这条原文解成 `kind=custom_input_unconnected` + `pin_name` + 未接线引脚所属的 `expression_name`，直接照它查连线即可；引擎在**第一个**未接线引脚处就中止该节点的编译，所以接好一个可能再报下一个。
- **删表达式**按 `expression_name` / `expression_desc` / `expression_type` 寻址；复数版本要**逐项读 `results[]`** —— 顶层 `success` 现在忠实反映子项（全失败为 `false`，部分失败给 `partial` + `failed_count`），但仍然逐项独立、不回滚。同 desc 的多个节点会被拒（`ambiguous_expression` + `candidates`），按 exact `name` 删。
- **寻址键只有这三个名字**（`name` / `desc` / `type` **不读**）。键名写错时错误里带 `unknown_keys` + `did_you_mean`（如 `{"name": "expression_name"}`），照它改键名即可 —— 这时 `expression_not_found` 说的是"键没被认出来"，不是节点不存在。
- **回环（`execute_python_*` 内）只能调已注册的 bridge 命令**：绝大多数 MCP 工具同名转发一条命令（`create_asset_safe` / `safe_delete_asset` / `import_assets` / `list_mcp_commands` 都是命令，回环里可直接调）；**只有 python 实现**的工具（如 `preview_material_expression`）不可回环，要它就走 MCP 工具面。调不存在的名字会得到 `unknown_command` + `hint` + `registered_command_count`（后者用来确认注册表本身是好的）。回环内建/删资产后回读 `does_asset_exist` 确认。
- 回读校验：`create_material_expression` 的 `inputs` 返回值、`get_material_expression_property(..., property="inputs")`、或 `list_material_expressions`（每个 pin 带 `connected`）。
- 经 MCP 写 `code`（无论建节点时写还是走 `set_material_expression_property`）服务端都先跑 HLSL lint：有 error 直接拒绝（`invalid_custom_hlsl`，材质不变），warnings 随返回带回。
- python 建节点与 MCP 工具连线**可以混用**（`connect_material_expressions_safe` 会按需补出 `Outputs`）；走 MCP 建节点时输出已由工具回读在 `outputs` 里，不必再猜输出名。

### 5. 保存与回验

```python
MEL.recompile_material(mat)
unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
print("node count:", MEL.get_num_material_expressions(mat))   # 回验
```
状态判定以**直读**为准：节点数用 `get_num_material_expressions`；节点清单与接线用 `list_material_expressions`（含 `object_path` 与每个 pin 的 `connected`）或 python 读表达式属性。`get_material_graph` 的 `edges` 可与之互验，用来核对连线。

**改完表达式必须触发一次"材质级"刷新，否则场景还画旧状态**：只改**表达式**属性（Custom 节点的 `code`、参数的 `default_value`）时，即使 `MEL.recompile_material(mat)` + `save_asset()` 之后**回读全对、`get_material_compile_errors` 也为空**，视口仍渲染**旧的编译产物/uniform** —— 缺的是材质级的 `PostEditChange`（那一步才 `CacheShaders` 并把材质的 render proxy 标脏）。判据就是"**资产对了、画面没变**"。两条修法：

- 用**封装好这一步的收口命令**：`recompile_material(asset_path, refresh=true, save=...)` —— `refresh` 就是"写一个材质级属性再写回"（内部取反再取回 `two_sided`）后的一次 `PostEditChange`，紧接一次重编、按需落盘。批量建图**每槽收口调它一次**即可，不要自己散着写。

> 这条能解释一个"看起来矛盾"的现象：同一批材质里**改过材质级属性的那些会立刻生效**，而**只动表达式的那些不会**。所以"画面对不上"时先把**刷新**排除掉，再去怀疑 UV / 贴图 / 法线。

### 6. 编译复查

```python
recompile_material(mat)
get_material_compile_errors(asset_path)      # 可选 since=<ISO-8601 或 epoch 秒>；异步：隔一次调用再复查
```

返回几块，**权责不同**：

| 字段 | 含义 |
|---|---|
| `errors_by_feature_level` | 引擎 `GetMaterialResource().GetCompileErrors()`——**判定"当前是否编译失败"只看这里** |
| `error_details` | 把已知的引擎原文解成结构化条目（`kind` + `pin_name` + `expression_name` + `feature_levels` + `hint`），先读它再读原文 |
| `log_errors_this_compile` | 日志兜底中**晚于分界**的行，每条 `{line, timestamp, age_seconds}` |
| `log_errors_historical` | 更早的行——改图中途那次编译失败会落这里，**不要**当成当前错误 |

同一条原文在 SM5/SM6 都会报，`error_details` 已按原文去重并列出 `feature_levels`。引擎的报错文本里点名节点用的是 Custom 节点自己的 `Description`（默认全是 `"Custom"`），所以唯一命中时才给 `expression_name`，否则给 `expression_name_candidates` —— 这一点决定了"知道是哪个节点报的"要靠解图而不是靠原文。

分界默认取"本材质最近一次 MCP 改图命令的时刻"，`boundary_source` 说明来源（`material_mutation` / `explicit_since` / `unknown`）、`since_boundary` 给出具体时刻（UTC）。`unknown` = 服务端没有该材质的改动记录（材质刚打开、未经过 MCP 改图），此时全部日志行按历史处理。时间一律 UTC 比较，别拿本地时间手算。

### 7. 批量脚本

长脚本写到 `Saved/MCPScripts/*.py` → `execute_python_file(file_path=..., deferred=True)` → `poll_python_job(job_id)`。脚本内用 `print()` 输出中间状态；需要等待就直接拆成多次调用。

### 8. 在 python 脚本里调 MCP 命令（bridge 回环）

编辑器 python（`execute_python_command` / `execute_python_file`）已经在 GameThread 上，**不能**走 TCP 发命令（bridge 会把命令排回 GameThread，脚本又同步等响应 → 必然死锁）。改用反射静态函数，同步派发到与 TCP 完全相同的命令处理器：

```python
import json, unreal

def bridge(cmd, **params):
    return json.loads(unreal.UnrealMCPPythonAPI.execute_mcp_command(cmd, json.dumps(params)))

r = bridge("list_material_expressions", asset_path="/Game/MCP/Pond/M_Pond.M_Pond")
print(r["result"]["count"], r["result"]["expressions"][0]["object_path"])

bridge("create_material_expression", asset_path=MAT,
       expression_class="MaterialExpressionTextureSample", editor_x=-600, editor_y=200)
```

- 返回 **JSON 字符串**，形状与 TCP 响应一致：`{"status": "success", "result": {...}}`。
- **`result` 里有两种信封，取包要兼容**：用 `CreateSuccessResponse` 的命令回 `result = {success: true, data: {…}}`，扁平命令回 `result = {…}`（如 `create_material_expression` 直接给 `name`/`inputs`）。按错形状取会把成功读成 `None`（本仓库连续误判过两轮）。两种都吃的写法：
  ```python
  def payload(r):
      res = r.get("result") or {}
      return res["data"] if isinstance(res, dict) and isinstance(res.get("data"), dict) else res
  ```
  读取失败时把 `r` 整个打出来 —— 错误信息可能在 `r["error"]` 里，而数据可能在 `result["error"]` 里。
- 参数可传裸对象，也可传 TCP 风格 `{"command": ..., "params": {...}}`（`command` 字段被忽略，第一个实参决定命令）。
- **无排队、无 future 等待**（同一个同步 dispatcher），一个脚本里连续几十次调用是安全的。
- 回环禁用名单：`execute_python_command` / `execute_python_file` / `poll_python_job` / `take_screenshot` → `{"status":"error","error":"reentry_forbidden"}`（这四个只有 TCP 路径可用）。其他失败形态：`wrong_thread` / `bridge_unavailable` / `invalid_params_json`。
  - 该名单来自命令注册表的 `loopback_forbidden` flag（不是代码里的手写清单）；要看全量命令名/参数/策略，用自省工具 `list_mcp_commands`（可选 `category="material"`）。

**命令名 = MCP 工具名（多数）**，少数不同：

| MCP 工具 | bridge 命令 | 命令参数 |
|---|---|---|
| `connect_material_expressions_safe`（表达式 → 表达式） | `connect_material_expression` | `{asset_path, source_name, target_name, output_name, input_name, source_desc?, target_desc?}`（注意是 **`output_name`**，不是工具层的 `source_output_name`） |
| `connect_material_pin`（表达式 → **材质属性**） | `connect_material_pin` | `{asset_path, property, expression_name\|expression_type\|expression_desc, source_output_name?}` |
| `create_material_expression` | 同名 | `{asset_path, expression_class, editor_x, editor_y, desc?, code?, inputs?}`（`code`/`inputs` 仅 `MaterialExpressionCustom`） |
| `set_material_expression_property` | 同名 | `{asset_path, expression_name, property, value, expression_desc?, recompile?}` |
| `get_material_expression_property` | 同名 | `{asset_path, expression_name, property, expression_desc?}`（`property="inputs"` 是虚拟读） |
| `list_material_expressions` / `get_material_graph` | 同名 | `{asset_path}` |
| `get_material_compile_errors` | 同名 | `{asset_path, since?}`（`since` = ISO-8601 或 epoch 秒） |
| `recompile_material` | 同名 | `{asset_path, refresh?, save?}`（`refresh` 默认 true = 材质级刷新；批量建图**每槽收口一次**，连线命令本身不再重编） |
| `wipe_material_graph` | 同名 | `{asset_path, max_iterations?}` |
| `validate_custom_hlsl` | 同名 | `{code, output_type?}` |
| `set_custom_input_name` | 同名 | `{asset_path, expression_name, old_name, new_name, expression_desc?, recompile?}` |
| `add_custom_input` | 同名 | `{asset_path, expression_name, input_name, index?, expression_desc?, recompile?}`（`index` 默认追加；`inputs` 顺序 = HLSL 参数顺序） |
| `remove_custom_input` | 同名 | `{asset_path, expression_name, input_name, expression_desc?, recompile?}`（回 `was_connected` / `disconnected_from`） |
| `delete_material_expressions` | 同名 | `{asset_path, expressions: [{name\|desc}...]}` |
| `get_material_parameters` / `set_material_parameters` | 同名 | `{asset_path[, values, recompile]}` |
| `import_assets` | 同名 | `{paths: [...], destination_path, force_legacy?, replace_existing?}` |
| `set_asset_properties` | 同名 | `{asset_path, props: {...}}` |

**批处理纪律**：`set_material_expression_property` / `set_custom_input_name` / `set_material_parameters` / `add_custom_input` / `remove_custom_input` 的 `recompile` 默认 **false** —— 批内全部保持 false。**连线类命令（`connect_material_expression` / `connect_material_pin` / `disconnect_material_property`）不再自行重编**，所以批量建图的做法是：先把整张图连完，再对每个材质调一次 `recompile_material(asset_path, save=true)` 收口（它同时做"材质级刷新"，缺了它资产对但视口不变）—— 每次重编都是一次完整 shader 编译，逐步重编是纯浪费。

> 连线命令去掉强制重编的实测账（每根线 ~250ms、10 根线 ≈2.4s/材质，且编的还是半成品图）：见 `Docs/MCP_Findings_2026-09-27_*.md` 与 `Docs/MCP_Tool_Improvement_Proposals_2026-09-27.md`。

> 临时夹具（探针材质）**删除必须走 `safe_delete_asset`**：用 `EditorAssetLibrary.delete_asset` 删"仍被引用"的刚建资产会失败并把包标记为 `potentially corrupt`，随后编辑器在刷新时 AV。证据见 `Docs/MCP_Findings_2026-09-27_asset-delete-corrupt-package.md`。

**注意**：直接调底层命令会跳过 `connect_material_expressions_safe` 那层保护（它自己会先 `list_material_expressions` 解析、连完再 `get_material_graph` 回验），所以命令里的 `source_name` / `target_name` 必须是 `list_material_expressions` 返回的**精确 name**。写 `code` 仍会被服务端 lint 拦下（`invalid_custom_hlsl`，材质不变）；数组属性（Custom 的 `inputs`）**整表写**被拒（`unsupported_property_type`）—— 新建节点用 `create_material_expression` 的 `inputs` 参数一次建好，已有节点用 `add_custom_input` / `remove_custom_input` 单元素增删，或 python 侧 `set_editor_property("inputs", pins)`。

---

## 一、Custom HLSL 可用接口

| 目标 | 写法 |
|---|---|
| 场景纹理取值 | `SceneTextureLookup` / `SceneTextureFetch` / `pin.Fetch(偏移)` / `ViewportUVToSceneTextureUV` / `GetSceneTextureViewSize` / `CalcSceneCustomDepth` |
| 探针取样 | `pin.Fetch(像素偏移)` —— default UV、texel 步长、clamp 都由引擎完成；也可把偏移接到 `SceneTexture` 节点的 `Coordinates` |
| 世界坐标 | `SvPositionToWorld(float4(pixelXY, deviceZ, 1))`；后处理用 `SvPositionToTranslatedWorld` |
| FOV | `GetTanHalfFieldOfView().y` |
| 时间 | `View.RealTime` |
| 纹理采样 | `Texture2DSample(texInput, texInputSampler, uv)` |

场景纹理的 UV / 尺寸换算一律交给上述引擎函数（`pin.Fetch` 或节点的 `Coordinates` / `Size` 输出）。

**Custom 节点里的裸 HLSL 会被原样塞进该材质的所有着色排列**（含 Lumen card / debug view 的 **VS**），所以只能写"到处都合法"的标识符：

- `VertexNormalWS` / `CameraVectorWS` 这类**只存在于像素着色器的 shortcut 名，写进 Custom 代码会让 `errors_by_feature_level.SM6` 非空**（`use of undeclared identifier`，且只炸在 `FLumenCardVS` / `FDebugViewModeVS` 这类排列里，编译"看起来部分成功"）。法线、相机向量要**从输入引脚接进来**（`MaterialExpressionVertexNormalWS` / `MaterialExpressionCameraVectorWS`），HLSL 里只用引脚名。
- **不要点名着色器参数结构体**（`View` / `ResolvedView` / 各 UB struct）：本仓库有实测记录，这么做会撞 `RHICoreShader.cpp:52` 的 uniform buffer 布局断言、**直接把编辑器关掉** —— 能编译 ≠ 绘制时安全。
- 要主光方向时：UE 5.5 **没有** `GetPrimaryLightDirection()`（探针实测 `use of undeclared identifier`）；`ResolvedView.DirectionalLightDirection` 编译干净（它**指向光源**，`dot(N, L)` 不取负），但要承担上一条的风险 —— **首选**把 `MaterialExpressionAtmosphericLightVector` 的输出接到引脚（引擎表达式，不需要同步，见 §五.3）；它不可用时才退回 `VectorParameter("LightDir")`（代价：默认值要自己取关卡太阳的反向 `-DirectionalLight.forward`，且太阳转动后不会跟随）。
- `pin.Fetch(offset)` 的 offset 是**视图像素**（default UV / texel 步长 / clamp 引擎都处理），所以 stencil 过滤可以直接写 `stencilTex.Fetch(off).r >= StencilValue - 0.25`。

---

## 二、资产导入与属性配置（以贴图为例）

### 导入：必须走 `import_assets` 工具

```python
import_assets(paths=[src_png], destination_path="/Game/MCP/Pond")     # replace_existing=True 才覆盖同名
# → {importer: "legacy", imported_count, failed_count,
#    results: [{source_path, asset_path, imported, error}],
#    cvar_overrides: [{cvar, requested, actual, verified}]}
```

**不要**在 `execute_python_command` / `execute_python_file` 里手写 `AssetImportTask` + `import_asset_tasks`：Interchange（UE5 默认贴图导入器）会在导入内部抽干 GameThread 任务队列，撞上 bridge 派发任务里的 TaskGraph 重入断言，**编辑器直接崩掉**——不是错误返回，是进程消失。导入一律走上面的 `import_assets`。

工具内部做的事：把本次涉及的每个扩展名的 `Interchange.FeatureFlags.Import.<EXT>`（PNG/BMP/EXR/HDR/TGA/TIFF/JPG/PSD/DDS/IES/FBX/OBJ…）置为关闭并**回读断言**（不一致就返回 `cvar_override_failed` 且一个文件都不导入），让导入回落到 legacy 同步工厂。副作用：这些 flag 在本编辑器会话内保持关闭，`cvar_overrides` 逐条列出，不落 ini（重启编辑器即恢复）。

单文件失败不中断整批：源文件不存在 → `source_not_found`；目标已存在且未允许覆盖 → `asset_exists`。

### 属性配置：优先 `set_asset_properties`

```python
set_asset_properties(
    asset_path="/Game/MCP/Pond/T_mask_r.T_mask_r",
    props={
        "srgb": False,
        "compression": "TC_MASKS",
        "no_alpha": True,
        "lod_group": "TEXTUREGROUP_WORLD",
        "filter": "TF_BILINEAR",
        "mip_gen": "TMGS_NO_MIPMAPS",
    },
)
# → {applied: [{key, property, value_before, value_after}],
#    failed:  [{key, error, message, candidates?}], applied_count, failed_count}
```

- 友好名：`srgb` / `compression`（=`compression_settings`）/ `compression_quality` / `lod_group`（别名 `texture_group`）/ `no_alpha`（=`compression_no_alpha`）/ `mip_gen`（=`mip_gen_settings`）/ `filter`。
- **两种拼法都能解析**：反射里存的是 C++ 名（`LODGroup`、`MipGenSettings`、`CompressionNoAlpha`、`AssetImportData`），python 显示的是 snake_case（`lod_group`…）；匹配忽略大小写**与下划线**，`applied[].property` 回读反射里的真实名。
- 枚举成员同样忽略大小写与下划线（`TEXTUREGROUP_WORLD_NORMALMAP` 与 `TEXTUREGROUP_WorldNormalMap` 等价）；解析失败给 `unknown_enum_member` + 全量 `candidates`（候选是 C++ UENUM 名）。
- 逐项独立：单项失败不影响其余项、不回滚，看 `failed_count`。错误码：`unknown_property`（反射里没这个属性，`has_alpha_channel` 这类计算属性属于此类）、`property_not_writable`（存在但引擎托管，如 `AssetImportData`）、`unknown_enum_member`、`write_failed`。

python 侧 `set_editor_property` 只在需要写工具未覆盖的属性时兜底（拼错属性名或写受保护属性会直接抛异常）：

```python
tex = unreal.EditorAssetLibrary.load_asset("/Game/MCP/Pond/T_mask_r")
tex.set_editor_property("srgb", False)
tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_MASKS)
unreal.EditorAssetLibrary.save_asset("/Game/MCP/Pond/T_mask_r", only_if_is_dirty=False)
```

**属性名与枚举名清单**（python 视角，常用组合）：

| 用途 | 属性名 | 取值 |
|---|---|---|
| sRGB | `srgb` | bool |
| 压缩 | `compression_settings` | `TC_DEFAULT` / `TC_NORMALMAP` / `TC_GRAYSCALE` / `TC_MASKS` |
| 保留 alpha | `compression_no_alpha` | 带 alpha 的贴图设 `False` |
| LOD 组 | `lod_group` | `TEXTUREGROUP_WORLD` / `TEXTUREGROUP_WORLD_NORMAL_MAP` / `TEXTUREGROUP_UI` |
| 过滤 | `filter` | `TF_BILINEAR` 等 |
| mip 生成 | `mip_gen_settings` | `TMGS_NO_MIPMAPS` / `TMGS_FROM_TEXTURE_GROUP` |

枚举值里的名字是 **python 风格**（大写 + 下划线）；C++ UENUM 里叫 `TEXTUREGROUP_WorldNormalMap`、`TMGS_NoMipmaps` 这种驼峰名。两种写法在 `set_asset_properties` 里都能用（匹配忽略大小写与下划线），但 `candidates` 回读的是 C++ 名，别以为工具给错了。

**先导 1 张**并读回尺寸与属性作为基准，再导全量。

---

## 三、验证纪律

每一步之后**断言实际状态**，不要沿用"上一步应该成功了"：

1. **节点数**：`get_num_material_expressions(mat)` == 设计值；
2. **代码落盘**：直读 Custom 表达式的 `code`，校验本次改动；保存函数的返回值不作证据；
3. **编译**：`get_material_compile_errors` 隔一次调用复查（shader 编译是异步的）。判定当前状态**只看 `errors_by_feature_level`**；需要日志佐证时看 `log_errors_this_compile`（晚于最近一次改图的行），更早的行在 `log_errors_historical`，两者别混用。已知原文先读 `error_details`（`kind` + `pin_name` + `expression_name`）。**孤儿节点不参与编译**：没接到任何材质属性的表达式（含引脚被删光的 Custom）编译结果永远是空 —— `errors_by_feature_level` 为空只说明"这个节点这轮没被编译"，不能当"编译通过"的证据，夹具里先把它接到 `EmissiveColor` 才有编译结论；
4. **绑定**：`list_blendables` 确认目标材质挂在 volume 上；
5. **日志时间切分**：判断"本次编译是否报了 `Failed to compile Material`"优先用 `get_material_compile_errors` 的 `log_errors_this_compile`（服务端已按"最近一次 MCP 改图时刻"切好，每条带 `timestamp` / `age_seconds`；改动密集时可用 `since` 自定分界）。只有需要 grep 编译标记以外的内容时，才回到手动的"改动前记日志行数、改完读增量"老办法；
6. **效果**：视觉验收由用户在编辑器视口完成，不要用截图/像素分析代替。

---

## 四、单节点预览（诊断）

### 1. 工具与语义

`preview_material_expression(asset_path, expression_name | expression_desc, output_index=0, channel="rgba", size=256, keep_temp=False, wait_s=20)`：把材质图里**某一个节点**单独渲染成 PNG，用来判断"这一层到底长什么样"（崩口掩码、两个 UV 的差异、某条 tint 链把颜色压暗了多少）。

- **只做诊断，不做观感验收**：无光照、无相机、无场景，世界空间与视角相关节点会**退化成常量**（`WS_UV` 这类节点的值本身不可信，要看依赖它的下游节点）
- 返回 `png_path`（RGB PNG，落在 `Saved/MCPMaterialPreview/`）+ `samples`（中心像素与 3x3 均值的 **0..1 线性值**，数值断言用）+ `expression` / `copied_nodes` / `render_settled` / `waited_ms`
- `channel="r|g|b|a"`：用 `ComponentMask` 取单通道（掩码类节点这样看形状）；`output_index`：多输出节点的槽位（Vector 参数 0..4 分别是 RGBA 分量）
- 默认用完即删预览材质；`keep_temp=True` 保留以便排查。失败时返回结构化 `error_code`（`expression_not_found` 带 `candidates`、`output_index_not_found` 带 `available_outputs`、`material_not_found`、`invalid_channel`…）

### 2. 内部机制（以及为什么不要自己 duplicate）

工具内部走 bridge 命令 `build_material_preview`：在 `/Game/MCP/_Preview/PM_<Asset>__<节点>` **新建**一个材质，clone 目标节点及其**上游闭包**，设 `MSM_UNLIT` 接 `EmissiveColor`，再 `ForceRecompileForRendering(Synchronous)`；python 侧只负责画到 RT、编码 PNG（线性值按 sRGB 写入）、清理。

> **不要用"duplicate 材质再改图"来实现预览**：会话内复制出来的材质**永远用源材质已编译的 shader**，图改了画面也不变（`RecompileMaterial`、save、`reload_packages`、unload+load、rename、改 usage、挂 MIC 全部无效），画出来就是源材质的样子。预览材质必须**新建**——这也是这一步放在 C++ 里的原因。
> 同理 **不要用 `export_render_target` 出图**：FCanvas 用纯颜色 blend state，RT 的 alpha 恒为 0，导出的是全透明 PNG；用本工具自己编码的 PNG。

### 3. 建议的排查顺序

1. `list_material_expressions` 拿到准确的 `name`/`desc`（预览报错会把候选列出来）
2. 掩码/权重类节点：`channel=r`（或 `a`）看形状，再用 `samples` 判数值（例如崩口覆盖率）
3. 参数类节点：直接拿 `samples` 对参数值（如 `Stain_Tint(0.42,0.38,0.32)` → `[0.4196, 0.3804, 0.3216]`）
4. 世界空间链路：预期就是常量，别据此判断材质坏了

---

## 五、MMD 材质 → UE 阶调 toon（可复用）

导入的 MMD 角色"有贴图但没有二次元感"时的判据与做法（甘雨 19 个材质全套实测）。

### 1. 先分清是"掉观感"还是"掉贴图"

逐个读材质图：若每个材质都是**单节点 `TextureSample → BaseColor`、`MSM_DEFAULT_LIT`、`BLEND_OPAQUE`、单面**，那是导入器丢掉了 MMD 的 toon 语义，贴图本身没丢（核 `srgb` / `compression_no_alpha`）。"补观感"只需改这 N 个材质本身，**网格槽位不用动**、不用重导。

### 2. 源头判据：blend / 双面去读 PMX，别凭观感猜

`Saved/MCPScripts/pmx_texdump.py <模型.pmx>` 直接解出每个材质的贴图、**no-cull 标志（0x01 = 双面）**、shared toon 索引（`-1` = 无阶调）、`diffuse.a`、edge 颜色/粗细；`png_alpha_stats.py` 看每张贴图**是否真有 alpha 通道**。据此定：

- **双面**：照抄 PMX 的 no-cull（本例 19/19 全双面）。
- **blend**：只有**有 alpha 通道**的贴图才谈得上 Masked（`OpacityMask` 取贴图 alpha）；贴图无 alpha 通道的槽设 Masked 毫无意义（alpha 恒 1）→ Opaque；alpha **全部 < 0.5** 的柔和叠加层（腮红类）只能用 Translucent（Masked 会把整块剪掉）。
- toon 索引 `-1` 的槽（眼睛/眼白）用 `Bands=1 + ShadowTint=(1,1,1)` 还原"无阶调"，不需要额外节点分支。
- **PMX 的 `dif.a` 不能当"透不透明"的依据**：它常是 1.0，而美术意图藏在**贴图 alpha** 里。判据取「该材质 UV 框内的 alpha 统计」（不是整张贴图）：`>=128` 占多数 ⇒ Masked 合适；**全部 `<128`** ⇒ Masked 会把整块剪掉、只能 Translucent（本例 `脸红`：`dif.a=1.0` 但框内 alpha 100% `<128`、均值 21.9/255 ⇒ 柔和叠加层）。整张贴图的统计会误导：`表情.png` 全图 98.5% 的 alpha 为 0，看着像"没有 alpha 可用"。
- "某槽引用了看起来不对的贴图"（如 `目` 采发丝图）：**先解 PMX 再判**——本例证实源模型就是这么配的 ⇒ 不该改。

### 3. 阶调怎么建（UE 5.5 没有 toon shading model）

**光方向这一项不要用参数同步**：接引擎的 `MaterialExpressionAtmosphericLightVector`（= `ResolvedView.AtmosphereLightDirection[0]` = `-DirectionalLight.forward`，指向光源），太阳转动**自动**跟随，不需要 MPC / 蓝图 / 编辑器期重跑。同族节点里只有这一个可用：

| 节点 | 能不能用 |
|---|---|
| `MaterialExpressionAtmosphericLightVector` | **能**（unlit 里也有效；`SceneRendering.cpp:1381,1526` 给的是 `-GetDirection()`） |
| `MaterialExpressionLightVector` | **不能**：基 pass 里 `Parameters.LightVector = 0`（`MaterialTemplate.ush:4439`），unlit 恒为 0 |
| `MaterialExpressionObjectOrientation` | **不能当"朝向"**：它是**局部 Z 轴**（`PrimitiveUniformShaderParametersBuilder.h:388`），不是角色正前方 |
| 裸 `ResolvedView.DirectionalLightDirection` 写进 Custom | **禁止**（本仓库有 uniform buffer 断言崩编辑器的先例，见 §一） |

**换空间要用 `Transform` 节点，注意两个枚举成员名不对称**：source 是 `EMaterialVectorCoordTransformSource`（成员 `TRANSFORMSOURCE_*`），destination 是 `EMaterialVectorCoordTransform`（成员 `TRANSFORM_*`）；写错回 `unknown_enum_member` + 可用成员表。World→Local 的向量变换只支持 Surface 域（`HLSLMaterialTranslator.cpp:10952-10966`）。

**给导入模型做"脸部/局部方向"类效果前，先标定模型自己的局部坐标轴**：本例（MMD 甘雨）脸在局部 **+Y**（眼骨局部 `y≈+21.8`）、局部单位 cm、网格 z∈[0,159.2]；旧代码里那个 `(x,0,z)` 投影"丢掉 Y"其实就是沿脸平面法线压平 —— 不标定就不知道自己在压哪个轴。标定脚本与数据：`Saved/MCPScripts/toon_face_centre.py` / `.txt`。


`MSM_UNLIT` + 一个 Custom 节点直连 `EmissiveColor`：`NdotL = saturate(dot(N, L))` → 量化 `floor(NdotL*bands)/max(bands-1,1)` → `tex*BaseTint*lerp(ShadowTint,1,ramp) + spec + rim`。参数只有阶调类：`Bands` / `ShadowTint` / `SpecularStrength` / `RimStrength` / `BaseTint` —— **光方向不是参数**，`In_LightDir` 由上面的引擎节点直接喂（法线、相机走输入引脚，见 §一 补充）。节点数是可预算的（本例 **10 节点/材质**），拿来当幂等断言；`wipe_material_graph` 的 `remaining` 必须为 0。取舍要明说：UNLIT 不吃场景光照/阴影。

**阶调的边缘要"量化 + 窄窗软化"，不要纯 `floor`**：纯 `floor` 会在阶跃处产生锯齿并在运动时闪烁；做法是先量化再只在每个 band 的最后一小段（本例 6%）`smoothstep`。这样既能拿到硬边界，又不抖。

**高光要单独做成 cel 高光**（这条最容易翻车）：直接搬图形学教科书的 Blinn-Phong `spec = pow(saturate(dot(N,H)), 32) * strength` 会立刻读成"塑料感"，因为它叶内连续衰减、边缘很宽、还会在背光面亮一块。二次元高光的要求恰好相反 —— **硬边、内部平坦、只在受光侧**：

| 病征 | 改法 |
|---|---|
| 叶内渐变 | `smoothstep(0.955, 0.995, NdotH)`：块内恒为 1，边缘只有 ~2–3 px |
| 爬到暗面 | 乘一个 `NdotL` 闸门，如 `smoothstep(0.02, 0.15, lit)` |
| 想给某些槽关掉阶调（`Bands=1`）时 | 闸门**不能**复用 `ramp`：`Bands=1` 时 `ramp ≡ 0`，会把这些槽的高光整个抹掉。用 `N·L` 独立判定 |

`SpecularStrength` 仍做成标量参数（逐槽可调亮度），但块的**形状与位置不做旋钮** —— 要让用户调的是"亮不亮"，不是"软不软"。

**暗贴图会被"乘法阴影"压成看着是黑**：阴影项是 `tex × lerp(ShadowTint, 1, ramp)`，所以源贴图本身就暗的槽（本例 `角`：它的 UV 区域内 mean 亮度只有 72、最暗 31，而可见的 UV 岛是暗的那半）在阴影片里掉到 ~25/255 —— 视口里就是一块死黑。

- **判断（离线，不用渲染）**：解 PMX 拿该材质的 UV 范围，再用 PIL 采贴图在该范围内的统计（`pmx_uv_region.py`；它还给 V 翻转后的区域，用来排除"上下翻错"）。本例角区域**两个方向都不是黑的** ⇒ 排除贴图/UV，锁定"乘法把暗部压死"。
- **修法（按代价从低到高）**：① 只给这一个槽把 `ShadowTint` **抬高到中性灰**（本例 0.85 ⇒ 72×0.85≈61/255）——保留阶调与随光移动的边界，只是不再压死；② 彻底去掉乘法（`ShadowTint=(1,1,1)`，观感变成"不随光变暗"的平贴图）。**不要去动全局默认值**——每个槽有自己的材质，改它只影响它自己。实测 0.35→25/255（视口里就是黑）、0.75→54、0.85→61、1.0→72；两个候选值都能"看着正常"，选哪个取决于要不要保留明暗响应。
- **要"自发光感"先看现有参数够不够**：UNLIT + 直连 `EmissiveColor` 的 toon 材质里，"常亮"= 去掉阶调（`Bands=1`）且去掉阴影乘法（`ShadowTint` 全白）——平坦输出与自发光等价，**不必新增节点**（新增会破节点数幂等断言）。代价：该槽从此完全不参与明暗。
- **逐槽微调不必整批重建**：`set_material_parameters` + `recompile_material(refresh=true, save=true)` 即可（`refresh` 那步缺了视口不更新）；但**构建脚本的批次表必须同步改成真值**，否则下次重建静默退回旧观感。
- **排查顺序**：`数学没错但看着是黑` 时，先怀疑乘法阴影，**不要**先怀疑法线/UV。判"是不是这个槽的材质在画"最省事的办法：把该槽换成纯色自发光材质（一眼看出是哪块几何），再换成纯 `TextureSample → Emissive` 对照（贴图 vs 数学，两个变量一次切开）。

### 4. 描边（后处理 + CustomDepth/Stencil）

`MD_POST_PROCESS` 材质 + 4 个 `SceneTexture` 节点，PPI id 实测：`1 = SceneDepth`、`13 = CustomDepth`、`14 = PostProcessInput0`、`25 = CustomStencil`。用 `pin.Fetch(像素偏移)` 做邻域（偏移是**视图像素**）：**stencil 决定"哪些像素属于角色"**，**CustomDepth 的邻域深度差才是边的来源**（`abs(cd - cdC) > DepthThreshold`，外轮廓与部件交界都出线），再用 `PPI_SceneDepth` 挡穿墙（`cd <= sceneDepth + DepthThreshold`）。角色侧 `render_custom_depth=true` + `custom_depth_stencil_value=N`；材质挂专用 unbound volume，用 **`add_blendable_to_post_volume`**（直接写 `settings.blendables` 被读保护）。**绑定完要显式 bounce 一次 volume 的 `enabled`（False→True）再存关卡**。`BlendableLocation` 抄工程里**已能工作**的那个描边材质（本例 `BL_SCENE_COLOR_AFTER_TONEMAPPING`），不要照文档默认值猜。

**打 stencil 前先普查"谁消费 stencil"**：任何以 `PPI_CustomStencil` 为条件的材质都会从"没效果"变成"生效"（本例 `M_Outline_RT` 是火焰描边，给角色打 tag 1 就顺带套上橙白火圈）。普查 = 遍历 `/Game` 下 `material_domain == MD_POST_PROCESS` 的材质，看 `SceneTexture` 节点 id 是否含 25、Custom 代码里有没有 `stencil`；**同时要记录"哪些真的挂在 volume 上"**（没挂的资产不参与渲染，谈冲突没有意义）。两个坑：扫描里**别用 `except: continue`** —— 它会把所有目标静默跳过，输出为空看起来就像"没有载体"（本仓库真踩过）；`WeightedBlendables` **不可迭代**，要取 `.get_editor_property("array")`。

**每台以 stencil 为条件的描边 SHOULD 精确拥有一个取值**：判据写 `abs(stencil - StencilValue) < 0.25`，材质暴露自己的 `StencilValue`。分配就是一个约定，例如 **tag 1 = 火焰描边、tag 2 = 普通描边**；一个 primitive 只有一个 stencil 值，所以"某物体同时吃两套"只能靠**把两个 `StencilValue` 设成同值**（精确匹配同值 = 并集），不能靠两个 tag。

反面写法是**累积**：`stencil >= StencilValue - 0.25`（写死的 `> 0.5` 就是它默认 1 时的等价形式）。只要有一台是累积的，它就会盖住所有更高 tag 的物体 —— 两台都精确才真正分得开。改老材质做这件事时，把判据**逐处**替换（本例 3 处）再补一个参数引脚。

改老材质做这件事时：门限表达式要**逐处**替换（本例 3 处），**给已有 Custom 节点加引脚走 python** —— `unreal.load_object(None, "<pkg>.<asset>:MaterialExpressionCustom_0")` 拿到对象后 `set_editor_property("inputs", pins)`；改前把 code + 引脚表备份成 JSON。**老材质若有构建脚本，必须同步改脚本**，否则下次重建静默退回旧行为（工程里 `build_outline_rt2.py` + 若干 patch 脚本是既有惯例，新 patch 要落进这条链）。

**顺序别记错**：材质的 `BlendablePriority` 和 volume 的 `priority` 是两件事。同 location 的材质链按**材质的** `BlendablePriority` 升序套用（`BlendableInterface.h:138-150` 的 `FCompare` + `PostProcessMaterial.cpp:1048` 的 `StableSort`），优先级相同时才保持卷的合并顺序；volume 的 `priority` 只在合并各 volume 的**设置**时起作用。要固定谁在上层，就显式给材质的 `BlendablePriority` 赋值。

### 5. 落盘与自证

脚本落 `Saved/MCPScripts/`（改材质 / 建描边 / 挂 volume 与组件 / 机器验收 / 批次清单各一份）。每个材质回读：节点数、`unreferenced_count`、blend、双面、贴图、`EmissiveColor` 与 `OpacityMask`/`Opacity` 连接、`errors_by_feature_level`（**SM5 与 SM6 都要空**）、`.uasset` mtime。两个实操注意：

- 用 `execute_python_file` 跑十几二十个材质的批量改造会**超客户端 90s 上限而在客户端报 timeout，但服务端还在跑** —— 以脚本自己写出的 report JSON 为准，不要重跑。
- 读某个节点采的是哪张贴图：`get_material_expression_property(property="texture")`（`list_material_expressions` 不列贴图）。

### 6. 反向外扩描边（inverted hull）—— 二次元角色描边的原生做法

要"MMD/原神那种描边"就走**几何外扩**，不要走后处理：后处理描边的线宽是像素（随距离/分辨率变）、深度连续处出不了线、贴地与凹处易断，而且做不到"每材质不同宽度"。判据：MMD 的 PMX 里每个材质带 `EdgeColor` / `EdgeSize` / 是否有 edge 标志 —— 那就是外扩的定义（解 PMX 就能拿到原始线宽与颜色）。

**UE 里怎么"只画背面"（不猜，实测）**：

| 问题 | 结论 | 依据 |
|---|---|---|
| 静态网格能反向剔除吗 | 能：`bReverseCulling` / `SetReverseCulling()` | `StaticMeshComponent.h:313,475` |
| 骨架网格能吗 | **不能**：没有该标志，反向剔除只能由"变换行列式为负"推出 | `SkeletalMesh.cpp:6940,7202` |
| 那用负缩放行不行 | 不行：行列式为负会连法线一起镜像，外推出的壳是**镜像的壳**，不对称姿势会画到错的一侧 | 同上 |
| 材质层能拿到正/背面吗 | **能**：`MaterialExpressionTwoSidedSign`（=`Parameters.TwoSidedSign`，正面 +1 / 背面 −1） | `MaterialExpressionTwoSidedSign.h`、`MaterialTemplate.ush:4444-4462` |

所以配方是：`MSM_UNLIT` + `BLEND_MASKED` + **`TwoSided=True`**（谁都不剔除）+ `WorldPositionOffset = VertexNormalWS × Width`（宽度用**世界单位**）+ `OpacityMask = Saturate(OneMinus(TwoSidedSign))`（只留背面 ⇒ 正面被 Masked 剪掉，不会盖住角色）。外推后的背面只在轮廓**之外**露出 → 得到一圈线，**不需要 stencil / CustomDepth / 后处理 volume**。

**载体：用独立 actor（或蓝图 actor），别给已有 actor 硬加组件。** 独立 actor 用 `SkeletalMeshActor` 这类**类里自带**组件的载具：同一 mesh、材质槽全指壳材质、`LeaderPoseComponent` 指向角色主组件（跨 actor 共享姿势）、`cast_shadow=false`、attached 到角色；做成蓝图 actor 亦可（本项目受控角色就是把描边做成蓝图里的一个组件，见重定向 skill §三.1）。

已知取舍：法线在硬边处分离 ⇒ 那些地方壳会裂（MMD 同样如此）；单一共享的壳材质会让"原本没有 edge"的槽也被描边，要对齐就得逐槽材质。

**描边色不要"贴图 × 一个固定深色"，也不要全局一个颜色**：业界（MToon / Unity Toon Shader / Guilty Gear Xrd）的做法是**描边色取材质自己的暗部色与基色插值**（`lerp(ShadeColor, BaseColor, k)`；GG 更进一步用 lit/shadow 两张手绘 albedo，描边直接取 shadow 那张）。关键是线条要**比物体在任何阶调下都暗**：本体画的是 `tex * lerp(ShadowTint, 1, ramp)`，暗面已经到 `tex*0.35`，所以 `tex*0.55` 这种"看起来只是稍深"的线在暗面**比物体更亮**（用户会直接说"像浅色"）。稳妥做法：

- 让描边材质**跑本体那份着色核心**（同一个 Custom 代码 × 一个 `Darken`），线条就成了"物体当前被画出来的颜色再压暗"，逐像素跟随法线与光照；`spec`/`rim` 用常量 0 接进去（那样描边不会长出高光）。
- **代码从本体材质读、不要重抄**：`get_material_expression_property(property="code")` + `property="inputs"` 拿引脚表，建成壳材质的 Custom 节点；验收里断言两边 `code` **逐字节相同**，本体改公式时描边不会悄悄漂移。
- **压暗要用"带色相的暗色"，不能用标量/乘黑**：标量乘等于把白色部件变成**灰线**（会被直接指出来："脸是白的，描边不能是灰色，要是棕色"）。做成**颜色参数**（默认暖棕，与源模型 PMX `EdgeColor` 同族），并把它写成机器判据（`LineTint.r > LineTint.b`）。这也是 ASW 的原话——"基色的**更暗、更饱和**的色调"。
- **线条色逐槽取自该槽自己的颜色**：保持该槽色相、压暗并（相对）提饱和 —— 做法是采该材质 UV 框内的贴图均色，`tint = (mean / max(mean)) * 幅度`（幅度 ~0.45），蓝发就得到深蓝线。**近中性的槽没有色相可保**（纯白/近灰），乘下去只会变灰，所以饱和度低于阈值（~0.12）时回落到暖棕 —— 这一条正是用户两轮反馈的合并："脸是白的，描边不能是灰色，要是棕色" + "头发蓝色应该描边深蓝"。压暗必须是**颜色相乘**，不是标量。
- **逐槽一个材质实例**（`MI_OL_<槽>`）：壳材质的贴图、暗部色、阶数、线条色都做成参数，实例的值**按索引**从本体组件的同一索引槽读出（两者挂同一 mesh ⇒ 索引即同一 section，别按名字查表）。槽序就是正确性。粒度是"每材质一个线条色"（GG 的 per-material line colour 同粒度）：槽内多色时取该槽均色的暗色，不逐像素跟随色相。
- 未做的两项业界常见件（知道缺什么）：屏幕空间恒定像素宽度（当前世界单位宽度会随距离变细）、深度淡出（接触地面处硬切）。

**改这些实例时：就地改参数，绝不要 recreate。** 已被挂载渲染的实例上用 `create_asset_safe(recreate=True)` 是"删了再建同名声"，渲染侧持有悬空数据 ⇒ **编辑器直接崩**（纯 Renderer 栈）。已存在就 `load_asset` 后改参数，不存在才创建。另：World Partition 关卡里 actor 的改动在 **external actor 包**，`save_current_level()` 不保证落盘，用 `EditorLoadingAndSavingUtils.save_dirty_packages(True, True)`。

> **排查手法（省时间的关键）**：效果"看不到"时不要靠读属性猜，**把一个变量挪到已知能渲染的通道上**再判。本例就是把描边材质**临时挂到角色自己的材质槽（0 号槽）**：脸立刻被外推成红色块 ⇒ 材质/WPO/使用标志全部无罪，问题锁定在载体那个子对象上。一次观感就把"材质 / 组件 / 渲染状态"三类原因切开了。

> 工具/编辑器侧实测原文（崩栈、`save_dirty_packages` 的必要性、旧实例的陈旧引用为何删不掉）：`Docs/MCP_Findings_2026-09-27_outline-mi-recreate-crash.md`。
