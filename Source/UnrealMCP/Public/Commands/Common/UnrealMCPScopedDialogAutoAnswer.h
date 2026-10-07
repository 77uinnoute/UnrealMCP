#pragma once

#include "CoreMinimal.h"
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Templates/UnrealTemplate.h"

/**
 * Answer the engine's modal dialogs inside a scope, and record every answer.
 *
 * WHY A MODAL IS WORSE THAN A FAILURE
 *
 * FMessageDialog::Open builds a nested Slate loop: it holds the GameThread while it waits, and the
 * bridge dispatches commands on that same thread - so one unexpected confirmation box stops the
 * whole MCP channel until a human clicks it. Any command that reaches an engine path which can
 * prompt therefore has to decide, in advance, what happens to that box.
 *
 * THE TWO AVAILABLE DECISIONS
 *
 *   unattended defaults
 *       TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true) makes
 *       FMessageDialog::Open return its default value without building a window. Nothing can hang,
 *       but the answer is whatever the engine chose, and for a confirmation that default is usually
 *       the one that ABORTS the operation (measured: the CDO-reference box behind
 *       IAssetTools::RenameAssets). The command must then report the failure - it must not report
 *       success just because it did not hang.
 *
 *   auto-answer (this scope)
 *       A dialog is still answered by us, but affirmatively and on the record: every answer lands in
 *       the response as auto_answered_dialogs[], so what the command clicked on the user's behalf is
 *       visible. This is the only way to express "continue" - FMessageDialog::Open ignores the
 *       delegate entirely while GIsRunningUnattendedScript is true, which is why the guard above has
 *       to be relaxed to false first.
 *
 * Callers pick one per command invocation; neither is a silent default beyond what the receipt shows.
 *
 * BOUNDARIES
 *
 *   - An editor started with -unattended cannot be helped: FApp::IsUnattended() makes
 *     FMessageDialog::Open ignore the delegate, so nothing is consulted. CanAnswer() reports that
 *     before a caller relies on it.
 *   - A Win32 blocking box (FPlatformMisc::MessageBoxExt) is not intercepted; the engine paths
 *     driven here do not use it.
 *   - NOT every engine modal travels through this delegate. Measured: the dialog behind
 *     FBlueprintEditorUtils::ChangeMemberVariableType on an already-referenced variable does NOT -
 *     arming this scope around that call left the editor frozen, and a human had to click the box
 *     (Docs/MCP_Findings_2026-10-06_platformer-round.md section 3). So this is a tool for a path
 *     whose dialog is known to be a FMessageDialog, not a general "no modal can block me" switch:
 *     where the box is not known, the containment has to be a preflight refusal instead.
 *   - Answering affirmatively is a real decision: an auto-checkout prompt is answered with "check
 *     it out". Only use the scope on a path whose reachable dialogs have been identified.
 *
 * ENGINE VERSION
 *
 * UE 5.7 only. FCoreDelegates::ModalMessageDialog is the four-argument form
 * (EAppMsgCategory, EAppMsgType::Type, Message, Title); before 5.3 it was ModalErrorMessage with
 * three. Code that spans versions has to probe both members - this plugin targets one engine
 * version, so it binds the one that exists (see CoreDelegates.h:248).
 */
class UNREALMCP_API FUnrealMCPScopedDialogAutoAnswer
{
public:
    struct FAnswered
    {
        FString Type;
        FString Title;
        FString Message;
        FString Answer;
    };

    FUnrealMCPScopedDialogAutoAnswer()
        : UnattendedGuard(GIsRunningUnattendedScript, false)
    {
        SavedDelegate = FCoreDelegates::ModalMessageDialog;
        FCoreDelegates::ModalMessageDialog.BindLambda(
            [this](EAppMsgCategory /*Category*/, EAppMsgType::Type Type, const FText& Message, const FText& Title)
            {
                return Answer(Type, Message, Title);
            });
    }

    ~FUnrealMCPScopedDialogAutoAnswer()
    {
        FCoreDelegates::ModalMessageDialog = SavedDelegate;
    }

    FUnrealMCPScopedDialogAutoAnswer(const FUnrealMCPScopedDialogAutoAnswer&) = delete;
    FUnrealMCPScopedDialogAutoAnswer& operator=(const FUnrealMCPScopedDialogAutoAnswer&) = delete;

    /** False when the editor itself runs -unattended: no dialog consults the delegate there. */
    static bool CanAnswer() { return !FApp::IsUnattended(); }

    const TArray<FAnswered>& Answered() const { return Dialogs; }

    /** The `auto_answered_dialogs` array of a response. */
    TArray<TSharedPtr<FJsonValue>> ToJson() const
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const FAnswered& Dialog : Dialogs)
        {
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("type"), Dialog.Type);
            Obj->SetStringField(TEXT("title"), Dialog.Title);
            Obj->SetStringField(TEXT("message"), Dialog.Message);
            Obj->SetStringField(TEXT("answer"), Dialog.Answer);
            Out.Add(MakeShared<FJsonValueObject>(Obj));
        }
        return Out;
    }

    /** Called by the delegate handler; public because that handler is a free lambda. */
    EAppReturnType::Type Answer(EAppMsgType::Type Type, const FText& Message, const FText& Title)
    {
        const EAppReturnType::Type Result = AffirmativeFor(Type);
        FAnswered Record;
        Record.Type = MsgTypeName(Type);
        Record.Title = Title.ToString();
        Record.Message = Message.ToString();
        Record.Answer = ReturnTypeName(Result);
        Dialogs.Add(Record);
        return Result;
    }

    /** The "continue" button of every button set the engine can put up. */
    static EAppReturnType::Type AffirmativeFor(EAppMsgType::Type Type)
    {
        switch (Type)
        {
        case EAppMsgType::Ok:                     return EAppReturnType::Ok;
        case EAppMsgType::YesNo:                  return EAppReturnType::Yes;
        case EAppMsgType::OkCancel:               return EAppReturnType::Ok;
        case EAppMsgType::YesNoCancel:            return EAppReturnType::Yes;
        case EAppMsgType::CancelRetryContinue:    return EAppReturnType::Continue;
        case EAppMsgType::YesNoYesAllNoAll:       return EAppReturnType::Yes;
        case EAppMsgType::YesNoYesAllNoAllCancel: return EAppReturnType::Yes;
        case EAppMsgType::YesNoYesAll:            return EAppReturnType::Yes;
        default:                                  return EAppReturnType::Ok;
        }
    }

    static const TCHAR* MsgTypeName(EAppMsgType::Type Type)
    {
        switch (Type)
        {
        case EAppMsgType::Ok:                     return TEXT("Ok");
        case EAppMsgType::YesNo:                  return TEXT("YesNo");
        case EAppMsgType::OkCancel:               return TEXT("OkCancel");
        case EAppMsgType::YesNoCancel:            return TEXT("YesNoCancel");
        case EAppMsgType::CancelRetryContinue:    return TEXT("CancelRetryContinue");
        case EAppMsgType::YesNoYesAllNoAll:       return TEXT("YesNoYesAllNoAll");
        case EAppMsgType::YesNoYesAllNoAllCancel: return TEXT("YesNoYesAllNoAllCancel");
        case EAppMsgType::YesNoYesAll:            return TEXT("YesNoYesAll");
        default:                                  return TEXT("Unknown");
        }
    }

    static const TCHAR* ReturnTypeName(EAppReturnType::Type Type)
    {
        switch (Type)
        {
        case EAppReturnType::No:       return TEXT("No");
        case EAppReturnType::Yes:      return TEXT("Yes");
        case EAppReturnType::YesAll:   return TEXT("YesAll");
        case EAppReturnType::NoAll:    return TEXT("NoAll");
        case EAppReturnType::Cancel:   return TEXT("Cancel");
        case EAppReturnType::Ok:       return TEXT("Ok");
        case EAppReturnType::Retry:    return TEXT("Retry");
        case EAppReturnType::Continue: return TEXT("Continue");
        default:                       return TEXT("Unknown");
        }
    }

private:
    TArray<FAnswered> Dialogs;

    /** Relaxed before the delegate is bound: FMessageDialog::Open only consults it when this is false. */
    TGuardValue<bool> UnattendedGuard;

    decltype(FCoreDelegates::ModalMessageDialog) SavedDelegate;
};
