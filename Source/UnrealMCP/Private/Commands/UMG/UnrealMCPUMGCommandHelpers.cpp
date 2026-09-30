// Shared UMG command helpers - see UnrealMCPUMGCommandHelpers.h for why they are not static.
// This file's own header comes first: the engine's include-order check requires it.
#include "Commands/UMG/UnrealMCPUMGCommandHelpers.h"
#include "Commands/UnrealMCPUMGCommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Components/TextBlock.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Image.h"
#include "Components/ProgressBar.h"
#include "Components/ScaleBoxSlot.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBoxSlot.h"
#include "Components/ScrollBoxSlot.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/Button.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/UObjectToken.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "WidgetBlueprintEditor.h"
#include "Components/GridSlot.h"
#include "Components/Widget.h"
#include "Components/SlateWrapperTypes.h"
#include "UObject/UnrealType.h"

namespace UnrealMCPUMG
{
	TSharedPtr<FJsonValue> JNum(double Value)
	{
		return MakeShared<FJsonValueNumber>(Value);
	}

	FString EnumLabel(UEnum* Enum, int64 Value)
	{
		return Enum ? Enum->GetNameStringByValue(Value) : FString();
	}

	FString NormalizeWidgetClassName(const FString& Raw)
	{
		FString Name = Raw.TrimStartAndEnd();
		int32 Separator = INDEX_NONE;
		if (Name.FindLastChar(TEXT('/'), Separator))
		{
			Name = Name.Mid(Separator + 1);
		}
		if (Name.StartsWith(TEXT("unreal."), ESearchCase::CaseSensitive))
		{
			Name.RightChopInline(7);
		}
		while (Name.EndsWith(TEXT("_C"), ESearchCase::CaseSensitive))
		{
			Name.LeftChopInline(2);
		}
		if (Name.Len() > 1 && Name[0] == TEXT('U') && FChar::IsUpper(Name[1]))
		{
			Name.RightChopInline(1);
		}
		if (Name.FindLastChar(TEXT('.'), Separator))
		{
			Name = Name.Mid(Separator + 1);
		}
		return Name;
	}

	/** Every native class derived from RequiredBase, as candidates for a caller who mistyped the name. */
	void GatherWidgetClassCandidates(UClass* RequiredBase, TArray<FString>& OutCandidates)
	{
		OutCandidates.Reset();
		if (!RequiredBase)
		{
			return;
		}
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (!Class || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				continue;
			}
			if (Class->ClassGeneratedBy || !Class->IsChildOf(RequiredBase))
			{
				continue;
			}
			const FString ClassName = Class->GetName();
			if (!ClassName.StartsWith(TEXT("U"), ESearchCase::CaseSensitive))
			{
				continue;
			}
			OutCandidates.AddUnique(ClassName.Mid(1));
		}
		OutCandidates.Sort();
	}

	UClass* ResolveWidgetClassByName(const FString& Raw, UClass* RequiredBase, TArray<FString>& OutCandidates)
	{
		OutCandidates.Reset();
		const FString Trimmed = Raw.TrimStartAndEnd();
		if (!RequiredBase || Trimmed.IsEmpty())
		{
			GatherWidgetClassCandidates(RequiredBase, OutCandidates);
			return nullptr;
		}

		// An explicit asset path: a widget blueprint contributes its generated class.
		if (Trimmed.Contains(TEXT("/")))
		{
			if (UObject* Asset = FUnrealMCPCommonUtils::FindAsset(Trimmed))
			{
				if (UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Asset))
				{
					UClass* Generated = WidgetBlueprint->GeneratedClass;
					return (Generated && Generated->IsChildOf(RequiredBase)) ? Generated : nullptr;
				}
				if (UClass* DirectClass = Cast<UClass>(Asset))
				{
					return DirectClass->IsChildOf(RequiredBase) ? DirectClass : nullptr;
				}
			}
			if (UClass* Loaded = FindObject<UClass>(nullptr, *Trimmed))
			{
				return Loaded->IsChildOf(RequiredBase) ? Loaded : nullptr;
			}
			GatherWidgetClassCandidates(RequiredBase, OutCandidates);
			return nullptr;
		}

		const FString Name = NormalizeWidgetClassName(Trimmed);
		for (const TCHAR* Module : { TEXT("UMG"), TEXT("UMGEditor"), TEXT("Engine") })
		{
			if (UClass* Found = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/%s.%s"), Module, *Name)))
			{
				if (Found->IsChildOf(RequiredBase))
				{
					return Found;
				}
			}
		}
		// Blueprint classes (including user widgets) live under "<Name>_C".
		if (UClass* Found = FindObject<UClass>(nullptr, *FString::Printf(TEXT("%s_C"), *Name)))
		{
			if (Found->IsChildOf(RequiredBase))
			{
				return Found;
			}
		}

		GatherWidgetClassCandidates(RequiredBase, OutCandidates);
		return nullptr;
	}

	TSharedPtr<FJsonObject> MarginToJson(const FMargin& Margin)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("left"), Margin.Left);
		Obj->SetNumberField(TEXT("top"), Margin.Top);
		Obj->SetNumberField(TEXT("right"), Margin.Right);
		Obj->SetNumberField(TEXT("bottom"), Margin.Bottom);
		return Obj;
	}

	/** Slot values, per concrete panel slot class (the getters live on the concrete types). */
	TSharedPtr<FJsonObject> SlotToJson(UPanelSlot* Slot)
	{
		if (!Slot)
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("class"), Slot->GetClass()->GetName());

		if (UCanvasPanelSlot* Canvas = Cast<UCanvasPanelSlot>(Slot))
		{
			const FAnchors A = Canvas->GetAnchors();
			const FVector2D Align = Canvas->GetAlignment();
			const FVector2D Pos = Canvas->GetPosition();
			const FVector2D Size = Canvas->GetSize();
			Obj->SetArrayField(TEXT("anchors"), {JNum(A.Minimum.X), JNum(A.Minimum.Y), JNum(A.Maximum.X), JNum(A.Maximum.Y)});
			Obj->SetArrayField(TEXT("alignment"), {JNum(Align.X), JNum(Align.Y)});
			Obj->SetArrayField(TEXT("position"), {JNum(Pos.X), JNum(Pos.Y)});
			Obj->SetArrayField(TEXT("size"), {JNum(Size.X), JNum(Size.Y)});
			Obj->SetBoolField(TEXT("auto_size"), Canvas->GetAutoSize());
			Obj->SetNumberField(TEXT("z_order"), Canvas->GetZOrder());
		}

		UEnum* HEnum = StaticEnum<EHorizontalAlignment>();
		UEnum* VEnum = StaticEnum<EVerticalAlignment>();
		FMargin Padding;
		EHorizontalAlignment HAlign = HAlign_Fill;
		EVerticalAlignment VAlign = VAlign_Fill;
		bool bHasBox = true;
		if (UVerticalBoxSlot* VBoxSlot = Cast<UVerticalBoxSlot>(Slot))
		{
			Padding = VBoxSlot->GetPadding(); HAlign = VBoxSlot->GetHorizontalAlignment(); VAlign = VBoxSlot->GetVerticalAlignment();
		}
		else if (UHorizontalBoxSlot* HBoxSlot = Cast<UHorizontalBoxSlot>(Slot))
		{
			Padding = HBoxSlot->GetPadding(); HAlign = HBoxSlot->GetHorizontalAlignment(); VAlign = HBoxSlot->GetVerticalAlignment();
		}
		else if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(Slot))
		{
			Padding = OverlaySlot->GetPadding(); HAlign = OverlaySlot->GetHorizontalAlignment(); VAlign = OverlaySlot->GetVerticalAlignment();
		}
		else if (USizeBoxSlot* SizeSlot = Cast<USizeBoxSlot>(Slot))
		{
			Padding = SizeSlot->GetPadding(); HAlign = SizeSlot->GetHorizontalAlignment(); VAlign = SizeSlot->GetVerticalAlignment();
		}
		else if (UScrollBoxSlot* ScrollSlot = Cast<UScrollBoxSlot>(Slot))
		{
			Padding = ScrollSlot->GetPadding(); HAlign = ScrollSlot->GetHorizontalAlignment(); VAlign = ScrollSlot->GetVerticalAlignment();
		}
		else if (UGridSlot* GridPadSlot = Cast<UGridSlot>(Slot))
		{
			Padding = GridPadSlot->GetPadding(); HAlign = GridPadSlot->GetHorizontalAlignment(); VAlign = GridPadSlot->GetVerticalAlignment();
		}
		else
		{
			bHasBox = false;
		}
		if (bHasBox)
		{
			Obj->SetObjectField(TEXT("padding"), MarginToJson(Padding));
			Obj->SetStringField(TEXT("horizontal_alignment"), EnumLabel(HEnum, (int64)HAlign));
			Obj->SetStringField(TEXT("vertical_alignment"), EnumLabel(VEnum, (int64)VAlign));
		}

		// ScaleBoxSlot is its own case: Padding is deprecated in 5.5 (Padding_DEPRECATED, no usable
		// setter) and both alignments are BlueprintReadOnly + private, which is also why python cannot
		// read this slot at all. Report what is real and say why the padding is missing.
		if (UScaleBoxSlot* ScaleSlot = Cast<UScaleBoxSlot>(Slot))
		{
			Obj->SetStringField(TEXT("horizontal_alignment"), EnumLabel(HEnum, (int64)ScaleSlot->GetHorizontalAlignment()));
			Obj->SetStringField(TEXT("vertical_alignment"), EnumLabel(VEnum, (int64)ScaleSlot->GetVerticalAlignment()));
			Obj->SetBoolField(TEXT("padding_deprecated"), true);
		}

		// Box slots (vertical / horizontal / scroll) also carry a size rule; the canvas slot's "size" is a vector,
		// so the rule gets its own key rather than overloading one name with two shapes.
		FSlateChildSize SizeRule;
		bool bHasSizeRule = false;
		if (UVerticalBoxSlot* VBoxSlot = Cast<UVerticalBoxSlot>(Slot))
		{
			SizeRule = VBoxSlot->GetSize(); bHasSizeRule = true;
		}
		else if (UHorizontalBoxSlot* HBoxSlot = Cast<UHorizontalBoxSlot>(Slot))
		{
			SizeRule = HBoxSlot->GetSize(); bHasSizeRule = true;
		}
		else if (UScrollBoxSlot* ScrollSlot = Cast<UScrollBoxSlot>(Slot))
		{
			SizeRule = ScrollSlot->GetSize(); bHasSizeRule = true;
		}
		if (bHasSizeRule)
		{
			TSharedPtr<FJsonObject> Rule = MakeShared<FJsonObject>();
			Rule->SetStringField(TEXT("rule"), SizeRule.SizeRule == ESlateSizeRule::Fill ? TEXT("Fill") : TEXT("Automatic"));
			Rule->SetNumberField(TEXT("value"), SizeRule.Value);
			Obj->SetObjectField(TEXT("size_rule"), Rule);
		}

		if (UGridSlot* GridSlot = Cast<UGridSlot>(Slot))
		{
			Obj->SetNumberField(TEXT("row"), GridSlot->GetRow());
			Obj->SetNumberField(TEXT("column"), GridSlot->GetColumn());
			Obj->SetNumberField(TEXT("row_span"), GridSlot->GetRowSpan());
			Obj->SetNumberField(TEXT("column_span"), GridSlot->GetColumnSpan());
			Obj->SetNumberField(TEXT("layer"), GridSlot->GetLayer());
			const FVector2D Nudge = GridSlot->GetNudge();
			Obj->SetArrayField(TEXT("nudge"), {JNum(Nudge.X), JNum(Nudge.Y)});
		}
		return Obj;
	}

	/** Read an enum-typed UPROPERTY through reflection (several UMG properties have no getter). */
	void AddEnumProperty(const TSharedPtr<FJsonObject>& Out, UObject* Object, const TCHAR* PropertyName,
		const TCHAR* Key)
	{
		if (!Object)
		{
			return;
		}
		FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), PropertyName);
		FNumericProperty* Numeric = CastField<FNumericProperty>(Property);
		if (!Numeric)
		{
			return;
		}
		UEnum* Enum = nullptr;
		if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			Enum = ByteProperty->Enum;
		}
		else if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			Enum = EnumProperty->GetEnum();
		}
		const void* Address = Property->ContainerPtrToValuePtr<void>(Object);
		Out->SetStringField(Key, EnumLabel(Enum, (int64)Numeric->GetUnsignedIntPropertyValue(Address)));
	}

	/** Per-type widget properties (only the types this domain builds plus the common handful). */
	TSharedPtr<FJsonObject> WidgetPropsToJson(UWidget* Widget)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		if (UTextBlock* TextBlock = Cast<UTextBlock>(Widget))
		{
			const FLinearColor C = TextBlock->GetColorAndOpacity().GetSpecifiedColor();
			P->SetStringField(TEXT("text"), TextBlock->GetText().ToString());
			P->SetNumberField(TEXT("font_size"), TextBlock->GetFont().Size);
			P->SetArrayField(TEXT("color"), {JNum(C.R), JNum(C.G), JNum(C.B), JNum(C.A)});
			AddEnumProperty(P, TextBlock, TEXT("Justification"), TEXT("justification"));
			P->SetBoolField(TEXT("auto_wrap"), TextBlock->GetAutoWrapText());
			P->SetNumberField(TEXT("wrap_text_at"), TextBlock->GetWrapTextAt());
		}
		else if (UButton* Button = Cast<UButton>(Widget))
		{
			const FLinearColor C = Button->GetBackgroundColor();
			P->SetArrayField(TEXT("background_color"), {JNum(C.R), JNum(C.G), JNum(C.B), JNum(C.A)});
		}
		else if (UImage* Image = Cast<UImage>(Widget))
		{
			const FLinearColor C = Image->GetColorAndOpacity();
			UObject* Resource = Image->GetBrush().GetResourceObject();
			P->SetArrayField(TEXT("tint"), {JNum(C.R), JNum(C.G), JNum(C.B), JNum(C.A)});
			P->SetStringField(TEXT("brush_resource"), Resource ? Resource->GetPathName() : FString());
		}
		else if (UProgressBar* Bar = Cast<UProgressBar>(Widget))
		{
			const FLinearColor C = Bar->GetFillColorAndOpacity();
			P->SetNumberField(TEXT("percent"), Bar->GetPercent());
			P->SetArrayField(TEXT("fill_color"), {JNum(C.R), JNum(C.G), JNum(C.B), JNum(C.A)});
		}
		return P;
	}

	TSharedPtr<FJsonObject> MakeListError(const FString& Code, const FString& Message, const TCHAR* Field,
	                                      const TArray<FString>& Values)
	{
		TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(Code, Message);
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FString& Value : Values)
		{
			Items.Add(MakeShared<FJsonValueString>(Value));
		}
		Error->SetArrayField(Field, Items);
		return Error;
	}

	void GatherWidgetNames(UWidgetTree* Tree, TArray<FString>& OutNames)
	{
		OutNames.Reset();
		if (!Tree)
		{
			return;
		}
		TArray<UWidget*> AllWidgets;
		Tree->GetAllWidgets(AllWidgets);
		for (UWidget* Widget : AllWidgets)
		{
			if (Widget)
			{
				OutNames.Add(Widget->GetName());
			}
		}
	}

	void GatherPanelNames(UWidgetTree* Tree, TArray<FString>& OutNames)
	{
		OutNames.Reset();
		if (!Tree)
		{
			return;
		}
		TArray<UWidget*> AllWidgets;
		Tree->GetAllWidgets(AllWidgets);
		for (UWidget* Widget : AllWidgets)
		{
			if (Widget && Widget->IsA<UPanelWidget>())
			{
				OutNames.Add(Widget->GetName());
			}
		}
	}

	UWidgetBlueprint* LoadWidgetBlueprintByParam(const FString& Name, TSharedPtr<FJsonObject>& OutError)
	{
		UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(FUnrealMCPCommonUtils::FindAsset(Name));
		if (!Blueprint || !Blueprint->WidgetTree)
		{
			OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("widget_blueprint_not_found"),
				FString::Printf(TEXT("Widget Blueprint '%s' not found"), *Name));
			return nullptr;
		}
		return Blueprint;
	}

	UWidget* FindWidgetOrError(UWidgetBlueprint* Blueprint, const FString& WidgetName, const FString& BlueprintName,
	                           TSharedPtr<FJsonObject>& OutError)
	{
		UWidget* Widget = (Blueprint && Blueprint->WidgetTree)
			? Blueprint->WidgetTree->FindWidget(FName(*WidgetName)) : nullptr;
		if (Widget)
		{
			return Widget;
		}

		TArray<FString> Names;
		GatherWidgetNames(Blueprint ? Blueprint->WidgetTree : nullptr, Names);
		OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("widget_not_found"), FString::Printf(
			TEXT("Widget '%s' not found in '%s' (widgets: %s)"), *WidgetName, *BlueprintName,
			Names.Num() > 0 ? *FString::Join(Names, TEXT(", ")) : TEXT("<none>")));
		return nullptr;
	}

	/** The parent panel a new widget goes into: 'parent_widget' when given, else the root panel. Null = no root yet. */
	UPanelWidget* ResolveParentPanel(UWidgetBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params,
	                                 FString& OutParentName, TSharedPtr<FJsonObject>& OutError)
	{
		OutParentName.Reset();

		FString RequestedParent;
		if (Params->TryGetStringField(TEXT("parent_widget"), RequestedParent) && !RequestedParent.IsEmpty())
		{
			UWidget* ParentWidget = FindWidgetOrError(Blueprint, RequestedParent, Blueprint->GetName(), OutError);
			if (!ParentWidget)
			{
				return nullptr;
			}
			UPanelWidget* Panel = Cast<UPanelWidget>(ParentWidget);
			if (!Panel)
			{
				TArray<FString> Panels;
				GatherPanelNames(Blueprint->WidgetTree, Panels);
				OutError = MakeListError(TEXT("unsupported_parent"), FString::Printf(
					TEXT("Widget '%s' is a %s, which cannot hold children"), *RequestedParent,
					*ParentWidget->GetClass()->GetName()), TEXT("panels"), Panels);
				return nullptr;
			}
			OutParentName = ParentWidget->GetName();
			return Panel;
		}

		UWidget* Root = Blueprint->WidgetTree ? Blueprint->WidgetTree->RootWidget : nullptr;
		if (!Root)
		{
			return nullptr;
		}
		UPanelWidget* RootPanel = Cast<UPanelWidget>(Root);
		if (!RootPanel)
		{
			TArray<FString> Panels;
			GatherPanelNames(Blueprint->WidgetTree, Panels);
			OutError = MakeListError(TEXT("unsupported_root_panel"), FString::Printf(
				TEXT("The root widget '%s' is a %s, not a panel; pass 'parent_widget' to pick a panel"),
				*Root->GetName(), *Root->GetClass()->GetName()), TEXT("panels"), Panels);
			return nullptr;
		}
		OutParentName = Root->GetName();
		return RootPanel;
	}

	void AddWidgetListToJson(const TSharedPtr<FJsonObject>& Result, UWidgetTree* Tree)
	{
		TArray<FString> Names;
		GatherWidgetNames(Tree, Names);
		Result->SetNumberField(TEXT("widget_count"), Names.Num());
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FString& Name : Names)
		{
			Items.Add(MakeShared<FJsonValueString>(Name));
		}
		Result->SetArrayField(TEXT("widget_names"), Items);
	}

	namespace
	{
		// Last time an MCP command changed this widget blueprint. Used as the boundary between
		// "messages from the compile this call caused" and history - the same role the material
		// domain's mutation table plays for get_material_compile_errors.
		TMap<FString, FDateTime>& MutationTimes()
		{
			static TMap<FString, FDateTime> Times;
			return Times;
		}

		FString MutationKey(const UWidgetBlueprint* Blueprint)
		{
			return Blueprint ? Blueprint->GetPathName() : FString();
		}
	}

	::FWidgetBlueprintEditor* FindWidgetBlueprintEditor(UWidgetBlueprint* Blueprint)
	{
		UAssetEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
		if (!Subsystem || !Blueprint)
		{
			return nullptr;
		}
		IAssetEditorInstance* Instance = Subsystem->FindEditorForAsset(Blueprint, /*bFocusIfOpen=*/false);
		if (!Instance || Instance->GetEditorName() != FName("WidgetBlueprintEditor"))
		{
			return nullptr;
		}
		return static_cast<::FWidgetBlueprintEditor*>(Instance);
	}

	void RecordUMGMutation(UWidgetBlueprint* Blueprint)
	{
		if (Blueprint)
		{
			MutationTimes().Add(MutationKey(Blueprint), FDateTime::UtcNow());
		}
	}

	bool FindUMGMutationTime(const UWidgetBlueprint* Blueprint, FDateTime& OutTime)
	{
		if (const FDateTime* Found = MutationTimes().Find(MutationKey(Blueprint)))
		{
			OutTime = *Found;
			return true;
		}
		return false;
	}

	bool TryParseSince(const TSharedPtr<FJsonValue>& Value, FDateTime& OutTime)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		if (Value->Type == EJson::Number)
		{
			// Epoch seconds, the shape a script computes most easily.
			OutTime = FDateTime(1970, 1, 1) + FTimespan::FromSeconds(Value->AsNumber());
			return true;
		}
		if (Value->Type != EJson::String)
		{
			return false;
		}
		const FString Text = Value->AsString().TrimStartAndEnd();
		if (Text.IsEmpty())
		{
			return false;
		}
		if (Text.IsNumeric())
		{
			OutTime = FDateTime(1970, 1, 1) + FTimespan::FromSeconds(FCString::Atod(*Text));
			return true;
		}
		return FDateTime::ParseIso8601(*Text, OutTime);
	}

	bool FinishWidgetWrite(UWidgetBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params,
	                       const TSharedPtr<FJsonObject>& Result)
	{
		RecordUMGMutation(Blueprint);

		const bool bRequested = FUnrealMCPCommonUtils::IsPersistRequested(Params);
		const bool bSaved = bRequested && Blueprint && FUnrealMCPCommonUtils::SaveAssetForObject(Blueprint);
		if (Result.IsValid())
		{
			// The truth about this call, not about "a save API was reached".
			Result->SetBoolField(TEXT("saved"), bSaved);
			Result->SetBoolField(TEXT("persist_requested"), bRequested);
		}
		return bSaved;
	}

	TSharedPtr<FJsonObject> EnsurePersistReceipt(const TSharedPtr<FJsonObject>& Params,
	                                             const TSharedPtr<FJsonObject>& Result)
	{
		if (!Result.IsValid())
		{
			return Result;
		}
		if (!Result->HasField(TEXT("persist_requested")))
		{
			Result->SetBoolField(TEXT("persist_requested"), FUnrealMCPCommonUtils::IsPersistRequested(Params));
		}
		if (!Result->HasField(TEXT("saved")))
		{
			// Reaching here means the handler reported nothing, which for a write command is
			// "nothing was written" - a refusal or a validation error.
			Result->SetBoolField(TEXT("saved"), false);
		}
		return Result;
	}

	FString BlueprintStatusLabel(const UWidgetBlueprint* Blueprint)
	{
		if (!Blueprint)
		{
			return FString();
		}
		UEnum* StatusEnum = StaticEnum<EBlueprintStatus>();
		return StatusEnum ? StatusEnum->GetNameStringByValue((int64)Blueprint->Status) : FString();
	}

	TSharedPtr<FJsonObject> CompileMessageToJson(const FUMGCompileMessage& Message)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("severity"), Message.Severity);
		Obj->SetStringField(TEXT("message"), Message.Message);
		if (!Message.NodeId.IsEmpty() || !Message.NodeName.IsEmpty())
		{
			TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
			Node->SetStringField(TEXT("node_id"), Message.NodeId);
			Node->SetStringField(TEXT("node_name"), Message.NodeName);
			Node->SetStringField(TEXT("graph_name"), Message.GraphName);
			Obj->SetObjectField(TEXT("node"), Node);
		}
		return Obj;
	}

	bool CompileWidgetWithMessages(UWidgetBlueprint* Blueprint, TArray<FUMGCompileMessage>& OutMessages,
	                               int32& OutErrors, int32& OutWarnings)
	{
		OutMessages.Reset();
		OutErrors = 0;
		OutWarnings = 0;
		if (!Blueprint)
		{
			return false;
		}

		FCompilerResultsLog MessageLog;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &MessageLog);

		for (const TSharedRef<FTokenizedMessage>& Message : MessageLog.Messages)
		{
			FUMGCompileMessage Entry;
			switch (Message->GetSeverity())
			{
			case EMessageSeverity::Error:              Entry.Severity = TEXT("Error"); ++OutErrors; break;
			case EMessageSeverity::PerformanceWarning: Entry.Severity = TEXT("PerformanceWarning"); ++OutWarnings; break;
			case EMessageSeverity::Warning:            Entry.Severity = TEXT("Warning"); ++OutWarnings; break;
			default:                                   Entry.Severity = TEXT("Info"); break;
			}
			Entry.Message = Message->ToText().ToString();

			// The node is what makes a message actionable, but the log only carries it as a token
			// (FCompilerResultsLog::GetNodesFromTokens is private, so scan the tokens here).
			for (const TSharedRef<IMessageToken>& Token : Message->GetMessageTokens())
			{
				if (Token->GetType() != EMessageToken::Object)
				{
					continue;
				}
				const FUObjectToken& ObjectToken = static_cast<const FUObjectToken&>(Token.Get());
				UEdGraphNode* Node = Cast<UEdGraphNode>(ObjectToken.GetObject().Get());
				if (!Node)
				{
					continue;
				}
				Entry.NodeId = Node->NodeGuid.ToString();
				Entry.NodeName = Node->GetName();
				if (const UEdGraph* Graph = Node->GetGraph())
				{
					Entry.GraphName = Graph->GetName();
				}
				break;
			}
			OutMessages.Add(MoveTemp(Entry));
		}

		return Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings;
	}

	/** Mark dirty, compile, save when the caller asked for it, and record the outcome on the response. */
	void FinishWidgetEdit(UWidgetBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params,
	                      const TSharedPtr<FJsonObject>& Result)
	{
		if (!Blueprint)
		{
			return;
		}
		Blueprint->MarkPackageDirty();
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		FinishWidgetWrite(Blueprint, Params, Result);
		if (Result.IsValid())
		{
			Result->SetBoolField(TEXT("compiled"),
				Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
			// Enum name, the same shape get_umg_compile_errors reports. This field used to be the raw
			// number as a string, which read as a different thing under the same key.
			Result->SetStringField(TEXT("status"), BlueprintStatusLabel(Blueprint));
			Result->SetNumberField(TEXT("status_code"), (int32)Blueprint->Status);
			AddWidgetListToJson(Result, Blueprint->WidgetTree);
		}
	}

	bool CompileWithMessages(UWidgetBlueprint* Blueprint, TArray<FString>& OutMessages)
	{
		OutMessages.Reset();
		if (!Blueprint)
		{
			return false;
		}
		FCompilerResultsLog MessageLog;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &MessageLog);
		for (const TSharedRef<FTokenizedMessage>& Message : MessageLog.Messages)
		{
			OutMessages.Add(Message->ToText().ToString());
		}
		return Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings;
	}

	/** The runtime table is what the widget actually applies: an editor entry alone proves nothing. */
	bool RuntimeBindingMatches(UWidgetBlueprint* Blueprint, const FString& WidgetName, const FString& PropertyName,
	                           TArray<TSharedPtr<FJsonObject>>* OutRuntimeBindings)
	{
		UWidgetBlueprintGeneratedClass* GeneratedClass = Blueprint
			? Cast<UWidgetBlueprintGeneratedClass>(Blueprint->GeneratedClass) : nullptr;
		if (!GeneratedClass)
		{
			return false;
		}
		bool bFound = false;
		for (const FDelegateRuntimeBinding& Binding : GeneratedClass->Bindings)
		{
			if (OutRuntimeBindings)
			{
				TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetStringField(TEXT("widget"), Binding.ObjectName);
				Obj->SetStringField(TEXT("property"), Binding.PropertyName.ToString());
				Obj->SetStringField(TEXT("function"), Binding.FunctionName.ToString());
				OutRuntimeBindings->Add(Obj);
			}
			if (Binding.ObjectName == WidgetName && Binding.PropertyName.ToString() == PropertyName)
			{
				bFound = true;
			}
		}
		return bFound;
	}

	TArray<TSharedPtr<FJsonValue>> BindingsToJsonArray(UWidgetBlueprint* Blueprint, bool bIncludeEffective)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		if (!Blueprint)
		{
			return Items;
		}
		TArray<TSharedPtr<FJsonObject>> RuntimeBindings;
		RuntimeBindingMatches(Blueprint, FString(), FString(), &RuntimeBindings);
		UEnum* KindEnum = StaticEnum<EBindingKind>();
		for (const FDelegateEditorBinding& Binding : Blueprint->Bindings)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("widget"), Binding.ObjectName);
			Obj->SetStringField(TEXT("property"), Binding.PropertyName.ToString());
			Obj->SetStringField(TEXT("function"), Binding.FunctionName.ToString());
			Obj->SetStringField(TEXT("kind"), EnumLabel(KindEnum, (int64)Binding.Kind));
			if (bIncludeEffective)
			{
				bool bEffective = false;
				for (const TSharedPtr<FJsonObject>& Runtime : RuntimeBindings)
				{
					FString RuntimeWidget;
					FString RuntimeProperty;
					Runtime->TryGetStringField(TEXT("widget"), RuntimeWidget);
					Runtime->TryGetStringField(TEXT("property"), RuntimeProperty);
					if (RuntimeWidget == Binding.ObjectName && RuntimeProperty == Binding.PropertyName.ToString())
					{
						bEffective = true;
						break;
					}
				}
				Obj->SetBoolField(TEXT("effective"), bEffective);
			}
			Items.Add(MakeShared<FJsonValueObject>(Obj));
		}
		return Items;
	}
}