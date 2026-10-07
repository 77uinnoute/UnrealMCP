"""
Engine python API introspection tools for Unreal MCP.

The MCP command registry describes only the bridge's own commands; the engine plugins' python API
(IKRigEditor / IKRetargeter / ...) is invisible to `list_mcp_commands`, so callers end up guessing
signatures and paying in TypeErrors. These two tools read the live python type instead.

Both are read-only pass-throughs: the bridge owns the fields, python only forwards parameters.
Layering: these cover the engine python API layer, `list_mcp_commands` covers the registered
command layer.
"""

import logging
from typing import Dict, Any, Optional
from mcp.server.fastmcp import FastMCP, Context

# Get logger
logger = logging.getLogger("UnrealMCP")


def register_python_api_tools(mcp: FastMCP):
    """Register engine python API introspection tools with the MCP server."""

    @mcp.tool()
    def python_api_index(ctx: Context, class_name: str) -> Dict[str, Any]:
        """
        List every documented member of an engine python-exposed class, with its signature line
        and docstring, plus the module that owns it (e.g. "/Script/IKRigEditor").

        Use this before calling an engine python API you are not sure about: no `dir()` reflection
        script needed, and a misspelled class comes back with candidate names instead of a
        TypeError from a failed call.

        Args:
            class_name: Python class name as exposed by the editor python `unreal` module,
                e.g. "IKRigController", "IKRetargeterController".

        Returns:
            Dict with class_name, module, functions [{function, signature, doc}] and count.
            An unknown class returns success=false + error=unknown_class + candidates.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            if not class_name:
                return {"success": False, "error": "missing_class_name",
                        "message": "class_name is required"}

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            logger.info(f"python_api_index: {class_name}")
            response = unreal.send_command("python_api_index", {"class_name": class_name})
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            return response

        except Exception as e:
            error_msg = f"Error reading python API index: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def python_api_doc(ctx: Context, class_name: str, function: Optional[str] = None,
                       function_name: Optional[str] = None) -> Dict[str, Any]:
        """
        Signature and docstring of ONE member of an engine python-exposed class.

        Read-only; on a miss the response carries the closest candidate names
        (error=unknown_function / unknown_class) so the next call can be right instead of lucky.

        Args:
            class_name: Python class name, e.g. "IKRetargeterController".
            function: Member name, e.g. "set_preview_mesh".
            function_name: Alias of `function` (the bridge parameter is named `function`); pass
                either, not both - `function` wins.

        Returns:
            Dict with class_name, function, module, signature, doc. A missing/illegal parameter
            comes back with an example call to copy.
        """
        from unreal_mcp_server import get_unreal_connection

        member = function or function_name
        try:
            if not class_name or not member:
                return {"success": False, "error": "missing_parameter",
                        "message": "class_name and function are both required "
                                   "('function_name' is accepted as an alias of 'function')",
                        "example": 'python_api_doc(class_name="IKRetargeterController", '
                                   'function="reset_retarget_pose")'}

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            logger.info(f"python_api_doc: {class_name}.{member}")
            response = unreal.send_command(
                "python_api_doc", {"class_name": class_name, "function": member})
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            return response

        except Exception as e:
            error_msg = f"Error reading python API doc: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
