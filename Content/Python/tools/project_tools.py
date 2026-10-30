"""
Project Tools for Unreal MCP.

This module provides tools for managing project-wide settings and configuration.
"""

import logging
from typing import Dict, Any
from mcp.server.fastmcp import FastMCP, Context

from tools._bridge import _bridge

# Get logger
logger = logging.getLogger("UnrealMCP")

def register_project_tools(mcp: FastMCP):
    """Register project tools with the MCP server."""
    
    @mcp.tool()
    def create_input_mapping(
        ctx: Context,
        action_name: str,
        key: str
    ) -> Dict[str, Any]:
        """
        Create an ACTION input mapping for the project (axis mappings are not supported here).

        Args:
            action_name: Name of the input action
            key: Key to bind (SpaceBar, LeftMouseButton, etc.)

        Returns:
            Response with action_name, key and `persisted`. `persisted: true` means the mapping was
            written to Config/DefaultInput.ini and therefore survives an editor restart; when it is
            false the response carries a `warning` that the mapping is only live for this session
            (blueprint InputAction nodes referring to it would fail to compile after a restart).
        """
        # No `input_type`: it used to be sent as "Action"/"Axis" and read by nobody - the command
        # builds an FInputActionKeyMapping whatever it says, so an "Axis" request silently produced
        # an action binding. Better to not offer the knob than to accept and ignore it.
        logger.info(f"Creating input mapping '{action_name}' with key '{key}'")
        return _bridge("create_input_mapping", {"action_name": action_name, "key": key})
    
    logger.info("Project tools registered successfully") 