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

不需要动：`UnrealMCPBridge.cpp`（已无按域路由）、桥本地的任何清单、`UnrealMCPPythonAPI` 的回环名单。

## flags 的含义（策略的唯一来源）

| flag | 含义 | 谁在读 |
|---|---|---|
| `bLoopbackForbidden` | 编辑器内回环调用会被拒（会重新排队 GameThread / 进 python VM） | `UnrealMCPPythonAPI::ExecuteMCPCommand` |
| `bMutatesGraph` | 会改图/资产：material 用它决定编译边界 stamp；particle/node 用它决定 undo 事务 | 各域 `RunCommand` + `Domain` 策略 |
| `bHidden` | 协议/内部命令，不进 agent 工具面（仍可调用、仍出现在自省里） | 工具面导出 |
| `bPersistAfterSuccess` | 成功后把所属蓝图写盘（编辑器会被构建脚本强杀） | blueprint / blueprint_node `RunCommand` |
| `bPythonExecution` | 会跑 python，其工作可排成 deferred job（`params.deferred`） | `UUnrealMCPBridge::ExecuteCommand` 的预处理 |

## 门禁怎么跑

```bat
python Build\check_command_consistency.py            # 全量检查（构建脚本尾部也会跑）
python Build\check_command_consistency.py --warn-only # 只报告不失败（过渡期用）
python Build\check_command_consistency.py --baseline  # 打印"旧链命令名 vs 注册集合"差集
```

判定口径：

- **ERROR（退出码 1）**：python 发送了未注册的命令；两个 `@mcp.tool` 重名；技能文档点名了既不是工具也不是命令的名字；工具没有暴露它转发命令的必填参数；**源码里重新出现 `CommandType == TEXT("...")` 形式的命令名比较**。
- **WARNING（不影响退出码）**：已注册但没有任何 python 工具发送、且未标 `hidden` 的命令（可能是死命令）；没有任何技能文档提到的工具。
- 白名单：`Build/command_consistency_allowlist.txt`，每行必须写原因；无原因的行本身报 ERROR。

## 计数口径

脚本只扫**命令面**：`Content/Python/tools`、`Content/Python/scripts`、`Content/Python/*.py`、`Source/UnrealMCP`、`.claude/skills`。
`Content/Python/.venv`（约 1200 个 site-packages 文件）被显式排除 —— 曾经因为扫它 + 在循环里重算收集器，一次运行要 60 秒；现在约 0.5 秒。
