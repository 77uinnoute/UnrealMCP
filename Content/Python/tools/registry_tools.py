"""
Registry Tools for Unreal MCP.

Exposes the command introspection tool. The answer comes straight from the C++ command registry, so
it is the authoritative list of what this bridge accepts - use it instead of reading the source or
guessing a name. It also reports each command's policy flags (re-entrant over the python loopback,
mutates a graph, hidden from the tool surface).
"""

import logging
from typing import Any, Dict, Optional

from mcp.server.fastmcp import FastMCP, Context

# Get logger
logger = logging.getLogger("UnrealMCP")


def register_registry_tools(mcp: FastMCP):
    """Register command-registry tools with the MCP server."""

    @mcp.tool()
    def list_mcp_commands(
        ctx: Context,
        category: Optional[str] = None,
    ) -> Dict[str, Any]:
        """
        List the bridge commands with their parameters and policy flags.

        Read-only; it touches no asset, object or graph. Use it to confirm a command name or its
        parameters before calling it, and to see which commands are hidden from this tool surface
        (they still work when called directly).

        LAYER BOUNDARY: this covers the *registered command layer* only. The engine's editor python
        API (IKRigController, IKRetargeterController, MaterialEditingLibrary, ...) is deliberately
        absent - read it with python_api_index / python_api_doc instead of guessing signatures.

        Known traps this list does NOT encode (on purpose: there is no prose field per command, and
        adding one would grow every response to save a single mis-call):
          - material graph: connect_material_pin and connect_material_expressions_safe name their
            arguments differently for the same job - check the params here before switching between them
          - pin defaults: an object/class pin takes an asset *path string*, not an object reference
          - enum-valued parameters accept both the C++ spelling and the python snake_case one
          - import_assets defaults to replace_existing=false; a same-name target fails per file and
            the pre-existing asset is never reported as this import's product
          - execute_python_* are strictly synchronous (no job mode): the call must finish inside
            the client timeout, so heavy work has to be split into several short calls

        Args:
            category: Optional filter, e.g. "material", "particle", "blueprint_node", "asset", "mcp".
                An unknown category returns an empty list rather than an error.

        Returns:
            Dict with command_count and commands [{name, category, description, params
            [{name, type, required, default?, allowed_values?, description?}], flags
            {loopback_forbidden, mutates_graph, hidden}}].
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params: Dict[str, Any] = {}
            if category:
                params["category"] = category

            response = unreal.send_command("list_mcp_commands", params)
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            logger.error(f"Error listing MCP commands: {e}")
            return {"success": False, "message": str(e)}
