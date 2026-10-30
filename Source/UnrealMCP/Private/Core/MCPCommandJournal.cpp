#include "Core/MCPCommandJournal.h"

#include "Algo/Reverse.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Crc.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
    const int32 JournalCapacity = 128;

    FString SerializeParams(const TSharedPtr<FJsonObject>& Params)
    {
        if (!Params.IsValid())
        {
            return FString();
        }
        FString Text;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
        FJsonSerializer::Serialize(Params.ToSharedRef(), Writer);
        return Text;
    }

    FString EscapeJson(const FString& Value)
    {
        FString Out = Value;
        Out.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
        Out.ReplaceInline(TEXT("\""), TEXT("\\\""));
        Out.ReplaceInline(TEXT("\r"), TEXT(""));
        Out.ReplaceInline(TEXT("\n"), TEXT(" "));
        return Out;
    }
}

FMCPCommandJournal& FMCPCommandJournal::Get()
{
    static FMCPCommandJournal Instance;
    return Instance;
}

FMCPCommandJournal::FMCPCommandJournal()
{
    MirrorPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs"), TEXT("UnrealMCP_journal.jsonl"));
    FPaths::CollapseRelativeDirectories(MirrorPath);
    Entries.SetNum(JournalCapacity);
}

int32 FMCPCommandJournal::Capacity() const
{
    return JournalCapacity;
}

int32 FMCPCommandJournal::Count() const
{
    FScopeLock ScopeLock(&Lock);
    int32 Total = 0;
    for (const FMCPJournalEntry& Entry : Entries)
    {
        if (Entry.Seq != 0)
        {
            ++Total;
        }
    }
    return Total;
}

FString FMCPCommandJournal::JournalPath() const
{
    return MirrorPath;
}

uint64 FMCPCommandJournal::Begin(const FString& Command, const FString& Category,
                                 const TSharedPtr<FJsonObject>& Params)
{
    const FString ParamsText = SerializeParams(Params);

    FMCPJournalEntry Entry;
    Entry.Command = Command;
    Entry.Category = Category;
    Entry.StartedAt = FPlatformTime::Seconds();
    const FDateTime Now = FDateTime::UtcNow();
    Entry.StartedAtUtc = FString::Printf(TEXT("%s.%03dZ"),
        *Now.ToString(TEXT("%Y-%m-%dT%H:%M:%S")), Now.GetMillisecond());
    Entry.ParamsSize = ParamsText.Len();
    Entry.ParamsHash = FString::Printf(TEXT("%08x"),
        FCrc::StrCrc32(*FString::Printf(TEXT("%s|%s"), *Command, *ParamsText)));

    FScopeLock ScopeLock(&Lock);
    Entry.Seq = NextSeq++;
    Entries[NextIndex] = Entry;
    NextIndex = (NextIndex + 1) % JournalCapacity;
    if (NextIndex == 0)
    {
        bFull = true;
    }
    return Entry.Seq;
}

void FMCPCommandJournal::Finish(uint64 Seq, const FString& ErrorCode)
{
    FString Line;
    {
        FScopeLock ScopeLock(&Lock);
        for (FMCPJournalEntry& Entry : Entries)
        {
            if (Entry.Seq != Seq)
            {
                continue;
            }
            if (Entry.bFinished)
            {
                return;
            }
            Entry.bFinished = true;
            Entry.DurationMs = (FPlatformTime::Seconds() - Entry.StartedAt) * 1000.0;
            Entry.ErrorCode = ErrorCode;

            Line = FString::Printf(
                TEXT("{\"seq\":%llu,\"command\":\"%s\",\"category\":\"%s\",\"started\":\"%s\","
                     "\"duration_ms\":%.3f,\"finished\":true,\"error_code\":\"%s\","
                     "\"params_size\":%d,\"params_hash\":\"%s\"}"),
                Entry.Seq, *EscapeJson(Entry.Command), *EscapeJson(Entry.Category), *Entry.StartedAtUtc,
                Entry.DurationMs, *EscapeJson(Entry.ErrorCode), Entry.ParamsSize, *Entry.ParamsHash);
            break;
        }
    }

    if (!Line.IsEmpty())
    {
        // Written after the timing is taken, so the append can never show up in its own duration.
        FFileHelper::SaveStringToFile(Line + TEXT("\n"), *MirrorPath,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
    }
}

TArray<FMCPJournalEntry> FMCPCommandJournal::Snapshot(int32 Limit, const FString& Category) const
{
    TArray<FMCPJournalEntry> Found;
    {
        FScopeLock ScopeLock(&Lock);
        for (const FMCPJournalEntry& Entry : Entries)
        {
            if (Entry.Seq == 0)
            {
                continue;
            }
            if (!Category.IsEmpty() && Entry.Category != Category)
            {
                continue;
            }
            Found.Add(Entry);
        }
    }

    Found.Sort([](const FMCPJournalEntry& A, const FMCPJournalEntry& B) { return A.Seq < B.Seq; });

    if (Limit > 0 && Found.Num() > Limit)
    {
        Found.RemoveAt(0, Found.Num() - Limit);
    }

    // Newest first: the caller is almost always asking "what just happened".
    Algo::Reverse(Found);
    return Found;
}

TArray<TSharedPtr<FJsonObject>> FMCPCommandJournal::AggregateByCommand() const
{
    struct FStats
    {
        int32 Calls = 0;
        double TotalMs = 0.0;
        double MaxMs = 0.0;
    };

    TMap<FString, FStats> ByCommand;
    {
        FScopeLock ScopeLock(&Lock);
        for (const FMCPJournalEntry& Entry : Entries)
        {
            if (Entry.Seq == 0 || !Entry.bFinished)
            {
                continue;
            }
            FStats& Stats = ByCommand.FindOrAdd(Entry.Command);
            Stats.Calls += 1;
            Stats.TotalMs += Entry.DurationMs;
            Stats.MaxMs = FMath::Max(Stats.MaxMs, Entry.DurationMs);
        }
    }

    TArray<TSharedPtr<FJsonObject>> Out;
    for (const TPair<FString, FStats>& Pair : ByCommand)
    {
        TSharedPtr<FJsonObject> Item = MakeShareable(new FJsonObject);
        Item->SetStringField(TEXT("command"), Pair.Key);
        Item->SetNumberField(TEXT("calls"), Pair.Value.Calls);
        Item->SetNumberField(TEXT("total_ms"), Pair.Value.TotalMs);
        Item->SetNumberField(TEXT("max_ms"), Pair.Value.MaxMs);
        Item->SetNumberField(TEXT("avg_ms"), Pair.Value.Calls > 0 ? Pair.Value.TotalMs / Pair.Value.Calls : 0.0);
        Out.Add(Item);
    }

    Out.Sort([](const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B)
    {
        return A->GetNumberField(TEXT("max_ms")) > B->GetNumberField(TEXT("max_ms"));
    });
    return Out;
}
