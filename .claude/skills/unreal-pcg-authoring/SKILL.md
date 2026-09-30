---
name: unreal-pcg-authoring
description: "UE5（UnrealMCP）里读写 PCG（Procedural Content Generation）图与生成产物的方法：命令层（16 个工具）的用法 —— 看图（list_pcg_assets / get_pcg_graph / get_pcg_node）、看组件与产物（list_pcg_components / get_pcg_generated_output）、建图（create_pcg_graph）、改图（add_pcg_node / connect_pcg_pins / disconnect_pcg_pins / remove_pcg_node / set_pcg_node_property）、配网格生成器（set_pcg_mesh_selector_type / set_pcg_mesh_selector_entries）、装配到关卡（set_pcg_component_graph）、触发生成与清理（generate_pcg_component / cleanup_pcg_component）；引脚标签与 python 属性名两套写法的对应关系、instanced 只读子对象与网格选择器专门入口的区别、整数权重的坑、写后回读纪律、生成与读取必须拆步、逐段 TAP 二分定位丢点、以及批量脚本走 bridge 回环的做法。触发场景：准备调用以下任一 MCP 工具前 MUST 加载本 Skill —— list_pcg_assets / get_pcg_graph / get_pcg_node / list_pcg_components / get_pcg_generated_output / create_pcg_graph / add_pcg_node / connect_pcg_pins / disconnect_pcg_pins / remove_pcg_node / set_pcg_node_property / set_pcg_mesh_selector_type / set_pcg_mesh_selector_entries / set_pcg_component_graph / generate_pcg_component / cleanup_pcg_component；以及任何涉及「查看/核对 PCG 图结构、PCG 节点属性名到底是什么、引脚标签该写哪个、从零建 PCG 图、改 PCG 图（加删节点、连断引脚、写节点属性）、给网格生成器配树种/权重、把 PCG 图挂到关卡组件上、PCG 生成结果出了多少点/属性分布如何、PCG 组件绑定的是哪张图、触发或清理 PCG 生成」的任务。材质/粒子/蓝图/动画各自走对应 Skill。"
metadata:
  version: "1.0.0"
  upstream: unreal-blueprint-authoring
  downstream: ~
---

# UnrealMCP PCG 编写 Skill

适用：UE 5.5 + 本仓库 `Plugins/UnrealMCP`（bridge `127.0.0.1:55557`，MCP server 名 `unrealMCP`）。PCG 插件（`D:\UE_5.5\Engine\Plugins\PCG`，beta）已在项目启用，本插件对它有**硬依赖**。

**环境注意**：改 `Plugins/UnrealMCP/Content/Python/**`（含工具描述）需**重启 unrealMCP server**；改插件 C++ 需编译 + 重启编辑器。禁用了 PCG 插件时本插件无法加载（硬依赖）。

---

## 〇、能力边界：读与写都有命令层

| 用途 | 路径 |
|---|---|
| 看图结构 / 查属性真名 / 读生成产物 / 改图 / 写属性 / 触发与清理生成 | **MCP 工具（12 个，命令名 = 工具名）** —— 本节的主要接口 |
| 批量脚本编排（一次调用里走多步） | python（`unreal.*` + `unreal.UnrealMCPPythonAPI.execute_mcp_command` 回环调同名命令） |

写路径已经有命令层，**不要再裸调 `PCGGraph.add_node_of_type` 那套**：命令层做了事务、写后回读、成功即保存、结构化错误码四件事，裸调一件都没有。

```python
import json, unreal

def bridge(cmd, **params):
    return json.loads(unreal.UnrealMCPPythonAPI.execute_mcp_command(cmd, json.dumps(params)))

g = bridge("get_pcg_graph", asset_path="/Game/MCP/_PCGProbe/PCG_Trees", detail="summary")["result"]
print(g["node_count"], [n["name"] for n in g["nodes"]])
print(g["edges"][0])   # {from_node, from_label, to_node, to_label}
```

脚本写到 `Saved/MCPScripts/*.py` → `execute_python_file(file_path=..., deferred=True)`。

---

## 一、十二个工具

只读（5 个）：

| 工具 | 主要参数 | 关键返回 |
|---|---|---|
| `list_pcg_assets` | `folder`（默认 `/Game`）、`class_filter`（`PCGGraph`/`PCGGraphInstance`/`PCGDataAsset`）、`recursive` | `assets[]{asset_path, name, class, package_path}`、`count` |
| `get_pcg_graph` | `asset_path`、`detail`（`summary`/`full`）、`max_nodes` | `nodes[]{name, settings_class, editor_x, editor_y, input_pins, output_pins}`、`edges[]`、`input_node`、`output_node`、`node_count`、`truncated` |
| `get_pcg_node` | `asset_path`、`node_name`（或 `node_index`）、`max_properties` | `properties[]{property_name, python_name, type, value_text, value, value_shape, editable, edit_const, transient, instanced, writable, hint}`、`writable_count`、`readonly_instanced_count` |
| `list_pcg_components` | `actor_label`（子串） | `components[]{actor_label, component_name, graph_path, graph_instance_path, generation_trigger, active, generating, generated, bounds_min/max, managed_resource_counts}` |
| `get_pcg_generated_output` | `actor_label` 或 `component_name`、`attribute`、`max_samples`（1..64）、`include_attributes` | `tagged_data[]{tags, data_type, pin, is_point_data, point_count}`、`total_points`、`attributes[]`、`attribute_stats` |

写（11 个，结构/属性写带 `saved` 回读）：

| 工具 | 主要参数 | 关键返回 |
|---|---|---|
| `add_pcg_node` | `asset_path`、`node_class`（短名或路径）、`editor_x`/`editor_y` | `node_name`（引擎命名，后续全靠它寻址）、`settings_class`、`in_graph`、`input_pins`/`output_pins`、`node_count` |
| `connect_pcg_pins` | `asset_path`、`from_node`/`from_label`、`to_node`/`to_label` | `connected`、`edge{}`、`edges_before`/`edges_after` |
| `disconnect_pcg_pins` | 同上五参 | `removed`、`edges_before`/`edges_after` |
| `remove_pcg_node` | `asset_path`、`node_name` | `removed_name`、`node_count` |
| `set_pcg_node_property` | `asset_path`、`property_name`（反射名或 snake_case）、`value`、`node_name`（或 `node_index`） | `value_before`、`value_after`、`value_shape`、`changed`、`verified`、`notified` |
| `set_pcg_mesh_selector_type` | `asset_path`、`node_name`、`selector_class`（`weighted`/`by_attribute`/`weighted_by_category`/类名） | `selector_class`、`selector_parameters_path`、`selector_instantiated`、`notified` |
| `set_pcg_mesh_selector_entries` | `asset_path`、`node_name`、`entries`（`[{static_mesh, weight(int), override_materials?}]`，**整表替换**） | `entry_count`、`entries`（含 `display_name`）、`selector_parameters_path`、`notified` |
| `create_pcg_graph` | `asset_path`（含名字的完整路径）、`folder` | `node_count`、`input_node`、`output_node`、`saved`（**落盘**，不覆盖已存在资产） |
| `set_pcg_component_graph` | `graph_path`、`actor_label`/`component_name`、`active` | `graph_interface_path`（组件持有的接口）、`graph_path`（有效图）、`level_package_dirty`（**不保存关卡**） |
| `generate_pcg_component` | `actor_label` 或 `component_name`、`force` | `dispatched`、`generating`、`active`、`generated_output_available` |
| `cleanup_pcg_component` | 同上 + `remove_components`、`save_generated_components` | `cleanup_dispatched`、`remove_components`、`generating`、`generated_output_available`（**无 `saved`**：清理不碰图资产） |

**从零到在视口里看见东西的标准顺序**（每一步都能回读自证）：

```
create_pcg_graph        → 拿到 output_node 名字（新图是空的，必须自己接）
add_pcg_node ×N         → 逐个拿 node_name（点生成 + 网格生成器…）
connect_pcg_pins        → 接成链路，最后必须接到 output_node（不接 = 生成成功但结果为空）
set_pcg_node_property   → 写参数（看 type/`writable`，整型别写浮点；结构体属性可以只传要改的字段）
set_pcg_mesh_selector_entries → 给网格生成器配"用哪些网格、权重多少"（**不配 = 只有点云，视口里看不见东西**）
set_pcg_component_graph → 把图挂到关卡里既有的 PCG 组件（active=true，否则生成不落地）
generate_pcg_component  → 只下发
list_pcg_components     → 判 generating 结束
get_pcg_generated_output→ 读点数/属性统计；list_pcg_components 的 instance_batches 读实例批次
```

**沿样条布局（篱笆/栏杆/围栏一类）**：`DefaultInputNode → SplineSampler → StaticMeshSpawner → DefaultOutputNode`，PCG 的输入数据来自**所属 actor**（含样条组件），所以样条必须和 PCG 组件在同一个 actor 上。

- 采样点自带样条朝向（采样 transform 取自 `LineData->GetTransformAtDistance`），**不用自己算旋转**。
- **刚性单元在曲线上会裂开**：接缝张开 ≈ `R·(1-cos(s/R))`（s=间距=单元长，R=曲率半径），与 s 呈**平方**关系。所以"线条形状"的件必须是**短单元**（或按弦拉伸，见下）。
- **柱/连接件与面板分开采样**：`SplineSampler(200cm)→柱生成器` + `SplineSampler(16cm)→面板生成器`，两路用 **Merge 节点**汇到输出节点；面板单元长度 = 面板采样间距，`b_fit_to_curve` 关掉（它会把间距向上取整，裂缝又回来）。
- **单元长度必须等于采样间距**，否则叠段/漏缝；网格约定：原点在件**底部**、**+X 为走向**、连接件只放在件首（两端都放会重合闪烁）。
- **输出节点输入引脚可以接多条边**：加链路后记得 `disconnect_pcg_pins` 断掉多余的直连，否则产物被重复计入。
- 想做到"零缝"可考虑 `PCGSplineToSegmentSettings`（点=弦中点、extents=半弦）+ 按 extents 拉伸的实例打包器。
- 验证实例朝向/间距时要取**世界空间** transform：`get_instance_transform(i, bWorldSpace=False)` 是组件局部坐标，拿去做世界空间样条查询会得到假数据。
- **在编辑器里挂组件别用 `unreal.new_object(...)`**：那样只是普通子对象，不进编辑器组件树、不参与渲染，只适合纯逻辑/临时对象。两条正路：走 `unreal.UnrealMCPComponentLibrary.add_instance_component(actor, comp)` + `register_component(comp)`（前者把 `CreationMethod` 设成 `Instance`，是编辑器组件树列它的那道门；后者完成注册，渲染/tick 需要，查询用 `is_component_registered`），或用引擎 Add Component 接口 / 做成蓝图 actor。

错误码：`asset_not_found`、`not_a_pcg_graph`、`not_a_pcg_graph_interface`、`asset_exists`（建图不覆盖）、`invalid_asset_path`、`create_failed`、`folder_not_found`、`unknown_class_filter`、`node_not_found`（带 `candidates`）、`node_has_no_settings`、`unknown_node_class`（带候选类名）、`node_create_failed`、`pin_not_found`（带候选标签）、`pin_not_connected`（带两侧候选标签）、`edge_not_found`（带该引脚现有的边）、`cannot_remove_graph_io_nodes`、`remove_failed`、`unknown_property`（带候选）、`property_not_writable`（带 `hint`）、`readback_mismatch`（带 `requested_value`/`readback_value`/`reverted`）、`not_a_static_mesh_spawner`、`unknown_selector_class`（带候选类名）、`selector_not_instantiated`（防御性：新节点通常已有实例）、`unsupported_selector`、`invalid_entry`、`invalid_weight`（浮点权重被拒）、`unknown_mesh`、`unknown_material`、`component_not_found`（带 `candidates`）、`not_generated`、`invalid_attribute`（带 `candidates`）、`no_editor_world`、`runtime_generation_component`。

**只读保证**（前 5 条）：不建资产、不改关卡、不保存、不触发生成。组件没生成过就返回 `not_generated`，**不会替你生成**。

**写纪律**（后 7 条）：改的是图资产本体，成功即保存；每条都回读自证，回读对不上就报 `readback_mismatch` 并且**不保存**；生成/清理只下发、不等待。

---

## 二、两套命名：这是 PCG 最容易踩的地方

同一个引脚/属性有两种写法，**用错就连不上或写不进**：

| 场景 | 用哪个 | 例子 |
|---|---|---|
| 图的连线操作（python 侧的连线/删线 API） | 引脚的**显示标签**（label） | `Out`、`In`、`Surface`、`Overrides`、`LowerBound`、`bInvertFilter` |
| 节点属性的读写（python 侧的属性读写） | **snake_case**（python_name） | `lower_bound`、`scale_min`、`rotation_max`、`density_mode` |

`get_pcg_graph` 的每个引脚同时给 `label` 与 `python_name`；`get_pcg_node` 的每个属性同时给 `property_name`（反射名）与 `python_name`。**不要凭记忆写**：属性名猜错会直接抛异常，引脚标签写错会连不上，所以连线一律以回读判定。

图输入/输出节点的固定命名：输出节点的输入引脚叫 `Out`，输入节点的输出引脚叫 `In`。**图输出节点没接 = 生成成功但结果为空**。

---

## 三、不要猜属性名：先 `get_pcg_node`

PCG 的 settings 属性名无法凭经验推断（例如变换节点上有 `scale_min`，却没有 `b_uniform_scale`）。流程固定为：

1. `get_pcg_graph` 拿到节点 `name`；
2. `get_pcg_node` 拿属性表，按 `property_name` / `python_name` 找到目标属性；
3. 看该条目的 `type` 与 `value_shape` 决定值的形态（整型 / 浮点 / 向量 / 枚举 / 数组），`value` 是结构化值，`enum_member` 给出枚举当前成员名，`value_text` 只在值可文本化时出现（复杂类型不伪造文本）；
   - 取值覆盖全形状：数值/布尔/字符串/枚举照常；**对象/类/软引用给路径文本**（可直接拿去用 unreal 的加载接口取到对象）；数组/集合/映射给数组；数学结构体（`Vector`/`Rotator`/`LinearColor`/`Quat`…）给**具名分量**（`x/y/z`、`pitch/yaw/roll`、`r/g/b/a`）；
   - `value_shape` 为 `omitted` 只在反射器没有该类型的 JSON 形状时出现（罕见）—— 不是"故意不读"，用 `type` 与 flags 兜底；
4. `writable` 为 true 才走属性写入；false 看 `hint`：
   - `instanced` 子对象（如静态网格生成器的网格选择器）**属性本身不可赋值**，只能读；要改它的内容，得先让引擎建立实例（引擎侧的 selector 类型设置函数），再往实例上写条目；
   - `edit_const` = 引擎托管，改不了。

**整型 vs 浮点必须看清**：整型属性必须写整数，写完回读一次值。

---

## 四、生成是异步的：触发与读取必须拆步

- 生成是异步的：触发生成（python）与读取结果（工具）**拆成两次调用**；
- 同一个脚本里**禁止 sleep 轮询**（会卡死 GameThread）；要等就在工具层多调一次；
- 判断"生成了没有"用 `list_pcg_components` 的 `generated` 与 `managed_resource_counts`。

只读侧还有一条：`get_pcg_generated_output` 只读**已有结果**、不自动触发生成。

---

## 五、诊断顺序（出了问题按这个走）

1. **结构**：`get_pcg_graph` 看边是否真的存在（`connected` / `edges_to`），别只看调用没报错；
2. **属性**：`get_pcg_node` 看关键属性是否真的写进去了（值是否被截断/换成了别的成员）；
3. **产物**：`get_pcg_generated_output` 不带 `attribute` 先看 `total_points` 与 `tagged_data`；
4. **数值**：带 `attribute` 看 `attribute_stats`（`min`/`max`/`mean`/`channels`/`samples`）——比任何截图都可靠；
5. **二分定位丢点**：把图输出节点依次接到链路中段节点之后、重新生成、再看点数，找出是哪一段开始丢点。

---

## 六、写路径纪律（命令层；与另一套 PCG skill 交叉验证过）

写命令自带事务、写后回读、成功即保存、结构化错误码；以下是使用约束：

1. **改图第一条：拿到的名字就是钥匙**。`add_pcg_node` 返回引擎命名的 `node_name`（形如 `TransformPoints_3`），后续 `connect_pcg_pins` / `remove_pcg_node` / `set_pcg_node_property` 全靠它寻址；不要自己拼名字。
2. **连线靠回读判定，不靠调用是否报错**：`connect_pcg_pins` 标签写错会报 `pin_not_connected` 并给出两侧候选标签 —— 拿到这个错误就照候选改标签，别反复重试同一个拼写。单输入引脚再连一次是**替换**，看 `edges_before`/`edges_after` 确认。
3. **删节点不重连**：`remove_pcg_node` 不自动接线（图输入/输出节点不可删，报 `cannot_remove_graph_io_nodes`）；要保留链路就自己 `connect_pcg_pins` 补回来。
4. **写属性看类型，别猜**：`get_pcg_node` 的 `type` 决定值的形态，`writable: false` 的属性一律别试（`property_not_writable` 会回同一个 `hint`）。**权重属性是整数**（`PCGMeshSelectorWeightedEntry.weight` 是 `int32`）：写浮点会被截断、命令以 `readback_mismatch` 报出来，改成整数权重即可。
5. **写完属性必须让引擎知道**：命令已经代做（响应 `notified: true`）—— 这条通知是 PCG 编译缓存失效的关键；若重新生成的点数/分布没变，先看 `notified` 与生成是否真的跑完（`generating`）。
6. **instanced 子对象分两种情况**：网格选择器有专门入口（`set_pcg_mesh_selector_type` 换类 + `set_pcg_mesh_selector_entries` 写表），**新节点已经自带默认加权选择器实例**（`MeshSelectorParameters` 指向 `...DefaultSelectorInstance`），所以配网格不必先调类型命令；其它 instanced 属性（子图引用、属性选择器、instance data packer）仍报 `property_not_writable` + `hint`。网格条目里 **`weight` 必须是整数**（引擎字段是 `int32`），写浮点会被 `invalid_weight` 拒绝；`static_mesh` 必须是能解析到的 `UStaticMesh`，材质放 `override_materials`。
7. **`static_mesh` 只吃网格对象**：传软路径会 `TypeError`；材质走 descriptor 的 `override_materials`，不要塞进 `static_mesh`。
8. **空关卡别用 Surface Sampler**：它必须有几何可采样（空关卡恒 0 点）；用体积采样器并把 `unbounded` 打开发散采样。
9. **别为了验证去生成测试 actor**：验证靠 `list_pcg_components` 的实例批次与 `get_pcg_generated_output` 的点数/属性统计。
10. **图内引用型属性名反直觉**：子图引用是 `subgraph_override`（不是 `subgraph`）；运行时 settings 名可能与编辑期类名不同（点过滤器在运行时叫属性过滤）—— 拿不准就先 `get_pcg_node` 看反射名。
11. **结构写之前先关该资产的编辑器**：用既有 `close_asset_editors`，避免 UI 与内存里的图不同步。

> 引擎事实：`PCGEdge` 的端点对 python 不可见（只能数边数）；本仓库用 `get_pcg_graph` 的 `edges[]{from_node, from_label, to_node, to_label}` 读边。

## 七、验收纪律

- 命令层的返回值就是验收凭据：节点数/边数、属性值（`value_before`/`value_after`）、点数、属性统计都能直接断言；
- **写命令的成功要按"回读一致"理解**：`connected: true`、`verified: true` + `value_after` 才算写完；只看 `success: true` 不够（回读对不上时命令本来就报错）；
- **效果验收仍由用户在编辑器视口完成**，不要用截图或像素分析代替（对齐 `rules.mdc`）；本工具集不产出图像；
- 生成会在关卡里创建/维护资源（含引擎自动建立的 PCG 世界 actor），测试后要清理并复查脏包集合，别把演示残留在用户关卡里。
