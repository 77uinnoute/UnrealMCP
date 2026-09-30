"""
Asset Safety Tools for Unreal MCP.

Provides the MCP tool surface for the bridge's asset-safety commands
(safe_delete_asset / list_asset_blockers / asset_status / list_disk_only_assets /
move_asset / move_directory / clear_blendables / remove_blendable / list_blendables).
These exist because scripted delete+create loops hit modal dialogs and
redirector/reference deadlocks: every command stays modal-free and returns conflicts as
structured JSON.

Every tool here is a thin pass-through: the bridge owns the fields, the reasons and the
existence triage. Python MUST NOT delete or rename files on its own - the mutation paths
belong to the registry-and-disk-checked commands. That includes asset renames: use
move_asset / move_directory, never unreal.EditorAssetLibrary.rename_asset (it leaves the
referencer packages stale on disk and leaves no redirector behind).
"""

import logging
from typing import Dict, Any
from mcp.server.fastmcp import FastMCP, Context

# Get logger
logger = logging.getLogger("UnrealMCP")

def register_asset_safety_tools(mcp: FastMCP):
    """Register asset safety tools with the MCP server."""

    @mcp.tool()
    def safe_delete_asset(ctx: Context, asset_path: str, force: bool = False) -> Dict[str, Any]:
        """
        Delete an asset with zero modal dialogs: object redirectors pointing at
        the asset are deleted first; remaining referencers are reported as
        blockers (asset is NOT deleted in that case); a GC pass runs after a
        successful delete so an immediate same-name recreate is safe.

        Args:
            asset_path: Asset path in any of the accepted forms - object path
                ("/Game/M_Outline.M_Outline"), package path ("/Game/M_Outline") or short name.
            force: Break the blocking references, then delete. Use it to escape a
                reference cycle (e.g. mesh -> skeleton/physics and back), where no
                single delete order works. `detached[]` then lists exactly which
                references were broken (level actor instances destroyed, in-asset
                object properties cleared). Default false keeps the old behaviour
                byte for byte. If a referencer cannot be detached (one that is only
                on disk), the delete is refused with reason=referencers_not_detachable
                and nothing is removed.

        Returns:
            Dict with deleted, was_redirector, blockers, detached ([] unless force) and,
            when deleted is false, a `reason` naming the measured cause
            (blocked_by_referencers / referencers_not_detachable / living_objects_in_package /
            memory_pinned / package_file_locked) plus `referencers`. `deleted` is true only when
            BOTH the package file is gone and the object is gone from memory (and the response
            reports `registry_entry_remaining` so the three-way state is checkable) - a blueprint
            that could not be removed is reported as deleted=false with the reason, never as a
            silent success.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"asset_path": asset_path, "force": force}
            logger.info(f"Safely deleting asset: {asset_path} (force={force})")
            response = unreal.send_command("safe_delete_asset", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error safely deleting asset: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def list_asset_blockers(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        List everything that references an asset: object redirectors pointing at
        it plus referencer packages (levels, volumes, other assets). Use before
        delete/overwrite decisions.

        Args:
            asset_path: Asset path in any accepted form: object path
                ("/Game/M_Outline.M_Outline"), package path ("/Game/M_Outline") or short name.

        Returns:
            Dict with blockers (package names) and redirectors (object paths)
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"asset_path": asset_path}
            logger.info(f"Listing asset blockers: {asset_path}")
            response = unreal.send_command("list_asset_blockers", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error listing asset blockers: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def asset_status(ctx: Context, path: str) -> Dict[str, Any]:
        """
        Existence triage for one asset, as three independent facts instead of one
        boolean: in_memory (object-table lookup), on_disk (the .uasset file),
        registry (asset-registry entry), plus the referencer packages. Read-only:
        it loads nothing, so a missing path cannot leave the loader with a failed
        package.

        Use this (not `load_asset(...) is None`) before delete/rename/cleanup
        decisions: right after PIE ends or a collect_garbage(), load_asset returns
        None for EVERY asset, which is indistinguishable from a disk orphan - that
        conflation once deleted 29 .uasset files in /Game/MCP/Ganyu.

        Args:
            path: "/Game/Dir/Asset" or "/Game/Dir/Asset.Asset".

        Returns:
            Dict with in_memory, on_disk, registry, referencers and the resolved
            package_name / object_path. A true disk orphan reads
            on_disk=true / registry=false / in_memory=false; a merely unloaded asset
            still reads on_disk=true / registry=true.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"path": path}
            logger.info(f"Asset status: {path}")
            response = unreal.send_command("asset_status", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error reading asset status: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def list_disk_only_assets(ctx: Context, dir: str) -> Dict[str, Any]:
        """
        List .uasset files in a content directory that have NO asset-registry entry
        (package path form, e.g. "/Game/MCP/Ganyu"). This is a read-only view of
        "on disk but unknown to the registry" - it lists, it never deletes.

        Removing such files is a filesystem operation: do it with the editor
        closed, then let the editor scan on next start. Do NOT delete files from a
        script based on this list alone; the same list looks identical whether the
        file is a genuine orphan or the editor simply has not scanned yet.

        Args:
            dir: Content directory, e.g. "/Game/MCP/Ganyu" (non-recursive).

        Returns:
            Dict with disk_only (package paths), count and scanned_files.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"dir": dir}
            logger.info(f"Listing disk-only assets under: {dir}")
            response = unreal.send_command("list_disk_only_assets", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error listing disk-only assets: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def move_asset(ctx: Context, asset_path: str, new_path: str, update_referencers: bool = True,
                   refresh_registry: bool = True, search_paths: list = None,
                   dry_run: bool = False) -> Dict[str, Any]:
        """
        Rename or move an asset, and fix the packages that reference it.

        USE THIS INSTEAD OF unreal.EditorAssetLibrary.rename_asset. A hard reference
        is a pointer, so the rename itself is free for whatever is already in memory
        - what goes stale is the path SERIALIZED inside every referencer package. It
        keeps naming the old path until that package is written again, and the plain
        python rename neither writes them nor leaves a redirector, so the references
        break silently at the next editor restart
        (log: `LoadErrors: 依赖包 ... 不可用`). Measured on this project: an asset
        created and saved in the same session is not in the registry dependency graph
        yet, which is why the plain python path misses even loaded referencers.

        The command uses the engine's own rename (the Content Browser path,
        IAssetTools::RenameAssets) and then, on top of it:
          - refreshes `search_paths` before reading referencers, so the dependency
            graph sees assets saved in this session too;
          - adds referencers found by scanning loaded packages for hard references
            (a referencer the graph does not know is still written out);
          - re-saves every referencer package it can load, unconditionally, because a
            fixed-up referencer is NOT marked dirty and a dirty-only save would skip
            exactly the packages that need writing;
          - reports the ones it cannot load (map packages) instead of loading them - for those
            the old path keeps a redirector, so it still resolves. The engine only does that for
            the referencers ITS list knows, so the command creates the redirector itself when its
            (larger) list found an unloadable referencer the engine did not see.

        Args:
            asset_path: Existing asset, e.g. "/Game/MCP/A" or "/Game/MCP/A.A".
            new_path: Destination "/Game/Dir/Name". A different directory means a move,
                a different name means a rename, both is both. The asset never changes
                its object name unless new_path says so.
            update_referencers: Re-save the referencer packages (default true). false
                does the move only and still reports the referencer list.
            refresh_registry: Re-scan search_paths before reading referencers
                (default true). false reads the dependency graph as it stands.
            search_paths: Paths to refresh, default ["/Game"]. Costs < 1s for all of
                /Game on this project; narrow it on a huge project.
            dry_run: Report only - no move, no save, no directory creation.

        Returns:
            Dict with renamed, source, destination, new_path_loaded, in_memory_after,
            on_disk_after, referencers_found (each with package / source=registry|loaded /
            map_package / loadable), referencers_saved, referencers_failed, unstorable,
            redirector_left, redirector_created_by_command, old_path_resolves (measured by
            resolving the old path, not inferred), old_path_target, registry_refreshed,
            loaded_scan_packages. Refusals
            carry an error code: destination_exists / source_missing / invalid_destination /
            source_load_failed / in_pie / rename_failed.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {
                "asset_path": asset_path,
                "new_path": new_path,
                "update_referencers": update_referencers,
                "refresh_registry": refresh_registry,
                "dry_run": dry_run,
            }
            if search_paths:
                params["search_paths"] = search_paths
            logger.info(f"Moving asset: {asset_path} -> {new_path} (dry_run={dry_run})")
            response = unreal.send_command("move_asset", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error moving asset: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def move_directory(ctx: Context, dir: str, new_dir: str, update_referencers: bool = True,
                       refresh_registry: bool = True, search_paths: list = None,
                       dry_run: bool = False) -> Dict[str, Any]:
        """
        Move a whole content directory (subdirectories included) to a new directory,
        running every contained asset through the same referencer fixup as
        `move_asset`. Refuses to merge: the destination directory must not exist.

        The subdirectory layout is preserved: `/D/Sub/B` lands on `/D2/Sub/B`. When
        nothing is left under the source directory it is removed from disk and the
        registry (`source_dir_removed=true`) - the engine's own directory rename takes
        the folder with it, and an empty leftover folder would otherwise block a later
        move back into that path.

        Referencers OUTSIDE the moved directory are handled the same way as for a
        single asset: loadable ones are re-saved, map packages are reported in
        `unstorable[]` (the engine leaves a redirector for those). Referencers INSIDE
        the moved directory need no special handling - moving their own package
        rewrites their data anyway.

        Args:
            dir: Existing content directory, e.g. "/Game/MCP/Ganyu" (a trailing "/"
                is accepted).
            new_dir: Destination directory, e.g. "/Game/MCP/Ganyu2".
            update_referencers: Re-save the referencer packages (default true).
            refresh_registry: Re-scan search_paths first (default true).
            search_paths: Paths to refresh, default ["/Game"].
            dry_run: Report only - nothing is moved, saved or created.

        Returns:
            Dict with source, destination, asset_count, moved_count, moved (new object
            paths), failed (asset + reason), the aggregated referencers_found /
            referencers_saved / referencers_failed / unstorable, redirector_left,
            source_dir_exists / source_dir_removed / destination_dir_exists (all measured
            after the whole move), and assets[] with the full per-asset result. Refusals:
            destination_exists / source_missing / invalid_destination / in_pie.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {
                "dir": dir,
                "new_dir": new_dir,
                "update_referencers": update_referencers,
                "refresh_registry": refresh_registry,
                "dry_run": dry_run,
            }
            if search_paths:
                params["search_paths"] = search_paths
            logger.info(f"Moving directory: {dir} -> {new_dir} (dry_run={dry_run})")
            response = unreal.send_command("move_directory", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error moving directory: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def clear_blendables(ctx: Context, volume_name: str) -> Dict[str, Any]:
        """
        Remove every blendable entry from a PostProcessVolume (python cannot
        touch the blendables array directly). Render state is refreshed after
        the change.

        Args:
            volume_name: PostProcessVolume actor name

        Returns:
            Dict with cleared (number of removed entries)
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"volume_name": volume_name}
            logger.info(f"Clearing blendables on volume: {volume_name}")
            response = unreal.send_command("clear_blendables", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error clearing blendables: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def remove_blendable(ctx: Context, volume_name: str, material_path: str) -> Dict[str, Any]:
        """
        Remove all blendable entries referencing a material from a
        PostProcessVolume. Render state is refreshed after the change.

        Args:
            volume_name: PostProcessVolume actor name
            material_path: Full material object path (e.g. "/Game/M_PP.M_PP")

        Returns:
            Dict with removed (number of removed entries)
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"volume_name": volume_name, "material_path": material_path}
            logger.info(f"Removing blendable {material_path} from volume: {volume_name}")
            response = unreal.send_command("remove_blendable", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error removing blendable: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def list_blendables(ctx: Context, volume_name: str) -> Dict[str, Any]:
        """
        List a PostProcessVolume's blendable entries with weights (python
        cannot read these directly).

        Args:
            volume_name: PostProcessVolume actor name

        Returns:
            Dict with blendables (list of {object, weight})
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"volume_name": volume_name}
            logger.info(f"Listing blendables on volume: {volume_name}")
            response = unreal.send_command("list_blendables", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error listing blendables: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
