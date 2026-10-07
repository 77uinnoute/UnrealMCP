"""
PIE (play-in-editor) lifecycle tools for Unreal MCP.

Thin pass-throughs to the bridge's `pie` commands. The contract that matters lives in the bridge:

  - start_pie never waits: PIE comes up on a later frame, so poll `pie_running` with short calls.
    A command that "waits for ready" can only wait by sleeping on the GameThread, which freezes the
    whole MCP channel.
  - get_actor_pose reports unknown bone names in `missing[]` instead of falling back to the actor
    transform (that fallback is what turns a typo into a plausible constant pose).
  - inject_key pushes the event through the game viewport like a physical key. It reports the
    viewport's raw return value (`viewport_handled`), which reads false for a key that feeds an
    axis mapping. There is no same-frame delivery readback - the event is processed later in the
    frame - so the only proof is reading the game's own state back.
"""

import logging
from typing import Dict, Any, List
from mcp.server.fastmcp import FastMCP, Context

# Get logger
logger = logging.getLogger("UnrealMCP")


def register_pie_tools(mcp: FastMCP):
    """Register PIE lifecycle tools with the MCP server."""

    @mcp.tool()
    def start_pie(ctx: Context) -> Dict[str, Any]:
        """
        Request a PIE session and return immediately. Does NOT wait for it to be ready.

        Poll `pie_running` with repeated short calls to know when the play world exists; a command
        that waited here would have to sleep/poll on the GameThread and freeze every other MCP call.

        Returns:
            Dict with `requested` (true when this call asked for the session),
            `was_already_running` and `pie_running` (its value right now).
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            logger.info("start_pie requested")
            response = unreal.send_command("start_pie", {})
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            return response

        except Exception as e:
            error_msg = f"Error starting PIE: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def stop_pie(ctx: Context) -> Dict[str, Any]:
        """
        Request the end of the PIE session and return immediately (does not wait for tear-down).

        Poll `pie_running` until it is false. Until then, runtime reads still answer from the live
        session; afterwards get_actor_pose fails with `not_in_pie` rather than returning stale data.

        Returns:
            Dict with `was_running` and `pie_running` (its value right now).
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            logger.info("stop_pie requested")
            response = unreal.send_command("stop_pie", {})
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            return response

        except Exception as e:
            error_msg = f"Error stopping PIE: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def get_actor_pose(ctx: Context, actor_label: str, bones: List[str]) -> Dict[str, Any]:
        """
        World-space locations of the named bones on a PIE actor.

        Use it to answer "did the pose actually move / is it frozen": `frame` is the read-time frame
        counter, so two identical readings mean nothing advanced.

        A name that is not a bone of that actor's skeleton comes back in `missing[]` and is NOT
        substituted with a fallback transform - the substitute looks plausible and reads like a
        frozen rig, which is exactly the misdiagnosis this avoids.

        Args:
            actor_label: Actor label in the PIE world (a spawned clone keeps the source label).
            bones: Bone names to sample, e.g. ["pelvis", "head"].

        Returns:
            Dict with found[], missing[], poses{bone: [x, y, z]}, frame, world_time_seconds.
            Outside PIE it fails with error=not_in_pie.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            if not actor_label:
                return {"success": False, "error": "invalid_params",
                        "message": "actor_label is required"}
            if not bones:
                return {"success": False, "error": "invalid_params",
                        "message": "bones must list at least one bone name"}

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            logger.info(f"get_actor_pose: {actor_label} {bones}")
            response = unreal.send_command(
                "get_actor_pose", {"actor_label": actor_label, "bones": list(bones)})
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            return response

        except Exception as e:
            error_msg = f"Error reading actor pose: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def inject_key(ctx: Context, key: str, event: str = "tap") -> Dict[str, Any]:
        """
        Press / release / tap a KEY in the running PIE session.

        This sits on the key side of a key->action binding, which makes it the injection that
        proves the MAPPING. An action-level inject only proves the action reaches logic; the key
        is what proves the binding itself. Events go through the game viewport - the same door a
        physical key uses, SetIgnoreInput gate included - so a UI-focused or input-ignoring state
        blocks them exactly as it blocks a person.

        `viewport_handled` is only the viewport's return value, and it reads FALSE for a key that
        feeds an axis mapping even though the input is used (measured: W returned false on a
        DefaultPawn and the pawn then flew) - never read it as success. There is also no same-frame
        delivery readback: the event is processed later in the frame, so anything read during this
        call reflects the state from BEFORE it (measured: false on the press frame, true on the
        release frame, while the input worked both times). Which binding reacted is not knowable
        from here: read the game's own state back (`get_actor_pose`, the pawn's location, a
        gameplay readout) - that is the only proof.

        Args:
            key: FKey name - the key, not the mapped action. e.g. "W", "SpaceBar",
                "LeftMouseButton".
            event: press / release / tap (default tap = press then release).

        Returns:
            Dict with key, event, viewport_handled, events[] (per-event viewport_handled),
            player_input_class, frame, world_time_seconds. Outside a play session:
            error=not_in_pie; a name that is not an FKey: error=unknown_key.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            if not key:
                return {"success": False, "error": "invalid_params",
                        "message": "key is required (the FKey name, e.g. \"W\")"}

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            logger.info(f"inject_key: {key} ({event})")
            response = unreal.send_command("inject_key", {"key": key, "event": event})
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            return response

        except Exception as e:
            error_msg = f"Error injecting key: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
