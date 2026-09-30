"""
Particle system tools for Unreal MCP.

Reads and edits Cascade (`UParticleSystem`) assets: emitters, LOD levels, modules and
module distributions. Every tool forwards to the bridge command of the same name with the
parameters unchanged, so the C++ kernel is the single source of behaviour and error codes.

Every write tool is refused with `particle_editor_open` while the asset is open in the
particle system editor: call `close_asset_editors` first, otherwise the open editor keeps
a stale view of the arrays these tools change. Structural writes also keep the engine's
`SoloTracking` invariant in sync (`UParticleSystem::SetupSoloing`), which is what makes
closing that editor safe for assets this module created or edited.

Response size: write tools default to `detail="summary"`, which returns the location
fields, `property_value_before` / `property_value_after` and `emitter_count` but not the
whole emitter tree (a 7 emitter system is tens of kilobytes per call). Pass
`detail="full"` (or ask for `fields=["emitters"]`) when the readback is needed, and
`detail="summary"` on a read tool to trim it as well. `include_modules=False` drops the
per-LOD module arrays while keeping `module_count`, and `fields=[...]` projects the
response to the named top level fields.

Addressing: every emitter and module item carries `object_path`
(`<package.asset>:<subobject name>`). A struct or array property the writer cannot express
can still be set from python with `unreal.load_object(None, object_path)` and
`set_editor_property`, using the C++ property names.

Scope note: this module covers Cascade only (no Niagara).
"""

import logging
from typing import Any, Dict, List, Optional
from mcp.server.fastmcp import FastMCP, Context

# Get logger
logger = logging.getLogger("UnrealMCP")

# Distribution kinds accepted by set_particle_distribution (mirrors the C++ kernel).
DISTRIBUTION_KINDS = ["constant", "uniform", "constant_curve", "uniform_curve"]

# Module slots accepted by add_particle_module (mirrors the C++ kernel).
MODULE_SLOTS = ["modules", "required", "type_data", "spawn", "event_generator"]

# Check ids accepted by validate_particle_system (mirrors the C++ kernel).
PARTICLE_CHECKS = ["sprite_material", "mesh_material", "light_sprite", "emitter_space",
                   "lod_structure", "inherited_modules", "material_usage", "additive_opacity"]


def register_particle_tools(mcp: FastMCP):
    """Register particle system tools with the MCP server."""

    def _send(command: str, params: Dict[str, Any]) -> Optional[Dict[str, Any]]:
        """Forward one command to the bridge; returns None when there is no connection."""
        from unreal_mcp_server import get_unreal_connection

        unreal = get_unreal_connection()
        if not unreal:
            return None

        response = unreal.send_command(command, params)
        return response

    @mcp.tool()
    def list_particle_emitters(ctx: Context, asset_path: str,
                               include_modules: bool = True,
                               fields: List[str] = None) -> Dict[str, Any]:
        """
        List the emitters of a Cascade particle system (read-only).

        Args:
            asset_path: Particle system asset path (e.g. /Game/Particles/PS_Foo)
            include_modules: False keeps module_count but drops the per-LOD modules arrays
            fields: Optional projection: only these top level response fields are returned
                (e.g. ["emitter_count", "emitters"])

        Returns:
            Dict with asset_path, emitter_count and emitters. Each emitter has
            emitter_index, emitter_class, emitter_name, object_path, lod_count and lods. Each
            LOD has lod_index, level, module_count, the slot class names (required_module,
            type_data_module, spawn_module, event_generator), the derived cache arrays
            (spawning_modules, spawn_modules, update_modules) and modules. Each module has
            module_index, module_class, module_name, object_path, slot, lod_validity,
            active_lods, editable, typed properties and distributions.

        Reads never create or modify an asset and never enter the undo stack.
        """
        try:
            params = {"asset_path": asset_path, "include_modules": include_modules}
            if fields:
                params["fields"] = fields
            response = _send("list_particle_emitters", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error listing particle emitters: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def list_particle_modules(ctx: Context, asset_path: str, emitter_index: int,
                              lod_index: int = 0, fields: List[str] = None) -> Dict[str, Any]:
        """
        List the modules of one LOD level of one emitter (read-only).

        Args:
            asset_path: Particle system asset path
            emitter_index: Emitter index (0 based), as returned by list_particle_emitters
            lod_index: LOD level index, 0 based (default 0)
            fields: Optional projection to the named top level response fields

        Returns:
            Same shape as list_particle_emitters but with a single emitter and a single LOD.
        """
        try:
            params = {"asset_path": asset_path, "emitter_index": emitter_index, "lod_index": lod_index}
            if fields:
                params["fields"] = fields
            response = _send("list_particle_modules", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error listing particle modules: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def get_particle_module(ctx: Context, asset_path: str, emitter_index: int,
                            module_class: str = None, module_index: int = -1,
                            lod_index: int = 0, slot: str = None) -> Dict[str, Any]:
        """
        Read one module: its typed properties and its distributions (read-only).

        Args:
            asset_path: Particle system asset path
            emitter_index: Emitter index (0 based)
            module_class: Module class short name (ParticleModuleVelocity or Velocity) or full path
            module_index: Index inside the emitter's module list; required when several
                modules of the same class exist (otherwise ambiguous_module is returned)
            lod_index: LOD level index (default 0)
            slot: Optional slot to read a slot module directly ("required" / "type_data" /
                "spawn" / "event_generator")

        Returns:
            Dict with asset_path, emitter_index, lod_index and module (module_index,
            module_class, module_name, slot, lod_validity, active_lods, editable, typed
            properties, distributions: kind / distribution_class / constants / min_max /
            keys / sampled_value).
        """
        try:
            params = {
                "asset_path": asset_path,
                "emitter_index": emitter_index,
                "module_index": module_index,
                "lod_index": lod_index,
            }
            if module_class:
                params["module_class"] = module_class
            if slot:
                params["slot"] = slot

            response = _send("get_particle_module", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error getting particle module: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def create_particle_system(ctx: Context, name: str, folder: str = "/Game/Particles",
                               emitter_class: str = "Sprite", lod_count: int = 1,
                            auto_close: bool = False) -> Dict[str, Any]:
        """
        Create a particle system asset with one ready-to-use emitter.

        The new emitter gets the engine's default particle material, so the asset renders
        without further edits.

        Args:
            name: Asset name (e.g. PS_MCPTest)
            folder: Content folder (default /Game/Particles)
            emitter_class: Emitter class short name (default "Sprite")
            lod_count: Number of LOD levels to create (default 1)

        Returns:
            Dict with asset_path, emitter_count, emitters and the new emitter_index.
            Errors: asset_exists (delete it or pick another name) / invalid_params.
        """
        try:
            params = {"name": name, "folder": folder, "emitter_class": emitter_class, "lod_count": lod_count}
            if auto_close:
                params["auto_close"] = True
            response = _send("create_particle_system", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error creating particle system: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def add_particle_emitter(ctx: Context, asset_path: str, emitter_class: str = "Sprite",
                             insert_index: int = -1, lod_count: int = -1,
                            auto_close: bool = False) -> Dict[str, Any]:
        """
        Add an emitter to a particle system and write the asset.

        The new emitter is created the way the Cascade editor does it: LOD levels are
        generated from the neighbouring level when there is one, and every LOD keeps a
        required module plus a spawn module.

        Args:
            asset_path: Particle system asset path
            emitter_class: Emitter class short name (default "Sprite")
            insert_index: Where to insert (default -1 = append)
            lod_count: LOD levels for the new emitter; default -1 (or any value <= 0) adopts the
                system's current LOD count. Every emitter of a system shares one LOD count, so an
                explicit different value brings the other emitters along.

        Returns:
            Dict with emitter_index of the new emitter, emitter_count and emitters, plus
            adjusted_emitters when other emitters had to be resized.
        """
        try:
            params = {"asset_path": asset_path, "emitter_class": emitter_class,
                      "insert_index": insert_index, "lod_count": lod_count}
            if auto_close:
                params["auto_close"] = True
            response = _send("add_particle_emitter", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error adding particle emitter: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def remove_particle_emitter(ctx: Context, asset_path: str, emitter_index: int,
                            auto_close: bool = False) -> Dict[str, Any]:
        """
        Remove one emitter (and its LOD levels and modules) from a particle system.

        Args:
            asset_path: Particle system asset path
            emitter_index: Emitter index (0 based)

        Returns:
            Dict with the removed emitter_index, the remaining emitter_count and emitters.
            Errors: emitter_index_out_of_range (candidates list the valid indices).
        """
        try:
            params = {"asset_path": asset_path, "emitter_index": emitter_index}
            if auto_close:
                params["auto_close"] = True
            response = _send("remove_particle_emitter", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error removing particle emitter: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def add_particle_module(ctx: Context, asset_path: str, emitter_index: int, module_class: str = None,
                            lod_index: int = 0, insert_index: int = -1,
                            slot: str = "modules", hide_sprite: bool = False,
                            module_classes: List[str] = None, copy_modules_from_emitter: int = -1,
                            detail: str = "summary",
                            auto_close: bool = False) -> Dict[str, Any]:
        """
        Add one or several modules to an emitter.

        Pass exactly one of `module_class` (one module), `module_classes` (a list, added in the
        given order) or `copy_modules_from_emitter` (another emitter's module set of the same
        system; its slot modules are not re-added). A batch is all-or-nothing: an unusable class
        fails the whole call instead of leaving half of it behind.

        The module is created with the particle system as outer (the engine's `Within`
        contract), initialised with `SetToSensibleDefaults`, added to every LOD level of the
        emitter, and its LOD validity bitmask is set accordingly — the same sharing model the
        Cascade editor uses. Non-"modules" slots write the corresponding LOD field, and slot
        modules must be of the matching type.

        Args:
            asset_path: Particle system asset path
            emitter_index: Emitter index (0 based)
            module_class: Module class short name (e.g. "Velocity") or full path
            lod_index: LOD level the caller cares about; the module is added to every LOD
            insert_index: Position in the module list (default -1 = append)
            slot: "modules" (default) / "type_data" / "spawn" / "event_generator".
                "required" is rejected: it is created with the emitter.
            hide_sprite: Light emitters only: also set Required.bUseMaxDrawCount=true and
                Required.MaxDrawCount=0 so the emitter draws no sprite of its own (a lit quad
                that reads as a floating square). The response reads both back.
            module_classes: Several classes, added in the given order
            copy_modules_from_emitter: Emitter index whose module set is copied over
            detail: "summary" (default) or "full" (returns the whole emitter tree)

        Returns:
            Dict with module_index (in the requested LOD), module_class, emitter_count and
            emitters (detail="full"), plus sprite_hidden / b_use_max_draw_count /
            max_draw_count when hide_sprite applied. A batch adds `added`
            ([{module_class, module_index}]) and an empty `failed`. Errors:
            module_class_not_found (with class candidates) / unsupported_module_slot /
            lod_index_out_of_range / invalid_params (mixed shapes) / invalid_detail.
        """
        try:
            params = {"asset_path": asset_path, "emitter_index": emitter_index,
                      "lod_index": lod_index, "insert_index": insert_index, "slot": slot,
                      "hide_sprite": hide_sprite, "detail": detail}
            if module_class:
                params["module_class"] = module_class
            if module_classes:
                params["module_classes"] = module_classes
            if copy_modules_from_emitter >= 0:
                params["copy_modules_from_emitter"] = copy_modules_from_emitter
            if auto_close:
                params["auto_close"] = True
            response = _send("add_particle_module", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error adding particle module: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def remove_particle_module(ctx: Context, asset_path: str, emitter_index: int,
                               module_class: str = None, module_index: int = -1,
                               lod_index: int = 0,
                            auto_close: bool = False) -> Dict[str, Any]:
        """
        Remove a module from every LOD level of an emitter.

        Args:
            asset_path: Particle system asset path
            emitter_index: Emitter index (0 based)
            module_class: Module class short name or full path
            module_index: Index inside the module list; needed when the class appears more
                than once (otherwise ambiguous_module is returned)
            lod_index: LOD level used to resolve the module (default 0)

        Returns:
            Dict with cleared_slots (slot fields that referenced the removed module and had
            to be cleared), module_class, module_index, emitter_count and emitters.
            The required and the spawn module cannot be removed (unsupported_module_slot):
            the engine requires both on every enabled LOD level. Removing a TypeData or event
            generator module clears that slot and reports it in cleared_slots. The class
            lookup also finds modules that live in a slot instead of the module list.
        """
        try:
            params = {"asset_path": asset_path, "emitter_index": emitter_index,
                      "module_index": module_index, "lod_index": lod_index}
            if module_class:
                params["module_class"] = module_class
            if auto_close:
                params["auto_close"] = True
            response = _send("remove_particle_module", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error removing particle module: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def set_particle_module_property(ctx: Context, asset_path: str, emitter_index: int,
                                     property_name: str, property_value,
                                     module_class: str = None, module_index: int = -1,
                                     lod_index: int = 0, detail: str = "summary",
                                     return_state: bool = True,
                            auto_close: bool = False) -> Dict[str, Any]:
        """
        Write one ordinary UPROPERTY of a module (not a pin, not a distribution).

        Value kinds are the same as the blueprint tooling: numbers, booleans, strings,
        enum member names, [x,y,z] / [r,g,b,a] arrays for the vector / colour structs, and
        asset paths for object/class properties.

        Any other struct takes an object of its writable fields, matched by name ignoring
        case and underscores: {"bProcessDuringUpdate": true, "bProcessDuringSpawn": false}.
        A struct array (e.g. the spawn module's BurstList, or set_particle_bursts) takes an
        array of such objects. A write is all-or-nothing: if one field is rejected the value
        is left untouched and the response lists `available_fields`.

        Args:
            asset_path: Particle system asset path
            emitter_index: Emitter index (0 based)
            property_name: Property name on the module class (C++ name); the aliases in the
                kernel are accepted too (e.g. RotationRate -> RotationRateAmount)
            property_value: Target value (number / bool / string / array / object)
            module_class: Module class short name or full path
            module_index: Index inside the module list (when the class repeats)
            lod_index: LOD level used to resolve the module (default 0)
            detail: "summary" (default) or "full" (returns the whole emitter tree)
            return_state: False is equivalent to detail="summary"

        Returns:
            Dict with property_name (the real name), resolved_from (the alias passed, when
            one was used), property_type, property_value_before, property_value_after
            (struct values come back as JSON objects), units / hint when the property has a
            known reading. Errors: unknown_property (with candidates) / unknown_field (with
            available_fields) / type_mismatch / unsupported_module_property (distributions
            have their own tool) / module_not_found / ambiguous_module / invalid_detail.
        """
        try:
            params = {"asset_path": asset_path, "emitter_index": emitter_index,
                      "property_name": property_name, "property_value": property_value,
                      "module_index": module_index, "lod_index": lod_index,
                      "detail": detail, "return_state": return_state}
            if module_class:
                params["module_class"] = module_class
            if auto_close:
                params["auto_close"] = True
            response = _send("set_particle_module_property", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error setting particle module property: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def set_particle_distribution(ctx: Context, asset_path: str, emitter_index: int,
                                  property_name: str, kind: str,
                                  values: List[float] = None, keys: List[Dict[str, Any]] = None,
                                  module_class: str = None, module_index: int = -1,
                                  lod_index: int = 0, detail: str = "summary",
                            auto_close: bool = False) -> Dict[str, Any]:
        """
        Write a distribution property (Initial Velocity, Lifetime, Color Over Life, ...).

        `kind` decides the distribution class: constant / uniform / constant_curve /
        uniform_curve. When the property already holds that kind it is written in place,
        otherwise the distribution object is replaced. The baked lookup table is refreshed,
        so the value read back is the value the runtime samples.

        Args:
            asset_path: Particle system asset path
            emitter_index: Emitter index (0 based)
            property_name: Distribution property name (e.g. StartVelocity, Lifetime)
            kind: "constant" / "uniform" / "constant_curve" / "uniform_curve"
            values: constant -> 1 value (3 or 1 for a vector); uniform -> [min, max]
                (or [minX,minY,minZ,maxX,maxY,maxZ]); not used by curve kinds
            keys: curve kinds only: [{"time": 0, "value": 1.0, "interp": "linear"}],
                where value is [min, max] for a scalar uniform curve and
                [minX,minY,minZ,maxX,maxY,maxZ] for a vector uniform curve
            module_class: Module class short name or full path
            module_index: Index inside the module list (when the class repeats)
            lod_index: LOD level used to resolve the module (default 0)

        Returns:
            Dict with property_value_before / property_value_after (kind + values),
            property_type, module_class, emitter_count and emitters. Errors:
            unsupported_distribution_kind (candidates list the kinds) /
            not_a_distribution_property (candidates list the distribution properties) /
            invalid_value (key/value shape).
        """
        try:
            params = {"asset_path": asset_path, "emitter_index": emitter_index,
                      "property_name": property_name, "kind": kind,
                      "module_index": module_index, "lod_index": lod_index, "detail": detail}
            if values is not None:
                params["values"] = values
            if keys is not None:
                params["keys"] = keys
            if module_class:
                params["module_class"] = module_class
            if auto_close:
                params["auto_close"] = True
            response = _send("set_particle_distribution", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error setting particle distribution: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def set_particle_lod_count(ctx: Context, asset_path: str, emitter_index: int,
                               lod_count: int,
                            auto_close: bool = False) -> Dict[str, Any]:
        """
        Set how many LOD levels the system has.

        The requested emitter defines the target and every other emitter is brought to the same
        count: all emitters of a particle system share one LOD count (the engine enforces this when
        it loads the asset). Growing creates the new levels from the existing data (modules are
        inherited and the new levels' module validity bits are set); shrinking drops the trailing
        levels and clears their validity bits on the surviving modules.

        Args:
            asset_path: Particle system asset path
            emitter_index: Emitter index (0 based)
            lod_count: Target number of LOD levels (>= 1)

        Returns:
            Dict with lod_index (the last level), emitter_count and emitters, plus
            adjusted_emitters listing the other emitters that were resized.
        """
        try:
            params = {"asset_path": asset_path, "emitter_index": emitter_index, "lod_count": lod_count}
            if auto_close:
                params["auto_close"] = True
            response = _send("set_particle_lod_count", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error setting particle LOD count: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def copy_particle_lod(ctx: Context, asset_path: str, emitter_index: int,
                          source_lod_index: int, insert_index: int = -1,
                            auto_close: bool = False) -> Dict[str, Any]:
        """
        Copy a LOD level, in every emitter.

        Every emitter of a particle system shares one LOD count, so the same insertion is applied
        to all of them; each new level is generated from that emitter's own source level.

        Args:
            asset_path: Particle system asset path
            emitter_index: Emitter whose source level is reported back (0 based)
            source_lod_index: LOD level to copy
            insert_index: Where the copy goes (default -1 = right after the source)

        Returns:
            Dict with lod_index of the copy, emitter_count and emitters, plus adjusted_emitters
            listing the other emitters that received a level too.
        """
        try:
            params = {"asset_path": asset_path, "emitter_index": emitter_index,
                      "source_lod_index": source_lod_index, "insert_index": insert_index}
            if auto_close:
                params["auto_close"] = True
            response = _send("copy_particle_lod", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error copying particle LOD: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def set_particle_lod_distance(ctx: Context, asset_path: str, emitter_index: int,
                                  lod_index: int, distance: float,
                            auto_close: bool = False) -> Dict[str, Any]:
        """
        Write one entry of the system's LOD distance array.

        Args:
            asset_path: Particle system asset path
            emitter_index: Emitter index (0 based; used to validate the asset)
            lod_index: Entry to write (the array length follows the emitter's LOD count)
            distance: Distance for that level

        Returns:
            Dict with property_value_before / property_value_after, lod_index,
            emitter_count and emitters.
        """
        try:
            params = {"asset_path": asset_path, "emitter_index": emitter_index,
                      "lod_index": lod_index, "distance": distance}
            if auto_close:
                params["auto_close"] = True
            response = _send("set_particle_lod_distance", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error setting particle LOD distance: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def set_particle_bursts(ctx: Context, asset_path: str, emitter_index: int,
                            bursts: List[Dict[str, Any]], lod_index: int = 0,
                            detail: str = "summary",
                            auto_close: bool = False) -> Dict[str, Any]:
        """
        Replace the spawn module's BurstList: pulsed / one-shot emission.

        The whole list is replaced (an empty list clears it), so callers do not have to read
        the current entries first. Use it together with a spawn Rate of 0 when the emitter
        should emit nothing but the bursts.

        Args:
            asset_path: Particle system asset path
            emitter_index: Emitter index (0 based)
            bursts: [{"count": 30, "time": 0.0}, {"count": 30, "time": 0.5}]. `count` and
                `time` are required, `count_low` (the lower bound of the random count) is
                optional
            lod_index: LOD level used to resolve the spawn module (default 0)
            detail: "summary" (default) or "full" (returns the whole emitter tree)

        Returns:
            Dict with burst_count, property_value_before / property_value_after,
            emitter_count and emitters (detail="full"). Errors: invalid_params (unknown burst
            field, missing count/time, spawn module missing on this LOD) / unknown_field (with
            available_fields) / emitter_index_out_of_range. A rejected entry reports which one it
            was: failed_index plus available_fields (the accepted burst fields) and
            unchanged: true.
        """
        try:
            params = {"asset_path": asset_path, "emitter_index": emitter_index,
                      "bursts": bursts, "lod_index": lod_index, "detail": detail}
            if auto_close:
                params["auto_close"] = True
            response = _send("set_particle_bursts", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error setting particle bursts: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def move_particle_module(ctx: Context, asset_path: str, emitter_index: int, to_index: int,
                             module_class: str = None, module_index: int = -1, lod_index: int = 0,
                             detail: str = "summary",
                             auto_close: bool = False) -> Dict[str, Any]:
        """
        Move a module inside its LOD's evaluation order.

        Cascade decides the order in which modules are evaluated, and until now that order could
        only be chosen while adding a module - a wrong order meant removing and re-adding. Moving
        changes the evaluation order and therefore can change the visual result.

        Args:
            asset_path: Particle system asset path
            emitter_index: Emitter index (0 based)
            to_index: Target position, 0 based (0 = evaluated first)
            module_class: Module class short name or full path
            module_index: Index in the module list; needed when the class appears more than once
            lod_index: LOD level whose order is changed (every LOD keeps the same order)
            detail: "summary" (default) or "full"

        Returns:
            Dict with module_class, from_index, to_index, module_order (class names in the new
            order), emitter_count and emitters (detail="full"). Errors: module_not_found /
            ambiguous_module / unsupported_module_slot (the required / spawn / type data / event
            generator modules keep the engine's position) / invalid_params (to_index out of range,
            with candidates).
        """
        try:
            params = {"asset_path": asset_path, "emitter_index": emitter_index,
                      "to_index": to_index, "module_index": module_index,
                      "lod_index": lod_index, "detail": detail}
            if module_class:
                params["module_class"] = module_class
            if auto_close:
                params["auto_close"] = True
            response = _send("move_particle_module", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error moving particle module: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def set_particle_emitter_name(ctx: Context, asset_path: str, emitter_index: int, name: str,
                                  detail: str = "summary",
                                  auto_close: bool = False) -> Dict[str, Any]:
        """
        Rename an emitter.

        Emitters all default to "Particle Emitter", so a multi-emitter system cannot be talked
        about by name - this is what makes list_particle_emitters readable and keeps later
        assertions addressable.

        Args:
            asset_path: Particle system asset path
            emitter_index: Emitter index (0 based)
            name: New emitter name (must not be empty)
            detail: "summary" (default) or "full"

        Returns:
            Dict with emitter_index, name_before, name_after, emitter_count and emitters.
        """
        try:
            params = {"asset_path": asset_path, "emitter_index": emitter_index, "name": name,
                      "detail": detail}
            if auto_close:
                params["auto_close"] = True
            response = _send("set_particle_emitter_name", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error renaming particle emitter: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def set_particle_lod_enabled(ctx: Context, asset_path: str, emitter_index: int, lod_index: int,
                                 enabled: bool, detail: str = "summary",
                                 auto_close: bool = False) -> Dict[str, Any]:
        """
        Enable or disable one LOD level of an emitter.

        Disabling is the engine's switch for "do not evaluate this level"; deleting it would throw
        its modules' settings away. LOD 0 must stay enabled.

        Args:
            asset_path: Particle system asset path
            emitter_index: Emitter index (0 based)
            lod_index: LOD level index (0 based)
            enabled: True to enable, false to disable
            detail: "summary" (default) or "full"

        Returns:
            Dict with emitter_index, lod_index, lod_enabled, emitter_count and emitters.
            Errors: lod_index_out_of_range (with candidates) / invalid_params (LOD 0 disabled).
        """
        try:
            params = {"asset_path": asset_path, "emitter_index": emitter_index,
                      "lod_index": lod_index, "enabled": enabled, "detail": detail}
            if auto_close:
                params["auto_close"] = True
            response = _send("set_particle_lod_enabled", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error setting particle LOD enabled: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def duplicate_particle_emitter(ctx: Context, asset_path: str, emitter_index: int, name: str = None,
                                   detail: str = "summary",
                                   auto_close: bool = False) -> Dict[str, Any]:
        """
        Copy an emitter (its LOD levels and modules, values included) into a new emitter.

        The copy is appended to the system. Modules are duplicated once and shared by the new
        LOD levels, the same sharing model the engine uses, so editing one level still affects
        the others. Without a name the copy is called "<source>_Copy".

        Args:
            asset_path: Particle system asset path
            emitter_index: Emitter index (0 based) to copy
            name: Name for the new emitter (default: source name + "_Copy")
            detail: "summary" (default) or "full"

        Returns:
            Dict with emitter_index (the new one), name_before / name_after, module_counts (per
            LOD), emitter_count and emitters. The whole system keeps one LOD count; emitters that
            had to be adjusted are listed in adjusted_emitters.
        """
        try:
            params = {"asset_path": asset_path, "emitter_index": emitter_index, "detail": detail}
            if name:
                params["name"] = name
            if auto_close:
                params["auto_close"] = True
            response = _send("duplicate_particle_emitter", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error duplicating particle emitter: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def validate_particle_system(ctx: Context, asset_path: str, emitter_index: int = -1,
                                 checks: List[str] = None) -> Dict[str, Any]:
        """
        Structural health pass over a Cascade particle system (read-only).

        One call reports the mistakes that never raise an error in the editor: placeholder
        materials, a mesh emitter whose material was overwritten by assigning TypeDataMesh.Mesh,
        a light emitter that still draws its sprite (a lit quad that reads as a floating
        square), emitters that disagree on local/world space, empty LODs and LOD count
        mismatches, leftover inherited modules, material usage / compile errors and additive
        materials without an Opacity input (which makes AlphaOverLife a no-op).

        Args:
            asset_path: Particle system asset path
            emitter_index: Restrict the per-emitter checks to one emitter (default -1 = all)
            checks: Optional list of check ids to run (default: all). The ids are the ones in
                PARTICLE_CHECKS; an unknown id returns unknown_check with the full list

        Returns:
            Dict with asset_path, check_count, issue_count (findings that are not "info") and
            checks: [{id, severity ("error" / "warning" / "info"), target, message, fix_hint}].
            Nothing is modified and the call is safe to repeat.
        """
        try:
            params = {"asset_path": asset_path}
            if emitter_index >= 0:
                params["emitter_index"] = emitter_index
            if checks:
                params["checks"] = checks
            response = _send("validate_particle_system", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error validating particle system: {e}")
            return {"success": False, "message": str(e)}

    logger.info("Particle tools registered successfully")
