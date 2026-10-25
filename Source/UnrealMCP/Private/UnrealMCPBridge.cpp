#include "UnrealMCPBridge.h"
#include "MCPServerRunnable.h"
#include "Core/MCPCommandRegistry.h"
#include "Core/MCPGCSafety.h"
#include "Core/PythonCodeGuard.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "HAL/RunnableThread.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Camera/CameraActor.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "JsonObjectConverter.h"
#include "GameFramework/Actor.h"
#include "Engine/Selection.h"
#include "Kismet/GameplayStatics.h"
#include "Async/Async.h"
#include "Misc/Timespan.h"
// Add Blueprint related includes
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Factories/BlueprintFactory.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
// UE5.5 correct includes
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "UObject/Field.h"
#include "UObject/FieldPath.h"
// Blueprint Graph specific includes
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_CallFunction.h"
#include "K2Node_InputAction.h"
#include "K2Node_Self.h"
#include "GameFramework/InputSettings.h"
#include "EditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"
// Include our new command handler classes
#include "Commands/UnrealMCPEditorCommands.h"
#include "Commands/Blueprint/UnrealMCPBlueprintCommands.h"
#include "Commands/Blueprint/UnrealMCPBlueprintNodeCommands.h"
#include "Commands/UnrealMCPProjectCommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Commands/UnrealMCPUMGCommands.h"
#include "Commands/Material/UnrealMCPMaterialCommands.h"
#include "Commands/Asset/UnrealMCPAssetCommands.h"
#include "Commands/Asset/UnrealMCPAssetEditCommands.h"
#include "Commands/Particle/UnrealMCPParticleCommands.h"
#include "Commands/Niagara/UnrealMCPNiagaraCommands.h"
#include "Commands/PCG/UnrealMCPPCGCommands.h"
#include "Commands/Animation/UnrealMCPAnimationCommands.h"
#include "Commands/Clothing/UnrealMCPClothCommands.h"
// Python execution support
#include "IPythonScriptPlugin.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
// Material conversion support
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Templates/Function.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionCustom.h"
#include "MaterialEditingLibrary.h"
// Material compile error retrieval (FMaterialResource / FMaterial::GetCompileErrors)
#include "MaterialShared.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Base64.h"
#include "Misc/App.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"

// Default settings
#define MCP_SERVER_HOST "127.0.0.1"
#define MCP_SERVER_PORT 55557

// Registered by Initialize() but defined further down this file.
static TSharedPtr<FJsonObject> HandleExecutePythonCommand(const TSharedPtr<FJsonObject>& Params);
static TSharedPtr<FJsonObject> HandleListMCPCommands(const TSharedPtr<FJsonObject>& Params);

UUnrealMCPBridge::UUnrealMCPBridge()
{
    EditorCommands = MakeShared<FUnrealMCPEditorCommands>();
    BlueprintCommands = MakeShared<FUnrealMCPBlueprintCommands>();
    BlueprintNodeCommands = MakeShared<FUnrealMCPBlueprintNodeCommands>();
    ProjectCommands = MakeShared<FUnrealMCPProjectCommands>();
    UMGCommands = MakeShared<FUnrealMCPUMGCommands>();
    MaterialCommands = MakeShared<FUnrealMCPMaterialCommands>();
    AssetCommands = MakeShared<FUnrealMCPAssetCommands>();
    AssetEditCommands = MakeShared<FUnrealMCPAssetEditCommands>();
    ParticleCommands = MakeShared<FUnrealMCPParticleCommands>();
    NiagaraCommands = MakeShared<FUnrealMCPNiagaraCommands>();
    AnimationCommands = MakeShared<FUnrealMCPAnimationCommands>();
    PCGCommands = MakeShared<FUnrealMCPPCGCommands>();
    ReflectionCommands = MakeShared<FUnrealMCPReflectionCommands>();
    ClothCommands = MakeShared<FUnrealMCPClothCommands>();
    PhysicsAssetCommands = MakeShared<FUnrealMCPPhysicsAssetCommands>();
    PythonAPICommands = MakeShared<FUnrealMCPPythonAPICommands>();
    PIECommands = MakeShared<FUnrealMCPPIECommands>();
    LiveCodingCommands = MakeShared<FUnrealMCPLiveCodingCommands>();
}

UUnrealMCPBridge::~UUnrealMCPBridge()
{
    EditorCommands.Reset();
    BlueprintCommands.Reset();
    BlueprintNodeCommands.Reset();
    ProjectCommands.Reset();
    UMGCommands.Reset();
    MaterialCommands.Reset();
    AssetCommands.Reset();
    AssetEditCommands.Reset();
    ParticleCommands.Reset();
    NiagaraCommands.Reset();
    AnimationCommands.Reset();
    PCGCommands.Reset();
    PIECommands.Reset();
    LiveCodingCommands.Reset();
}

// Initialize subsystem
void UUnrealMCPBridge::Initialize(FSubsystemCollectionBase& Collection)
{
    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Initializing"));
    
    bIsRunning = false;
    ListenerSocket = nullptr;
    ConnectionSocket = nullptr;
    ServerThread = nullptr;
    Port = MCP_SERVER_PORT;
    FIPv4Address::Parse(MCP_SERVER_HOST, ServerAddress);

    // Rebuild the command table: handlers bind to the command objects created in the constructor,
    // so a re-created subsystem must not keep dispatch entries pointing at the old ones.
    FMCPCommandRegistry& Registry = FMCPCommandRegistry::Get();
    Registry.Reset();

    // Bridge-local commands
    Registry.Register({
        TEXT("ping"),
        TEXT("mcp"),
        TEXT("Liveness probe. Answers with pong."),
        {},
        MCPFlags(false, false, true),
        [](const TSharedPtr<FJsonObject>&) -> TSharedPtr<FJsonObject>
        {
            TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
            ResultJson->SetStringField(TEXT("message"), TEXT("pong"));
            return ResultJson;
        }});

    FMCPCommandFlags PythonCommandFlags = MCPFlags(true);
    Registry.Register({
        TEXT("execute_python_command"),
        TEXT("mcp"),
        TEXT("Run python in the editor (synchronous; the call must finish within the timeout)."),
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("command"), TEXT("string"), TEXT("Python source to execute")),
        }),
        PythonCommandFlags,
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleExecutePythonCommand(Params); }});

    FMCPCommandFlags PythonFileFlags = MCPFlags(true);
    Registry.Register({
        TEXT("execute_python_file"),
        TEXT("mcp"),
        TEXT("Run a local .py file in the editor (synchronous; the call must finish within the timeout)."),
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("file_path"), TEXT("string"), TEXT("Absolute path to a .py file on this machine")),
        }),
        PythonFileFlags,
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleExecutePythonFile(Params); }});

    Registry.Register({
        TEXT("list_mcp_commands"),
        TEXT("mcp"),
        TEXT("List the registered commands with their parameters and policy flags."),
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("category"), TEXT("string"), TEXT("Only list this category")),
        }),
        MCPFlags(),
        [](const TSharedPtr<FJsonObject>& Params) { return HandleListMCPCommands(Params); }});

    // Domain commands: each domain owns its own registrations (see its RegisterCommands()).
    EditorCommands->RegisterCommands(Registry);
    BlueprintCommands->RegisterCommands(Registry);
    BlueprintNodeCommands->RegisterCommands(Registry);
    ProjectCommands->RegisterCommands(Registry);
    UMGCommands->RegisterCommands(Registry);
    MaterialCommands->RegisterCommands(Registry);
    AssetCommands->RegisterCommands(Registry);
    AssetEditCommands->RegisterCommands(Registry);
    ParticleCommands->RegisterCommands(Registry);
    NiagaraCommands->RegisterCommands(Registry);
    AnimationCommands->RegisterCommands(Registry);
    PCGCommands->RegisterCommands(Registry);
    ReflectionCommands->RegisterCommands(Registry);
    ClothCommands->RegisterCommands(Registry);
    PhysicsAssetCommands->RegisterCommands(Registry);
    PythonAPICommands->RegisterCommands(Registry);
    PIECommands->RegisterCommands(Registry);
    LiveCodingCommands->RegisterCommands(Registry);

    Registry.Seal();

    // Start the server automatically
    StartServer();
}

// Clean up resources when subsystem is destroyed
void UUnrealMCPBridge::Deinitialize()
{
    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Shutting down"));
    StopServer();
}

// Start the MCP server
void UUnrealMCPBridge::StartServer()
{
    if (bIsRunning)
    {
        UE_LOG(LogTemp, Warning, TEXT("UnrealMCPBridge: Server is already running"));
        return;
    }

    // Create socket subsystem
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!SocketSubsystem)
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to get socket subsystem"));
        return;
    }

    // Create listener socket
    TSharedPtr<FSocket> NewListenerSocket = MakeShareable(SocketSubsystem->CreateSocket(NAME_Stream, TEXT("UnrealMCPListener"), false));
    if (!NewListenerSocket.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to create listener socket"));
        return;
    }

    // Allow address reuse for quick restarts
    NewListenerSocket->SetReuseAddr(true);
    NewListenerSocket->SetNonBlocking(true);

    // Bind to address
    FIPv4Endpoint Endpoint(ServerAddress, Port);
    if (!NewListenerSocket->Bind(*Endpoint.ToInternetAddr()))
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to bind listener socket to %s:%d"), *ServerAddress.ToString(), Port);
        return;
    }

    // Start listening
    if (!NewListenerSocket->Listen(5))
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to start listening"));
        return;
    }

    ListenerSocket = NewListenerSocket;
    bIsRunning = true;
    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Server started on %s:%d"), *ServerAddress.ToString(), Port);

    // Start server thread
    ServerThread = FRunnableThread::Create(
        new FMCPServerRunnable(this, ListenerSocket),
        TEXT("UnrealMCPServerThread"),
        0, TPri_Normal
    );

    if (!ServerThread)
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to create server thread"));
        StopServer();
        return;
    }
}

// Stop the MCP server
void UUnrealMCPBridge::StopServer()
{
    if (!bIsRunning)
    {
        return;
    }

    bIsRunning = false;

    // Clean up thread
    if (ServerThread)
    {
        ServerThread->Kill(true);
        delete ServerThread;
        ServerThread = nullptr;
    }

    // Close sockets
    if (ConnectionSocket.IsValid())
    {
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ConnectionSocket.Get());
        ConnectionSocket.Reset();
    }

    if (ListenerSocket.IsValid())
    {
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ListenerSocket.Get());
        ListenerSocket.Reset();
    }

    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Server stopped"));
}

// ----------------------------------------------------------------------------
// Python 执行辅助
// ----------------------------------------------------------------------------

// 回环 fail-fast 守卫的源码，从插件 Content/Python/tools/ 读一次并缓存。
// 守卫本体写成真 .py 文件（可读、可单独测、自己写明两个逃生口），这里只负责把它内联进来；
// 文件缺失就退化成"没有守卫"并告警，绝不因为守卫装不上而让 python 执行不了。
static const FString& LoopbackGuardSource()
{
    static FString Cached;
    static bool bLoaded = false;
    if (!bLoaded)
    {
        bLoaded = true;

        TArray<FString> Candidates;
        if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealMCP")))
        {
            Candidates.Add(Plugin->GetBaseDir() / TEXT("Content/Python/tools/unreal_mcp_loopback.py"));
        }
        Candidates.Add(FPaths::ProjectPluginsDir() / TEXT("UnrealMCP/Content/Python/tools/unreal_mcp_loopback.py"));
        Candidates.Add(FPaths::EnginePluginsDir() / TEXT("UnrealMCP/Content/Python/tools/unreal_mcp_loopback.py"));

        for (const FString& Candidate : Candidates)
        {
            if (FFileHelper::LoadFileToString(Cached, *Candidate) && !Cached.IsEmpty())
            {
                return Cached;
            }
        }
        Cached.Reset();
        UE_LOG(LogTemp, Warning, TEXT("UnrealMCPBridge: loopback guard source not found (%s); editor python will NOT stop at the first failed command"),
            *FString::Join(Candidates, TEXT(", ")));
    }
    return Cached;
}

// 把守卫包成一条不含 ".py" 的前置语句（理由同 WrapPythonSource 的注释），装成
// sys.modules["unreal_mcp_loopback"] 并立刻 install()。用独立模块字典 exec，所以不污染脚本的全局名。
static FString LoopbackGuardPrelude()
{
    const FString& Source = LoopbackGuardSource();
    if (Source.IsEmpty())
    {
        return FString();
    }
    const FTCHARToUTF8 SourceUtf8(*Source);
    const FString SourceB64 = FBase64::Encode(reinterpret_cast<const uint8*>(SourceUtf8.Get()), (uint32)SourceUtf8.Length());
    return FString::Printf(TEXT(
        "import base64 as _mcp_b64, sys as _mcp_sys, types as _mcp_types; "
        "_mcp_g = _mcp_types.ModuleType('unreal_mcp_loopback'); "
        "_mcp_g.__dict__['__builtins__'] = __builtins__; "
        "exec(compile(_mcp_b64.b64decode('%s').decode('utf-8'), '<unreal_mcp_loopback>', 'exec'), _mcp_g.__dict__); "
        "_mcp_sys.modules['unreal_mcp_loopback'] = _mcp_g; "
        "_mcp_g.install()"), *SourceB64);
}

// 引擎在 ExecuteFile 模式下会在代码文本里找第一个 ".py"，其后是空白/结尾就把前缀当文件路径
// 去 RunFile（PythonScriptPlugin.cpp:677-763）——代码里写了 "xxx.py" 的脚本会被当成路径执行失败。
// 源码与文件名都经 base64 包装成一行 exec(compile(...))：base64 字母表没有 '.'，整行不含 ".py"，
// 必走 RunString；compile 的文件名让 traceback 指向真实文件与原始行号。
static FString WrapPythonSource(const FString& Source, const FString& VirtualFilename)
{
    const FTCHARToUTF8 SourceUtf8(*Source);
    const FTCHARToUTF8 NameUtf8(*VirtualFilename);
    const FString SourceB64 = FBase64::Encode(reinterpret_cast<const uint8*>(SourceUtf8.Get()), (uint32)SourceUtf8.Length());
    const FString NameB64 = FBase64::Encode(reinterpret_cast<const uint8*>(NameUtf8.Get()), (uint32)NameUtf8.Length());
    const FString Body = FString::Printf(
        TEXT("import base64 as _mcp_b64; exec(compile(_mcp_b64.b64decode('%s').decode('utf-8'), _mcp_b64.b64decode('%s').decode('utf-8'), 'exec'))"),
        *SourceB64, *NameB64);

    const FString Prelude = LoopbackGuardPrelude();
    return Prelude.IsEmpty() ? Body : Prelude + TEXT("; ") + Body;
}

// 在 GameThread 上同步执行一段 Python 代码，整理成 ResultJson。
// 字段结构与 execute_python_command 的同步响应一致：
//   success / result / log / error
static TSharedPtr<FJsonObject> ExecutePythonCode(const FString& PyCode, const FString& VirtualFilename)
{
    // Wall-clock timing for the bridge-side handler. Reported back as duration_ms / python_ms:
    // without them the reply only says whether the call finished, never how long it took, and the
    // client has no way to tell a 200 ms call from a 70 s one.
    const double HandlerStartSeconds = FPlatformTime::Seconds();

    IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
    if (!(PythonPlugin && PythonPlugin->IsPythonAvailable()))
    {
        TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
        ResultJson->SetBoolField(TEXT("success"), false);
        ResultJson->SetStringField(TEXT("error"), TEXT("Python is not available in this editor"));
        return ResultJson;
    }

    // Nothing runs until the source passes the banned-API guard: a rejected script must not have
    // half-executed, and the caller needs the offending lines rather than a traceback.
    TArray<FPythonGuardMatch> GuardMatches;
    if (!FPythonCodeGuard::Check(PyCode, GuardMatches))
    {
        TSharedPtr<FJsonObject> GuardJson = MakeShareable(new FJsonObject);
        GuardJson->SetBoolField(TEXT("success"), false);
        GuardJson->SetStringField(TEXT("error"), TEXT("banned_api"));
        GuardJson->SetBoolField(TEXT("pie_running"), FPythonCodeGuard::IsPlaying());

        TArray<TSharedPtr<FJsonValue>> MatchesJson;
        int32 BlockingCount = 0;
        for (const FPythonGuardMatch& Match : GuardMatches)
        {
            TSharedPtr<FJsonObject> MatchJson = MakeShareable(new FJsonObject);
            MatchJson->SetNumberField(TEXT("line"), Match.Line);
            MatchJson->SetStringField(TEXT("api"), Match.Api);
            MatchJson->SetStringField(TEXT("reason"), Match.Reason);
            MatchJson->SetStringField(TEXT("use_instead"), Match.UseInstead);
            MatchJson->SetBoolField(TEXT("blocks"), Match.bBlocks);
            MatchesJson.Add(MakeShareable(new FJsonValueObject(MatchJson)));

            if (Match.bBlocks)
            {
                ++BlockingCount;
            }
        }
        GuardJson->SetArrayField(TEXT("matches"), MatchesJson);

        GuardJson->SetStringField(TEXT("message"), FString::Printf(
            TEXT("%d blocking API call(s) found and NOTHING was executed. Each match names the line, why it "
                 "is refused and what to use instead%s."),
            BlockingCount,
            MatchesJson.Num() > BlockingCount
                ? TEXT("; entries with blocks=false are warnings that would have run")
                : TEXT("")));
        GuardJson->SetNumberField(TEXT("duration_ms"),
            (FPlatformTime::Seconds() - HandlerStartSeconds) * 1000.0);
        return GuardJson;
    }

    FPythonCommandEx PyCommandEx;
    PyCommandEx.Command = WrapPythonSource(PyCode, VirtualFilename);
    PyCommandEx.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;

    // Commands the script reaches over the loopback run inside this scope, so the GC sites in the
    // plugin can skip a forced collection instead of hitting MarkAsGarbage()'s check(!IsRooted())
    // and killing the editor (see Core/MCPGCSafety.h).
    bool bExecSuccess = false;
    const double ExecStartSeconds = FPlatformTime::Seconds();
    {
        UnrealMCP::FMCPPythonExecutionScope PythonScope;
        bExecSuccess = PythonPlugin->ExecPythonCommandEx(PyCommandEx);
    }
    const double PythonMs = (FPlatformTime::Seconds() - ExecStartSeconds) * 1000.0;

    TArray<TSharedPtr<FJsonValue>> LogArray;
    // Collect any Python error output so the actual traceback reaches
    // the client instead of being silently dropped. Error entries are
    // also the authoritative failure signal: ExecPythonCommandEx can
    // report success even when the command raised an exception.
    TArray<FString> ErrorLines;
    for (const FPythonLogOutputEntry& Entry : PyCommandEx.LogOutput)
    {
        TSharedPtr<FJsonObject> PythonLogObj = MakeShareable(new FJsonObject);
        PythonLogObj->SetStringField(TEXT("type"), LexToString(Entry.Type));
        PythonLogObj->SetStringField(TEXT("output"), Entry.Output);
        LogArray.Add(MakeShareable(new FJsonValueObject(PythonLogObj)));

        if (Entry.Type == EPythonLogOutputType::Error)
        {
            ErrorLines.Add(Entry.Output);
        }
    }

    // A command is only successful when the engine reports success AND
    // no Error-level log output was produced (the traceback).
    const bool bCommandSuccess = bExecSuccess && ErrorLines.Num() == 0;

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), bCommandSuccess);
    ResultJson->SetStringField(TEXT("result"), PyCommandEx.CommandResult);
    ResultJson->SetArrayField(TEXT("log"), LogArray);
    // duration_ms = whole bridge-side handler (banned-API scan + python exec + log collection + JSON);
    // python_ms   = the engine's python call alone. Both exclude the TCP round trip and the client
    // tool wrapper, which the bridge cannot see. Same field name as live_coding_compile's duration_ms.
    ResultJson->SetNumberField(TEXT("duration_ms"), (FPlatformTime::Seconds() - HandlerStartSeconds) * 1000.0);
    ResultJson->SetNumberField(TEXT("python_ms"), PythonMs);

    // On failure, expose the traceback directly in the top-level 'error'
    // field so clients never have to dig through the log entries.
    if (!bCommandSuccess)
    {
        FString CombinedError;
        for (FString Line : ErrorLines)
        {
            Line.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
            CombinedError += Line;
            if (!CombinedError.EndsWith(TEXT("\n")))
            {
                CombinedError += TEXT("\n");
            }
        }
        // Unhandled exceptions surface the traceback via CommandResult
        // instead of LogOutput — use it when no Error lines were captured.
        CombinedError.TrimStartAndEndInline();
        if (CombinedError.IsEmpty() && !PyCommandEx.CommandResult.IsEmpty())
        {
            CombinedError = PyCommandEx.CommandResult;
            CombinedError.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
        }
        if (CombinedError.IsEmpty() || CombinedError == TEXT("None"))
        {
            CombinedError = TEXT("Python command failed (see log output).");
        }
        ResultJson->SetStringField(TEXT("error"), CombinedError);
    }

    // Non-blocking findings from the banned-API scan. The source ran, but it stepped around a path the
    // plugin keeps a safer one for - attached here so a caller cannot miss it, and kept separate from
    // 'log' so it survives even when the python produced no output of its own.
    if (GuardMatches.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> WarningJson;
        for (const FPythonGuardMatch& Match : GuardMatches)
        {
            TSharedPtr<FJsonObject> Warning = MakeShareable(new FJsonObject);
            Warning->SetNumberField(TEXT("line"), Match.Line);
            Warning->SetStringField(TEXT("api"), Match.Api);
            Warning->SetStringField(TEXT("reason"), Match.Reason);
            Warning->SetStringField(TEXT("use_instead"), Match.UseInstead);
            WarningJson.Add(MakeShareable(new FJsonValueObject(Warning)));
        }
        ResultJson->SetArrayField(TEXT("guard_warnings"), WarningJson);
    }
    return ResultJson;
}

// Bridge-local command bodies. They live here (rather than inline in the dispatch chain) because the
// registry binds handlers, and the chain is being retired.
TSharedPtr<FJsonObject> UUnrealMCPBridge::HandleExecutePythonCommand(const TSharedPtr<FJsonObject>& Params)
{
    FString PyCommand;
    if (!Params->TryGetStringField(TEXT("command"), PyCommand))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'command' parameter"));
    }
    return ExecutePythonCode(PyCommand, TEXT("<mcp_command>"));
}

TSharedPtr<FJsonObject> UUnrealMCPBridge::HandleExecutePythonFile(const TSharedPtr<FJsonObject>& Params)
{
    FString FilePath;
    if (!Params->TryGetStringField(TEXT("file_path"), FilePath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'file_path' parameter"));
    }
    if (!FPaths::GetExtension(FilePath).Equals(TEXT("py"), ESearchCase::IgnoreCase))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'file_path' must end with .py"));
    }

    FString PyCommand;
    if (!FFileHelper::LoadFileToString(PyCommand, *FilePath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to read file: %s"), *FilePath));
    }

    TSharedPtr<FJsonObject> ResultJson = ExecutePythonCode(PyCommand, FilePath);
    ResultJson->SetStringField(TEXT("file_path"), FilePath);
    ResultJson->SetNumberField(TEXT("code_bytes"), (double)FTCHARToUTF8(*PyCommand).Length());
    return ResultJson;
}

// Introspection: report the registered command surface (name, category, params, policy flags).
// Read-only by construction - it reads the registry and touches no asset, object or graph.
static TSharedPtr<FJsonObject> HandleListMCPCommands(const TSharedPtr<FJsonObject>& Params)
{
    FString CategoryFilter;
    const bool bFiltered = Params.IsValid() && Params->TryGetStringField(TEXT("category"), CategoryFilter);

    const TArray<FMCPCommandEntry>& All = FMCPCommandRegistry::Get().All();
    TArray<TSharedPtr<FJsonValue>> CommandsJson;
    for (const FMCPCommandEntry& Entry : All)
    {
        if (bFiltered && Entry.Category != CategoryFilter)
        {
            continue;
        }

        TArray<TSharedPtr<FJsonValue>> ParamsJson;
        for (const FMCPParamSpec& Spec : Entry.Params)
        {
            TSharedPtr<FJsonObject> ParamJson = MakeShareable(new FJsonObject);
            ParamJson->SetStringField(TEXT("name"), Spec.Name);
            ParamJson->SetStringField(TEXT("type"), Spec.Type);
            ParamJson->SetBoolField(TEXT("required"), Spec.bRequired);
            if (!Spec.Default.IsEmpty())
            {
                ParamJson->SetStringField(TEXT("default"), Spec.Default);
            }
            if (Spec.AllowedValues.Num() > 0)
            {
                TArray<TSharedPtr<FJsonValue>> ValuesJson;
                for (const FString& Value : Spec.AllowedValues)
                {
                    ValuesJson.Add(MakeShared<FJsonValueString>(Value));
                }
                ParamJson->SetArrayField(TEXT("allowed_values"), ValuesJson);
            }
            if (!Spec.Description.IsEmpty())
            {
                ParamJson->SetStringField(TEXT("description"), Spec.Description);
            }
            ParamsJson.Add(MakeShared<FJsonValueObject>(ParamJson));
        }

        TSharedPtr<FJsonObject> FlagsJson = MakeShareable(new FJsonObject);
        FlagsJson->SetBoolField(TEXT("loopback_forbidden"), Entry.Flags.bLoopbackForbidden);
        FlagsJson->SetBoolField(TEXT("mutates_graph"), Entry.Flags.bMutatesGraph);
        FlagsJson->SetBoolField(TEXT("hidden"), Entry.Flags.bHidden);
        FlagsJson->SetBoolField(TEXT("persist_after_success"), Entry.Flags.bPersistAfterSuccess);

        TSharedPtr<FJsonObject> CommandJson = MakeShareable(new FJsonObject);
        CommandJson->SetStringField(TEXT("name"), Entry.Name);
        CommandJson->SetStringField(TEXT("category"), Entry.Category);
        CommandJson->SetStringField(TEXT("description"), Entry.Description);
        CommandJson->SetArrayField(TEXT("params"), ParamsJson);
        CommandJson->SetObjectField(TEXT("flags"), FlagsJson);
        CommandsJson.Add(MakeShared<FJsonValueObject>(CommandJson));
    }

    TSharedPtr<FJsonObject> ResultJson = MakeShareable(new FJsonObject);
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetNumberField(TEXT("command_count"), CommandsJson.Num());
    if (bFiltered)
    {
        ResultJson->SetStringField(TEXT("category"), CategoryFilter);
    }
    ResultJson->SetArrayField(TEXT("commands"), CommandsJson);
    return ResultJson;
}

// Execute a command received from a client
FString UUnrealMCPBridge::ExecuteCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Executing command: %s"), *CommandType);

    // Create a promise to wait for the result
    TPromise<FString> Promise;
    TFuture<FString> Future = Promise.GetFuture();
    
    // Queue execution on Game Thread. The actual routing lives in
    // DispatchCommandDirect so the editor python loopback entry can call the
    // exact same handler synchronously (no duplicated dispatch logic).
    AsyncTask(ENamedThreads::GameThread, [this, CommandType, Params, Promise = MoveTemp(Promise)]() mutable
    {
        TSharedPtr<FJsonObject> ResponseJson = DispatchCommandDirect(CommandType, Params);

        FString ResultString;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
        FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);
        Promise.SetValue(ResultString);
    });

    // Wait with a timeout so a heavy command blocking the GameThread cannot
    // leave the MCP server thread (and thus the client) waiting forever.
    // Default is 120s; a request may override it via params.timeout_ms
    // (clamped to [5000, 600000] ms so callers cannot disable the timeout
    // entirely or freeze the server thread for hours).
    int32 CommandTimeoutMs = 120000;
    double TimeoutMsParam = 0.0;
    if (Params.IsValid() && Params->TryGetNumberField(TEXT("timeout_ms"), TimeoutMsParam))
    {
        CommandTimeoutMs = FMath::Clamp(FMath::RoundToInt32(TimeoutMsParam), 5000, 600000);
    }
    if (!Future.WaitFor(FTimespan::FromMilliseconds(CommandTimeoutMs)))
    {
        TSharedPtr<FJsonObject> ErrorJson = MakeShareable(new FJsonObject);
        ErrorJson->SetStringField(TEXT("status"), TEXT("error"));
        ErrorJson->SetStringField(TEXT("error"), FString::Printf(
            TEXT("GameThread did not respond in time (%d ms). Command aborted or still running."), CommandTimeoutMs));
        FString ErrorString;
        TSharedRef<TJsonWriter<>> ErrWriter = TJsonWriterFactory<>::Create(&ErrorString);
        FJsonSerializer::Serialize(ErrorJson.ToSharedRef(), ErrWriter);
        return ErrorString;
    }

    return Future.Get();
}

// Route a command to its handler. GameThread-only, fully synchronous: no
// queuing, no future waiting. Shared by the TCP path (ExecuteCommand's
// AsyncTask lambda) and the editor python loopback entry
// (UUnrealMCPPythonAPI::ExecuteMCPCommand). Do not duplicate this logic.
TSharedPtr<FJsonObject> UUnrealMCPBridge::DispatchCommandDirect(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject);

    try
    {
        // Registered commands dispatch straight from the process-wide table; the registry
        // turns an unknown name into the structured "unknown_command" error itself, so the
        // legacy chain of name comparisons (and its final else) is gone.
        TSharedPtr<FJsonObject> ResultJson = FMCPCommandRegistry::Get().Execute(CommandType, Params);

        // A handler that claims a command type but produced no result must not be read as a response.
        if (!ResultJson.IsValid())
        {
            ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
            ResponseJson->SetStringField(TEXT("error_code"), TEXT("unknown_command"));
            ResponseJson->SetObjectField(TEXT("result"),
                FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unknown_command"),
                    FString::Printf(TEXT("Command '%s' is routed but has no handler branch"), *CommandType)));
            return ResponseJson;
        }

        // Check if the result contains an error
        bool bSuccess = true;
        FString ErrorMessage;

        if (ResultJson->HasField(TEXT("success")))
        {
            bSuccess = ResultJson->GetBoolField(TEXT("success"));
            if (!bSuccess && ResultJson->HasField(TEXT("error")))
            {
                ErrorMessage = ResultJson->GetStringField(TEXT("error"));
            }
        }

        if (bSuccess)
        {
            // Set success status and include the result
            ResponseJson->SetStringField(TEXT("status"), TEXT("success"));
            ResponseJson->SetObjectField(TEXT("result"), ResultJson);
        }
        else
        {
            // Set error status and include the error message. Keep the result object
            // (with log/result fields) so clients get the full context in one response.
            ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
            ResponseJson->SetStringField(TEXT("error"), ErrorMessage);
            ResponseJson->SetObjectField(TEXT("result"), ResultJson);
        }
    }
    catch (const std::exception& e)
    {
        ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
        ResponseJson->SetStringField(TEXT("error"), UTF8_TO_TCHAR(e.what()));
    }

    return ResponseJson;
}