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

from tools._bridge import _bridge

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
        params: Dict[str, Any] = {}
        if category:
            params["category"] = category

        return _bridge("list_mcp_commands", params)

    @mcp.tool()
    def get_command_journal(
        ctx: Context,
        limit: Optional[int] = 64,
        category: Optional[str] = None,
    ) -> Dict[str, Any]:
        """
        Recent dispatch history from the editor: what ran, how long it took, and how it ended.

        Read-only. Two uses:

        - After something went wrong: `error_code` per entry tells a rejection (missing_params /
          bad_param_type) from a command that failed on its own; `in_flight` is the last command that
          started and never finished — if the channel is silent too, that is the command that wedged
          the GameThread.
        - Before designing a script: `by_command` gives calls / total / max / avg milliseconds per
          command over the current ring, which is where "this needs splitting into batches" or "this
          needs a longer timeout" comes from.

        Args:
            limit: How many recent entries to return (default 64, capped by the ring capacity).
            category: Only this command category (e.g. "blueprint", "material"). Omit for all.

        Returns:
            entries[] (newest first: seq / command / category / started / finished / duration_ms /
            error_code / params_size / params_hash), entry_count, ring_capacity, ring_count,
            in_flight (or null), by_command[] (command / calls / total_ms / max_ms / avg_ms, sorted by
            max_ms desc) and journal_path (the append-only JSONL mirror that survives a killed editor).
        """
        params: Dict[str, Any] = {}
        if limit is not None:
            params["limit"] = int(limit)
        if category:
            params["category"] = category

        return _bridge("get_command_journal", params)
