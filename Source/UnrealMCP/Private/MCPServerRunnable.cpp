// ============================================================================
// MCP 服务端线程（TCP）
//
// 整体流程：
//   1. 外层循环：轮询 ListenerSocket 是否有待接入的客户端（每 0.1s 一次）。
//   2. 接入后设为非阻塞模式，进入内层循环收数据。
//      引擎陷阱（SocketsBSD.cpp FSocketBSD::Recv）：非阻塞「暂无数据」被翻译成
//      return true 且 BytesRead==0，与直觉相反；对端 FIN 反而返回 false。因此
//      接收循环必须先用 Wait(WaitForRead) 等到可读再 Recv，绝不能拿「无数据」
//      当断连（否则每命令断连重连、首帧未到即关连接，客户端报 WinError 10053）。
//   3. 内层循环：Recv 到的字节累加进 ReceiveBuffer，按帧协议分帧：
//      - 新协议（首字节 0x00）：4 字节大端长度前缀 + JSON payload，按长度精确
//        收满再解析，杜绝 TCP 分片导致的中段丢字节；
//      - 旧协议（首字节 '{'）：整个缓冲当作一条 JSON 尝试解析（向后兼容）。
//      每条命令的响应会注入 bytes_received 字段供客户端做完整性校验。
//   4. 一命令一连接：响应发出后服务器主动关闭本连接，回到外层 accept 下一个。
//      客户端（python MCP server）每条命令建立全新连接、取完响应即关闭。
//      语义：单客户端独占消失——任何连接最多被服务一条命令，谁卡死谁自己完，
//      活着的客户端永远能在下一轮被服务（多实例/重启场景自愈）。
//      客户端断开或出错时同样退出内层循环，销毁 socket。
//
// 线程模型：本线程只做收发与 JSON 解析，真正的命令执行由
// Bridge->ExecuteCommand 派发到 GameThread 并阻塞等待结果（带 120s 超时）。
// ============================================================================

#include "MCPServerRunnable.h"
#include "UnrealMCPBridge.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "JsonObjectConverter.h"
#include "Misc/ScopeLock.h"
#include "Misc/Timespan.h"
#include "HAL/PlatformTime.h"

// 单次 Recv 的最大字节数
const int32 MessageBufferSize = 8192;

FMCPServerRunnable::FMCPServerRunnable(UUnrealMCPBridge* InBridge, TSharedPtr<FSocket> InListenerSocket)
    : Bridge(InBridge)
    , ListenerSocket(InListenerSocket)
    , bRunning(true)
{
    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Created server runnable"));
}

FMCPServerRunnable::~FMCPServerRunnable()
{
    // 监听 socket 归 Bridge 所有，这里不销毁；客户端 socket 已在 Run() 结束时释放
}

bool FMCPServerRunnable::Init()
{
    return true;
}

uint32 FMCPServerRunnable::Run()
{
    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Server thread starting..."));
    
    while (bRunning)
    {
        bool bPending = false;
        if (ListenerSocket->HasPendingConnection(bPending) && bPending)
        {
            UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Client connection pending, accepting..."));
            
            // 接受新连接前先释放上一个客户端 socket，否则重连会累积泄漏句柄
            if (ClientSocket.IsValid())
            {
                CloseClientSocket();
            }

            FSocket* AcceptedSocket = ListenerSocket->Accept(TEXT("MCPClient"));
            // FSocket 必须经 SocketSubsystem 销毁，直接 delete 会泄漏底层系统句柄，
            // 所以给 TSharedPtr 指定自定义删除器
            ClientSocket = MakeShareable(AcceptedSocket, [](FSocket* SocketToDestroy)
            {
                if (SocketToDestroy)
                {
                    if (ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
                    {
                        SocketSubsystem->DestroySocket(SocketToDestroy);
                    }
                }
            });
            if (ClientSocket.IsValid())
            {
                UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Client connection accepted"));

                // 设为非阻塞：空闲时 Recv 返回 SE_EWOULDBLOCK 而不是卡住线程，
                // 否则 Stop() 无法让线程退出
                ClientSocket->SetNonBlocking(true);
                ClientSocket->SetNoDelay(true);
                int32 SocketBufferSize = 65536;  // 64KB 收发缓冲
                ClientSocket->SetSendBufferSize(SocketBufferSize, SocketBufferSize);
                ClientSocket->SetReceiveBufferSize(SocketBufferSize, SocketBufferSize);
                
                uint8 Buffer[MessageBufferSize + 1];
                TArray<uint8> ReceiveBuffer;
                // 缓冲里首次出现残留数据的时间点，用于判断半包是否已经烂掉
                double PartialFrameStartTime = 0.0;

                // 帧协议模式：
                //   Framed —— 新协议：4 字节大端长度前缀 + JSON payload，按长度精确收满；
                //             从根本上杜绝 TCP 分片下「半包被当整帧解析」的数据丢失。
                //   Legacy —— 旧协议：裸 JSON 流，收到的字节整体尝试解析（向后兼容旧客户端）。
                // 判定依据：合法 JSON 一定以 '{' 或空白开头，绝不可能以 0x00 开头，
                // 因此首字节为 0x00 即无歧义地进入 Framed 模式。
                enum class EFrameMode { Undecided, Legacy, Framed };
                EFrameMode FrameMode = EFrameMode::Undecided;

                // 执行命令并回写响应。响应 JSON 中注入 bytes_received（本条命令实际
                // 收到的字节数），供客户端与发送长度比对做完整性校验。
                auto SendResponse = [this](const FString& Response, int32 BytesReceived)
                {
                    FString ResponseToSend = Response;
                    TSharedPtr<FJsonObject> ResponseObj;
                    TSharedRef<TJsonReader<>> ResponseReader = TJsonReaderFactory<>::Create(ResponseToSend);
                    if (FJsonSerializer::Deserialize(ResponseReader, ResponseObj) && ResponseObj.IsValid())
                    {
                        ResponseObj->SetNumberField(TEXT("bytes_received"), BytesReceived);
                        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseToSend);
                        FJsonSerializer::Serialize(ResponseObj.ToSharedRef(), Writer);
                    }

                    // 循环发完所有响应字节。非阻塞 socket 在发送缓冲写满时
                    // 会返回失败并带 SE_EWOULDBLOCK，这属于正常背压，必须重试，
                    // 否则大响应（截图、长列表）会被截断成非法 JSON。
                    FTCHARToUTF8 Utf8Response(*ResponseToSend);
                    const char* Data = Utf8Response.Get();
                    const int32 TotalLen = Utf8Response.Length();
                    int32 TotalSent = 0;
                    const double SendDeadline = FPlatformTime::Seconds() + 30.0;
                    while (TotalSent < TotalLen)
                    {
                        int32 BytesSent = 0;
                        if (ClientSocket->Send((const uint8*)(Data + TotalSent), TotalLen - TotalSent, BytesSent))
                        {
                            TotalSent += BytesSent;
                            continue;
                        }

                        const int32 SendError = (int32)ISocketSubsystem::Get()->GetLastErrorCode();
                        if (SendError == SE_EWOULDBLOCK || SendError == SE_EINTR)
                        {
                            if (FPlatformTime::Seconds() > SendDeadline)
                            {
                                UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Send timed out after 30s, sent %d/%d bytes"), TotalSent, TotalLen);
                                break;
                            }
                            // 对端还没读走数据，稍等再发
                            FPlatformProcess::Sleep(0.005f);
                            continue;
                        }

                        UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Failed to send response, error %d, sent %d/%d bytes"), SendError, TotalSent, TotalLen);
                        break;
                    }
                };

                // 把一段 payload 当作一条 JSON 命令解析并执行。
                // 返回 true = 该帧已消费（无论执行成功与否）；false = JSON 不完整。
                auto ProcessPayload = [&](const uint8* Data, int32 DataLen, int32 BytesReceived) -> bool
                {
                    // 拷一份并补 0 结尾，供 JSON 解析器按字符串读取
                    TArray<uint8> NullTerminated(Data, DataLen);
                    NullTerminated.Add(0);
                    FString ReceivedText = UTF8_TO_TCHAR((const ANSICHAR*)NullTerminated.GetData());
                    ReceivedText.TrimEndInline();

                    TSharedPtr<FJsonObject> JsonObject;
                    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ReceivedText);
                    if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
                    {
                        return false;
                    }

                    FString CommandType;
                    if (!JsonObject->TryGetStringField(TEXT("type"), CommandType))
                    {
                        UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Missing 'type' field in command, dropping frame"));
                        return true;
                    }

                    // 派发到 GameThread 执行并等待结果（内部带 120s 超时）
                    FString Response = Bridge->ExecuteCommand(CommandType, JsonObject->GetObjectField(TEXT("params")));
                    SendResponse(Response, BytesReceived);
                    return true;
                };

                while (bRunning)
                {
                    // 先等可读再 Recv。引擎语义（SocketsBSD.cpp FSocketBSD::Recv）：
                    // 流式 socket 上 recv()==0（对端 FIN）返回 false；而 -1+EWOULDBLOCK
                    // （非阻塞、暂无数据）被翻译成 return true 且 BytesRead==0。
                    // 因此绝不能在无数据时直接 Recv——wouldblock 会被误判成断连，
                    // 关闭健康连接（每命令断连重连 + 首帧未到即被关，客户端收 WinError 10053）。
                    if (!ClientSocket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(100)))
                    {
                        // 超时（含 Stop 置位后的周期性醒来检查 bRunning）或 select 异常：
                        // 一律回到循环头，不做任何断连判定
                        continue;
                    }

                    int32 BytesRead = 0;
                    if (ClientSocket->Recv(Buffer, MessageBufferSize, BytesRead))
                    {
                        if (BytesRead == 0)
                        {
                            // 防御性兜底：可读后读到 0 字节视为对端关闭
                            //（正常 FIN 走 Recv==false 分支）
                            UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Client disconnected (zero bytes after readable)"));
                            break;
                        }

                        // 累加本次收到的数据（一条命令可能被 TCP 拆成多个分段）
                        ReceiveBuffer.Append(Buffer, BytesRead);

                        // 防止非法输入把缓冲撑爆
                        if (ReceiveBuffer.Num() > 16 * 1024 * 1024)
                        {
                            UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Receive buffer exceeded 16MB, resetting"));
                            ReceiveBuffer.Reset();
                            FrameMode = EFrameMode::Undecided;
                            PartialFrameStartTime = 0.0;
                            continue;
                        }

                        if (FrameMode == EFrameMode::Undecided && ReceiveBuffer.Num() > 0)
                        {
                            FrameMode = (ReceiveBuffer[0] == 0x00) ? EFrameMode::Framed : EFrameMode::Legacy;
                        }

                        bool bConsumedSomething = false;

                        if (FrameMode == EFrameMode::Framed)
                        {
                            // 长度前缀模式：精确读满 4 + N 字节再解析。
                            // 循环处理以便消费缓冲中可能残留的后续帧。
                            while (bRunning && ReceiveBuffer.Num() >= 4)
                            {
                                const int32 FrameLen = ((int32)ReceiveBuffer[0] << 24)
                                    | ((int32)ReceiveBuffer[1] << 16)
                                    | ((int32)ReceiveBuffer[2] << 8)
                                    | ((int32)ReceiveBuffer[3]);

                                if (FrameLen <= 0 || FrameLen > 16 * 1024 * 1024)
                                {
                                    UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Invalid frame length %d, discarding buffer to resync"), FrameLen);
                                    ReceiveBuffer.Reset();
                                    FrameMode = EFrameMode::Undecided;
                                    break;
                                }

                                if (ReceiveBuffer.Num() < 4 + FrameLen)
                                {
                                    // 帧未收满，保留数据等下一次 Recv 补齐
                                    break;
                                }

                                ProcessPayload(ReceiveBuffer.GetData() + 4, FrameLen, FrameLen);

                                // 消费这一帧（保留其后可能的残余字节）
                                const int32 Consumed = 4 + FrameLen;
                                const int32 Remaining = ReceiveBuffer.Num() - Consumed;
                                if (Remaining > 0)
                                {
                                    FMemory::Memmove(ReceiveBuffer.GetData(), ReceiveBuffer.GetData() + Consumed, Remaining);
                                }
                                ReceiveBuffer.SetNum(Remaining > 0 ? Remaining : 0);
                                bConsumedSomething = true;
                            }
                        }
                        else if (FrameMode == EFrameMode::Legacy && ReceiveBuffer.Num() > 0)
                        {
                            // 旧协议：把整个缓冲当作一条 JSON 命令来解析。
                            // MCP 客户端一问一答、不做管道化，所以解析成功即整帧到达，
                            // 可以放心清空缓冲；解析失败则说明帧未收完，保留数据等下次补齐。
                            if (ProcessPayload(ReceiveBuffer.GetData(), ReceiveBuffer.Num(), ReceiveBuffer.Num()))
                            {
                                ReceiveBuffer.Reset();
                                bConsumedSomething = true;
                            }
                        }

                        // 一命令一连接：命令执行并回完响应后主动关闭本连接，
                        // 回到外层 accept 下一个客户端。结构上消灭「闲置/僵尸
                        // 连接占住单客户端 bridge」的卡死模式（MCP server 重启
                        // 残留的 lifespan 空闲连接不再能饿死活实例）。python
                        // 端一问一答从不流水线，残余字节随连接一起丢弃。
                        if (bConsumedSomething)
                        {
                            CloseClientSocket();
                            break;
                        }

                        // 半包存活上限（两种模式共用）：对端半途中断时，残留字节
                        // 永久拼不成合法帧，该连接会彻底瘫死，超时即丢弃重新同步。
                        if (!bConsumedSomething && ReceiveBuffer.Num() > 0)
                        {
                            const double Now = FPlatformTime::Seconds();
                            if (PartialFrameStartTime <= 0.0)
                            {
                                PartialFrameStartTime = Now;
                            }
                            else if (Now - PartialFrameStartTime > 10.0)
                            {
                                UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Partial frame stale for 10s (%d bytes), discarding to resync"), ReceiveBuffer.Num());
                                ReceiveBuffer.Reset();
                                FrameMode = EFrameMode::Undecided;
                                PartialFrameStartTime = 0.0;
                            }
                        }
                        else
                        {
                            PartialFrameStartTime = 0.0;
                        }
                    }
                    else
                    {
                        // Recv 失败：流式 socket 上对端 FIN（优雅关闭）也走这里
                        //（BytesRead==0 且 bSuccess=false），与真错误统一由此判定
                        int32 LastError = (int32)ISocketSubsystem::Get()->GetLastErrorCode();

                        if (LastError == SE_EWOULDBLOCK || LastError == SE_EINTR)
                        {
                            // 罕见竞态（如 select 可读与 recv 之间数据被信号打断）：
                            // 可恢复，回到循环头继续等待
                            UE_LOG(LogTemp, Verbose, TEXT("MCPServerRunnable: Socket would block after readable wait, continuing..."));
                            continue;
                        }

                        UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Client disconnected or error. Last error code: %d"), LastError);
                        break;
                    }
                }

                // 内层循环结束（断开/出错/停机）：释放当前客户端 socket
                CloseClientSocket();
            }
            else
            {
                UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Failed to accept client connection"));
            }
        }
        
        // 没有待接入连接时短暂休眠，避免空转
        FPlatformProcess::Sleep(0.1f);
    }
    
    CloseClientSocket();
    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Server thread stopping"));
    return 0;
}

void FMCPServerRunnable::CloseClientSocket()
{
    if (!ClientSocket.IsValid())
    {
        return;
    }

    // ClientSocket 带有走 SocketSubsystem 的自定义删除器，
    // 因此释放最后一个引用即可正确回收系统句柄
    ClientSocket->Close();
    ClientSocket.Reset();
}

void FMCPServerRunnable::Stop()
{
    // 由 GameThread 调用，通知本线程退出（bRunning 是 atomic）
    bRunning = false;
}

void FMCPServerRunnable::Exit()
{
}

// ----------------------------------------------------------------------------
// 以下 HandleClientConnection / ProcessMessage 为历史遗留实现（按换行分帧、
// 阻塞式 Recv），当前 Run() 并未调用，属未启用代码，保留仅作参考。
// ----------------------------------------------------------------------------

void FMCPServerRunnable::HandleClientConnection(TSharedPtr<FSocket> InClientSocket)
{
    if (!InClientSocket.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("MCPServerRunnable: Invalid client socket passed to HandleClientConnection"));
        return;
    }

    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Starting to handle client connection"));
    
    // Set socket options for better connection stability
    InClientSocket->SetNonBlocking(false);
    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Set socket to blocking mode"));
    
    // Properly read full message with timeout
    const int32 MaxBufferSize = 4096;
    uint8 Buffer[MaxBufferSize];
    FString MessageBuffer;
    
    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Starting message receive loop"));
    
    while (bRunning && InClientSocket.IsValid())
    {
        // Log socket state
        bool bIsConnected = InClientSocket->GetConnectionState() == SCS_Connected;
        UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Socket state - Connected: %s"), 
               bIsConnected ? TEXT("true") : TEXT("false"));
        
        // Log pending data status before receive
        uint32 PendingDataSize = 0;
        bool HasPendingData = InClientSocket->HasPendingData(PendingDataSize);
        UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Before Recv - HasPendingData=%s, Size=%d"), 
               HasPendingData ? TEXT("true") : TEXT("false"), PendingDataSize);
        
        // Try to receive data with timeout
        int32 BytesRead = 0;
        bool bReadSuccess = false;
        
        UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Attempting to receive data..."));
        bReadSuccess = InClientSocket->Recv(Buffer, MaxBufferSize, BytesRead, ESocketReceiveFlags::None);
        
        UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Recv attempt complete - Success=%s, BytesRead=%d"), 
               bReadSuccess ? TEXT("true") : TEXT("false"), BytesRead);
        
        if (BytesRead > 0)
        {
            // Log raw data for debugging
            FString HexData;
            for (int32 i = 0; i < FMath::Min(BytesRead, 50); ++i)
            {
                HexData += FString::Printf(TEXT("%02X "), Buffer[i]);
            }
            UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Raw data (first 50 bytes hex): %s%s"), 
                   *HexData, BytesRead > 50 ? TEXT("...") : TEXT(""));
            
            // Convert and log received data
            Buffer[BytesRead] = 0; // Null terminate
            FString ReceivedData = UTF8_TO_TCHAR(Buffer);
            UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Received data as string: '%s'"), *ReceivedData);
            
            // Append to message buffer
            MessageBuffer.Append(ReceivedData);
            
            // Process complete messages (messages are terminated with newline)
            if (MessageBuffer.Contains(TEXT("\n")))
            {
                UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Newline detected in buffer, processing messages"));
                
                TArray<FString> Messages;
                MessageBuffer.ParseIntoArray(Messages, TEXT("\n"), true);
                
                UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Found %d message(s) in buffer"), Messages.Num());
                
                // Process all complete messages
                for (int32 i = 0; i < Messages.Num() - 1; ++i)
                {
                    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Processing message %d: '%s'"), 
                           i + 1, *Messages[i]);
                    ProcessMessage(InClientSocket, Messages[i]);
                }
                
                // Keep any incomplete message in the buffer
                MessageBuffer = Messages.Last();
                UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Remaining buffer after processing: %s"), 
                       *MessageBuffer);
            }
            else
            {
                UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: No complete message yet (no newline detected)"));
            }
        }
        else if (!bReadSuccess)
        {
            UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Connection closed or error occurred - Last error: %d"), 
                   (int32)ISocketSubsystem::Get()->GetLastErrorCode());
            break;
        }
        
        // Small sleep to prevent tight loop
        FPlatformProcess::Sleep(0.01f);
    }
    
    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Exited message receive loop"));
}

void FMCPServerRunnable::ProcessMessage(TSharedPtr<FSocket> Client, const FString& Message)
{
    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Processing message: %s"), *Message);
    
    // Parse message as JSON
    TSharedPtr<FJsonObject> JsonMessage;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Message);
    
    if (!FJsonSerializer::Deserialize(Reader, JsonMessage) || !JsonMessage.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Failed to parse message as JSON"));
        return;
    }
    
    // Extract command type and parameters using MCP protocol format
    FString CommandType;
    TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject());
    
    if (!JsonMessage->TryGetStringField(TEXT("command"), CommandType))
    {
        UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Message missing 'command' field"));
        return;
    }
    
    // Parameters are optional in MCP protocol
    if (JsonMessage->HasField(TEXT("params")))
    {
        TSharedPtr<FJsonValue> ParamsValue = JsonMessage->TryGetField(TEXT("params"));
        if (ParamsValue.IsValid() && ParamsValue->Type == EJson::Object)
        {
            Params = ParamsValue->AsObject();
        }
    }
    
    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Executing command: %s"), *CommandType);
    
    // Execute command
    FString Response = Bridge->ExecuteCommand(CommandType, Params);
    
    // Send response with newline terminator
    Response += TEXT("\n");
    
    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Sending response: %s"), *Response);
    
    FTCHARToUTF8 Utf8Response(*Response);
    const char* Data = Utf8Response.Get();
    int32 TotalLen = Utf8Response.Length();
    int32 TotalSent = 0;
    while (TotalSent < TotalLen)
    {
        int32 BytesSent = 0;
        if (!Client->Send((const uint8*)(Data + TotalSent), TotalLen - TotalSent, BytesSent))
        {
            UE_LOG(LogTemp, Error, TEXT("MCPServerRunnable: Failed to send response"));
            break;
        }
        TotalSent += BytesSent;
    }
    if (TotalSent >= TotalLen)
    {
        UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Response sent successfully, bytes: %d"), TotalSent);
    }
} 