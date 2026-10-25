#include "Commands/UnrealMCPPythonAPI.h"

#include "UnrealMCPBridge.h"
#include "Core/MCPCommandRegistry.h"
#include "Editor.h"
#include "Json.h"
#include "Misc/ScopeLock.h"

namespace
{
    /** Parameter names a command declares (empty for an unknown command). */
    void GetDeclaredParamNames(const FString& CommandType, TSet<FString>& OutNames)
    {
        if (const FMCPCommandEntry* Entry = FMCPCommandRegistry::Get().Find(CommandType))
        {
            for (const FMCPParamSpec& Spec : Entry->Params)
            {
                OutNames.Add(Spec.Name);
            }
        }
    }

    int32 CountDeclaredKeys(const TSharedPtr<FJsonObject>& Object, const TSet<FString>& Declared)
    {
        int32 Count = 0;
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
        {
            Count += Declared.Contains(Pair.Key) ? 1 : 0;
        }
        return Count;
    }

    /**
     * Accepts every spelling a caller might use for the loopback's parameter body:
     *
     *   1. a bare params object                 {"name": "WBP_Hud", "path": "/Game/UI"}
     *   2. the TCP-shaped envelope              {"type": "create_umg_widget_blueprint", "params": {...}}
     *   3. the same envelope without a marker   {"params": {...}}
     *
     * Case 3 is ambiguous with a command that has a real argument called "params"
     * (add_blueprint_function_node takes its pin-default map under exactly that name), so it is
     * resolved against the command's own declaration: whichever object matches more declared
     * parameter names wins. A caller that passes the argument properly scores higher on the outer
     * body and is left alone; a caller that wrapped the body scores higher on the inner one. When the
     * body is {"params": {...}} and nothing is declared, the single key decides.
     *
     * The echoed marker key (case 2) is dropped from the body when the command does not declare it,
     * so an envelope never shows up as an undeclared parameter.
     */
    TSharedPtr<FJsonObject> NormalizeParamsBody(const TSharedPtr<FJsonObject>& Body, const FString& CommandType)
    {
        TSet<FString> Declared;
        GetDeclaredParamNames(CommandType, Declared);

        FString Marker;
        const bool bMarkedEnvelope = Body->TryGetStringField(TEXT("type"), Marker)
            || Body->TryGetStringField(TEXT("command"), Marker);

        const TSharedPtr<FJsonObject>* Nested = nullptr;
        const bool bHasNested = Body->TryGetObjectField(TEXT("params"), Nested) && Nested && Nested->IsValid();

        if (bMarkedEnvelope && bHasNested)
        {
            return *Nested;
        }

        if (!bMarkedEnvelope && bHasNested)
        {
            const int32 OuterScore = CountDeclaredKeys(Body, Declared);
            const int32 InnerScore = CountDeclaredKeys(*Nested, Declared);
            if (InnerScore > OuterScore || (Body->Values.Num() == 1 && OuterScore == 0))
            {
                return *Nested;
            }
        }

        if (bMarkedEnvelope && !Declared.Contains(TEXT("type")) && !Declared.Contains(TEXT("command")))
        {
            TSharedPtr<FJsonObject> Stripped = MakeShareable(new FJsonObject());
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Body->Values)
            {
                if (Pair.Key != TEXT("type") && Pair.Key != TEXT("command"))
                {
                    Stripped->SetField(Pair.Key, Pair.Value);
                }
            }
            return Stripped;
        }

        return Body;
    }
}

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
            TSharedPtr<FJsonObject> Parsed;
            TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ParamsJson);
            TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject());
            const bool bParsed = FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid();
            if (bParsed)
            {
                Params = NormalizeParamsBody(Parsed, CommandType);
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
