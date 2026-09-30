#include "Commands/UnrealMCPEditorCommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "LevelEditorViewport.h"
#include "ImageUtils.h"
#include "HighResScreenshot.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#include "Engine/World.h"
#include "Misc/FileHelper.h"
#include "GameFramework/Actor.h"
#include "Engine/Selection.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Camera/CameraActor.h"
#include "Components/StaticMeshComponent.h"
#include "EditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "Particles/Emitter.h"
#include "UObject/UObjectIterator.h"

FUnrealMCPEditorCommands::FUnrealMCPEditorCommands()
{
}

void FUnrealMCPEditorCommands::RegisterCommands(FMCPCommandRegistry& Registry)
{
    MCP_REGISTER_COMMAND(Registry, "get_actors_in_level", "editor", "List every actor in the editor level.", {}, MCPFlags(),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleGetActorsInLevel(Params); });

    MCP_REGISTER_COMMAND(Registry, "find_actors_by_name", "editor", "List actors whose name contains the given pattern.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("pattern"), TEXT("string"), TEXT("Substring matched against the actor name")),
        }), MCPFlags(),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleFindActorsByName(Params); });

    MCP_REGISTER_COMMAND(Registry, "spawn_actor", "editor", "Spawn an actor by class name (any AActor subclass: StaticMeshActor, PointLight, SkyLight, SkyAtmosphere, ExponentialHeightFog, PostProcessVolume, CameraActor, ...).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("type"), TEXT("string"), TEXT("Actor class to spawn")),
            MCPParam(TEXT("name"), TEXT("string"), TEXT("Name for the new actor (must not exist yet)")),
            MCPParamOpt(TEXT("location"), TEXT("array"), TEXT("[X, Y, Z] world location")),
            MCPParamOpt(TEXT("rotation"), TEXT("array"), TEXT("[Pitch, Yaw, Roll] world rotation")),
            MCPParamOpt(TEXT("scale"), TEXT("array"), TEXT("[X, Y, Z] scale")),
        }), MCPFlags(),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleSpawnActor(Params); });

    // Deprecated alias: kept because the dispatcher still routes it.
    MCP_REGISTER_COMMAND(Registry, "create_actor", "editor", "Deprecated alias of spawn_actor.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("type"), TEXT("string"), TEXT("Actor class to spawn")),
            MCPParam(TEXT("name"), TEXT("string"), TEXT("Name for the new actor (must not exist yet)")),
            MCPParamOpt(TEXT("location"), TEXT("array"), TEXT("[X, Y, Z] world location")),
            MCPParamOpt(TEXT("rotation"), TEXT("array"), TEXT("[Pitch, Yaw, Roll] world rotation")),
            MCPParamOpt(TEXT("scale"), TEXT("array"), TEXT("[X, Y, Z] scale")),
        }), MCPFlags(),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleSpawnActor(Params); });

    MCP_REGISTER_COMMAND(Registry, "spawn_particle_actor", "editor", "Spawn an emitter for a particle system asset, optionally attached to an actor.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("template"), TEXT("string"), TEXT("Path of the particle system asset")),
            MCPParam(TEXT("name"), TEXT("string"), TEXT("Name for the new emitter actor (must not exist yet)")),
            MCPParamOpt(TEXT("attach_to"), TEXT("string"), TEXT("Actor to attach the emitter to")),
            MCPParamOpt(TEXT("location"), TEXT("array"), TEXT("[X, Y, Z] world location")),
            MCPParamOpt(TEXT("rotation"), TEXT("array"), TEXT("[Pitch, Yaw, Roll] world rotation")),
            MCPParamOpt(TEXT("relative_location"), TEXT("array"), TEXT("[X, Y, Z] offset from the attach parent")),
            MCPParamOpt(TEXT("relative_rotation"), TEXT("array"), TEXT("[Pitch, Yaw, Roll] rotation relative to the attach parent")),
            MCPParamOpt(TEXT("auto_activate"), TEXT("bool"), TEXT("Activate the system after spawning (default true)")),
        }), MCPFlags(),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleSpawnParticleActor(Params); });

    MCP_REGISTER_COMMAND(Registry, "delete_actor", "editor", "Destroy the actor with the given name.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("name"), TEXT("string"), TEXT("Name of the actor to destroy")),
        }), MCPFlags(),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleDeleteActor(Params); });

    MCP_REGISTER_COMMAND(Registry, "set_actor_transform", "editor", "Apply a new location, rotation and/or scale to an actor.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("name"), TEXT("string"), TEXT("Name of the actor to move")),
            MCPParamOpt(TEXT("location"), TEXT("array"), TEXT("[X, Y, Z] world location")),
            MCPParamOpt(TEXT("rotation"), TEXT("array"), TEXT("[Pitch, Yaw, Roll] world rotation")),
            MCPParamOpt(TEXT("scale"), TEXT("array"), TEXT("[X, Y, Z] scale")),
        }), MCPFlags(),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleSetActorTransform(Params); });

    MCP_REGISTER_COMMAND(Registry, "set_actor_location_safe", "editor",
        "Move a level actor with explicit sweep/teleport control - the flags set_actor_transform does not expose.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("name"), TEXT("string"), TEXT("Level actor name")),
            MCPParam(TEXT("location"), TEXT("array"), TEXT("[x, y, z] world location")),
            MCPParamOpt(TEXT("sweep"), TEXT("bool"), TEXT("Sweep against blocking geometry (default false)")),
            MCPParamOpt(TEXT("teleport"), TEXT("bool"), TEXT("Teleport physics (default false)")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleSetActorLocationSafe(Params); });

    MCP_REGISTER_COMMAND(Registry, "set_actor_custom_depth_safe", "editor",
        "Enable or disable custom depth and set the stencil value on every primitive component of an actor.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("name"), TEXT("string"), TEXT("Level actor name")),
            MCPParamOpt(TEXT("stencil_value"), TEXT("number"), TEXT("Stencil value 0-255 (default 1)")),
            MCPParamOpt(TEXT("enabled"), TEXT("bool"), TEXT("Enable custom depth (default true)")),
        }), MCPFlags(false, false, false),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleSetActorCustomDepthSafe(Params); });

    MCP_REGISTER_COMMAND(Registry, "get_actor_properties", "editor", "Return an actor's transform and reflected properties.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("name"), TEXT("string"), TEXT("Name of the actor to read")),
        }), MCPFlags(),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleGetActorProperties(Params); });

    MCP_REGISTER_COMMAND(Registry, "set_actor_property", "editor", "Set one property on an actor.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("name"), TEXT("string"), TEXT("Name of the actor to change")),
            MCPParam(TEXT("property_name"), TEXT("string"), TEXT("Property name on the actor class")),
            MCPParam(TEXT("property_value"), TEXT("string"), TEXT("Value to set (number / bool / string / array, as the property accepts)")),
        }), MCPFlags(),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleSetActorProperty(Params); });

    MCP_REGISTER_COMMAND(Registry, "spawn_blueprint_actor", "editor", "Spawn an instance of a Blueprint class in the level.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("blueprint_name"), TEXT("string"), TEXT("Name of the Blueprint asset")),
            MCPParam(TEXT("actor_name"), TEXT("string"), TEXT("Name (and label) for the new actor")),
            MCPParamOpt(TEXT("location"), TEXT("array"), TEXT("[X, Y, Z] world location")),
            MCPParamOpt(TEXT("rotation"), TEXT("array"), TEXT("[Pitch, Yaw, Roll] world rotation")),
            MCPParamOpt(TEXT("scale"), TEXT("array"), TEXT("[X, Y, Z] scale")),
        }), MCPFlags(),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleSpawnBlueprintActor(Params); });

    MCP_REGISTER_COMMAND(Registry, "focus_viewport", "editor", "Point the level viewport at an actor or a location.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("target"), TEXT("string"), TEXT("Actor to look at (alternative to 'location')")),
            MCPParamOpt(TEXT("location"), TEXT("array"), TEXT("[X, Y, Z] point to look at (alternative to 'target')")),
            MCPParamOpt(TEXT("distance"), TEXT("float"), TEXT("Distance to keep from the target (default 1000)")),
            MCPParamOpt(TEXT("orientation"), TEXT("array"), TEXT("[Pitch, Yaw, Roll] viewport rotation")),
        }), MCPFlags(),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleFocusViewport(Params); });

    // Queued onto the GameThread when called over the python loopback, hence the flag.
    MCP_REGISTER_COMMAND(Registry, "take_screenshot", "editor", "Save a PNG to disk: source=level_viewport (default) reads the active level viewport backbuffer (no PIE, no UMG); source=pie takes a Slate screenshot of the PIE game viewport widget, which includes UMG. The pie source is for UI layout triage only, never an acceptance verdict.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("filepath"), TEXT("string"), TEXT("Output file path (.png appended when missing)")),
            MCPParamOpt(TEXT("source"), TEXT("string"), TEXT("level_viewport (default) or pie")),
        }), MCPFlags(/*bLoopbackForbidden=*/true),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleTakeScreenshot(Params); });

    MCP_REGISTER_COMMAND(Registry, "get_console_variable", "editor", "Read a console variable's current value.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("name"), TEXT("string"), TEXT("Console variable name")),
        }), MCPFlags(),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleGetConsoleVariable(Params); });

    MCP_REGISTER_COMMAND(Registry, "set_console_variable", "editor", "Set a console variable's value.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("name"), TEXT("string"), TEXT("Console variable name")),
            MCPParam(TEXT("value"), TEXT("string"), TEXT("Value to assign (parsed by the variable)")),
        }), MCPFlags(),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleSetConsoleVariable(Params); });

    MCP_REGISTER_COMMAND(Registry, "close_asset_editors", "editor", "Close every editor tab showing an asset and restore its remembered tab location.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Package path of the asset, e.g. /Game/BP_Thing")),
        }), MCPFlags(),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleCloseAssetEditors(Params); });
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleGetActorsInLevel(const TSharedPtr<FJsonObject>& Params)
{
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
    
    TArray<TSharedPtr<FJsonValue>> ActorArray;
    for (AActor* Actor : AllActors)
    {
        if (Actor)
        {
            ActorArray.Add(FUnrealMCPCommonUtils::ActorToJson(Actor));
        }
    }
    
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("actors"), ActorArray);
    
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleFindActorsByName(const TSharedPtr<FJsonObject>& Params)
{
    FString Pattern;
    if (!Params->TryGetStringField(TEXT("pattern"), Pattern))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'pattern' parameter"));
    }
    
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
    
    TArray<TSharedPtr<FJsonValue>> MatchingActors;
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName().Contains(Pattern))
        {
            MatchingActors.Add(FUnrealMCPCommonUtils::ActorToJson(Actor));
        }
    }
    
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("actors"), MatchingActors);
    
    return ResultObj;
}

namespace
{
    /**
     * Turn a type name into a spawnable actor class. Accepts a plain class name ("SkyLight"), the
     * "A"-prefixed form ("ASkyLight"), a full object path ("/Script/Engine.SkyLight") and matches the
     * name case-insensitively, because the python tool upper-cases the type before it reaches us.
     */
    UClass* FindSpawnableActorClass(const FString& TypeName)
    {
        TArray<FString> Candidates;
        Candidates.Add(TypeName);
        if (!TypeName.Contains(TEXT(".")))
        {
            Candidates.Add(FString::Printf(TEXT("/Script/Engine.%s"), *TypeName));
            if (!TypeName.StartsWith(TEXT("A")))
            {
                Candidates.Add(TEXT("A") + TypeName);
            }
        }

        for (const FString& Candidate : Candidates)
        {
            UClass* Class = FindObject<UClass>(nullptr, *Candidate);
            if (!Class)
            {
                Class = UClass::TryFindTypeSlow<UClass>(Candidate);
            }
            if (Class && Class->IsChildOf(AActor::StaticClass()) && !Class->HasAnyClassFlags(CLASS_Abstract))
            {
                return Class;
            }
        }

        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            if (Class && Class->IsChildOf(AActor::StaticClass())
                && !Class->HasAnyClassFlags(CLASS_Abstract)
                && Class->GetName().Equals(TypeName, ESearchCase::IgnoreCase))
            {
                return Class;
            }
        }

        return nullptr;
    }
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSpawnActor(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString ActorType;
    if (!Params->TryGetStringField(TEXT("type"), ActorType))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'type' parameter"));
    }

    // Get actor name (required parameter)
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }

    // Get optional transform parameters
    FVector Location(0.0f, 0.0f, 0.0f);
    FRotator Rotation(0.0f, 0.0f, 0.0f);
    FVector Scale(1.0f, 1.0f, 1.0f);

    if (Params->HasField(TEXT("location")))
    {
        Location = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location"));
    }
    if (Params->HasField(TEXT("rotation")))
    {
        Rotation = FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation"));
    }
    if (Params->HasField(TEXT("scale")))
    {
        Scale = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("scale"));
    }

    // Create the actor based on type
    AActor* NewActor = nullptr;
    UWorld* World = GEditor->GetEditorWorldContext().World();

    if (!World)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    // Check if an actor with this name already exists
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(World, AActor::StaticClass(), AllActors);
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor with name '%s' already exists"), *ActorName));
        }
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.Name = *ActorName;

    // Any AActor subclass by name, instead of the handful that used to be hard-coded here (SkyLight,
    // SkyAtmosphere, ExponentialHeightFog, PostProcessVolume and friends were unreachable).
    UClass* ActorClass = FindSpawnableActorClass(ActorType);
    if (!ActorClass)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Unknown actor type: %s (expected an AActor class name such as StaticMeshActor, PointLight, "
                 "SpotLight, DirectionalLight, SkyLight, SkyAtmosphere, ExponentialHeightFog, "
                 "PostProcessVolume or CameraActor)"), *ActorType));
    }
    NewActor = World->SpawnActor<AActor>(ActorClass, Location, Rotation, SpawnParams);

    if (NewActor)
    {
        // Apply the whole requested transform instead of only the scale: a light spawned with a rotation does
        // not keep it (a DirectionalLight asked for [10, 20, 30] lands on (-29.53, -4.42, 34.47), while the
        // same call on a PointLight is exact), and callers expect the rotation they asked for.
        NewActor->SetActorTransform(FTransform(Rotation, Location, Scale));

        // Set the actor label so the Outliner shows the requested name
        NewActor->SetActorLabel(*ActorName);

        // Return the created actor's details
        return FUnrealMCPCommonUtils::ActorToJsonObject(NewActor, true);
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create actor"));
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleDeleteActor(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }

    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
    
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName)
        {
            // Store actor info before deletion for the response
            TSharedPtr<FJsonObject> ActorInfo = FUnrealMCPCommonUtils::ActorToJsonObject(Actor);
            
            // Delete the actor
            Actor->Destroy();
            
            TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
            ResultObj->SetObjectField(TEXT("deleted_actor"), ActorInfo);
            return ResultObj;
        }
    }
    
    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSetActorTransform(const TSharedPtr<FJsonObject>& Params)
{
    // Get actor name
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }

    // Find the actor
    AActor* TargetActor = nullptr;
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
    
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName)
        {
            TargetActor = Actor;
            break;
        }
    }

    if (!TargetActor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }

    // Get transform parameters
    FTransform NewTransform = TargetActor->GetTransform();

    if (Params->HasField(TEXT("location")))
    {
        NewTransform.SetLocation(FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location")));
    }
    if (Params->HasField(TEXT("rotation")))
    {
        NewTransform.SetRotation(FQuat(FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation"))));
    }
    if (Params->HasField(TEXT("scale")))
    {
        NewTransform.SetScale3D(FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("scale")));
    }

    // Set the new transform
    TargetActor->SetActorTransform(NewTransform);

    // Return updated actor info
    return FUnrealMCPCommonUtils::ActorToJsonObject(TargetActor, true);
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleGetActorProperties(const TSharedPtr<FJsonObject>& Params)
{
    // Get actor name
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }

    // Find the actor
    AActor* TargetActor = nullptr;
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
    
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName)
        {
            TargetActor = Actor;
            break;
        }
    }

    if (!TargetActor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }

    // Always return detailed properties for this command
    return FUnrealMCPCommonUtils::ActorToJsonObject(TargetActor, true);
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSetActorProperty(const TSharedPtr<FJsonObject>& Params)
{
    // Get actor name
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }

    // Find the actor
    AActor* TargetActor = nullptr;
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
    
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName)
        {
            TargetActor = Actor;
            break;
        }
    }

    if (!TargetActor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }

    // Get property name
    FString PropertyName;
    if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'property_name' parameter"));
    }

    // Get property value
    if (!Params->HasField(TEXT("property_value")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'property_value' parameter"));
    }
    
    TSharedPtr<FJsonValue> PropertyValue = Params->Values.FindRef(TEXT("property_value"));
    
    // Set the property using our utility function
    FString ErrorMessage;
    if (FUnrealMCPCommonUtils::SetObjectProperty(TargetActor, PropertyName, PropertyValue, ErrorMessage))
    {
        // Property set successfully
        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("actor"), ActorName);
        ResultObj->SetStringField(TEXT("property"), PropertyName);
        ResultObj->SetBoolField(TEXT("success"), true);
        
        // Also include the full actor details
        ResultObj->SetObjectField(TEXT("actor_details"), FUnrealMCPCommonUtils::ActorToJsonObject(TargetActor, true));
        return ResultObj;
    }
    else
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSpawnBlueprintActor(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString ActorName;
    if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'actor_name' parameter"));
    }

    // Find the blueprint (in-memory lookup, supports not-yet-saved assets)
    if (BlueprintName.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Blueprint name is empty"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Get transform parameters
    FVector Location(0.0f, 0.0f, 0.0f);
    FRotator Rotation(0.0f, 0.0f, 0.0f);
    FVector Scale(1.0f, 1.0f, 1.0f);

    if (Params->HasField(TEXT("location")))
    {
        Location = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location"));
    }
    if (Params->HasField(TEXT("rotation")))
    {
        Rotation = FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation"));
    }
    if (Params->HasField(TEXT("scale")))
    {
        Scale = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("scale"));
    }

    // Spawn the actor
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    FTransform SpawnTransform;
    SpawnTransform.SetLocation(Location);
    SpawnTransform.SetRotation(FQuat(Rotation));
    SpawnTransform.SetScale3D(Scale);

    FActorSpawnParameters SpawnParams;
    SpawnParams.Name = *ActorName;

    AActor* NewActor = World->SpawnActor<AActor>(Blueprint->GeneratedClass, SpawnTransform, SpawnParams);
    if (NewActor)
    {
        // Set the actor label so the Outliner shows the requested name
        NewActor->SetActorLabel(*ActorName);
        return FUnrealMCPCommonUtils::ActorToJsonObject(NewActor, true);
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to spawn blueprint actor"));
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleFocusViewport(const TSharedPtr<FJsonObject>& Params)
{
    // Get target actor name if provided
    FString TargetActorName;
    bool HasTargetActor = Params->TryGetStringField(TEXT("target"), TargetActorName);

    // Get location if provided
    FVector Location(0.0f, 0.0f, 0.0f);
    bool HasLocation = false;
    if (Params->HasField(TEXT("location")))
    {
        Location = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location"));
        HasLocation = true;
    }

    // Get distance
    float Distance = 1000.0f;
    if (Params->HasField(TEXT("distance")))
    {
        Distance = Params->GetNumberField(TEXT("distance"));
    }

    // Get orientation if provided
    FRotator Orientation(0.0f, 0.0f, 0.0f);
    bool HasOrientation = false;
    if (Params->HasField(TEXT("orientation")))
    {
        Orientation = FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("orientation"));
        HasOrientation = true;
    }

    // Get the active viewport
    FLevelEditorViewportClient* ViewportClient = (FLevelEditorViewportClient*)GEditor->GetActiveViewport()->GetClient();
    if (!ViewportClient)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get active viewport"));
    }

    // If we have a target actor, focus on it
    if (HasTargetActor)
    {
        // Find the actor
        AActor* TargetActor = nullptr;
        TArray<AActor*> AllActors;
        UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
        
        for (AActor* Actor : AllActors)
        {
            if (Actor && Actor->GetName() == TargetActorName)
            {
                TargetActor = Actor;
                break;
            }
        }

        if (!TargetActor)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor not found: %s"), *TargetActorName));
        }

        // Focus on the actor
        ViewportClient->SetViewLocation(TargetActor->GetActorLocation() - FVector(Distance, 0.0f, 0.0f));
    }
    // Otherwise use the provided location
    else if (HasLocation)
    {
        ViewportClient->SetViewLocation(Location - FVector(Distance, 0.0f, 0.0f));
    }
    else
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Either 'target' or 'location' must be provided"));
    }

    // Set orientation if provided
    if (HasOrientation)
    {
        ViewportClient->SetViewRotation(Orientation);
    }

    // Force viewport to redraw
    ViewportClient->Invalidate();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleTakeScreenshot(const TSharedPtr<FJsonObject>& Params)
{
    // Get file path parameter
    FString FilePath;
    if (!Params->TryGetStringField(TEXT("filepath"), FilePath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'filepath' parameter"));
    }
    
    // Ensure the file path has a proper extension
    if (!FilePath.EndsWith(TEXT(".png")))
    {
        FilePath += TEXT(".png");
    }

    FString Source = TEXT("level_viewport");
    Params->TryGetStringField(TEXT("source"), Source);
    if (Source != TEXT("level_viewport") && Source != TEXT("pie"))
    {
        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"),
            FString::Printf(TEXT("Unknown source '%s'; use level_viewport or pie"), *Source));
        TArray<TSharedPtr<FJsonValue>> Sources;
        Sources.Add(MakeShared<FJsonValueString>(TEXT("level_viewport")));
        Sources.Add(MakeShared<FJsonValueString>(TEXT("pie")));
        Error->SetArrayField(TEXT("candidates"), Sources);
        return Error;
    }

    auto SavePng = [&FilePath, &Source](int32 Width, int32 Height, const TArray<FColor>& Pixels) -> TSharedPtr<FJsonObject>
    {
        TArray<uint8> CompressedBitmap;
        FImageUtils::CompressImageArray(Width, Height, Pixels, CompressedBitmap);
        if (!FFileHelper::SaveArrayToFile(CompressedBitmap, *FilePath))
        {
            return nullptr;
        }
        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("filepath"), FilePath);
        ResultObj->SetNumberField(TEXT("width"), Width);
        ResultObj->SetNumberField(TEXT("height"), Height);
        ResultObj->SetStringField(TEXT("source"), Source);
        // 回传文件大小，供客户端做黑帧/空帧校验（过小则重试）
        const int64 FileSize = IFileManager::Get().FileSize(*FilePath);
        if (FileSize >= 0)
        {
            ResultObj->SetNumberField(TEXT("file_size"), (double)FileSize);
        }
        return ResultObj;
    };

    // PIE: UMG is composited by Slate on top of the game viewport widget, not drawn into the scene
    // backbuffer, so it is only in a Slate screenshot of that widget.
    if (Source == TEXT("pie"))
    {
        UWorld* PlayWorld = GEditor ? GEditor->PlayWorld.Get() : nullptr;
        if (!PlayWorld)
        {
            TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("pie_not_running"),
                TEXT("source=pie needs a running PIE session"));
            Error->SetStringField(TEXT("hint"), TEXT("Call start_pie first; for UI layout triage only, never as acceptance"));
            return Error;
        }
        UGameViewportClient* GameViewport = PlayWorld->GetGameViewport();
        TSharedPtr<SViewport> ViewportWidget = GameViewport ? GameViewport->GetGameViewportWidget() : nullptr;
        if (!ViewportWidget.IsValid() || !FSlateApplication::IsInitialized()
            || !FSlateApplication::Get().FindWidgetWindow(ViewportWidget.ToSharedRef()).IsValid())
        {
            TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("pie_not_ready"),
                TEXT("The PIE game viewport widget is not available yet"));
            Error->SetStringField(TEXT("hint"), TEXT("PIE is still starting; retry in a later call"));
            return Error;
        }

        TArray<FColor> Pixels;
        FIntVector Size;
        TSharedPtr<SWindow> WidgetWindow = FSlateApplication::Get().FindWidgetWindow(ViewportWidget.ToSharedRef());
        if (FSlateApplication::Get().TakeScreenshot(ViewportWidget.ToSharedRef(), Pixels, Size) && Size.X > 0 && Size.Y > 0)
        {
            if (TSharedPtr<FJsonObject> Saved = SavePng(Size.X, Size.Y, Pixels))
            {
                return Saved;
            }
        }

        // Slate only has pixels for a window that is actually on screen: an unfocused or minimized
        // editor yields a zero-sized capture. Say which it was instead of a bare failure.
        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("pie_screenshot_failed"),
            TEXT("Slate could not capture the PIE game viewport widget"));
        Error->SetNumberField(TEXT("captured_width"), Size.X);
        Error->SetNumberField(TEXT("captured_height"), Size.Y);
        if (WidgetWindow.IsValid())
        {
            Error->SetBoolField(TEXT("window_visible"), WidgetWindow->IsVisible());
            Error->SetBoolField(TEXT("window_minimized"), WidgetWindow->IsWindowMinimized());
        }
        Error->SetStringField(TEXT("hint"),
            TEXT("The PIE window has to be on screen and not minimized for Slate to paint it; bring the editor window to the front and retry"));
        return Error;
    }

    // Get the active viewport
    if (GEditor && GEditor->GetActiveViewport())
    {
        FViewport* Viewport = GEditor->GetActiveViewport();

        // 强制重绘：非实时视口的 backbuffer 是陈旧的（材质改完后截图字节
        // 完全不变的元凶）。置全部编辑器视口 realtime + invalidate，再同步
        // 画一帧，保证 ReadPixels 读到当前场景状态。
        for (FEditorViewportClient* ViewportClient : GEditor->GetAllViewportClients())
        {
            if (ViewportClient)
            {
                ViewportClient->SetRealtime(true);
                ViewportClient->Invalidate();
            }
        }
        Viewport->Draw();

        TArray<FColor> Bitmap;
        FIntRect ViewportRect(0, 0, Viewport->GetSizeXY().X, Viewport->GetSizeXY().Y);

        if (ViewportRect.Width() > 0 && ViewportRect.Height() > 0
            && Viewport->ReadPixels(Bitmap, FReadSurfaceDataFlags(), ViewportRect))
        {
            if (TSharedPtr<FJsonObject> Saved = SavePng(ViewportRect.Width(), ViewportRect.Height(), Bitmap))
            {
                return Saved;
            }
        }
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to take screenshot"));
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleGetConsoleVariable(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (!Params->TryGetStringField(TEXT("name"), Name))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }

    // python 侧没有读取 cvar 的 API，只能由 C++ 提供
    IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*Name);
    if (!CVar)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Console variable not found: %s"), *Name));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("name"), Name);
    ResultObj->SetStringField(TEXT("value"), CVar->GetString());
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSpawnParticleActor(const TSharedPtr<FJsonObject>& Params)
{
    FString TemplatePath;
    if (!Params->TryGetStringField(TEXT("template"), TemplatePath) || TemplatePath.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'template' parameter (the particle system asset path)"));
    }

    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName) || ActorName.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }

    UParticleSystem* Template = Cast<UParticleSystem>(FUnrealMCPCommonUtils::FindAsset(TemplatePath));
    if (!Template)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("not_particle_system: '%s' is not a UParticleSystem asset"), *TemplatePath));
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(World, AActor::StaticClass(), AllActors);

    for (const AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Actor with name '%s' already exists"), *ActorName));
        }
    }

    // The attachment target is resolved before spawning: a mistyped parent must not leave a
    // half-built emitter behind in the level.
    AActor* AttachParent = nullptr;
    FString AttachTo;
    Params->TryGetStringField(TEXT("attach_to"), AttachTo);
    if (!AttachTo.IsEmpty())
    {
        for (AActor* Actor : AllActors)
        {
            if (Actor && Actor->GetName() == AttachTo)
            {
                AttachParent = Actor;
                break;
            }
        }
        if (!AttachParent)
        {
            TArray<TSharedPtr<FJsonValue>> Candidates;
            for (const AActor* Actor : AllActors)
            {
                if (Actor)
                {
                    Candidates.Add(MakeShared<FJsonValueString>(Actor->GetName()));
                }
            }
            TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("actor_not_found: no actor named '%s'"), *AttachTo));
            Error->SetArrayField(TEXT("candidates"), Candidates);
            return Error;
        }
    }

    const FVector Location = Params->HasField(TEXT("location"))
        ? FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location")) : FVector::ZeroVector;
    const FRotator Rotation = Params->HasField(TEXT("rotation"))
        ? FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation")) : FRotator::ZeroRotator;
    const FVector RelativeLocation = Params->HasField(TEXT("relative_location"))
        ? FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("relative_location")) : FVector::ZeroVector;
    const FRotator RelativeRotation = Params->HasField(TEXT("relative_rotation"))
        ? FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("relative_rotation")) : FRotator::ZeroRotator;

    bool bAutoActivate = true;
    Params->TryGetBoolField(TEXT("auto_activate"), bAutoActivate);

    FActorSpawnParameters SpawnParams;
    SpawnParams.Name = *ActorName;

    AEmitter* NewActor = World->SpawnActor<AEmitter>(AEmitter::StaticClass(), Location, Rotation, SpawnParams);
    if (!NewActor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to spawn an Emitter named '%s'"), *ActorName));
    }

    UParticleSystemComponent* Component = NewActor->GetParticleSystemComponent();
    if (!Component)
    {
        NewActor->Destroy();
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("The spawned Emitter has no ParticleSystemComponent"));
    }

    Component->SetTemplate(Template);
    NewActor->SetActorLabel(ActorName);

    if (AttachParent)
    {
        // KeepWorldTransform leaves the actor where it was spawned; the relative transform the
        // caller asked for is applied on top of that.
        NewActor->AttachToActor(AttachParent, FAttachmentTransformRules::KeepWorldTransform);
        NewActor->SetActorRelativeLocation(RelativeLocation);
        NewActor->SetActorRelativeRotation(RelativeRotation);
    }

    if (bAutoActivate)
    {
        Component->ActivateSystem(/*bFlagAsJustAttached=*/true);
    }
    else
    {
        Component->DeactivateSystem();
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("actor"), NewActor->GetName());
    ResultObj->SetStringField(TEXT("template"), Template->GetPathName());
    if (AttachParent)
    {
        ResultObj->SetStringField(TEXT("attached_to"), AttachParent->GetName());
    }
    else
    {
        ResultObj->SetField(TEXT("attached_to"), MakeShared<FJsonValueNull>());
    }

    const FVector AppliedRelativeLocation = NewActor->GetRootComponent()
        ? NewActor->GetRootComponent()->GetRelativeLocation() : FVector::ZeroVector;
    TArray<TSharedPtr<FJsonValue>> RelativeLocationJson;
    RelativeLocationJson.Add(MakeShared<FJsonValueNumber>(AppliedRelativeLocation.X));
    RelativeLocationJson.Add(MakeShared<FJsonValueNumber>(AppliedRelativeLocation.Y));
    RelativeLocationJson.Add(MakeShared<FJsonValueNumber>(AppliedRelativeLocation.Z));
    ResultObj->SetArrayField(TEXT("relative_location"), RelativeLocationJson);
    ResultObj->SetBoolField(TEXT("activated"), bAutoActivate);
    return ResultObj;
}

int32 FUnrealMCPEditorCommands::CloseAssetEditorsFor(UObject* Asset)
{
    UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
    if (!AssetEditorSubsystem || !Asset)
    {
        return 0;
    }

    // Closing an asset editor tab makes the engine record where that asset should
    // open next time (SStandaloneAssetEditorToolkitHost::OnTabClosed writes
    // [AssetEditorToolkitTabLocation] in GEditorPerProjectIni). MCP has to close
    // editors before editing graphs, so capture the record and put it back: an
    // MCP-driven close must not change where the user sees the asset afterwards.
    const TCHAR* const TabLocationSection = TEXT("AssetEditorToolkitTabLocation");
    const FString TabLocationKey = Asset->GetPathName();
    int32 PreviousLocation = INDEX_NONE;
    const bool bHadRecord = GConfig->GetInt(TabLocationSection, *TabLocationKey, PreviousLocation, GEditorPerProjectIni);

    const int32 ClosedEditors = AssetEditorSubsystem->CloseAllEditorsForAsset(Asset);
    if (ClosedEditors == 0)
    {
        // Nothing was closed, so the engine wrote nothing: leave the config alone.
        return 0;
    }

    if (bHadRecord)
    {
        GConfig->SetInt(TabLocationSection, *TabLocationKey, PreviousLocation, GEditorPerProjectIni);
    }
    else
    {
        GConfig->RemoveKey(TabLocationSection, *TabLocationKey, GEditorPerProjectIni);
    }
    GConfig->Flush(false, GEditorPerProjectIni);
    return ClosedEditors;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleCloseAssetEditors(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' parameter"));
    }

    UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Asset)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
    }

    UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
    if (!AssetEditorSubsystem)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("AssetEditorSubsystem is not available"));
    }

    // Reported state of the tab-location record (restored_value / key_removed below).
    const TCHAR* const TabLocationSection = TEXT("AssetEditorToolkitTabLocation");
    const FString TabLocationKey = Asset->GetPathName();
    int32 PreviousLocation = INDEX_NONE;
    const bool bHadRecord = GConfig->GetInt(TabLocationSection, *TabLocationKey, PreviousLocation, GEditorPerProjectIni);

    FString LocationBefore = TEXT("absent");
    if (bHadRecord)
    {
        LocationBefore = (PreviousLocation == static_cast<int32>(EAssetEditorToolkitTabLocation::Docked))
            ? TEXT("docked")
            : TEXT("standalone");
    }

    const int32 ClosedEditors = CloseAssetEditorsFor(Asset);
    if (ClosedEditors == 0)
    {
        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetBoolField(TEXT("success"), true);
        ResultObj->SetStringField(TEXT("asset_path"), AssetPath);
        ResultObj->SetStringField(TEXT("asset_kind"), Asset->GetClass()->GetName());
        ResultObj->SetNumberField(TEXT("closed_editors"), 0);
        ResultObj->SetStringField(TEXT("restored_value"), LocationBefore);
        ResultObj->SetBoolField(TEXT("key_removed"), false);
        ResultObj->SetBoolField(TEXT("configuration_changed"), false);
        return ResultObj;
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("asset_path"), AssetPath);
    ResultObj->SetStringField(TEXT("asset_kind"), Asset->GetClass()->GetName());
    ResultObj->SetNumberField(TEXT("closed_editors"), ClosedEditors);
    // restored_value is the state the key is left in; key_removed says whether a
    // key was actively deleted (no prior record); configuration_changed says the
    // ini was touched at all. Together they are decidable without guessing.
    ResultObj->SetStringField(TEXT("restored_value"), bHadRecord ? LocationBefore : TEXT("absent"));
    ResultObj->SetBoolField(TEXT("key_removed"), !bHadRecord);
    ResultObj->SetBoolField(TEXT("configuration_changed"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSetConsoleVariable(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    FString Value;
    if (!Params->TryGetStringField(TEXT("name"), Name) || !Params->TryGetStringField(TEXT("value"), Value))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' or 'value' parameter"));
    }

    IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*Name);
    if (!CVar)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Console variable not found: %s"), *Name));
    }

    CVar->Set(*Value, ECVF_SetByGameSetting);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("name"), Name);
    ResultObj->SetStringField(TEXT("value"), CVar->GetString());
    return ResultObj;
} 

//---------------------------------------------------------------------------------------------
// set_actor_location_safe / set_actor_custom_depth_safe
//
// Both used to be python-only tools in Content/Python/tools/ue_safe_api.py, which made them
// unreachable from editor-side scripts. Their reason to exist is that the plain calls are traps:
// UPrimitiveComponent::SetActorLocation needs the sweep argument spelled out in python, and the
// custom-depth properties are per-component (render_custom_depth / custom_depth_stencil_value, not
// "custom_stencil_value") on every primitive component of the actor.
//---------------------------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSetActorLocationSafe(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'name' parameter"));
    }
    if (!Params->HasField(TEXT("location")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'location' parameter"));
    }

    AActor* TargetActor = nullptr;
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName)
        {
            TargetActor = Actor;
            break;
        }
    }
    if (!TargetActor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("actor_not_found"),
            FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }

    const FVector NewLocation = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location"));
    bool bSweep = false;
    bool bTeleport = false;
    Params->TryGetBoolField(TEXT("sweep"), bSweep);
    Params->TryGetBoolField(TEXT("teleport"), bTeleport);

    // Sweep/teleport are the whole point of this command: set_actor_transform moves the actor
    // without touching collision or physics.
    const bool bMoved = TargetActor->SetActorLocation(NewLocation, bSweep, nullptr,
        bTeleport ? ETeleportType::TeleportPhysics : ETeleportType::None);

    TSharedPtr<FJsonObject> Result = FUnrealMCPCommonUtils::ActorToJsonObject(TargetActor, true);
    Result->SetBoolField(TEXT("moved"), bMoved);
    Result->SetBoolField(TEXT("sweep"), bSweep);
    Result->SetBoolField(TEXT("teleport"), bTeleport);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSetActorCustomDepthSafe(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("Missing 'name' parameter"));
    }

    bool bEnabled = true;
    Params->TryGetBoolField(TEXT("enabled"), bEnabled);
    int32 StencilValue = 1;
    if (Params->HasField(TEXT("stencil_value")))
    {
        StencilValue = static_cast<int32>(Params->GetNumberField(TEXT("stencil_value")));
    }

    AActor* TargetActor = nullptr;
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName)
        {
            TargetActor = Actor;
            break;
        }
    }
    if (!TargetActor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("actor_not_found"),
            FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }

    // Write both properties through the engine accessors (they mark the render state dirty) rather
    // than through reflection, so the stencil reaches the renderer without a second poke.
    TArray<UPrimitiveComponent*> Components;
    TargetActor->GetComponents<UPrimitiveComponent>(Components);
    for (UPrimitiveComponent* Component : Components)
    {
        if (!Component)
        {
            continue;
        }
        Component->SetRenderCustomDepth(bEnabled);
        Component->SetCustomDepthStencilValue(StencilValue);
    }

    TSharedPtr<FJsonObject> Result = FUnrealMCPCommonUtils::ActorToJsonObject(TargetActor, true);
    Result->SetNumberField(TEXT("component_count"), Components.Num());
    Result->SetBoolField(TEXT("enabled"), bEnabled);
    Result->SetNumberField(TEXT("stencil_value"), StencilValue);
    return Result;
}
