# UnrealMCP

Unreal Engine 5.5 - 5.8 editor plugin + Model Context Protocol (MCP) server that lets an AI agent drive the editor:
Blueprints (graphs, nodes, pins, components), materials, UMG widgets, animation sequences and montages,
skeletal meshes and retargeting, cloth/physics, Cascade particle systems, PCG graphs, levels and lighting,
plus asset import/move/delete — through a single structured command surface exposed as MCP tools.

Two halves:

| Half | What it is | Where |
|---|---|---|
| Bridge (C++) | Editor plugin: a local TCP server that dispatches the commands | `Source/UnrealMCP` |
| MCP server (Python) | FastMCP server: forwards MCP tool calls to the bridge | `Content/Python/unreal_mcp_server.py`, `Content/Python/tools` |

The bridge listens on `127.0.0.1:55557` once the editor has loaded the plugin.

## Requirements

- Unreal Engine 5.5 - 5.8 (installed build). All four build from this one source tree: every engine API that
  moved between those releases is guarded in `Source/UnrealMCP/Private/Compat/UnrealMCPVersionCompat.h`, so a new
  engine version is a change there rather than a hunt through the command files.
- Windows (the bridge and the build scripts are Windows-first).
- A C++ toolchain UnrealBuildTool accepts. Visual Studio 2022 is the supported one; newer Visual Studio releases work
  as long as they ship the MSVC v143 toolset (UBT picks that toolset up and reports it as `Visual Studio 2022 14.3x`).
- Python with the `mcp` package (see `Content/Python/pyproject.toml`); `PythonScriptPlugin` must be enabled in the editor.

## Install

1. Put this folder at `<YourProject>/Plugins/UnrealMCP`.
2. Enable the plugin (Plugins → UnrealMCP) and restart the editor. The log prints
   `UnrealMCPBridge: Server started on 127.0.0.1:55557`.
3. Point your MCP client at `Content/Python/unreal_mcp_server.py`, using a Python that has the `mcp` package:

   ```json
   {
     "mcpServers": {
       "unrealMCP": {
         "command": "python",
         "args": ["<Project>/Plugins/UnrealMCP/Content/Python/unreal_mcp_server.py"]
       }
     }
   }
   ```

   `-m sse` switches the transport away from the default stdio.

## Build from source

- Plugin only: `Build_UnrealMCP.bat` (RunUAT `BuildPlugin`, then deploys DLL/PDB/modules to `Binaries/Win64`).
  Pass an engine path as the first argument when the engine is not at the default location.
- Host project module (only if the project has its own `Source/`): `Build_Project.bat`.
- Both scripts close a running editor first, and both write `Build/build_status.txt`; `Build/wait_status.bat` polls for it.

## Using it

- `list_mcp_commands` returns the whole command surface with parameter specs; `python_api_index` / `python_api_doc`
  introspect the editor's Python API. `Source/UnrealMCP/Public/Core/MCPCommandRegistry.h` is the single declaration point.
- Editing anything under `Content/Python` requires restarting the MCP server; editing C++ requires a rebuild plus an editor restart.

## Skills

`.claude/skills/<name>/SKILL.md` — ten task-oriented guides (Blueprint, material, UMG, animation, cloth/physics,
level & lighting, asset pipeline, particle, PCG, retargeting). Each is a plain Agent Skill: YAML frontmatter
(`name`, `description`) plus Markdown, so any agent that reads `.claude/skills` can pick the right one.

## License

MIT — see [LICENSE](LICENSE).
