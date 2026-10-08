// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class UnrealMCP : ModuleRules
{
	public UnrealMCP(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		// Use IWYUSupport instead of the deprecated bEnforceIWYU in UE5.5
		IWYUSupport = IWYUSupport.Full;

		// Live Coding (hot reload) commands. ILiveCodingModule only exists in the Windows engine
		// module, so the dependency and the code that uses it are both Windows-only.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("LiveCoding");
			PublicDefinitions.Add("UNREALMCP_WITH_LIVE_CODING=1");
		}
		else
		{
			PublicDefinitions.Add("UNREALMCP_WITH_LIVE_CODING=0");
		}

		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
		);
		
		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
		);
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"InputCore",
				"Networking",
				"Sockets",
				"HTTP",
				"Json",
				"JsonUtilities",
				"DeveloperSettings"
			}
		);
		
			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"UnrealEd",
					"EditorScriptingUtilities",
					"EditorSubsystem",
					"Slate",
					"SlateCore",
					"RHI",                 // For GetFeatureLevelShaderPlatform (material resource lookup on 5.7+)
					"ApplicationCore",     // For IPlatformInputDeviceMapper::Get, which the inline
					                       // FInputKeyEventArgs(FInputDeviceId, ...) constructor references
					                       // (inject_key). Slate/SlateCore pull the module in, but a static
					                       // import symbol needs the module linked into THIS binary: without
					                       // the dependency the build compiles and then fails at LNK2019.
					"UMG",
					"MovieScene",          // For UMovieScene / AddPossessable / property tracks (widget animation authoring)
					"MovieSceneTracks",    // For UMovieSceneFloatTrack / UMovieSceneByteTrack and their sections
					"Kismet",
					"KismetCompiler",
					"BlueprintGraph",
					"Projects",
					"AssetRegistry",
					"AssetTools",          // For UAssetImportTask / FAssetToolsModule (import_assets)
					"ClothingSystemRuntimeCommon",  // For UClothingAssetCommon::ApplyParameterMasks (apply_cloth_masks)
					"PhysicsUtilities",    // For FPhysicsAssetUtils::CreateNewBody / CreateNewConstraint (add_physics_asset_body / add_physics_asset_constraint)
					"PhysicsCore"          // For EPhysicsType (UBodySetup::PhysicsType) read back by list_physics_asset_bodies
				}
			);
		
			if (Target.bBuildEditor == true)
				{
					PrivateDependencyModuleNames.AddRange(
						new string[]
						{
							"PropertyEditor",      // For widget property editing
							"ToolMenus",           // For editor UI
							"BlueprintEditorLibrary", // For Blueprint utilities
							"UMGEditor",           // For WidgetBlueprint.h and other UMG editor functionality
							"PythonScriptPlugin",  // For executing Python commands in editor
							"MaterialEditor",      // For material expression editing (UMaterialEditingLibrary)
							"AnimGraph",           // For UAnimPreviewInstance (animation editor session / preview control)
							"LevelEditor",         // For ULevelEditorSubsystem (PIE lifecycle: play / end play / is playing)
							"PCG"                  // For the read-only PCG probing commands (UPCGGraph / UPCGComponent / FPCGDataCollection)
						}
					);
				}
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
		);
	}
} 