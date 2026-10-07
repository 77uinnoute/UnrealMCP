#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/** One declared parameter of a command. */
struct FMCPParamSpec
{
    FString Name;

    /** "string" / "int" / "float" / "bool" / "object" / "array". */
    FString Type;

    bool bRequired = false;

    /** Optional default, already as JSON text (e.g. "false", "0"). */
    FString Default;

    /** Optional legal values, for enum-like parameters. */
    TArray<FString> AllowedValues;

    FString Description;
};

/**
 * Policy flags. Each one replaces a hand-written command-name list that used to live
 * somewhere else (see the loopback blacklist and the material graph-mutating list).
 */
struct FMCPCommandFlags
{
    /** Reject over the editor python loopback: these re-enter the GameThread queue / python VM. */
    bool bLoopbackForbidden = false;

    /** Changes a graph or asset, for callers that must know before/after (material compile boundary). */
    bool bMutatesGraph = false;

    /** Protocol / internal: not part of the agent tool surface (still callable and introspectable). */
    bool bHidden = false;

    /**
     * Write the owning blueprint to disk after a successful command (the editor is routinely killed
     * by the build script, so unsaved graph / component edits would be lost). The per-command wrapper
     * that used to consult FUnrealMCPCommonUtils::IsPersistedBlueprintCommand() now reads this.
     */
    bool bPersistAfterSuccess = false;
};

/** One registered command: the only place its name, parameters and policy are declared. */
struct FMCPCommandEntry
{
    FString Name;
    FString Category;
    FString Description;
    TArray<FMCPParamSpec> Params;
    FMCPCommandFlags Flags;
    TFunction<TSharedPtr<FJsonObject>(const TSharedPtr<FJsonObject>&)> Handler;
};

/**
 * Process-wide command table.
 *
 * This file holds the whole contract: a command registered here is dispatchable, introspectable
 * and (through its flags) policy-complete. Callers must not keep a second list of command names.
 *
 * Registration happens while modules start, so the table is a function-local static and collects
 * into a pending array until Seal() builds the lookup index - that way duplicate names are
 * reported with both sides at once instead of silently first-wins.
 */
class UNREALMCP_API FMCPCommandRegistry
{
public:
    static FMCPCommandRegistry& Get();

    /** Queue one command. Duplicates are resolved (and reported) at Seal(). */
    void Register(FMCPCommandEntry Entry);

    /**
     * Drop everything and open the table again. Handlers bind to the domain objects that own
     * them, so a subsystem that is re-created must rebuild the table: a stale binding would
     * point at a destroyed command object.
     */
    void Reset();

    /** Build the lookup index, report conflicts and close the table. */
    void Seal();

    const FMCPCommandEntry* Find(const FString& Name) const;

    /**
     * Run one command by name, synchronously, on the calling thread (GameThread in practice).
     * An unknown name yields a structured result carrying error_code "unknown_command" and the
     * requested name in the message.
     */
    TSharedPtr<FJsonObject> Execute(const FString& Name, const TSharedPtr<FJsonObject>& Params) const;

    /** Every command, in registration order. */
    const TArray<FMCPCommandEntry>& All() const { return Commands; }

    /** Commands of one category; an unknown category yields an empty array. */
    TArray<const FMCPCommandEntry*> ByCategory(const FString& Category) const;

    int32 Num() const { return Commands.Num(); }
    bool IsSealed() const { return bSealed; }

private:
    TArray<FMCPCommandEntry> Commands;
    TMap<FString, int32> NameToIndex;
    bool bSealed = false;
};

/** Required parameter spec. */
inline FMCPParamSpec MCPParam(const FString& Name, const FString& Type, const FString& Description)
{
    FMCPParamSpec Spec;
    Spec.Name = Name;
    Spec.Type = Type;
    Spec.bRequired = true;
    Spec.Description = Description;
    return Spec;
}

/** Optional parameter spec. */
inline FMCPParamSpec MCPParamOpt(const FString& Name, const FString& Type, const FString& Description)
{
    FMCPParamSpec Spec;
    Spec.Name = Name;
    Spec.Type = Type;
    Spec.bRequired = false;
    Spec.Description = Description;
    return Spec;
}

inline FMCPCommandFlags MCPFlags(bool bLoopbackForbidden = false, bool bMutatesGraph = false, bool bHidden = false,
                                 bool bPersistAfterSuccess = false)
{
    FMCPCommandFlags Flags;
    Flags.bLoopbackForbidden = bLoopbackForbidden;
    Flags.bMutatesGraph = bMutatesGraph;
    Flags.bHidden = bHidden;
    Flags.bPersistAfterSuccess = bPersistAfterSuccess;
    return Flags;
}

/**
 * Register one command. Used inside a domain's RegisterCommands(), where its private handler
 * methods are in scope.
 */
#define MCP_REGISTER_COMMAND(Registry, CommandName, CategoryName, DescriptionText, ParamsArray, FlagsValue, HandlerLambda) \
    (Registry).Register(FMCPCommandEntry{ TEXT(CommandName), TEXT(CategoryName), TEXT(DescriptionText), \
        ParamsArray, FlagsValue, HandlerLambda })
