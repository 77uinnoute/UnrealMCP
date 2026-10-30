---
name: unreal-umg-authoring
description: "UE5（UnrealMCP）里用 MCP 工具 + python 脚本程序化建/改 UMG 控件蓝图（Widget Blueprint）的方法：控件只能由 add_widget 经 WidgetTree::ConstructWidget 创建（python 侧 unreal.WidgetTree 不存在，NewObject 造的控件不归树）、控件树与槽的寻址与命名纪律（树内名唯一、控件名=变量名）、set_widget_properties 的键表与反射兜底、set_widget_slot 的 slot 对象与每类槽的可写字段、结构编辑（remove / reparent / reorder / rename / set_root_widget）的引用检查与清理链、persist 落盘与回读验收纪律。触发场景：准备调用以下任一 MCP 工具前 MUST 加载本 Skill —— create_umg_widget_blueprint / get_widget_tree / add_widget / set_widget_properties / set_widget_slot / add_text_block_to_widget / add_button_to_widget / bind_widget_event / set_text_block_binding / set_widget_property_binding / unbind_widget_property / prune_widget_bindings / remove_widget / reparent_widget / reorder_widget / rename_widget / set_root_widget / add_widget_to_viewport；以及任何涉及「用 MCP 建/改 UE 控件蓝图、HUD 与菜单、控件树层级与子件顺序（绘制序）、VerticalBox/HorizontalBox/Overlay/GridPanel/ScrollBox/SizeBox/Border/Image/ProgressBar/Slider/CheckBox/EditableText 控件、Canvas 槽 anchors/alignment/position/size、非 canvas 槽 padding 与对齐、控件属性（文本/颜色/字号/brush/tint/percent/visibility/render_transform）、控件变量与属性绑定、删控件后绑定与 getter 的清理、UMG 验收脚本」的任务。"
metadata:
  version: "1.0.0"
  upstream: ~
  downstream: ~
---

# UnrealMCP UMG 控件编写 Skill

适用：本仓库 `Plugins/UnrealMCP`（bridge `127.0.0.1:55557`，MCP server 名 `unrealMCP`）。
目标：用 MCP 工具 + python 脚本**可重复、可验证**地建/改控件蓝图：造控件、改控件树、写属性与槽、删/搬/改名。

**环境注意**：改 `Plugins/UnrealMCP/Content/Python/**`（含工具描述）后需**重启 unrealMCP server**；改插件 C++ 则需编译 + 重启编辑器（`Build_UnrealMCP.bat` → `Editor.bat start`）。

---

## 〇、能力边界：哪些必须走命令，哪些 python 也行

| 事 | 只有命令能做 | python 本来也能做 |
|---|---|---|
| 造控件 | ✅ `add_widget` —— `UWidgetTree::ConstructWidget<T>` 是 C++ 模板，`python_api_index("WidgetTree")` → `unknown_class`；`new_object` 造的控件不归树、不注册变量、重启即失 | — |
| 读控件树 | ✅ `get_widget_tree`（资产侧）—— 一次读全：绘制序、每类槽展开字段、绑定/事件、`effective`/`compiled`。python 只能按对象路径逐个取（`…:WidgetTree` 或 `…:WidgetTree.<控件名>`，见 §四a），没有"一次读全"的入口 | 单个控件的自身属性/槽字段（按对象路径） |
| 读**运行时**控件树（PIE） | ✅ `get_pie_widget_tree` —— 实例的控件树、当前文本、几何、有效可见性都只有 C++ 拿得到 | 拿到 `instance_path` 后可以用 `unreal.load_object` 取到**实例对象本身**并 `call_method` 调它的蓝图函数（见 `unreal-umg-case-list-grid-selection` 的“运行时怎么取证”那节） |
| 改树结构（删/换父/调序/换根/改名） | ✅ `remove_widget` / `reparent_widget` / `reorder_widget` / `set_root_widget` / `rename_widget` —— 没有树对象，且要连带清理绑定与变量 | — |
| 绑定 | ✅ `set_text_block_binding` / `bind_widget_event` —— `Bindings` 在 python 侧不可见（生成类的 `Bindings` 是 protected） | — |
| 写控件属性 | 命令更省事（统一寻址 + `applied[]/failed[]` + 落盘） | ✅ `TextBlock.set_font / set_color_and_opacity / justification(Read-Write 属性)`、`Image.set_brush`、`Slider.set_value`、`CheckBox.set_checked_state`、`ProgressBar.set_percent`、`Widget.set_visibility / set_is_enabled / set_render_transform` |
| 写槽 | `set_widget_slot` 覆盖 canvas / vbox / hbox / scroll / overlay / sizebox / grid / **scalebox（只有对齐，见 §四）** | ✅ 其余槽（`WrapBoxSlot` / `UniformGridSlot` / `WidgetSwitcherSlot`）python 直接写：`unreal.load_object(None, "<包>.<名>:WidgetTree.<控件名>").get_editor_property("slot")` → `set_editor_property(...)`，写法与字段名见 §四a |

结论：**造控件、改结构、读树、绑定**是命令的硬边界；属性与槽两条路都通，选命令是为了同一套寻址/错误/落盘语义。

---

## 一、寻址与命名纪律

- **控件名是唯一键**：`WidgetTree->FindWidget(FName)` 按名查；树内重名不存在（`add_widget` 重名 → `name_collision`，不静默改名）。控件名 = 控件变量名 = 属性绑定里的 `ObjectName`，改名必须用 `rename_widget`（它同时改对象名、变量名、`Bindings`、导航绑定与图内引用）。
- `blueprint_name` 接受名字或路径（`/Game/UI/WBP_HUD`）；`get_widget_tree` 返回的层级就是 UMG 的**绘制序**（`children` 顺序 = 子件顺序 = 先画的在前）。
- 造控件要 `as_variable=true` 才成为蓝图变量（缺省 false，与设计器一致）——变量是"能被事件图/绑定引用"的前提。
- `parent_widget` 缺省 = **根面板**；树里没有根时，新控件会成为根（`became_root: true`）。根控件**没有槽**（`slot` 字段回缺省即表示"它是根"）。
- 结构性概念对照：`UPanelWidget::AddChild` 加在末尾；`reorder_widget(index)` 用 `ShiftChild` 重排；`reparent_widget` 会**换槽类**（`CanvasPanelSlot` ↔ `VerticalBoxSlot` …）并迁移可迁移字段。

## 二、建控件（`add_widget`）固定顺序

```
1. 想清父面板：parent_widget（缺省根面板）
2. add_widget(widget_class="ProgressBar", widget_name="HpBar", parent_widget="VB", as_variable=True)
3. 回读断言：widget_class / parent_widget / is_variable / widget_count / compiled
4. 再写槽（set_widget_slot）与属性（set_widget_properties）—— 它们是分开的两件事
```

- `widget_class` 写法三选一：python 类名（`ProgressBar`）、C++ 名（`UProgressBar`）、资产路径（`/Game/MCP/UmgProbe/WBP_Child` → 实例化成嵌套 UserWidget，回读 `WBP_Child_C`）。未知类 → `unknown_widget_class` + `candidates`（候选是原生控件类清单）。
- 单子面板（`Border` / `SizeBox` …）已满时 → `unsupported_parent`，且命令会把半成品控件丢掉（不留孤儿）。
- 常用类：`CanvasPanel` / `VerticalBox` / `HorizontalBox` / `Overlay` / `GridPanel` / `ScrollBox` / `SizeBox` / `Border` / `WrapBox` / `Image` / `TextBlock` / `Button` / `ProgressBar` / `Slider` / `CheckBox` / `EditableText` / `Spacer`。
- **建资产时就能选父类**：`create_umg_widget_blueprint(name, path?, parent_class?)` —— 原生类名（默认 `UserWidget`，它自己抽象但可以被继承，不是错误）或**某个控件蓝图的资产路径**（项目里最常见的"继承自己的 base widget"）。传错 → `invalid_parent_class`；原生候选清单对父类通常为空（抽象的 `UserWidget` 被过滤、具体的 user widget 全是蓝图），所以拒绝时给的是 `hint`：改传资产路径。
- **根控件也能选（2026-10-15 起）**：`root_widget_class` —— 任意**具体**控件类（`SizeBox` / `Border` / `VerticalBox` / `Overlay` / `ScrollBox` …），或 `"none"`/`"empty"`/`"null"` 留**空树**（之后第一个 `add_widget` 自己当根，回包 `became_root: true`）。默认 `CanvasPanel`，与编辑器新建时一致，**现有脚本不受影响**。回包给 `has_root` + `root_widget_name` / `root_widget_class` 自证；传错 → `invalid_root_widget_class`（+ `root_widget_classes`），传抽象类（`PanelWidget` / `Widget`）→ `abstract_root_widget_class`。
  - **为什么需要它**：`UCanvasPanel` 是**无布局容器**，按锚点/偏移摆子件、**不声明内在尺寸** ⇒ 根是画布时，这个 user widget 的 desired size = 0，**放进任何按内容量尺寸的父（`HorizontalBox`/`VerticalBox`/自动 SizeBox）都会量成 0×0**（实测：卡片被量成 0，纸底与内容全塌，只剩溢出的小字）。根是 `SizeBox`/`Border`/`Box` 就有尺寸。三种解法任选：① 建资产时 `root_widget_class="SizeBox"`（最直接）；② 放它的**父**侧包一层 SizeBox 或给槽尺寸（我给 `WBP_FTF_Toolbar` 的卡片就是这么绕的）；③ 不嵌套 user widget，把子件直接建在父树的对应面板里。
  - 换根（`set_root_widget`）解决不了这个：它把**旧根塞进新根当子件**（于是 SizeBox 里还留着一层画布），而且单子件面板已有子件时会被拒。
- **控件是否暴露为变量**：创建时用 `add_widget(as_variable=…)`；**已有的控件**用 `set_widget_variable(blueprint_name, widget_name, as_variable)`（`bIsVariable` 没开给反射，`set_widget_properties` 改不了）。命令会编译并回读生成类属性，不符 → `variable_not_effective`；请求的状态与现状相同 → `changed: false`（不写、不谎报）。

## 三、写属性（`set_widget_properties`）

- 入参是 `props` 对象，**逐键独立**：`applied[]` 里每项带 `key` / `property` / `value_before` / `value_after`（真回读），失败的进 `failed[]` 带 `error` / `message` / `candidates`。**一项失败不影响其余项**，所以一次调用写一组属性最省往返。
- **通用键**：`visibility`（`Visible` / `Collapsed` / `Hidden` / `HitTestInvisible` / `SelfHitTestInvisible`）、`is_enabled`、`tooltip`、`render_opacity`、`render_transform`（`{"translation":[x,y], "scale":[x,y], "shear":[x,y], "angle":度}`）、`render_transform_pivot`、`clipping`、`navigation`（`{"up":"OtherWidgetName", "left":""}`，空串/`stop` = Escape 规则）与 `navigation_all`（一个控件名给所有方向）。
- **类型专属键**：TextBlock `text` / `font_size` / `color` / `justification` / `auto_wrap` / `wrap_text_at`；Button `background_color`；Image `brush`（资产路径，或 `{"resource": 贴图或材质, "margin": {…9-slice…}, "draw_as": "Box|Border|Image|RoundedBox"}`）/ `tint`；ProgressBar `percent` / `fill_color`；Slider `value`；CheckBox `checked_state`；EditableText `text`。
- 表外键走**反射兜底**（`FindFProperty`）：数值 / bool / 字符串 / 名字 / 文本 / 枚举 / 对象引用（资产路径）/ `FLinearColor`、`FVector2D`、`FMargin`、`FWidgetTransform`。属性不存在 → `unknown_property` + `candidates`；只读 → `property_not_writable`；类型不对 → `unsupported_property_type`。
- 颜色既收 0..1 也收 0..255（>1 视为 0..255 归一到 0..1）；枚举名忽略大小写、下划线与作用域前缀（`VAlign_Center` 与 `center` 都认，但一次全名匹配优先，所以 `VAlign_Center` 不会误配 `HAlign_Center`）。
- 想确认"写进去了没有"：读 `applied[].value_after`（不是请求值）；`font_size` 这类默认值可能就是你要写的值（TextBlock 默认 24），**探针里别用默认值当证据**。

## 四、写槽（`set_widget_slot`）

一个 `slot` 对象，键与 `get_widget_tree` 读回的 `slot` **同名**：

| 槽类 | 可写键 |
|---|---|
| `CanvasPanelSlot` | `anchors` `[min_x,min_y,max_x,max_y]`、`alignment` `[x,y]`、`position` `[x,y]`、`size` `[w,h]`（会关掉 auto size）、`auto_size`、`z_order` |
| `VerticalBoxSlot` / `HorizontalBoxSlot` / `ScrollBoxSlot` | `padding`、`horizontal_alignment`、`vertical_alignment`、`size_rule`（`{"rule":"Fill"|"Automatic","value":1.0}`，或裸数字 = Fill） |
| `OverlaySlot` / `SizeBoxSlot` | `padding`、`horizontal_alignment`、`vertical_alignment` |
| `GridSlot` | 上述对齐三项 + `row`、`column`、`row_span`、`column_span`、`layer`、`nudge` `[x,y]` |
| `ScaleBoxSlot` | **只有** `horizontal_alignment` / `vertical_alignment`（这是唯一"python 也改不了"的槽，见 §〇） |
| `WrapBoxSlot` / `UniformGridSlot` / `WidgetSwitcherSlot` | **命令不提供**（python 直接写更省事，见 §四a） |
| `ButtonSlot` / `BorderSlot` … | 无（回 `unsupported_slot` + 空 `fields`） |

- `padding` 收数字（四边相同）、`[l,t,r,b]` 或 `{left,top,right,bottom}`。
- **某个键不属于该槽类 → 整发拒绝**（`unsupported_slot` + 该槽类完整 `fields`），不会写一半。所以"canvas 槽塞 padding"这种误用会当场被挡下。
- 定位语义：canvas 的 `anchors` 决定 `position` 的含义 —— `anchors=[0.5,0.5,0.5,0.5]` + `alignment=[0.5,0.5]` 表示"控件中心钉在屏幕中心"（准星/居中 HUD），缺省 `[0,0,0,0]` 是左上角像素。
- 控件属性（字号/颜色/justification/brush…）**不在**这里，用 `set_widget_properties`。

### 四a、命令没提供的槽：python 直接写（附正确入口）

树里的控件是**包内子对象**，python 可以用对象路径拿到它（这是唯一入口：`unreal.WidgetBlueprint` 和生成类 CDO 上都没有 `widget_tree`，`as_variable` 的控件在 CDO 上是 null）：

```python
w = unreal.load_object(None, "/Game/UI/WBP_HUD.WBP_HUD:WidgetTree.MyWrapBoxChild")
slot = w.get_editor_property("slot")            # WrapBoxSlot / UniformGridSlot / WidgetSwitcherSlot
slot.set_editor_property("fill_empty_space", True)     # WrapBoxSlot
slot.set_editor_property("row", 1); slot.set_editor_property("column", 2)   # UniformGridSlot
slot.set_editor_property("padding", unreal.Margin(4, 4, 4, 4))              # WidgetSwitcherSlot
```

字段名（实测）：`WrapBoxSlot` = `padding` / `fill_empty_space` / `fill_span_when_less_than`（**不是** `fill_span`）/ `force_new_line` / 两个对齐；`UniformGridSlot` = `row` / `column` / 两个对齐；`WidgetSwitcherSlot` = `padding` / 两个对齐。写属性同理（`w.set_editor_property("visibility", unreal.SlateVisibility.COLLAPSED)`）。

- 这些槽的字段都是引擎里公开（BlueprintReadWrite）的字段，所以 python 侧**读也读得到**；`get_widget_tree` 只展开命令支持的那几类槽，其余只回 `class`（不是漏读，是没展开）。
- **ScaleBox 是唯一例外**：它的两个对齐在引擎里是**受保护**字段（`BlueprintReadOnly` + private），python 连读都会报 `is protected and cannot be read`；`padding` 在 5.5 已废弃。所以那个槽只能走 `set_widget_slot`。`get_widget_tree` 对它回 `padding_deprecated: true` 就是为了说清"不是漏了，是引擎里没有可用的 padding"。
- **不要用"直接改任意对象字段"的通用反射命令去绕**（`set_object_property` 之类）：实测它会触发版本控制检出的**模态对话框**并顺带存盘，之后删该资产会失败并留下"该包可能已损坏"，紧接着编辑器崩溃。要写就写在引擎公开的字段上。

## 五、结构编辑（删 / 搬 / 调序 / 改名 / 换根）

| 命令 | 纪律 |
|---|---|
| `remove_widget` | 先查引用：事件图/函数图里的控件变量 get/set 节点、`bind_widget_event` 建的组件事件节点、调用 getter 的节点 → 有则**拒绝**并回 `blocked_by_references` + `blockers[]`（`kind`/`graph`/`node`/`detail`）。无引用才删，并连带清理：该控件的绑定条目 → 绑定函数图与仅它使用的 getter → 控件变量与绑定变量 → 从父面板移除 → 整棵子树改名到 transient。回读 `removed_widgets` / `removed_bindings` / `removed_variables` / `removed_functions` |
| `reparent_widget` | `new_parent` 必须是面板（非面板 → `unsupported_parent` + 面板候选）；自身/成环 → `invalid_reparent`；根控件 → `invalid_reparent`（根没有槽，用 `set_root_widget`）。回读 `migrated_fields`（两种槽都有的字段，如 padding/对齐）与 `dropped_fields`（如 box 的 `size_rule` 搬到 canvas 就丢） |
| `reorder_widget` | 同父内按 `index` 重排（= 绘制序），越界夹取；回读 `index_before` / `index_after` / `child_order` |
| `rename_widget` | 一次同步：控件对象名 + 控件变量名 + `Bindings[].ObjectName` + 导航绑定 + 图内引用节点（回读 `reference_nodes`）。撞名 → `name_collision`（含父类成员占用）；新旧同名 → 幂等成功 `renamed: false` |
| `set_root_widget` | 让既有面板当根，**旧根成为新根的子件**（顺序：新根原有子件 → 旧根）。树里已有根时新根必须是面板，否则 `unsupported_root_panel` + 面板候选；`AddChild` 失败会把树还原再报错 |
| `add_widget_to_viewport` | **不是**入视口命令：只回报控件类路径，运行时入视口要在 GameMode/Player 的 BeginPlay 里 `CreateWidget → AddToViewport`（`WidgetBlueprintLibrary.Create` 返回 `UserWidget*`，要塞进"控件类引用"变量得先 Cast） |

- 父面板相关的报错都带 `panels` / `widget_names` 候选，先读它再改参数，别重试同一组参数。

## 五b、属性绑定（拉模式）：能绑什么、两道闸门、生效对账

**先想清楚要不要绑**（UMG 绑定是**轮询**求值，Epic 自己的建议是能事件驱动就别绑）：

| | 控件变量 + 直接写（推） | 属性绑定（拉） |
|---|---|---|
| 怎么改值 | `set_widget_properties` 或事件图 `Get 控件 → Set 属性` | 只改 backing 变量，控件自己求值 |
| 适合 | 值在少数明确的地方变（进关卡设一次、按钮点了改一次） | 值随时可能变、且不想在每个改动点写 UI 代码 |
| 代价 | 改动点多了要记得同步 | 每帧求值有开销；隐式；有两道会**静默失效**的闸门 |

**能绑什么**由类决定：`set_widget_property_binding(blueprint_name, widget_name, property_name, binding_name)`。属性名不可绑 → `unsupported_property` + `bindable_properties`（该类上**真正**可绑的属性清单）；返回类型不在映射表 → `unsupported_property_type`（不会降级成 Text）。

映射表（delegate 返回 → 变量与 getter 返回引脚）：

| delegate 返回 | 类型 | 例 |
|---|---|---|
| `FTextProperty` | `text` | TextBlock `Text` |
| `FFloatProperty` / `FDoubleProperty` | `float` / `double` | ProgressBar `Percent` |
| `FBoolProperty` | `bool` | — |
| `FByteProperty`+枚举 / `FEnumProperty` | `enum` | 控件 `Visibility`（`ESlateVisibility`） |
| `FStructProperty` | `struct` | `ColorAndOpacity`（`FSlateColor`）、`Brush` |
| `FClassProperty` / `FObjectProperty` | `class` / `object` | 贴图/材质型属性 |

回执的 `variable_type` 就是它推导出来的类型。

**三条必须知道的事**：

1. **命令自己编译并回读运行时表**：只有条目真的进了 `UWidgetBlueprintGeneratedClass::Bindings` 才回 `effective: true`。引擎只在 full compile 时把绑定从编辑器表搬到运行时表，被拒就**静默丢弃** —— 那种情况命令回 `binding_not_effective`（带 `message` 摘要），**不要**当成功。失败时编辑器侧条目会留下（`get_widget_tree` 里该条 `effective: false`），用 `unbind_widget_property` 收掉。
2. **一个 `binding_name` = 一个变量 + 一个 `Get<binding_name>` getter**。要让两个控件共用同一个 getter，两次绑定的**属性类型必须相同**（都 `Visibility` 行；`Visibility` + `Text` → `variable_type_mismatch`）。同 `binding_name` 重绑是幂等的（复用变量与 getter 的已有接线，不重复登记、不堆节点）。
3. **生效对账**：`get_widget_tree` 的 `bindings[]`（编辑器表，每项带 `effective`）对 `runtime_bindings[]`（运行时表）。只有两边对得上才算真绑上。要一次看全，用 `get_umg_compile_errors` 的 `bindings_dropped[]`（见 §六）。

**解绑与清理**：

- `unbind_widget_property(blueprint_name, widget_name, property_name, remove_function=False, remove_variable=False)` —— 默认**只删绑定条目**，getter 与变量留着；要删必须显式声明，且**被任何东西引用时整发拒绝**：`blocked_by_references` + `blockers[]`（`kind` = `binding_entry`（别的绑定还在用这个 getter）/ `function_node`（图里有节点调用它）/ `variable_node`（图里在读这个变量））。先读 `blockers` 再决定是改绑还是先清理引用。
- `prune_widget_bindings(blueprint_name, dry_run=True)` —— 报三类绑定残留：`stale_binding`（`Bindings[]` 里的控件已不在控件树）/ `orphan_function`（没人调用、也没绑定在用的 `Get<X>`）/ `unused_variable`（只被那种孤儿 getter 用过的变量）。默认只报；`dry_run=false` 用**同一份计划**清理（`removed_count == item_count`）。**你自己声明但还没接线的普通变量不会被报、也不会被删**。
- `set_text_block_binding` 是 `set_widget_property_binding` 的**弃用别名**（回执带 `deprecated_command`），新代码用新名字。

## 六、落盘、编译与收口（诊断）

- 本域写命令**成功即编译 + 按 `persist` 落盘**（`persist` 缺省 true）；回执恒带 `compiled`、`persist_requested`（你要的是什么）与 `saved`（本次**真的**存盘了吗）。**被拒的命令回 `saved=false`**：`persist_requested=true` + `saved=false` 就是"该存但没存"。
- 同一条资产的**多条写命令**传 `persist=false`，批末自己 flush 一次（`unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)`），避免"命令条数 = 盘 IO 次数"。`persist=false` 时 `saved` 必为 false 且盘上 `.uasset` 的 mtime **不变**（拿 mtime 当判据，别拿"没报错"当"已保存"）。
- `compiled=true` 不等于资产已经落盘（`persist=false` 时 `saved=false`）；`saved=false` 时**别重启编辑器**，否则这批改动丢。
- **编译状态字段全域同形**：写命令回执与 `get_widget_tree` 都给 `status` = 枚举名（`BS_UpToDate*` / `BS_Error` …）+ 数字 `status_code`；`get_umg_compile_errors` 的 `status` 同形。布尔 `compiled` 只回答"是否处于可运行状态"，区分不了 `BS_UpToDate` 与 `BS_UpToDateWithWarnings`（要精确判断就读 `status`）。历史上写命令这里曾是数字字符串（`"3"`），已统一。

**写完必须收口**：`compiled` 只是个布尔，UMG 的失败大多发生在**编译期**且会被静默吞掉（典型：绑定不是纯函数 → 引擎 `FDelegateEditorBinding::IsBindingValid` 拒绝并在 full compile 时把该条绑定从运行时表里丢掉，命令当时照回 success）。所以每次"绑完 / 改完结构"之后：

- `compile_umg_widget(blueprint_name, force_full=true)` —— 显式编译并拿回 `num_errors` / `num_warnings` / `messages[]`。`force_full` 的真实含义是**先 `MarkBlueprintAsStructurallyModified` 再编译**（实测唯一会触发"编辑器绑定 → 运行时绑定表"搬运与校验的路径），不是某个引擎开关；只改数据不改结构时可以 `force_full=false`。
- `get_umg_compile_errors(blueprint_name, since?)` —— 编译诊断的**权威读数**：
  - `status`（`BS_UpToDate*` / `BS_Error` …）与 `compiled`；`messages[]` 每条带 `severity` / `message`，能定位时附 `node{node_id,node_name,graph_name}`（消息来自带 `FCompilerResultsLog` 的编译调用，不是从编辑器日志里 grep）。
  - `bindings_dropped[]`：**编辑器绑定表里有、运行时表里没有**的条目，逐条 `{widget, property, function, message?, message_matched}`。`message_matched=true` = 编译器消息里认领到了这条（消息文本含该属性名或控件名）；`false` = 只是差集，**不要**把 `message` 当证据。命令**只报告、绝不自动修**。
  - `binding_summary{editor_count, runtime_count}`：两边计数不等就是有丢弃（`editor_count > runtime_count`）。
  - `log_this_compile[]` / `log_historical[]` 的分界：`boundary_source=umg_mutation`（默认，取本会话对该资产最近一次 MCP 写）/ `explicit_since`（你传了 `since`，ISO-8601 或 epoch 秒）/ `unknown`（本会话没经 MCP 写过它）——`unknown` 时全部消息按历史算、`log_this_compile` 为空，这是**如实**而非缺陷。
  - 这个命令**自己会编译**（回执带 `compile_performed`）才能拿到消息，所以它不是纯读：经常读它会反复重编；它不落盘。
- 消息里出现 `needs to be bound to a pure function` → getter 被外部改成了非纯；`signatures don't match` → getter 签名（返回类型/参数）与属性对不上。两种都表现为"编辑器里有绑定、运行时不生效"，修法见 §五b（重绑或 `unbind_widget_property` + `prune_widget_bindings`）。

## 六b、设计器会话、控件动画与 PIE 入视口

### 1. 打开设计器（`open_umg_designer`）

`open_umg_designer(blueprint_name, focus_widget?)` = 命令式打开控件蓝图的设计器（蓝图有 `open_blueprint_graph`，控件蓝图此前没有任何入口），可选把某个控件选中（便于让用户立刻看到目标）。回执：`opened` / `editor_open` / `has_preview`（设计器是否有预览实例）/ `focused` + `focus_reason`。

- **打开是同步的**：实测同一次派发里 `has_preview` / `focused` 就已经为真，不需要"再等一帧"。
- `focus_widget` 不存在 → `widget_not_found` + `widgets`（树里现有控件名）；**不存在的资产**同样拒绝，不会建出空 tab。
- **tab 位置记录会被还原**：引擎在 tab 关闭时把"下次打开位置"写进 `[AssetEditorToolkitTabLocation]`（`GEditorPerProjectIni`），本命令先读后写回（与 `close_asset_editors` 同一套做法），所以 MCP 打开/关闭都不改变用户下次看到它的位置。
- 设计器开着**不影响**写命令与删资产（它们走内存对象）；preview 只在设计器会话里存在。

### 2. 设计尺寸（`set_umg_design_size`）

`set_umg_design_size(blueprint_name, width?, height?)` 只改**设计器预览尺寸**：写的是控件 CDO 上的 `DesignTimeSize` + `DesignSizeMode=Custom`，并 `MarkBlueprintAsModified`（不必重编译）。回 `applied[]` / `design_size_before` / `design_size`（回读）/ `size_mode_changed` / `scope`。

- **只给了尺寸 = 一定进 Custom 模式**：尺寸只存在 Custom / CustomOnScreen 两种模式下，其它模式是算出来的。
- **`dpi_scale` / `preview_platform` 刻意不是参数**：UMG 没有"每资产 DPI"（设计器的 DPI 是从项目 UI 缩放曲线推出来的）也没有"每资产预览平台"（那是设计器视图自己的设备 profile）。写了这两个字段的调用整个被拒（`unsupported` + `unsupported_fields` + `supported_fields`），**不半应用**。
- 请求里一个尺寸字段都没有 → `missing_parameter`。

### 2b. 设计器覆盖层：虚线框与网格吸附（`set_umg_designer_overlay`）

截图做外观验收时，设计器自己画的**虚线控件轮廓**会混进画面（`captured_via=widget_preview_slate` 时 `includes_editor_overlay: true` 说的就是它），它由项目级用户设置决定：

- 开关是 `UWidgetDesignerSettings::bShowOutlines`（显示名 "Show Dashed Outlines By Default"），类声明为 `UCLASS(config=EditorPerProjectUserSettings)` ⇒ **项目级、不是资产属性**，所以这条命令**没有 `blueprint_name`**，也不写任何资产。
- **python 控不了**：`unreal.WidgetDesignerSettings` 未暴露给 python（`getattr` 得 `None`）⇒ 只能走命令：`set_umg_designer_overlay(show_outlines=False)`（另有 `grid_snap_enabled` / `grid_snap_size` / `save`）。
- 落盘位置是 `Saved/Config/<Platform>/EditorPerProjectUserSettings.ini` 的 `[/Script/UMGEditor.WidgetDesignerSettings]` 节。**该 ini 通常在编辑器退出时才刷新**，所以"改完立刻 grep ini 看不到键"属正常，别拿它当失败判据 —— 判据是**下一张截图里虚线有没有**。
- **已打开的设计器视图保持它创建时的状态**（回执里有 `existing_designer_views_may_need_reopen`）⇒ 改完要重开设计器或编辑器才生效。

### 3. 控件动画（`UWidgetAnimation`）

| 需求 | 命令 | 关键点 |
|---|---|---|
| 建动画 | `create_widget_animation(blueprint_name, animation_name)` | 建 `UWidgetAnimation` + 它自己的 `UMovieScene`（默认 20fps、2s 播放区间），挂进 `Blueprint->Animations` 后**标结构性修改 + 编译**；重名 → `name_collision` |
| 删 / 改名 / 改时长 | `remove_widget_animation` / `rename_widget_animation` / `set_widget_animation_playback_range` | 删除时若**图里有节点仍持有该动画** → `blocked_by_references` + 节点清单（先删/改那些节点）；改名重名 → `name_collision`；改时长用 `length` 或 `end_time`（+ `start_time`），超出区间的键保留并计入 `keys_beyond_range` |
| 加轨道 | `add_widget_animation_track(blueprint_name, animation_name, widget_name, property)` | `property` 只认三个：`RenderOpacity`→`MovieSceneFloatTrack`、`Visibility`→`MovieSceneByteTrack`、`RenderTransform`→`MovieScene2DTransformTrack`；其它 → `unsupported_property` + `supported_properties`。同一条（控件+属性）再调一次是**幂等复用**（`track.reused: true`） |
| 写关键帧 | `set_widget_animation_keyframes(…, keys=[{time, value}])` | **整表替换**（`keys_replaced` 报被清掉的条数），time 单位是**秒** |
| 读回 | `list_widget_animations(blueprint_name)` | 动画 → 轨道（`widget_name` + `property`）→ 关键帧；`in_generated_class` 是"运行时能不能播"的判据，`widget_exists` / `orphan_track_count` 是"有没有指向已删控件的残留"的判据 |
| 预览播放 | `play_widget_animation_preview` / `stop_widget_animation_preview` | 只作用于**设计器预览实例**，不落盘、不改资产 |

值按属性类型派发：

```python
# RenderOpacity：数字
keys=[{"time": 0.0, "value": 0.0}, {"time": 1.0, "value": 1.0}]
# Visibility：枚举成员名（前缀可省）或数字
keys=[{"time": 0.0, "value": "Visible"}, {"time": 1.0, "value": "Collapsed"}]
# RenderTransform：对象，至少给一个字段（translation / scale / shear / angle）
keys=[{"time": 0.0, "value": {"translation": [0.0, 0.0], "angle": 0.0}},
      {"time": 1.0, "value": {"translation": [100.0, 0.0], "angle": 45.0}}]
```

- **写完必须编译 + 回读生成类**：动画和绑定一样，只有 full compile 会把 `Blueprint->Animations` 的副本搬进 `UWidgetBlueprintGeneratedClass::Animations`，而**运行时读的是生成类那份**。三条写命令都自动标结构性修改 + 编译，并在回执里给 `compiled` / `animation_effective`（为假时直接 `animation_not_effective` 报错，不假装成功）。
- **关键帧列表先全量校验再写**：有一条非法值就整个拒绝，**不动**已有键（否则"改坏一个值"会静默清掉整条轨道）。
- 超出播放区间的关键帧：命令会把播放区间**延长到覆盖它**并回 `playback_range_extended: true`（不延长的话那些键永远不会播）。
- 轨道与控件的关系靠 `AnimationBindings`（引擎的 `BindPossessableObject` 写）：**不要手搓这条记录**，写错（控件名不是 `FName`、漏 `bIsRootWidget`）就会出现"动画有键、运行时不动"。
- **预览播放的是副本**：设计器会把动画复制到它自己的预览类上，所以 `play_widget_animation_preview` 要求设计器开着（否则 `editor_not_open`）；副本还没刷出来 → `animation_not_in_preview`（先 `compile_umg_widget` 再试）。播放/停止**不改资产**（可用 `.uasset` mtime 自证）。
- **动画是按控件名字找目标的**：`rename_widget` 会同步动画绑定（回 `animation_bindings_updated`），`remove_widget` 会把该控件的轨道一起删掉（回 `removed_animation_tracks`）—— 这两条是刻意的，因为跳过就会留下"键都在、运行时不生效"的静默坏账。**别绕过这两个命令直接改控件名/删控件**（例如手搓 python），否则孤儿轨道只能靠 `list_widget_animations` 的 `orphan_track_count` / `widget_exists` 事后发现。

### 4. 让控件真的出现在屏幕上（`add_widget_to_viewport`，**BREAKING**）

语义已从"只回报类路径"改成"**在运行中的 PIE 世界里 `CreateWidget + AddToViewport`**"：

- 没有 PIE → `pie_not_running`（`hint` 提示先 `start_pie`）；PIE 起来了但还没有 PlayerController → `pie_not_ready`。
- 成功回 `created_widget_name` / `z_order` / `pie_world` / `owning_player` / `in_viewport`（`IsInViewport()`，命令侧能自证的全部）+ `class_path`。
- 实例**只活在 PIE 世界**：PIE 停就没了，也不改资产。观感（真的画在屏幕上）由用户在 PIE 视口确认。
- **PIE 需要编辑器 tick 才能起来**：`start_pie` 之后**不要在同一次派发里**等它（脚本占着 game thread，PIE 永远起不来）。拆成"这次派发 start_pie →（真实时间过去）→ 下一次派发断言"。夹具清理放再下一次（见 §七 的 PIE 运行期限制）。
- 常规做法仍是 GameMode/Player 的 BeginPlay 里 `Create Widget → Add To Viewport`；本命令是给"验收时把控件塞到眼前"用的。

### 5. 把界面**截下来**看（`take_screenshot` 的两条 UMG 路）

控件蓝图是**唯一一类没有 scene viewport 的资产编辑器**（它的预览是纯 Slate），所以"截图看界面"要选对路：

| 要什么 | 调用 | 回包判据 |
|---|---|---|
| **设计长什么样**（迭代 UI 时最常用） | `take_screenshot(source="asset_editor", asset_path="<WBP 路径>")`，先 `open_umg_designer` | `captured_via: widget_preview_slate`、`includes_umg: true`、`camera: null`；**不需要 PIE**；尺寸默认是设计尺寸（如 960x540） |
| **游戏里长什么样**（玩家视角） | `start_pie` → `add_widget_to_viewport(blueprint_name=…)` → `take_screenshot(source="pie", route="slate")` | `captured_via: pie_slate`、`includes_umg: true` |

三条纪律：

1. **`route` 不给就永远看不到 UI**：`source="pie"` 默认 `auto`，先读 PIE 后缓冲且成功即 return，而那条路不含 UMG（`captured_via: pie_viewport_readback`、`includes_umg: false`）。要走 Slate 必须显式 `route="slate"`。
2. **两条路各有"辅助线"问题**：设计器预览那条**含设计器自己画的虚线轮廓**（`includes_editor_overlay: true`，Slate 取的是窗口区域，压在上面的也会入镜）；PIE `route=slate` 那条没有辅助线，但要编辑器窗口在前台（最小化时 Slate 拿不到像素，回 `pie_screenshot_failed`）。
3. **要不要 PIE 看问题**：只想看设计/改设计 → 设计器预览那条，省掉 PIE 的启动与拆解；要判"玩家实际观感/遮挡/命中区" → PIE 那条。两者都只是**排查**手段，观感结论仍以用户在视口里看到的为准。
4. **"设计器预览不显示 UMG 画面"（截图纯黑 / `widget_preview_screenshot_failed` / `captured_width` 有值但 `no pixels`）⇒ 关掉重开设计器，不要去折腾窗口位置**。
   症状与窗口在屏与否**无关**：实测（2026-10-10）窗口矩形落在桌面内、`IsVisible=true`、`IsMinimized=false`、桌面截图里窗口也明明在，但设计器的**设计视口里只有空灰网格**；`FSlateApplication::TakeScreenshot` 走 `Renderer->PrepareToTakeScreenshot`（`SlateApplication.cpp:4217`）从窗口后缓冲拷像素，拷不到就 `return false`，而这条路径**判不出**"视图陈旧"（所以任何基于窗口几何的预检都会误判为"可以截"）。
   做法（实测有效）：`close_asset_editors(asset_path=…)` → `open_umg_designer(blueprint_name=…)` → 重新 `take_screenshot`。当场变化：同一资产同一次调用由 `widget_preview_screenshot_failed` 变为正常图（`553975` 字节）。
   反面做法：把窗口拖回屏内只对"窗口被最小化/移出桌面"有效（那种情况回的是 `pie_screenshot_failed` 一类的窗口侧错误），对"设计器视图陈旧"无效 —— 别在这上面花时间。
5. **窗口的 show state 刚变过时不要截（会 AV 把编辑器打死）**：实测 2026-10-12，PIE 运行中调 `route="slate"`：窗口最小化时调用会正常回 `pie_screenshot_failed`（`window_minimized: true`，这是好行为），但从外部 `ShowWindowAsync(SW_RESTORE)` 把窗口拉回来**紧接着**再调同一个截图 ⇒ 客户端 `No response from Unreal Engine`，日志 `Unhandled Exception: EXCEPTION_ACCESS_VIOLATION reading address 0xffffffffffffffff`、栈顶 `SlateCore → Slate → ApplicationCore → USER32`，编辑器进程直接退出（`AudioMixerRenderThread crashed` 是表象）。做法：需要 UMG 的 `route="slate"` 截图安排在整个流程里"**窗口确定稳定**"的时刻（刚 `start_editor.bat` 起完、或用户确认窗口已在前台之后），**不要**与"外部动窗口 / 切分辨率 / 拖窗口"的调用相邻；   只要不看 UMG 就用默认 backbuffer 路（回包 `captured_via: pie_viewport_readback`）。
6. **UI 出图不要依赖控件自己的 tick 把状态推上去**。控件的可见性/内容是它在 `NativeTick` 里读外部状态决定的，
   这本身没问题；但**编辑器窗口在后台时 PIE tick 被节流**，到 `take_screenshot` 那一刻可能一次都没跑到
   （实测：面板 `visibility=COLLAPSED`、15 个子件全是空数据，而回包里图片大小、`captured_via: pie_slate`、
   `includes_umg: true` 全都正常；`get_pie_widget_tree` 那边给出 `has_geometry: false` +
   "Slate only arranges a tree while it is drawing" 的提示）。**做法**：截图前由脚本把该控件实例的状态
   **显式推一次** —— 从宿主（GameMode/PlayerController）拿到实例引用，直接调它的 `SetXxx` 方法 +
   `w.set_visibility(...)`，再截图；   不要"设个全局开关然后等它自己刷新"。判据用控件自己的回读方法（如
   `call_method("DescribePanel")`）或 `get_pie_widget_tree` 的结构，而不是"截图看着有"。
   **更狠的一条：`Collapsed` 的 user widget 不会 tick** —— 若控件在 `NativeTick` 里决定自己该不该显示，
   那它一旦被 Collapsed 就再也醒不过来（实测：subsystem 报 `open=1`、控件仍 `COLLAPSED`、子件全空；只有脚本
   显式推它才显示；同一时刻 Actor 的 tick 是正常的，所以不是"PIE 没 tick"）。**做法**：控件自身保持可见
   （`SelfHitTestInvisible`），"关着"用**隐藏内容**表达（背景图 Collapsed + 子件 Collapsed + 提示框隐藏）；

## 七、验收骨架（放 `Saved/MCPScripts/`，跑完自删夹具）

```python
import json, unreal

PROBE_DIR, PROBE_NAME = "/Game/MCP/UmgProbe", "WBP_Probe"
PROBE = PROBE_DIR + "/" + PROBE_NAME
FAILURES = []

def bridge(command, **params):                       # 与 TCP 同形状的同步派发（编辑器内回环）
    return json.loads(unreal.UnrealMCPPythonAPI.execute_mcp_command(command, json.dumps(params)))

def tolerated(command, **params):                    # 预期会失败的那几条：放行给断言，不抛
    params["on_error"] = "continue"                  # 守卫消费这个键，不会转给 handler
    return json.loads(unreal.UnrealMCPPythonAPI.execute_mcp_command(command, json.dumps(params)))

def call(command, **params):                         # (result, error_code)
    r = bridge(command, **params); res = r.get("result") or {}
    if r.get("status") == "success" and res.get("success") is not False: return res, None
    return res, res.get("error_code")

def check(ok, msg, extra=None):
    print(("PASS: " if ok else "FAIL: ") + msg + ("" if ok or extra is None else " | " + json.dumps(extra, default=str)[:400]))
    if not ok: FAILURES.append(msg)

def main():
    tolerated("safe_delete_asset", asset_path=PROBE)                   # 夹具从零开始（不存在是正常的）
    call("create_umg_widget_blueprint", name=PROBE_NAME, path=PROBE_DIR)
    ...                                                                # 断言集中收集，不中途 abort
    deletion = tolerated("safe_delete_asset", asset_path=PROBE)
    check((deletion.get("result") or {}).get("deleted") is True, "fixture deleted", deletion)
    assert not FAILURES, FAILURES

main()
```

> **回环是 fail-fast 的**（2026-10-15 起）：脚本里某条 bridge 指令失败会抛
> `unreal_mcp_loopback.UnrealMCPCommandError`（消息带命令名与 `error_code`），**脚本当场中断**，
> 不再"把失败当返回值、后面几十条继续跑"。所以**故意造失败的每一步**（负例、可能不存在的夹具、
> 断言 `deleted` 的删除）都必须显式放行：逐调用 `"on_error": "continue"`，或整段脚本开头
> `import unreal_mcp_loopback; unreal_mcp_loopback.FAIL_FAST = False`。
>
> 配套的另一半（同日）：**"没达到预期效果"一律是失败**，不再有 `success:true` 但什么也没干的形状 ——
> `safe_delete_asset` 拒删（被引用 / 关卡是 World Partition / 资产编辑器开着 / 引用不可达）现在直接回
> `success:false` + `error_code`（= 原来的 `reason`），`set_widget_properties` 之类"全失败"的批同样
> 回 `nothing_applied`；`dry_run` 是查询，仍 `success:true` 且**不带 `deleted` 字段**。

- 运行：`execute_python_file(file_path=<绝对路径>, timeout=300)`（`timeout` 是**工具面**参数，bridge 命令只有 `file_path`/`deferred`）；脚本长/编译多时才考虑 `deferred=True`。
- **断言用回读**：`applied[].value_after`、`slot.*`、`child_order`、`removed_*`、`blockers`；别断言"命令返回 success"。
- 负例要断言 `error_code` **和**候选字段（`candidates` / `panels` / `fields` / `blockers`）。
- **判资产存在用 `unreal.EditorAssetLibrary.does_asset_exist(path)`**：`does_asset_exist` 不是 bridge 命令，python 侧 `bridge("does_asset_exist", …)` 会回 `unknown_command`（拿它做门禁会让断言静默 SKIP）。**例外：PIE 运行期间这个 python API（以及 `list_assets`）会回空/False**，而资产其实在（C++ 侧 `get_umg_compile_errors` / `safe_delete_asset` 照样能读到）——PIE 期间的探针不要用它当门禁，用命令自身的错误码（如 `widget_blueprint_not_found`）。
- **PIE 运行中不要删夹具资产**：`safe_delete_asset` 已因此加了守卫 —— PIE / SIE 里调用会整调用拒（`pie_running` + `hint`，`force` 也不绕过），因为它在 play mode 下文件部分做不成、却会把内存对象删掉；若该资产此刻在设计器里开着，tab 的 `EditingObjects` 变空，下一帧直接断言崩溃（`AssetEditorToolkit.cpp` 的 `EditingObjects.Num() > 0`）。清理一律放在 `stop_pie` 之后、确认 `pie running == false` 的那一次派发里做；PIE 期间的"资产还在不在"别用 `does_asset_exist`（会说谎），用任一读命令复查。
- 探针的"写入证据"不能用默认值：TextBlock 的 `font_size` 默认就是 24，写 24 的回读与不写一样（踩过）。
- 想验"控件真的归控件树"（而不是 `NewObject` 造的孤儿）：造控件 → 保存 → 重启编辑器 → 再 `get_widget_tree` 读回仍在树中。重启由用户执行，脚本分两段（写夹具 / 读夹具）。

> **这一节已拆成独立 skill**：`unreal-umg-case-list-grid-selection`（列表/网格/选中态/详情联动的实测套路）。

## 八、外观：命令面之外的那一半（Border 的 brush、9-slice 贴图、批量写法）

命令面能写 TextBlock/Image/Button/ProgressBar 的属性，但**外观的底色与边框有一半在命令之外**，这部分只能走 python 对象路径：

| 你要改 | 命令能不能 | 怎么做 |
|---|---|---|
| Border 的底色 / 背景贴图 | **不能** —— `get_widget_tree` 对 Border 恒回 `properties: null` | python：`w.get_editor_property("Background")`（`FSlateBrush`）、`w.set_editor_property("BrushColor", …)`、`w.set_editor_property("Padding", unreal.Margin(...))` |
| TextBlock 的 Font / ColorAndOpacity | 能（`font_size`/`color`） | python 也行：`Font`（改 `size` 后写回）、`ColorAndOpacity` |
| Button 的内边距 / 各态样式 | 不能（只有 `background_color`） | python 写 `WidgetStyle`，见下 |
| 批量几十个控件 | 能但一条一个 | **结构用命令、属性用 python 一次性跑完**，收尾 `unreal.BlueprintEditorLibrary.compile_blueprint(asset)` + `unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)` |

**几个只有踩过才知道的桶**：

- **颜色属性有两种类型**：`Border.BrushColor` 与 **`Image.ColorAndOpacity`** 是 `LinearColor`（直接赋），
  `TextBlock.ColorAndOpacity` 与 brush 的 `tint_color` 是 `FSlateColor`（`unreal.SlateColor(specified_color=LinearColor(...))`，或改出已有结构的 `specified_color` 再写回）。**同名不同型**，别按控件名猜：读出来 `isinstance(x, unreal.LinearColor)`。猜错就是 `Failed to find property 'specified_color' on 'LinearColor'` 或 `Cannot nativize 'SlateColor' as 'LinearColor'`。读 FSlateColor 的 alpha 要 `.get_editor_property("specified_color").a`（`FSlateColor` 自身没有 `.a`）。
- **`draw_as=Box` 在本工程画不出东西，且 `ImageSize` 只有命令能改**：
  `FSlateBrush::ImageSize` 的类型是 `DeprecateSlateVector2D`，**python 造不出这个结构**（`Vector2D` / `IntPoint` 都被 setter 拒绝）⇒ 脚本建的 brush 只能带 Slate 默认的 **32×32**（`DefaultImageSize`，注意**不是 0** —— 我一度按"0 除"解释，已被回读推翻）。实测：`draw_as=Box` + `margin 16/24` 在 `UBorder.Background` 上**什么都不画**，把 `image_size` 从 32 改成 64（贴图真实尺寸）**仍不画**，切回 `draw_as=Image` 立刻恢复；机制未定位（`image_type` 读出来是 `NO_IMAGE` 的 brush 里也有渲染正常的，所以不是它）。
  做法：① **槽位/底板这类贴图用 `draw_as=IMAGE` 整体拉伸**（近正方、边框 1~2px 时与 9-slice 肉眼无差），这也是本工程的现状；② 需要改 brush 的 `resource / draw_as / margin / image_size / tint / color` 时走命令 `set_widget_brush`（逐键 before→after 回读，传贴图会自动补 `image_size`），但**别把它的 9-slice 当成已验证能力**。判据：改完必须**截图对比字节数** —— 相同字节说明预览没刷新或像素真没变，别只看回执。
- **这些颜色是 linear，不是 sRGB —— 最容易整块做灰的坑**：`FLinearColor` 按 **linear** 参与渲染，Slate 输出时才做 sRGB 编码。照 sRGB 直觉填 `0.043` 会显示成 `(59,62,72)` 而不是 `(11,12,17)`，暗色主题会整体发灰、缺对比。做法：**先定 sRGB hex 目标再转 linear**（`c = h/255; c/12.92 if c<=0.04045 else ((c+0.055)/1.055)**2.4`）。**验证必须采样**：截图后读渲染像素，反推 `linear ≈ ((v/255+0.055)/1.055)**2.4` 与设定值逐点比对（实测命中 `#0E1016`/`#C9A253` 才算对）；只看肉眼"好像有点灰"会来回改错方向。
- **纵向渐变的条状贴图不要当 9-slice**：金属条、进度条填充这类"只有纵向渐变、两端没有对称 cap"的图，用 `margin` 切会露出 cap 与中段的色阶断层（实测 `传说` 条与名牌左半段发暗并出现竖缝）。整条拉伸 `draw_as=IMAGE` + `margin=0` 才是对的；9-slice 只用于**四边对称**的边框/圆角/凹槽。
- **按钮的底色是两层相乘**：`Button.BackgroundColor` 只是乘数，真正形状来自 `WidgetStyle` 的三个 state brush。默认 brush 是带 9-slice 的凸起样式；要平底按钮得把 `normal/hovered/pressed` 三个 brush 设成 `draw_as=IMAGE` + `margin=0`（此时 hover/press 反馈靠改各态 `tint_color`）。**内边距在 `WidgetStyle.normal_padding` / `pressed_padding`，不在 state brush 上**（写 brush 的 `padding` 会报 `Failed to find property 'padding' on 'SlateBrush'`）。
- **换了背景贴图要把 `BrushColor` 归白**（1,1,1,1）：BrushColor 是乘数，旧值会把贴图压暗/染色。
- **`Border` 只有一个 brush ⇒ 画不出"描边 + 透明心"**：边框、圆角、发光、选中环都只能靠**带 9-slice margin 的贴图**（`draw_as=BOX` + `margin=(l,t,r,b)`）。用纯色 Border 当"框"只能得到实心块（选中框会盖住图标，这是实测踩到的）。
- **9-slice 贴图不要用 AI 生图**：出图尺寸不是你请求的那个，而且边框宽度左右不对称 ⇒ 切不出精确 margin。程序化画（PIL/Python 画完存 PNG）才能拿到对称、精确的像素。导入走 `import_texture`：`srgb=true` / `compression=TC_EDITOR_ICON` / `lod_group=TEXTUREGROUP_UI` / `mip_gen=TMGS_NO_MIPMAPS`（逐项 before→after 回读）。
- **`SizeBox` 的尺寸覆写没有 `b_override_*` 属性可写**：用方法 `set_width_override(v)` / `set_height_override(v)`（清除用 `clear_*`）；写 `b_override_width_override` 会报属性不存在。
- **`VerticalBox` 自动尺寸 + 自动换行文字会叠在一起，用显式 canvas 锚点最稳**：两张 `TextBlock` 放进
  `auto_size` 的 `VerticalBox`（`size_rule=Automatic`），PIE 量到的落位是"第二行 +46 单位"而堆叠位置应为
  "+61" ⇒ 两行字重叠（观感上是"字压在字上"，截图一眼能看出）。改成**各自一个居中的 canvas 槽**
  （`anchors=[0.5,y,0.5,y]` + `alignment=[0.5,0.5]` + `auto_size=True`，`y` 取 0.28 / 0.62 这类比例）
  立刻确定：不需要猜容器怎么分摊高度。判据是 PIE 里 `get_pie_widget_tree` 报的
  `geometry.absolute_position`，两行之间的差应 ≥ 上面一行的高度 + padding。
- **`UImage::SetBrushFromTexture` 会把 brush 尺寸设成贴图的尺寸，槽小就溢出**：贴图 128×128、槽只有 84 时，
  `SetBrushFromTexture(tex, true)` 让 brush 的 `ImageSize=128`，图会按 128 画（Overlay 不裁剪）⇒ 要么
  在 `set_widget_brush(..., image_size=[w,h])` 里明确尺寸，要么在 C++ 里 `SetBrushFromTexture` 之后
  再 `Image->SetBrushSize(FVector2D(w, h))`。**别指望宿主容器把它裁到槽里**。
- **面板"外框 + 内衬"两色底**：内衬 Border 在 Overlay 里若是 `HAlign_Left/VAlign_Top`，它就只包住自己的内容，其余区域露出外框色（表现为一块"跑偏的斜框"）。要真正套住整块，把内衬的 **Overlay 槽**改成 `HAlign_Fill/VAlign_Fill`，并给外框 `Padding` 留出边框厚度。（slot 的 padding/对齐两条路都能写。）
- **枚举成员名与 C++ 不同名**：`unreal.VerticalAlignment.V_ALIGN_CENTER/V_ALIGN_FILL`、`unreal.HorizontalAlignment.H_ALIGN_FILL`、`unreal.TextJustify.CENTER`、`unreal.SlateBrushDrawType.BOX`。写之前先 `dir(unreal.<Enum>)`（实测 `VerticalAlignment.CENTER` 不存在）。
- **往 Box 里加按钮别用 `add_button_to_widget`**：它默认带 `position`，非 Canvas 父会被正面拒绝（`unsupported_parent`）。用 `add_widget`(Button) + `add_widget`(TextBlock, parent=按钮) 两步。
- **"风格统一"不是审美话术，是可回读的色牌纪律**（一套 UI **只允许一支装饰金**）：
  1. 先定令牌（写成 linear 值，一次性回填所有控件）：`GOLD #C9A253`（唯一装饰金）／`GOLD_HI #EBCE93`（它的亮变体，用于主标题）／`IVORY #EDE3C8`（主文字与数值）／`SLATE #8C93A3`（标签、描述、次要按钮）／`CRIMSON #8E2B2B`（交互与危险）。
  2. **金色只允许"暗金 + 它的亮变体"两种明度**：第三种金（亮黄 `#D9C487`、琥珀橙渐变条）是最常见的误入，它会让眼睛把同一支强调色读成不同含义。收口后工程里"金"的出现位置应当**数得清**（本工程＝标题分隔线／物品名／稀有度块／负重条 4 处，同色系）。
  3. **语义色（稀有度）允许不同色相，但两条硬约束**：① **降饱和到"做旧颜料"**（实测把槽位环从 `(0.745,0.371,0.045)`/`(0.342,0.107,0.708)`/`(0.076,0.323,0.745)` 收到 `#C9A253`/`#6B4E8E`/`#3F6E96`/`#4E7A5A`/`#9AA0AA`，并让**最高稀有度就是那支暗金**，不再新增金色）；② **面积要小** —— 满宽高饱和长条是大色块，改小色块 + 文字（`Detail_Rarity` 这类 Border 要同时改 brush `image_size` **和**槽位 `horizontal_alignment=Left` + `size_rule=Automatic`，只改 brush 会被 Fill 槽拉满宽）。
  4. **文字只有三级**（象牙＝值／灰蓝＝标签描述／暗字＝只在色块内），别再混第四种前景色。
  判据：`dump` 一遍所有相关控件的 `ColorAndOpacity`/`BrushColor`/`FillColorAndOpacity`（脚本见 §八 做法），确认它们都能落到令牌上，而不是"看起来差不多"。
- **同一个物品在槽位与详情必须同色**：详情大图的 tint 若残留（本工程曾是 `Detail_Icon.ColorAndOpacity=(1,0.63,0.16)` = 橙），就会出现"槽位里是冷钢剑、详情里是铜剑"的**同物两色**。详情图标 tint 归白（或用与槽位同源的 tint），这是同物同色的硬判据。
- **图标成套化的三步后处理**（PIL，导前一次做完，输入用已抠像/裁剪好的成图）：
  1. 用**不透明像素**的 2%/98% 分位把亮度重映射到统一黑白锚点（消除各自曝光差）；
  2. 降饱和一小截（`grey + (rgb-grey)*0.88`，`grey = 平均亮度`）；
  3. 取 `Alpha` 通道 `MaxFilter` 膨胀 ~1.5px 得到外环，填统一暖近黑（`#1A1510`, α≈210）当**描边**，再叠回图标 → 每张图从暗底上"抠"出来的方式一致。
  **判据是 44px 缩略拼版**（真实绘制尺寸），不是 256px 大图 —— 大图看不出成套不成套。
- **"好看"来自材质与立体，不来自细线**（MCP 做 UI 的美术配方，实测一轮从"板子"到"画面"）：
  1. **整屏插画底图**：先出一张与色牌一致的场景插画（`generate_image(provider="openai")`，参考图选同系列贴图锚定色板），裁到设计比例（如 1536×1024 → 1200×720）当 `BG`；`BG` 的槽要设 `anchors=[0,0,1,1]` + 偏移全 0，否则视口比设计尺寸高时下方露场景。
  2. **倒角/描边/投影都烘进贴图**：`Border` 纯色 + 1px 线只能得到"CSS 板子"。做法是 PIL 烘一张"板"（渐变填充 + 2~4px 深色描边 + 顶亮线 + 底暗线 + 内线），**并把投影一起烘**：把形状复制一份偏移 `(5,6)`、`GaussianBlur(6)`、alpha≈140 画在底层，**画布留出 padding**（控件尺寸按整张贴图给，否则投影被裁）。
  3. **9-slice 不可用 ⇒ 每个尺寸单独烘**（卡 322×442、按钮 312×88、chip 116×44、各板 968×156/592×344/624×984…）。笨但确定：一次解决"`draw_as=Box` 不渲染"与"一张贴图复用到不同比例会串味"。
  4. **对比靠材质差，不靠亮度**：暗底上放**奶油羊皮纸卡**（`#E9DEC4`→`#CEBD9C` + 墨线 `#3A2E22` 4px 描边），卡上文字用**墨色 `(0.042,0.027,0.015)`**、价格用暗红 `(0.17,0.03,0.025)` —— 这一条最能立刻"变好看"（参考图正是这么做的）。浅色卡上的 chip 文字反过来用象牙白。
  5. **按钮被压黑的坑**：`Button.BackgroundColor` 与 state brush 的 `tint_color` **是两次相乘** ⇒ 烘焙板已经够色，`BackgroundColor` 必须归白 `(1,1,1,1)`，反馈只留三档 state tint（normal 1.0 / hovered 1.2 / pressed 0.8）。
  6. **装饰性 Image 要限尺寸**：`Image` 的期望尺寸 = 贴图原尺寸，一张 560×12 的分隔线会**横贯全屏**。给这类图用 `set_widget_brush(image_size=[286,8])` 把期望尺寸压到格内（或塞 SizeBox）；要在网格里做行分隔，用 GridSlot 的 `layer=1` + `nudge=[0,-7]` 压在行边界上。
  7. **单子件面板塞不下装饰**：`Border`/`SizeBox` 只能有一个子件，把分隔线/角标加进它们会报 `unsupported_parent` —— 装饰要加到内部的多子件面板（`GridPanel`/`Overlay`/`VBox`）里，或直接做成画布兄弟节点。
  8. 设计师虚线框会污染每张验收截图：`set_umg_designer_overlay(show_outlines=false)`（编辑器被强杀重启后可能回退，重设即可）。
- **换字体族（含中文）的正确路径**（`import_assets` 对 `.ttf` 直接拒：`unsupported_extension: '.ttf' has no Interchange import flag`）：
  1. 在编辑器内用 `unreal.AssetImportTask` + **`unreal.FontFileImportFactory()`**（它的 `formats` = `ttf/ttc/otf/otc`）把字体导成 **FontFace** 资产；
  2. 建 `unreal.Font` 资产（`asset_tools.create_asset(..., unreal.Font, unreal.FontFactory())`）。它的 `composite_font` 成员（导出文本里是 `DefaultTypeface` / `FallbackTypeface` / `SubTypefaces`）**python 读写不了**（非 BlueprintReadWrite，`get_editor_property` 报 `Failed to find property 'default_typeface'`）；
  3. ⇒ 走结构体的 **`import_text`**：先 `export_text()` 一份**引擎自带字体**（如 `/Engine/EngineFonts/Roboto`）当模板，看到字段真名是 **`FontFaceAsset`**、值形如 `"/Script/Engine.FontFace'/Game/…'"`（写成 `FontObject` 会被**静默丢弃**，导出里只剩 `Name="Regular"`），再改写成自己的路径 + 保留 `FallbackTypeface`（如 `DroidSansFallback`）兜底；
  4. 赋给控件：`Font.font_object = <Font 资产>` + `typeface_font_name = "Regular"`。
  **关键判据**：**不要**把 FontFace 直接塞进 `Font.font_object` —— 那样中文会渲染成**豆腐块 `□□□`**（实测），必须经 composite Font 资产；换完必须截图确认中文成形（`read_file` 看图）。
- **内容"全屏等比"而不是"固定尺寸贴在角上"**：`CanvasPanel_0 → [BG(拉伸锚点), ScaleBox(拉伸锚点, `Stretch=SCALE_TO_FIT`, `StretchDirection=BOTH`) → SizeBox(1200×720) → CanvasPanel(Design) → 全部内容]`。
  两个要点：① **`CanvasPanel` 没有内在尺寸**，`ScaleBox` 量到 0 就什么都不缩放 ⇒ 必须用 `SizeBox` 给出 1200×720 的目标；② **`BG` 留在 ScaleBox 外面**单独拉伸铺满（否则按比例缩放会在上下/左右留黑边）。改完在设计器里看"字节数几乎不变"（5:3 预览下 0.75 缩放与原来等价）就是没弄坏；真正的判据是 **PIE 里换一个非 5:3 的视口**看是否居中铺满。
- 改完外观的验收就用 §六b-5 的两条路（设计器预览 / PIE `route=slate`）：**"控件在不在、颜色变没变"可以靠回读与截图，观感结论仍由用户在视口里定**。
