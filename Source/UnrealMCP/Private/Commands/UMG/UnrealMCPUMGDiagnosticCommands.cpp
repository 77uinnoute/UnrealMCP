// UMG compile diagnostics: what the widget compiler said, and whether the bindings the editor
// holds are the ones the widget will actually apply at runtime.
#include "Commands/UnrealMCPUMGCommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/Widget.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Commands/UMG/UnrealMCPUMGCommandHelpers.h"

namespace
{
	/** The runtime table, i.e. the bindings the widget applies (an editor entry alone proves nothing). */
	void GatherRuntimeBindings(UWidgetBlueprint* Blueprint, TArray<TSharedPtr<FJsonObject>>& OutBindings)
	{
		OutBindings.Reset();
		RuntimeBindingMatches(Blueprint, FString(), FString(), &OutBindings);
	}

	bool HasRuntimeBinding(const TArray<TSharedPtr<FJsonObject>>& RuntimeBindings, const FString& Widget,
	                       const FString& Property)
	{
		for (const TSharedPtr<FJsonObject>& Runtime : RuntimeBindings)
		{
			FString RuntimeWidget;
			FString RuntimeProperty;
			Runtime->TryGetStringField(TEXT("widget"), RuntimeWidget);
			Runtime->TryGetStringField(TEXT("property"), RuntimeProperty);
			if (RuntimeWidget == Widget && RuntimeProperty == Property)
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * The editor binding table minus the runtime one: entries the engine accepted when the designer
	 * wrote them but dropped when it compiled. The message is a best-effort match (the compiler names
	 * the property or the widget when it refuses a binding) - never invented when nothing matches.
	 */
	void GatherDroppedBindings(UWidgetBlueprint* Blueprint, const TArray<FUMGCompileMessage>& Messages,
	                           const TArray<TSharedPtr<FJsonObject>>& RuntimeBindings,
	                           TArray<TSharedPtr<FJsonValue>>& OutDropped)
	{
		OutDropped.Reset();
		if (!Blueprint)
		{
			return;
		}
		for (const FDelegateEditorBinding& Binding : Blueprint->Bindings)
		{
			const FString PropertyName = Binding.PropertyName.ToString();
			if (HasRuntimeBinding(RuntimeBindings, Binding.ObjectName, PropertyName))
			{
				continue;
			}

			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("widget"), Binding.ObjectName);
			Item->SetStringField(TEXT("property"), PropertyName);
			Item->SetStringField(TEXT("function"), Binding.FunctionName.ToString());

			FString Matched;
			for (const FUMGCompileMessage& Message : Messages)
			{
				const bool bNamesProperty = !PropertyName.IsEmpty() && Message.Message.Contains(PropertyName);
				const bool bNamesWidget = !Binding.ObjectName.IsEmpty() && Message.Message.Contains(Binding.ObjectName);
				if (bNamesProperty || bNamesWidget)
				{
					Matched = Message.Message;
					break;
				}
			}
			Item->SetBoolField(TEXT("message_matched"), !Matched.IsEmpty());
			Item->SetStringField(TEXT("message"), Matched);
			OutDropped.Add(MakeShared<FJsonValueObject>(Item));
		}
	}
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleGetUMGCompileErrors(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'blueprint_name' parameter"));
	}

	TSharedPtr<FJsonObject> Error;
	UWidgetBlueprint* Blueprint = LoadWidgetBlueprintByParam(BlueprintName, Error);
	if (!Blueprint)
	{
		return Error;
	}

	// The messages must come from a results log, not from grepping the editor log: the compiler's
	// log is where FDelegateEditorBinding::IsBindingValid writes its refusal.
	TArray<FUMGCompileMessage> Messages;
	int32 NumErrors = 0;
	int32 NumWarnings = 0;
	const bool bCompiled = CompileWidgetWithMessages(Blueprint, Messages, NumErrors, NumWarnings);

	FDateTime SinceBoundary;
	FString BoundarySource = TEXT("unknown");
	if (TryParseSince(Params->TryGetField(TEXT("since")), SinceBoundary))
	{
		BoundarySource = TEXT("explicit_since");
	}
	if (BoundarySource == TEXT("unknown") && FindUMGMutationTime(Blueprint, SinceBoundary))
	{
		BoundarySource = TEXT("umg_mutation");
	}

	// Everything captured was said by the compile that just ran, so a known boundary puts all of it
	// in "this compile"; without one there is nothing honest to split against.
	const bool bThisCompile = BoundarySource != TEXT("unknown") && FDateTime::UtcNow() >= SinceBoundary;

	TArray<TSharedPtr<FJsonValue>> AllMessages;
	TArray<TSharedPtr<FJsonValue>> ThisCompile;
	TArray<TSharedPtr<FJsonValue>> Historical;
	for (const FUMGCompileMessage& Message : Messages)
	{
		TSharedPtr<FJsonObject> Item = CompileMessageToJson(Message);
		AllMessages.Add(MakeShared<FJsonValueObject>(Item));
		if (bThisCompile)
		{
			ThisCompile.Add(MakeShared<FJsonValueObject>(Item));
		}
		else
		{
			Historical.Add(MakeShared<FJsonValueObject>(Item));
		}
	}

	TArray<TSharedPtr<FJsonObject>> RuntimeBindings;
	GatherRuntimeBindings(Blueprint, RuntimeBindings);

	// Reconciling the two tables only means something once the generated class exists: with no
	// generated class every editor entry would look dropped, which would be a lie about the asset.
	const bool bReconciled = Blueprint->GeneratedClass != nullptr;
	TArray<TSharedPtr<FJsonValue>> Dropped;
	if (bReconciled)
	{
		GatherDroppedBindings(Blueprint, Messages, RuntimeBindings, Dropped);
	}

	TSharedPtr<FJsonObject> BindingSummary = MakeShared<FJsonObject>();
	BindingSummary->SetNumberField(TEXT("editor_count"), Blueprint->Bindings.Num());
	BindingSummary->SetNumberField(TEXT("runtime_count"), RuntimeBindings.Num());

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
	Result->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Result->SetBoolField(TEXT("compiled"), bCompiled);
	Result->SetStringField(TEXT("status"), BlueprintStatusLabel(Blueprint));
	Result->SetNumberField(TEXT("status_code"), (int32)Blueprint->Status);
	Result->SetNumberField(TEXT("num_errors"), NumErrors);
	Result->SetNumberField(TEXT("num_warnings"), NumWarnings);
	Result->SetArrayField(TEXT("messages"), AllMessages);
	Result->SetArrayField(TEXT("log_this_compile"), ThisCompile);
	Result->SetArrayField(TEXT("log_historical"), Historical);
	Result->SetStringField(TEXT("boundary_source"), BoundarySource);
	Result->SetStringField(TEXT("since_boundary"),
		BoundarySource == TEXT("unknown") ? FString() : SinceBoundary.ToIso8601());
	Result->SetObjectField(TEXT("binding_summary"), BindingSummary);
	Result->SetBoolField(TEXT("binding_reconciled"), bReconciled);
	Result->SetArrayField(TEXT("bindings_dropped"), Dropped);
	// The command compiles to capture the messages - say so instead of looking like a pure read.
	Result->SetBoolField(TEXT("compile_performed"), true);
	Result->SetStringField(TEXT("hint"),
		TEXT("status/messages are the authority on the current compile state; log_this_compile vs log_historical follows boundary_source. bindings_dropped lists editor bindings the compile did not carry into the runtime table - this command reports them, it never repairs them."));
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleCompileUMGWidget(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'blueprint_name' parameter"));
	}

	TSharedPtr<FJsonObject> Error;
	UWidgetBlueprint* Blueprint = LoadWidgetBlueprintByParam(BlueprintName, Error);
	if (!Blueprint)
	{
		return Error;
	}

	bool bForceFull = true;
	Params->TryGetBoolField(TEXT("force_full"), bForceFull);
	if (bForceFull)
	{
		// MarkBlueprintAsStructurallyModified is the path that also re-runs the editor -> runtime
		// binding copy together with its validity checks; it is not an engine "full compile" switch.
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}

	TArray<FUMGCompileMessage> Messages;
	int32 NumErrors = 0;
	int32 NumWarnings = 0;
	const bool bCompiled = CompileWidgetWithMessages(Blueprint, Messages, NumErrors, NumWarnings);

	TArray<TSharedPtr<FJsonValue>> MessageItems;
	for (const FUMGCompileMessage& Message : Messages)
	{
		MessageItems.Add(MakeShared<FJsonValueObject>(CompileMessageToJson(Message)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
	Result->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Result->SetBoolField(TEXT("force_full"), bForceFull);
	Result->SetBoolField(TEXT("compiled"), bCompiled);
	Result->SetStringField(TEXT("status"), BlueprintStatusLabel(Blueprint));
	Result->SetNumberField(TEXT("num_errors"), NumErrors);
	Result->SetNumberField(TEXT("num_warnings"), NumWarnings);
	Result->SetArrayField(TEXT("messages"), MessageItems);

	// Compiling is not a structural write, so no mutation boundary is recorded here; the receipt is
	// still owed because this command writes the asset when persist is on.
	const bool bRequested = FUnrealMCPCommonUtils::IsPersistRequested(Params);
	Result->SetBoolField(TEXT("saved"), bRequested && FUnrealMCPCommonUtils::SaveAssetForObject(Blueprint));
	Result->SetBoolField(TEXT("persist_requested"), bRequested);
	return Result;
}
