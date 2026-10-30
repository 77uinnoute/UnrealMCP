#pragma once

#include "CoreMinimal.h"
#include "Json.h"
#include "HAL/CriticalSection.h"

/**
 * One executed (or still executing) command.
 *
 * `bFinished == false` is the interesting state: it is the command that was handed to the GameThread
 * and never came back - the shape a wedged editor leaves behind. The rest of the fields exist so a
 * post-mortem does not need the reply that was never delivered.
 */
struct FMCPJournalEntry
{
    uint64 Seq = 0;
    FString Command;
    FString Category;

    /** FPlatformTime::Seconds() when dispatch entered, and how long it stayed (0 while running). */
    double StartedAt = 0.0;
    double DurationMs = 0.0;

    /** Wall clock of the start, so a journal line can be matched against the editor log. */
    FString StartedAtUtc;

    bool bFinished = false;

    /** Empty on success; otherwise the reply's error_code (missing_params, bad_param_type, ...). */
    FString ErrorCode;

    /** Hint of the parameter payload, never its content: size in bytes plus a CRC of the JSON text. */
    int32 ParamsSize = 0;
    FString ParamsHash;
};

/**
 * In-memory ring buffer of the last N commands plus an append-only JSONL mirror.
 *
 * Why: command timing existed in exactly three places (execute_python_command's duration_ms /
 * python_ms, live_coding_compile's duration_ms, live_coding_status' seconds_since_request) and was
 * only ever a field of the reply - a command that timed out, crashed the editor or lost its reply
 * took the measurement with it. Dispatch is the one place every command passes through, so the
 * timing and the outcome are recorded here instead.
 *
 * Read it back with the get_command_journal command; the mirror lives at
 * <project>/Saved/Logs/UnrealMCP_journal.jsonl and survives the editor being killed.
 */
class FMCPCommandJournal
{
public:
    static FMCPCommandJournal& Get();

    /** Records the start of one dispatch. Returns the sequence number to hand back to Finish(). */
    uint64 Begin(const FString& Command, const FString& Category, const TSharedPtr<FJsonObject>& Params);

    /** Completes one entry. Safe to call twice (a second call is ignored) and safe under unwinding. */
    void Finish(uint64 Seq, const FString& ErrorCode);

    /** Newest first, at most Limit entries; Limit <= 0 means "everything held". */
    TArray<FMCPJournalEntry> Snapshot(int32 Limit = 0, const FString& Category = FString()) const;

    /** Per-command aggregate over the ring: calls, total / max / average milliseconds. */
    TArray<TSharedPtr<FJsonObject>> AggregateByCommand() const;

    FString JournalPath() const;

    int32 Capacity() const;
    int32 Count() const;

private:
    FMCPCommandJournal();

    void AppendToFile(const FMCPJournalEntry& Entry);

    mutable FCriticalSection Lock;
    TArray<FMCPJournalEntry> Entries;
    int32 NextIndex = 0;
    bool bFull = false;
    uint64 NextSeq = 1;
    FString MirrorPath;
};
