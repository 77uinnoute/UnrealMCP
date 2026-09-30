// UMG runtime introspection: read the live widget tree of a widget blueprint instance in the PIE
// world. Python cannot reach UUserWidget::WidgetTree, reads the widget variables of a generated
// class instance back as None and has no WidgetBlueprintLibrary, so this is the only way to see
// what a running UI actually holds (text, visibility, geometry).
#include "Commands/UnrealMCPUMGCommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "WidgetBlueprint.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Components/Image.h"
#include "Components/Button.h"
#include "Layout/Geometry.h"
#include "UObject/UObjectIterator.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#include "Editor.h"
#include "Commands/UMG/UnrealMCPUMGCommandHelpers.h"

namespace
{
	TArray<TSharedPtr<FJsonValue>> Vec2Json(const FVector2D& V)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Add(JNum(V.X));
		Out.Add(JNum(V.Y));
		return Out;
	}

	TArray<TSharedPtr<FJsonValue>> ColorJson(const FLinearColor& C)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Add(JNum(C.R));
		Out.Add(JNum(C.G));
		Out.Add(JNum(C.B));
		Out.Add(JNum(C.A));
		return Out;
	}

	/** Visible itself and every parent up the chain (including the owning user widgets) visible. */
	bool IsEffectivelyVisible(const UWidget* Widget)
	{
		for (const UWidget* Cursor = Widget; Cursor; )
		{
			if (!Cursor->IsVisible())
			{
				return false;
			}
			if (const UPanelWidget* Parent = Cursor->GetParent())
			{
				Cursor = Parent;
				continue;
			}
			// The root of a (nested) user widget's tree: continue at the user widget that owns it.
			const UWidgetTree* Tree = Cursor->GetTypedOuter<UWidgetTree>();
			Cursor = Tree ? Tree->GetTypedOuter<UUserWidget>() : nullptr;
		}
		return true;
	}

	TSharedPtr<FJsonObject> RuntimeWidgetToJson(UWidget* Widget, int32 Depth, int32 MaxDepth)
	{
		TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("name"), Widget->GetName());
		Node->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
		Node->SetStringField(TEXT("visibility"), EnumLabel(StaticEnum<ESlateVisibility>(), (int64)Widget->GetVisibility()));
		Node->SetBoolField(TEXT("is_visible"), IsEffectivelyVisible(Widget));
		Node->SetNumberField(TEXT("render_opacity"), Widget->GetRenderOpacity());

		const FWidgetTransform& Transform = Widget->GetRenderTransform();
		TSharedPtr<FJsonObject> TransformJson = MakeShared<FJsonObject>();
		TransformJson->SetArrayField(TEXT("translation"), Vec2Json(Transform.Translation));
		TransformJson->SetArrayField(TEXT("scale"), Vec2Json(Transform.Scale));
		TransformJson->SetArrayField(TEXT("shear"), Vec2Json(Transform.Shear));
		TransformJson->SetNumberField(TEXT("angle"), Transform.Angle);
		Node->SetObjectField(TEXT("render_transform"), TransformJson);

		Node->SetArrayField(TEXT("desired_size"), Vec2Json(Widget->GetDesiredSize()));
		const FGeometry& Geometry = Widget->GetCachedGeometry();
		const FVector2D LocalSize = FVector2D(Geometry.GetLocalSize());
		TSharedPtr<FJsonObject> GeometryJson = MakeShared<FJsonObject>();
		GeometryJson->SetArrayField(TEXT("absolute_position"), Vec2Json(FVector2D(Geometry.GetAbsolutePosition())));
		GeometryJson->SetArrayField(TEXT("absolute_size"), Vec2Json(FVector2D(Geometry.GetAbsoluteSize())));
		GeometryJson->SetArrayField(TEXT("local_size"), Vec2Json(LocalSize));
		Node->SetObjectField(TEXT("geometry"), GeometryJson);
		Node->SetBoolField(TEXT("has_geometry"), LocalSize.X > 0.0 || LocalSize.Y > 0.0);

		if (TSharedPtr<FJsonObject> SlotJson = SlotToJson(Widget->Slot))
		{
			Node->SetObjectField(TEXT("slot"), SlotJson);
		}

		if (const UTextBlock* Text = Cast<UTextBlock>(Widget))
		{
			Node->SetStringField(TEXT("text"), Text->GetText().ToString());
			Node->SetNumberField(TEXT("font_size"), Text->GetFont().Size);
			Node->SetStringField(TEXT("font_object"), Text->GetFont().FontObject ? Text->GetFont().FontObject->GetPathName() : FString());
			Node->SetArrayField(TEXT("color"), ColorJson(Text->GetColorAndOpacity().GetSpecifiedColor()));
			Node->SetBoolField(TEXT("auto_wrap"), Text->GetAutoWrapText());
			Node->SetNumberField(TEXT("wrap_text_at"), Text->GetWrapTextAt());
		}
		else if (const UImage* Image = Cast<UImage>(Widget))
		{
			const FSlateBrush& Brush = Image->GetBrush();
			UObject* Resource = Brush.GetResourceObject();
			Node->SetStringField(TEXT("brush_resource"), Resource ? Resource->GetPathName() : FString());
			Node->SetArrayField(TEXT("image_size"), Vec2Json(FVector2D(Brush.GetImageSize())));
			Node->SetArrayField(TEXT("tint"), ColorJson(Image->GetColorAndOpacity()));
		}
		else if (const UButton* Button = Cast<UButton>(Widget))
		{
			Node->SetBoolField(TEXT("is_hovered"), Button->IsHovered());
			Node->SetBoolField(TEXT("is_pressed"), Button->IsPressed());
		}

		TArray<TSharedPtr<FJsonValue>> Children;
		const bool bDescend = MaxDepth < 0 || Depth < MaxDepth;
		if (bDescend)
		{
			if (const UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
			{
				for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
				{
					if (UWidget* Child = Panel->GetChildAt(Index))
					{
						Children.Add(MakeShared<FJsonValueObject>(RuntimeWidgetToJson(Child, Depth + 1, MaxDepth)));
					}
				}
			}
			else if (const UUserWidget* Nested = Cast<UUserWidget>(Widget))
			{
				if (Nested->WidgetTree && Nested->WidgetTree->RootWidget)
				{
					Children.Add(MakeShared<FJsonValueObject>(RuntimeWidgetToJson(Nested->WidgetTree->RootWidget, Depth + 1, MaxDepth)));
				}
			}
		}
		Node->SetNumberField(TEXT("child_count"), Children.Num());
		Node->SetArrayField(TEXT("children"), Children);
		return Node;
	}
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleGetPIEWidgetTree(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'blueprint_name' parameter"));
	}

	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(FUnrealMCPCommonUtils::FindAsset(BlueprintName));
	if (!WidgetBlueprint || !WidgetBlueprint->GeneratedClass)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("widget_blueprint_not_found"),
			FString::Printf(TEXT("Widget Blueprint '%s' not found"), *BlueprintName));
	}

	UWorld* PlayWorld = GEditor ? GEditor->PlayWorld : nullptr;
	if (!PlayWorld)
	{
		TSharedPtr<FJsonObject> Result = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("pie_not_running"),
			TEXT("No PIE session is running; runtime widget instances only exist in a game world"));
		Result->SetStringField(TEXT("hint"), TEXT("Call start_pie, create the widget (in game or with add_widget_to_viewport), then read it in a later call"));
		return Result;
	}

	int32 InstanceIndex = 0;
	Params->TryGetNumberField(TEXT("instance_index"), InstanceIndex);
	int32 MaxDepth = -1;
	Params->TryGetNumberField(TEXT("max_depth"), MaxDepth);
	FString RootName;
	Params->TryGetStringField(TEXT("root_widget"), RootName);

	UClass* WidgetClass = WidgetBlueprint->GeneratedClass;
	TArray<UUserWidget*> Instances;
	TSet<FString> PresentClasses;
	for (TObjectIterator<UUserWidget> It; It; ++It)
	{
		UUserWidget* Candidate = *It;
		if (!Candidate || Candidate->IsTemplate() || Candidate->GetWorld() != PlayWorld)
		{
			continue;
		}
		PresentClasses.Add(Candidate->GetClass()->GetName());
		if (Candidate->IsA(WidgetClass))
		{
			Instances.Add(Candidate);
		}
	}
	// Unique ids grow with allocation: a stable stand-in for creation order.
	Instances.Sort([](const UUserWidget& A, const UUserWidget& B) { return A.GetUniqueID() < B.GetUniqueID(); });

	if (!Instances.IsValidIndex(InstanceIndex))
	{
		TArray<FString> Classes = PresentClasses.Array();
		Classes.Sort();
		TSharedPtr<FJsonObject> Result = MakeListError(TEXT("widget_instance_not_found"), FString::Printf(
			TEXT("The PIE world holds %d instance(s) of %s; instance_index %d is not one of them"),
			Instances.Num(), *WidgetClass->GetName(), InstanceIndex), TEXT("candidates"), Classes);
		Result->SetNumberField(TEXT("instance_count"), Instances.Num());
		return Result;
	}

	UUserWidget* Instance = Instances[InstanceIndex];
	UWidget* Root = Instance->WidgetTree ? Instance->WidgetTree->RootWidget : nullptr;
	if (!RootName.IsEmpty())
	{
		UWidget* Found = Instance->WidgetTree ? Instance->WidgetTree->FindWidget(FName(*RootName)) : nullptr;
		if (!Found)
		{
			TArray<FString> Names;
			GatherWidgetNames(Instance->WidgetTree, Names);
			return MakeListError(TEXT("widget_not_found"), FString::Printf(
				TEXT("Widget '%s' is not in the instance tree of %s"), *RootName, *Instance->GetName()),
				TEXT("widget_names"), Names);
		}
		Root = Found;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint_name"), BlueprintName);
	Result->SetNumberField(TEXT("instance_count"), Instances.Num());
	Result->SetNumberField(TEXT("instance_index"), InstanceIndex);
	Result->SetStringField(TEXT("instance_name"), Instance->GetName());
	// The object path is what python needs to reach this exact instance (unreal.load_object).
	Result->SetStringField(TEXT("instance_path"), Instance->GetPathName());
	Result->SetBoolField(TEXT("in_viewport"), Instance->IsInViewport());
	const APlayerController* Owner = Instance->GetOwningPlayer();
	Result->SetStringField(TEXT("owning_player"), Owner ? Owner->GetClass()->GetName() : FString());
	Result->SetStringField(TEXT("pie_world"), PlayWorld->GetName());
	Result->SetStringField(TEXT("geometry_frame"), TEXT("last_paint"));
	// Cached geometry is filled by the layout pass that runs inside a draw. Slate skips tick/draw
	// while the user is idle (Slate.AllowSlateToSleep, default on in the editor,
	// SlateApplication.cpp:572-1712) and the editor's "Use Less CPU in Background" stops the
	// viewports (EditorEngine.cpp:1768), so a tree can be on screen and still hold zero geometry.
	// Ask for one forced redraw of the PIE window (SlateApplication.cpp:4184) before giving up.
	bool bGeometryRefreshed = false;
	TSharedPtr<FJsonObject> RootJson;
	if (Root)
	{
		RootJson = RuntimeWidgetToJson(Root, 0, MaxDepth);
	}

	TSharedPtr<SWindow> PIEWindow;
	if (RootJson.IsValid() && !RootJson->GetBoolField(TEXT("has_geometry")) && FSlateApplication::IsInitialized())
	{
		UGameViewportClient* GameViewport = PlayWorld->GetGameViewport();
		TSharedPtr<SViewport> ViewportWidget = GameViewport ? GameViewport->GetGameViewportWidget() : nullptr;
		if (ViewportWidget.IsValid())
		{
			PIEWindow = FSlateApplication::Get().FindWidgetWindow(ViewportWidget.ToSharedRef());
			if (PIEWindow.IsValid())
			{
				FSlateApplication::Get().ForceRedrawWindow(PIEWindow.ToSharedRef());
				RootJson = RuntimeWidgetToJson(Root, 0, MaxDepth);
				bGeometryRefreshed = true;
			}
		}
	}

	if (RootJson.IsValid())
	{
		// Still nothing: say why, so zeros are not read as "the layout collapsed".
		if (!RootJson->GetBoolField(TEXT("has_geometry")))
		{
			FString Reason = FString::Printf(
				TEXT("%s Slate only arranges a tree while it is drawing, so the PIE window has to be on screen and painting; desired_size is still valid."),
				bGeometryRefreshed ? TEXT("a forced redraw still produced no geometry:") : TEXT("no cached geometry yet:"));
			if (PIEWindow.IsValid())
			{
				Reason += FString::Printf(TEXT(" window_visible=%s window_minimized=%s"),
					PIEWindow->IsVisible() ? TEXT("true") : TEXT("false"),
					PIEWindow->IsWindowMinimized() ? TEXT("true") : TEXT("false"));
			}
			RootJson->SetStringField(TEXT("geometry_hint"), Reason);
		}
		Result->SetObjectField(TEXT("root"), RootJson);
	}
	Result->SetBoolField(TEXT("geometry_refreshed"), bGeometryRefreshed);
	Result->SetBoolField(TEXT("has_root"), Root != nullptr);
	return Result;
}
