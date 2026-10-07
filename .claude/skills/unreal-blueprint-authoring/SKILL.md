---
name: unreal-blueprint-authoring
description: "UE5（UnrealMCP）里用 MCP 工具 + python 脚本程序化建/改蓝图的方法：图与节点的寻址（graph_name / node guid）、建节点→写引脚→连线→回读的固定顺序、常量与变量节点、组件层级与 socket 挂接、节点自身属性写入（pins_rebuilt）、结构化错误码与候选字段、批量脚本里走 bridge 回环或反射库的方式、逐步验证纪律。触发场景：准备调用以下任一 MCP 工具前 MUST 加载本 Skill —— find_blueprint_nodes / verify_blueprint_graph / list_blueprint_graphs / add_blueprint_event_node / add_blueprint_function_node / add_blueprint_input_action_node / add_blueprint_variable / add_blueprint_variable_node / add_blueprint_literal_node / add_blueprint_node_by_class / add_blueprint_self_reference / add_blueprint_get_self_component_reference / connect_blueprint_nodes / set_blueprint_pin_default / set_blueprint_node_property / disconnect_blueprint_pins / delete_blueprint_nodes / compile_blueprint / create_blueprint / add_component_to_blueprint / set_component_property / set_static_mesh_properties / set_physics_properties / set_blueprint_property / attach_component_to_component；以及任何涉及「用 MCP 改/建 UE 蓝图、蓝图图节点连线、EventGraph/函数图/MacroGraph 寻址、K2 节点、Cast/Switch/Comment 节点、变量 Get/Set 节点、局部变量、组件层级与 socket、payload pin 默认值、unreal.UnrealMCPBlueprintGraphLibrary 反射库、蓝图验收脚本」的任务。"
metadata:
  version: "1.0.0"
  upstream: ~
  downstream: ~
---

# UnrealMCP 蓝图编辑 Skill

适用：本仓库 `Plugins/UnrealMCP`（bridge `127.0.0.1:55557`，MCP server 名 `unrealMCP`）。
目标：用 MCP 工具 + python 脚本**可重复、可验证**地读/建/改蓝图图与组件层级。

**环境注意**：改了 `Plugins/UnrealMCP/Content/Python/**`（含工具描述）后需**重启 unrealMCP server**；改插件 C++ 则需编译 + 重启编辑器（`Build_UnrealMCP.bat` → `Editor.bat start`）。

---

## 〇、蓝图编辑操作规范

### 0. 两条可用路径（同一内核）

| 用途 | MCP 工具路径（JSON 进 / JSON 出） | python 反射路径（编辑器脚本内） |
|---|---|---|
| 列图 | `list_blueprint_graphs(blueprint_name)` → `graphs[]`（`graph_name` / `graph_class` / `node_count` / `is_editable`） | `lib.get_graphs(bp).graphs` |
| 列节点 + 连接 | `find_blueprint_nodes(blueprint_name, node_type="All", graph_name?)` → 每节点 `node_id/name/type/properties/pins/connections` | `lib.get_graph_nodes(graph).nodes` / `lib.get_node_info(node)` / `lib.get_node_pins(node)` |
| 建节点 | `add_blueprint_function_node` / `add_blueprint_event_node` / `add_blueprint_input_action_node` / `add_blueprint_literal_node` / `add_blueprint_node_by_class` / `add_blueprint_self_reference` / `add_blueprint_get_self_component_reference` / `add_blueprint_variable_node` | `lib.add_function_call_node` / `add_event_node` / `add_input_action_node` / `add_literal_node` / `add_node_by_class` / `add_self_reference_node` / `add_variable_get_node` / `add_variable_set_node` |
| 写引脚默认值 | `set_blueprint_pin_default(blueprint_name, node_id, pin_name, value, graph_name?)` | `lib.set_pin_default(node, pin_name, value, value_kind)` |
| 写节点自身属性 | `set_blueprint_node_property(blueprint_name, node_id, property_name, property_value, graph_name?)` | `lib.set_node_property(node, property_name, value, value_kind)` |
| 连线 / 断线 | `connect_blueprint_nodes(...)` / `disconnect_blueprint_pins(...)` | `lib.connect_pins(...)` / `lib.disconnect_pin(...)` |
| 删节点 | `delete_blueprint_nodes(node_ids=[...])` | `lib.delete_node(node)` |
| 变量声明 | `add_blueprint_variable(blueprint_name, variable_name, variable_type, sub_class?, default_value?, is_exposed?)` | 无（变量面仅有节点级 API） |
| 编译 / 事务 | `compile_blueprint(blueprint_name)` | `lib.compile_blueprint_checked(bp)` / `begin_transaction(desc)` / `end_transaction()` |
| 组件 | `add_component_to_blueprint` / `set_component_property` / `set_static_mesh_properties` / `set_physics_properties` / `attach_component_to_component` | 无（组件面走命令） |

两条路径操作**同一份内存图**，可混用。python 侧看不到图内结构（`UEdGraph::Nodes` 是 protected、`UEdGraphNode::Pins` 根本没有反射属性、`AllocateDefaultPins`/`PostPlacedNewNode`/`CreateNewGuid` 都是普通 C++ 成员），所以反射库是唯一入口；它与 MCP 命令共用内核 `FUnrealMCPBlueprintGraphOps`，**图状态与错误码逐字一致**，可用于交叉验证。

### 0b. 建蓝图资产：`create_blueprint` 与继承 C++ 类

- `create_blueprint(name, parent_class)` 只落在 `/Game/Blueprints/<name>`（要别的目录再 `move_asset`）。`parent_class` 收：全路径（`/Script/Engine.Character`、`/Script/<Module>.<Class>`、`/Game/Dir/BP_X.BP_X_C`）、真实类名（`Character` / `AnimNotify` / `AnimInstance`）、或旧短名（`Pawn` → `APawn`）。解析不到是**硬错误**（`parent_class_not_found` + `tried`），**不会**回退成 Actor、也不留半成品。
- 工程/插件自己的 C++ 类同样能当父类：只要它编译进一个**已加载**模块且是 `UCLASS(Blueprintable)` ⇒ `parent_class="/Script/<Module>.<Class>"`，响应的 `parent_class` / `parent_class_path` 是服务端解析后的真类（可用它断言）。改了 C++ 要重编工程模块并重启编辑器（本仓库：`Plugins/UnrealMCP/Build_Project.bat`；只改插件则 `Build_UnrealMCP.bat`）。
- **继承是否生效必须自证**，别只看 `create_blueprint` 回 success：① 调它**继承来的** `UFUNCTION(BlueprintCallable)` 建节点（`add_blueprint_function_node(target="BP_Child_C", function_name="Ping")`）—— 能建出且 `self_context: true` 才算继承到了（self 引脚的 `category` 会显示父类名，如 `object/MCPTestActor`）；② 调它继承来的 `UPROPERTY` 建变量节点（`add_blueprint_variable_node(variable_name="TestValue", node_kind="get")`）；③ **负向对照**：故意写错函数名必须回 `function_not_found`（证明解析真在父类链上查，不是"什么都能建"）；④ `compile_blueprint` 断言 `compiled: true` 且 `errors: []`。

### 1. 寻址：先读，再写

```
list_blueprint_graphs(bp)                         → 图清单（只读，不建图）
find_blueprint_nodes(bp, graph_name="FooFunc")    → 节点 + pins + connections（只读，不建图）
```

- `graph_name` 省略时回落**唯一名字含 `EventGraph` 的图**；有多张事件图时报 `graph_not_found` + `available_graphs`（不是猜）。写命令默认 `bCreateEventGraphIfMissing=true`（真建图），**读命令永远不建图**。
- `node_id` = `FGuid::ToString()`（写、不去短横线）。**只会从读命令/写命令的返回值里拿**，别用 `load_object("<graph>.K2Node_CallFunction_0")` 猜名字。
- guid 是节点实例身份：删节点、重建节点（见 §5 的 `pins_rebuilt`）都会换新 guid → 写前现读，一条脚本内读完即用。
- 图分三类来源：`UbergraphPages`（事件图，通常 1 张）→ `FunctionGraphs`（每个图表函数一张，`UserConstructionScript` 也在内）→ `MacroGraphs`（宏图 `is_editable=false`，写入报 `unsupported_graph_kind`）。委托签名图与中间生成图不在清单里；折叠图是图内子图，也不是顶层页。

### 2. 建图固定顺序

```
建节点 → 写引脚默认值 / 写节点属性 → 连线 → 回读 → 编译
```

- **节点创建顺序是硬约束**（内核已封装）：`NewObject → Configure → 位置 → CreateNewGuid → PostPlacedNewNode → AllocateDefaultPins → AddNode → ReconstructNode → 标记修改`。`Configure` 在引脚存在之前执行，所以函数/事件/变量引用必须在建节点时一次给对。
- `add_blueprint_node_by_class` 出来的是**骨架节点**：Cast 没有目标枚举、Switch 没有枚举、Sequence 没有引脚数配置。必须再用 `set_blueprint_node_property` 写 `Enum`（会触发 `pins_rebuilt`）补全。
- 每个写命令 = **一个撤销步骤** + **成功即落盘**。失败会 `Cancel` 事务，不留半成品。
- 只需往"已存在的引脚"塞常量时用 `set_blueprint_pin_default`，**不要**为它建 Literal 节点。
- **引脚的 `value` 取值形式**（`set_blueprint_pin_default` 与 `add_blueprint_function_node` 的 `params` 同一套 dispatch）：
  - 数字/布尔引脚接受**数字/布尔**，也接受**文本形式**（`"0.25"`、`"true"`、`"3"`）—— 因为 MCP 工具的 `value` 声明就是字符串，工具面只能传文本；
  - 对象引脚（`object/...`、`class/...`）接受三种写法：**资产路径**（`/Game/UI/WBP_HUD`）、**内嵌对象路径**（`/Game/UI/WBP_HUD.WBP_HUD:MyAnimation`）、**对象自己的名字**（`MyAnimation`）—— 裸名字在**容器蓝图内部**找（先图所属蓝图自身的子对象，再它的包），所以"接上本控件蓝图里那条动画"直接写动画名就行；
  - 解析不到时给 `load_failed` + **`candidates`**（作用域内该类对象的名字），类型不对给 `type_mismatch` 并点名两边类（"Asset 'X' (WidgetBlueprint) is not assignable to pin 'InAnimation' (WidgetAnimation)"）。
- 连线走 `UEdGraphSchema_K2`：类型兼容/隐式转换由引擎判断；目标输入引脚已有连线会被**自动断开**。响应里回的是**解析后的真实端点**（请求名可能被 schema 改写），以返回值为准。

### 3. 节点类型映射表

| 需求 | 用法 | 备注 |
|---|---|---|
| 函数调用 | `add_blueprint_function_node(target=<类名>, function_name=<函数名>, params={pin: value})` | `target` 如 `KismetSystemLibrary` / `MeshComponent`；`params` 写不进的引脚进 `failed`，**不静默丢弃**。**建的节点类按引擎 spawner 判据选**（`BlueprintFunctionNodeSpawner.cpp:208-247`）：带 `ArrayParm` 的函数 → `UK2Node_CallArrayFunction`，带 `DataTablePin` → `UK2Node_CallDataTableFunction`，带 `MaterialParameterCollectionFunction` → `UK2Node_CallMaterialParameterCollectionFunction`，可交换结合且纯 → `UK2Node_CommutativeAssociativeBinaryOperator`，否则 `UK2Node_CallFunction`。**不要指望普通 CallFunction 能带 wildcard 数组引脚**：`Array_Length` / `Array_Get` 这类函数的数组引脚定型、以及重建后的自愈，全都只在 `UK2Node_CallArrayFunction` 里（`AllocateDefaultPins` 强制 wildcard、`NotifyPinConnectionListChanged` 从对端拷类型、`PostReconstructNode` 逐引脚重派生）；建错类时"连线看起来正常、编译也过"，但**任何一次重建**（关卡加载 / 编译前刷新 / `refresh_blueprint_node`）都会把引脚打回 `wildcard[]` → `Target Array 的类型尚未确定` |
| 标准事件 | `add_blueprint_event_node(event_name="ReceiveBeginPlay")` | 标准事件带 `Receive` 前缀 |
| 输入动作事件 | `add_blueprint_input_action_node(action_name=...)` | **建的是 legacy `K2Node_InputAction`**（引脚 `Pressed`/`Released`/`Key`），配套 `create_input_mapping(action_name, key, input_type)` 写 `Config/DefaultInput.ini`（回执 `persisted: true` 才跨重启有效）。EnhancedInput 的 `K2Node_EnhancedInputAction` **只能手工在编辑器里拖**：用图库的 `lib.add_node_by_class` 建出来的实例会让 Kismet 编译器崩。前提：`UEnhancedPlayerInput::ProcessInputStack` 会先调 `Super::`（`EnhancedPlayerInput.cpp:779-781`），所以 legacy 映射在 Enhanced Input 工程里照旧生效；映射缺失时编译只报 warning（`引用了未知操作 'X'`）。 |
| 常量 | `add_blueprint_literal_node(literal_type, value)` | 本质是建 `KismetSystemLibrary.MakeLiteral<Type>` 并写其 `Value` 引脚（**不注入自定义节点类**）。`literal_type`：`float`/`double`/`int`/`int64`/`bool`/`name`/`byte`/`string`/`text`；未知类型 → `unsupported_literal_type`，值解析失败 → `invalid_value` 且不建节点。**`float` 与 `double` 都落到 `MakeLiteralDouble`**（5.5 里 `MakeLiteralFloat` 不是 UFUNCTION，它的显示名才叫 "Make Literal Float"） |
| 变量 Get/Set | `add_blueprint_variable_node(variable_name, node_kind="get"\|"set")` | 见 §4 |
| 本组件引用 / Self | `add_blueprint_get_self_component_reference(component_name)` / `add_blueprint_self_reference()` | |
| Unsafe 类 | `add_blueprint_node_by_class(node_class)` | 已验可用：`K2Node_ExecutionSequence`、`K2Node_Select`、`K2Node_SwitchEnum`、`K2Node_CastByteToEnum`、`K2Node_Knot`、`EdGraphNode_Comment`（短名或全路径）；未知类 → `node_class_not_found` + 候选 |

函数图/宏图操作：要往函数体里加节点，先 `list_blueprint_graphs` 确认 `graph_name` 存在（函数图名通常等于函数名，但别猜），再给每个命令传 `graph_name`。

**建函数图**：`add_blueprint_function_graph(blueprint_name, function_name, signature_class?)`

- 不给 `signature_class` = **用户函数**；给了 = **实现该类的 `BlueprintImplementableEvent`**（引擎按 `bIsUserCreated=false` + `SignatureFromObject=该类` 建图，与编辑器 Persona 对 `UAnimNotify` 的做法一致）。
- **蓝图里的动画回调就走这条**：先 `create_blueprint(parent_class="AnimNotify")`，再 `add_blueprint_function_graph(function_name="Received_Notify", signature_class="AnimNotify")`。
- 返回 `created`：同名已存在时是 `false`（**幂等复用**，重试安全），绝不建第二张。
- 返回 `entry_node_ids` / `result_node_ids`：往里连线用它们。**无返回值的用户函数没有结果节点**（`result_node_ids` 为空数组）；override（如 `Received_Notify` 返回 bool）两者都有。
- `signature_class` 上没这个函数 → `signature_function_not_found` + `candidates`（该类的可实现事件优先，其后是可调用函数），**不会建出空图**。
- `list_blueprint_function_graphs` 回 `is_override` 与 `signature_class`；`remove_blueprint_function_graph` 对引擎托管图（构造脚本）返回 `graph_not_removable`，删不存在的图返回 `function_graph_not_found`。
- **断言对象**：`compiled: true` + `errors: []` + 入口节点输出引脚含该事件自身的参数（`Received_Notify` = `MeshComp` / `Animation` / `EventReference`）。override **不需要**额外建"事件节点"，签名由函数图的入口节点承载。

**编辑函数签名**：`add_blueprint_function_param` / `remove_blueprint_function_param` / `rename_blueprint_function_param` / `rename_blueprint_function_graph`

- 参数 = 入口节点上的用户引脚（方向是 `EGPD_Output`）；**返回值/输出 = 结果节点上的引脚**，函数还没有结果节点时由命令按需创建（`FindOrCreateFunctionResultNode`）。所以"加返回值"就是 `add_blueprint_function_param(..., is_output=True)`。
- `param_type` 与 `add_blueprint_variable` 同一套（含 `"int[]"` / `"struct{}"` / `"int{,}"` 容器后缀）；不支持的类型 → `unsupported_variable_type` + 受支持类型清单。
- 每个签名类命令**回读整个签名**（`params`：`name`/`direction`/`type`/`sub_type`/`container`/`default_value`）并带编译结果；`list_blueprint_function_graphs` 的每项也有 `params`。
- 名字冲突 → 结构化错误 + 当前参数名在 `candidates`；删/改一个不存在的参数 → `param_not_found`。
- **override 函数（`signature_class` 建的）参数不可编辑**：签名属于父类 → `function_not_editable`。
- 参数改名不是"只有改 pin 名"：必须同时更新终结点节点的 `UserDefinedPins` 记录（否则 `ReconstructNode` 按旧记录重建，名字回弹），并修正图内引用该名的 getter 节点 —— 命令已按编辑器 `OnPinRenamed` 的序列实现。
- `rename_blueprint_function_graph` 改名函数图（函数随之改名）：占用名 → `graph_name_in_use`（不依赖引擎静默加后缀）；`old == new` 是成功空操作；不存在 → `function_graph_not_found`。

### 4. 变量

**声明成员变量**：`add_blueprint_variable(variable_name, variable_type, is_exposed?, default_value?, sub_class?)`

- 基础类型：`bool` / `byte` / `int` / `int64` / `float` / `double` / `name` / `string` / `text` / `object` / `class` / `struct` / `enum` / `vector` / `vector2d` / `rotator` / `transform` / `linear_color`
- 容器后缀（**非直觉，别猜**）：`"int[]"`=Array、`"struct{}"`=Set、`"int{,}"`=Map
- `sub_class` 传 object/class/struct/enum 的资产路径。未知类型 → `unsupported_variable_type` + `supported_types`
- **`is_exposed` 是"实例可编辑"（那只眼睛），不是"类默认值可见"**：`is_exposed=false` 置位 `CPF_DisableEditOnInstance`（关卡里的实例不可在细节面板改），`true` 清位，**缺省不动位**；它与 `set_blueprint_variable_flags(props={"instance_editable": …})`、`list_blueprint_variables.flags.instance_editable` **读同一个位**。想要"类默认值面板里能不能改"（`CPF_Edit`）看**只读字段 `class_editable`** —— 两者是不同的位，改一个 MUST NOT 动另一个。
- **名字占用分三种，别再靠"命令成功"当变量加上了**：
  - 空名 → `invalid_params`（引擎对空名只回一个裸 false，命令在调用前挡下）；
  - 名字是**本蓝图自己**的变量 → **幂等成功**（重复调用只重写 `default_value` / `is_exposed`），回执的 `pin_category` / `container` / `default_value` / `is_exposed` **从变量读回**；
  - 名字**只**在父类/继承属性里（如 `AActor` 的 `Tags`、`RootComponent`）→ `variable_name_in_use` + `candidates`（父类可见变量名），**蓝图状态不变**。
- 判定用的是引擎 `AddMemberVariable` 内部同一份 `GetClassVariableList`，所以"命令说没占用、引擎说占用"这种二义不存在；`AddMemberVariable` 仍拒（防御路径）→ `invalid_value`。

**建变量节点**：`add_blueprint_variable_node(variable_name, node_kind, graph_name?, is_local?, variable_type?, sub_class?, default_value?)`

- `is_local=True` 必须同时给 `graph_name`（函数图名，非函数图 → `invalid_params`）。
- **局部变量的寻址是图作用域**（`FindLocalVariable(Blueprint, 顶层函数图, name)`），**不是** `GeneratedClass` 上的 `FProperty`：局部变量住在函数入口节点的 `LocalVariables` 上。
- 局部变量**已存在** → 直接复用建节点（回执 `is_local: true`），**不重复添加**（引擎 `AddLocalVariable` 无去重，多调一次会多出一条）；只有在它**不存在**时才需要 `variable_type`（object/struct/enum 另加 `sub_class`）来创建。
- 局部变量不存在且**没给** `variable_type` → `variable_not_found` + `candidates`（该函数图现有局部变量名）。
- 节点的引用是 **local member**（`VariableReference.SetLocalMember(name, 顶层函数图名, VarGuid)`，`K2Node_LocalVariable.cpp:154/185` 的引擎配方），不是 self/member 引用 —— 局部变量的 scope 是函数图而不是类。这正是为什么它**没有 self 引脚**。
- 局部变量节点**建出来就有引脚**（get：变量输出 + `0.0` 之类的默认值；set：`execute`/`then` + 变量输入 + `Output_Get`），因为编译把局部变量变成了 UFUNCTION 上的属性 —— **不需要**手工建引脚。
- 同一路径也接受"顺手创建"：`is_local=True` + 不存在的名字 + `variable_type` → 命令先 `AddLocalVariable`（并编译一次让它可寻址）再建节点。
- 成员变量查不到 → `variable_not_found` + 可用变量名列表。
- 刚用 `add_blueprint_variable` 加的成员变量，**要先编译才会成为 `GeneratedClass` 上的 FProperty** —— 内核已处理（查不到就编译一次再查），但调用方别在"未编译"状态下去读它的类型。

**写别的类的属性（`owner_class`）**：`add_blueprint_variable_node(variable_name, node_kind, owner_class="PlayerController")`

- 给出 `owner_class` 时建的是**非 self 上下文**的外部成员引用（`VariableReference.SetFromField<FProperty>(Prop, false)`）：节点带一个**类型为该类的 `self` 引脚**，回执 `self_context: false`。
- **Target 引脚必须连**：它没有默认对象，不连就是"无效目标"而**编译失败**（`K2Node_Variable::CheckForErrors`）——这是引擎的正确行为，不是工具问题。典型接法：`GameplayStatics.GetPlayerController(0)` 的 `ReturnValue` → 该节点的 `self`。
- 属性必须在 `owner_class`（含父类）上**蓝图可见**，否则 `variable_not_found` + 该类全部可见属性名（`ActorInstanceGuid` 这类 `BlueprintReadOnly` 属性可见但只读）；`node_kind="set"` 撞上 `BlueprintReadOnly` → `property_not_writable`。
- 位域 bool 照常可写（`APlayerController::bShowMouseCursor` 是这类属性的例子）。
- 与 `is_local=True` **不能同时给**（`invalid_params`）—— 一个是类作用域、一个是函数作用域。
- 不给 `owner_class` 时行为与以前一致（self 上下文，`add_blueprint_get_self_component_reference` 那种节点不会因缺 self 连线而报错）。

**删 / 改名 / 改类型 / 改默认值 / 改 flags（成员变量）**

| 需求 | 命令 | 关键点 |
|---|---|---|
| 读 | `list_blueprint_variables` / `get_blueprint_variable_info` | 只列**本蓝图自己**的变量（不含父类），每项带 `flags{}`：`instance_editable` / `expose_on_spawn` / `blueprint_read_only` / `replicated` / `rep_notify_func` / `category` / `tooltip` / `transient` / `private` |
| 删 | `remove_blueprint_variable` | **删变量会动图**：命令同时调 `RemoveMemberVariable` + `RemoveVariableNodes`，把引用该变量的 get/set 节点一并清掉；改完回 `compiled`/`errors` |
| 改名 | `rename_blueprint_variable` | **改名也会动图**：图内引用节点跟随新名（回读 `reference_nodes_renamed` / `reference_nodes_stale`，后者必须为 0）；占用名 → `variable_name_in_use`（引擎自己不去重，撞名会被静默吞掉） |
| 改类型 | `set_blueprint_variable_type` | 类型解析**复用与 `add_blueprint_variable` 同一份**（含 `int[]` / `struct{}` / `int{,}` 后缀）；也可以改写 `container=none/array/set/map`，两者冲突 → `unsupported_container_type`；不支持类型 → `unsupported_variable_type`+`supported_types`；**变量已被图节点引用时直接拒**（`variable_referenced_by_nodes`，见下） |
| 改默认值 | `set_blueprint_variable_default_value` | 写 `FBPVariableDescription.DefaultValue`，由**编译**把它解析进 CDO（`KismetCompiler::SetPropertyDefaultValue`）；解析失败会在 `errors` 里现形，不会静默回退。**回读要看 CDO**：编译后引擎会把这个描述字段清空（struct/容器类型必清），所以命令回的是 CDO 上的导出文本 + `default_value_source: "cdo"` |
| 改 flags | `set_blueprint_variable_flags(props={...})` | 逐项 `applied`/`failed`；未知键进 `failed` 并带候选键名。语义映射（与引擎详情面板一致）：`instance_editable`→清/置 `CPF_DisableEditOnInstance`；`expose_on_spawn`/`private`/`tooltip`→元数据（`MD_ExposeOnSpawn`/`MD_Private`/`MD_Tooltip`）；`blueprint_read_only`→`CPF_BlueprintReadOnly`；`replicated`→`CPF_Net`；`rep_notify_func`→`RepNotifyFunc` + `CPF_RepNotify|CPF_Net` |

- `rename_blueprint_variable`：引擎的 `RenameMemberVariable` 在变量带 OnRep 函数时会弹模态对话框，命令在调用前先清 `RepNotifyFunc`（与对话框选"Yes"的结局一致），并在响应里回 `rep_notify_cleared`；看到这个字段说明 OnRep 关联被断开了。
- `set_blueprint_variable_type`：引擎的 `ChangeMemberVariableType` 会重建**引用该变量的图节点**的引脚，而**在被引用时这条调用会把 GameThread 冻死**（实测：编辑器还活着、端口还监听、日志再无新行、客户端连续两次超时 —— 见 `Docs/MCP_Findings_2026-10-06_platformer-round.md` §3）。⇒ 命令改成**前置拒绝**：调用前扫全图找 `UK2Node_Variable` 里指向该变量的节点，命中就回 `variable_referenced_by_nodes` + 列出 `referencing_nodes[]`（图名/节点名/类名）+ `hint`。安全路线就是当时那份规避法：`remove_blueprint_variable`（它本来就会连带清掉引用节点）→ `add_blueprint_variable`（目标类型）→ 重建节点。**没有覆盖开关**（曾试过 `force=true`，现已撤掉）：实测它**不改类型**（回执却带 `compiled: true`）、照样弹框，而且**手点那个框也没把类型改上去** ⇒ 这个形状下这条路根本走不通。机制已定性为**模态框**（不是死循环 —— 人点就走），且卡在**命令返回之后**；并且**该框不走 `FCoreDelegates::ModalMessageDialog`**：给它套 `FUnrealMCPScopedDialogAutoAnswer` 仍会卡住（若走委托，弹框瞬间就该被自动答掉）⇒ **自动应答救不了它**，护栏只有前置拒绝。另外回执带 `type_change_not_applied`，把"看着成功、其实没改"变成显式失败（详见 `Docs/MCP_Findings_2026-10-06_platformer-round.md` §3）。

**删 / 改名 / 改默认值（局部变量）**：`list_ / add_ / remove_ / rename_blueprint_local_variable`、`set_blueprint_local_variable_default`

- 作用域是**函数图**（`function_name`）：局部变量存在函数入口节点的 `LocalVariables` 上，生命周期到该函数为止。
- 局部变量**不能遮蔽**成员变量（本蓝图或父类的任一成员名）→ `variable_name_in_use`（引擎的 `RenameLocalVariable` 在撞名时是静默 no-op，命令已改成显式报错）。
- override 函数（`add_blueprint_function_graph(signature_class=…)` 建的）签名属于父类 → 全部局部变量命令报 `function_not_editable`。
- **引用节点与局部变量同生共死**（可验收）：引用它的 get/set 节点用 `add_blueprint_variable_node(is_local=True, graph_name=…)` 建成；`remove_blueprint_local_variable` 后这些节点**消失**（用 `check_blueprint_element(kind="node", name=<guid>)` 断言 `found: false`）；`rename_blueprint_local_variable` 后它们跟随新名（回 `reference_nodes_renamed` ≥ 1、`reference_nodes_stale` == 0）。

### 5. 组件与层级

| 需求 | 命令 | 语义 |
|---|---|---|
| 加组件 | `add_component_to_blueprint(component_type=<无 U 前缀的类名>, component_name, location/rotation/scale, component_properties?)` | **所有新组件都落在 SCS root**，不建层级 |
| 建层级 | `attach_component_to_component(child_component, parent_component, socket_name?)` | child/parent 都是**组件变量名**；socket 可选 |
| 组件属性 | `set_component_property(component_name, property_name, property_value)` | 属性名可 C++ 名或 snake_case；回 `property_value_before/after`；**改的是 SCS 模板** ⇒ 另回 `placed_instances` / `counted_in`（`editor`/`pie`），实例数 > 0 时附 `hint`：**已放置实例各自保存属性值、不跟随模板** |
| CDO 属性 | `set_blueprint_property(property_name, property_value)` | 改类默认对象 |

- 引擎口径：同一 SCS 内挂接**只** `AddChildNode`，不写 `ParentComponentOrVariableName` / `ParentComponentOwnerClassName`（那是"父组件来自另一个 SCS"才用的路径，写错会让引擎 PostLoad 命中 `possible cyclic linkage` 的 `ensure`）；socket 走 `AttachToName`。命令已按此实现，返回值里的 `attach_parent` / `attach_socket` / `is_root` 是**服务端从 SCS 实读回读**的，不是回显请求。
- `USCS_Node::AddChildNode(..., bAddToAllNodes)` 对 `AllNodes` 不去重：重挂组件时按"是否真的从 AllNodes 摘除"决定该标志位，并用 `len(names) == len(set(names))` 断言无重复登记。
- 失败码：`component_not_found`（带可用组件名）/ `invalid_attach`（自挂、成环、非 scene component）。

**改之前先看清结构**：`get_blueprint_component_hierarchy` 是唯一能看到组件树的地方（`get_asset_properties` 只回资产自身属性，没有组件树）。每项给 `parent` / `attach_socket` / `is_root` / `is_inherited` / `children`，并回 `root_components` / `root_count` / `unique_root` / `duplicate_components`。非 Actor 蓝图（AnimNotify / AnimInstance）没有 SCS → `blueprint_not_ready`。

**删组件 / 换根 / 摘除 / 碰撞**

| 需求 | 命令 | 保护与回读 |
|---|---|---|
| 删组件 | `remove_component_from_blueprint(component_name, recursive=true, force=false)` | ① **唯一的根删不掉**：目标是 SCS 里唯一的 root 且 `force=false` → `root_component_protected`（回 `hint: force=true`）；② `recursive=false` 且有子组件 → `component_has_children`（带子组件名），**绝不静默丢子树**；③ 删完回读剩余清单，`removed_components` 给出被删的整棵子树 |
| 换根 | `set_blueprint_root_component(component_name)` | 目标成为 SCS 根，原根变成它的**子组件**（非 scene 组件的旧根只保持为根，不会挂成子节点）；新根的位置/旋转归零、attach socket 清空。回读整棵层级并断言 **`unique_root: true`**（换根最容易出"两个根"），同时断言 `component_count` 不变 |
| 摘除 | `detach_component(component_name)` | 从父组件摘出并挂回 SCS 根（**不删除**）；已经是根时 `already_root: true`（幂等）。非 scene 组件 → `component_not_scene` |
| 碰撞 | `set_component_collision(component_name, props={collision_enabled, collision_profile, object_type, responses})` | 走既有属性反射写 `BodyInstance`（与 `set_component_property` 同一内核），逐项 `applied`/`failed`，回读 `collision{}`。非 primitive 组件 → `component_not_primitive`；未知键 → `failed`+候选键名。**同样只改 SCS 模板** ⇒ 另回 `placed_instances` / `counted_in` + `hint`（实例不跟随） |

- 碰撞的 `responses` 只接受 `{通道名: 响应名}`，通道/响应名**前缀可省**（`Visibility` / `ECC_Visibility` / `Ignore` / `ECR_Ignore` 都认）—— `ECollisionChannel` / `ECollisionResponse` 的成员带 `ECC_`/`ECR_` 前缀且**没有 DisplayName**，反射器只认带前缀的拼法，命令内部先解析成成员名再交出去；`responses` 走引擎的 `FBodyInstance::SetResponseToChannels`（`FCollisionResponse::ResponseToChannels` 是 transient，反射器写不了）。
- `object_type` 同样两种拼法都收（`WorldStatic` / `ECC_WorldStatic`）。
- **已知取舍**：直接改变量式写入（`collision_enabled` / `object_type` / `responses`）**不会**同步 `CollisionProfileName` —— 引擎的 `FBodyInstance::InvalidateCollisionProfileName()` 是 private，跨模块调不到；`collision{}` 会如实回读两者，要 profile 一致就显式写 `collision_profile`。
- **SCS 根行为**：`add_component_to_blueprint` 加的第一个 scene 组件会成为根、并让 SCS 退休 `DefaultSceneRoot`；再加的 scene 组件会被 `ValidateSceneRootNodes` **挂到现有根下面**。要造第二个根：加组件后 `detach_component`。非 scene 组件（如 `ActorComponent`）不参与这套重挂，自身就是个 root。
- 重挂到别的父组件用 `attach_component_to_component`（内部 detach + attach 并回读 `attach_parent`/`attach_socket`/`is_root`），删多余组件用 `remove_component_from_blueprint`，只有"摘到根"这一种情况用 `detach_component`。

### 5b. 节点位置、引脚形态与自定义事件

| 需求 | 命令 | 关键点 |
|---|---|---|
| 挪节点 | `move_blueprint_node(node_id, position=[x,y], graph_name?)` | 写 `NodePosX/Y` 并**回读** `pos_x`/`pos_y` |
| 重建引脚 | `refresh_blueprint_node(node_id, graph_name?)` | 跑 `ReconstructNode` 并回读**刷新后的引脚清单**；同时回 `node_id_before` / `node_id_changed`（重建可能换 guid，别继续用旧 id） |
| 拆 / 合引脚 | `split_blueprint_pin` / `recombine_blueprint_pin(node_id, pin_name, graph_name?)` | 走 `UEdGraphSchema_K2::SplitPin` / `RecombinePin`（虚函数，跨模块可调）。**回读 `sub_pins[]`**：拆后非空（子引脚名以父引脚名开头）、合后为空；不可拆（非结构体、已拆、已合、引擎拒绝）→ `pin_not_splittable`；`recombine` 传了子引脚时回 `parent_pin` 指路 |
| 自定义事件 | `add_blueprint_custom_event_node(event_name, params=[{name,type,sub_class?}], node_position?, graph_name?)` | `UK2Node_CustomEvent` + 参数走用户引脚（与函数参数同一套类型解析）；**回读参数清单**；重名（含与函数图撞名）→ `custom_event_name_in_use`+候选；某个参数建失败会把刚建的节点删掉（不留半成品） |
| 事件改名 | `rename_blueprint_custom_event(node_id, new_name, graph_name?)` | 走节点自身的 `OnRenameNode`（参数保留）；回读新事件名与 `node_id`（附 `node_id_before`/`node_id_changed`） |
| 打开 / 聚焦 | `open_blueprint_graph(graph_name?)` / `focus_blueprint_node(node_id, graph_name?)` | 三态语义，**没有静默 false**：资产不存在 → `blueprint_not_found`；图不存在 → `graph_not_found`+`available_graphs`；编辑器会话不可用 → `editor_not_open`。`open_blueprint_graph` 会按需打开蓝图编辑器（用到 `FBlueprintEditor::OpenGraphAndBringToFront`），`focus_blueprint_node` 用 `JumpToNode` |

- `node_id` 不必配 `graph_name`：不给 `graph_name` 时命令会**遍历该蓝图的全部图**找这个 guid（guid 全蓝图唯一），比默认落到事件图更不容易误报 `node_not_found`。

**`split_blueprint_pin` 是"结构体引脚写不进默认值"的通用绕法**（实测，为给 `UHitReact::HitReact` 的三个结构体入参赋值而摸出来）：

症状与原因：

- `set_blueprint_pin_default` 对**非 compact 的结构体引脚**直接拒：`unsupported_pin_type` + `Struct pin 'World' of type 'HitReactImpulse_WorldParams' is not supported`；
- **结构体文本默认值会被静默丢弃**：写 `(LinearDirection=(X=0,Y=1,Z=0))` 回读 `default_value` 仍是空、BP 变量走 `set_blueprint_variable_default_value` 读回 CDO 是 `()`（引擎 `FProperty::ImportText_Internal` 会跳过**非可编辑**字段 —— `Transient, VisibleInstanceOnly` 且无 `CPF_Edit` 的成员就是这么被吃掉的；python 侧 `struct.import_text()` 会显式报 `Cannot perform text import on property 'X' here`）；
- 造 `K2Node_MakeStruct` 补不上：`set_blueprint_node_property(node_id, 'StructType', '/Script/<Mod>.<Struct>')` 回 `load_failed` —— 该属性的对象解析器只认**资产路径**，解析不了 `/Script/...` 里的脚本结构体（`reflect_probe` 只会告诉你 `supported_shapes: ["asset path string"]`）。

⇒ 解法：**把结构体输入引脚拆开**。子引脚是普通类型（bool / real / FName / Vector / soft object …），默认值全都能写，且**拆开本身满足 by-ref 入参"必须连线"的编译器要求**：

```
split_blueprint_pin(node_id, 'Params')                 → Params_Profile / Params_SimulatedBoneName / Params_bIncludeSelf / ...
split_blueprint_pin(node_id, 'Impulse')                → Impulse_LinearImpulse / Impulse_AngularImpulse / Impulse_RadialImpulse
split_blueprint_pin(node_id, 'Impulse_LinearImpulse')  → Impulse_LinearImpulse_bApplyImpulse / _Impulse / _bFactorMass
split_blueprint_pin(node_id, 'World')                  → World_LinearDirection / World_AngularDirection / World_RadialLocation
set_blueprint_pin_default(node_id, 'World_LinearDirection', '[0.0,1.0,0.0]')
set_blueprint_pin_default(node_id, 'Params_Profile', '/ProcHitReact/Profiles/HRP_Flop.HRP_Flop')
```

- **拆引脚吃掉了 by-ref 的报错**：`Params` / `World` 是 `const T&`，未拆时编译报"必须连接一个输入（by ref 参数需要一个有效的输入）"；拆开后这条消失，最终 `compiled: true` + `errors: []`。**所以结构体 by-ref 入参不需要 Make 节点，也不需要额外的 producer 节点**。
- **Vector 子引脚要传 JSON 数组字符串**：`value` 是字符串，传 `"[0.0,1.0,0.0]"` 成功，传 `"0,1,0"` 回 `type_mismatch`（`expected an array of numbers or a struct object`）。
- 子引脚名恒为 `<父引脚名>_<字段名>`，可取 split 回执的 `sub_pins[]` 现读，不要猜。
- 该手法的相关事实（同一个夹具里踩到）：**python 调不到蓝图自定义事件**（`FUNC_BlueprintEvent` 不在 python 暴露面，`hasattr(actor, 'punch')` 为 `False`）⇒ 想让"python/左键"驱动，用 `K2Node_InputKey` 事件节点（`add_blueprint_node_by_class('K2Node_InputKey')` + `set_blueprint_node_property(node_id, 'InputKey', '{"KeyName": "LeftMouseButton"}')`，免写 `DefaultInput.ini`）；**非 possessed 的 actor 收不到输入**，还要把 `AutoReceiveInput` 设成 `Player0`（`set_blueprint_property(property_name='AutoReceiveInput', property_value='Player0')`，而且**同关卡里已存在的实例要单独再设一次**：实例保存的是差值属性，CDO 改了不会回溯，实测 PIE 里读回仍是 `DISABLED`，得在编辑器世界对实例 `set_editor_property('auto_receive_input', unreal.AutoReceiveInput.PLAYER0)` 或重新 spawn）。

### 5c. 接口、存在性检查与蓝图对比

| 需求 | 命令 | 关键点 |
|---|---|---|
| 实现接口 | `implement_blueprint_interface(interface_path)` | 走 `FBlueprintEditorUtils::ImplementNewInterface`，图由引擎自动生成；**响应回读 `generated_graphs[]`**（否则调用方不知道多了什么）；重复实现幂等（`already_implemented: true`，不重复建图）。`interface_path` 同时接受 `/Script/<Module>.<Interface>` 与蓝图接口资产路径；解析不到 → `interface_not_found`+候选 |
| 移除接口 | `unimplement_blueprint_interface(interface_path, preserve_functions=false)` | 走 `RemoveInterface`；`preserve_functions=false` 时接口产生的图一并删除，回读 `removed_graphs[]` 与剩余 `interfaces[]`。**未实现的接口会先被挡下**（引擎的 `RemoveInterface` 对未实现接口会命中 `ensure`） |
| 列接口 | `list_blueprint_interfaces` | 每个接口的路径 + 每个接口函数的 `implemented` 状态（函数式签名看接口自己的图、事件式看 ubergraph 事件节点） |
| 找可覆盖函数 | `list_overridable_functions(include_parent_events=true)` | 只列**能被蓝图覆盖的事件**（`UEdGraphSchema_K2::CanKismetOverrideFunction`，即 `FUNC_BlueprintEvent` 且非 delegate/内部/废弃），给 `declaring_class` 与 `overridden`；普通 `BlueprintCallable` 函数不在结果里。**配合 `add_blueprint_function_graph(signature_class=…)` 用**：`signature_class` 传 `declaring_class` 的类路径/类名，不再靠猜函数名 |
| 存在性检查 | `check_blueprint_element(kind=variable\|function\|component\|graph\|node, name)` | **这是唯一的存在性入口**：一条顶替一堆 `*_exists`。`found: false` 是**正常返回**（不是错误码 —— 错误码只留给参数错与蓝图不存在）；`kind` 非法 → `invalid_params`+合法 kind 列表。**要加新 kinds 时是加枚举值，不要再长出一批 exists 命令** |
| 对比蓝图 | `compare_blueprints(blueprint_a, blueprint_b)` | 变量 / 函数图 / 组件 / 接口四项的 `only_in_a`/`only_in_b`/`changed` + 两侧 `parent_class`；不返回单个布尔。一侧解析失败 → `blueprint_not_found` + `side: "a"\|"b"` |

- **接口图不在 `Blueprint->FunctionGraphs` 里**：`FBlueprintEditorUtils::AddInterfaceGraph` 建好图后只把它放进 `FBPInterfaceDescription::Graphs`，所以 `list_blueprint_function_graphs` 与"图删除清单"不含接口图。枚举接口图用 `implement`/`unimplement` 回读的 `generated_graphs` / `removed_graphs`，或 `list_blueprint_interfaces` 的 `graphs`。

### 6. 写值的形态与错误码

`params`（函数节点）、`set_component_property`、`component_properties`、`set_blueprint_pin_default`、`set_blueprint_node_property` 共用同一套值派发：

| 目标类型 | 传法 |
|---|---|
| 数值 / bool | JSON number / bool（字符串形式 `"3.0"`、`"true"` 也接受） |
| 字符串类（FName/FString/FText） | 字符串 |
| 枚举 | 成员名或数字 |
| Vector / Vector2D / Rotator / LinearColor | `[x,y,z]`、`[r,g,b,a]` 数组，或 `{"X":1,...}` 对象；反射库的 `value_kind="vector"` 另接受逗号串 `"1,2,3"` |
| object / class 引脚或属性 | **资产路径字符串**（如 `/Game/BP_Foo.BP_Foo`） |
| 结构体属性（无 codec 的） | 字段对象 `{"KeyName": "I"}`，或**结构体文本字符串**（走结构体自己的 `ImportText`，所以 `FKey` 直接写 `"I"`、`"SpaceBar"` 即可；写进去的值仍以字段对象读回）。结构体自己拒收（或"导入成功却没变化"）→ `type_mismatch` + `supported_shapes` 列出三种形态 |
| 数组属性 | 数组（object 数组元素仍是路径字符串） |

错误码（两个入口一致，**报错响应里带自纠信息，先读它再改**）：

| 错误码 | 含义 | 附带信息 |
|---|---|---|
| `graph_not_found` | 图名不存在 / 多张 EventGraph 歧义 | `available_graphs` |
| `unsupported_graph_kind` | 对宏图做写入 | — |
| `node_not_found` | guid 不在该图 | — |
| `node_class_not_found` | K2 类名无法解析 | 候选类名 |
| `function_not_found` / `variable_not_found` | 函数/变量名不在目标类/蓝图上 | 可用名列表 |
| `pin_not_found` / `output_not_found` / `input_not_found` | 引脚名不对 | 可用引脚 |
| `not_an_input_pin` | 给输出引脚写默认值 | — |
| `incompatible_types` | 两端类型不兼容 | 两端类型 |
| `unsupported_pin_type` / `unsupported_variable_type` / `unsupported_literal_type` | 类型面不支持 | `supported_types` |
| `unknown_property` | 节点/组件上没这个属性 | `Did you mean: ...` / `candidates` |
| `property_not_writable` / `unsupported_property_type` / `load_failed` / `type_mismatch` | 属性写入四类失败（**这五个码不在错误码常量表里，是内核按消息分类出来的**） | — |
| `invalid_value` / `invalid_params` | 值解析失败 / 参数非法 | — |
| `component_not_found` / `invalid_attach` | 组件挂接 | 可用组件名 |
| `blueprint_not_ready` | 蓝图无可用 SCS（先编译） | — |
| `variable_name_in_use` | 变量改名/新增撞名（成员或局部；局部还含遮蔽成员变量）。两种子情形：**本蓝图已有**同名 → 不算错误（`add_blueprint_variable` 是幂等成功，只有 `rename_*` 才报此码）；**父类/继承属性占用**（`GetClassVariableList` 命中而本蓝图 `NewVariables` 未命中，如 `Tags`）→ `add_blueprint_variable` 报此码且状态不变 | `candidates` |
| `component_has_children` / `root_component_protected` | 删组件的两道保护（不递归 / 删唯一的根） | 子组件名 / `hint` |
| `component_not_scene` / `component_not_primitive` | 目标不是 scene 组件（换根、摘除）/ 没有 BodyInstance（碰撞） | — |
| `pin_not_splittable` | 引脚不可拆/不可合（非结构体、已拆、已合、引擎拒绝） | `sub_pins` / `parent_pin` |
| `custom_event_name_in_use` | 自定义事件重名（也含与函数图撞名） | `candidates` |
| `interface_not_found` | 接口路径解析不到 / 该蓝图未实现该接口 | `candidates` / `tried` |
| `editor_not_open` | 编辑器会话不可用（open/focus 类命令） | — |

`set_blueprint_node_property` 额外注意：

- 只写**节点自身**的 `CPF_Edit` 属性（不是引脚默认值）。
- **是否重建引脚由属性类型推导，不是名字白名单**：写入**枚举型属性**（`FEnumProperty`、带枚举的 `FByteProperty`、指向 `UEnum` 的对象属性）或**容器属性**（`FArray`/`FSet`/`FMap`）→ 写后 `ReconstructNode()`，响应 `pins_rebuilt: true` + 刷新后的引脚列表 + **新 guid**；其余类型（数值 / 字符串 / 名称 / 文本 / 布尔 / 结构体等位置与视觉属性）→ `pins_rebuilt: false`。
  - 判据是"属性类型能否驱动引脚"，**不是**写入前后引脚有没有变（引脚变化是重建的**结果**，写属性本身不动引脚，所以前后快照比对判不出来）。
- 不确定某个命令名/参数名时，用自省工具 `list_mcp_commands`（可选 `category`）直接问 bridge：返回每条命令的 `params`（含 `required`）与 `flags`，比读源码或猜名字可靠。
- 响应恒有 `property_type` / `property_value_before` / `property_value_after`（读回值，用它断言，别信请求）。
- **对象引用引脚的默认值按家族只写一侧**（引擎硬规则 `EdGraphSchema_K2`）：`class` / `object` / `interface` 引脚只设 `DefaultObject`、字符串留空；`soft object` / `soft class` 只设路径字符串、object 留空。两个都设 = 蓝图**必然编译不过**（报 `String NewDefaultValue ... specified on class pin`）。插件已按此写入；回读时 `default_value` 与 `default_object` 都会给出对象路径（路径由 `DefaultObject` 推导），所以"读到路径"仍然成立。
- **成员变量节点是 self 上下文**（与编辑器拖组件进图一致）：`add_blueprint_variable_node(is_local=False)` 与 `add_blueprint_get_self_component_reference` 建出的节点**不需要**给它的 self 引脚接线就能编译；如果看到"变量节点使用了无效的目标"，那是节点被建成非 self 上下文的旧状态，删掉重建即可。**局部变量节点（`is_local=True`）不是 self 上下文**，它压根没有 self 引脚 —— 别按 self 那套去接。**给了 `owner_class` 的节点也不是 self 上下文，而且必须连它的 Target 引脚**（不连必然编译失败，见 §〇.4 的 `owner_class` 小节）。
- **判断 self 上下文只看 `self_context` 回读，别看有没有 `self` 引脚**：引擎对**定义在本蓝图里**的成员函数**总会**建一个 `self`(Target) 引脚（`FBlueprintNodeStatics::CreateSelfPin`，`BlueprintNodeStatics.cpp:48-52`：`FunctionClass == GetBlueprint()->GeneratedClass` ⇒ 建 `PSC_Self` 引脚；只有 pure 函数才把它隐藏，`K2Node_CallFunction.cpp:1177`），而 `function_reference.member_parent` 为空**正是** self 上下文的形态（自引用时 `MemberReference.h:90-93` 把 `MemberParent` 置空）。判据一律读 `self_context`（`find_blueprint_nodes` 的节点对象与建节点响应都带，值来自 `FunctionReference/VariableReference.IsSelfContext()` 直读）。self 上下文 = Target 由引用隐含给出 ⇒ **不要去连那条引脚**：引擎没有"连上 self 引脚就把自类调用改成调在被连对象上"的正规路径（`K2Node_CallFunction::PostReconstruction`，`K2Node_CallFunction.cpp:2036-2072` 只在能解析出**外部**类时才 `SetExternalMember`），连了只会留下一个多余的连接，并让下一个读图的人继续误判。
- **编译断言必须看效果**：`compile_blueprint` 返回 `status`（`EBlueprintStatus` 名）、`compiled`（只有 `BS_UpToDate*` 才为 true）与 `errors`。写脚本时 MUST 断言 `compiled is True` 且 `errors == []`，不要只断言命令 `status == "success"`。
- **编译通过 ≠ 逻辑接通**：`compiled=true`/`errors=[]` 与"输出悬空、必需输入吃默认值、exec 输入多来源"完全可以并存。接完线先跑 `verify_blueprint_graph`（只读、不编译不保存）再谈验收；默认规则 `dangling_producer`、`required_input_unconnected`、`exec_multi_source`、`unreachable_node`、`anim_sequence_missing`、`anim_root_unconnected`，可用 `rules` 只跑一条。`issues` 为空才算静态通过。
- **认节点用语义字段，别用标题**：`find_blueprint_nodes` 与建节点命令共用同一序列化 —— 函数类节点回 `function_reference`（`member_name`/`member_parent`/`member_guid`，非本地化），`K2Node_DynamicCast` 回 `target_type`，AnimGraph 节点回 `inner_node`（`sequence`/`loop_animation`/`play_rate` 等白名单）。中文环境里靠 `node_title` 或引脚顺序区分 Add 与 Greater 会认反。
- **删除类命令会自证**：`safe_delete_asset` 返回的 `deleted` 只在**盘上文件与内存对象都消失**时才为 true；`asset_path` 接受对象路径 / 包路径 / 短名三种形态；失败时 `detail` 会说明原因（例如包内仍有存活对象）。`create_input_mapping` 返回 `persisted`（映射是否真写进 `Config/DefaultInput.ini`，不落盘则编辑器重启后失效）。

### 7. 批量脚本与 bridge 回环

编辑器 python（`execute_python_command` / `execute_python_file`）已经在 GameThread 上，**不能**走 TCP 发命令（会被排回 GameThread → 死锁）。两条替代：

```python
import json, unreal

def bridge(command, **params):          # 与 TCP 完全相同的同步派发
    return json.loads(unreal.UnrealMCPPythonAPI.execute_mcp_command(command, json.dumps(params)))

lib = unreal.UnrealMCPBlueprintGraphLibrary           # 反射库（图/节点/引脚的对象面）
bp  = unreal.load_asset("/Game/Blueprints/BP_Foo")
g   = lib.ensure_event_graph(bp).graphs[0].graph
r   = lib.add_function_call_node(g, "KismetSystemLibrary", "Delay", 320, 0)
assert r.success, r.error_message
assert lib.set_pin_default(r.node, "Duration", "3.0", "auto").success
```

- `bridge()` 返回 JSON 字符串解析后的 dict，形状与 TCP 响应一致；**命令名 = MCP 工具名**（少数不同见下）：
  - `connect_blueprint_nodes` 的 bridge 参数名与工具一致（`source_node_id` / `source_pin` / `target_node_id` / `target_pin`）；
  - 组件挂接 bridge 命令名 = `attach_component_to_component`；
  - 材质域另有一套名字（见材质 Skill），别串用。
- 回环**禁用名单**：`execute_python_command` / `execute_python_file` / `take_screenshot` → `{"status":"error","error":"reentry_forbidden"}`。
- 长脚本走 `Saved/MCPScripts/*.py` + `execute_python_file(file_path=..., timeout=...)`（同步；重活拆成多次短调用）；脚本内 `print()` 输出中间状态。
- 若只是要"建节点 / 连线 / 断言"，直接用反射库更短（对象面，不需要字符串 guid）；要"结构化错误信封 / 增量改场景 / 组件命令"时用 `bridge()`。同一脚本里混用是常态。

### 8. 保存、编译、回验

```python
lib.compile_blueprint_checked(bp)      # 或 bridge("compile_blueprint", blueprint_name=...)
```

- `compiled=true` 对 `BS_UpToDate` **和** `BS_UpToDateWithWarnings` 都为真 → **想确认零警告必须读 `warnings` 数组**，别只看 `compiled`。
- `errors` **只报本次编译**：命令在编译前清空全部图的节点消息，节点 `ErrorMsg` 只在编译器再次访问它时才被重写（修好后旧消息常驻，属正常）。回包另有 `saved`（是否落盘）。
- **写命令成功即落盘**，且落盘**完全不看包的脏标志**：落盘走 `UEditorLoadingAndSavingUtils::SavePackages({Package}, /*bOnlyDirty=*/false)`（`UnrealEd/Public/FileHelpers.h:39`；`bOnlyDirty=false` 会跳过 `Package->IsDirty()` 那道闸）。
  原因：`FBlueprintEditorUtils::MarkBlueprintAsModified` 在 `Blueprint->bBeingCompiled` 时直接 return（`BlueprintEditorUtils.cpp:1986`），而**每条结构性图改动都会就地同步编译骨架**（`:1967`）⇒ "改图"与"编译"同栈、窗口内的改动不置脏。
  **不要**依赖 `save_asset(path, only_if_is_dirty=True)` 判成败；python 侧要手动兜底时用 `unreal.EditorLoadingAndSavingUtils.save_packages([pkg], False)`（同一条 C++ 路径）。
- 状态判定以**直读**为准：节点数用 `graph.node_count` / `lib.get_graph_nodes(graph).nodes`，连线用 `pins[].linked_to`（`<guid>.<pin>`）或 `connections`（`{"from","to"}` 两端都是 guid）。

**exec 引脚的连线纪律**：exec **输出**引脚只能有一条连线；`UEdGraphSchema::TryCreateConnection` 回答 `CONNECT_RESPONSE_BREAK_OTHERS_A` 时**只断源引脚**的旧线，**不会断目标输入引脚上的旧线**（`EdGraphSchema.cpp:478-482`）。所以"先连 A→C，再连 B→C"会留下 **C 有两个 exec 来源**：编译能过，但编译结果里 C 走的是其中一条（常常是永不执行的那条）。改接 exec 前先 `disconnect_blueprint_pins` 显式拆掉旧线，并在改完后**回读确认每个 exec 输入只有一条来源**。

### 9. UMG（Widget Blueprint）命令面

| 需求 | 命令 | 备注 |
|---|---|---|
| 建控件资产 | `create_umg_widget_blueprint(name, path?)` | 默认建在 `/Game/Widgets/<name>`；`path` 给内容夹，`name` 传整条资产路径时以它为准。返回 `{"name","path"}`；已存在则报错 |
| 加文本块 | `add_text_block_to_widget(blueprint_name, widget_name, text?, position?)` | 例：`add_text_block_to_widget("/Game/Widgets/WBP_HUD", "AmmoText", "子弹: --", [40,40])` |
| 文本绑定 | `set_text_block_binding(blueprint_name, widget_name, binding_name)` | 建 `FText` 成员变量 `binding_name` + getter 函数 `Get<binding_name>`（入口 → 变量 get → 结果节点），并把该 TextBlock 的 Text 绑到它；**运行时只改这个变量就能改显示** |
| 编译诊断 | `compile_umg_widget(blueprint_name, force_full=true)` / `get_umg_compile_errors(blueprint_name, since?)` | 拿到编译器的 `status` / `messages[]` / **`bindings_dropped[]`**（编辑器有绑定、运行时表没有 = 被引擎静默丢弃）。纪律与字段语义见 UMG skill §六 |
| 加按钮 / 绑事件 | `add_button_to_widget` / `bind_widget_event` | |
| 开设计器 / 设计尺寸 | `open_umg_designer(blueprint_name, focus_widget?)` / `set_umg_design_size(blueprint_name, width?, height?)` | 打开设计器（可选选中控件，实测同步就绪）；设计尺寸只影响设计器预览（`dpi_scale` / `preview_platform` 引擎没有每资产字段，会 `unsupported`）。见 UMG skill §六b |
| 控件动画 | `create_widget_animation` / `add_widget_animation_track` / `set_widget_animation_keyframes` / `list_widget_animations` / `play_widget_animation_preview` / `stop_widget_animation_preview` | 轨道只支持 `RenderOpacity` / `Visibility` / `RenderTransform`，key 的 time 是秒；写完自动编译并回读生成类。见 UMG skill §六b |
| 加进视口 | `add_widget_to_viewport(blueprint_name, z_order?)` | **BREAKING**：语义 = 在**运行中的 PIE 世界**里 `CreateWidget + AddToViewport`，没 PIE → `pie_not_running`（不再只回类路径）。观感由用户在 PIE 视口确认 |

- `set_text_block_binding` 用插件的图 API 建 getter（入口节点来自引擎 `AddFunctionGraph`）；**不要手搓入口节点**——引擎已经建过一个，再搓一个就是"图里两个函数入口"，widget 直接编译不过。
- 文字拼装：`Conv_StringToText` 在 **`KismetTextLibrary`**（`KismetStringLibrary` 里没有）；`Concat_StrStr(A, B)` = A+B，想显示"标签: 数值"就把**标签放 A、数值放 B**（`Conv_IntToString` 得到字符串）。
- 控件树里的 TextBlock 默认**不是**类的可读属性；要断言显示内容，读它绑定的**变量值**（`hud.get_editor_property("AmmoValue")`）最省事。
- UMG 命令各自会保存资产，但**写完仍要回读确认真的在盘上**（`does_asset_exist` + 磁盘 mtime）。

**`verify_blueprint_graph` 的误报与修**（判真假一律直读 `linked_to`，不要据此改图）：
`unreachable_node` 曾把"以 `K2Node_InputKey` / `K2Node_InputAction` / `K2Node_InputTouch` 为根"的执行链判成不可达 ——
入口集只认 `UK2Node_Event`，而它们是 `UK2Node` + `IK2Node_EventNodeInterface`（`K2Node_InputKey.h:35`）。
**已按引擎自己的判据修好**（`KismetCompiler::GatherRootSet`，`KismetCompiler.cpp:104-128`：`FunctionEntry | Event | Timeline | IsNodeRootSet()` + "无输入引脚的非纯 K2Node"），回包另带 `root_node_count` 以区分"干净"与"没有入口"。
`dangling_producer` 对"纯节点上没人用的返回值"是启发式（Tick 的 `DeltaSeconds`、`Montage_Play.ReturnValue` 都会被报）⇒ 按需忽略。

**带输入的行为要在 PIE 里验，三段各管一件事**：
① 直接调节点要调用的那个函数（例：`anim.montage_play(montage)` 回播放时长、下一帧 `montage_is_playing=True`）证明"资产 + 图接对了"；
② 用状态变化驱动下一帧的分支（例：`pawn.add_movement_input(Vector(0,100,0))`，再下一帧 `montage_is_playing=False`）证明"打断链真的跑"；
③ **按键本身可以脚本注入了**：`inject_key(key, event)`（`key` 是键名不是动作名，`event` = press / release / tap）走 `UGameViewportClient::InputKey` —— 真人按键的同一道门，含 `SetIgnoreInput` 那道闸，所以 UI 聚焦/输入被忽略时它同样进不去。它证明的是**键位映射**（动作级注入只证明"动作能到逻辑"，键级注入才证明"绑对了"）；
   **别拿回执当判据**：`viewport_handled` 只是视口返回值，**对轴映射恒为 false**（实测 `W` 返回 false 而 DefaultPawn 照飞）；同帧也**没有**"送到了"的读数 —— 事件在帧内稍后才被处理，调用期间去读按下状态读到的是**本次事件之前**的态（实测：press 帧读到 false、release 帧读到 true，而两次注入都生效）。⇒ **只能回读游戏自己的状态**（`get_actor_pose` / Pawn 位置 / 玩法读数）来证明效果。
   `PlayerController` 上确实没有按键注入 API（`is_input_key_down` / `get_input_*` 一类只是读取）—— 通路在**视口层**，不在 PC 上。手感与观感仍归用户判定。
PIE 世界用 `unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world()` 取
（`start_pie` 起的是真 Play，GameMode 会 spawn 默认 Pawn）。

**写"数组类"引脚：先连线定型、再写值**（UE 5.7 实测）。
`Actor.AddTag` **不是**蓝图可调用函数（`add_blueprint_function_node(target="Actor", function_name="AddTag")` → `function_not_found`；`AddTag` / `K2_AddTag` / `ActorAddTag` 三个名字都不存在），可用的只有 `ActorHasTag`（读）与 `Tags`（`Array[Name]`，Read-Write）。
写 `Tags` 时 **`Set Tags` 节点的默认值不接受数组字面量**（回 `type_mismatch: 'name' pin 'Tags' requires a string value`）：必须先用 `K2Node_MakeArray`，把元素引脚 `[0]` 连到 `Tags`，**等连线就位后再写 `[0]` 的值** —— 连线前 `[0]` 是 wildcard，写值报 `unsupported_pin_type`。

**PIE 探针里 `SetActorLocation` 的 sweep 会在同一次派发内派发重叠**：`sweep=True, teleport=False` 扫进目标位置时，overlap 委托在**本次派发内**就跑完 —— 一次调用扫过 8 枚金币，**同一次回包**里读到的收集状态就是 8/8，不需要跨帧等待。（`teleport=True` 的瞬移不产生重叠，用来摆位很方便。）

**环境坑（省时用）**：
- **编辑器 python 解释器常驻**：`sys.modules` 里缓存的脚本模块**不会**因磁盘改动而刷新 ⇒ 改过的 helper 必须 `importlib.reload`，否则跑的还是旧版（曾因此白跑两轮，且桥侧 `BRIDGE_LOG` 还带着上一轮内容）。
- **编辑器崩溃/重启不会回到崩溃前正在编辑的关卡**（实测回到 `/Game/test`），要用 `LevelEditorSubsystem.load_level()` 显式切回 ⇒ **动手前先把改动存盘**。
- **参数名已统一为一个概念一个名字**：`create_asset_safe` 的资产类参数两层都叫 `asset_class`；`set_object_property` 的对象选择参数两层都叫 `target`（与同族 `reflect_probe` 一致 —— 这两个命令是"先探后写"连着用的）。两个旧名（`asset_class_name` / `object_path`）仍可用作**弃用别名**，用了会在回执里看到 `renamed_params[]`；两个名字同时给且取值不一致 → `invalid_params`（不猜哪个优先）。用引擎内回环直调 bridge 命令时按 **bridge** 名字写。
- `unreal.CollisionResponse` 的枚举成员在 python 侧既不是 `ECR_IGNORE` 也不是 `IGNORE`（实测未命中）⇒ 要逐通道改响应就走 bridge 的 `set_component_collision(responses={"Pawn": "Overlap", ...})`（通道名可省前缀）。

---

## 一、MCP 工具 ↔ 参数速查

| 工具 | 参数 | 返回要点 |
|---|---|---|
| `list_blueprint_graphs` | `blueprint_name` | `graphs[]`：`graph_name` / `graph_class` / `node_count` / `is_editable`（只读） |
| `add_blueprint_function_graph` | `function_name`, `signature_class?` | `graph_name` / `created`（幂等）/ `is_override` / `signature_class` / `entry_node_ids` / `result_node_ids` / `compiled` / `errors` |
| `remove_blueprint_function_graph` | `function_name` | `removed` / `function_graphs`（剩余）/ `compiled`；引擎托管图 → `graph_not_removable` |
| `list_blueprint_function_graphs` | `blueprint_name` | `function_graphs[]`：`graph_name` / `node_count` / `signature_class` / `is_override` / `params[]`（只读） |
| `rename_blueprint_function_graph` | `old_name`, `new_name` | `renamed` / `function_graphs`（改名后）/ `compiled`；占用名 → `graph_name_in_use` |
| `add_blueprint_function_param` | `function_name`, `param_name`, `param_type`, `sub_class?`, `is_output?`, `default_value?` | `params[]`（整个签名回读）/ `param_count` / `compiled` / `errors`；override → `function_not_editable` |
| `remove_blueprint_function_param` | `function_name`, `param_name` | `removed` / `was_output` / `params[]` / `compiled`；不存在 → `param_not_found` |
| `rename_blueprint_function_param` | `function_name`, `old_name`, `new_name` | `old_name` / `new_name` / `params[]` / `compiled` |
| `list_blueprint_variables` / `get_blueprint_variable_info` | `blueprint_name`（+`variable_name`） | `variables[]` / `variable_count`；详情回 `type`/`container`/`sub_class`/`default_value`/`flags{}`（只读） |
| `remove_blueprint_variable` | `variable_name` | `removed` / `variables[]`（剩余）/ `compiled` / `errors`；**同时清掉图里引用它的 get/set 节点** |
| `rename_blueprint_variable` | `old_name`, `new_name` | `variable{}`（回读）/ `reference_nodes_renamed` / `reference_nodes_stale` / `compiled`；占用名 → `variable_name_in_use` |
| `set_blueprint_variable_type` | `variable_name`, `variable_type`, `sub_class?`, `container?` | `variable{}`（回读）/ `type` / `container` / `compiled`；类型语法与 `add_blueprint_variable` 同一份；被图节点引用 → **直接拒**：`variable_referenced_by_nodes` + `referencing_nodes[]` + `hint`（无覆盖开关，见正文）；类型没真正落上时另带 `type_change_not_applied` |
| `set_blueprint_variable_default_value` | `variable_name`, `default_value` | `default_value`（回读）/ `compiled` / `errors`（解析失败在这里现形） |
| `set_blueprint_variable_flags` | `variable_name`, `props={}` | `applied[]` / `failed[]` / `applied_count` / `flags{}` / `compiled` |
| `list_blueprint_local_variables` / `add_blueprint_local_variable` / `remove_blueprint_local_variable` / `rename_blueprint_local_variable` / `set_blueprint_local_variable_default` | `function_name` + 变量名（新增还要 `variable_type`, `sub_class?`, `default_value?`） | `variables[]`（局部）/ `variable_count` / `compiled`；函数不存在 → `function_graph_not_found`+`candidates`；override 函数 → `function_not_editable` |
| `get_blueprint_component_hierarchy` | `blueprint_name` | `components[]`（`component_name`/`component_class`/`component_template`/`parent`/`attach_socket`/`is_root`/`is_inherited`/`children`）/ `root_components` / `root_count` / `unique_root` / `duplicate_components`（只读） |
| `remove_component_from_blueprint` | `component_name`, `recursive=true`, `force=false` | `removed_components[]` / `components[]`（剩余）/ `root_count` / `unique_root` / `compiled` |
| `set_blueprint_root_component` | `component_name` | 整棵层级回读 + `root_components` / `root_count` / `unique_root` / `compiled` |
| `detach_component` | `component_name` | 同上（组件不删，只挂回 SCS 根） |
| `set_component_collision` | `component_name`, `props={}` | `applied[]` / `failed[]` / `collision{collision_enabled,collision_profile,object_type,responses}`（回读）/ `compiled` / `placed_instances` / `counted_in` / `hint`（有实例时） |
| `implement_blueprint_interface` | `interface_path` | `implemented` / `already_implemented` / `generated_graphs[]` / `interface{}` / `compiled` |
| `unimplement_blueprint_interface` | `interface_path`, `preserve_functions=false` | `removed` / `removed_graphs[]` / `interfaces[]`（剩余）/ `compiled` |
| `list_blueprint_interfaces` | `blueprint_name` | `interfaces[]`（`interface_path` + `functions[{function_name,implemented}]`）/ `interface_count`（只读） |
| `list_overridable_functions` | `blueprint_name`, `include_parent_events=true` | `functions[]`（`function_name`/`declaring_class`/`signature_class`/`source`/`overridden`）/ `function_count` / `overridden_count`（只读） |
| `check_blueprint_element` | `kind`, `name` | `found` + 定位字段；**`found:false` 是正常返回**，非法 `kind` → `invalid_params`+`candidates`（只读） |
| `compare_blueprints` | `blueprint_a`, `blueprint_b` | `variables`/`functions`/`components`/`interfaces` 四项 `only_in_a`/`only_in_b`/`changed` + `sides{a,b}`（含 `parent_class`）（只读） |
| `move_blueprint_node` / `refresh_blueprint_node` | `node_id`, `position`（移动）/ `graph_name?` | `pos_x`/`pos_y` 回读；刷新回 `readback[]`（引脚）+ `node_id_before`/`node_id_changed` |
| `split_blueprint_pin` / `recombine_blueprint_pin` | `node_id`, `pin_name`, `graph_name?` | `sub_pins[]` / `sub_pin_count`（拆后非空、合后为空）+ `readback[]`；不可拆 → `pin_not_splittable` |
| `add_blueprint_custom_event_node` | `event_name`, `params=[{name,type,sub_class?}]`, `node_position?`, `graph_name?` | `node_id` / `event_name` / `params[]` / `param_count` / `compiled`；重名 → `custom_event_name_in_use`+`candidates` |
| `rename_blueprint_custom_event` | `node_id`, `new_name`, `graph_name?` | `event_name`（回读）/ `params[]` / `node_id` / `node_id_changed` |
| `open_blueprint_graph` / `focus_blueprint_node` | `graph_name?` / `node_id`, `graph_name?` | `opened` / `focused`；三态：`blueprint_not_found` / `graph_not_found`+`available_graphs` / `editor_not_open` |
| `find_blueprint_nodes` | `blueprint_name`, `node_type="All"\|"Event"`, `event_type?`, `graph_name?` | `graph`/`graph_name`/`count`/`node_count`/`nodes`（`node_ids` **仅** `node_type="Event"` 才有，All 模式省略该字段）；节点含 `properties`（类型化：数字是数字、结构体是数组）、`pins`（`pin_name`/`direction`/`category`/`default_value`/`default_object`/`default_text_value`（仅 text 引脚非空时）/`b_hidden`/`b_not_connectable`/`connected`/`linked_to`）、`connections`（只从 output 侧生成）|
| `add_blueprint_function_node` | `target`, `function_name`, `params={}`, `node_position`, `graph_name?` | `node_id`/`graph_name`/`node_count`/`applied`/`failed`/`readback` |
| `add_blueprint_event_node` | `event_name`（`Receive*`）, `node_position`, `graph_name?` | 同上 |
| `add_blueprint_input_action_node` | `action_name`, `node_position`, `graph_name?` | 同上 |
| `add_blueprint_literal_node` | `literal_type`, `value`, `node_position`, `graph_name?` | 同上 + `value` 回读 |
| `add_blueprint_node_by_class` | `node_class`, `node_position`, `graph_name?` | 同上 + `node_class` |
| `add_blueprint_variable` | `variable_name`, `variable_type`, `is_exposed?`, `default_value?`, `sub_class?` | `variable_name`/`variable_type`/`pin_category`/`container_type`/`is_exposed`（实例可编辑）/`class_editable`（类默认值可见） |
| `add_blueprint_variable_node` | `variable_name`, `node_kind="get"\|"set"`, `node_position`, `graph_name?`, `is_local=false`, `variable_type?`, `sub_class?`, `default_value?` | `node_id`/`variable_name`/`node_kind`/`is_local`/`readback` |
| `add_blueprint_get_self_component_reference` | `component_name`, `node_position`, `graph_name?` | 同建节点（`node_id`/`readback`） |
| `add_blueprint_self_reference` | `node_position`, `graph_name?` | 同上 |
| `connect_blueprint_nodes` | `source_node_id`, `source_pin`, `target_node_id`, `target_pin`, `graph_name?` | 解析后的 `source_linked_to`/`target_linked_from` + `displaced_links[]` + `verified`/`src_pin_consumers`/`tgt_pin_prev_source`（写后双向回读）。**`displaced_links` 是被引擎顶掉的旧线**（单连接引脚，通常断的是**源**侧）：非空即"接上了但代价是别处断了一根"，此时 `verified=false`、并附 `hint` ⇒ 要扇出用 Sequence 节点，别重复接同一个输出 |
| `set_blueprint_pin_default` | `node_id`, `pin_name`, `value`, `graph_name?` | `default_value_after` + `readback` |
| `set_blueprint_node_property` | `node_id`, `property_name`, `property_value`, `graph_name?` | `property_type`/`before`/`after`/`pins_rebuilt`（+重建时 `readback`） |
| `disconnect_blueprint_pins` | `node_id`, `pin_name`, `linked_node_id?`, `linked_pin_name?`, `graph_name?` | `links_before`/`disconnected_count`/`remaining_links` |
| `delete_blueprint_nodes` | `node_ids=[...]`, `graph_name?`, `force=false` | `deleted`/`failed`（逐 guid）/`deleted_count`/`remaining_nodes`；函数 Entry/Result 与 AnimGraph Root/StateMachine **默认拒绝**（`failed[]` 里 `error_code=protected_node`），要删必须显式 `force=true`，此时响应带 `structure_after` 结构回读 |
| `verify_blueprint_graph` | `blueprint_name`, `graph_name?`, `rules?`, `max_issues=200` | 只读自检：`issues`（`rule`/`severity`/`node_id`/`pin_name`/`detail`/修复提示字段）/`issue_count`/`rules_run`/`truncated`；规则见 §一 的验收纪律 |
| `compile_blueprint` | `blueprint_name` | `status`/`compiled`/`errors`/`warnings`；`BS_Error` 且 `errors` 为空时另带 `skeleton_hint`（函数图结构诊断，不是编译器消息） |
| `create_blueprint` / `add_component_to_blueprint` / `set_component_property` / `set_static_mesh_properties` / `set_physics_properties` / `set_blueprint_property` / `attach_component_to_component` | 见 §5 与上文；`component_type` 不带 `U` 前缀，`location/rotation/scale` 必须 3 元素 | 组件类命令回 `component_name`/`component_class`/`component_template`，属性类命令回 `property_value_before`/`after` |
| `blueprint_graph_reflection_help` | 无（本地返回） | 反射库函数清单 + 对应 MCP 工具清单 |

---

## 二、python 反射库 `unreal.UnrealMCPBlueprintGraphLibrary`

全部 `GameThread-only`，全部返回 `USTRUCT` 信封（`.success` / `.error_code` / `.error_message` / `.candidates`），**没有异常**。

| 调用 | 返回结构 | 说明 |
|---|---|---|
| `get_graphs(bp)` | `graph_array_result.graphs[]` | 每项含 `graph_name`/`graph_class`/`node_count`/`is_editable`/`graph`（活对象） |
| `get_graph_nodes(graph)` | `node_array_result.nodes[]` | 只读、不建图 |
| `get_node_pins(node)` | `list[pin_info]` | **无错误通道**（坏节点给空表） |
| `get_node_info(node)` | `node_info` | 节点 + `properties` + `pins` |
| `get_node_properties(node)` | `list[node_property_info]`（`name`/`type`/`value`） | 无错误通道 |
| `add_function_call_node(graph, target_class, function_name, x, y)` | `node_op_result` | `.node` + `.readback` |
| `add_event_node(graph, event_name, x, y)` | `node_op_result` | |
| `add_variable_get_node(graph, variable_name, x, y)` / `add_variable_set_node(...)` | `node_op_result` | |
| `add_literal_node(graph, literal_type, value, x, y)` | `node_op_result` | `.value_after` = 字面量回读 |
| `add_node_by_class(graph, node_class, x, y)` | `node_op_result` | |
| `add_self_reference_node(graph, x, y)` / `add_input_action_node(graph, action_name, x, y)` | `node_op_result` | |
| `connect_pins(source_node, source_pin, target_node, target_pin)` | `graph_op_result` | `.candidates` 回**真实端点**，可能被 schema 改写 |
| `set_pin_default(node, pin_name, value, value_kind)` | `graph_op_result` | `.value_after` = 引脚回读 |
| `set_node_property(node, property_name, value, value_kind)` | `node_property_op_result` | `property_type` / `value_before` / `value_after` / `pins_rebuilt` / `pins`（仅重建时填） |
| `disconnect_pin(node, pin_name, linked_node_id, linked_pin_name)` | `graph_op_result` | `.candidates` 里是 `disconnected=N` / `remaining=M` |
| `delete_node(node)` | `graph_op_result` | `.value_after` = 被删 guid；`.candidates` = `remaining_nodes=N` |
| `ensure_event_graph(bp)` | `graph_array_result` | **可能建图**；`.graphs[0]` |
| `find_graph(bp, graph_name)` | `graph_array_result` | 不建图；歧义/缺失 → `graph_not_found` |
| `add_function_graph(bp, graph_name)` | `graph_array_result` | |
| `compile_blueprint_checked(bp)` | `compile_result` | `status`/`compiled`/`errors`/`warnings`；会保存资产 |
| `begin_transaction(description)` / `end_transaction()` | `void` | 让脚本内的图操作可被 Ctrl+Z 撤销 |
| `get_supported_variable_types()` / `get_supported_literal_types()` | `list[str]` | 无错误通道 |

`ValueKind`（`set_pin_default` / `set_node_property`）：`auto`（默认，按形状判 number/bool/string）、`number`、`bool`、`string`、`name`、`text`、`enum`、`vector`、`vector2d`、`linear_color`、`object`、`class`。解析失败 → `invalid_value`，消息形如 `Property '<name>': <原因>`。

常用 struct 字段（python 侧 snake_case）：`pin_info` = `pin_name` / `direction`("input"/"output") / `category` / `default_value` / `default_object` / `connected` / `linked_to`；`node_info` = `node_id` / `name` / `type` / `graph_name` / `pos_x` / `pos_y` / `properties` / `pins` / `node`；`node_property_info` = `name` / `type` / `value`。

---

## 三、验证纪律

每一步之后**断言实际状态**，不要沿用"上一步应该成功了"。

### 0. 验收脚本骨架（`Content/Python/scripts/blueprints/*.py` 的既有范式）

```python
import json, time, unreal

TEST_BLUEPRINT = "MCPGraphTest"
BP_PATH = "/Game/Blueprints/" + TEST_BLUEPRINT
RUN_TAG = str(int(time.time()))[-5:]          # 唯一命名，避免引擎静默加后缀
FAILURES = []

def bridge(command, **params):
    return json.loads(unreal.UnrealMCPPythonAPI.execute_mcp_command(command, json.dumps(params)))

def check(condition, message, extra=None):
    if condition:
        print("PASS: " + message)
    else:
        text = "FAIL: " + message
        if extra is not None:
            text += " | " + json.dumps(extra, default=str)[:600]      # 证据截断
        print(text); FAILURES.append(message)

def main():
    ...                                        # 断言集中收集，不中途 abort
    if FAILURES:
        raise AssertionError("%d check(s) failed: %s" % (len(FAILURES), FAILURES))
    print("ALL CHECKS PASSED")

main()
```

运行方式：`execute_python_file(file_path=<绝对路径>, deferred=True)` → `poll_python_job(job_id)`。

**脚本里的 bridge 用信封形式** `{"type": command, "params": params}`：它是无歧义的（解包只看 `type`）。裸载荷（直接把参数对象传进去）也能用，但**自带 `params` 字段的命令**（如 `add_blueprint_function_node` 的引脚默认值表）在裸载荷下会把整个载荷替换掉，表现成"明明传了 blueprint_name 却报缺 blueprint_name" —— 脚本里一律用信封形式。

### 1. 断言什么（都是"直读"而不是"回显"）

| 目标 | 手段 |
|---|---|
| 交叉验证两条入口看到同一个节点/同一份属性 | `lib.get_node_info(node).node_id` 能在 `find_blueprint_nodes` 结果里找到；`len(node_info.properties) == len(lib.get_node_properties(node))` |
| 两条入口的错误码与候选一致 | 命令入口与 `lib.*` 同一操作，断言 `error_code` **和** `candidates` 都相等 |
| 写入真的生效 | 用 `default_value_after` / `property_value_after` / `value_after` |
| 引脚默认值在 `ReconstructNode` 后存活 | 先写引脚默认值 → 再写会重建的属性（`pins_rebuilt is True`）→ 重读该引脚仍等值 |
| `pins_rebuilt` 语义正确 | 视觉属性（`FontSize`/`NodeComment`）必须 `False`；会改布局的属性必须 `True`，且用返回的新引脚/新 guid |
| 序列化类型没退化 | `FontSize` 是 number、`CommentColor` 是 list；每个节点都**带 `properties`**（该字段曾被弄丢过，作回归守卫） |
| 寻址类错误可自纠 | `graph_not_found` 带 `available_graphs`、`pin_not_found` 带可用引脚、`unknown_property` 带 Did-you-mean |
| 组件层级是 SCS 真实状态 | 断 `attach_parent` / `attach_socket` / `is_root`（服务端实读），并 `len(names) == len(set(names))` 证明无重复登记 |
| 失败不留残骸 | 非法字面量/非法值之后节点数不变（`baseline` 比对） |
| 收尾干净 | 删掉本次创建的节点/组件后，节点数回到 `baseline` |
| 编译结论 | `compiled is True` + 读 `warnings` 数组判零警告 |

### 2. 纪律条目

1. **唯一命名**：需要断言"没有重名/没有重复登记"时，名字必须带 `RUN_TAG`，否则引擎自动加后缀会让断言失去意义。
2. **不依赖 python 读不到的东西**：`get_editor_property` 只暴露 `CPF_Edit` 属性（`AttachParent` / `NodeComment` 等都读不到），要断言这类状态就依赖**服务端从活对象实读回读**的字段。
3. **guid 当次有效**：任何 `pins_rebuilt` 或重建之后，旧 guid 一律作废，重新读。
4. **先读后写**：写命令前先 `list_blueprint_graphs` / `find_blueprint_nodes` 确认目标存在；不要靠猜名字或猜 `graph_name`。
5. **报错即自纠**：拿 `error_code` + `candidates`（`available_graphs` / 可用引脚 / Did-you-mean / `supported_types`）改参数，不要重试同一个参数。
6. **环境相关路径用 `skip` 而非 FAIL**：例如枚举资产可能解析不到时，标跳过而不是判失败。
7. **只读命令可反复调**：`list_blueprint_graphs` / `find_blueprint_nodes` 不进撤销栈、不写盘、不建图，可放心用于断言。

---

## 四、批量写的落盘纪律（`persist`）

本域写命令**成功即落盘**（注册表里带落盘标志）：一次调用 = 一次整包写盘。批量图编辑（同一条资产上几十次写）
会把"命令条数"变成同样次数的盘 IO —— 编辑器长时间无响应，而且同步 job 期间整条 MCP 通道被占住、没有进度也不能取消。

1. **批内写显式 `persist=false`**：同一条 Blueprint 的多条写命令（节点 / 引脚 / 变量 / 组件 / 函数图）都接受这个可选参数；
   默认 `true` 与旧行为一致，传 `false` 时编辑只留在内存与撤销栈里、**不写盘**。
2. **回执自证**：每条回执带 `saved`（本次是否真的落盘）与 `persist_requested`（你要的是什么）；
   `persist=false` 时 `saved` 必为 `false`。不要拿"没报错"当"已保存"。
3. **结束 flush 一次**：批内写完后调用一次 `unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)`
   （World Partition 里 actor 的改动在 external actor 包，也只有这个会落）。在此之前**不要**重启或强杀编辑器，否则这批改动丢失。
4. **"没落盘" ≠ "没生效"**：结构编辑在内存里立即可见、可编译、可回读，只是磁盘上没有。
5. **`create_blueprint(persist=false)` 的边界**：资产只在内存里，同会话按名查找仍找得到，但**按路径加载**（别的会话/进程）找不到 ——
   批量建资产时最后必须 flush。
6. **没有该参数的命令**：`move_asset` / `move_directory` / `safe_delete_asset` 这类，落盘是操作语义的一部分
   （引用者重存、删前先落盘），不要指望能关掉。
7. **`persist=false` 的语义是"这次派发里的所有落盘都关掉"，重建与编译也一样**（已修正）：
   派发时注册表按 `persist` 建一次作用域（`FMCPPersistScope`），插件内部那几个"自己顺手存一下"的点
   （节点写回执 `MakeWriteResult`、图库 `GuardPersist` / `CompileBlueprintChecked`、`compile_blueprint` 编译后的保存、
   粒子 `PersistSystem`）都读同一标志。所以 `set_blueprint_node_property` / `add_blueprint_node_by_class` /
   `connect_blueprint_nodes` / `compile_blueprint` 传 `persist=false` 时**一次都不写盘**，重建引脚只影响内存与撤销栈。
   实测（当时的夹具脚本已随会话清掉，判据与数字如下）：加节点 / 编译的 `persist=false` 为 **0/3**（修正前 3/3），
   `persist=true` 仍 1 次/条，批末 flush 恰好 1 次。
   > 早前"重建会引发引擎自动保存、命令层拦不住"的结论是错的：那批存盘来自插件自己的帮忙保存。引擎侧
   > 仅有两处编译后保存，且都由 `UBlueprintEditorSettings::SaveOnCompile` 门控（本机为 `SoC_Never`，未触发）。
8. **改 AnimGraph 节点的数据（`FAnimNode`）**：标量与数组要走不同通道，而且**写完必须逐字段读回**。

   - **标量字段**（阻尼、迭代次数、`GravityScale`、`BoundBone`/`ChainEnd` 这类简单成员）两种写法都行：
     `nd.get_editor_property('node')` → 改 → `nd.set_editor_property('node', s)`（不重建、不重编译、不落盘），
     或走命令 `set_blueprint_node_property(property_name='Node', …)`；
   - **数组 / 子结构字段**（`PhysicsBodyDefinitions` 等）**必须走点号路径命令**，并且用**引擎属性原名**：

     ```
     set_blueprint_node_property(blueprint_name=…, node_id=…, graph_name='AnimGraph',
                                 property_name='Node.PhysicsBodyDefinitions',
                                 property_value=[{…}, {…}], persist=False)
     ```

     snake_case（`node.physics_body_definitions`）会被 `path_segment_not_found` 拒；
     **整坨写整个 `Node` 不可靠**（字段改名 / 含不可写字段 / 子对象加载失败，任一条都会打断整次写入）
     ⇒ 一律走点号路径写子字段。
   - **读回是唯一的验收**：`set_editor_property` 对"数组 of 结构"是**静默无效**的（写 7.5 读回 10，不抛异常），
     所以写完一律用 `reflect_probe` 逐字段比对，别信"没报错"。
   - 结构里 `component_pose`（引脚链路 LinkID）**必须跳过**，跨图复制会破坏接线；
     确实改了引脚/函数引用的属性仍要回落到 writer（它会重建并重编译，落盘由 `persist` 控制）。
9. **AnimDynamics 的 per-body 定义是否生效，由 `PhysicsBodyDefinitions[i].BoundBone.BoneName` 是否被回填决定**
   （`AnimNode_AnimDynamics.cpp:705` 触发条件 → `:523-558` 重建；回填发生在
   `UAnimGraphNode_AnimDynamics::PostEditChangeProperty` → `ValidateChainPhysicsBodyDefinitions`，`:210`）：

   - 名字为 `NAME_None` ⇒ 每帧初始化把**整表重建为默认值**（`BoxExtents(10,10,10)` / 半径 10 / 约束全 0），**作者值被丢弃**；
   - 名字已被回填（任何一次走编辑器/命令通道的属性写都会回填）⇒ 使用**作者值**。

   ⇒ **同一份参数会在两种行为之间摇摆**。对比两个资产的物理前，先比这一列。
   **脚本化的回填方法**（不必在编辑器里手点）：用 `set_object_property` 对该节点写一个**无害的同值属性**，
   例如 `property_name="Node.GravityScale", property_value=<当前值>` —— `set_object_property` 每次成功写入都会调
   `PostEditChangeProperty`，落到 `else` 分支执行 `ValidateChainPhysicsBodyDefinitions`，由引擎按链回填名字；
   随后 `node.BoundBone`/`ChainEnd` 与 body 首/末一致，运行时不再重建，**蓝图里的 per-body 参数就是生效值**。
   （想反过来"让重建兜底"：把名字留空即可，但任何一次属性写都会把它填回去。）
10. **输入接线与 Cast 节点的两个必答项（`BP_GrabDriver` 实测）**

   - **非 possessed 的 actor 收不到输入**：输入只路由给"被 possess 的 pawn"的输入栈。关卡里摆着的角色/工具 actor
     若没被 possess（例：`L_GrabTest` 里被 possess 的是 `DefaultPawn` 自由飞行相机），必须自己调
     `Enable Input(PlayerController)`（`Actor.EnableInput`，K2Node_CallFunction）才会收到键/鼠标事件 ——
     这就是 C++ 侧"组件自己 `PushInputComponent`"在蓝图里的等价物，漏了它表现为"节点连得对、PIE 里毫无反应"。
     判据：PIE 里 `GetPlayerPawn(0)` 是不是它；不是就必须 `Enable Input`。
   - **反过来，玩家 pawn 的输入还在生效**：非 possessed 场景里 possess 的仍是玩家 pawn，它的 look 绑定会继续吃鼠标
     —— 表现为"按住左键拖光标，视角一起转"。光标驱动的交互必须在驱动侧关掉 look：
     `Set Ignore Look Input(PC, true)`（`Controller.SetIgnoreLookInput`），要转视角时再 `Reset Ignore Look Input(PC)`。
     实测：这一条 + `Enable Input` 一起才是"组件自读鼠标"时代的等价物（旧 PhysicsGrab 组件正是 `SetIgnoreLookInput(true)`
     + 按住右键才放开）。
   - **`add_blueprint_node_by_class(K2Node_DynamicCast)` 之后必须 `refresh_blueprint_node`**：它是骨架节点
     （起始名为"坏的类型转换节点"），`set_blueprint_node_property(property_name="TargetType", …)` 只写属性、
     **不重建引脚**（回执 `pins_rebuilt: false`，也没有 `As…` 输出引脚）；对节点点一次 `refresh_blueprint_node`
     才会出现 `As<类名>`（实测名字是 `AsPhysics Grab`，不是 `AsPhysicsGrabComponent`），并顺手重编译。
   - 顺带：`GameplayStatics.GetPlayerController` 在 UE5 是**纯节点**（无 exec 引脚），接线时不要从它往下串 exec。
   - **数据引脚连了 ≠ 会执行**：带结构体 out 参数的调用链（例：`Read Cursor Drag Info` 的 `OutDragInfo → Apply Drag Info.DragInfo`）很容易只连数据、漏掉 `Read.then → Apply.execute`。漏 exec 时**编译不报错、运行期不警告**（没有 Accessed None），表现为"事件明明触发了但下游毫无动静"。
     定位法：在两个节点之间插 `PrintString` 打点（先 `add_blueprint_function_node(target="KismetSystemLibrary", function_name="PrintString")`，再用 `set_blueprint_pin_default(pin_name="InString", value="XXX")` 写文本（**creation 时的 `params` 也会写入并持久** —— 7 例实测全过，含 `PrintString.InString` 与 `K2_SetTimer` 的三个参数；**但写完必须回读**，不回读就分不清"根本没设"与"设了没生效"，`InString` 的默认值是 `Hello`），配合同一条链上的 C++ 侧日志/状态读数比对，缺口一定能看见）。
   - **接线完成后逐条验 exec 链**：对每条事件链，用 `find_blueprint_nodes` 的 readback 检查"上一步的 `then` 是否在下一步的 `execute` 的 `linked_from` 里"。**并且当场看 `connect_blueprint_nodes` 的 `displaced_links[]`** —— 引擎顶掉旧线是静默的（曾表现为事后 `verify_blueprint_graph` 报 4 个 `unreachable_node`），现在那一刻就会进回执；非空即"新接的这根把别处顶断了"，按 `hint` 用 Sequence 节点重做。
   - 收尾跑一遍 `verify_blueprint_graph`：误报判读见本文件"`verify_blueprint_graph` 的误报与修"一节；报告规范见 `## 三、验证纪律`。
11. **图必须可读（交付纪律，用户明确要求）**：节点**不要堆在原点附近**。约定：

   - **一条事件链一行**：`y` 按链分行（BeginPlay / 每个按键 / Tick 各一行），`x` 沿 exec 流递增（每步 ~300–400）；
   - **共享的纯节点单独一列**（例：`Get PC` / `Get Grab` 被多行复用 ⇒ 放同一列便于看"谁在喂谁"）；
   - **每行加一个 `EdGraphNode_Comment`** 标注"这一行是干什么的"（`add_blueprint_node_by_class(node_class="EdGraphNode_Comment")` → `set_blueprint_node_property(NodeComment/NodeWidth/NodeHeight)` → `move_blueprint_node` 摆到该行上方）；
   - **`add_blueprint_function_node` / `add_blueprint_variable_node` 的 `node_position` 实测不生效**（节点仍落在 0,0；只有 `add_blueprint_event_node` 生效）⇒ 建完统一用 `move_blueprint_node` 收口，并**回读 `pos_x`/`pos_y` 自证**；
   - 布局是要交给人看的：一次把整张图摆好（脚本里成批 `move_blueprint_node`），不要留一张"点状云"。

---

> **这一节已拆成独立 skill**：`unreal-blueprint-case-bp-to-cpp-migration`（把蓝图逻辑搬进 C++：reparent 的硬约束、会丢什么、怎么验）。

## 本 skill 的 `scripts/`（只读探查工具；改顶部 `BP` 路径即可重跑）

| 脚本 | 做什么 |
|---|---|
| `fei_740_bp_survey.py` | 一次看清一个 BP：图表清单 / 变量 / 函数图 / 组件层级 / 每张图的节点（含 `function_reference` 与节点位置） |
| `fei_741_bp_graph_detail.py` | 单张图的细节 dump：每个节点的引脚（默认值 / 对象 / 连线目标）与节点属性 |
| `fei_601_dump_graphs.py` | 遍历一个 BP 的所有图逐图 dump 节点（走反射库 `lib.get_graph_nodes`） |
