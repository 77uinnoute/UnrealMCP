#include "Commands/UnrealMCPPythonAPI.h"

#include "UnrealMCPBridge.h"
#include "Core/MCPCommandRegistry.h"
#include "Editor.h"
#include "Json.h"
#include "Misc/ScopeLock.h"

FString UUnrealMCPPythonAPI::ExecuteMCPCommand(const FString& CommandType, const FString& ParamsJson)
{
    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject);

    // Re-entrancy guard: a command whose registry entry forbids the editor python
    // loopback must never be invoked from here (it re-enters the GameThread queue /
    // python VM and would deadlock). The policy lives on the registry entry, so the
    // TCP path is unaffected and no second name list is kept.
    const FMCPCommandEntry* LoopbackEntry = FMCPCommandRegistry::Get().Find(CommandType);

    if (!IsInGameThread())
    {
        ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
        ResponseJson->SetStringField(TEXT("error"), TEXT("wrong_thread"));
        ResponseJson->SetStringField(TEXT("detail"), TEXT("ExecuteMCPCommand must be called on the GameThread (editor python satisfies this)"));
    }
    else if (LoopbackEntry && LoopbackEntry->Flags.bLoopbackForbidden)
    {
        ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
        ResponseJson->SetStringField(TEXT("error"), TEXT("reentry_forbidden"));
        ResponseJson->SetStringField(TEXT("detail"), FString::Printf(
            TEXT("Command '%s' would re-enter the GameThread task queue / python VM from an editor python script and deadlock. It is only available over TCP."),
            *CommandType));
    }
    else
    {
        UUnrealMCPBridge* Bridge = GEditor ? GEditor->GetEditorSubsystem<UUnrealMCPBridge>() : nullptr;
        if (!Bridge)
        {
            ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
            ResponseJson->SetStringField(TEXT("error"), TEXT("bridge_unavailable"));
            ResponseJson->SetStringField(TEXT("detail"), TEXT("UnrealMCP bridge subsystem is not running"));
        }
        else
        {
            // Accept either the full TCP-style body {"type"/"command":..., "params":{...}}
            // (the "command" field is ignored, CommandType wins) or a bare
            // params object {...}.
            //
            // The unwrap keys off the envelope marker ("type" / "command"), NOT off "params" alone:
            // a command's own argument may legitimately be called "params" (add_blueprint_function_node
            // takes its pin-default map under exactly that name), and unwrapping on "params" alone
            // replaced the whole payload with that inner object - the handler then reported
            // "Missing 'blueprint_name'" while the caller had passed it.
            TSharedPtr<FJsonObject> Parsed;
            TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ParamsJson);
            TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject());
            const bool bParsed = FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid();
            if (bParsed)
            {
                FString EnvelopeMarker;
                const TSharedPtr<FJsonObject>* NestedParams = nullptr;
                const bool bIsEnvelope = Parsed->TryGetStringField(TEXT("type"), EnvelopeMarker)
                    || Parsed->TryGetStringField(TEXT("command"), EnvelopeMarker);
                if (bIsEnvelope && Parsed->TryGetObjectField(TEXT("params"), NestedParams) && NestedParams)
                {
                    Params = *NestedParams;
                }
                else
                {
                    Params = Parsed;
                }
            }

            if (!bParsed)
            {
                ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
                ResponseJson->SetStringField(TEXT("error"), TEXT("invalid_params_json"));
            }
            else
            {
                // Same synchronous dispatcher the TCP path uses.
                ResponseJson = Bridge->DispatchCommandDirect(CommandType, Params);
            }
        }
    }

    FString ResultString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
    FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);
    return ResultString;
}
