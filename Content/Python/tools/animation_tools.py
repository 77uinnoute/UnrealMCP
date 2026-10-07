"""
Animation tools for Unreal MCP: `UAnimSequence` and `UAnimMontage` assets.

Reads and edits animation sequences (discovery, sampling, curves, bone tracks, notifies, notify
tracks, sync markers, additive / root motion settings, compression) and animation montages
(sections, slot tracks, animation segments, notifies, branching points, blend settings). It also
drives the editor session of either asset type (open the Persona tab, move/play the preview, jump
to a montage section). Every tool forwards to the bridge command of the same name with the
parameters unchanged, so the C++ kernel is the single source of behaviour and error codes.

Why these commands exist at all: animation data lives behind
`IAnimationDataModel` / `IAnimationDataController`, which are `#if WITH_EDITOR` pure C++
interfaces - `set_editor_property` on a sequence returns read-only copies of the internal
arrays, so python cannot author animation data on its own. A montage keeps its content in
`CompositeSections` / `SlotAnimTracks` / `Notifies`, which python cannot reach either.

Notifies, sections, slot tracks and sync markers are addressed by NAME first (`notify_name`,
`section_name`, `slot_name`, `marker_name`); array indices drift as soon as anything is
inserted, removed or sorted, so every write answers with the index it ended up at and the
fields it read back. A failed lookup returns `notify_not_found` / `ambiguous_notify` /
`section_not_found` / `slot_track_not_found` / `segment_not_found` / `sync_marker_not_found`
with the `candidates` that exist.

The notify commands are shared by the two asset types (`list_notifies`, `add_notify`,
`add_notify_state`, `remove_notify`, `set_notify_trigger_time`, `get_root_motion_at_time`):
pass a montage and they answer with `is_montage: true` plus the montage-only fields
(`linked_section_name`, `is_branching_point`). Branching points and `set_notify_link_to_section`
are montage-only.

Write commands save the asset on success (`saved`) and answer with a readback of what they
changed. Structural montage writes (section, slot track and segment add/remove) close the
asset's Persona editor first and report `editor_closed`, because Persona's preview holds raw
pointers into those arrays. `compress_animation` recompresses synchronously and answers with
`compressed_data_valid` and `compressed_size`; it never reports "started".

The editor-session commands (`open_*_editor`, `refresh_montage_editor`, `set_preview_time`,
`play_preview`, `stop_preview`, `jump_to_section`) are the exception to "writes save": they change
what the editor shows, not what the asset holds, so they never save and they answer
`editor_not_open` when no Persona session holds the asset instead of silently doing nothing.
"""

import logging
from typing import Any, Dict, List, Optional
from mcp.server.fastmcp import FastMCP, Context

logger = logging.getLogger("UnrealMCP")


def register_animation_tools(mcp: FastMCP):
    """Register animation sequence tools with the MCP server."""

    def _send(command: str, params: Dict[str, Any]) -> Optional[Dict[str, Any]]:
        """Forward one command to the bridge; returns None when there is no connection."""
        from unreal_mcp_server import get_unreal_connection

        unreal = get_unreal_connection()
        if not unreal:
            return None

        return unreal.send_command(command, params)

    def send_command(command: str, params: Dict[str, Any]) -> Dict[str, Any]:
        """Send one bridge command; always answers with a dict, never None and never raising."""
        try:
            response = _send(command, params)
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error calling {command}: {e}")
            return {"success": False, "message": str(e)}

    def _notify_locator(notify_index: int = -1, notify_name: str = "", guid: str = "",
                        time: float = None) -> Dict[str, Any]:
        params: Dict[str, Any] = {
            "notify_index": notify_index,
            "notify_name": notify_name or "",
            "guid": guid or "",
        }
        if time is not None:
            params["time"] = time
        return params

    def _section_locator(section_name: str = "", section_index: int = -1) -> Dict[str, Any]:
        return {"section_name": section_name or "", "section_index": section_index}

    def _slot_locator(track_index: int = -1, slot_name: str = "") -> Dict[str, Any]:
        return {"track_index": track_index, "slot_name": slot_name or ""}

    def _segment_locator(track_index: int = -1, segment_index: int = -1,
                         slot_name: str = "") -> Dict[str, Any]:
        return {"track_index": track_index, "segment_index": segment_index, "slot_name": slot_name or ""}

    # ------------------------------------------------------------------
    # Discovery and properties
    # ------------------------------------------------------------------

    @mcp.tool()
    def anim_self_check(ctx: Context) -> Dict[str, Any]:
        """
        Liveness probe for the animation command domain (read-only).

        Returns:
            Dict with ok, engine_version and registered_anim_commands. If this fails while
            other domains answer, the animation domain did not register.
        """
        return send_command("anim_self_check", {})

    @mcp.tool()
    def list_anim_sequences(ctx: Context, search_path: str = "/Game", skeleton: str = "",
                            max_results: int = 200) -> Dict[str, Any]:
        """
        List animation sequences below a folder (read-only).

        Uses asset registry metadata only: the assets are never loaded, so a broad search does
        not pull the library into memory. Length / frame_rate therefore come from the asset
        tags, and only for entries the registry carries them for.

        Args:
            search_path: Content folder to search (default /Game)
            skeleton: Optional skeleton path filter
            max_results: Upper limit on returned entries (default 200)

        Returns:
            Dict with found_count, returned_count, truncated and sequences (asset_path, name,
            skeleton, length, frame_rate, info_source).
        """
        return send_command("list_anim_sequences", {"search_path": search_path, "skeleton": skeleton,
                                            "max_results": max_results})

    @mcp.tool()
    def get_anim_sequence_info(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Full summary of one animation sequence (read-only).

        Args:
            asset_path: Sequence asset path (e.g. /Game/Animations/MM_Idle)

        Returns:
            Dict with asset_path, skeleton, length, frame_rate, frame_count, bone_track_count,
            curve_count, notify_count, notify_track_count, sync_marker_count, rate_scale,
            enable_root_motion, root_motion_root_lock, additive_anim_type, additive_base_pose,
            raw_size, compressed_size, compression_scheme.
        """
        return send_command("get_anim_sequence_info", {"asset_path": asset_path})

    @mcp.tool()
    def find_animations_for_skeleton(ctx: Context, skeleton: str, search_path: str = "/Game",
                                    max_results: int = 200) -> Dict[str, Any]:
        """
        List the sequences that use a skeleton (read-only, registry metadata only).

        Args:
            skeleton: Skeleton asset path
            search_path: Content folder to search (default /Game)
            max_results: Upper limit on returned entries (default 200)

        Returns:
            Dict with skeleton, found_count, returned_count, truncated and sequences.
        """
        return send_command("find_animations_for_skeleton",
                     {"skeleton": skeleton, "search_path": search_path, "max_results": max_results})

    @mcp.tool()
    def search_animations(ctx: Context, query: str, search_path: str = "/Game",
                          max_results: int = 200) -> Dict[str, Any]:
        """
        Find sequences by asset name (read-only).

        Args:
            query: Name fragment, or a wildcard pattern (MM_*)
            search_path: Content folder to search (default /Game)
            max_results: Upper limit on returned entries (default 200)

        Returns:
            Dict with query, found_count, returned_count, truncated and sequences.
        """
        return send_command("search_animations", {"query": query, "search_path": search_path,
                                          "max_results": max_results})

    @mcp.tool()
    def get_animation_length(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Read the play length of a sequence in seconds (read-only).

        Returns: Dict with asset_path and length.
        """
        return send_command("get_animation_length", {"asset_path": asset_path})

    @mcp.tool()
    def get_animation_frame_rate(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Read the sampling frame rate of a sequence (read-only).

        Returns: Dict with asset_path, frame_rate (decimal) and frame_rate_fraction.
        """
        return send_command("get_animation_frame_rate", {"asset_path": asset_path})

    @mcp.tool()
    def get_animation_frame_count(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Read the number of sampled keys of a sequence (read-only).

        Returns: Dict with asset_path and frame_count.
        """
        return send_command("get_animation_frame_count", {"asset_path": asset_path})

    @mcp.tool()
    def get_animation_skeleton(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Read the skeleton a sequence uses (read-only).

        Returns: Dict with asset_path and skeleton.
        """
        return send_command("get_animation_skeleton", {"asset_path": asset_path})

    @mcp.tool()
    def get_rate_scale(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Read the playback rate scale of a sequence (read-only).

        Returns: Dict with asset_path and rate_scale.
        """
        return send_command("get_rate_scale", {"asset_path": asset_path})

    @mcp.tool()
    def get_animated_bones(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        List the skeleton bone names of a sequence (read-only).

        Returns: Dict with bone_count and bones (reference skeleton order). Cross-check a
        bone_name against this list before sampling or writing a track.
        """
        return send_command("get_animated_bones", {"asset_path": asset_path})

    # ------------------------------------------------------------------
    # Sampling
    # ------------------------------------------------------------------

    @mcp.tool()
    def get_bone_transform_at_time(ctx: Context, bone_name: str, asset_path: str = "",
                                   time: float = None, frame: int = None) -> Dict[str, Any]:
        """
        Sample one bone's local transform at a time in the sequence (read-only).

        Raw authored data is used, so a freshly created (uncompressed) sequence answers too.
        Pass either `time` (seconds) or `frame`; time is clamped to the sequence length.

        Args:
            bone_name: Bone to sample, as returned by get_animated_bones
            asset_path: Sequence asset path
            time: Time in seconds
            frame: Frame number (alternative to time)

        Returns:
            Dict with bone_name, bone_index, time, frame and transform
            (position / rotation (euler) / quaternion / scale).
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "bone_name": bone_name}
        if time is not None:
            params["time"] = time
        if frame is not None:
            params["frame"] = frame
        return send_command("get_bone_transform_at_time", params)

    @mcp.tool()
    def get_bone_transform_at_frame(ctx: Context, bone_name: str, asset_path: str = "",
                                    frame: int = None, time: float = None) -> Dict[str, Any]:
        """
        Sample one bone's local transform at a frame (read-only).

        Same as get_bone_transform_at_time; `frame` is documented as the primary argument.

        Args:
            bone_name: Bone to sample
            asset_path: Sequence asset path
            frame: Frame number
            time: Time in seconds (alternative to frame)

        Returns:
            Dict with bone_name, bone_index, time, frame and transform.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "bone_name": bone_name}
        if frame is not None:
            params["frame"] = frame
        if time is not None:
            params["time"] = time
        return send_command("get_bone_transform_at_frame", params)

    @mcp.tool()
    def get_pose_at_time(ctx: Context, asset_path: str = "", time: float = None,
                         frame: int = None) -> Dict[str, Any]:
        """
        Sample the whole local-space pose of a sequence at a time (read-only).

        One entry per reference-skeleton bone; this is raw data, not the compressed runtime
        pose, and it is the raw reading (no semantic interpretation of the pose).

        Args:
            asset_path: Sequence asset path
            time: Time in seconds
            frame: Frame number (alternative to time)

        Returns:
            Dict with time, bone_count and bones (bone_name, bone_index, transform).
        """
        params: Dict[str, Any] = {"asset_path": asset_path}
        if time is not None:
            params["time"] = time
        if frame is not None:
            params["frame"] = frame
        return send_command("get_pose_at_time", params)

    @mcp.tool()
    def get_pose_at_frame(ctx: Context, asset_path: str = "", frame: int = None,
                          time: float = None) -> Dict[str, Any]:
        """
        Sample the whole local-space pose of a sequence at a frame (read-only).

        Args:
            asset_path: Sequence asset path
            frame: Frame number
            time: Time in seconds (alternative to frame)

        Returns:
            Dict with time, bone_count and bones.
        """
        params: Dict[str, Any] = {"asset_path": asset_path}
        if frame is not None:
            params["frame"] = frame
        if time is not None:
            params["time"] = time
        return send_command("get_pose_at_frame", params)

    @mcp.tool()
    def get_root_motion_at_time(ctx: Context, asset_path: str = "", time: float = None,
                                frame: int = None) -> Dict[str, Any]:
        """
        Root motion accumulated from the sequence start up to a time (read-only).

        Returns zero translation when the sequence has root motion disabled; check
        enable_root_motion in the response before drawing conclusions.

        Returns:
            Dict with time, enable_root_motion and root_motion
            (translation / rotation / quaternion / scale).
        """
        params: Dict[str, Any] = {"asset_path": asset_path}
        if time is not None:
            params["time"] = time
        if frame is not None:
            params["frame"] = frame
        return send_command("get_root_motion_at_time", params)

    @mcp.tool()
    def get_total_root_motion(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Root motion accumulated over the whole sequence (read-only).

        Returns:
            Dict with length, enable_root_motion and root_motion.
        """
        return send_command("get_total_root_motion", {"asset_path": asset_path})

    # ------------------------------------------------------------------
    # Curves
    # ------------------------------------------------------------------

    @mcp.tool()
    def list_curves(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        List the float curves of a sequence (read-only).

        Returns:
            Dict with curve_count, transform_curve_count and curves (curve_name, key_count,
            default_value, curve_type_flags, editable, metadata, disabled).
        """
        return send_command("list_curves", {"asset_path": asset_path})

    @mcp.tool()
    def get_curve_info(ctx: Context, curve_name: str, asset_path: str = "") -> Dict[str, Any]:
        """
        Read one curve's key count, default value and flags (read-only).

        Args:
            curve_name: Curve name, as listed by list_curves
            asset_path: Sequence asset path

        Returns:
            Dict with curve_name, key_count, default_value, curve_type_flags, editable,
            metadata, disabled. Errors: curve_not_found (candidates list the curves).
        """
        return send_command("get_curve_info", {"asset_path": asset_path, "curve_name": curve_name})

    @mcp.tool()
    def get_curve_value_at_time(ctx: Context, curve_name: str, time: float,
                                asset_path: str = "") -> Dict[str, Any]:
        """
        Evaluate one curve at a time (read-only).

        Args:
            curve_name: Curve name
            time: Time in seconds
            asset_path: Sequence asset path

        Returns:
            Dict with curve_name, time and value.
        """
        return send_command("get_curve_value_at_time",
                     {"asset_path": asset_path, "curve_name": curve_name, "time": time})

    @mcp.tool()
    def get_curve_keyframes(ctx: Context, curve_name: str, asset_path: str = "") -> Dict[str, Any]:
        """
        List the keys of one curve (read-only).

        Args:
            curve_name: Curve name
            asset_path: Sequence asset path

        Returns:
            Dict with curve_name, key_count and keys (time, value, arrive_tangent,
            leave_tangent, interp, tangent).
        """
        return send_command("get_curve_keyframes", {"asset_path": asset_path, "curve_name": curve_name})

    # ------------------------------------------------------------------
    # Notifies, notify tracks, sync markers (reads)
    # ------------------------------------------------------------------

    @mcp.tool()
    def list_notifies(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        List every notify of a sequence or a montage (read-only).

        Returns:
            Dict with notify_count, track_count, is_montage and notifies (notify_index,
            notify_name, notify_class, entry_type ("notify" / "notify_state"), trigger_time,
            duration, end_time, track_index, track_name, trigger_chance, weight_threshold,
            trigger_on_server, trigger_on_follower, filter_type, filter_lod, color, guid). A
            montage also reports linked_section_name and is_branching_point per notify.
        """
        return send_command("list_notifies", {"asset_path": asset_path})

    @mcp.tool()
    def get_notify_info(ctx: Context, asset_path: str = "", notify_index: int = -1,
                        notify_name: str = "", guid: str = "", time: float = None) -> Dict[str, Any]:
        """
        Read one notify, located by index, name or guid (read-only).

        Name is preferred over the index; when a name matches several notifies, pass `time` to
        pick one, otherwise the response is ambiguous_notify with every candidate listed.

        Args:
            asset_path: Sequence asset path
            notify_index: Index as returned by list_notifies
            notify_name: Notify name
            guid: Editor guid of the notify
            time: Trigger time, used to disambiguate a name

        Returns:
            Dict with notify (see list_notifies for its fields).
        """
        return send_command("get_notify_info", {"asset_path": asset_path,
                                         **_notify_locator(notify_index, notify_name, guid, time)})

    @mcp.tool()
    def list_notify_tracks(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        List the notify tracks of a sequence (read-only).

        UE 5.5 tracks carry real names, so the names reported here are the ones the editor
        shows and the ones rename_notify_track changes.

        Returns:
            Dict with track_count and tracks (track_index, track_name, color, notify_count,
            sync_marker_count).
        """
        return send_command("list_notify_tracks", {"asset_path": asset_path})

    @mcp.tool()
    def get_notify_track_count(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Read how many notify tracks a sequence has (read-only).

        Returns: Dict with track_count.
        """
        return send_command("get_notify_track_count", {"asset_path": asset_path})

    @mcp.tool()
    def list_sync_markers(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        List the authored sync markers of a sequence (read-only).

        Returns:
            Dict with marker_count and markers (marker_name, time, track_index).
        """
        return send_command("list_sync_markers", {"asset_path": asset_path})

    # ------------------------------------------------------------------
    # Settings (reads)
    # ------------------------------------------------------------------

    @mcp.tool()
    def get_additive_anim_type(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Read the additive animation type of a sequence (read-only).

        Returns: Dict with additive_anim_type (None / LocalSpace / MeshSpace).
        """
        return send_command("get_additive_anim_type", {"asset_path": asset_path})

    @mcp.tool()
    def get_additive_base_pose(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Read the additive base pose sequence (read-only).

        Returns: Dict with additive_base_pose (empty when the reference pose is used).
        """
        return send_command("get_additive_base_pose", {"asset_path": asset_path})

    @mcp.tool()
    def get_enable_root_motion(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Read whether root motion extraction is enabled (read-only).

        Returns: Dict with enable_root_motion.
        """
        return send_command("get_enable_root_motion", {"asset_path": asset_path})

    @mcp.tool()
    def get_root_motion_root_lock(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Read the root motion root lock mode (read-only).

        Returns: Dict with root_motion_root_lock (RefPose / AnimFirstFrame / Zero).
        """
        return send_command("get_root_motion_root_lock", {"asset_path": asset_path})

    @mcp.tool()
    def get_force_root_lock(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Read whether the root lock is forced (read-only).

        Returns: Dict with force_root_lock.
        """
        return send_command("get_force_root_lock", {"asset_path": asset_path})

    @mcp.tool()
    def get_compression_info(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Read compression settings and sizes (read-only).

        Returns:
            Dict with compression_scheme, curve_compression_scheme, raw_size, compressed_size,
            ratio and compressed_data_valid (False means the compressed data is stale).
        """
        return send_command("get_compression_info", {"asset_path": asset_path})

    @mcp.tool()
    def get_source_files(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        List the source files a sequence was imported from (read-only).

        Returns: Dict with file_count and files.
        """
        return send_command("get_source_files", {"asset_path": asset_path})

    @mcp.tool()
    def export_animation_to_json(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Dump a sequence's structure as one JSON document (read-only).

        Returns:
            Dict with json_length and json (a document holding sequence, bones, curves,
            transform_curve_count, notifies, notify_tracks and sync_markers). Useful to
            capture a baseline before edits, or to diff two sequences.
        """
        return send_command("export_animation_to_json", {"asset_path": asset_path})

    # ------------------------------------------------------------------
    # Writes: creation and properties
    # ------------------------------------------------------------------

    @mcp.tool()
    def create_anim_sequence(ctx: Context, name: str, skeleton: str, folder: str = "/Game",
                             frame_rate: float = 30.0, duration: float = 1.0,
                             tracks: List[Dict[str, Any]] = None,
                             auto_close: bool = False) -> Dict[str, Any]:
        """
        Create an animation sequence, optionally with authored bone tracks.

        The asset is created through the sequence factory (no half-created asset on failure)
        and the frame rate / frame count are set before any track is written.

        Args:
            name: Asset name
            skeleton: Skeleton asset path
            folder: Content folder (default /Game)
            frame_rate: Sampling frame rate (default 30)
            duration: Length in seconds (default 1)
            tracks: Optional bone tracks, each {bone_name, keys: [{time, position,
                rotation, scale}]}. Keys are resampled onto every frame.

        Returns:
            Dict with asset_path, created, tracks_written and the sequence summary
            (length, frame_rate, frame_count, bone_track_count, ...). Errors: asset_exists /
            asset_not_skeleton / create_failed / bone_not_found / invalid_value.
        """
        params: Dict[str, Any] = {"name": name, "skeleton": skeleton, "folder": folder,
                                  "frame_rate": frame_rate, "duration": duration}
        if tracks:
            params["tracks"] = tracks
        if auto_close:
            params["auto_close"] = True
        return send_command("create_anim_sequence", params)

    @mcp.tool()
    def create_anim_sequence_from_pose(ctx: Context, name: str, skeleton: str, folder: str = "/Game",
                                       frame_rate: float = 30.0, duration: float = 1.0,
                                       auto_close: bool = False) -> Dict[str, Any]:
        """
        Create a sequence holding the skeleton reference pose on every bone track.

        Args:
            name: Asset name
            skeleton: Skeleton asset path
            folder: Content folder (default /Game)
            frame_rate: Sampling frame rate (default 30)
            duration: Length in seconds (default 1)

        Returns:
            Dict with asset_path, created, from_reference_pose and the sequence summary.
        """
        params: Dict[str, Any] = {"name": name, "skeleton": skeleton, "folder": folder,
                                  "frame_rate": frame_rate, "duration": duration}
        if auto_close:
            params["auto_close"] = True
        return send_command("create_anim_sequence_from_pose", params)

    @mcp.tool()
    def set_animation_frame_rate(ctx: Context, frame_rate: float, asset_path: str = "",
                                 auto_close: bool = False) -> Dict[str, Any]:
        """
        Set the sampling frame rate of a sequence.

        The frame count is kept, so the length in seconds changes with the rate; the response
        reports both the new rate and the new length.

        Args:
            frame_rate: New sampling frame rate (> 0)
            asset_path: Sequence asset path

        Returns:
            Dict with frame_rate and length read back from the asset.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "frame_rate": frame_rate}
        if auto_close:
            params["auto_close"] = True
        return send_command("set_animation_frame_rate", params)

    @mcp.tool()
    def set_rate_scale(ctx: Context, rate_scale: float, asset_path: str = "",
                       auto_close: bool = False) -> Dict[str, Any]:
        """
        Set the playback rate scale of a sequence.

        Returns: Dict with rate_scale read back from the asset.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "rate_scale": rate_scale}
        if auto_close:
            params["auto_close"] = True
        return send_command("set_rate_scale", params)

    # ------------------------------------------------------------------
    # Writes: bone tracks
    # ------------------------------------------------------------------

    @mcp.tool()
    def add_bone_track(ctx: Context, bone_name: str, asset_path: str = "",
                       auto_close: bool = False) -> Dict[str, Any]:
        """
        Add an empty bone track to a sequence.

        Args:
            bone_name: Bone name, must exist on the sequence's skeleton
            asset_path: Sequence asset path

        Returns:
            Dict with bone_name and the sequence summary. Errors: bone_not_found (candidates
            list the skeleton bones) / bone_track_exists / write_failed.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "bone_name": bone_name}
        if auto_close:
            params["auto_close"] = True
        return send_command("add_bone_track", params)

    @mcp.tool()
    def remove_bone_track(ctx: Context, bone_name: str, asset_path: str = "",
                          auto_close: bool = False) -> Dict[str, Any]:
        """
        Remove a bone track from a sequence.

        Returns:
            Dict with bone_name and the sequence summary. Errors: bone_track_not_found.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "bone_name": bone_name}
        if auto_close:
            params["auto_close"] = True
        return send_command("remove_bone_track", params)

    @mcp.tool()
    def set_bone_track_keys(ctx: Context, bone_name: str, keys: List[Dict[str, Any]],
                            asset_path: str = "", bake_every_frame: bool = True,
                            auto_close: bool = False) -> Dict[str, Any]:
        """
        Write the keyframes of one bone (creates the track when it is missing).

        Keys are sorted by time. `bake_every_frame` (the default) resamples them onto every
        frame of the sequence, which is how the editor stores authored tracks; pass False for
        sparse keys interpolated by the curve.

        Args:
            bone_name: Bone name
            keys: [{time, position: [x,y,z], rotation: [x,y,z,w] | [roll,pitch,yaw],
                scale: [x,y,z]}] - position / rotation / scale are optional
            asset_path: Sequence asset path
            bake_every_frame: Resample onto every frame (default True)

        Returns:
            Dict with keys_written, bake_every_frame, the sequence summary and
            sampled_at_first_key (the transform read back through the sampling command).
            Errors: bone_not_found / invalid_value / write_failed.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "bone_name": bone_name, "keys": keys,
                                  "bake_every_frame": bake_every_frame}
        if auto_close:
            params["auto_close"] = True
        return send_command("set_bone_track_keys", params)

    # ------------------------------------------------------------------
    # Writes: curves
    # ------------------------------------------------------------------

    @mcp.tool()
    def add_curve(ctx: Context, curve_name: str, asset_path: str = "",
                  auto_close: bool = False) -> Dict[str, Any]:
        """
        Add an empty float curve to a sequence.

        Returns:
            Dict with curve_name and the sequence summary. Errors: curve_exists (candidates
            list the existing curves) / write_failed.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "curve_name": curve_name}
        if auto_close:
            params["auto_close"] = True
        return send_command("add_curve", params)

    @mcp.tool()
    def remove_curve(ctx: Context, curve_name: str, asset_path: str = "",
                     auto_close: bool = False) -> Dict[str, Any]:
        """
        Remove a float curve from a sequence.

        Returns:
            Dict with curve_name and the sequence summary. Errors: curve_not_found.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "curve_name": curve_name}
        if auto_close:
            params["auto_close"] = True
        return send_command("remove_curve", params)

    @mcp.tool()
    def set_curve_keys(ctx: Context, curve_name: str, keys: List[Dict[str, Any]],
                       asset_path: str = "", replace_all: bool = True,
                       auto_close: bool = False) -> Dict[str, Any]:
        """
        Write a curve's keys.

        The curve must exist (add_curve first). With replace_all=False the given keys are
        merged into the existing ones and the result is re-sorted.

        Args:
            curve_name: Curve name
            keys: [{time, value, arrive_tangent, leave_tangent, interp, tangent}] - interp is
                constant / linear / cubic (default cubic), tangent auto / user / break
            asset_path: Sequence asset path
            replace_all: Replace every key (default True)

        Returns:
            Dict with key_count and keys read back from the asset. Errors: curve_not_found /
            unknown_value (candidates list the accepted strings) / invalid_value.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "curve_name": curve_name,
                                  "keys": keys, "replace_all": replace_all}
        if auto_close:
            params["auto_close"] = True
        return send_command("set_curve_keys", params)

    @mcp.tool()
    def add_curve_key(ctx: Context, curve_name: str, time: float, value: float,
                      asset_path: str = "", interp: str = "cubic", tangent: str = "auto",
                      auto_close: bool = False) -> Dict[str, Any]:
        """
        Append one key to an existing curve.

        Args:
            curve_name: Curve name
            time: Key time in seconds
            value: Key value
            asset_path: Sequence asset path
            interp: constant / linear / cubic (default cubic)
            tangent: auto / user / break (default auto)

        Returns:
            Dict with key_count and value_at_key (the curve evaluated at the key time).
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "curve_name": curve_name, "time": time,
                                  "value": value, "interp": interp, "tangent": tangent}
        if auto_close:
            params["auto_close"] = True
        return send_command("add_curve_key", params)

    # ------------------------------------------------------------------
    # Writes: notifies
    # ------------------------------------------------------------------

    @mcp.tool()
    def add_notify(ctx: Context, time: float, asset_path: str = "", notify_name: str = "",
                   notify_class: str = "", track_index: int = 0,
                   auto_close: bool = False) -> Dict[str, Any]:
        """
        Add an instant notify at a time.

        With no notify_class the notify is a "skeleton notify" (name only, no object), which is
        what a designer-placed placeholder is. UAnimNotify itself is abstract in 5.5, so an
        abstract class is refused instead of leaving a null notify behind.

        Args:
            time: Trigger time in seconds
            asset_path: Sequence asset path
            notify_name: Notify name (defaults to the class name)
            notify_class: Concrete UAnimNotify subclass name or path
            track_index: Notify track index (default 0)

        Returns:
            Dict with notify_index (after the notify array was re-sorted) and notify readback.
            Errors: invalid_params (abstract or unknown class) / notify_track_not_found.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "notify_name": notify_name,
                                  "notify_class": notify_class, "time": time,
                                  "track_index": track_index}
        if auto_close:
            params["auto_close"] = True
        return send_command("add_notify", params)

    @mcp.tool()
    def add_notify_state(ctx: Context, time: float, asset_path: str = "", notify_name: str = "",
                         notify_class: str = "", duration: float = 0.0, track_index: int = 0,
                         auto_close: bool = False) -> Dict[str, Any]:
        """
        Add a notify state (a notify with a duration) at a time.

        Args:
            time: Start time in seconds
            asset_path: Sequence asset path
            notify_name: Notify name (defaults to the class name)
            notify_class: Concrete UAnimNotifyState subclass name or path
            duration: State duration in seconds
            track_index: Notify track index (default 0)

        Returns:
            Dict with notify_index and notify readback (entry_type is "notify_state").
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "notify_name": notify_name,
                                  "notify_class": notify_class, "time": time,
                                  "duration": duration, "track_index": track_index}
        if auto_close:
            params["auto_close"] = True
        return send_command("add_notify_state", params)

    @mcp.tool()
    def remove_notify(ctx: Context, asset_path: str = "", notify_index: int = -1,
                      notify_name: str = "", guid: str = "", time: float = None,
                      auto_close: bool = False) -> Dict[str, Any]:
        """
        Remove one notify, located by index, name or guid.

        Args:
            asset_path: Sequence asset path
            notify_index: Index as returned by list_notifies
            notify_name: Notify name
            guid: Editor guid of the notify
            time: Trigger time, used to disambiguate a name

        Returns:
            Dict with removed_notify_index and notify_count. Errors: notify_not_found
            (candidates list every notify) / ambiguous_notify.
        """
        params: Dict[str, Any] = {"asset_path": asset_path,
                                  **_notify_locator(notify_index, notify_name, guid, time)}
        if auto_close:
            params["auto_close"] = True
        return send_command("remove_notify", params)

    @mcp.tool()
    def set_notify_trigger_time(ctx: Context, value: float, asset_path: str = "",
                                notify_index: int = -1, notify_name: str = "", guid: str = "",
                                time: float = None, auto_close: bool = False) -> Dict[str, Any]:
        """
        Move a notify to a new trigger time.

        Args:
            value: New trigger time in seconds
            asset_path: Sequence asset path
            notify_index / notify_name / guid / time: notify locator (see get_notify_info)

        Returns:
            Dict with property_name, notify_count and notify readback.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "value": value,
                                  **_notify_locator(notify_index, notify_name, guid, time)}
        if auto_close:
            params["auto_close"] = True
        return send_command("set_notify_trigger_time", params)

    @mcp.tool()
    def set_notify_duration(ctx: Context, value: float, asset_path: str = "",
                            notify_index: int = -1, notify_name: str = "", guid: str = "",
                            time: float = None, auto_close: bool = False) -> Dict[str, Any]:
        """
        Set the duration of a notify state.

        Args:
            value: Duration in seconds
            asset_path: Sequence asset path
            notify_index / notify_name / guid / time: notify locator

        Returns:
            Dict with notify readback. Errors: not_a_notify_state when the target is an
            instant notify.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "value": value,
                                  **_notify_locator(notify_index, notify_name, guid, time)}
        if auto_close:
            params["auto_close"] = True
        return send_command("set_notify_duration", params)

    @mcp.tool()
    def set_notify_track(ctx: Context, value: int, asset_path: str = "", notify_index: int = -1,
                         notify_name: str = "", guid: str = "", time: float = None,
                         auto_close: bool = False) -> Dict[str, Any]:
        """
        Move a notify to another notify track.

        Args:
            value: Target track index (see list_notify_tracks)
            asset_path: Sequence asset path
            notify_index / notify_name / guid / time: notify locator

        Returns:
            Dict with notify readback (track_index / track_name). Errors:
            notify_track_not_found (candidates list the tracks).
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "value": value,
                                  **_notify_locator(notify_index, notify_name, guid, time)}
        if auto_close:
            params["auto_close"] = True
        return send_command("set_notify_track", params)

    @mcp.tool()
    def set_notify_name(ctx: Context, value: str, asset_path: str = "", notify_index: int = -1,
                        notify_name: str = "", guid: str = "", time: float = None,
                        auto_close: bool = False) -> Dict[str, Any]:
        """
        Rename a notify.

        Args:
            value: New notify name
            asset_path: Sequence asset path
            notify_index / notify_name / guid / time: notify locator (notify_name is the
                current name here)

        Returns:
            Dict with notify readback.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "value": value,
                                  **_notify_locator(notify_index, notify_name, guid, time)}
        if auto_close:
            params["auto_close"] = True
        return send_command("set_notify_name", params)

    @mcp.tool()
    def set_notify_color(ctx: Context, value: Any, asset_path: str = "", notify_index: int = -1,
                         notify_name: str = "", guid: str = "", time: float = None,
                         auto_close: bool = False) -> Dict[str, Any]:
        """
        Set a notify's editor colour.

        Args:
            value: "#RRGGBB"/"#RRGGBBAA", or [r,g,b,a] (0-1, or 0-255 when any value exceeds 1)
            asset_path: Sequence asset path
            notify_index / notify_name / guid / time: notify locator

        Returns:
            Dict with notify readback (color).
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "value": value,
                                  **_notify_locator(notify_index, notify_name, guid, time)}
        if auto_close:
            params["auto_close"] = True
        return send_command("set_notify_color", params)

    @mcp.tool()
    def set_notify_trigger_chance(ctx: Context, value: float, asset_path: str = "",
                                  notify_index: int = -1, notify_name: str = "", guid: str = "",
                                  time: float = None, auto_close: bool = False) -> Dict[str, Any]:
        """
        Set the chance a notify triggers (0 = never, 1 = always).

        Args:
            value: Chance in [0,1] (clamped)
            asset_path: Sequence asset path
            notify_index / notify_name / guid / time: notify locator

        Returns:
            Dict with notify readback (trigger_chance).
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "value": value,
                                  **_notify_locator(notify_index, notify_name, guid, time)}
        if auto_close:
            params["auto_close"] = True
        return send_command("set_notify_trigger_chance", params)

    @mcp.tool()
    def set_notify_trigger_on_server(ctx: Context, value: bool, asset_path: str = "",
                                     notify_index: int = -1, notify_name: str = "", guid: str = "",
                                     time: float = None, auto_close: bool = False) -> Dict[str, Any]:
        """
        Set whether a notify triggers on dedicated servers.

        Args:
            value: True to trigger on dedicated servers
            asset_path: Sequence asset path
            notify_index / notify_name / guid / time: notify locator

        Returns:
            Dict with notify readback (trigger_on_server).
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "value": value,
                                  **_notify_locator(notify_index, notify_name, guid, time)}
        if auto_close:
            params["auto_close"] = True
        return send_command("set_notify_trigger_on_server", params)

    @mcp.tool()
    def set_notify_trigger_on_follower(ctx: Context, value: bool, asset_path: str = "",
                                       notify_index: int = -1, notify_name: str = "",
                                       guid: str = "", time: float = None,
                                       auto_close: bool = False) -> Dict[str, Any]:
        """
        Set whether a notify triggers when the animation is a sync group follower.

        Args:
            value: True to also trigger for followers
            asset_path: Sequence asset path
            notify_index / notify_name / guid / time: notify locator

        Returns:
            Dict with notify readback (trigger_on_follower).
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "value": value,
                                  **_notify_locator(notify_index, notify_name, guid, time)}
        if auto_close:
            params["auto_close"] = True
        return send_command("set_notify_trigger_on_follower", params)

    @mcp.tool()
    def set_notify_trigger_weight_threshold(ctx: Context, value: float, asset_path: str = "",
                                            notify_index: int = -1, notify_name: str = "",
                                            guid: str = "", time: float = None,
                                            auto_close: bool = False) -> Dict[str, Any]:
        """
        Set the blend weight below which a notify does not trigger.

        Args:
            value: Threshold in [0,1] (clamped)
            asset_path: Sequence asset path
            notify_index / notify_name / guid / time: notify locator

        Returns:
            Dict with notify readback (weight_threshold).
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "value": value,
                                  **_notify_locator(notify_index, notify_name, guid, time)}
        if auto_close:
            params["auto_close"] = True
        return send_command("set_notify_trigger_weight_threshold", params)

    @mcp.tool()
    def set_notify_lod_filter(ctx: Context, value: Any, asset_path: str = "",
                              notify_index: int = -1, notify_name: str = "", guid: str = "",
                              time: float = None, auto_close: bool = False) -> Dict[str, Any]:
        """
        Set the LOD filter of a notify.

        Args:
            value: "none" / "lod", or {"type": "lod", "lod": 2} to set the level as well
            asset_path: Sequence asset path
            notify_index / notify_name / guid / time: notify locator

        Returns:
            Dict with notify readback (filter_type, filter_lod). Errors: unknown_value
            (candidates list the accepted filter types).
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "value": value,
                                  **_notify_locator(notify_index, notify_name, guid, time)}
        if auto_close:
            params["auto_close"] = True
        return send_command("set_notify_lod_filter", params)

    # ------------------------------------------------------------------
    # Writes: notify tracks and sync markers
    # ------------------------------------------------------------------

    @mcp.tool()
    def add_notify_track(ctx: Context, asset_path: str = "", name: str = "", color: Any = None,
                         auto_close: bool = False) -> Dict[str, Any]:
        """
        Add a notify track to a sequence.

        Args:
            asset_path: Sequence asset path
            name: Track name (defaults to the next number, which is how the engine names them)
            color: Track colour ("#RRGGBB" or [r,g,b,a])

        Returns:
            Dict with track_index, track_count and the new track. Errors:
            notify_track_name_taken / invalid_value (bad colour).
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "name": name}
        if color is not None:
            params["color"] = color
        if auto_close:
            params["auto_close"] = True
        return send_command("add_notify_track", params)

    @mcp.tool()
    def rename_notify_track(ctx: Context, name: str, asset_path: str = "", track_index: int = -1,
                            track_name: str = "", auto_close: bool = False) -> Dict[str, Any]:
        """
        Rename a notify track.

        Args:
            name: New track name
            asset_path: Sequence asset path
            track_index: Track index
            track_name: Current track name (alternative to the index)

        Returns:
            Dict with track_index and the track read back. Errors: notify_track_not_found /
            notify_track_name_taken.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "track_index": track_index,
                                  "track_name": track_name, "name": name}
        if auto_close:
            params["auto_close"] = True
        return send_command("rename_notify_track", params)

    @mcp.tool()
    def remove_notify_track(ctx: Context, asset_path: str = "", track_index: int = -1,
                            track_name: str = "", auto_close: bool = False) -> Dict[str, Any]:
        """
        Remove a notify track; notifies that were on it move to track 0 and higher tracks shift down.

        Args:
            asset_path: Sequence asset path
            track_index: Track index
            track_name: Track name (alternative to the index)

        Returns:
            Dict with removed_track_index, track_count and notify_count. Errors:
            last_notify_track (the only track still holds notifies) / notify_track_not_found.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "track_index": track_index,
                                  "track_name": track_name}
        if auto_close:
            params["auto_close"] = True
        return send_command("remove_notify_track", params)

    @mcp.tool()
    def add_sync_marker(ctx: Context, marker_name: str, time: float, asset_path: str = "",
                        auto_close: bool = False) -> Dict[str, Any]:
        """
        Add a sync marker at a time.

        Markers are kept sorted by time, so the returned marker_index is the marker's place
        after the sort, not an insertion position.

        Args:
            marker_name: Marker name
            time: Marker time in seconds
            asset_path: Sequence asset path

        Returns:
            Dict with marker_index, marker_count and the marker. Errors: invalid_value
            (empty name) / write_failed.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "marker_name": marker_name, "time": time}
        if auto_close:
            params["auto_close"] = True
        return send_command("add_sync_marker", params)

    @mcp.tool()
    def remove_sync_marker(ctx: Context, asset_path: str = "", marker_name: str = "",
                           marker_index: int = -1, time: float = None,
                           auto_close: bool = False) -> Dict[str, Any]:
        """
        Remove a sync marker, located by index or by name (plus optional time).

        Args:
            asset_path: Sequence asset path
            marker_name: Marker name
            marker_index: Marker index
            time: Marker time, used to disambiguate a name

        Returns:
            Dict with removed_marker_name and marker_count. Errors: sync_marker_not_found
            (candidates list every marker) / ambiguous_notify.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "marker_name": marker_name,
                                  "marker_index": marker_index}
        if time is not None:
            params["time"] = time
        if auto_close:
            params["auto_close"] = True
        return send_command("remove_sync_marker", params)

    @mcp.tool()
    def set_sync_marker_time(ctx: Context, new_time: float, asset_path: str = "",
                             marker_name: str = "", marker_index: int = -1, time: float = None,
                             auto_close: bool = False) -> Dict[str, Any]:
        """
        Move a sync marker to a new time.

        Args:
            new_time: New marker time in seconds
            asset_path: Sequence asset path
            marker_name: Marker name
            marker_index: Marker index
            time: Current marker time, used to disambiguate a name

        Returns:
            Dict with marker_count, marker_index (after the re-sort) and the marker read back.
            Errors: sync_marker_not_found / ambiguous_notify.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "marker_name": marker_name,
                                  "marker_index": marker_index, "new_time": new_time}
        if time is not None:
            params["time"] = time
        if auto_close:
            params["auto_close"] = True
        return send_command("set_sync_marker_time", params)

    @mcp.tool()
    def set_sync_marker_time_by_name(ctx: Context, marker_name: str, new_time: float, asset_path: str = "",
                                     time: float = None, auto_close: bool = False) -> Dict[str, Any]:
        """
        Move a sync marker addressed by its name.

        Args:
            marker_name: Marker name
            new_time: New marker time in seconds
            asset_path: Sequence asset path
            time: Current marker time, used when the name appears more than once

        Returns:
            Dict with marker_count, marker_index (after the re-sort) and the marker read back.
            Errors: sync_marker_not_found (candidates list every marker) / ambiguous_notify.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "marker_name": marker_name,
                                  "new_time": new_time}
        if time is not None:
            params["time"] = time
        if auto_close:
            params["auto_close"] = True
        return send_command("set_sync_marker_time_by_name", params)

    # ------------------------------------------------------------------
    # Writes: additive, root motion, compression
    # ------------------------------------------------------------------

    @mcp.tool()
    def set_additive_anim_type(ctx: Context, type: str, asset_path: str = "",
                               auto_close: bool = False) -> Dict[str, Any]:
        """
        Set the additive animation type.

        Args:
            type: "None" / "LocalSpace" / "MeshSpace"
            asset_path: Sequence asset path

        Returns:
            Dict with additive_anim_type read back. Errors: unknown_value (candidates list
            the accepted types).
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "type": type}
        if auto_close:
            params["auto_close"] = True
        return send_command("set_additive_anim_type", params)

    @mcp.tool()
    def set_additive_base_pose(ctx: Context, asset_path: str = "", base_pose: str = "",
                               auto_close: bool = False) -> Dict[str, Any]:
        """
        Set the additive base pose sequence (empty path switches back to the reference pose).

        Returns:
            Dict with additive_base_pose read back. Errors: asset_not_anim_sequence when the
            path is not a sequence.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "base_pose": base_pose}
        if auto_close:
            params["auto_close"] = True
        return send_command("set_additive_base_pose", params)

    @mcp.tool()
    def set_enable_root_motion(ctx: Context, enable: bool, asset_path: str = "",
                               auto_close: bool = False) -> Dict[str, Any]:
        """
        Enable or disable root motion extraction.

        Returns: Dict with enable_root_motion read back.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "enable": enable}
        if auto_close:
            params["auto_close"] = True
        return send_command("set_enable_root_motion", params)

    @mcp.tool()
    def set_root_motion_root_lock(ctx: Context, lock_type: str, asset_path: str = "",
                                  auto_close: bool = False) -> Dict[str, Any]:
        """
        Set the root motion root lock mode.

        Args:
            lock_type: "RefPose" / "AnimFirstFrame" / "Zero"
            asset_path: Sequence asset path

        Returns:
            Dict with root_motion_root_lock read back. Errors: unknown_value (candidates list
            the accepted modes).
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "lock_type": lock_type}
        if auto_close:
            params["auto_close"] = True
        return send_command("set_root_motion_root_lock", params)

    @mcp.tool()
    def set_force_root_lock(ctx: Context, force: bool, asset_path: str = "",
                            auto_close: bool = False) -> Dict[str, Any]:
        """
        Force the root lock even when root motion is disabled.

        Returns: Dict with force_root_lock read back.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "force": force}
        if auto_close:
            params["auto_close"] = True
        return send_command("set_force_root_lock", params)

    @mcp.tool()
    def set_compression_scheme(ctx: Context, compression_scheme: str, asset_path: str = "",
                               auto_close: bool = False) -> Dict[str, Any]:
        """
        Assign a bone compression settings asset to a sequence.

        Args:
            compression_scheme: UAnimBoneCompressionSettings asset path
            asset_path: Sequence asset path

        Returns:
            Dict with compression_scheme read back. Errors: asset_not_found when the path is
            not a compression settings asset.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "compression_scheme": compression_scheme}
        if auto_close:
            params["auto_close"] = True
        return send_command("set_compression_scheme", params)

    @mcp.tool()
    def compress_animation(ctx: Context, asset_path: str = "",
                           auto_close: bool = False) -> Dict[str, Any]:
        """
        Recompress a sequence synchronously and report what it ended up with.

        The call waits for the derived data instead of returning a "started" signal, so
        compressed_size / compressed_data_valid describe the finished state.

        Returns:
            Dict with compressed_data_valid, compressed_size and raw_size. Errors:
            compression_failed when the compressed data is still stale afterwards.
        """
        params: Dict[str, Any] = {"asset_path": asset_path}
        if auto_close:
            params["auto_close"] = True
        return send_command("compress_animation", params)

    # ------------------------------------------------------------------
    # Montage: discovery and creation
    # ------------------------------------------------------------------

    @mcp.tool()
    def list_montages(ctx: Context, search_path: str = "/Game", skeleton: str = "",
                      max_results: int = 200) -> Dict[str, Any]:
        """
        List montage assets below a folder (read-only, registry metadata only).

        Assets are never loaded, so the result carries only what the asset registry tags hold
        (asset_path, asset_name, skeleton_path, length) plus info_source = "registry".

        Returns:
            Dict with montages, found_count, returned_count and truncated.
        """
        return send_command("list_montages", {"search_path": search_path, "skeleton": skeleton,
                                              "max_results": max_results})

    @mcp.tool()
    def get_montage_info(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Full summary of one montage, plus its sections, slot tracks and blend settings.

        Returns:
            Dict with montage (length, section_count, slot_track_count, segment_count,
            notify_count, branching_point_count, blend_* , enable_root_motion_*),
            sections, slot_tracks and blend.
        """
        return send_command("get_montage_info", {"asset_path": asset_path})

    @mcp.tool()
    def find_montages_for_skeleton(ctx: Context, skeleton: str, search_path: str = "/Game",
                                   max_results: int = 200) -> Dict[str, Any]:
        """
        List the montages that use a skeleton (read-only, registry metadata only).

        Returns: Dict with montages, found_count and returned_count.
        """
        return send_command("find_montages_for_skeleton", {"skeleton": skeleton, "search_path": search_path,
                                                           "max_results": max_results})

    @mcp.tool()
    def find_montages_using_animation(ctx: Context, anim_path: str, max_results: int = 200,
                                      max_scan: int = 100) -> Dict[str, Any]:
        """
        List the montages that play an animation (read-only).

        Uses the asset registry referencers first (nothing is loaded). When the registry has no
        dependency data for the animation yet - normal right after a create - it falls back to a
        bounded scan that loads at most `max_scan` montages; `scan_strategy` says which path ran.

        Returns:
            Dict with montages, found_count, returned_count and scan_strategy
            ("registry_referencers" or "loaded_scan").
        """
        return send_command("find_montages_using_animation", {"anim_path": anim_path,
                                                              "max_results": max_results,
                                                              "max_scan": max_scan})

    @mcp.tool()
    def get_montage_length(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Read the play length of a montage in seconds (read-only).

        Returns: Dict with length.
        """
        return send_command("get_montage_length", {"asset_path": asset_path})

    @mcp.tool()
    def get_montage_skeleton(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Read the skeleton a montage uses (read-only).

        Returns: Dict with skeleton_path and skeleton_name.
        """
        return send_command("get_montage_skeleton", {"asset_path": asset_path})

    @mcp.tool()
    def create_montage_from_animation(ctx: Context, anim_path: str, name: str,
                                      folder: str = "/Game") -> Dict[str, Any]:
        """
        Create a montage that plays one animation.

        The animation becomes the single segment of the DefaultSlot track and the result is
        guaranteed to have a `Default` section (a montage without one cannot take a segment).
        The asset is saved on success (`saved`).

        Returns:
            Dict with asset_path, created, length, sections and `saved`. Errors: asset_not_found,
            asset_exists, create_failed.
        """
        return send_command("create_montage_from_animation", {"anim_path": anim_path, "name": name,
                                                              "folder": folder})

    @mcp.tool()
    def create_empty_montage(ctx: Context, name: str, skeleton: str,
                             folder: str = "/Game") -> Dict[str, Any]:
        """
        Create an empty montage on a skeleton.

        Comes with one `Default` section and one `DefaultSlot` track (the engine's own defaults),
        so add_anim_segment has somewhere to write. Saved on success (`saved`).

        Returns:
            Dict with asset_path, created, sections and `saved`. Errors: asset_not_found (skeleton),
            asset_exists, create_failed.
        """
        return send_command("create_empty_montage", {"name": name, "skeleton": skeleton, "folder": folder})

    @mcp.tool()
    def duplicate_montage(ctx: Context, asset_path: str, name: str, folder: str = "/Game") -> Dict[str, Any]:
        """
        Duplicate a montage asset under a new name.

        Returns:
            Dict with asset_path (the copy), source_path, created and `saved`. Errors:
            asset_not_found, asset_exists, create_failed.
        """
        return send_command("duplicate_montage", {"asset_path": asset_path, "name": name, "folder": folder})

    # ------------------------------------------------------------------
    # Montage: sections
    # ------------------------------------------------------------------

    @mcp.tool()
    def list_sections(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        List the composite sections of a montage in time order (read-only).

        Returns:
            Dict with section_count and sections (section_index, section_name, start_time,
            end_time, length, next_section_name, is_looping, slot_index, segment_index,
            link_method).
        """
        return send_command("list_sections", {"asset_path": asset_path})

    @mcp.tool()
    def get_section_info(ctx: Context, asset_path: str = "", section_name: str = "",
                         section_index: int = -1) -> Dict[str, Any]:
        """
        Read one section, by name (preferred) or by index (read-only).

        Returns:
            Dict with section (see list_sections) and section_count. Errors: section_not_found
            with `candidates`.
        """
        return send_command("get_section_info", {"asset_path": asset_path,
                                                 **_section_locator(section_name, section_index)})

    @mcp.tool()
    def get_section_index_at_time(ctx: Context, time: float, asset_path: str = "") -> Dict[str, Any]:
        """
        Index of the section that contains a montage time (read-only).

        Returns:
            Dict with section_index and section_name. Errors: section_not_found when no section
            covers the time (with `candidates`).
        """
        return send_command("get_section_index_at_time", {"asset_path": asset_path, "time": time})

    @mcp.tool()
    def get_section_name_at_time(ctx: Context, time: float, asset_path: str = "") -> Dict[str, Any]:
        """
        Name of the section that contains a montage time (read-only).

        Returns:
            Dict with matched_section_name, section_index. Errors: section_not_found.
        """
        return send_command("get_section_name_at_time", {"asset_path": asset_path, "time": time})

    @mcp.tool()
    def get_section_length(ctx: Context, asset_path: str = "", section_name: str = "",
                           section_index: int = -1) -> Dict[str, Any]:
        """
        Length of one section in seconds (read-only).

        Returns: Dict with section_index and length.
        """
        return send_command("get_section_length", {"asset_path": asset_path,
                                                    **_section_locator(section_name, section_index)})

    @mcp.tool()
    def get_next_section(ctx: Context, asset_path: str = "", section_name: str = "",
                         section_index: int = -1) -> Dict[str, Any]:
        """
        Read the next section of one section (read-only).

        Returns:
            Dict with section_name, next_section_name and is_looping (true when the section
            points at itself).
        """
        return send_command("get_next_section", {"asset_path": asset_path,
                                                  **_section_locator(section_name, section_index)})

    @mcp.tool()
    def get_all_section_links(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        List every (section -> next section) link the montage has (read-only).

        Returns: Dict with link_count and links ([{section, next_section}]).
        """
        return send_command("get_all_section_links", {"asset_path": asset_path})

    @mcp.tool()
    def add_section(ctx: Context, section_name: str, start_time: float,
                    asset_path: str = "") -> Dict[str, Any]:
        """
        Add a section at a start time.

        The sections are re-sorted by time afterwards (the order every other command reports).
        This is a structural write, so the montage's Persona editor is closed first and the
        response carries `editor_closed`.

        Returns:
            Dict with section_index, section (readback), section_count, `editor_closed` and
            `saved`. Errors: section_name_taken (with `candidates`), invalid_value,
            asset_not_found.
        """
        return send_command("add_section", {"asset_path": asset_path, "section_name": section_name,
                                            "start_time": start_time})

    @mcp.tool()
    def remove_section(ctx: Context, asset_path: str = "", section_name: str = "",
                       section_index: int = -1) -> Dict[str, Any]:
        """
        Remove one section.

        A montage must keep at least one section (`last_section`), and links that pointed at the
        removed section are cleared. Structural write: the editor is closed first (`editor_closed`).

        Returns:
            Dict with removed_section_name, section_count, sections, `editor_closed`, `saved`.
            Errors: section_not_found, last_section.
        """
        return send_command("remove_section", {"asset_path": asset_path,
                                                **_section_locator(section_name, section_index)})

    @mcp.tool()
    def rename_section(ctx: Context, new_name: str, asset_path: str = "", section_name: str = "",
                       section_index: int = -1) -> Dict[str, Any]:
        """
        Rename one section; links pointing at the old name follow the rename.

        Structural write: the editor is closed first (`editor_closed`).

        Returns:
            Dict with old_section_name, section (readback), section_count, `editor_closed`,
            `saved`. Errors: section_name_taken, section_not_found.
        """
        return send_command("rename_section", {"asset_path": asset_path, "new_name": new_name,
                                                **_section_locator(section_name, section_index)})

    @mcp.tool()
    def set_section_start_time(ctx: Context, time: float, asset_path: str = "", section_name: str = "",
                               section_index: int = -1) -> Dict[str, Any]:
        """
        Move one section's start time; the list is re-sorted, so the response repeats the index.

        Returns:
            Dict with section_index (after the re-sort) and section (readback). Errors:
            section_not_found, invalid_value.
        """
        return send_command("set_section_start_time", {"asset_path": asset_path, "time": time,
                                                        **_section_locator(section_name, section_index)})

    @mcp.tool()
    def set_next_section(ctx: Context, next_section: str = "", asset_path: str = "",
                         section_name: str = "", section_index: int = -1) -> Dict[str, Any]:
        """
        Set the section that follows one section; an empty name clears the link.

        Returns:
            Dict with section (readback). Errors: section_not_found (both the section and the
            next section are validated).
        """
        return send_command("set_next_section", {"asset_path": asset_path, "next_section": next_section,
                                                  **_section_locator(section_name, section_index)})

    @mcp.tool()
    def set_section_loop(ctx: Context, loop: bool, asset_path: str = "", section_name: str = "",
                         section_index: int = -1) -> Dict[str, Any]:
        """
        Make a section loop on itself (loop=true) or stop it looping (loop=false).

        A loop is stored as a next-section link onto the section itself.

        Returns:
            Dict with loop and section (readback). Errors: section_not_found.
        """
        return send_command("set_section_loop", {"asset_path": asset_path, "loop": loop,
                                                  **_section_locator(section_name, section_index)})

    @mcp.tool()
    def clear_section_link(ctx: Context, asset_path: str = "", section_name: str = "",
                           section_index: int = -1) -> Dict[str, Any]:
        """
        Clear the next-section link of one section.

        Returns:
            Dict with section (readback, next_section_name empty). Errors: section_not_found.
        """
        return send_command("clear_section_link", {"asset_path": asset_path,
                                                    **_section_locator(section_name, section_index)})

    # ------------------------------------------------------------------
    # Montage: slot tracks and animation segments
    # ------------------------------------------------------------------

    @mcp.tool()
    def list_slot_tracks(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        List the slot tracks of a montage (read-only).

        Returns:
            Dict with slot_track_count and slot_tracks (track_index, slot_name, group_name,
            segment_count, track_length).
        """
        return send_command("list_slot_tracks", {"asset_path": asset_path})

    @mcp.tool()
    def get_slot_track_info(ctx: Context, asset_path: str = "", track_index: int = -1,
                            slot_name: str = "") -> Dict[str, Any]:
        """
        Read one slot track, by name (preferred) or by index (read-only).

        Returns:
            Dict with slot_track (see list_slot_tracks) and slot_track_count. Errors:
            slot_track_not_found with `candidates`.
        """
        return send_command("get_slot_track_info", {"asset_path": asset_path,
                                                     **_slot_locator(track_index, slot_name)})

    @mcp.tool()
    def add_slot_track(ctx: Context, slot_name: str, asset_path: str = "") -> Dict[str, Any]:
        """
        Add a slot track to a montage.

        Structural write: the montage editor is closed first (`editor_closed`).

        Returns:
            Dict with track_index, slot_track (readback), slot_track_count, `editor_closed` and
            `saved`. Errors: slot_name_taken, asset_not_found.
        """
        return send_command("add_slot_track", {"asset_path": asset_path, "slot_name": slot_name})

    @mcp.tool()
    def remove_slot_track(ctx: Context, asset_path: str = "", track_index: int = -1,
                          slot_name: str = "") -> Dict[str, Any]:
        """
        Remove one slot track; a montage keeps at least one (`last_slot_track`).

        Structural write: the montage editor is closed first (`editor_closed`).

        Returns:
            Dict with removed_slot_name, slot_track_count, length, `editor_closed`, `saved`.
            Errors: slot_track_not_found, last_slot_track.
        """
        return send_command("remove_slot_track", {"asset_path": asset_path,
                                                   **_slot_locator(track_index, slot_name)})

    @mcp.tool()
    def set_slot_name(ctx: Context, new_name: str, asset_path: str = "", track_index: int = -1,
                      slot_name: str = "") -> Dict[str, Any]:
        """
        Rename one slot track.

        Returns:
            Dict with old_slot_name, slot_track (readback), `saved`. Errors: slot_name_taken,
            slot_track_not_found.
        """
        return send_command("set_slot_name", {"asset_path": asset_path, "new_name": new_name,
                                               **_slot_locator(track_index, slot_name)})

    @mcp.tool()
    def get_all_used_slot_names(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Slot names the montage uses, plus the slot names its skeleton defines (read-only).

        Returns: Dict with used_slot_names and skeleton_slot_names.
        """
        return send_command("get_all_used_slot_names", {"asset_path": asset_path})

    @mcp.tool()
    def list_anim_segments(ctx: Context, asset_path: str = "", track_index: int = -1,
                           slot_name: str = "") -> Dict[str, Any]:
        """
        List the animation segments of one slot track (read-only).

        Returns:
            Dict with track_index, slot_name, segment_count and segments (segment_index,
            anim_path, anim_name, start_time, end_time, length, start_position, end_position,
            play_rate, loop_count, valid).
        """
        return send_command("list_anim_segments", {"asset_path": asset_path,
                                                    **_slot_locator(track_index, slot_name)})

    @mcp.tool()
    def get_anim_segment_info(ctx: Context, asset_path: str = "", track_index: int = -1,
                              slot_name: str = "", segment_index: int = -1) -> Dict[str, Any]:
        """
        Read one animation segment (read-only).

        Returns:
            Dict with segment (see list_anim_segments), segment_count and length. Errors:
            segment_not_found, slot_track_not_found.
        """
        return send_command("get_anim_segment_info", {"asset_path": asset_path,
                                                       **_segment_locator(track_index, segment_index, slot_name)})

    @mcp.tool()
    def add_anim_segment(ctx: Context, anim_path: str, asset_path: str = "", track_index: int = -1,
                         slot_name: str = "", start_time: float = None, play_rate: float = None,
                         loop_count: int = None) -> Dict[str, Any]:
        """
        Add an animation segment to a slot track.

        The segment plays the whole animation from `start_time` in the montage at `play_rate`,
        repeated `loop_count` times; the omitted ones keep the engine defaults (0, 1, 1).
        Structural write: the montage editor is closed first (`editor_closed`).

        Args:
            anim_path: Animation sequence asset to play
            track_index / slot_name: Which slot track to write into
            start_time: Position in the montage; default 0
            play_rate: Segment play rate; default 1
            loop_count: How often the animation repeats; default 1

        Returns:
            Dict with track_index, segment_index, segment (readback), segment_count, length,
            `editor_closed`, `saved`. Errors: asset_not_found (animation), invalid_value
            (0 play rate, negative start time, loop_count < 1, animation that cannot live in the
            track), slot_track_not_found.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "anim_path": anim_path,
                                  **_slot_locator(track_index, slot_name)}
        if start_time is not None:
            params["start_time"] = start_time
        if play_rate is not None:
            params["play_rate"] = play_rate
        if loop_count is not None:
            params["loop_count"] = loop_count
        return send_command("add_anim_segment", params)

    @mcp.tool()
    def remove_anim_segment(ctx: Context, asset_path: str = "", track_index: int = -1,
                            slot_name: str = "", segment_index: int = -1) -> Dict[str, Any]:
        """
        Remove one animation segment from a slot track.

        Structural write: the montage editor is closed first (`editor_closed`).

        Returns:
            Dict with removed_anim_path, removed_segment_index, segment_count, length,
            `editor_closed`, `saved`. Errors: segment_not_found, slot_track_not_found.
        """
        return send_command("remove_anim_segment", {"asset_path": asset_path,
                                                     **_segment_locator(track_index, segment_index, slot_name)})

    @mcp.tool()
    def set_segment_start_time(ctx: Context, value: float, asset_path: str = "", track_index: int = -1,
                               slot_name: str = "", segment_index: int = -1) -> Dict[str, Any]:
        """
        Move one segment's position in the montage (the segment's `start_time`).

        Returns: Dict with property_name, segment (readback), length. Errors: invalid_value,
        segment_not_found.
        """
        return send_command("set_segment_start_time", {"asset_path": asset_path, "value": value,
                                                        **_segment_locator(track_index, segment_index, slot_name)})

    @mcp.tool()
    def set_segment_play_rate(ctx: Context, value: float, asset_path: str = "", track_index: int = -1,
                              slot_name: str = "", segment_index: int = -1) -> Dict[str, Any]:
        """
        Set one segment's play rate (negative plays the animation backwards).

        Returns: Dict with property_name, segment (readback), length. Errors: invalid_value
        (0 is not a play rate), segment_not_found.
        """
        return send_command("set_segment_play_rate", {"asset_path": asset_path, "value": value,
                                                       **_segment_locator(track_index, segment_index, slot_name)})

    @mcp.tool()
    def set_segment_start_position(ctx: Context, value: float, asset_path: str = "",
                                   track_index: int = -1, slot_name: str = "",
                                   segment_index: int = -1) -> Dict[str, Any]:
        """
        Set the time inside the animation one segment starts at.

        Returns: Dict with property_name, segment (readback), length. Errors: invalid_value
        (must stay before the end position), segment_not_found.
        """
        return send_command("set_segment_start_position", {"asset_path": asset_path, "value": value,
                                                            **_segment_locator(track_index, segment_index, slot_name)})

    @mcp.tool()
    def set_segment_end_position(ctx: Context, value: float, asset_path: str = "",
                                 track_index: int = -1, slot_name: str = "",
                                 segment_index: int = -1) -> Dict[str, Any]:
        """
        Set the time inside the animation one segment ends at.

        Returns: Dict with property_name, segment (readback), length. Errors: invalid_value
        (must be after the start position and inside the animation), segment_not_found.
        """
        return send_command("set_segment_end_position", {"asset_path": asset_path, "value": value,
                                                          **_segment_locator(track_index, segment_index, slot_name)})

    @mcp.tool()
    def set_segment_loop_count(ctx: Context, value: int, asset_path: str = "", track_index: int = -1,
                               slot_name: str = "", segment_index: int = -1) -> Dict[str, Any]:
        """
        Set how often one segment repeats its animation.

        Returns: Dict with property_name, segment (readback), length. Errors: invalid_value
        (at least 1), segment_not_found.
        """
        return send_command("set_segment_loop_count", {"asset_path": asset_path, "value": value,
                                                        **_segment_locator(track_index, segment_index, slot_name)})

    # ------------------------------------------------------------------
    # Montage: notifies, branching points, blend settings
    # ------------------------------------------------------------------

    @mcp.tool()
    def set_notify_link_to_section(ctx: Context, section: str, asset_path: str = "",
                                   notify_index: int = -1, notify_name: str = "", guid: str = "",
                                   time: float = None) -> Dict[str, Any]:
        """
        Move a montage notify to a section start ("link a notify to a section").

        UE stores no section link on a notify: the link is the notify's position, so this moves
        its trigger time onto the section's start time. `list_notifies` reports the section a
        notify currently sits in as linked_section_name.

        Returns:
            Dict with section, notify (readback with linked_section_name), notify_count.
            Errors: section_not_found, notify_not_found, ambiguous_notify.
        """
        return send_command("set_notify_link_to_section", {"asset_path": asset_path, "section": section,
                                                            **_notify_locator(notify_index, notify_name, guid, time)})

    @mcp.tool()
    def list_branching_points(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        List the branching points of a montage (read-only).

        A branching point is a notify whose montage tick type is BranchingPoint; the montage
        stops at those while ticking. `branching_point_index` is the index among the branching
        points (what remove_branching_point takes), `notify_index` is the index in the notify
        array (what remove_notify takes).

        Returns:
            Dict with branching_point_count, notify_count and branching_points
            (branching_point_index, notify_index, notify_name, notify_class, trigger_time,
            track_index, section_name).
        """
        return send_command("list_branching_points", {"asset_path": asset_path})

    @mcp.tool()
    def add_branching_point(ctx: Context, name: str, trigger_time: float, asset_path: str = "",
                            track_index: int = 0) -> Dict[str, Any]:
        """
        Add a branching point to a montage.

        Returns:
            Dict with notify_index, notify (readback with is_branching_point true),
            branching_point_count, `editor_closed`, `saved`. Errors: invalid_value
            (empty name, time outside the montage).
        """
        return send_command("add_branching_point", {"asset_path": asset_path, "name": name,
                                                     "trigger_time": trigger_time, "track_index": track_index})

    @mcp.tool()
    def remove_branching_point(ctx: Context, branching_point_index: int,
                               asset_path: str = "") -> Dict[str, Any]:
        """
        Remove one branching point.

        Only a branching point is removed: an ordinary notify is never taken along, which is what
        makes this different from remove_notify.

        Returns:
            Dict with removed_branching_point_index, branching_point_count, notify_count,
            `editor_closed`, `saved`. Errors: branching_point_not_found with `candidates`.
        """
        return send_command("remove_branching_point", {"asset_path": asset_path,
                                                        "branching_point_index": branching_point_index})

    @mcp.tool()
    def is_branching_point_at_time(ctx: Context, time: float, asset_path: str = "") -> Dict[str, Any]:
        """
        Whether a branching point sits at a montage time (read-only).

        Returns: Dict with is_branching_point and notify_name (empty when nothing matched).
        """
        return send_command("is_branching_point_at_time", {"asset_path": asset_path, "time": time})

    @mcp.tool()
    def set_blend_in(ctx: Context, asset_path: str = "", blend_time: float = None,
                     blend_option: str = "") -> Dict[str, Any]:
        """
        Set the blend-in time and/or curve of a montage.

        Args:
            blend_time: Blend time in seconds
            blend_option: Alpha blend option, e.g. Linear / Cubic / CubicInOut / Sinusoidal

        Returns:
            Dict with blend_time, blend_option (readback), blend (both in and out settings).
            Errors: invalid_blend_option with `candidates` (the whole option list),
            invalid_value, unsupported_property when neither field is passed.
        """
        params: Dict[str, Any] = {"asset_path": asset_path}
        if blend_time is not None:
            params["blend_time"] = blend_time
        if blend_option:
            params["blend_option"] = blend_option
        return send_command("set_blend_in", params)

    @mcp.tool()
    def set_blend_out(ctx: Context, asset_path: str = "", blend_time: float = None,
                      blend_option: str = "") -> Dict[str, Any]:
        """
        Set the blend-out time and/or curve of a montage (used when the montage blends out itself).

        Returns:
            Dict with blend_time, blend_option (readback), blend. Errors: invalid_blend_option
            with `candidates`, invalid_value.
        """
        params: Dict[str, Any] = {"asset_path": asset_path}
        if blend_time is not None:
            params["blend_time"] = blend_time
        if blend_option:
            params["blend_option"] = blend_option
        return send_command("set_blend_out", params)

    @mcp.tool()
    def get_blend_settings(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Read the blend in/out settings of a montage (read-only).

        Returns:
            Dict with blend_in_time, blend_in_option, blend_in_mode, blend_out_time,
            blend_out_option, blend_out_mode, blend_out_trigger_time, enable_auto_blend_out
            and blend (the same values as one object).
        """
        return send_command("get_blend_settings", {"asset_path": asset_path})

    @mcp.tool()
    def set_blend_out_trigger_time(ctx: Context, trigger_time: float, asset_path: str = "") -> Dict[str, Any]:
        """
        Set the time from the end at which the montage starts to blend out.

        A negative value means "use the blend-out time" (the engine default is -1), so negative
        values are accepted here.

        Returns: Dict with blend_out_trigger_time (readback) and blend.
        """
        return send_command("set_blend_out_trigger_time", {"asset_path": asset_path, "trigger_time": trigger_time})

    @mcp.tool()
    def get_enable_root_motion_translation(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Read whether the montage allows root motion translation (read-only).

        Returns: Dict with enable_root_motion_translation, enable_root_motion_rotation and enable.
        """
        return send_command("get_enable_root_motion_translation", {"asset_path": asset_path})

    @mcp.tool()
    def set_enable_root_motion_translation(ctx: Context, enable: bool, asset_path: str = "") -> Dict[str, Any]:
        """
        Allow or forbid root motion translation on the montage.

        Returns:
            Dict with enable (readback) and both root motion flags. `saved` on success.
        """
        return send_command("set_enable_root_motion_translation", {"asset_path": asset_path, "enable": enable})

    @mcp.tool()
    def get_enable_root_motion_rotation(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Read whether the montage allows root motion rotation (read-only).

        Returns: Dict with enable_root_motion_rotation, enable_root_motion_translation and enable.
        """
        return send_command("get_enable_root_motion_rotation", {"asset_path": asset_path})

    @mcp.tool()
    def set_enable_root_motion_rotation(ctx: Context, enable: bool, asset_path: str = "") -> Dict[str, Any]:
        """
        Allow or forbid root motion rotation on the montage.

        Returns:
            Dict with enable (readback) and both root motion flags. `saved` on success.
        """
        return send_command("set_enable_root_motion_rotation", {"asset_path": asset_path, "enable": enable})

    # ------------------------------------------------------------------
    # Editor session and preview
    #
    # These commands drive the Persona tab, not the asset: they never save, and when nothing holds a
    # preview of the asset they answer `editor_not_open` instead of pretending to have worked.
    # ------------------------------------------------------------------

    @mcp.tool()
    def open_animation_editor(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Open the asset editor (Persona) of a sequence or a montage.

        Opening the tab also binds the asset to a preview component, so the preview commands work
        right after this returns. `preview_available` says whether that binding succeeded.

        Returns:
            Dict with opened, editor_open, preview_available, editor_name, preview_time, length,
            playing, looping, play_rate, current_section_name, current_section_index. Errors:
            asset_not_found, asset_not_anim_sequence.
        """
        return send_command("open_animation_editor", {"asset_path": asset_path})

    @mcp.tool()
    def open_montage_editor(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Open the montage editor (Persona) of a montage.

        Returns:
            Dict with opened and the preview state (see open_animation_editor). Errors:
            asset_not_found, asset_not_anim_montage.
        """
        return send_command("open_montage_editor", {"asset_path": asset_path})

    @mcp.tool()
    def refresh_montage_editor(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Close and reopen the montage editor of a montage.

        UE has no "refresh this tab" call, so this is what refresh means: the tab is reopened and
        therefore re-reads the asset from disk. Use it after a structural change made outside the
        editor, since structural writes close the editor anyway.

        Returns:
            Dict with refreshed and the preview state. Errors: asset_not_found.
        """
        return send_command("refresh_montage_editor", {"asset_path": asset_path})

    @mcp.tool()
    def set_preview_time(ctx: Context, time: float, asset_path: str = "") -> Dict[str, Any]:
        """
        Move the preview of a sequence or a montage to a time (no asset write, nothing saved).

        For a montage this also moves the section the preview starts from.

        Returns:
            Dict with preview_time (readback), length, playing, current_section_name. Errors:
            editor_not_open (no preview of this asset is live - call open_animation_editor first),
            invalid_value (time outside the asset).
        """
        return send_command("set_preview_time", {"asset_path": asset_path, "time": time})

    @mcp.tool()
    def play_preview(ctx: Context, asset_path: str = "", loop: bool = None,
                     play_rate: float = None) -> Dict[str, Any]:
        """
        Start (or resume) the preview of a sequence or a montage.

        `loop` and `play_rate` only apply to a sequence: a montage preview is driven by its own
        section loop settings.

        Returns:
            Dict with played, preview_time, playing, looping, play_rate. Errors: editor_not_open,
            invalid_value.
        """
        params: Dict[str, Any] = {"asset_path": asset_path}
        if loop is not None:
            params["loop"] = loop
        if play_rate is not None:
            params["play_rate"] = play_rate
        return send_command("play_preview", params)

    @mcp.tool()
    def stop_preview(ctx: Context, asset_path: str = "") -> Dict[str, Any]:
        """
        Pause the preview of a sequence or a montage where it currently is.

        Returns:
            Dict with stopped and the preview time it stopped at. Errors: editor_not_open.
        """
        return send_command("stop_preview", {"asset_path": asset_path})

    @mcp.tool()
    def jump_to_section(ctx: Context, section: str, asset_path: str = "") -> Dict[str, Any]:
        """
        Jump the montage preview to a section.

        The preview is started if needed (a montage can only be jumped inside an active montage
        instance) and the play state it had before is restored afterwards.

        Returns:
            Dict with section, current_section_name (readback), current_section_index, preview_time.
            Errors: section_not_found with `candidates`, editor_not_open.
        """
        return send_command("jump_to_section", {"asset_path": asset_path, "section": section})

    # ------------------------------------------------------------------
    # BlendSpace samples
    # ------------------------------------------------------------------

    @mcp.tool()
    def set_blend_space_samples(ctx: Context, samples: List[Dict[str, Any]], asset_path: str = "",
                                finalize: bool = True, persist: bool = True) -> Dict[str, Any]:
        """
        Write a BlendSpace's sample list and build its runtime data, in one call.

        The sample array is REPLACED whole, so `samples` is the final list - a sample left out is
        a sample removed. Every entry is {animation: <asset path>|null, sample_value: {x, y, z}}.

        Two failures this closes that the raw path leaves open: writing SampleData from python
        (set_editor_property) silently does not persist even though save_asset returns True, and a
        BlendSpace whose samples were written by script has no runtime segment/triangle table until
        finalize_blend_space runs - the blend output is empty (a T-pose in PIE) until it does.

        Args:
            samples: Whole list, in any order: [{"animation": "/Game/.../Fei_Idle",
                "sample_value": {"x": 0, "y": 0, "z": 0}}, ...]. `animation: null` clears a sample.
            asset_path: BlendSpace asset path
            finalize: Rebuild the runtime data after writing (default True). With False the reply
                carries requires_finalize: true and the caller must call finalize_blend_space.
            persist: Save the asset after a successful write (default True)

        Returns:
            Dict with applied, sample_count, none_count, valid_sample_count, triangle_count,
            resample_source (direct / editor / skipped), requires_finalize, saved, the per-sample
            readback (index / animation / valid / sample_value_*), and `warnings` for a sample count
            that does not match the axis grids or a sample animation from another skeleton.
            Errors: asset_not_blend_space, invalid_params, write_failed.
        """
        return send_command("set_blend_space_samples", {"asset_path": asset_path, "samples": samples,
                                                        "finalize": finalize, "persist": persist})
