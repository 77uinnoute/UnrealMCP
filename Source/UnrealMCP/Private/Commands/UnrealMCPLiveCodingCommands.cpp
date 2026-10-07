#include "Commands/UnrealMCPLiveCodingCommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Core/MCPCommandRegistry.h"

#include "CoreGlobals.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

#if UNREALMCP_WITH_LIVE_CODING
#include "ILiveCodingModule.h"
#endif

namespace
{
#if UNREALMCP_WITH_LIVE_CODING
    const TCHAR* ResultToString(ELiveCodingCompileResult Result)
    {
        switch (Result)
        {
        case ELiveCodingCompileResult::Success:            return TEXT("success");
        case ELiveCodingCompileResult::NoChanges:          return TEXT("no_changes");
        case ELiveCodingCompileResult::InProgress:         return TEXT("in_progress");
        case ELiveCodingCompileResult::CompileStillActive: return TEXT("compile_still_active");
        case ELiveCodingCompileResult::NotStarted:         return TEXT("not_started");
        case ELiveCodingCompileResult::Cancelled:          return TEXT("cancelled");
        case ELiveCodingCompileResult::Failure:            return TEXT("failure");
        default:                                           return TEXT("unknown");
        }
    }
#endif

    /**
     * Tail of the LogLiveCoding output, recorded only between Begin() and End().
     *
     * This is what makes the non-waiting path usable: ELiveCodingCompileResult is only filled in
     * by a Compile() call that waited, and the engine broadcasts its patch-complete delegate on
     * success only - so after a wait=false request the log is the only place the outcome exists.
     *
     * CanBeUsedOnAnyThread is true on purpose. The compile runs off-thread and logs from there, and
     * a device that answers false has its output buffered for the GameThread - which is exactly the
     * thread that is blocked waiting for that compile, so the lines would only arrive afterwards.
     */
    class FLiveCodingLogSpy : public FOutputDevice
    {
    public:
        void Begin()
        {
            FScopeLock Lock(&CriticalSection);
            Lines.Reset();
            bRecording = true;
        }

        void End()
        {
            FScopeLock Lock(&CriticalSection);
            bRecording = false;
        }

        TArray<FString> Snapshot() const
        {
            FScopeLock Lock(&CriticalSection);
            return Lines;
        }

        virtual bool CanBeUsedOnAnyThread() const override { return true; }

        virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
        {
            FScopeLock Lock(&CriticalSection);
            if (!bRecording || Category != FName(TEXT("LogLiveCoding")))
            {
                return;
            }
            FString Line(V);
            Line.TrimStartAndEndInline();
            if (Line.IsEmpty())
            {
                return;
            }
            Lines.Add(MoveTemp(Line));
            if (Lines.Num() > MaxLines)
            {
                Lines.RemoveAt(0, Lines.Num() - MaxLines);
            }
        }

    private:
        static const int32 MaxLines = 64;

        mutable FCriticalSection CriticalSection;
        TArray<FString> Lines;
        bool bRecording = false;
    };

    FLiveCodingLogSpy& GetLogSpy()
    {
        static FLiveCodingLogSpy Spy;
        return Spy;
    }

    /** Later lines win, so the final verdict of the compile is the one reported. */
    FString ClassifyFromLog(const TArray<FString>& Lines)
    {
        FString Outcome = TEXT("unknown");
        for (const FString& Line : Lines)
        {
            if (Line.Contains(TEXT("no code changes"), ESearchCase::IgnoreCase))
            {
                Outcome = TEXT("no_changes");
            }
            else if (Line.Contains(TEXT("succeeded"), ESearchCase::IgnoreCase))
            {
                Outcome = TEXT("success");
            }
            else if (Line.Contains(TEXT("failed"), ESearchCase::IgnoreCase))
            {
                Outcome = TEXT("failure");
            }
            else if (Line.Contains(TEXT("cancel"), ESearchCase::IgnoreCase))
            {
                Outcome = TEXT("cancelled");
            }
        }
        return Outcome;
    }

    /**
     * A failed patch says nothing in the editor log beyond "see Live console": the compiler and
     * linker diagnostics are written to UnrealBuildTool's own log. That file is the only place the
     * reason exists, so a failure reply has to carry it or the caller is back to guessing.
     */
    FString GetUBTLogPath()
    {
        return FPaths::Combine(FString(FPlatformProcess::UserSettingsDir()), TEXT("UnrealBuildTool/Log.txt"));
    }

    void AddUBTDiagnostics(const TSharedPtr<FJsonObject>& Data, int32 TailLines, int32 MaxErrors)
    {
        TArray<FString> Lines;
        const FString UBTLogPath = GetUBTLogPath();
        Data->SetStringField(TEXT("ubt_log"), UBTLogPath);
        if (!FFileHelper::LoadFileToStringArray(Lines, *UBTLogPath))
        {
            return;
        }

        TArray<TSharedPtr<FJsonValue>> Errors;
        for (const FString& Line : Lines)
        {
            if (Line.Contains(TEXT("error C")) || Line.Contains(TEXT("error LNK")) ||
                Line.Contains(TEXT("error MSB")) || Line.Contains(TEXT(": error ")) ||
                Line.Contains(TEXT("CompilationResultException")))
            {
                Errors.Add(MakeShared<FJsonValueString>(Line));
                if (Errors.Num() >= MaxErrors)
                {
                    break;
                }
            }
        }
        Data->SetArrayField(TEXT("compile_errors"), Errors);

        TArray<TSharedPtr<FJsonValue>> Tail;
        const int32 First = FMath::Max(0, Lines.Num() - TailLines);
        for (int32 Index = First; Index < Lines.Num(); ++Index)
        {
            Tail.Add(MakeShared<FJsonValueString>(Lines[Index]));
        }
        Data->SetArrayField(TEXT("ubt_log_tail"), Tail);
    }
}

FUnrealMCPLiveCodingCommands::FUnrealMCPLiveCodingCommands()
{
    if (GLog)
    {
        GLog->AddOutputDevice(&GetLogSpy());
    }
}

FUnrealMCPLiveCodingCommands::~FUnrealMCPLiveCodingCommands()
{
    // The spy is a function-local static; detach it so a re-created command object does not leave
    // a dangling device behind.
    GetLogSpy().End();
    if (GLog)
    {
        GLog->RemoveOutputDevice(&GetLogSpy());
    }
}

void FUnrealMCPLiveCodingCommands::RegisterCommands(FMCPCommandRegistry& Registry)
{
    MCP_REGISTER_COMMAND(Registry, "live_coding_compile", "editor",
        "Hot reload C++ through Live Coding. wait=true (default) blocks until the patch finishes and returns its outcome (success / no_changes / failure ...); wait=false only starts it - poll live_coding_status after that. The compiling thread is the GameThread, so a waiting call keeps the MCP channel busy for its duration: call it with wait=false when that matters. Data-type changes (new UCLASS / UPROPERTY layout, added classes) still need a full editor restart, not a patch.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("wait"), TEXT("bool"), TEXT("Block until the compile finishes (default true)")),
        }), MCPFlags(),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleLiveCodingCompile(Params); });

    MCP_REGISTER_COMMAND(Registry, "live_coding_status", "editor",
        "Read Live Coding state: whether it is available / started / enabled for this session, whether a compile is running, and the outcome plus the log tail of the last request started with wait=false.",
        (TArray<FMCPParamSpec>{}), MCPFlags(),
        [this](const TSharedPtr<FJsonObject>& Params) { return HandleLiveCodingStatus(Params); });
}

TSharedPtr<FJsonObject> FUnrealMCPLiveCodingCommands::HandleLiveCodingCompile(const TSharedPtr<FJsonObject>& Params)
{
#if UNREALMCP_WITH_LIVE_CODING
    ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
    if (!LiveCoding)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("live_coding_unavailable"),
            TEXT("The LiveCoding module is not loaded. Enable Live Coding in Editor Preferences and restart the editor."));
    }

    if (bRequestPending || LiveCoding->IsCompiling())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("live_coding_busy"),
            TEXT("A compile is already running; poll live_coding_status until is_compiling is false."));
    }

    bool bWait = true;
    if (Params.IsValid())
    {
        Params->TryGetBoolField(TEXT("wait"), bWait);
    }

    FLiveCodingLogSpy& Spy = GetLogSpy();
    Spy.Begin();
    bRequestPending = true;
    RequestStartTime = FPlatformTime::Seconds();
    LastOutcome = TEXT("in_progress");

    ELiveCodingCompileResult CompileResult = ELiveCodingCompileResult::Failure;
    const double StartTime = FPlatformTime::Seconds();
    const bool bAccepted = LiveCoding->Compile(
        bWait ? ELiveCodingCompileFlags::WaitForCompletion : ELiveCodingCompileFlags::None, &CompileResult);
    const double DurationSeconds = FPlatformTime::Seconds() - StartTime;

    const TArray<FString> Lines = Spy.Snapshot();

    FString Outcome;
    if (bWait)
    {
        // The synchronous call is the authority: the enum it filled in is the engine's own verdict.
        Outcome = ResultToString(CompileResult);
        bRequestPending = false;
        Spy.End();
    }
    else
    {
        Outcome = TEXT("in_progress");
    }
    LastOutcome = Outcome;

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("accepted"), bAccepted);
    Data->SetBoolField(TEXT("wait"), bWait);
    Data->SetStringField(TEXT("outcome"), Outcome);
    Data->SetBoolField(TEXT("is_compiling"), LiveCoding->IsCompiling());
    Data->SetNumberField(TEXT("duration_ms"), DurationSeconds * 1000.0);
    Data->SetBoolField(TEXT("has_started"), LiveCoding->HasStarted());
    Data->SetBoolField(TEXT("enabled_for_session"), LiveCoding->IsEnabledForSession());
    Data->SetStringField(TEXT("outcome_source"), bWait ? TEXT("compile_result") : TEXT("pending"));

    TArray<TSharedPtr<FJsonValue>> LogTailJson;
    for (const FString& Line : Lines)
    {
        LogTailJson.Add(MakeShared<FJsonValueString>(Line));
    }
    Data->SetArrayField(TEXT("log_tail"), LogTailJson);

    if (bWait)
    {
        if (Outcome == TEXT("not_started"))
        {
            Data->SetStringField(TEXT("hint"),
                TEXT("Live Coding did not start; check the Live Coding setting, then restart the editor."));
        }
        else if (Outcome == TEXT("failure"))
        {
            AddUBTDiagnostics(Data, /*TailLines=*/15, /*MaxErrors=*/20);
            Data->SetStringField(TEXT("hint"),
                TEXT("The patch failed; compile_errors carries the compiler/linker lines from UnrealBuildTool's "
                     "log (its messages may be in the console codepage, but the file/line/error code are ASCII). "
                     "A module whose object files were built in a throwaway location cannot be patched - build "
                     "it as part of the project's editor target instead."));
        }
    }
    else
    {
        Data->SetStringField(TEXT("hint"),
            TEXT("Compile started; poll live_coding_status until is_compiling is false, then read its outcome."));
    }

    return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
#else
    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("live_coding_unavailable"),
        TEXT("This build has no Live Coding support (it exists on Windows only)."));
#endif
}

TSharedPtr<FJsonObject> FUnrealMCPLiveCodingCommands::HandleLiveCodingStatus(const TSharedPtr<FJsonObject>& Params)
{
#if UNREALMCP_WITH_LIVE_CODING
    ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("available"), LiveCoding != nullptr);

    if (!LiveCoding)
    {
        Data->SetStringField(TEXT("outcome"), LastOutcome);
        Data->SetStringField(TEXT("hint"),
            TEXT("The LiveCoding module is not loaded; enable Live Coding and restart the editor."));
        return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
    }

    const bool bCompiling = LiveCoding->IsCompiling();

    // An outstanding wait=false request ends here: the engine only reports a compile's result to the
    // call that waited, so the captured log is what turns "stopped compiling" into an outcome.
    FString Outcome = LastOutcome;
    FString OutcomeSource = TEXT("compile_result");
    if (bRequestPending && !bCompiling)
    {
        Outcome = ClassifyFromLog(GetLogSpy().Snapshot());
        OutcomeSource = TEXT("log");
        bRequestPending = false;
        GetLogSpy().End();
    }
    else if (bRequestPending)
    {
        OutcomeSource = TEXT("pending");
    }
    LastOutcome = Outcome;

    Data->SetBoolField(TEXT("has_started"), LiveCoding->HasStarted());
    Data->SetBoolField(TEXT("enabled_for_session"), LiveCoding->IsEnabledForSession());
    Data->SetBoolField(TEXT("can_enable"), LiveCoding->CanEnableForSession());
    Data->SetBoolField(TEXT("is_compiling"), bCompiling);
    Data->SetBoolField(TEXT("request_pending"), bRequestPending);
    if (RequestStartTime > 0.0)
    {
        Data->SetNumberField(TEXT("seconds_since_request"), FPlatformTime::Seconds() - RequestStartTime);
    }
    Data->SetStringField(TEXT("outcome"), Outcome);
    Data->SetStringField(TEXT("outcome_source"), OutcomeSource);

    // Both failure routes end here, so the diagnostics are attached in one place.
    if (Outcome == TEXT("failure"))
    {
        AddUBTDiagnostics(Data, /*TailLines=*/15, /*MaxErrors=*/20);
    }

    TArray<TSharedPtr<FJsonValue>> StatusLogTailJson;
    for (const FString& Line : GetLogSpy().Snapshot())
    {
        StatusLogTailJson.Add(MakeShared<FJsonValueString>(Line));
    }
    Data->SetArrayField(TEXT("log_tail"), StatusLogTailJson);

    return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
#else
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("available"), false);
    Data->SetStringField(TEXT("outcome"), TEXT("none"));
    Data->SetStringField(TEXT("hint"), TEXT("This build has no Live Coding support (it exists on Windows only)."));
    return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
#endif
}
