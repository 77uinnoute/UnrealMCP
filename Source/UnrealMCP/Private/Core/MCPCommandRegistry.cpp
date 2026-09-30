#include "Core/MCPCommandRegistry.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"

FMCPCommandRegistry& FMCPCommandRegistry::Get()
{
    // Function-local static: registrars run during module startup, which can happen before or
    // after other translation units' static initialisers.
    static FMCPCommandRegistry Instance;
    return Instance;
}

void FMCPCommandRegistry::Register(FMCPCommandEntry Entry)
{
    if (Entry.Name.IsEmpty())
    {
        UE_LOG(LogTemp, Error, TEXT("MCPCommandRegistry: refused a registration with an empty command name (category '%s')"),
            *Entry.Category);
        return;
    }
    if (!Entry.Handler)
    {
        UE_LOG(LogTemp, Error, TEXT("MCPCommandRegistry: refused '%s' - no handler bound"), *Entry.Name);
        return;
    }
    if (bSealed)
    {
        UE_LOG(LogTemp, Error, TEXT("MCPCommandRegistry: refused late registration of '%s' - the table is sealed"),
            *Entry.Name);
        return;
    }
    Commands.Add(MoveTemp(Entry));
}

void FMCPCommandRegistry::Reset()
{
    Commands.Reset();
    NameToIndex.Reset();
    bSealed = false;
}

void FMCPCommandRegistry::Seal()
{
    if (bSealed)
    {
        return;
    }

    // Drop duplicates in one pass so the conflict is reported with both sides, and so neither
    // introspection nor dispatch ever sees the same name twice.
    TArray<FMCPCommandEntry> Unique;
    Unique.Reserve(Commands.Num());
    for (FMCPCommandEntry& Command : Commands)
    {
        const int32 Existing = Unique.IndexOfByPredicate([&Command](const FMCPCommandEntry& Other)
        {
            return Other.Name == Command.Name;
        });
        if (Existing != INDEX_NONE)
        {
            UE_LOG(LogTemp, Error, TEXT("MCPCommandRegistry: duplicate command '%s' (category '%s') ignored; it is already registered by category '%s'"),
                *Command.Name, *Command.Category, *Unique[Existing].Category);
            continue;
        }
        Unique.Add(MoveTemp(Command));
    }
    Commands = MoveTemp(Unique);

    NameToIndex.Reset();
    for (int32 Index = 0; Index < Commands.Num(); ++Index)
    {
        NameToIndex.Add(Commands[Index].Name, Index);
    }
    bSealed = true;

    TSet<FString> Categories;
    for (const FMCPCommandEntry& Command : Commands)
    {
        Categories.Add(Command.Category);
    }
    UE_LOG(LogTemp, Display, TEXT("MCPCommandRegistry: sealed with %d commands across %d categories"),
        Commands.Num(), Categories.Num());
}

const FMCPCommandEntry* FMCPCommandRegistry::Find(const FString& Name) const
{
    const int32* Index = NameToIndex.Find(Name);
    return Index ? &Commands[*Index] : nullptr;
}

TSharedPtr<FJsonObject> FMCPCommandRegistry::Execute(const FString& Name, const TSharedPtr<FJsonObject>& Params) const
{
    if (const FMCPCommandEntry* Entry = Find(Name))
    {
        // One persist decision per dispatch. The flag is what the command's caller asked for; helpers
        // that save on their own (GuardPersist, MakeWriteResult, the compile handlers) read it back
        // through IsPersistEnabled() instead of deciding again - a batch that asked for persist=false
        // used to be saved anyway by those very helpers.
        bool bPersist = Entry->Flags.bPersistAfterSuccess;
        if (Params.IsValid())
        {
            Params->TryGetBoolField(TEXT("persist"), bPersist);
        }
        FUnrealMCPCommonUtils::FMCPPersistScope PersistScope(bPersist);

        return Entry->Handler(Params);
    }

    // Same message as the hand-written fallback this replaced, plus the error_code the result
    // form carries so callers do not have to match on the message text.
    //
    // The hint exists because "Unknown command" was read as "the feature is broken" more than once.
    // Most MCP tools forward a command of the same name, so a name that reaches this branch is
    // usually a typo - but a few tools exist only in python (preview_material_expression: its
    // build_material_preview step is a command, the render/PNG step is python), and those can never
    // be called from execute_python_* inside the editor.
    TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unknown_command"),
        FString::Printf(TEXT("Unknown command: %s"), *Name));
    Error->SetStringField(TEXT("hint"),
        TEXT("Only registered bridge commands can be called from here. Most MCP tools forward one "
             "under the same name; the few that are python-only (e.g. preview_material_expression) "
             "have to be used from the MCP tool surface."));
    Error->SetNumberField(TEXT("registered_command_count"), Commands.Num());
    return Error;
}

TArray<const FMCPCommandEntry*> FMCPCommandRegistry::ByCategory(const FString& Category) const
{
    TArray<const FMCPCommandEntry*> Matches;
    for (const FMCPCommandEntry& Command : Commands)
    {
        if (Command.Category == Category)
        {
            Matches.Add(&Command);
        }
    }
    return Matches;
}
