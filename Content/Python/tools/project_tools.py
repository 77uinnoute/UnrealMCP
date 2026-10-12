"""
Project Tools for Unreal MCP.

This module provides tools for managing project-wide settings and configuration.
"""

import logging
from typing import Dict, Any
from mcp.server.fastmcp import FastMCP, Context

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
        from unreal_mcp_server import get_unreal_connection
        
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            # No `input_type`: it used to be sent as "Action"/"Axis" and read by nobody - the command
            # builds an FInputActionKeyMapping whatever it says, so an "Axis" request silently produced
            # an action binding. Better to not offer the knob than to accept and ignore it.
            params = {
                "action_name": action_name,
                "key": key
            }
            
            logger.info(f"Creating input mapping '{action_name}' with key '{key}'")
            response = unreal.send_command("create_input_mapping", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Input mapping creation response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error creating input mapping: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
    
    logger.info("Project tools registered successfully") 