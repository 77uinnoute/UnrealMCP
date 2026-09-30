# Agent notes — UnrealMCP

- **Skills**: `.claude/skills/<name>/SKILL.md` (YAML frontmatter with `name` / `description`). Load the skill that
  matches the task before calling those MCP tools; the tool names in its `description` are the trigger.
- **Builds**: `Build_UnrealMCP.bat` builds the plugin via RunUAT and deploys `Binaries/Win64`;
  `Build_Project.bat` builds the host project's own editor target. Both close a running editor first and write
  `Build/build_status.txt`; poll that file with `Build/wait_status.bat` instead of sleeping.
- **Runtime**: the bridge is a TCP server on `127.0.0.1:55557`; the MCP server is `Content/Python/unreal_mcp_server.py`
  (FastMCP, stdio by default). `Content/Python/tools/*.py` are the MCP tool wrappers.
- **Command surface**: `Source/UnrealMCP/Public/Core/MCPCommandRegistry.h` is the single place a command is declared.
  `Build/check_command_consistency.py` is the gate that keeps registry, python send sites, tools and docs in sync
  (see `Build/README.md`).
- **Restart rules**: any change under `Content/Python` needs an MCP server restart; any C++ change needs a rebuild
  plus an editor restart.
