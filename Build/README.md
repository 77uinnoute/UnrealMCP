# 命令面维护

命令名、参数与策略**只有一处声明**：命令注册表（`Source/UnrealMCP/Public/Core/MCPCommandRegistry.h`）。
本目录的 `check_command_consistency.py` 是它的门禁。

## 新增一条命令

以"在 material 域加一条 `get_foo`"为例，需要动 **2 个文件**（改造前是 6 处）：

1. **命令实现与注册**：在所属域的 `RegisterCommands()` 里加一条注册（handler 仍是该域私有的 `HandleXxx`）：
   ```cpp
   MCP_REGISTER_COMMAND(Registry, "get_foo", "material", "One-line description.",
       (TArray<FMCPParamSpec>{
           MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path")),   // required
           MCPParamOpt(TEXT("detail"), TEXT("string"), TEXT("Verbosity")),              // optional
       }), MCPFlags(/*loopback*/false, /*mutates*/false, /*hidden*/false, /*persist*/false),
       ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_foo"), Params,
           [this](const TSharedPtr<FJsonObject>& P) { return HandleGetFoo(P); }); }));
   ```
   - 宏的实参以**逗号**切分，所以参数列表与 handler lambda 都要用**括号包住**（上面的 `(TArray<...>{...})` 与 `([this](...){...})`）。
   - 只要注册，就已可分派、可被 `list_mcp_commands` 看到 —— 没有第二处清单要改。
2. **python 工具包装**（可选但通常需要）：在 `Content/Python/tools/<domain>_tools.py` 加 `@mcp.tool()` 函数，内部 `unreal.send_command("get_foo", params)`。改完 `Content/Python/` 下的文件需要**重启 unrealMCP server** 才生效。

   > 新命令**不留 python 包装**就会被门禁判成 ERROR（见下"判定口径"）。确实只给 `execute_python_*` 回环或脚本用的，要么标 `MCPFlags(…, bHidden=true)`，要么进白名单写清原因。

不需要动：`UnrealMCPBridge.cpp`（已无按域路由）、桥本地的任何清单、`UnrealMCPPythonAPI` 的回环名单。

## flags 的含义（策略的唯一来源）

| flag | 含义 | 谁在读 |
|---|---|---|
| `bLoopbackForbidden` | 编辑器内回环调用会被拒（会重新排队 GameThread / 进 python VM） | `UnrealMCPPythonAPI::ExecuteMCPCommand` |
| `bMutatesGraph` | 会改图/资产：material 用它决定编译边界 stamp；particle/node 用它决定 undo 事务 | 各域 `RunCommand` + `Domain` 策略 |
| `bHidden` | 协议/内部命令，不进 agent 工具面（仍可调用、仍出现在自省里） | 工具面导出 |
| `bPersistAfterSuccess` | 成功后把所属蓝图写盘（编辑器会被构建脚本强杀） | blueprint / blueprint_node `RunCommand` |

## 门禁怎么跑

```bat
python Build\check_command_consistency.py            # 全量检查（构建脚本尾部也会跑）
python Build\check_command_consistency.py --warn-only # 只报告不失败（过渡期用）
python Build\check_command_consistency.py --baseline  # 打印"旧链命令名 vs 注册集合"差集
```

判定口径：

- **ERROR（退出码 1）**：python 发送了未注册的命令；**已注册、未标 `hidden`、且没有任何 python 工具发送的命令**（死命令，或漏了工具包装）；两个 `@mcp.tool` 重名；技能文档点名了既不是工具也不是命令的名字；**技能/规则的 YAML frontmatter 解析失败**（agent 的 skill loader 会解析它，本门禁原先只做文本扫描 ⇒ 缺陷直到加载时才炸，实测 `unreal-video-capture`）；工具没有暴露它转发命令的必填参数；**源码里重新出现 `CommandType == TEXT("...")` 形式的命令名比较**。
- **WARNING（不影响退出码）**：工具发送了命令没有内联声明的键（参数规格可能由 helper 构造，提取器看不见）；没有任何技能文档提到的工具。
- 白名单：`Build/command_consistency_allowlist.txt`，每行必须写原因；无原因的行本身报 ERROR。

**"未标 hidden 的死命令"为什么是 ERROR**：命令面就是文档对调用方的承诺，一个谁都不发的命令要么是死代码、要么是漏掉的工具包装 —— 两者都该在构建时红。
真正的协议/内部命令（`ping`）用 `MCPFlags(…, bHidden=true)` 声明，门禁认这个 flag（`Registry.Register({…})` 表形式与 `MCP_REGISTER_COMMAND` 宏两种写法都识别）。

**扫描口径（别把真发送点藏起来，藏了就会误报成死命令）**：

- 发送点按 `send_command` / `_send` / `_bridge` / `_bridge_result` / `send_mcp_command` 的**首个实参**识别；实参可以是字面量，也可以是模块级 `NAME = "literal"` 常量（`mcp_material_preview.py` 的 `PREVIEW_COMMAND` 就是这种）。
- 新增命令时，python 包装里请保持**字面量** `send_command("命令名", …)`：命令名变成局部变量会让门禁看不见这个发送点。
- 工具文档提及：反引号 token 若**已是一个已知工具/命令**名，允许带尾部调用签名（`` `add_widget(…)` `` 计为提及 `add_widget`）；未知标识符带签名则整条忽略（那是引擎/python API 引用，如 `get_bone_pose(...)`）。技能 frontmatter `description` 的触发清单（`/` 分隔段）同样计入提及。
  ⇒ **引用引擎 / python API 时要么写成可调用形状，要么带属主点号**：`unreal.LevelEditorSubsystem.load_level(…)`、`unreal.MovieSceneTrackExtensions.get_sections(…)`。
  反面写法是"反引号包一个裸的小写动词名、后面不跟 `(`"（`load_level`、`get_sections`、`get_possessables`…）：那会被当成**点名了一个不存在的工具**而报 ERROR。
  `unreal-video-capture` 的 8 条 MRQ/引擎 API 引用就是这么踩的（2026-10-15 已按上面两种形状改写，门禁转 OK）。
  个别确实无法改写的可以进白名单，但必须写清原因（例如 `compile_performed` 是某个命令的**响应字段**名）。


## 计数口径

脚本只扫**命令面**：`Content/Python/tools`、`Content/Python/scripts`、`Content/Python/*.py`、`Source/UnrealMCP`、`.claude/skills`。
`Content/Python/.venv`（约 1200 个 site-packages 文件）被显式排除 —— 曾经因为扫它 + 在循环里重算收集器，一次运行要 60 秒；现在约 0.5 秒。
