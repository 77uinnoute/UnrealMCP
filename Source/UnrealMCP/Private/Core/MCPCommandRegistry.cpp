#include "Core/MCPCommandRegistry.h"
#include "Core/MCPCommandJournal.h"
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

namespace
{
    /**
     * Records one dispatch in the command journal: opened on construction, closed on destruction - so the
     * unwinding path of a handler that throws still leaves a finished entry behind instead of a permanent
     * "in flight" ghost.
     */
    class FMCPJournalScope
    {
    public:
        FMCPJournalScope(const FString& InCommand, const FString& InCategory, const TSharedPtr<FJsonObject>& Params)
            : Seq(FMCPCommandJournal::Get().Begin(InCommand, InCategory, Params))
        {
        }

        ~FMCPJournalScope()
        {
            FMCPCommandJournal::Get().Finish(Seq, ErrorCode);
        }

        void RecordOutcome(const TSharedPtr<FJsonObject>& Reply)
        {
            if (!Reply.IsValid() || FUnrealMCPCommonUtils::ResponseIndicatesSuccess(Reply))
            {
                return;
            }
            FString Code;
            if (!Reply->TryGetStringField(TEXT("error_code"), Code))
            {
                Code = TEXT("failed");
            }
            ErrorCode = Code;
        }

        void RecordError(const FString& Code)
        {
            ErrorCode = Code;
        }

    private:
        uint64 Seq;
        FString ErrorCode;
    };

    FString JsonTypeName(EJson Type)
    {
        switch (Type)
        {
        case EJson::Null:    return TEXT("null");
        case EJson::String:  return TEXT("string");
        case EJson::Number:  return TEXT("number");
        case EJson::Boolean: return TEXT("boolean");
        case EJson::Array:   return TEXT("array");
        case EJson::Object:  return TEXT("object");
        default:             return TEXT("none");
        }
    }

    /**
     * Whether a value is clearly incompatible with the declared type.
     *
     * Deliberately lenient on scalars: a tool may forward "3" where the spec says int, or 1 where it says
     * bool, and the handler reads what it needs. Only structural mismatches (object/array vs scalar) and
     * non-numeric text for a numeric parameter are refused - those cannot be interpreted by any handler,
     * so the current behaviour was a mid-handler failure with a much worse message ("Required parameter X
     * missing" or a KeyError from python) instead of a uniform refusal.
     */
    bool ValueMatchesDeclaredType(const TSharedPtr<FJsonValue>& Value, const FString& DeclaredType)
    {
        const FString Type = DeclaredType.ToLower();
        const EJson Actual = Value->Type;

        if (Type == TEXT("object"))
        {
            return Actual == EJson::Object;
        }
        if (Type == TEXT("array"))
        {
            return Actual == EJson::Array;
        }
        if (Type == TEXT("bool"))
        {
            if (Actual == EJson::Boolean)
            {
                return true;
            }
            if (Actual == EJson::Number)
            {
                const double Number = Value->AsNumber();
                return Number == 0.0 || Number == 1.0;
            }
            if (Actual == EJson::String)
            {
                const FString Text = Value->AsString().ToLower();
                return Text == TEXT("true") || Text == TEXT("false") || Text == TEXT("0") || Text == TEXT("1");
            }
            return false;
        }
        if (Type == TEXT("int") || Type == TEXT("float"))
        {
            if (Actual == EJson::Number)
            {
                return Type == TEXT("float") || FMath::IsNearlyEqual(Value->AsNumber(), FMath::RoundToDouble(Value->AsNumber()));
            }
            if (Actual == EJson::String)
            {
                const FString Text = Value->AsString().TrimStartAndEnd();
                return Text.IsNumeric();
            }
            return false;
        }
        if (Type == TEXT("string") || Type == TEXT("any") || Type == TEXT("enum"))
        {
            // Scalars are all printable into a string; only containers are refused.
            return Actual != EJson::Object && Actual != EJson::Array;
        }
        return true;
    }
}

TSharedPtr<FJsonObject> FMCPCommandRegistry::Execute(const FString& Name, const TSharedPtr<FJsonObject>& Params) const
{
    if (const FMCPCommandEntry* Entry = Find(Name))
    {
        FMCPJournalScope JournalScope(Entry->Name, Entry->Category, Params);

        // Declared-contract enforcement, once, before the handler runs. The registry already knows which
        // parameters are required and what shape each one takes (it is what list_mcp_commands reports), but
        // nothing checked it: every handler re-derived "is X missing / is X a dict" by hand, in its own
        // wording, and the python side had to guess which key to send. A missing required parameter and a
        // structurally wrong value are refused here with one shape (see the fields below); a merely
        // scalar-vs-scalar difference is accepted on purpose (see ValueMatchesDeclaredType).
        TArray<FString> MissingRequired;
        TArray<TSharedPtr<FJsonValue>> BadTypes;
        TArray<TSharedPtr<FJsonValue>> BadValues;

        const TSharedPtr<FJsonObject> Provided = Params.IsValid() ? Params : MakeShareable(new FJsonObject);

        auto HasValue = [&Provided](const FString& Key) -> bool
        {
            const TSharedPtr<FJsonValue> Value = Provided->TryGetField(Key);
            return Value.IsValid() && Value->Type != EJson::Null;
        };

        for (const FMCPParamSpec& Spec : Entry->Params)
        {
            if (Spec.bRequired && !HasValue(Spec.Name))
            {
                const bool bSatisfiedByAlias = Spec.AlsoSatisfiedBy.ContainsByPredicate(
                    [&HasValue](const FString& Alias) { return HasValue(Alias); });
                if (!bSatisfiedByAlias)
                {
                    MissingRequired.Add(Spec.Name);
                    continue;
                }
            }

            const TSharedPtr<FJsonValue> Value = Provided->TryGetField(Spec.Name);
            if (!Value.IsValid() || Value->Type == EJson::Null)
            {
                continue;
            }

            if (!ValueMatchesDeclaredType(Value, Spec.Type))
            {
                TSharedPtr<FJsonObject> Item = MakeShareable(new FJsonObject);
                Item->SetStringField(TEXT("param"), Spec.Name);
                Item->SetStringField(TEXT("expected"), Spec.Type);
                Item->SetStringField(TEXT("actual"), JsonTypeName(Value->Type));
                BadTypes.Add(MakeShared<FJsonValueObject>(Item));
                continue;
            }

            if (Spec.AllowedValues.Num() > 0 && Value->Type == EJson::String
                && !Spec.AllowedValues.Contains(Value->AsString()))
            {
                TSharedPtr<FJsonObject> Item = MakeShareable(new FJsonObject);
                Item->SetStringField(TEXT("param"), Spec.Name);
                Item->SetStringField(TEXT("value"), Value->AsString());
                TArray<TSharedPtr<FJsonValue>> Allowed;
                Allowed.Reserve(Spec.AllowedValues.Num());
                for (const FString& Legal : Spec.AllowedValues)
                {
                    Allowed.Add(MakeShared<FJsonValueString>(Legal));
                }
                Item->SetArrayField(TEXT("allowed_values"), Allowed);
                BadValues.Add(MakeShared<FJsonValueObject>(Item));
            }
        }

        if (MissingRequired.Num() > 0 || BadTypes.Num() > 0 || BadValues.Num() > 0)
        {
            const FString Code = MissingRequired.Num() > 0 ? TEXT("missing_params")
                : (BadTypes.Num() > 0 ? TEXT("bad_param_type") : TEXT("invalid_value"));
            const FString Message = MissingRequired.Num() > 0
                ? FString::Printf(TEXT("%s is missing required parameter(s): %s"),
                    *Entry->Name, *FString::Join(MissingRequired, TEXT(", ")))
                : FString::Printf(TEXT("%s was called with parameter value(s) its declaration rejects"), *Entry->Name);

            TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(Code, Message);
            if (MissingRequired.Num() > 0)
            {
                TArray<TSharedPtr<FJsonValue>> Items;
                for (const FString& Param : MissingRequired)
                {
                    Items.Add(MakeShared<FJsonValueString>(Param));
                }
                Error->SetArrayField(TEXT("missing_params"), Items);
            }
            if (BadTypes.Num() > 0)
            {
                Error->SetArrayField(TEXT("bad_param_types"), BadTypes);
            }
            if (BadValues.Num() > 0)
            {
                Error->SetArrayField(TEXT("invalid_values"), BadValues);
            }

            TArray<TSharedPtr<FJsonValue>> Declared;
            for (const FMCPParamSpec& Spec : Entry->Params)
            {
                TSharedPtr<FJsonObject> Item = MakeShareable(new FJsonObject);
                Item->SetStringField(TEXT("name"), Spec.Name);
                Item->SetStringField(TEXT("type"), Spec.Type);
                Item->SetBoolField(TEXT("required"), Spec.bRequired);
                Declared.Add(MakeShared<FJsonValueObject>(Item));
            }
            Error->SetArrayField(TEXT("declared_params"), Declared);
            Error->SetStringField(TEXT("hint"), FString::Printf(
                TEXT("Nothing ran. Send the parameter(s) named above; list_mcp_commands reports the full "
                     "declaration of '%s' (types, defaults, allowed values)."), *Entry->Name));

            UE_LOG(LogTemp, Warning, TEXT("MCPCommandRegistry: rejected '%s' (%s)"), *Entry->Name, *Code);
            JournalScope.RecordError(Code);
            return Error;
        }

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
        JournalScope.RecordOutcome(Result);
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
    FMCPJournalScope JournalScope(Name, TEXT("unknown"), Params);
    JournalScope.RecordError(TEXT("unknown_command"));
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
