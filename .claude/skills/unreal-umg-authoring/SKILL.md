---
name: unreal-umg-authoring
description: "UE5（UnrealMCP）里用 MCP 工具 + python 脚本程序化建/改 UMG 控件蓝图（Widget Blueprint）的方法：控件只能由 add_widget 经 WidgetTree::ConstructWidget 创建（python 侧 unreal.WidgetTree 不存在，NewObject 造的控件不归树）、控件树与槽的寻址与命名纪律（树内名唯一、控件名=变量名）、set_widget_properties 的键表与反射兜底、set_widget_slot 的 slot 对象与每类槽的可写字段、结构编辑（remove / reparent / reorder / rename / set_root）的引用检查与清理链、persist 落盘与回读验收纪律。触发场景：准备调用以下任一 MCP 工具前 MUST 加载本 Skill —— create_umg_widget_blueprint / get_widget_tree / add_widget / set_widget_properties / set_widget_slot / add_text_block_to_widget / add_button_to_widget / bind_widget_event / set_text_block_binding / set_widget_property_binding / unbind_widget_property / prune_widget_bindings / remove_widget / reparent_widget / reorder_widget / rename_widget / set_root_widget / add_widget_to_viewport；以及任何涉及「用 MCP 建/改 UE 控件蓝图、HUD 与菜单、控件树层级与子件顺序（绘制序）、VerticalBox/HorizontalBox/Overlay/GridPanel/ScrollBox/SizeBox/Border/Image/ProgressBar/Slider/CheckBox/EditableText 控件、Canvas 槽 anchors/alignment/position/size、非 canvas 槽 padding 与对齐、控件属性（文本/颜色/字号/brush/tint/percent/visibility/render_transform）、控件变量与属性绑定、删控件后绑定与 getter 的清理、UMG 验收脚本」的任务。"
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

## 七、验收骨架（放 `Saved/MCPScripts/`，跑完自删夹具）

```python
import json, unreal

PROBE_DIR, PROBE_NAME = "/Game/MCP/UmgProbe", "WBP_Probe"
PROBE = PROBE_DIR + "/" + PROBE_NAME
FAILURES = []

def bridge(command, **params):                       # 与 TCP 同形状的同步派发（编辑器内回环）
    return json.loads(unreal.UnrealMCPPythonAPI.execute_mcp_command(command, json.dumps(params)))

def call(command, **params):                         # (result, error_code)
    r = bridge(command, **params); res = r.get("result") or {}
    if r.get("status") == "success" and res.get("success") is not False: return res, None
    return res, res.get("error_code")

def check(ok, msg, extra=None):
    print(("PASS: " if ok else "FAIL: ") + msg + ("" if ok or extra is None else " | " + json.dumps(extra, default=str)[:400]))
    if not ok: FAILURES.append(msg)

def main():
    bridge("safe_delete_asset", asset_path=PROBE)                      # 夹具从零开始
    call("create_umg_widget_blueprint", name=PROBE_NAME, path=PROBE_DIR)
    ...                                                                # 断言集中收集，不中途 abort
    deletion = bridge("safe_delete_asset", asset_path=PROBE)
    check((deletion.get("result") or {}).get("deleted") is True, "fixture deleted", deletion)
    assert not FAILURES, FAILURES

main()
```

- 运行：`execute_python_file(file_path=<绝对路径>, timeout=300)`（`timeout` 是**工具面**参数，bridge 命令只有 `file_path`/`deferred`）；脚本长/编译多时才考虑 `deferred=True`。
- **断言用回读**：`applied[].value_after`、`slot.*`、`child_order`、`removed_*`、`blockers`；别断言"命令返回 success"。
- 负例要断言 `error_code` **和**候选字段（`candidates` / `panels` / `fields` / `blockers`）。
- **判资产存在用 `unreal.EditorAssetLibrary.does_asset_exist(path)`**：`does_asset_exist` 不是 bridge 命令，python 侧 `bridge("does_asset_exist", …)` 会回 `unknown_command`（拿它做门禁会让断言静默 SKIP）。**例外：PIE 运行期间这个 python API（以及 `list_assets`）会回空/False**，而资产其实在（C++ 侧 `get_umg_compile_errors` / `safe_delete_asset` 照样能读到）——PIE 期间的探针不要用它当门禁，用命令自身的错误码（如 `widget_blueprint_not_found`）。
- **PIE 运行中不要删夹具资产**：`safe_delete_asset` 已因此加了守卫 —— PIE / SIE 里调用会整调用拒（`pie_running` + `hint`，`force` 也不绕过），因为它在 play mode 下文件部分做不成、却会把内存对象删掉；若该资产此刻在设计器里开着，tab 的 `EditingObjects` 变空，下一帧直接断言崩溃（`AssetEditorToolkit.cpp` 的 `EditingObjects.Num() > 0`）。清理一律放在 `stop_pie` 之后、确认 `pie running == false` 的那一次派发里做；PIE 期间的"资产还在不在"别用 `does_asset_exist`（会说谎），用任一读命令复查。
- 探针的"写入证据"不能用默认值：TextBlock 的 `font_size` 默认就是 24，写 24 的回读与不写一样（踩过）。
- 想验"控件真的归控件树"（而不是 `NewObject` 造的孤儿）：造控件 → 保存 → 重启编辑器 → 再 `get_widget_tree` 读回仍在树中。重启由用户执行，脚本分两段（写夹具 / 读夹具）。

> **这一节已拆成独立 skill**：`unreal-umg-case-list-grid-selection`（列表/网格/选中态/详情联动的实测套路）。
