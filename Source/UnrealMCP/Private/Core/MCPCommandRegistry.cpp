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

int32 FMCPCommandRegistry::AddParamsToCategory(const FString& Category, const TArray<FMCPParamSpec>& ExtraParams)
{
    if (bSealed || ExtraParams.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("MCPCommandRegistry: AddParamsToCategory('%s') ignored (%s)"),
            *Category, bSealed ? TEXT("the table is sealed") : TEXT("no parameters given"));
        return 0;
    }

    int32 Extended = 0;
    for (FMCPCommandEntry& Command : Commands)
    {
        if (Command.Category != Category)
        {
            continue;
        }

        bool bTouched = false;
        for (const FMCPParamSpec& Extra : ExtraParams)
        {
            const bool bAlreadyDeclared = Command.Params.ContainsByPredicate([&Extra](const FMCPParamSpec& Spec)
            {
                return Spec.Name == Extra.Name;
            });
            if (!bAlreadyDeclared)
            {
                Command.Params.Add(Extra);
                bTouched = true;
            }
        }
        Extended += bTouched ? 1 : 0;
    }

    UE_LOG(LogTemp, Display, TEXT("MCPCommandRegistry: category '%s' now declares %d shared parameter(s) on %d command(s)"),
        *Category, ExtraParams.Num(), Extended);
    return Extended;
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

        // Parameters the command never declared. These used to vanish without a trace - a caller passing
        // `width` to take_screenshot got a plain success and believed the resize happened (it never did).
        // They are reported rather than rejected: `persist` is handled here (not by any handler), and a few
        // handlers read keys their registration does not list yet, so a hard refusal would break commands
        // that work. Reported + logged is the part that removes the silent-drop failure mode.
        TArray<FString> UndeclaredParams;
        if (Params.IsValid())
        {
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Params->Values)
            {
                if (Pair.Key == TEXT("persist"))
                {
                    continue;
                }
                const bool bDeclared = Entry->Params.ContainsByPredicate([&Pair](const FMCPParamSpec& Spec)
                {
                    return Spec.Name == Pair.Key;
                });
                if (!bDeclared)
                {
                    UndeclaredParams.Add(Pair.Key);
                }
            }
        }

        // A command that writes the editor world gets one undo step and a level that is left UNSAVED-DIRTY.
        // Two things used to be missing together: the writes answered success with a correct read-back while
        // get_dirty_map_packages() stayed empty (nothing would prompt a save, so the change could be dropped
        // silently), and with no transaction there was no undo entry either. Driven by the flag, so a future
        // editor-world writer cannot forget it.
        TUniquePtr<FUnrealMCPCommonUtils::FMCPEditorWorldWriteScope> WriteScope;
        if (Entry->Flags.bWritesEditorWorld)
        {
            WriteScope = MakeUnique<FUnrealMCPCommonUtils::FMCPEditorWorldWriteScope>(Entry->Name);
        }

        TSharedPtr<FJsonObject> Result = Entry->Handler(Params);

        if (WriteScope.IsValid())
        {
            WriteScope->Finish(FUnrealMCPCommonUtils::ResponseIndicatesSuccess(Result));
            WriteScope.Reset();
        }

        if (UndeclaredParams.Num() > 0)
        {
            UndeclaredParams.Sort();
            UE_LOG(LogTemp, Warning, TEXT("MCPCommandRegistry: '%s' was called with undeclared parameter(s): %s"),
                *Entry->Name, *FString::Join(UndeclaredParams, TEXT(", ")));

            if (Result.IsValid())
            {
                TArray<TSharedPtr<FJsonValue>> Items;
                Items.Reserve(UndeclaredParams.Num());
                for (const FString& Undeclared : UndeclaredParams)
                {
                    Items.Add(MakeShared<FJsonValueString>(Undeclared));
                }
                Result->SetArrayField(TEXT("undeclared_params"), Items);
                Result->SetStringField(TEXT("undeclared_params_hint"),
                    FString::Printf(TEXT("%s does not declare these parameter(s), so they had no defined effect "
                                         "(some handlers still read a few of them, but nothing is guaranteed). "
                                         "Check the declared set with list_mcp_commands."),
                        *Entry->Name));
            }
        }

        // Editor/PIE context on EVERY reply, injected once here rather than by each handler: a caller has
        // to be able to tell "no such asset" from "PIE is running and this type cannot load in play mode",
        // and a per-handler convention would leave gaps exactly where nobody remembered.
        FUnrealMCPCommonUtils::AddEditorState(Result);

        // A write that landed on the editor world while PIE ran changes nothing the play session renders.
        if (Entry->Flags.bWritesEditorWorld)
        {
            FUnrealMCPCommonUtils::AddEditorWorldWriteWarning(Result, Entry->Name);
        }
        return Result;
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
