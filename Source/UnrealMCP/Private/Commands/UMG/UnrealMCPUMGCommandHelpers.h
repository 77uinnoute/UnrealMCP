#pragma once

#include "CoreMinimal.h"
#include "Json.h"
#include "Layout/Margin.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelSlot.h"
#include "WidgetBlueprint.h"

class FWidgetBlueprintEditor;

/**
 * Helpers shared by the UMG command files (this domain used to be one 5000-line file).
 * Everything here is used by at least two of the Commands/UMG/*.cpp files; the per-topic helpers
 * live in the anonymous namespace of the file that owns them.
 */
namespace UnrealMCPUMG
{
	/** A graph node that refers to a widget or a binding artifact. */
	struct FWidgetReferenceInfo
	{
		FString Kind;
		FString Graph;
		FString Node;
		FString Detail;
	};

	// --- JSON shape of the read-back commands -------------------------------------------------
	TSharedPtr<FJsonValue> JNum(double Value);
	FString EnumLabel(UEnum* Enum, int64 Value);
	TSharedPtr<FJsonObject> MarginToJson(const FMargin& Margin);
	TSharedPtr<FJsonObject> SlotToJson(UPanelSlot* Slot);
	void AddEnumProperty(const TSharedPtr<FJsonObject>& Out, UObject* Object, const TCHAR* PropertyName,
	                     const TCHAR* Key);
	TSharedPtr<FJsonObject> WidgetPropsToJson(UWidget* Widget);

	// --- addressing and errors ----------------------------------------------------------------
	/** Python name / C++ name / asset path -> a class derived from RequiredBase (nullptr + candidates when unknown). */
	FString NormalizeWidgetClassName(const FString& Raw);
	void GatherWidgetClassCandidates(UClass* RequiredBase, TArray<FString>& OutCandidates);
	UClass* ResolveWidgetClassByName(const FString& Raw, UClass* RequiredBase, TArray<FString>& OutCandidates);

	TSharedPtr<FJsonObject> MakeListError(const FString& Code, const FString& Message, const TCHAR* Field,
	                                      const TArray<FString>& Values);
	void GatherWidgetNames(UWidgetTree* Tree, TArray<FString>& OutNames);
	void GatherPanelNames(UWidgetTree* Tree, TArray<FString>& OutNames);
	UWidgetBlueprint* LoadWidgetBlueprintByParam(const FString& Name, TSharedPtr<FJsonObject>& OutError);
	UWidget* FindWidgetOrError(UWidgetBlueprint* Blueprint, const FString& WidgetName,
	                           const FString& BlueprintName, TSharedPtr<FJsonObject>& OutError);
	UPanelWidget* ResolveParentPanel(UWidgetBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params,
	                                 FString& OutParentName, TSharedPtr<FJsonObject>& OutError);

	// --- designer session -----------------------------------------------------------------------
	/** The live designer session for a widget blueprint, or nullptr (never guesses a toolkit). */
	::FWidgetBlueprintEditor* FindWidgetBlueprintEditor(UWidgetBlueprint* Blueprint);

	// --- write收口 (compile + persist + counts) ------------------------------------------------
	void AddWidgetListToJson(const TSharedPtr<FJsonObject>& Result, UWidgetTree* Tree);
	void FinishWidgetEdit(UWidgetBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params,
	                      const TSharedPtr<FJsonObject>& Result);

	/**
	 * The mutation boundary plus the persist receipt (persist_requested / saved) that every UMG write
	 * command owes its caller. Call once, after the write has been applied and before returning.
	 */
	bool FinishWidgetWrite(UWidgetBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params,
	                       const TSharedPtr<FJsonObject>& Result);

	/** Fill the persist receipt on responses that did not report one (error paths); returns the response. */
	TSharedPtr<FJsonObject> EnsurePersistReceipt(const TSharedPtr<FJsonObject>& Params,
	                                             const TSharedPtr<FJsonObject>& Result);

	/** The boundary `get_umg_compile_errors` splits "this compile" against, when a mutation was seen. */
	void RecordUMGMutation(UWidgetBlueprint* Blueprint);
	bool FindUMGMutationTime(const UWidgetBlueprint* Blueprint, FDateTime& OutTime);
	bool TryParseSince(const TSharedPtr<FJsonValue>& Value, FDateTime& OutTime);

	// --- compile diagnostics -------------------------------------------------------------------
	/** One captured compiler message: severity, text, and the node it locates when there is one. */
	struct FUMGCompileMessage
	{
		FString Severity;
		FString Message;
		FString NodeId;
		FString NodeName;
		FString GraphName;
	};

	/** Compile with a results log and keep what it said (severity / text / node via token scan). */
	bool CompileWidgetWithMessages(UWidgetBlueprint* Blueprint, TArray<FUMGCompileMessage>& OutMessages,
	                               int32& OutErrors, int32& OutWarnings);
	FString BlueprintStatusLabel(const UWidgetBlueprint* Blueprint);
	TSharedPtr<FJsonObject> CompileMessageToJson(const FUMGCompileMessage& Message);

	// --- binding reconciliation (editor table vs the table the widget applies) ------------------
	bool CompileWithMessages(UWidgetBlueprint* Blueprint, TArray<FString>& OutMessages);
	bool RuntimeBindingMatches(UWidgetBlueprint* Blueprint, const FString& WidgetName,
	                           const FString& PropertyName, TArray<TSharedPtr<FJsonObject>>* OutRuntimeBindings);
	TArray<TSharedPtr<FJsonValue>> BindingsToJsonArray(UWidgetBlueprint* Blueprint, bool bIncludeEffective);
}

using namespace UnrealMCPUMG;
