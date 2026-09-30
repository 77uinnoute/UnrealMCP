"""
Blueprint Tools for Unreal MCP.

This module provides tools for creating and manipulating Blueprint assets in Unreal Engine.
"""

import logging
from typing import Dict, List, Any
from mcp.server.fastmcp import FastMCP, Context

# Get logger
logger = logging.getLogger("UnrealMCP")

def register_blueprint_tools(mcp: FastMCP):
    """Register Blueprint tools with the MCP server."""
    
    @mcp.tool()
    def create_blueprint(
        ctx: Context,
        name: str,
        parent_class: str
    ) -> Dict[str, Any]:
        """
        Create a new Blueprint under /Game/Blueprints/.

        Args:
            name: Asset name
            parent_class: Any class reference: a full path ("/Script/Engine.Character",
                "/Game/Dir/BP_X.BP_X_C"), the class name as it really is ("Character",
                "AnimNotify", "AnimNotifyState", "AnimInstance"), or the legacy short
                form ("Pawn" -> APawn).

        Returns:
            Dict with name, path, parent_class / parent_class_path (the RESOLVED class,
            read back rather than echoed) and blueprint_class (the asset class the
            factory chose, e.g. "AnimBlueprint" for an AnimInstance parent).

            An unresolvable parent_class is an error: `parent_class_not_found` with the
            `tried` forms, and NO asset is created. It never falls back to Actor.
        """
        # Import inside function to avoid circular imports
        from unreal_mcp_server import get_unreal_connection
        
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
                
            response = unreal.send_command("create_blueprint", {
                "name": name,
                "parent_class": parent_class
            })
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Blueprint creation response: {response}")
            return response or {}
            
        except Exception as e:
            error_msg = f"Error creating blueprint: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
    
    @mcp.tool()
    def get_asset_properties(
        ctx: Context,
        asset_name: str,
        timeout: float = None
    ) -> Dict[str, Any]:
        """
        Get all properties of an asset by name or path.
        
        Works for any asset type: Blueprint, Material, MaterialInstance, MaterialFunction,
        DataAsset, etc.
        
        Args:
            asset_name: Asset name (e.g. 'MyBlueprint') or full path (e.g. '/Game/MyAsset')
            timeout: Response receive timeout in seconds (default: server recv-timeout)
        
        Returns:
            Dict containing asset name, class, path, and all properties
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
                
            response = unreal.send_command("get_asset_properties", {
                "asset_name": asset_name
            }, recv_timeout=timeout)
            return response or {}
            
        except Exception as e:
            logger.error(f"Error getting asset properties: {e}")
            return {}

    @mcp.tool()
    def add_component_to_blueprint(
        ctx: Context,
        blueprint_name: str,
        component_type: str,
        component_name: str,
        location: List[float] = [],
        rotation: List[float] = [],
        scale: List[float] = [],
        component_properties: Dict[str, Any] = {}
    ) -> Dict[str, Any]:
        """
        Add a component to a Blueprint.

        Args:
            blueprint_name: Name of the target Blueprint
            component_type: Type of component to add (use component class name without U prefix)
            component_name: Name for the new component
            location: [X, Y, Z] coordinates for component's position
            rotation: [Pitch, Yaw, Roll] values for component's rotation
            scale: [X, Y, Z] values for component's scale
            component_properties: Properties applied to the new component template.
                Object properties (SkeletalMesh, OverrideMaterials, ...) take an
                asset path string; enums take a member name; vectors take [x, y, z].

        Returns:
            Dict with component_name, component_class, component_template and the
            per-property applied / failed arrays. Fails with blueprint_not_ready
            instead of crashing when the Blueprint has no usable construction script.
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            # Ensure all parameters are properly formatted
            params = {
                "blueprint_name": blueprint_name,
                "component_type": component_type,
                "component_name": component_name,
                "location": location or [0.0, 0.0, 0.0],
                "rotation": rotation or [0.0, 0.0, 0.0],
                "scale": scale or [1.0, 1.0, 1.0]
            }
            
            # Add component_properties if provided
            if component_properties and len(component_properties) > 0:
                params["component_properties"] = component_properties
            
            # Validate location, rotation, and scale formats
            for param_name in ["location", "rotation", "scale"]:
                param_value = params[param_name]
                if not isinstance(param_value, list) or len(param_value) != 3:
                    logger.error(f"Invalid {param_name} format: {param_value}. Must be a list of 3 float values.")
                    return {"success": False, "message": f"Invalid {param_name} format. Must be a list of 3 float values."}
                # Ensure all values are float
                params[param_name] = [float(val) for val in param_value]
            
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
                
            logger.info(f"Adding component to blueprint with params: {params}")
            response = unreal.send_command("add_component_to_blueprint", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Component addition response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error adding component to blueprint: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
    
    @mcp.tool()
    def set_static_mesh_properties(
        ctx: Context,
        blueprint_name: str,
        component_name: str,
        static_mesh: str = "/Engine/BasicShapes/Cube.Cube"
    ) -> Dict[str, Any]:
        """
        Set static mesh properties on a StaticMeshComponent.
        
        Args:
            blueprint_name: Name of the target Blueprint
            component_name: Name of the StaticMeshComponent
            static_mesh: Path to the static mesh asset (e.g., "/Engine/BasicShapes/Cube.Cube")
            
        Returns:
            Response indicating success or failure
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            params = {
                "blueprint_name": blueprint_name,
                "component_name": component_name,
                "static_mesh": static_mesh
            }
            
            logger.info(f"Setting static mesh properties with params: {params}")
            response = unreal.send_command("set_static_mesh_properties", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Set static mesh properties response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error setting static mesh properties: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
    
    @mcp.tool()
    def set_component_property(
        ctx: Context,
        blueprint_name: str,
        component_name: str,
        property_name: str,
        property_value,
    ) -> Dict[str, Any]:
        """Set a property on a component in a Blueprint's component template.

        The write goes through the shared property reflector, so the accepted value
        shapes are whatever the property type takes, and they are the same shapes the
        read paths emit: compact structs (FVector / FRotator / FLinearColor / FColor /
        FQuat / FIntPoint) are NUMBER ARRAYS, FTransform is an object
        ({Rotation, Translation, Scale3D}, and a flattened 10-number array is also
        accepted on write), other structs are field objects, enums take a member name
        or a number, containers take an array (maps take {Key: value}) and object
        references take an asset path. Use
        reflect_probe(target=<blueprint path>, component_name=..., property=...)
        to see the exact shapes before writing.

        Args:
            blueprint_name: Name of the target Blueprint
            component_name: Component variable name
            property_name: Property name (C++ or snake_case)
            property_value: value in the shape reflect_probe reports for the property

        Returns:
            Dict with property_value_before and property_value_after, or a structured
            error carrying error_code, unchanged, supported_shapes / available_fields
            and a hint (unknown_property keeps its "did you mean" suggestion).
        """

        from unreal_mcp_server import get_unreal_connection
        
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            params = {
                "blueprint_name": blueprint_name,
                "component_name": component_name,
                "property_name": property_name,
                "property_value": property_value
            }
            
            logger.info(f"Setting component property with params: {params}")
            response = unreal.send_command("set_component_property", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Set component property response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error setting component property: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
    
    @mcp.tool()
    def set_physics_properties(
        ctx: Context,
        blueprint_name: str,
        component_name: str,
        simulate_physics: bool = True,
        gravity_enabled: bool = True,
        mass: float = 1.0,
        linear_damping: float = 0.01,
        angular_damping: float = 0.0
    ) -> Dict[str, Any]:
        """Set physics properties on a component."""
        from unreal_mcp_server import get_unreal_connection
        
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            params = {
                "blueprint_name": blueprint_name,
                "component_name": component_name,
                "simulate_physics": simulate_physics,
                "gravity_enabled": gravity_enabled,
                "mass": float(mass),
                "linear_damping": float(linear_damping),
                "angular_damping": float(angular_damping)
            }
            
            logger.info(f"Setting physics properties with params: {params}")
            response = unreal.send_command("set_physics_properties", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Set physics properties response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error setting physics properties: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
    
    @mcp.tool()
    def compile_blueprint(
        ctx: Context,
        blueprint_name: str
    ) -> Dict[str, Any]:
        """Compile a Blueprint and report the real result.

        Returns:
            Dict with name, status (EBlueprintStatus name), compiled (engine
            semantics: true for BS_UpToDate and BS_UpToDateWithWarnings), errors
            and warnings (per-node compiler messages).
        """

        from unreal_mcp_server import get_unreal_connection
        
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            params = {
                "blueprint_name": blueprint_name
            }
            
            logger.info(f"Compiling blueprint: {blueprint_name}")
            response = unreal.send_command("compile_blueprint", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Compile blueprint response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error compiling blueprint: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def set_blueprint_property(
        ctx: Context,
        blueprint_name: str,
        property_name: str,
        property_value
    ) -> Dict[str, Any]:
        """
        Set a property on a Blueprint class default object.
        
        Args:
            blueprint_name: Name of the target Blueprint
            property_name: Name of the property to set
            property_value: Value to set the property to
            
        Returns:
            Response indicating success or failure
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            params = {
                "blueprint_name": blueprint_name,
                "property_name": property_name,
                "property_value": property_value
            }
            
            logger.info(f"Setting blueprint property with params: {params}")
            response = unreal.send_command("set_blueprint_property", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Set blueprint property response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error setting blueprint property: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    # @mcp.tool() commented out, just use set_component_property instead
    def set_pawn_properties(
        ctx: Context,
        blueprint_name: str,
        auto_possess_player: str = "",
        use_controller_rotation_yaw: bool = None,
        use_controller_rotation_pitch: bool = None,
        use_controller_rotation_roll: bool = None,
        can_be_damaged: bool = None
    ) -> Dict[str, Any]:
        """
        Set common Pawn properties on a Blueprint.
        This is a utility function that sets multiple pawn-related properties at once.
        
        Args:
            blueprint_name: Name of the target Blueprint (must be a Pawn or Character)
            auto_possess_player: Auto possess player setting (None, "Disabled", "Player0", "Player1", etc.)
            use_controller_rotation_yaw: Whether the pawn should use the controller's yaw rotation
            use_controller_rotation_pitch: Whether the pawn should use the controller's pitch rotation
            use_controller_rotation_roll: Whether the pawn should use the controller's roll rotation
            can_be_damaged: Whether the pawn can be damaged
            
        Returns:
            Response indicating success or failure with detailed results for each property
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            # Define the properties to set
            properties = {}
            if auto_possess_player and auto_possess_player != "":
                properties["auto_possess_player"] = auto_possess_player
            
            # Only include boolean properties if they were explicitly set
            if use_controller_rotation_yaw is not None:
                properties["bUseControllerRotationYaw"] = use_controller_rotation_yaw
            if use_controller_rotation_pitch is not None:
                properties["bUseControllerRotationPitch"] = use_controller_rotation_pitch
            if use_controller_rotation_roll is not None:
                properties["bUseControllerRotationRoll"] = use_controller_rotation_roll
            if can_be_damaged is not None:
                properties["bCanBeDamaged"] = can_be_damaged
                
            if not properties:
                logger.warning("No properties specified to set")
                return {"success": True, "message": "No properties specified to set", "results": {}}
            
            # Set each property using the generic set_blueprint_property function
            results = {}
            overall_success = True
            
            for prop_name, prop_value in properties.items():
                params = {
                    "blueprint_name": blueprint_name,
                    "property_name": prop_name,
                    "property_value": prop_value
                }
                
                logger.info(f"Setting pawn property {prop_name} to {prop_value}")
                response = unreal.send_command("set_blueprint_property", params)
                
                if not response:
                    logger.error(f"No response from Unreal Engine for property {prop_name}")
                    results[prop_name] = {"success": False, "message": "No response from Unreal Engine"}
                    overall_success = False
                    continue
                
                results[prop_name] = response
                if not response.get("success", False):
                    overall_success = False
            
            return {
                "success": overall_success,
                "message": "Pawn properties set" if overall_success else "Some pawn properties failed to set",
                "results": results
            }
            
        except Exception as e:
            error_msg = f"Error setting pawn properties: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
    
    @mcp.tool()
    def attach_component_to_component(
        ctx: Context,
        blueprint_name: str,
        child_component: str,
        parent_component: str,
        socket_name: str = None
    ) -> Dict[str, Any]:
        """Attach one component of a Blueprint to another (optionally to a socket/bone).

        Components added by add_component_to_blueprint all land on the SCS root; use this
        to build a parent/child hierarchy (a mesh under a spring arm, a weapon on a socket).

        Args:
            blueprint_name: Name of the target Blueprint
            child_component: Component to move (variable name)
            parent_component: Component to attach it under (variable name)
            socket_name: Optional socket/bone name on the parent to attach to

        Returns:
            Dict with component_name, component_class, component_template, attach_parent,
            attach_socket (null when none) and the blueprint's component name list.
            Errors: component_not_found (with the available component names) /
            invalid_attach (self-attach, cycle, or a component that is not a scene component).
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {
                "blueprint_name": blueprint_name,
                "child_component": child_component,
                "parent_component": parent_component
            }
            if socket_name:
                params["socket_name"] = socket_name

            logger.info(f"Attaching component '{child_component}' to '{parent_component}'")
            response = unreal.send_command("attach_component_to_component", params)

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error attaching component: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def list_blueprint_variables(
        ctx: Context,
        blueprint_name: str
    ) -> Dict[str, Any]:
        """
        List a Blueprint's own member variables.

        Args:
            blueprint_name: Name of the target Blueprint

        Returns:
            Dict with variables: [{variable_name, type, container, sub_class, default_value,
            is_exposed (instance editable, same source as flags.instance_editable),
            class_editable (CPF_Edit: visible/editable in the class defaults panel),
            flags{...}}] and variable_count.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            return unreal.send_command("list_blueprint_variables",
                                       {"blueprint_name": blueprint_name}) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error listing blueprint variables: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def get_blueprint_variable_info(
        ctx: Context,
        blueprint_name: str,
        variable_name: str
    ) -> Dict[str, Any]:
        """
        Read one member variable's type, default value and flags.

        Args:
            blueprint_name: Name of the target Blueprint
            variable_name: Member variable to read

        Returns:
            Dict with variable_name, type, container, sub_class, default_value,
            is_exposed (instance editable - the same bit as flags.instance_editable),
            class_editable (CPF_Edit: visible/editable in the class defaults panel) and
            flags (instance_editable / expose_on_spawn / blueprint_read_only / replicated /
            rep_notify_func / category / tooltip / transient / private).
            A missing variable reports variable_not_found with the current names in candidates.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            return unreal.send_command("get_blueprint_variable_info",
                                       {"blueprint_name": blueprint_name,
                                        "variable_name": variable_name}) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error reading blueprint variable info: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def remove_blueprint_variable(
        ctx: Context,
        blueprint_name: str,
        variable_name: str
    ) -> Dict[str, Any]:
        """
        Remove a member variable and the graph nodes that reference it.

        The engine's RemoveMemberVariable drops the description and RemoveVariableNodes drops the
        Get/Set nodes, so the blueprint does not keep referencing a variable that no longer exists.

        Args:
            blueprint_name: Name of the target Blueprint
            variable_name: Member variable to remove

        Returns:
            Dict with removed, the remaining variables and the compile result (compiled / errors).
            A missing variable reports variable_not_found with candidates.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            return unreal.send_command("remove_blueprint_variable",
                                       {"blueprint_name": blueprint_name,
                                        "variable_name": variable_name}) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error removing blueprint variable: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def rename_blueprint_variable(
        ctx: Context,
        blueprint_name: str,
        old_name: str,
        new_name: str
    ) -> Dict[str, Any]:
        """
        Rename a member variable; the graph nodes referencing it follow the new name.

        Args:
            blueprint_name: Name of the target Blueprint
            old_name: Current member variable name
            new_name: New member variable name

        Returns:
            Dict with old_name, new_name, the variable read back, reference_nodes_renamed /
            reference_nodes_stale (how many Get/Set nodes followed the rename) and the compile
            result. A name that is already taken reports variable_name_in_use with candidates;
            a variable with an OnRep function reports rep_notify_cleared, because the engine's
            rename path would otherwise open a modal dialog.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            return unreal.send_command("rename_blueprint_variable",
                                       {"blueprint_name": blueprint_name,
                                        "old_name": old_name,
                                        "new_name": new_name}) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error renaming blueprint variable: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def set_blueprint_variable_type(
        ctx: Context,
        blueprint_name: str,
        variable_name: str,
        variable_type: str,
        sub_class: str = None,
        container: str = None
    ) -> Dict[str, Any]:
        """
        Change a member variable's type (ChangeMemberVariableType).

        Args:
            blueprint_name: Name of the target Blueprint
            variable_name: Member variable to retype
            variable_type: Pin category (bool / int / float / object / struct / enum ...), with the
                same container suffixes as add_blueprint_variable ("int[]", "struct{}", "int{,}")
            sub_class: Class / struct / enum path for object and struct types
            container: Optional container kind (none / array / set / map) as an alternative to the
                type suffix; giving both a suffix and a different container is invalid_params

        Returns:
            Dict with the variable read back (type, container, ...) and the compile result.
            An unsupported type reports unsupported_variable_type with supported_types.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"blueprint_name": blueprint_name, "variable_name": variable_name,
                      "variable_type": variable_type}
            if sub_class:
                params["sub_class"] = sub_class
            if container:
                params["container"] = container

            return unreal.send_command("set_blueprint_variable_type", params) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error setting blueprint variable type: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def set_blueprint_variable_default_value(
        ctx: Context,
        blueprint_name: str,
        variable_name: str,
        default_value: str
    ) -> Dict[str, Any]:
        """
        Write a member variable's default value.

        The compiler parses FBPVariableDescription::DefaultValue into the class default object, so
        the value is applied by the recompile this command triggers - a value that cannot be parsed
        shows up in `errors` instead of silently reverting.

        Args:
            blueprint_name: Name of the target Blueprint
            variable_name: Member variable to write
            default_value: Default value as text ("42", "true", "(X=1,Y=2,Z=3)")

        Returns:
            Dict with default_value read back and the compile result.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            return unreal.send_command("set_blueprint_variable_default_value",
                                       {"blueprint_name": blueprint_name,
                                        "variable_name": variable_name,
                                        "default_value": default_value}) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error setting blueprint variable default value: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def set_blueprint_variable_flags(
        ctx: Context,
        blueprint_name: str,
        variable_name: str,
        props: Dict[str, Any]
    ) -> Dict[str, Any]:
        """
        Set member variable flags and metadata.

        Args:
            blueprint_name: Name of the target Blueprint
            variable_name: Member variable to change
            props: Flag name to value. Supported keys:
                instance_editable (bool), expose_on_spawn (bool), blueprint_read_only (bool),
                replicated (bool), rep_notify_func (string), category (string), tooltip (string),
                transient (bool), private (bool)

        Returns:
            Dict with applied / failed (one entry per key, with the flag read back), applied_count,
            failed_count, the variable's flags and the compile result. An unknown key lands in
            `failed` with the supported key names in candidates.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            return unreal.send_command("set_blueprint_variable_flags",
                                       {"blueprint_name": blueprint_name,
                                        "variable_name": variable_name,
                                        "props": props or {}}) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error setting blueprint variable flags: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def list_blueprint_local_variables(
        ctx: Context,
        blueprint_name: str,
        function_name: str
    ) -> Dict[str, Any]:
        """
        List the local variables of a function graph.

        Args:
            blueprint_name: Name of the target Blueprint
            function_name: Function graph holding the local variables

        Returns:
            Dict with function_name, variables and variable_count.
            A missing graph reports function_graph_not_found with candidates, and an override
            function (created with signature_class) reports function_not_editable.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            return unreal.send_command("list_blueprint_local_variables",
                                       {"blueprint_name": blueprint_name,
                                        "function_name": function_name}) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error listing local variables: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def add_blueprint_local_variable(
        ctx: Context,
        blueprint_name: str,
        function_name: str,
        variable_name: str,
        variable_type: str,
        sub_class: str = None,
        default_value: str = None
    ) -> Dict[str, Any]:
        """
        Add a local variable to a function graph (FBlueprintEditorUtils::AddLocalVariable).

        A local variable may not mask a member variable of the blueprint or of a parent class.

        Args:
            blueprint_name: Name of the target Blueprint
            function_name: Function graph that owns the local variable
            variable_name: Name of the new local variable
            variable_type: Pin category, with the same container suffixes as add_blueprint_variable
            sub_class: Class / struct / enum path for object and struct types
            default_value: Default value as text

        Returns:
            Dict with the local variables read back and the compile result.
            A name in use reports variable_name_in_use with candidates; an override function
            reports function_not_editable.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"blueprint_name": blueprint_name, "function_name": function_name,
                      "variable_name": variable_name, "variable_type": variable_type}
            if sub_class:
                params["sub_class"] = sub_class
            if default_value is not None:
                params["default_value"] = default_value

            return unreal.send_command("add_blueprint_local_variable", params) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error adding local variable: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def remove_blueprint_local_variable(
        ctx: Context,
        blueprint_name: str,
        function_name: str,
        variable_name: str
    ) -> Dict[str, Any]:
        """
        Remove a local variable and the graph nodes that reference it.

        Args:
            blueprint_name: Name of the target Blueprint
            function_name: Function graph that owns the local variable
            variable_name: Local variable to remove

        Returns:
            Dict with removed, the remaining local variables and the compile result.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            return unreal.send_command("remove_blueprint_local_variable",
                                       {"blueprint_name": blueprint_name,
                                        "function_name": function_name,
                                        "variable_name": variable_name}) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error removing local variable: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def rename_blueprint_local_variable(
        ctx: Context,
        blueprint_name: str,
        function_name: str,
        old_name: str,
        new_name: str
    ) -> Dict[str, Any]:
        """
        Rename a local variable; the graph nodes referencing it follow the new name.

        Args:
            blueprint_name: Name of the target Blueprint
            function_name: Function graph that owns the local variable
            old_name: Current local variable name
            new_name: New local variable name

        Returns:
            Dict with the local variables read back, reference_nodes_renamed /
            reference_nodes_stale and the compile result. A taken name reports
            variable_name_in_use with candidates (the engine's own rename silently no-ops).
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            return unreal.send_command("rename_blueprint_local_variable",
                                       {"blueprint_name": blueprint_name,
                                        "function_name": function_name,
                                        "old_name": old_name,
                                        "new_name": new_name}) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error renaming local variable: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def set_blueprint_local_variable_default(
        ctx: Context,
        blueprint_name: str,
        function_name: str,
        variable_name: str,
        default_value: str
    ) -> Dict[str, Any]:
        """
        Write a local variable's default value and read it back.

        Args:
            blueprint_name: Name of the target Blueprint
            function_name: Function graph that owns the local variable
            variable_name: Local variable to write
            default_value: Default value as text

        Returns:
            Dict with default_value read back and the compile result.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            return unreal.send_command("set_blueprint_local_variable_default",
                                       {"blueprint_name": blueprint_name,
                                        "function_name": function_name,
                                        "variable_name": variable_name,
                                        "default_value": default_value}) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error setting local variable default: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def get_blueprint_component_hierarchy(
        ctx: Context,
        blueprint_name: str
    ) -> Dict[str, Any]:
        """
        Read a Blueprint's component tree (the SCS), which get_asset_properties does not show.

        Args:
            blueprint_name: Name of the target Blueprint

        Returns:
            Dict with components: [{component_name, component_class, component_template, parent,
            attach_socket, is_root, is_inherited, children}], component_count, root_components,
            root_count, unique_root and duplicate_components (empty unless the SCS holds a name
            twice). A blueprint without a construction script reports blueprint_not_ready.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            return unreal.send_command("get_blueprint_component_hierarchy",
                                       {"blueprint_name": blueprint_name}) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error reading component hierarchy: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def remove_component_from_blueprint(
        ctx: Context,
        blueprint_name: str,
        component_name: str,
        recursive: bool = True,
        force: bool = False
    ) -> Dict[str, Any]:
        """
        Remove a component (and by default its children) from a Blueprint's construction script.

        Args:
            blueprint_name: Name of the target Blueprint
            component_name: Component variable name to remove
            recursive: Also remove the component's children (default True)
            force: Allow removing the blueprint's only root component (default False)

        Returns:
            Dict with removed_components, the remaining components read back (component_count,
            root_components, unique_root) and the compile result.
            recursive=False with children reports component_has_children (with the child names);
            the only root reports root_component_protected unless force=True; a missing component
            reports component_not_found with candidates.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            params = {"blueprint_name": blueprint_name, "component_name": component_name,
                      "recursive": recursive, "force": force}
            return unreal.send_command("remove_component_from_blueprint", params) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error removing component: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def set_blueprint_root_component(
        ctx: Context,
        blueprint_name: str,
        component_name: str
    ) -> Dict[str, Any]:
        """
        Make a component the Blueprint's SCS root; the previous root becomes its child.

        Args:
            blueprint_name: Name of the target Blueprint
            component_name: Scene component to promote to root

        Returns:
            Dict with the whole hierarchy read back, including root_components, root_count and
            unique_root (the response must show exactly one root), plus the compile result.
            A non-scene component reports component_not_scene.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            return unreal.send_command("set_blueprint_root_component",
                                       {"blueprint_name": blueprint_name,
                                        "component_name": component_name}) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error setting root component: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def detach_component(
        ctx: Context,
        blueprint_name: str,
        component_name: str
    ) -> Dict[str, Any]:
        """
        Detach a component from its parent and leave it under the SCS root (it is not deleted).

        Args:
            blueprint_name: Name of the target Blueprint
            component_name: Scene component to detach

        Returns:
            Dict with the hierarchy read back (the component's parent is the SCS root, or it is the
            root itself) and the compile result. A non-scene component reports component_not_scene.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            return unreal.send_command("detach_component",
                                       {"blueprint_name": blueprint_name,
                                        "component_name": component_name}) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error detaching component: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def set_component_collision(
        ctx: Context,
        blueprint_name: str,
        component_name: str,
        props: Dict[str, Any]
    ) -> Dict[str, Any]:
        """
        Configure a primitive component's collision.

        Args:
            blueprint_name: Name of the target Blueprint
            component_name: Primitive component variable name
            props: Collision field to value. Supported keys:
                collision_enabled (enum name, e.g. "NoCollision" / "QueryAndPhysics"),
                collision_profile (profile name, e.g. "BlockAll"),
                object_type (channel name, e.g. "WorldStatic"),
                responses (object of channel name to response name, e.g. {"Visibility": "ECR_Ignore"})

        Returns:
            Dict with applied / failed (one entry per key), applied_count, failed_count and the
            collision read back (collision_enabled / collision_profile / object_type / responses),
            plus the compile result. A component without a BodyInstance reports
            component_not_primitive; an unknown key lands in `failed` with candidates.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            return unreal.send_command("set_component_collision",
                                       {"blueprint_name": blueprint_name,
                                        "component_name": component_name,
                                        "props": props or {}}) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error setting component collision: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def implement_blueprint_interface(
        ctx: Context,
        blueprint_name: str,
        interface_path: str
    ) -> Dict[str, Any]:
        """
        Implement an interface and read back the graphs the engine generated for it.

        Args:
            blueprint_name: Name of the target Blueprint
            interface_path: Interface class path ("/Script/Engine.Interface_AssetUserData") or a
                blueprint interface asset path ("/Game/BPI/BPI_Interact")

        Returns:
            Dict with implemented, already_implemented (implementing twice must not generate a
            second set of graphs), generated_graphs, the interface description and the compile
            result. An unresolvable path reports interface_not_found with candidates.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            return unreal.send_command("implement_blueprint_interface",
                                       {"blueprint_name": blueprint_name,
                                        "interface_path": interface_path}) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error implementing interface: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def unimplement_blueprint_interface(
        ctx: Context,
        blueprint_name: str,
        interface_path: str,
        preserve_functions: bool = False
    ) -> Dict[str, Any]:
        """
        Remove an interface implementation and the graphs it produced.

        Args:
            blueprint_name: Name of the target Blueprint
            interface_path: Interface class path or blueprint interface asset path
            preserve_functions: Keep the interface functions as normal blueprint functions

        Returns:
            Dict with removed, removed_graphs, the remaining interfaces / function graphs and the
            compile result. An interface the blueprint does not implement reports
            interface_not_found with the implemented interface paths as candidates.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            return unreal.send_command("unimplement_blueprint_interface",
                                       {"blueprint_name": blueprint_name,
                                        "interface_path": interface_path,
                                        "preserve_functions": preserve_functions}) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error removing interface: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def list_blueprint_interfaces(
        ctx: Context,
        blueprint_name: str
    ) -> Dict[str, Any]:
        """
        List a Blueprint's implemented interfaces and each interface function's implementation state.

        Args:
            blueprint_name: Name of the target Blueprint

        Returns:
            Dict with interfaces: [{interface_name, interface_path, graphs,
            functions: [{function_name, implemented}]}] and interface_count.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            return unreal.send_command("list_blueprint_interfaces",
                                       {"blueprint_name": blueprint_name}) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error listing interfaces: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def list_overridable_functions(
        ctx: Context,
        blueprint_name: str,
        include_parent_events: bool = True
    ) -> Dict[str, Any]:
        """
        List the parent / interface functions this Blueprint can override.

        Use this before add_blueprint_function_graph(signature_class=...) so the declaring class and
        function name are read from the engine instead of guessed.

        Args:
            blueprint_name: Name of the target Blueprint
            include_parent_events: Include parent class events as well as interface functions

        Returns:
            Dict with functions: [{function_name, declaring_class, signature_class, source,
            overridden}] (overridden means a graph or event of that name already exists) and
            function_count / overridden_count. Plain BlueprintCallable functions are not listed.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            return unreal.send_command("list_overridable_functions",
                                       {"blueprint_name": blueprint_name,
                                        "include_parent_events": include_parent_events}) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error listing overridable functions: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def check_blueprint_element(
        ctx: Context,
        blueprint_name: str,
        kind: str,
        name: str
    ) -> Dict[str, Any]:
        """
        Check whether one element exists in a Blueprint, and locate it.

        This is the single existence entry point: it replaces a pile of per-kind "exists" commands,
        and new kinds are enum values here rather than new commands.

        Args:
            blueprint_name: Name of the target Blueprint
            kind: One of variable / function / component / graph / node
            name: Name to look for (a node guid for kind="node")

        Returns:
            Dict with found plus the locating fields (variable: type / container / sub_class;
            function and graph: graph_name / node_count; component: component_class /
            component_template / parent / is_root; node: graph_name / node_type / node_title).
            found=false is a normal answer, not an error code; an unknown kind reports
            invalid_params with the legal kinds.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            return unreal.send_command("check_blueprint_element",
                                       {"blueprint_name": blueprint_name,
                                        "kind": kind,
                                        "name": name}) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error checking blueprint element: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def compare_blueprints(
        ctx: Context,
        blueprint_a: str,
        blueprint_b: str
    ) -> Dict[str, Any]:
        """
        Compare two Blueprints: variables, function graphs, components and interfaces.

        Args:
            blueprint_a: First Blueprint (left side)
            blueprint_b: Second Blueprint (right side)

        Returns:
            Dict with variables / functions / components / interfaces, each holding only_in_a,
            only_in_b and changed (name plus value_in_a / value_in_b), plus sides.a / sides.b with
            each blueprint's parent_class. A path that does not resolve reports blueprint_not_found
            with side "a" or "b".
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            return unreal.send_command("compare_blueprints",
                                       {"blueprint_a": blueprint_a,
                                        "blueprint_b": blueprint_b}) or {
                "success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error comparing blueprints: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    logger.info("Blueprint tools registered successfully") 