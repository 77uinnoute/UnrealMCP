#include "Commands/UnrealMCPProjectCommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Core/MCPCommandJournal.h"
#include "GameFramework/InputSettings.h"

FUnrealMCPProjectCommands::FUnrealMCPProjectCommands()
{
}

void FUnrealMCPProjectCommands::RegisterCommands(FMCPCommandRegistry& Registry)
{
    MCP_REGISTER_COMMAND(Registry, "create_input_mapping", "project", "Add an action key mapping to the project's input settings.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("action_name"), TEXT("string"), TEXT("Input action name")),
            MCPParam(TEXT("key"), TEXT("string"), TEXT("Key name (e.g. SpaceBar, LeftMouseButton)")),
            MCPParamOpt(TEXT("shift"), TEXT("bool"), TEXT("Require Shift")),
            MCPParamOpt(TEXT("ctrl"), TEXT("bool"), TEXT("Require Ctrl")),
            MCPParamOpt(TEXT("alt"), TEXT("bool"), TEXT("Require Alt")),
            MCPParamOpt(TEXT("cmd"), TEXT("bool"), TEXT("Require Cmd")),
        }), MCPFlags(),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleCreateInputMapping(Params); });

    // Diagnostics, not a project setting: it lives here because this domain file already has the
    // Registry reference and needs no build-config change. Read-only, safe over the python loopback.
    MCP_REGISTER_COMMAND(Registry, "get_command_journal", "diagnostics",
        "Recent dispatch history: the last N commands with their duration and outcome, the one command "
        "that started and never finished (the shape a wedged GameThread leaves), and per-command timing "
        "aggregates. Data is kept in memory (ring buffer) and mirrored to Saved/Logs/UnrealMCP_journal.jsonl, "
        "so it survives the editor being killed. Read-only.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("limit"), TEXT("int"), TEXT("How many recent entries to return (default 64, cap = ring capacity)")),
            MCPParamOpt(TEXT("category"), TEXT("string"), TEXT("Only entries of this command category, e.g. \"blueprint\"")),
        }), MCPFlags(),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleGetCommandJournal(Params); });
}

TSharedPtr<FJsonObject> FUnrealMCPProjectCommands::HandleGetCommandJournal(const TSharedPtr<FJsonObject>& Params)
{
    FMCPCommandJournal& Journal = FMCPCommandJournal::Get();

    double LimitValue = 64.0;
    if (Params.IsValid())
    {
        Params->TryGetNumberField(TEXT("limit"), LimitValue);
    }
    const int32 Limit = FMath::Clamp(FMath::RoundToInt(LimitValue), 1, Journal.Capacity());

    FString Category;
    if (Params.IsValid())
    {
        Params->TryGetStringField(TEXT("category"), Category);
    }

    TArray<TSharedPtr<FJsonValue>> EntryItems;
    for (const FMCPJournalEntry& Entry : Journal.Snapshot(Limit, Category))
    {
        TSharedPtr<FJsonObject> Item = MakeShareable(new FJsonObject);
        Item->SetNumberField(TEXT("seq"), static_cast<double>(Entry.Seq));
        Item->SetStringField(TEXT("command"), Entry.Command);
        Item->SetStringField(TEXT("category"), Entry.Category);
        Item->SetStringField(TEXT("started"), Entry.StartedAtUtc);
        Item->SetBoolField(TEXT("finished"), Entry.bFinished);
        Item->SetNumberField(TEXT("duration_ms"), Entry.DurationMs);
        if (!Entry.ErrorCode.IsEmpty())
        {
            Item->SetStringField(TEXT("error_code"), Entry.ErrorCode);
        }
        Item->SetNumberField(TEXT("params_size"), Entry.ParamsSize);
        Item->SetStringField(TEXT("params_hash"), Entry.ParamsHash);
        EntryItems.Add(MakeShared<FJsonValueObject>(Item));
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetArrayField(TEXT("entries"), EntryItems);
    Result->SetNumberField(TEXT("entry_count"), EntryItems.Num());
    Result->SetNumberField(TEXT("ring_capacity"), Journal.Capacity());
    Result->SetNumberField(TEXT("ring_count"), Journal.Count());
    Result->SetStringField(TEXT("journal_path"), Journal.JournalPath());

    // The journal query is itself a running command, so an unfiltered "oldest unfinished" would report
    // get_command_journal every single time and say nothing. Every OTHER unfinished entry is the real
    // signal (the command that is still holding the GameThread).
    FMCPJournalEntry InFlight;
    bool bHasInFlight = false;
    for (const FMCPJournalEntry& Candidate : Journal.Snapshot(0))
    {
        if (Candidate.bFinished || Candidate.Command == TEXT("get_command_journal"))
        {
            continue;
        }
        InFlight = Candidate;
        bHasInFlight = true;
        break;  // Snapshot is newest first: the first hit is the innermost call, the one actually stuck.
    }

    TSharedPtr<FJsonObject> InFlightJson = nullptr;
    if (bHasInFlight)
    {
        InFlightJson = MakeShareable(new FJsonObject);
        InFlightJson->SetNumberField(TEXT("seq"), static_cast<double>(InFlight.Seq));
        InFlightJson->SetStringField(TEXT("command"), InFlight.Command);
        InFlightJson->SetStringField(TEXT("category"), InFlight.Category);
        InFlightJson->SetStringField(TEXT("started"), InFlight.StartedAtUtc);
        InFlightJson->SetNumberField(TEXT("params_size"), InFlight.ParamsSize);
        InFlightJson->SetStringField(TEXT("params_hash"), InFlight.ParamsHash);
    }
    if (InFlightJson.IsValid())
    {
        Result->SetObjectField(TEXT("in_flight"), InFlightJson);
        Result->SetStringField(TEXT("in_flight_hint"),
            TEXT("This command started and never returned. If the editor is responsive the handler is simply "
                 "slow or stuck on a dialog; if the whole channel is silent, the GameThread is wedged - this "
                 "is the command that wedged it."));
    }
    else
    {
        Result->SetField(TEXT("in_flight"), MakeShareable(new FJsonValueNull()));
    }

    TArray<TSharedPtr<FJsonValue>> Aggregates;
    for (const TSharedPtr<FJsonObject>& Item : Journal.AggregateByCommand())
    {
        Aggregates.Add(MakeShared<FJsonValueObject>(Item));
    }
    Result->SetArrayField(TEXT("by_command"), Aggregates);
    Result->SetStringField(TEXT("by_command_hint"),
        TEXT("Sorted by max_ms desc over the current ring contents (finished entries only): use it to find the "
             "commands worth splitting into batches or giving a longer timeout."));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPProjectCommands::HandleCreateInputMapping(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString ActionName;
    if (!Params->TryGetStringField(TEXT("action_name"), ActionName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'action_name' parameter"));
    }

    FString Key;
    if (!Params->TryGetStringField(TEXT("key"), Key))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'key' parameter"));
    }

    // Get the input settings
    UInputSettings* InputSettings = GetMutableDefault<UInputSettings>();
    if (!InputSettings)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get input settings"));
    }

    // Create the input action mapping
    FInputActionKeyMapping ActionMapping;
    ActionMapping.ActionName = FName(*ActionName);
    ActionMapping.Key = FKey(*Key);

    // Add modifiers if provided
    if (Params->HasField(TEXT("shift")))
    {
        ActionMapping.bShift = Params->GetBoolField(TEXT("shift"));
    }
    if (Params->HasField(TEXT("ctrl")))
    {
        ActionMapping.bCtrl = Params->GetBoolField(TEXT("ctrl"));
    }
    if (Params->HasField(TEXT("alt")))
    {
        ActionMapping.bAlt = Params->GetBoolField(TEXT("alt"));
    }
    if (Params->HasField(TEXT("cmd")))
    {
        ActionMapping.bCmd = Params->GetBoolField(TEXT("cmd"));
    }

    // Add the mapping. bForceRebuildKeymaps stays at its default (true) so the action is usable
    // immediately; TryUpdateDefaultConfigFile is what makes it SURVIVE AN EDITOR RESTART - plain
    // SaveConfig() left no DefaultInput.ini behind, so the mapping vanished on the next launch and
    // every InputAction node referencing it failed to compile.
    InputSettings->AddActionMapping(ActionMapping);
    const bool bPersisted = InputSettings->TryUpdateDefaultConfigFile();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("action_name"), ActionName);
    ResultObj->SetStringField(TEXT("key"), Key);
    ResultObj->SetBoolField(TEXT("persisted"), bPersisted);
    if (!bPersisted)
    {
        ResultObj->SetStringField(TEXT("warning"),
            TEXT("The mapping is live in this session but could not be written to Config/DefaultInput.ini"));
    }
    return ResultObj;
} 