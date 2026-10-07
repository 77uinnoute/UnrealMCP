#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Http.h"
#include "Json.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Commands/UnrealMCPEditorCommands.h"
#include "Commands/Blueprint/UnrealMCPBlueprintCommands.h"
#include "Commands/Blueprint/UnrealMCPBlueprintNodeCommands.h"
#include "Commands/UnrealMCPProjectCommands.h"
#include "Commands/UnrealMCPUMGCommands.h"
#include "Commands/Material/UnrealMCPMaterialCommands.h"
#include "Commands/Asset/UnrealMCPAssetCommands.h"
#include "Commands/Asset/UnrealMCPAssetEditCommands.h"
#include "Commands/Particle/UnrealMCPParticleCommands.h"
#include "Commands/Animation/UnrealMCPAnimationCommands.h"
#include "Commands/PCG/UnrealMCPPCGCommands.h"
#include "Commands/Clothing/UnrealMCPClothCommands.h"
#include "Commands/Physics/UnrealMCPPhysicsAssetCommands.h"
#include "Commands/UnrealMCPReflectionCommands.h"
#include "Commands/UnrealMCPPythonAPICommands.h"
#include "Commands/UnrealMCPLiveCodingCommands.h"
#include "Commands/UnrealMCPPIECommands.h"
#include "UnrealMCPBridge.generated.h"

class FMCPServerRunnable;

/**
 * Editor subsystem for MCP Bridge
 * Handles communication between external tools and the Unreal Editor
 * through a TCP socket connection. Commands are received as JSON and
 * routed to appropriate command handlers.
 */
UCLASS()
class UNREALMCP_API UUnrealMCPBridge : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	UUnrealMCPBridge();
	virtual ~UUnrealMCPBridge();

	// UEditorSubsystem implementation
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// Server functions
	void StartServer();
	void StopServer();
	bool IsRunning() const { return bIsRunning; }

    // Command execution
    FString ExecuteCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

    // Synchronous GameThread dispatch shared by the TCP path and the editor
    // python loopback entry (UUnrealMCPPythonAPI). No queuing, no future wait.
    TSharedPtr<FJsonObject> DispatchCommandDirect(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    // Bridge-local command handlers (registered in Initialize, see the command registry).
    TSharedPtr<FJsonObject> HandleExecutePythonCommand(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleExecutePythonFile(const TSharedPtr<FJsonObject>& Params);

	// Server state
	bool bIsRunning;
	TSharedPtr<FSocket> ListenerSocket;
	TSharedPtr<FSocket> ConnectionSocket;
	FRunnableThread* ServerThread;

	// Server configuration
	FIPv4Address ServerAddress;
	uint16 Port;

    // Command handler instances
    TSharedPtr<FUnrealMCPEditorCommands> EditorCommands;
    TSharedPtr<FUnrealMCPBlueprintCommands> BlueprintCommands;
    TSharedPtr<FUnrealMCPBlueprintNodeCommands> BlueprintNodeCommands;
    TSharedPtr<FUnrealMCPProjectCommands> ProjectCommands;
    TSharedPtr<FUnrealMCPUMGCommands> UMGCommands;
    TSharedPtr<FUnrealMCPMaterialCommands> MaterialCommands;
    TSharedPtr<FUnrealMCPAssetCommands> AssetCommands;
    TSharedPtr<FUnrealMCPAssetEditCommands> AssetEditCommands;
    TSharedPtr<FUnrealMCPParticleCommands> ParticleCommands;
    TSharedPtr<FUnrealMCPAnimationCommands> AnimationCommands;
    TSharedPtr<FUnrealMCPPCGCommands> PCGCommands;
    TSharedPtr<FUnrealMCPReflectionCommands> ReflectionCommands;
    TSharedPtr<FUnrealMCPClothCommands> ClothCommands;
    TSharedPtr<FUnrealMCPPhysicsAssetCommands> PhysicsAssetCommands;
    TSharedPtr<FUnrealMCPPythonAPICommands> PythonAPICommands;
    TSharedPtr<FUnrealMCPPIECommands> PIECommands;
    TSharedPtr<FUnrealMCPLiveCodingCommands> LiveCodingCommands;
}; 