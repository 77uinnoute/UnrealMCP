"""
Reflection Tools for Unreal MCP.

Exposes the read-only property probe and the generic property writer. The probe reports what a property
is, which JSON shapes it accepts, and why it cannot be written, straight from the engine's reflection
data - including the types nothing can write, which python's own reflection cannot see.

The writer reaches the targets python cannot address at all: a sub-object of an asset whose class has no
python bindings (a cloth config object is the case that forced it) surfaces in python as its base class,
so `set_editor_property` there fails with "Failed to find property ... on 'ClothConfigBase'".

Both tools also accept a PATH into the target ("LodData[0].PhysicalMeshData.WeightMaps[1].Values"), which
is what reaches baked per-vertex data - the cloth MaxDistance mask, whose all-zero default pins every
cloth particle to its skinned position and makes the cloth look like plain skinning.
"""

import logging
from typing import Any, Dict, Optional

from mcp.server.fastmcp import FastMCP, Context

# Get logger
logger = logging.getLogger("UnrealMCP")


def register_reflection_tools(mcp: FastMCP):
    """Register reflection tools with the MCP server."""

    @mcp.tool()
    def reflect_probe(
        ctx: Context,
        target: str,
        property: str,
        node_id: Optional[str] = None,
        graph_name: Optional[str] = None,
        component_name: Optional[str] = None,
        expression_name: Optional[str] = None,
    ) -> Dict[str, Any]:
        """
        Inspect one property before writing it: type, accepted shapes, and whether it is writable.

        Read-only; it never modifies the asset, object or graph. Use it when a write would otherwise be
        a guess, or when a write failed and the reason is not obvious.

        Sub-object paths are accepted too, spelled "<asset object path>:<sub name>[.<sub name>]" (e.g.
        "/Game/X.X:A.Config"), which is the only way to describe a config object nested in an asset - and
        the shape set_asset_properties / set_object_property expect for the same target.

        The property may be a PATH rather than a plain name, e.g.
        "LodData[0].PhysicalMeshData.WeightMaps[1].Values" - the read side of what set_object_property
        writes, and the only way to inspect baked/large data such as a cloth mask or weight array.
        Path grammar: segments separated by '.', "[i]" for an array index, "[key]" for a map key.

        Args:
            target: Asset path ("/Game/MCP/Blueprints/BP_Foo"), a sub-object path, or a class path / class
                name ("/Script/Engine.Actor", "StaticMeshComponent") to inspect that class's defaults.
            property: Property name or property path on the target (C++ name; snake_case also resolves).
            node_id: Optional graph node guid (from find_blueprint_nodes); target must then be a Blueprint.
            graph_name: Optional graph to look the node up in; defaults to the event graph. Never created.
            component_name: Optional component name; target must then be a Blueprint with that SCS component.
            expression_name: Optional material expression name/desc; target must then be a Material.

        Returns:
            success plus:
            - property_type / property_class: engine type, e.g. "TArray<FName>" / "FArrayProperty"
            - container / element_type / value_type: for Array / Set / Map
            - supported: whether the reflector can write it
            - supported_shapes: value shapes it accepts, e.g. "array", "{FieldName: value}"
            - semantics: "replace" for containers (element count == final element count)
            - editable / edit_const / transient: property flags
            - hint: the alternative tool or spelling to use when it cannot be written
            - current_value: the value as it stands now
        """
        from unreal_mcp_server import get_unreal_connection

        if not property:
            # Reject locally: no point spending a bridge round-trip on a call that cannot succeed.
            return {"success": False, "error_code": "invalid_params", "error": "Missing 'property' parameter"}

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params: Dict[str, Any] = {"target": target, "property": property}
            if node_id:
                params["node_id"] = node_id
            if graph_name:
                params["graph_name"] = graph_name
            if component_name:
                params["component_name"] = component_name
            if expression_name:
                params["expression_name"] = expression_name

            logger.info(f"Probing property '{property}' on target: {target}")
            response = unreal.send_command("reflect_probe", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            logger.error(f"Error probing property: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def set_object_property(
        ctx: Context,
        object_path: str,
        property_name: Optional[str] = None,
        property_value: Optional[Any] = None,
        properties: Optional[Dict[str, Any]] = None,
        persist: bool = True,
    ) -> Dict[str, Any]:
        """
        Write properties on any object path, including sub-objects python cannot reach.

        Use it when the target is a sub-object of an asset whose class has no python bindings (a
        cloth config object inside a SkeletalMesh's clothing asset is the canonical case: python sees
        ClothConfigBase and set_editor_property raises "Failed to find property ..."), when several
        properties must change with ONE save, or when `set_asset_properties` cannot address the object
        because it takes an asset path, not a sub-object path.

        Sub-object paths are spelled "<asset object path>:<sub name>[.<sub name>]" - e.g.
        "/Game/MCP/Ganyu/Ganyu_UE.Ganyu_UE:Ganyu_UE_Clothing_0.ChaosClothConfig". The response echoes
        both the requested path and the resolved object_path, so a wrong guess is visible immediately.

        Two write forms:
          - single: property_name + property_value
          - batch: properties={name: value, ...} (written in one call, saved at most once)

        Either name may be a PATH into the target instead of a plain property, which is how data python
        and the plain object path cannot reach is addressed - a baked cloth mask or weight array being the
        case that forced it: "LodData[0].PhysicalMeshData.WeightMaps[1].Values". Path grammar: segments
        separated by '.', with "[i]" for an array index and "[key]" for a map key (numeric or name);
        intermediate segments may be structs, objects, arrays or maps. A path leaf takes the same value
        shapes reflect_probe reports, and may be written even when the leaf itself carries no editable
        flag (baked data has none) - EditConst leaves are still refused. The response echoes both the
        requested path and the reflected leaf name in `property_path` / `property_name`.

        Args:
            object_path: Asset path, class path/name, or sub-object path to write.
            property_name: Single-property form: property name (snake_case resolves) or a property path.
            property_value: Single-property form: value in the shape reflect_probe reports.
            properties: Batch form: property name or path -> value. Same resolution and shapes as above.
            persist: Save the owning package once after the writes (default true). Pass false to leave
                the change in memory (useful while iterating; nothing reaches disk until a later save).

        Returns:
            success plus:
            - object_path / object_class / object_class_path: what the path resolved to
            - writes [{property_name (reflected leaf name), property_path (only for a path), property_type,
              property_value_before, property_value_after, changed}]: property_value_after is the
              authority, and `changed` is false when a setter clamped the value back
            - written_count / failed [{property_name, property_path, error_code, error, ...}] / failed_count
            - saved (true only when persist was honoured), persist_requested
            Failed error codes: unknown_property / unknown_field (the step that failed; candidates lists
            the legal names there), property_not_writable (exists but not editable), type_mismatch
            (supported_shapes lists the accepted shapes), unknown_enum_member, unsupported_property_type,
            load_failed (a null object or container segment mid-path), write_failed. Items are written
            independently: one bad name does not stop the rest and nothing is rolled back.
        """
        from unreal_mcp_server import get_unreal_connection

        if not property_name and not properties:
            logger.error("set_object_property called without a property to write")
            return {"success": False, "error_code": "invalid_params",
                    "error": "Pass 'property_name' + 'property_value', or a non-empty 'properties' object"}
        if properties is not None and not isinstance(properties, dict):
            logger.error("set_object_property called with a non-object 'properties'")
            return {"success": False, "error_code": "invalid_params",
                    "error": "'properties' must be an object of property name -> value"}

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params: Dict[str, Any] = {"object_path": object_path, "persist": bool(persist)}
            if property_name:
                params["property_name"] = property_name
                if property_value is not None:
                    params["property_value"] = property_value
            if properties:
                params["properties"] = properties

            count = len(properties) if properties else 1
            logger.info(f"Writing {count} propert(y/ies) on: {object_path}")
            response = unreal.send_command("set_object_property", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            logger.error(f"Error writing object property: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def apply_cloth_masks(
        ctx: Context,
        object_path: str,
        persist: bool = True,
    ) -> Dict[str, Any]:
        """
        Rebuild a clothing asset's derived mask data after set_object_property changed a baked mask.

        A cloth mask written through a property path is raw data until the engine consumes it: every enabled
        PointWeightMap has to be pushed into the sim mesh, the tethers regenerated (they are derived from the
        mask - an all-zero MaxDistance mask marks every node kinematic, so nothing gets generated), and the
        per-render-vertex cloth contributions recomputed. Until that happens the renderer keeps treating the
        whole cloth section as plain skinning, i.e. the cloth looks frozen no matter what the mask says.

        THE mask that matters on a freshly created cloth: "LodData[0].PhysicalMeshData.WeightMaps[1].Values"
        (MaxDistance, one value per sim particle) - it is 0 everywhere by default, and 0 pins that particle to
        its skinned position (Chaos sphere radius = Low + mask * (High - Low) = 0). Read it and the particles'
        positions with reflect_probe, write a mask with set_object_property, then call this. It is the
        programmatic equivalent of the Cloth Painter's apply, which is the only other way to trigger it:
        UClothingAssetCommon::ApplyParameterMasks is not a UFunction, so nothing reflection-driven can call it.

        Args:
            object_path: Clothing asset path (e.g.
                "/Game/MCP/Ganyu/Ganyu_UE.Ganyu_UE:Ganyu_UE_Clothing_0"), or a SkeletalMesh path - every
                clothing asset on that mesh is rebuilt.
            persist: Save the owning package after the rebuild (default true).

        Returns:
            success plus assets [{object_path, mesh_path, lods [{lod_index, particle_count,
            triangle_count, max_distance_count, max_distance_pinned_count (particles the constraint still
            holds at their skinned position), max_distance_min / max, euclidean_tether_batches,
            geodesic_tether_batches}], sections [{section_index, material_index, cloth_lod_bias,
            vertex_count, skinned_only_count, cloth_only_count, blended_count}]}], asset_count, saved.
            The section split is the proof the cloth can move now: skinned_only_count is every vertex whose
            cloth contribution is ignored (0xFFFF static alpha).
        """
        from unreal_mcp_server import get_unreal_connection

        if not object_path:
            return {"success": False, "error_code": "invalid_params", "error": "Missing 'object_path' parameter"}

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params: Dict[str, Any] = {"object_path": object_path, "persist": bool(persist)}
            logger.info(f"Applying cloth parameter masks on: {object_path}")
            response = unreal.send_command("apply_cloth_masks", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            logger.error(f"Error applying cloth masks: {e}")
            return {"success": False, "message": str(e)}
