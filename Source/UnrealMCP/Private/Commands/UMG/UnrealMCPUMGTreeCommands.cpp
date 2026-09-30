// UMG commands: widget tree structure editing (remove / reparent / reorder / rename / set root).
#include "Commands/UnrealMCPUMGCommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBoxSlot.h"
#include "Components/ScrollBoxSlot.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Variable.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Components/GridSlot.h"
#include "Components/Widget.h"
#include "Components/SlateWrapperTypes.h"
#include "EdGraph/EdGraph.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "ScopedTransaction.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "MovieScene.h"
#include "MovieScenePossessable.h"
#include "Commands/UMG/UnrealMCPUMGCommandHelpers.h"

namespace
{
	/** Slot values copied off a slot before a reparent destroys it. */
	struct FSlotSnapshot
	{
		bool bCanvas = false;
		FAnchors Anchors;
		FVector2D Alignment = FVector2D(0.5f, 0.5f);
		FVector2D Position = FVector2D::ZeroVector;
		FVector2D Size = FVector2D(100.f, 100.f);
		bool bAutoSize = false;
		int32 ZOrder = 0;

		bool bBox = false;
		FMargin Padding;
		EHorizontalAlignment HAlign = HAlign_Fill;
		EVerticalAlignment VAlign = VAlign_Fill;
		bool bSizeRule = false;
		FSlateChildSize SizeRule;

		bool bGrid = false;
		int32 Row = 0;
		int32 RowSpan = 1;
		int32 Column = 0;
		int32 ColumnSpan = 1;
		int32 Layer = 0;
		FVector2D Nudge = FVector2D::ZeroVector;
	};

	void GatherWidgetReferences(UWidgetBlueprint* Blueprint, const TArray<FName>& WidgetNames, TArray<FWidgetReferenceInfo>& OutRefs)
	{
		OutRefs.Reset();
		if (!Blueprint || WidgetNames.Num() == 0)
		{
			return;
		}

		// Graphs in which a widget variable node is the mechanism rather than a user of the widget:
		// the Get<widget> getter, and every getter a property binding points at.
		TArray<FString> GetterNames;
		for (const FName& WidgetName : WidgetNames)
		{
			GetterNames.Add(FString::Printf(TEXT("Get%s"), *WidgetName.ToString()));
		}
		for (const FDelegateEditorBinding& Binding : Blueprint->Bindings)
		{
			if (!Binding.FunctionName.IsNone())
			{
				GetterNames.AddUnique(Binding.FunctionName.ToString());
			}
		}

		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (!Graph)
			{
				continue;
			}
			const FString GraphName = Graph->GetName();
			const bool bInsideGetter = GetterNames.Contains(GraphName);

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}
				if (UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node))
				{
					const FName VariableName = VariableNode->GetVarName();
					// Inside the widget's own Get<X> graph the variable is the point of the graph, not a user of it.
					if (WidgetNames.Contains(VariableName) && !bInsideGetter)
					{
						FWidgetReferenceInfo Info;
						Info.Kind = TEXT("variable_node");
						Info.Graph = GraphName;
						Info.Node = VariableNode->GetName();
						Info.Detail = VariableName.ToString();
						OutRefs.Add(Info);
					}
					continue;
				}
				if (UK2Node_ComponentBoundEvent* BoundEvent = Cast<UK2Node_ComponentBoundEvent>(Node))
				{
					const FName ComponentName = BoundEvent->GetComponentPropertyName();
					if (WidgetNames.Contains(ComponentName))
					{
						FWidgetReferenceInfo Info;
						Info.Kind = TEXT("event_node");
						Info.Graph = GraphName;
						Info.Node = BoundEvent->GetName();
						Info.Detail = FString::Printf(TEXT("%s.%s"), *ComponentName.ToString(),
							*BoundEvent->DelegatePropertyName.ToString());
						OutRefs.Add(Info);
					}
					continue;
				}
				if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
				{
					const FString CalledName = CallNode->FunctionReference.GetMemberName().ToString();
					if (!bInsideGetter && GetterNames.Contains(CalledName))
					{
						FWidgetReferenceInfo Info;
						Info.Kind = TEXT("function_node");
						Info.Graph = GraphName;
						Info.Node = CallNode->GetName();
						Info.Detail = CalledName;
						OutRefs.Add(Info);
					}
				}
			}
		}
	}

	FString PanelChildOrder(UPanelWidget* Panel)
	{
		if (!Panel)
		{
			return FString();
		}
		TArray<FString> Names;
		const int32 Count = Panel->GetChildrenCount();
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (UWidget* Child = Panel->GetChildAt(Index))
			{
				Names.Add(Child->GetName());
			}
		}
		return FString::Join(Names, TEXT(","));
	}

	/** The widget and everything under it, in the order UWidgetTree::GetChildWidgets reports them. */
	TArray<FString> CollectSubtreeNames(UWidget* Widget)
	{
		TArray<FString> Names;
		if (!Widget)
		{
			return Names;
		}
		Names.Add(Widget->GetName());
		TArray<UWidget*> Children;
		UWidgetTree::GetChildWidgets(Widget, Children);
		for (UWidget* Child : Children)
		{
			if (Child)
			{
				Names.Add(Child->GetName());
			}
		}
		return Names;
	}

	FSlotSnapshot CaptureSlot(UPanelSlot* Slot)
	{
		FSlotSnapshot Snapshot;
		if (!Slot)
		{
			return Snapshot;
		}
		if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Slot))
		{
			Snapshot.bCanvas = true;
			Snapshot.Anchors = CanvasSlot->GetAnchors();
			Snapshot.Alignment = CanvasSlot->GetAlignment();
			Snapshot.Position = CanvasSlot->GetPosition();
			Snapshot.Size = CanvasSlot->GetSize();
			Snapshot.bAutoSize = CanvasSlot->GetAutoSize();
			Snapshot.ZOrder = CanvasSlot->GetZOrder();
			return Snapshot;
		}
		if (UVerticalBoxSlot* VBoxSlot = Cast<UVerticalBoxSlot>(Slot))
		{
			Snapshot.bBox = true;
			Snapshot.Padding = VBoxSlot->GetPadding();
			Snapshot.HAlign = VBoxSlot->GetHorizontalAlignment();
			Snapshot.VAlign = VBoxSlot->GetVerticalAlignment();
			Snapshot.bSizeRule = true;
			Snapshot.SizeRule = VBoxSlot->GetSize();
		}
		else if (UHorizontalBoxSlot* HBoxSlot = Cast<UHorizontalBoxSlot>(Slot))
		{
			Snapshot.bBox = true;
			Snapshot.Padding = HBoxSlot->GetPadding();
			Snapshot.HAlign = HBoxSlot->GetHorizontalAlignment();
			Snapshot.VAlign = HBoxSlot->GetVerticalAlignment();
			Snapshot.bSizeRule = true;
			Snapshot.SizeRule = HBoxSlot->GetSize();
		}
		else if (UScrollBoxSlot* ScrollSlot = Cast<UScrollBoxSlot>(Slot))
		{
			Snapshot.bBox = true;
			Snapshot.Padding = ScrollSlot->GetPadding();
			Snapshot.HAlign = ScrollSlot->GetHorizontalAlignment();
			Snapshot.VAlign = ScrollSlot->GetVerticalAlignment();
			Snapshot.bSizeRule = true;
			Snapshot.SizeRule = ScrollSlot->GetSize();
		}
		else if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(Slot))
		{
			Snapshot.bBox = true;
			Snapshot.Padding = OverlaySlot->GetPadding();
			Snapshot.HAlign = OverlaySlot->GetHorizontalAlignment();
			Snapshot.VAlign = OverlaySlot->GetVerticalAlignment();
		}
		else if (USizeBoxSlot* SizeBoxSlot = Cast<USizeBoxSlot>(Slot))
		{
			Snapshot.bBox = true;
			Snapshot.Padding = SizeBoxSlot->GetPadding();
			Snapshot.HAlign = SizeBoxSlot->GetHorizontalAlignment();
			Snapshot.VAlign = SizeBoxSlot->GetVerticalAlignment();
		}
		else if (UGridSlot* GridSlot = Cast<UGridSlot>(Slot))
		{
			Snapshot.bBox = true;
			Snapshot.Padding = GridSlot->GetPadding();
			Snapshot.HAlign = GridSlot->GetHorizontalAlignment();
			Snapshot.VAlign = GridSlot->GetVerticalAlignment();
			Snapshot.bGrid = true;
			Snapshot.Row = GridSlot->GetRow();
			Snapshot.RowSpan = GridSlot->GetRowSpan();
			Snapshot.Column = GridSlot->GetColumn();
			Snapshot.ColumnSpan = GridSlot->GetColumnSpan();
			Snapshot.Layer = GridSlot->GetLayer();
			Snapshot.Nudge = GridSlot->GetNudge();
		}
		return Snapshot;
	}

	/** Copy the slot fields the new slot class also has; report the ones it had to drop. */
	void ApplyOverlappingSlotFields(const FSlotSnapshot& From, UPanelSlot* To, TArray<FString>& OutMigrated,
	                                TArray<FString>& OutDropped)
	{
		OutMigrated.Reset();
		OutDropped.Reset();
		if (!To)
		{
			return;
		}

		const bool bTargetCanvas = To->IsA<UCanvasPanelSlot>();
		const bool bTargetBox = To->IsA<UVerticalBoxSlot>() || To->IsA<UHorizontalBoxSlot>() || To->IsA<UScrollBoxSlot>();
		const bool bTargetPadding = bTargetBox || To->IsA<UOverlaySlot>() || To->IsA<USizeBoxSlot>() || To->IsA<UGridSlot>();
		const bool bTargetGrid = To->IsA<UGridSlot>();

		if (From.bCanvas)
		{
			if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(To))
			{
				CanvasSlot->SetAnchors(From.Anchors);
				CanvasSlot->SetAlignment(From.Alignment);
				CanvasSlot->SetPosition(From.Position);
				CanvasSlot->SetAutoSize(From.bAutoSize);
				if (!From.bAutoSize)
				{
					CanvasSlot->SetSize(From.Size);
				}
				CanvasSlot->SetZOrder(From.ZOrder);
				CanvasSlot->SynchronizeProperties();
				for (const TCHAR* Field : { TEXT("anchors"), TEXT("alignment"), TEXT("position"), TEXT("size"),
					TEXT("auto_size"), TEXT("z_order") })
				{
					OutMigrated.Add(Field);
				}
			}
			else
			{
				for (const TCHAR* Field : { TEXT("anchors"), TEXT("alignment"), TEXT("position"), TEXT("size"),
					TEXT("auto_size"), TEXT("z_order") })
				{
					OutDropped.Add(Field);
				}
			}
		}

		if (From.bBox)
		{
			if (!bTargetPadding)
			{
				OutDropped.Add(TEXT("padding"));
				OutDropped.Add(TEXT("horizontal_alignment"));
				OutDropped.Add(TEXT("vertical_alignment"));
			}
			else
			{
				if (UVerticalBoxSlot* VBoxTarget = Cast<UVerticalBoxSlot>(To)) { VBoxTarget->SetPadding(From.Padding); VBoxTarget->SetHorizontalAlignment(From.HAlign); VBoxTarget->SetVerticalAlignment(From.VAlign); }
				else if (UHorizontalBoxSlot* HBoxTarget = Cast<UHorizontalBoxSlot>(To)) { HBoxTarget->SetPadding(From.Padding); HBoxTarget->SetHorizontalAlignment(From.HAlign); HBoxTarget->SetVerticalAlignment(From.VAlign); }
				else if (UScrollBoxSlot* ScrollTarget = Cast<UScrollBoxSlot>(To)) { ScrollTarget->SetPadding(From.Padding); ScrollTarget->SetHorizontalAlignment(From.HAlign); ScrollTarget->SetVerticalAlignment(From.VAlign); }
				else if (UOverlaySlot* OverlayTarget = Cast<UOverlaySlot>(To)) { OverlayTarget->SetPadding(From.Padding); OverlayTarget->SetHorizontalAlignment(From.HAlign); OverlayTarget->SetVerticalAlignment(From.VAlign); }
				else if (USizeBoxSlot* SizeBoxTarget = Cast<USizeBoxSlot>(To)) { SizeBoxTarget->SetPadding(From.Padding); SizeBoxTarget->SetHorizontalAlignment(From.HAlign); SizeBoxTarget->SetVerticalAlignment(From.VAlign); }
				else if (UGridSlot* GridTarget = Cast<UGridSlot>(To)) { GridTarget->SetPadding(From.Padding); GridTarget->SetHorizontalAlignment(From.HAlign); GridTarget->SetVerticalAlignment(From.VAlign); }
				To->SynchronizeProperties();
				OutMigrated.Add(TEXT("padding"));
				OutMigrated.Add(TEXT("horizontal_alignment"));
				OutMigrated.Add(TEXT("vertical_alignment"));
			}
		}

		if (From.bSizeRule)
		{
			if (UVerticalBoxSlot* VBoxRule = Cast<UVerticalBoxSlot>(To)) { VBoxRule->SetSize(From.SizeRule); OutMigrated.Add(TEXT("size_rule")); }
			else if (UHorizontalBoxSlot* HBoxRule = Cast<UHorizontalBoxSlot>(To)) { HBoxRule->SetSize(From.SizeRule); OutMigrated.Add(TEXT("size_rule")); }
			else if (UScrollBoxSlot* ScrollRule = Cast<UScrollBoxSlot>(To)) { ScrollRule->SetSize(From.SizeRule); OutMigrated.Add(TEXT("size_rule")); }
			else { OutDropped.Add(TEXT("size_rule")); }
		}

		if (From.bGrid)
		{
			if (UGridSlot* GridSlot = Cast<UGridSlot>(To))
			{
				GridSlot->SetRow(From.Row);
				GridSlot->SetColumn(From.Column);
				GridSlot->SetRowSpan(From.RowSpan);
				GridSlot->SetColumnSpan(From.ColumnSpan);
				GridSlot->SetLayer(From.Layer);
				GridSlot->SetNudge(From.Nudge);
				GridSlot->SynchronizeProperties();
				for (const TCHAR* Field : { TEXT("row"), TEXT("column"), TEXT("row_span"), TEXT("column_span"),
					TEXT("layer"), TEXT("nudge") })
				{
					OutMigrated.Add(Field);
				}
			}
			else
			{
				for (const TCHAR* Field : { TEXT("row"), TEXT("column"), TEXT("row_span"), TEXT("column_span"),
					TEXT("layer"), TEXT("nudge") })
				{
					OutDropped.Add(Field);
				}
			}
		}
		(void)bTargetGrid;
	}

	/**
	 * The cleanup chain behind remove_widget: bindings, the getter function the bindings were using, and the
	 * widget variable. Called only after the caller confirmed no graph node still refers to the widget.
	 */
	void RemoveWidgetArtifacts(UWidgetBlueprint* Blueprint, const FName WidgetName,
	                           TArray<TSharedPtr<FJsonObject>>& OutBindings, TArray<FString>& OutVariables,
	                           TArray<FString>& OutFunctions,
	                           TArray<TSharedPtr<FJsonObject>>& OutAnimationTracks)
	{
		if (!Blueprint)
		{
			return;
		}
		const FString WidgetNameString = WidgetName.ToString();

		// Animations address their target by widget name, so a track would otherwise survive the widget
		// it names - the leftover the report has to make visible (and the reason this is cleaned here
		// rather than left to a separate prune command).
		for (UWidgetAnimation* Animation : Blueprint->Animations)
		{
			if (!Animation)
			{
				continue;
			}
			UMovieScene* Scene = Animation->GetMovieScene();
			TArray<FGuid> RemovedGuids;
			for (int32 Index = Animation->AnimationBindings.Num() - 1; Index >= 0; --Index)
			{
				const FWidgetAnimationBinding& AnimationBinding = Animation->AnimationBindings[Index];
				if (AnimationBinding.WidgetName != WidgetName && AnimationBinding.SlotWidgetName != WidgetName)
				{
					continue;
				}
				int32 TrackCount = 0;
				if (Scene)
				{
					for (const FMovieSceneBinding& SceneBinding : Scene->GetBindings())
					{
						if (SceneBinding.GetObjectGuid() == AnimationBinding.AnimationGuid)
						{
							TrackCount = SceneBinding.GetTracks().Num();
							break;
						}
					}
				}
				TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetStringField(TEXT("animation"), Animation->GetName());
				Obj->SetStringField(TEXT("widget"), AnimationBinding.WidgetName.ToString());
				Obj->SetStringField(TEXT("slot_widget"), AnimationBinding.SlotWidgetName.ToString());
				Obj->SetStringField(TEXT("possessable_guid"), AnimationBinding.AnimationGuid.ToString());
				Obj->SetNumberField(TEXT("tracks"), TrackCount);
				OutAnimationTracks.Add(Obj);

				RemovedGuids.AddUnique(AnimationBinding.AnimationGuid);
				Animation->AnimationBindings.RemoveAt(Index);
			}
			if (Scene)
			{
				for (const FGuid& Guid : RemovedGuids)
				{
					// The possessable owns the tracks; the binding entry that holds them is removed
					// through the public accessor (UMovieScene::RemoveBinding is protected).
					Scene->RemovePossessable(Guid);
					Scene->GetBindings().RemoveAll([&Guid](const FMovieSceneBinding& SceneBinding)
					{
						return SceneBinding.GetObjectGuid() == Guid;
					});
				}
			}
		}

		for (int32 Index = Blueprint->Bindings.Num() - 1; Index >= 0; --Index)
		{
			const FDelegateEditorBinding& Binding = Blueprint->Bindings[Index];
			if (Binding.ObjectName != WidgetNameString)
			{
				continue;
			}
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("widget"), Binding.ObjectName);
			Obj->SetStringField(TEXT("property"), Binding.PropertyName.ToString());
			Obj->SetStringField(TEXT("function"), Binding.FunctionName.ToString());
			OutBindings.Add(Obj);
			Blueprint->Bindings.RemoveAt(Index);
		}

		// The getter a binding points at, plus the widget's own Get<widget> getter: a function graph is only
		// removed when no node outside it still calls it.
		auto IsGraphReferenced = [Blueprint](UEdGraph* Graph)
		{
			TArray<UEdGraph*> AllGraphs;
			Blueprint->GetAllGraphs(AllGraphs);
			for (UEdGraph* Other : AllGraphs)
			{
				if (!Other || Other == Graph)
				{
					continue;
				}
				for (UEdGraphNode* Node : Other->Nodes)
				{
					if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
					{
						if (CallNode->FunctionReference.GetMemberName() == Graph->GetFName())
						{
							return true;
						}
					}
				}
			}
			return false;
		};

		TArray<FString> FunctionCandidates;
		for (const TSharedPtr<FJsonObject>& BindingJson : OutBindings)
		{
			FString FunctionName;
			if (BindingJson->TryGetStringField(TEXT("function"), FunctionName) && !FunctionName.IsEmpty())
			{
				FunctionCandidates.AddUnique(FunctionName);
			}
		}
		FunctionCandidates.AddUnique(FString::Printf(TEXT("Get%s"), *WidgetNameString));

		for (const FString& FunctionName : FunctionCandidates)
		{
			UEdGraph* Graph = nullptr;
			for (UEdGraph* FunctionGraph : Blueprint->FunctionGraphs)
			{
				if (FunctionGraph && FunctionGraph->GetName() == FunctionName)
				{
					Graph = FunctionGraph;
					break;
				}
			}
			if (!Graph || IsGraphReferenced(Graph))
			{
				continue;
			}
			OutFunctions.Add(Graph->GetName());
			FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph, EGraphRemoveFlags::Default);
		}

		// A binding variable is named after the getter ("GetAmmo" reads the "Ammo" variable), so the
		// variable is derived from the function name rather than guessed from the widget name.
		TArray<FName> VariablesToRemove;
		for (const FString& FunctionName : FunctionCandidates)
		{
			if (FunctionName.StartsWith(TEXT("Get"), ESearchCase::CaseSensitive) && FunctionName.Len() > 3)
			{
				VariablesToRemove.AddUnique(FName(*FunctionName.RightChop(3)));
			}
		}
		VariablesToRemove.AddUnique(WidgetName);

		for (const FName& VariableName : VariablesToRemove)
		{
			if (!FBlueprintEditorUtils::FindMemberVariableGuidByName(Blueprint, VariableName).IsValid())
			{
				continue;
			}
			FBlueprintEditorUtils::RemoveVariableNodes(Blueprint, VariableName);
			FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, VariableName);
			OutVariables.Add(VariableName.ToString());
		}
	}
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleRemoveWidget(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'blueprint_name' parameter"));
	}
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'widget_name' parameter"));
	}

	TSharedPtr<FJsonObject> Error;
	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByParam(BlueprintName, Error);
	if (!WidgetBlueprint)
	{
		return Error;
	}
	UWidget* Widget = FindWidgetOrError(WidgetBlueprint, WidgetName, BlueprintName, Error);
	if (!Widget)
	{
		return Error;
	}

	const bool bWasRoot = (WidgetBlueprint->WidgetTree->RootWidget == Widget);
	const TArray<FString> Subtree = CollectSubtreeNames(Widget);

	// Anything the graph still refers to is the caller's to deal with: refuse instead of breaking it.
	TArray<FName> SubtreeNames;
	for (const FString& Name : Subtree)
	{
		SubtreeNames.Add(FName(*Name));
	}
	TArray<FWidgetReferenceInfo> References;
	GatherWidgetReferences(WidgetBlueprint, SubtreeNames, References);
	if (References.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Blockers;
		for (const FWidgetReferenceInfo& Reference : References)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("kind"), Reference.Kind);
			Obj->SetStringField(TEXT("graph"), Reference.Graph);
			Obj->SetStringField(TEXT("node"), Reference.Node);
			Obj->SetStringField(TEXT("detail"), Reference.Detail);
			Blockers.Add(MakeShared<FJsonValueObject>(Obj));
		}
		TSharedPtr<FJsonObject> Blocked = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("blocked_by_references"),
			FString::Printf(TEXT("Widget '%s' is still referenced by %d graph node(s); remove those first"),
				*WidgetName, References.Num()));
		Blocked->SetArrayField(TEXT("blockers"), Blockers);
		return Blocked;
	}

	// Cleanup chain, children first so nested widgets leave nothing behind either.
	TArray<TSharedPtr<FJsonObject>> RemovedBindings;
	TArray<FString> RemovedVariables;
	TArray<FString> RemovedFunctions;
	TArray<TSharedPtr<FJsonObject>> RemovedAnimationTracks;
	for (int32 Index = Subtree.Num() - 1; Index >= 0; --Index)
	{
		RemoveWidgetArtifacts(WidgetBlueprint, FName(*Subtree[Index]), RemovedBindings, RemovedVariables,
		                      RemovedFunctions, RemovedAnimationTracks);
	}

	const bool bRemoved = WidgetBlueprint->WidgetTree->RemoveWidget(Widget);
	TArray<FString> RemovedWidgets;
	if (bRemoved)
	{
		RemovedWidgets.Append(Subtree);
		// Rename the whole subtree out of the way so the names can be reused right away.
		Widget->Rename(nullptr, GetTransientPackage());
		TArray<UWidget*> Children;
		UWidgetTree::GetChildWidgets(Widget, Children);
		for (UWidget* Child : Children)
		{
			if (Child)
			{
				Child->Rename(nullptr, GetTransientPackage());
			}
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
	}

	TArray<TSharedPtr<FJsonValue>> RemovedBindingValues;
	for (const TSharedPtr<FJsonObject>& Obj : RemovedBindings)
	{
		RemovedBindingValues.Add(MakeShared<FJsonValueObject>(Obj));
	}
	TArray<TSharedPtr<FJsonValue>> Variables;
	for (const FString& Name : RemovedVariables)
	{
		Variables.Add(MakeShared<FJsonValueString>(Name));
	}
	TArray<TSharedPtr<FJsonValue>> Functions;
	for (const FString& Name : RemovedFunctions)
	{
		Functions.Add(MakeShared<FJsonValueString>(Name));
	}
	TArray<TSharedPtr<FJsonValue>> Widgets;
	for (const FString& Name : RemovedWidgets)
	{
		Widgets.Add(MakeShared<FJsonValueString>(Name));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("blueprint_name"), BlueprintName);
	Result->SetStringField(TEXT("widget_name"), WidgetName);
	Result->SetBoolField(TEXT("removed"), bRemoved);
	Result->SetBoolField(TEXT("was_root"), bWasRoot);
	Result->SetArrayField(TEXT("removed_widgets"), Widgets);
	Result->SetArrayField(TEXT("removed_bindings"), RemovedBindingValues);
	TArray<TSharedPtr<FJsonValue>> RemovedAnimationTrackValues;
	for (const TSharedPtr<FJsonObject>& Obj : RemovedAnimationTracks)
	{
		RemovedAnimationTrackValues.Add(MakeShared<FJsonValueObject>(Obj));
	}
	Result->SetArrayField(TEXT("removed_animation_tracks"), RemovedAnimationTrackValues);
	Result->SetArrayField(TEXT("removed_variables"), Variables);
	Result->SetArrayField(TEXT("removed_functions"), Functions);
	Result->SetBoolField(TEXT("has_root"), WidgetBlueprint->WidgetTree->RootWidget != nullptr);
	FinishWidgetEdit(WidgetBlueprint, Params, Result);
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleReparentWidget(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'blueprint_name' parameter"));
	}
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'widget_name' parameter"));
	}
	FString NewParentName;
	if (!Params->TryGetStringField(TEXT("new_parent"), NewParentName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'new_parent' parameter"));
	}

	TSharedPtr<FJsonObject> Error;
	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByParam(BlueprintName, Error);
	if (!WidgetBlueprint)
	{
		return Error;
	}
	UWidget* Widget = FindWidgetOrError(WidgetBlueprint, WidgetName, BlueprintName, Error);
	if (!Widget)
	{
		return Error;
	}
	UWidget* ParentWidget = FindWidgetOrError(WidgetBlueprint, NewParentName, BlueprintName, Error);
	if (!ParentWidget)
	{
		return Error;
	}
	UPanelWidget* NewParent = Cast<UPanelWidget>(ParentWidget);
	if (!NewParent)
	{
		TArray<FString> Panels;
		GatherPanelNames(WidgetBlueprint->WidgetTree, Panels);
		return MakeListError(TEXT("unsupported_parent"), FString::Printf(
			TEXT("Widget '%s' is a %s, which cannot hold children"), *NewParentName,
			*ParentWidget->GetClass()->GetName()), TEXT("panels"), Panels);
	}
	if (!Widget->Slot)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_reparent"),
			FString::Printf(TEXT("Widget '%s' is the root widget and has no parent slot; use set_root_widget"), *WidgetName));
	}
	if (NewParent == Widget)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_reparent"),
			TEXT("A widget cannot be reparented into itself"));
	}
	for (UPanelWidget* Walk = NewParent; Walk; Walk = Walk->GetParent())
	{
		if (Walk == Widget)
		{
			return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_reparent"), FString::Printf(
				TEXT("Panel '%s' is inside '%s'; reparenting would create a cycle"), *NewParentName, *WidgetName));
		}
	}

	UPanelWidget* OldParent = Widget->GetParent();
	const FString OldParentName = OldParent ? OldParent->GetName() : FString();
	const FSlotSnapshot Snapshot = CaptureSlot(Widget->Slot);

	UPanelSlot* NewSlot = NewParent->AddChild(Widget);
	if (!NewSlot)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unsupported_parent"), FString::Printf(
			TEXT("Panel '%s' (%s) refused the child; it holds a limited number"),
			*NewParentName, *NewParent->GetClass()->GetName()));
	}
	TArray<FString> Migrated;
	TArray<FString> Dropped;
	ApplyOverlappingSlotFields(Snapshot, NewSlot, Migrated, Dropped);

	TArray<TSharedPtr<FJsonValue>> MigratedValues;
	for (const FString& Field : Migrated)
	{
		MigratedValues.Add(MakeShared<FJsonValueString>(Field));
	}
	TArray<TSharedPtr<FJsonValue>> DroppedValues;
	for (const FString& Field : Dropped)
	{
		DroppedValues.Add(MakeShared<FJsonValueString>(Field));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("blueprint_name"), BlueprintName);
	Result->SetStringField(TEXT("widget_name"), Widget->GetName());
	Result->SetStringField(TEXT("parent_widget"), NewParent->GetName());
	Result->SetStringField(TEXT("previous_parent"), OldParentName);
	Result->SetArrayField(TEXT("migrated_fields"), MigratedValues);
	Result->SetArrayField(TEXT("dropped_fields"), DroppedValues);
	Result->SetStringField(TEXT("child_order"), PanelChildOrder(NewParent));
	if (OldParent)
	{
		Result->SetStringField(TEXT("previous_parent_child_order"), PanelChildOrder(OldParent));
	}
	if (TSharedPtr<FJsonObject> SlotJson = SlotToJson(Widget->Slot))
	{
		Result->SetObjectField(TEXT("slot"), SlotJson);
	}
	FinishWidgetEdit(WidgetBlueprint, Params, Result);
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleReorderWidget(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'blueprint_name' parameter"));
	}
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'widget_name' parameter"));
	}
	double RequestedIndex = 0.0;
	if (!Params->TryGetNumberField(TEXT("index"), RequestedIndex))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'index' parameter"));
	}

	TSharedPtr<FJsonObject> Error;
	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByParam(BlueprintName, Error);
	if (!WidgetBlueprint)
	{
		return Error;
	}
	UWidget* Widget = FindWidgetOrError(WidgetBlueprint, WidgetName, BlueprintName, Error);
	if (!Widget)
	{
		return Error;
	}
	UPanelWidget* Parent = Widget->GetParent();
	if (!Parent)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"),
			FString::Printf(TEXT("Widget '%s' is the root widget, so it has no sibling order to change"), *WidgetName));
	}

	const int32 IndexBefore = Parent->GetChildIndex(Widget);
	const int32 Count = Parent->GetChildrenCount();
	const int32 TargetIndex = FMath::Clamp((int32)RequestedIndex, 0, FMath::Max(Count - 1, 0));
	Parent->ShiftChild(TargetIndex, Widget);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("blueprint_name"), BlueprintName);
	Result->SetStringField(TEXT("widget_name"), Widget->GetName());
	Result->SetStringField(TEXT("parent_widget"), Parent->GetName());
	Result->SetNumberField(TEXT("index_requested"), (int32)RequestedIndex);
	Result->SetNumberField(TEXT("index_before"), IndexBefore);
	Result->SetNumberField(TEXT("index_after"), Parent->GetChildIndex(Widget));
	Result->SetStringField(TEXT("child_order"), PanelChildOrder(Parent));
	FinishWidgetEdit(WidgetBlueprint, Params, Result);
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleRenameWidget(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'blueprint_name' parameter"));
	}
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'widget_name' parameter"));
	}
	FString NewName;
	if (!Params->TryGetStringField(TEXT("new_name"), NewName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'new_name' parameter"));
	}

	TSharedPtr<FJsonObject> Error;
	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByParam(BlueprintName, Error);
	if (!WidgetBlueprint)
	{
		return Error;
	}
	UWidget* Widget = FindWidgetOrError(WidgetBlueprint, WidgetName, BlueprintName, Error);
	if (!Widget)
	{
		return Error;
	}

	const FName OldFName = Widget->GetFName();
	const FName NewFName = FName(*NewName);
	if (OldFName == NewFName)
	{
		TSharedPtr<FJsonObject> Unchanged = MakeShared<FJsonObject>();
		Unchanged->SetStringField(TEXT("blueprint_name"), BlueprintName);
		Unchanged->SetStringField(TEXT("widget_name"), Widget->GetName());
		Unchanged->SetStringField(TEXT("previous_name"), WidgetName);
		Unchanged->SetBoolField(TEXT("renamed"), false);
		Unchanged->SetBoolField(TEXT("compiled"),
			WidgetBlueprint->Status == BS_UpToDate || WidgetBlueprint->Status == BS_UpToDateWithWarnings);
		return Unchanged;
	}
	if (WidgetBlueprint->WidgetTree->FindWidget(NewFName))
	{
		TArray<FString> Names;
		GatherWidgetNames(WidgetBlueprint->WidgetTree, Names);
		return MakeListError(TEXT("name_collision"), FString::Printf(
			TEXT("A widget named '%s' already exists in '%s'"), *NewName, *BlueprintName),
			TEXT("widget_names"), Names);
	}

	FKismetNameValidator NameValidator(WidgetBlueprint, OldFName);
	if (NameValidator.IsValid(NewFName) != EValidatorResult::Ok)
	{
		TArray<FString> Names;
		GatherWidgetNames(WidgetBlueprint->WidgetTree, Names);
		return MakeListError(TEXT("name_collision"), FString::Printf(
			TEXT("Name '%s' is not available in '%s' (a variable or function of this blueprint or its parent already uses it)"),
			*NewName, *BlueprintName), TEXT("widget_names"), Names);
	}

	const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "RenameWidget", "Rename Widget"));
	WidgetBlueprint->Modify();
	Widget->Modify();

	// Same order the editor's rename uses: rename the template, move the graph references, then the
	// bindings, the navigation rules and the child class variables.
	Widget->SetDisplayLabel(NewName);
	Widget->Rename(*NewName);
	FBlueprintEditorUtils::ReplaceVariableReferences(WidgetBlueprint, OldFName, NewFName);

	int32 UpdatedBindings = 0;
	for (FDelegateEditorBinding& Binding : WidgetBlueprint->Bindings)
	{
		if (Binding.ObjectName == WidgetName)
		{
			Binding.ObjectName = NewName;
			++UpdatedBindings;
		}
	}

	// Animations address their target by widget name (FWidgetAnimationBinding::WidgetName + the
	// possessable it points at). Skipping them here is the same silent failure the binding table
	// would have: the track stays in the asset, the widget is never found, the animation does nothing.
	int32 UpdatedAnimationBindings = 0;
	for (UWidgetAnimation* Animation : WidgetBlueprint->Animations)
	{
		if (!Animation)
		{
			continue;
		}
		UMovieScene* Scene = Animation->GetMovieScene();
		TArray<FGuid> RenamedGuids;
		for (FWidgetAnimationBinding& AnimationBinding : Animation->AnimationBindings)
		{
			if (AnimationBinding.WidgetName == OldFName)
			{
				AnimationBinding.WidgetName = NewFName;
				RenamedGuids.AddUnique(AnimationBinding.AnimationGuid);
				++UpdatedAnimationBindings;
			}
			if (AnimationBinding.SlotWidgetName == OldFName)
			{
				AnimationBinding.SlotWidgetName = NewFName;
				RenamedGuids.AddUnique(AnimationBinding.AnimationGuid);
			}
		}
		if (Scene && RenamedGuids.Num() > 0)
		{
			for (const FGuid& Guid : RenamedGuids)
			{
				if (FMovieScenePossessable* Possessable = Scene->FindPossessable(Guid))
				{
					if (Possessable->GetName() == WidgetName)
					{
						Possessable->SetName(NewName);
					}
				}
			}
		}
	}

	WidgetBlueprint->WidgetTree->ForEachWidget([OldFName, NewFName](UWidget* TreeWidget)
	{
		if (TreeWidget && TreeWidget->Navigation)
		{
			TreeWidget->Navigation->SetFlags(RF_Transactional);
			TreeWidget->Navigation->Modify();
			TreeWidget->Navigation->TryToRenameBinding(OldFName, NewFName);
		}
	});

	FBlueprintEditorUtils::ValidateBlueprintChildVariables(WidgetBlueprint, NewFName);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

	// Read the graph back: these are the nodes that now point at the new name.
	TArray<TSharedPtr<FJsonValue>> ReferenceNodes;
	TArray<UEdGraph*> Graphs;
	WidgetBlueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph)
		{
			continue;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node))
			{
				if (VariableNode->GetVarName() == NewFName && Graph->GetName() != FString::Printf(TEXT("Get%s"), *NewName))
				{
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("graph"), Graph->GetName());
					Obj->SetStringField(TEXT("node"), VariableNode->GetName());
					ReferenceNodes.Add(MakeShared<FJsonValueObject>(Obj));
				}
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("blueprint_name"), BlueprintName);
	Result->SetStringField(TEXT("widget_name"), Widget->GetName());
	Result->SetStringField(TEXT("previous_name"), WidgetName);
	Result->SetBoolField(TEXT("renamed"), true);
	Result->SetNumberField(TEXT("bindings_updated"), UpdatedBindings);
	Result->SetNumberField(TEXT("animation_bindings_updated"), UpdatedAnimationBindings);
	Result->SetArrayField(TEXT("reference_nodes"), ReferenceNodes);
	Result->SetStringField(TEXT("child_order"),
		PanelChildOrder(Widget->GetParent() ? Widget->GetParent() : Cast<UPanelWidget>(Widget)));
	FinishWidgetEdit(WidgetBlueprint, Params, Result);
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleSetRootWidget(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'blueprint_name' parameter"));
	}
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'widget_name' parameter"));
	}

	TSharedPtr<FJsonObject> Error;
	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByParam(BlueprintName, Error);
	if (!WidgetBlueprint)
	{
		return Error;
	}
	UWidget* Widget = FindWidgetOrError(WidgetBlueprint, WidgetName, BlueprintName, Error);
	if (!Widget)
	{
		return Error;
	}

	UWidget* PreviousRoot = WidgetBlueprint->WidgetTree->RootWidget;
	if (PreviousRoot == Widget)
	{
		TSharedPtr<FJsonObject> Unchanged = MakeShared<FJsonObject>();
		Unchanged->SetStringField(TEXT("blueprint_name"), BlueprintName);
		Unchanged->SetStringField(TEXT("root_widget"), Widget->GetName());
		Unchanged->SetStringField(TEXT("previous_root"), Widget->GetName());
		Unchanged->SetBoolField(TEXT("already_root"), true);
		Unchanged->SetBoolField(TEXT("compiled"),
			WidgetBlueprint->Status == BS_UpToDate || WidgetBlueprint->Status == BS_UpToDateWithWarnings);
		return Unchanged;
	}

	// A non-panel new root can only work when there is nothing to keep underneath it.
	UPanelWidget* NewRootPanel = Cast<UPanelWidget>(Widget);
	if (PreviousRoot && !NewRootPanel)
	{
		TArray<FString> Panels;
		GatherPanelNames(WidgetBlueprint->WidgetTree, Panels);
		return MakeListError(TEXT("unsupported_root_panel"), FString::Printf(
			TEXT("'%s' is a %s, so it cannot keep the existing hierarchy under it; pick a panel"),
			*WidgetName, *Widget->GetClass()->GetName()), TEXT("panels"), Panels);
	}

	UPanelWidget* PreviousParent = Widget->GetParent();
	const FString PreviousParentName = PreviousParent ? PreviousParent->GetName() : FString();
	if (PreviousParent)
	{
		PreviousParent->RemoveChild(Widget);
	}
	WidgetBlueprint->WidgetTree->RootWidget = Widget;

	if (PreviousRoot && !NewRootPanel->AddChild(PreviousRoot))
	{
		// Put the tree back the way it was before reporting the refusal.
		WidgetBlueprint->WidgetTree->RootWidget = PreviousRoot;
		if (PreviousParent)
		{
			PreviousParent->AddChild(Widget);
		}
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unsupported_root_panel"), FString::Printf(
			TEXT("Panel '%s' (%s) cannot take the previous root as a child; it holds a limited number of children"),
			*WidgetName, *NewRootPanel->GetClass()->GetName()));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("blueprint_name"), BlueprintName);
	Result->SetStringField(TEXT("root_widget"), Widget->GetName());
	Result->SetStringField(TEXT("previous_root"), PreviousRoot ? PreviousRoot->GetName() : FString());
	Result->SetStringField(TEXT("previous_parent"), PreviousParentName);
	Result->SetBoolField(TEXT("already_root"), false);
	if (NewRootPanel)
	{
		Result->SetStringField(TEXT("root_child_order"), PanelChildOrder(NewRootPanel));
	}
	FinishWidgetEdit(WidgetBlueprint, Params, Result);
	return Result;
}