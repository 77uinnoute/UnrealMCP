"""
Asset Pipeline Tools for Unreal MCP.

Provides the MCP tool surface for the bridge's asset pipeline commands:

  - import_assets: source files -> content assets, with the Interchange importer
    for the requested extensions disabled and READ BACK first. Importing through
    the editor python context (AssetImportTask + import_asset_tasks) crashes the
    editor: Interchange drains the GameThread task queue from inside the import
    and trips the TaskGraph RecursionGuard assert while the MCP dispatch task is
    still on the stack. Disabling the flag makes the translator decline the
    extension and the import falls back to the legacy synchronous factory.
  - set_asset_properties: generic asset property writes with friendly names and
    enum member tolerance, because the type-specific setters cannot write a
    texture's srgb / lod_group / compression and the python path fails on
    guessed property and enum names.
  - inspect_skeletal_mesh: read-only SkeletalMesh diagnostics (bounds height, root
    bone, embedded scale via local-vs-world magnitudes, per-slot BaseColor source).
    Read local vs world magnitudes before blaming the retargeter for a collapsed rig.
"""

import logging
from typing import Dict, Any, List, Optional
from mcp.server.fastmcp import FastMCP, Context

from tools._bridge import MCPToolError, _bridge

# Get logger
logger = logging.getLogger("UnrealMCP")


def _forward_import(command: str, paths, params: dict) -> Dict[str, Any]:
    """Thin forwarder for the import commands.

    The response shape belongs to the bridge command, so nothing is rewritten here; the tool layer
    only rejects an empty path list (that check must not cost a bridge round trip).
    """
    if not paths or not isinstance(paths, list):
        raise MCPToolError(command, "missing_paths", "paths must be a non-empty array of absolute file paths")
    if not all(isinstance(path, str) and path for path in paths):
        raise MCPToolError(command, "missing_paths", "paths must contain non-empty strings only")

    logger.info(f"{command}: importing {len(paths)} file(s) into {params.get('destination_path')}")
    # 派发表保持**字面量**命令名：一致性自检（Build/check_command_consistency.py）
    # 是按字面量识别"哪个 python 工具发送哪条命令"的，把命令名变成变量会让它把命令误报成死命令。
    senders = {
        "import_assets": lambda payload: _bridge("import_assets", payload),
        "import_texture": lambda payload: _bridge("import_texture", payload),
        "import_skeletal_mesh": lambda payload: _bridge("import_skeletal_mesh", payload),
        "import_animation": lambda payload: _bridge("import_animation", payload),
    }
    return senders[command](params)


def register_asset_pipeline_tools(mcp: FastMCP):
    """Register asset pipeline tools with the MCP server."""

    @mcp.tool()
    def import_assets(
        ctx: Context,
        paths: List[str],
        destination_path: str,
        force_legacy: bool = True,
        replace_existing: bool = False,
        inspect_materials: bool = False,
    ) -> Dict[str, Any]:
        """
        Import source files into the content browser. Base entry point: use it for asset types that
        have no dedicated tool, and for mixed batches. Type-specific options live in the dedicated
        tools - import_texture (texture properties), import_skeletal_mesh (skeleton / physics /
        morph), import_animation (skeleton is required, optional mesh for morph curves). This tool
        no longer accepts skeleton_path / import_mesh; passing them is a structured
        unsupported_parameter error.
        Import source files into the content browser. THIS is the tool to use for
        imports - do NOT hand-roll AssetImportTask/import_asset_tasks inside
        execute_python_command: the editor crashes (see below).

        With force_legacy (default) the Interchange feature flag for every requested
        extension is disabled and READ BACK before anything is imported; if the
        readback disagrees, nothing is imported and the call fails with
        cvar_override_failed. Without that gate the import re-enters the GameThread
        task queue and trips "Assertion failed: ++Queue(QueueIndex).RecursionGuard
        == 1" (TaskGraph.cpp) - an editor crash, not an error return.

        Side effect to be aware of: with force_legacy the Interchange flags stay off
        for the rest of the editor session, so later imports of those extensions go
        through the legacy importer too. cvar_overrides in the response lists exactly
        which flags were touched.

        Files are independent: a missing source or a name clash fails that item only.

        Importing persists - you no longer need to call save_directory() afterwards. The
        command saves every asset this call produced, not just the main one: a legacy FBX
        import also creates a Skeleton, a PhysicsAsset and auto-created materials by paths
        other than the import task's object list, and those used to stay in memory only
        (gone on the next editor restart, leaving the mesh's skeleton pointer dangling).
        They are now found by diffing the destination folder's asset-registry snapshot
        taken before and after the import, saved one by one, and read back from disk.
        Assets that already existed before the call - e.g. the Skeleton/PhysicsAsset that
        a replace_existing overwrite reuses - are NOT reported and NOT saved again.

        So each result item carries companion_assets[] (one {asset_path, class, saved} per
        extra asset this call produced in the destination folder) and save_failures[] (one
        {asset_path, error} per companion that could not be written; empty when all
        succeeded), and the top level carries companion_count /
        companion_save_failed_count. created_assets is unchanged: it still lists only the
        objects the file's own import task produced, and asset_path is still the first of
        them - companions show up in the new fields only. A companion that fails to save
        does NOT flip imported to false (the mesh really did import); read save_failures /
        companion_save_failed_count for that.

        Each file gets its own unique destination name (a repeated base name becomes
        name_2, name_3 ...), and an existing asset at the target path is reported as a
        per-file failure unless replace_existing is set - so the engine never has to ask
        about overwriting. Results are judged by the objects each file's import task
        actually created; an existing asset is never reported as this import's product
        (see the name_collision note below).

        While the command runs, the engine's modal dialogs are suppressed **for this
        command's window** (bone-merge prompt, overwrite prompt, and every other
        AddModalWindow-based dialog): they are answered with the engine's default
        (bone merge => Cancel, overwrite => no overwrite). A dialog can therefore never
        wedge the whole MCP channel until someone clicks it - but it also means a failed
        skeletal import may be a prompt you never saw, so read skeleton_mismatch instead
        of expecting a dialog.

        Success is judged by the objects each file's import task actually created, not
        by a name guessed from the source file name - the legacy FBX/OBJ importer names
        assets after the internal mesh names (blaster-a.fbx -> blaster-a_blaster-a), so
        read created_assets when you need to know every asset a file produced. An existing
        asset at the target path is never reported as this import's product: if the task
        created nothing and something else already occupies the path, the item fails with
        name_collision (see existing_asset / existing_class) instead of a false
        imported=true. The fallback also requires the occupying object to be the SAME KIND
        as the source (a png can never produce a Material), which is what catches a sibling
        file of the same call having grabbed the name first.

        Args:
            paths: Absolute source file paths, e.g. ["D:/tex/a.png", "D:/tex/b.png"].
                Supported extensions: png bmp exr hdr tga tif tiff jpg jpeg psd dds
                ies fbx obj.
            destination_path: Content path, e.g. "/Game/MCP/Textures"
            force_legacy: Disable the Interchange importer for these extensions first
                (default True; keep it unless you have verified Interchange is safe)
            replace_existing: Overwrite assets that already exist (default False -
                an existing asset is reported as a per-file failure instead)
            inspect_materials: After importing, read back each created skeletal mesh's
                material slots and report the ones whose BaseColor is not backed by a
                texture (the "white mesh" signature). Adds materials_without_texture[]
                to the response; when False the response shape is unchanged.

        Returns:
            Dict with importer ("legacy"/"interchange"), imported_count, failed_count,
            companion_count, companion_save_failed_count,
            results [{source_path, asset_path, imported, error, created_assets,
            companion_assets [{asset_path, class, saved}], save_failures
            [{asset_path, error}], skeleton_mismatch?, skeleton_path?, hint?}],
            cvar_overrides [{cvar, requested, actual, verified}],
            materials_without_texture[] (only when inspect_materials was requested).
            On a refused precheck the response carries error plus collisions
            [{kind, source_path, internal_names{mesh,skeleton}, existing?, hint}].
        """
        return _forward_import("import_assets", paths, {
            "paths": paths,
            "destination_path": destination_path,
            "force_legacy": force_legacy,
            "replace_existing": replace_existing,
            "inspect_materials": inspect_materials,
        })

    @mcp.tool()
    def import_texture(
        ctx: Context,
        paths: List[str],
        destination_path: str,
        force_legacy: bool = True,
        replace_existing: bool = False,
        srgb: Optional[bool] = None,
        compression: Optional[str] = None,
        compression_quality: Optional[int] = None,
        lod_group: Optional[str] = None,
        no_alpha: Optional[bool] = None,
        mip_gen: Optional[str] = None,
        filter: Optional[str] = None,
    ) -> Dict[str, Any]:
        """
        Import textures (png/tga/jpg/jpeg/bmp/exr/hdr/psd/dds) and configure them in the same call.

        Property names and values are the same friendly names set_asset_properties accepts
        (compression: "TC_MASKS"/"TC_NORMALMAP"/..., lod_group: "TEXTUREGROUP_WORLD"/...,
        mip_gen: "TMGS_NO_MIPMAPS"/..., filter: "TF_BILINEAR"/...). Only the properties you pass are
        written - anything omitted keeps the value the importer gave it - and each one is read back
        (value_before / value_after) in results[i].properties.applied[] / failed[].

        Response: the same envelope as import_assets (importer, per-file results with
        created_assets / companion_assets / save_failures, cvar_overrides) plus asset_type
        ("texture") and supported_extensions. A property that fails is reported in
        properties.failed[] without flipping the file's imported flag.
        """
        params = {
            "paths": paths,
            "destination_path": destination_path,
            "force_legacy": force_legacy,
            "replace_existing": replace_existing,
        }
        optional = {
            "srgb": srgb,
            "compression": compression,
            "compression_quality": compression_quality,
            "lod_group": lod_group,
            "no_alpha": no_alpha,
            "mip_gen": mip_gen,
            "filter": filter,
        }
        params.update({key: value for key, value in optional.items() if value is not None})
        return _forward_import("import_texture", paths, params)

    @mcp.tool()
    def import_skeletal_mesh(
        ctx: Context,
        paths: List[str],
        destination_path: str,
        force_legacy: bool = True,
        replace_existing: bool = False,
        skeleton_path: Optional[str] = None,
        create_physics_asset: Optional[bool] = None,
        physics_asset: Optional[str] = None,
        clear_physics_asset: bool = False,
        import_morph_targets: bool = True,
    ) -> Dict[str, Any]:
        """
        Import skeletal meshes (fbx/obj) with mesh-specific import options.

        skeleton_path binds the mesh to an existing Skeleton (omit to create a new one);
        create_physics_asset / physics_asset control the PhysicsAsset (leave both unset to keep the
        importer default); import_morph_targets (default True) brings blend shapes in as morph
        targets - without it the morph curves of any animation have nothing to drive.

        A replace_existing import that replaces a mesh KEEPS its existing physics_asset: the
        importer writes None onto the new mesh, and "do not create one" is not "clear mine", so
        the command restores the previous value and reports physics_asset_restored. Pass
        clear_physics_asset=True to really leave it empty (that combination is reported as
        cleared=["physics_asset"]); combining it with physics_asset is refused as conflicting_params.

        Response: the import_assets envelope plus asset_type ("skeletal_mesh") and
        supported_extensions; sibling assets (Skeleton / PhysicsAsset / auto materials) show up in
        companion_assets and are read back from disk. Each replaced file item also carries
        `equivalence` with slot/material drift: slots_added / slots_removed / slots_renamed /
        materials_lost / materials_changed (a re-import that cannot line the slot names up keeps the
        slot and drops the material instance - that is what materials_lost reports).
        """
        params = {
            "paths": paths,
            "destination_path": destination_path,
            "force_legacy": force_legacy,
            "replace_existing": replace_existing,
            "import_morph_targets": import_morph_targets,
        }
        if skeleton_path:
            params["skeleton_path"] = skeleton_path
        if create_physics_asset is not None:
            params["create_physics_asset"] = create_physics_asset
        if physics_asset:
            params["physics_asset"] = physics_asset
        if clear_physics_asset:
            params["clear_physics_asset"] = clear_physics_asset
        return _forward_import("import_skeletal_mesh", paths, params)

    @mcp.tool()
    def import_animation(
        ctx: Context,
        paths: List[str],
        destination_path: str,
        skeleton_path: str,
        force_legacy: bool = True,
        replace_existing: bool = False,
        import_mesh: bool = False,
        import_morph_targets: bool = True,
        override_animation_name: Optional[str] = None,
        frame_rate: Optional[int] = None,
    ) -> Dict[str, Any]:
        """
        Import animation (fbx) onto an existing Skeleton - skeleton_path is REQUIRED.

        Without a Skeleton the importer creates no objects at all for an animation-only FBX, so a
        missing skeleton_path fails with missing_skeleton before anything is imported (use
        import_skeletal_mesh first if you have no Skeleton yet).

        import_mesh=True also imports the FBX mesh: morph curves belong to the mesh's morph targets,
        so that is what you want when the animation carries facial/expression curves (and it keeps
        import_morph_targets meaningful). override_animation_name renames the created sequence;
        frame_rate sets a custom sample rate (unset = importer default).

        NOTE on import_mesh: it is forwarded to the importer UI (bImportMesh), but UE 5.5's
        FbxFactory never reads that flag on the SkeletalMesh path - the mesh, the auto-created
        materials and the PhysicsAsset are produced unconditionally in FBXIT_SkeletalMesh mode
        (FbxFactory.cpp fills the skel-mesh array at :445-448, imports mesh+materials+PA in the
        :644 branch, and only gates the *animation* part at :759). So the flag currently has no
        observable effect on what gets created; the AnimSequence lands as "<fbx base>_Anim".

        Response: the import_assets envelope plus asset_type ("animation") and
        supported_extensions.
        """
        params = {
            "paths": paths,
            "destination_path": destination_path,
            "skeleton_path": skeleton_path,
            "force_legacy": force_legacy,
            "replace_existing": replace_existing,
            "import_mesh": import_mesh,
            "import_morph_targets": import_morph_targets,
        }
        if override_animation_name:
            params["override_animation_name"] = override_animation_name
        if frame_rate is not None:
            params["frame_rate"] = frame_rate
        return _forward_import("import_animation", paths, params)

    @mcp.tool()
    def set_asset_properties(
        ctx: Context,
        asset_path: str,
        props: Dict[str, Any]
    ) -> Dict[str, Any]:
        """
        Write properties on any asset through one generic entry point.

        Use this instead of guessing reflection names in python: the type-specific
        setters (set_component_property / set_static_mesh_properties /
        set_blueprint_property) cannot write a texture's srgb / lod_group /
        compression, and set_editor_property throws on a misspelled name or enum
        member instead of telling you the legal ones.

        Friendly keys (mapped to the real engine property, echoed back per item in
        `property`):
            srgb, compression / compression_settings, compression_quality,
            lod_group (alias texture_group), no_alpha, mip_gen, filter

        Both spellings resolve: reflection stores the C++ name (LODGroup,
        MipGenSettings, CompressionNoAlpha) while python shows the snake_case form,
        and the lookup ignores case AND underscores. Keys may be given either way;
        `property` in the response reports the reflected name.

        Enum members are matched ignoring case and underscores, so both
        TEXTUREGROUP_WORLD_NORMALMAP and TEXTUREGROUP_WORLD_NORMAL_MAP work, as do
        TMGS_NoMipmaps and TMGS_NO_MIPMAPS.

        Items are written independently: one bad key does not abort the rest and
        nothing is rolled back. Check failed_count.

        Args:
            asset_path: Full asset object path (e.g.
                "/Game/MCP/Textures/T_Rock.T_Rock")
            props: Property name -> value, e.g.
                {"srgb": False, "lod_group": "TEXTUREGROUP_WORLD_NORMAL_MAP",
                 "mip_gen": "TMGS_NO_MIPMAPS"}

        Returns:
            Dict with applied [{key, property, value_before, value_after}] and
            failed [{key, error, message, ...}], plus applied_count / failed_count.
            A failed item carries whatever the write produced: candidates (legal
            enum members), available_fields (real struct fields), supported_shapes
            (shapes the property accepts), failed_index (which container element was
            refused) and unchanged (true - a refused write never leaves a partial
            change). Failed error codes: unknown_property (no such property -
            includes computed attributes like has_alpha_channel),
            unknown_field (no such struct field), unknown_enum_member (candidates
            lists the legal members), invalid_value, type_mismatch,
            load_failed, unsupported_property_type,
            property_not_writable (exists but engine-managed).
        """
        if not props or not isinstance(props, dict):
            raise MCPToolError("set_asset_properties", "invalid_params",
                               "props must be a non-empty object of property name -> value")

        params = {"asset_path": asset_path, "props": props}
        logger.info(f"Setting {len(props)} propert(y/ies) on {asset_path}")
        return _bridge("set_asset_properties", params)

    @mcp.tool()
    def inspect_skeletal_mesh(
        ctx: Context,
        asset_path: str
    ) -> Dict[str, Any]:
        """
        Read-only health check on a SkeletalMesh. Answers "why does this rig behave
        wrong / collapse?" without writing anything.

        The field that matters most is has_embedded_scale: a skeleton exported as
        "metre data + let UE do m->cm" ends up with LOCAL bone translations in metres
        while WORLD (component space) translations are in centimetres. Mesh bounds and
        world-space poses look perfectly normal in that state (bounds 159 cm, world
        90 cm), so nothing on the command surface used to reveal it - while a retargeter
        fed such a skeleton pushes the root bone 100x and collapses the whole rig.
        Criterion: ANY mid-skeleton probe bone with |LOCAL| < 1 and |WORLD| > 10.

        probe_bones lists local and world translations side by side so you can check the
        magnitudes yourself; bone_local_unit is their order-of-magnitude verdict
        ("meters" when local is ~100x smaller than world, otherwise "cm").

        has_embedded_scale reports a FACT, not a verdict: some rigs embed a scale on
        purpose and work fine (Richie). Do not treat true as "broken".

        slots_without_texture lists material slots whose BaseColor input is not a
        TextureSample (typically a VectorParameter left behind when the FBX's texture
        path did not resolve). That is the "white mesh" signature - cheap to check here,
        and worth checking right after importing a character.

        Args:
            asset_path: SkeletalMesh asset path, object or package form
                (e.g. "/Game/MCP/Ganyu/Ganyu_unit.Ganyu_unit" or
                "/Game/MCP/Ganyu/Ganyu_unit")

        Returns:
            Dict with height_cm, bone_count, root_bone{name,scale,translation},
            has_embedded_scale, bone_local_unit ("meters"/"cm"),
            probe_bones[{name,local,world}], skeleton_consistent,
            slots_without_texture[], materials_count.
            Asset missing or not a SkeletalMesh -> structured error, no edits made.
        """
        if not asset_path or not isinstance(asset_path, str):
            raise MCPToolError("inspect_skeletal_mesh", "invalid_params", "asset_path must be a non-empty string")

        logger.info(f"Inspecting skeletal mesh {asset_path}")
        return _bridge("inspect_skeletal_mesh", {"asset_path": asset_path})

    def _physics_send(command: str, params: dict) -> Dict[str, Any]:
        """Forward to one of the physics-asset commands. The dispatch stays literal on purpose:
        Build/check_command_consistency.py identifies "which tool sends which command" from literals,
        so building the name in a variable makes it report the command as dead."""
        if not params.get("asset_path"):
            raise MCPToolError(command, "invalid_params", "Missing 'asset_path' parameter")

        senders = {
            "add_physics_asset_body": lambda payload: _bridge("add_physics_asset_body", payload),
            "add_physics_asset_constraint": lambda payload: _bridge("add_physics_asset_constraint", payload),
            "set_physics_asset_collision": lambda payload: _bridge("set_physics_asset_collision", payload),
            "remove_physics_asset_body": lambda payload: _bridge("remove_physics_asset_body", payload),
            "remove_physics_asset_constraint": lambda payload: _bridge("remove_physics_asset_constraint", payload),
            "list_physics_asset_bodies": lambda payload: _bridge("list_physics_asset_bodies", payload),
        }
        return senders[command](params)

    @mcp.tool()
    def list_physics_asset_bodies(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """
        Read every body (bone, shapes, radii) and constraint (bones, limits) of a PhysicsAsset.

        This is the read side of the physics-asset commands, and the only way to see
        collision_disabled_with for a body: those pairs live in UPhysicsAsset::CollisionDisableTable,
        a map keyed by a struct the property reflector cannot address.

        Args:
            asset_path: PhysicsAsset path, e.g. "/Game/MCP/Ganyu/Ganyu_UE_PhysicsAsset".

        Returns:
            Dict with the asset's bodies[] and constraints[] as the bridge reads them back.
            Errors: load_failed (no asset at the path), not_a_physics_asset.
        """
        return _physics_send("list_physics_asset_bodies", {"asset_path": asset_path})

    @mcp.tool()
    def add_physics_asset_body(ctx: Context, asset_path: str, bone_name: str, radius: float,
                              length: float = 0.0, mesh_path: Optional[str] = None,
                              simulate: bool = False, persist: bool = True) -> Dict[str, Any]:
        """
        Add a collision body (sphere, or a capsule aligned to the bone) for one bone of a PhysicsAsset.

        The only scriptable way to author PhysicsAsset bodies - FPhysicsAssetUtils::CreateNewBody has no
        python binding. Nothing is simulated by this command: a RigidBody node or a physical animation
        component still has to be pointed at the asset.

        Args:
            asset_path: PhysicsAsset path.
            bone_name: Bone to create the body for.
            radius: Sphere/capsule radius in cm.
            length: Capsule segment length in cm; 0 (default) creates a sphere.
            mesh_path: Mesh used to resolve the bone (default: the loaded mesh whose PhysicsAsset is this asset).
            simulate: Mark the body PhysType_Simulated (dynamic in a RigidBody node); false (default) = kinematic.
            persist: Save the asset after the command (default true).

        Returns:
            Dict with the created body read back from the asset; Errors: bone_not_found (candidates list
            the skeleton's bones), body_exists, load_failed, not_a_physics_asset.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "bone_name": bone_name,
                                  "radius": radius, "length": length, "simulate": simulate,
                                  "persist": persist}
        if mesh_path:
            params["mesh_path"] = mesh_path
        return _physics_send("add_physics_asset_body", params)

    @mcp.tool()
    def add_physics_asset_constraint(ctx: Context, asset_path: str, child_bone: str, parent_bone: str,
                                     swing1_deg: float = 30.0, swing2_deg: float = 30.0,
                                     twist_deg: float = 20.0, linear: str = "locked",
                                     disable_collision: bool = True,
                                     persist: bool = True) -> Dict[str, Any]:
        """
        Add a constraint between two already-present bodies of a PhysicsAsset.

        The child bone names the constraint, as in the editor. Linear axes are locked or free; the three
        angular limits are given in degrees. The parent must already have a body.

        Args:
            asset_path: PhysicsAsset path.
            child_bone: Bone whose body is constrained (ConstraintBone1).
            parent_bone: Bone it is constrained to (ConstraintBone2); must already have a body.
            swing1_deg: Swing1 limit in degrees (default 30).
            swing2_deg: Swing2 limit in degrees (default 30).
            twist_deg: Twist limit in degrees (default 20).
            linear: "locked" (default) or "free".
            disable_collision: Disable collision between the two bodies (default true).
            persist: Save the asset after the command (default true).

        Returns:
            Dict with the created constraint read back; Errors: body_not_found (candidates list bodies),
            constraint_exists, invalid_linear, load_failed.
        """
        return _physics_send("add_physics_asset_constraint", {
            "asset_path": asset_path, "child_bone": child_bone, "parent_bone": parent_bone,
            "swing1_deg": swing1_deg, "swing2_deg": swing2_deg, "twist_deg": twist_deg,
            "linear": linear, "disable_collision": disable_collision, "persist": persist})

    @mcp.tool()
    def set_physics_asset_collision(ctx: Context, asset_path: str, bone_name: str,
                                    other_bones: Optional[List[str]] = None, disable: bool = True,
                                    persist: bool = True) -> Dict[str, Any]:
        """
        Disable (or re-enable) collision between one body of a PhysicsAsset and other bodies.

        The only scriptable way to narrow what a body collides with: the pairs live in
        UPhysicsAsset::CollisionDisableTable, which the property reflector cannot address, and the
        solver-side switch only covers the pair a constraint joins.

        Args:
            asset_path: PhysicsAsset path.
            bone_name: Body whose collision set is changed.
            other_bones: Bones to pair with; omitted or empty = every other body of the asset.
            disable: true (default) disables those pairs, false re-enables them.
            persist: Save the asset after the command (default true).

        Returns:
            Dict with the body's collision_disabled_with read back; Errors: body_not_found,
            other_bone_not_found (candidates), load_failed.
        """
        params: Dict[str, Any] = {"asset_path": asset_path, "bone_name": bone_name,
                                  "disable": disable, "persist": persist}
        if other_bones:
            params["other_bones"] = other_bones
        return _physics_send("set_physics_asset_collision", params)

    @mcp.tool()
    def remove_physics_asset_body(ctx: Context, asset_path: str, bone_name: str,
                                  persist: bool = True) -> Dict[str, Any]:
        """
        Remove a body from a PhysicsAsset for good, with every constraint that referenced it and its
        collision-table entries (the remaining table indices are renumbered so each key still addresses
        the same pair). Until this existed a body could only be parked as PhysType_Kinematic.

        Args:
            asset_path: PhysicsAsset path.
            bone_name: Bone whose body is removed.
            persist: Save the asset after the command (default true).

        Returns:
            Dict with the remaining bodies/constraints; Errors: body_not_found, load_failed.
        """
        return _physics_send("remove_physics_asset_body",
                             {"asset_path": asset_path, "bone_name": bone_name, "persist": persist})

    @mcp.tool()
    def remove_physics_asset_constraint(ctx: Context, asset_path: str, child_bone: str, parent_bone: str,
                                        persist: bool = True) -> Dict[str, Any]:
        """
        Remove one constraint of a PhysicsAsset, identified by the same (child, parent) bone pair
        add_physics_asset_constraint uses.

        Args:
            asset_path: PhysicsAsset path.
            child_bone: ConstraintBone1 of the constraint to remove.
            parent_bone: ConstraintBone2 of the constraint to remove.
            persist: Save the asset after the command (default true).

        Returns:
            Dict with the remaining constraints; Errors: constraint_not_found, load_failed.
        """
        return _physics_send("remove_physics_asset_constraint", {
            "asset_path": asset_path, "child_bone": child_bone, "parent_bone": parent_bone,
            "persist": persist})
