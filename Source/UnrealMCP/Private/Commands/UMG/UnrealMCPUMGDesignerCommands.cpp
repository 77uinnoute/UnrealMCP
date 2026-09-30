// UMG designer session: open the widget designer, set the design preview size, and drive the
// designer's animation preview. None of this is reachable from python (editor toolkit state).
#include "Commands/UnrealMCPUMGCommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "WidgetBlueprint.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/UMGSequencePlayer.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "WidgetBlueprintEditor.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Editor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Commands/UMG/UnrealMCPUMGCommandHelpers.h"

namespace
{
	UWidgetAnimation* FindAssetAnimation(UWidgetBlueprint* Blueprint, const FString& AnimationName)
	{
		if (!Blueprint)
		{
			return nullptr;
		}
		for (UWidgetAnimation* Animation : Blueprint->Animations)
		{
			if (Animation && Animation->GetName() == AnimationName)
			{
				return Animation;
			}
		}
		return nullptr;
	}

	/**
	 * The designer preview plays its own copies of the animations (the editor duplicates the asset's
	 * animations onto the preview class), so the asset object itself cannot be played there.
	 */
	UWidgetAnimation* FindPreviewAnimation(UUserWidget* Preview, const FString& AnimationName)
	{
		UWidgetBlueprintGeneratedClass* PreviewClass = Preview
			? Cast<UWidgetBlueprintGeneratedClass>(Preview->GetClass()) : nullptr;
		if (!PreviewClass)
		{
			return nullptr;
		}
		for (UWidgetAnimation* Animation : PreviewClass->Animations)
		{
			if (!Animation)
			{
				continue;
			}
			const FString Name = Animation->GetName();
			if (Name == AnimationName || Name.StartsWith(AnimationName + TEXT("_")))
			{
				return Animation;
			}
		}
		return nullptr;
	}

	TArray<TSharedPtr<FJsonValue>> AnimationNamesToJson(UWidgetBlueprint* Blueprint)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (UWidgetAnimation* Animation : Blueprint->Animations)
		{
			if (Animation)
			{
				Items.Add(MakeShared<FJsonValueString>(Animation->GetName()));
			}
		}
		return Items;
	}

	TArray<FString> AnimationNameList(UWidgetBlueprint* Blueprint)
	{
		TArray<FString> Names;
		if (!Blueprint)
		{
			return Names;
		}
		for (UWidgetAnimation* Animation : Blueprint->Animations)
		{
			if (Animation)
			{
				Names.Add(Animation->GetName());
			}
		}
		return Names;
	}

	EUMGSequencePlayMode::Type ParsePlayMode(const FString& Raw, bool& bOutValid)
	{
		const FString Mode = Raw.TrimStartAndEnd();
		bOutValid = true;
		if (Mode.Equals(TEXT("forward"), ESearchCase::IgnoreCase) || Mode.IsEmpty())
		{
			return EUMGSequencePlayMode::Forward;
		}
		if (Mode.Equals(TEXT("reverse"), ESearchCase::IgnoreCase))
		{
			return EUMGSequencePlayMode::Reverse;
		}
		if (Mode.Equals(TEXT("pingpong"), ESearchCase::IgnoreCase) || Mode.Equals(TEXT("ping_pong"), ESearchCase::IgnoreCase))
		{
			return EUMGSequencePlayMode::PingPong;
		}
		bOutValid = false;
		return EUMGSequencePlayMode::Forward;
	}
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleOpenUMGDesigner(const TSharedPtr<FJsonObject>& Params)
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

	UAssetEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	if (!Subsystem)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("editor_not_open"),
			TEXT("No asset editor subsystem: the editor session is unavailable"));
	}

	// The engine records where an asset should open next time when its tab closes
	// (SStandaloneAssetEditorToolkitHost::OnTabClosed writes [AssetEditorToolkitTabLocation] in
	// GEditorPerProjectIni). Capture the record and put it back, exactly like close_asset_editors:
	// an MCP-driven open must not change where the user sees the asset afterwards.
	const TCHAR* const TabLocationSection = TEXT("AssetEditorToolkitTabLocation");
	const FString TabLocationKey = Blueprint->GetPathName();
	int32 PreviousLocation = INDEX_NONE;
	const bool bHadRecord = GConfig->GetInt(TabLocationSection, *TabLocationKey, PreviousLocation, GEditorPerProjectIni);

	const bool bOpened = Subsystem->OpenEditorForAsset(Blueprint, EToolkitMode::Standalone,
		TSharedPtr<IToolkitHost>(), /*bShowProgressWindow=*/false);

	if (bHadRecord)
	{
		GConfig->SetInt(TabLocationSection, *TabLocationKey, PreviousLocation, GEditorPerProjectIni);
	}
	else
	{
		GConfig->RemoveKey(TabLocationSection, *TabLocationKey, GEditorPerProjectIni);
	}
	GConfig->Flush(false, GEditorPerProjectIni);

	if (!bOpened)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("open_failed"), FString::Printf(
			TEXT("The asset editor subsystem refused to open '%s'"), *Blueprint->GetName()));
	}

	FWidgetBlueprintEditor* Editor = FindWidgetBlueprintEditor(Blueprint);

	FString RequestedFocus;
	const bool bFocusRequested = Params->TryGetStringField(TEXT("focus_widget"), RequestedFocus)
		&& !RequestedFocus.IsEmpty();
	bool bFocused = false;
	FString FocusReason;
	if (bFocusRequested)
	{
		UWidget* Widget = Blueprint->WidgetTree ? Blueprint->WidgetTree->FindWidget(FName(*RequestedFocus)) : nullptr;
		if (!Widget)
		{
			TArray<FString> Names;
			GatherWidgetNames(Blueprint->WidgetTree, Names);
			return MakeListError(TEXT("widget_not_found"), FString::Printf(
				TEXT("Widget '%s' is not in '%s'"), *RequestedFocus, *Blueprint->GetName()), TEXT("widgets"), Names);
		}
		if (!Editor)
		{
			FocusReason = TEXT("designer_not_open");
		}
		else
		{
			// The widget template is what the designer owns; the reference is only valid once its
			// preview exists, so a failure here means "opened, but not selectable yet".
			FWidgetReference Reference = Editor->GetReferenceFromTemplate(Widget);
			if (Reference.IsValid())
			{
				Editor->SelectWidgets(TSet<FWidgetReference>{ Reference }, /*bAppendOrToggle=*/false);
				bFocused = true;
			}
			else
			{
				FocusReason = TEXT("designer_not_ready");
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
	Result->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Result->SetBoolField(TEXT("opened"), true);
	Result->SetBoolField(TEXT("editor_open"), Editor != nullptr);
	Result->SetStringField(TEXT("editor_name"), Editor ? TEXT("WidgetBlueprintEditor") : FString());
	Result->SetBoolField(TEXT("has_preview"), Editor && Editor->GetPreview() != nullptr);
	Result->SetBoolField(TEXT("prior_tab_location_known"), bHadRecord);
	Result->SetNumberField(TEXT("prior_tab_location"), bHadRecord ? PreviousLocation : -1);
	if (bFocusRequested)
	{
		Result->SetStringField(TEXT("focus_widget"), RequestedFocus);
		Result->SetBoolField(TEXT("focused"), bFocused);
		Result->SetStringField(TEXT("focus_reason"), FocusReason);
	}
	Result->SetArrayField(TEXT("animations"), AnimationNamesToJson(Blueprint));
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleSetUMGDesignSize(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'blueprint_name' parameter"));
	}

	double Width = 0.0;
	double Height = 0.0;
	const bool bHasWidth = Params->TryGetNumberField(TEXT("width"), Width);
	const bool bHasHeight = Params->TryGetNumberField(TEXT("height"), Height);
	const bool bHasDpi = Params->HasField(TEXT("dpi_scale"));
	const bool bHasPlatform = Params->HasField(TEXT("preview_platform"));

	if (!bHasWidth && !bHasHeight && !bHasDpi && !bHasPlatform)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"),
			TEXT("Pass at least one of 'width' / 'height'"));
	}

	// Honest boundary of what the designer actually stores. The "DPI" readout is derived from the
	// project's UI scale curve (FWidgetBlueprintEditorUtils::GetWidgetPreviewDPIScale ->
	// UUserInterfaceSettings::GetDPIScaleBasedOnSize) and the preview platform is the private device
	// profile of the designer's own view (SDesignerView::PreviewOverrideName) - neither has a field a
	// command could write per asset. Refuse the whole call rather than half-applying it.
	TArray<TSharedPtr<FJsonValue>> Unsupported;
	if (bHasDpi)
	{
		Unsupported.Add(MakeShared<FJsonValueString>(TEXT("dpi_scale")));
	}
	if (bHasPlatform)
	{
		Unsupported.Add(MakeShared<FJsonValueString>(TEXT("preview_platform")));
	}
	if (Unsupported.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Supported;
		Supported.Add(MakeShared<FJsonValueString>(TEXT("width")));
		Supported.Add(MakeShared<FJsonValueString>(TEXT("height")));
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error_code"), TEXT("unsupported"));
		Result->SetStringField(TEXT("message"),
			TEXT("The designer has no per-asset DPI scale or preview platform to write: the DPI readout is derived from the project's UI scale curve, and the preview platform is the designer view's own device profile"));
		Result->SetArrayField(TEXT("unsupported_fields"), Unsupported);
		Result->SetArrayField(TEXT("supported_fields"), Supported);
		return Result;
	}

	TSharedPtr<FJsonObject> Error;
	UWidgetBlueprint* Blueprint = LoadWidgetBlueprintByParam(BlueprintName, Error);
	if (!Blueprint)
	{
		return Error;
	}

	UUserWidget* DesignCDO = Blueprint->GeneratedClass
		? Blueprint->GeneratedClass->GetDefaultObject<UUserWidget>() : nullptr;
	if (!DesignCDO)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("blueprint_not_ready"),
			TEXT("The widget blueprint has no generated class yet; compile it first"));
	}

	UEnum* ModeEnum = StaticEnum<EDesignPreviewSizeMode>();
	auto ModeLabel = [ModeEnum, DesignCDO]()
	{
		return ModeEnum ? ModeEnum->GetNameStringByValue((int64)DesignCDO->DesignSizeMode) : FString();
	};

	const FString ModeBefore = ModeLabel();
	const double WidthBefore = DesignCDO->DesignTimeSize.X;
	const double HeightBefore = DesignCDO->DesignTimeSize.Y;

	TArray<TSharedPtr<FJsonValue>> Applied;
	DesignCDO->Modify();
	Blueprint->Modify();
	if (bHasWidth)
	{
		DesignCDO->DesignTimeSize.X = Width;
		Applied.Add(MakeShared<FJsonValueString>(TEXT("width")));
	}
	if (bHasHeight)
	{
		DesignCDO->DesignTimeSize.Y = Height;
		Applied.Add(MakeShared<FJsonValueString>(TEXT("height")));
	}
	// A size is only stored (and only used) in the custom modes; that is the mode the designer's own
	// resolution boxes write into, so asking for a size means switching to it.
	const bool bSizeModeChanged = DesignCDO->DesignSizeMode != EDesignPreviewSizeMode::Custom
		&& DesignCDO->DesignSizeMode != EDesignPreviewSizeMode::CustomOnScreen;
	if (bSizeModeChanged)
	{
		DesignCDO->DesignSizeMode = EDesignPreviewSizeMode::Custom;
		Applied.Add(MakeShared<FJsonValueString>(TEXT("design_size_mode")));
	}

	// What the designer itself does for a design-only change (no recompile needed).
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	TSharedPtr<FJsonObject> Before = MakeShared<FJsonObject>();
	Before->SetStringField(TEXT("mode"), ModeBefore);
	Before->SetNumberField(TEXT("width"), WidthBefore);
	Before->SetNumberField(TEXT("height"), HeightBefore);

	TSharedPtr<FJsonObject> After = MakeShared<FJsonObject>();
	After->SetStringField(TEXT("mode"), ModeLabel());
	After->SetNumberField(TEXT("width"), DesignCDO->DesignTimeSize.X);
	After->SetNumberField(TEXT("height"), DesignCDO->DesignTimeSize.Y);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
	Result->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Result->SetArrayField(TEXT("applied"), Applied);
	Result->SetObjectField(TEXT("design_size_before"), Before);
	Result->SetObjectField(TEXT("design_size"), After);
	Result->SetBoolField(TEXT("size_mode_changed"), bSizeModeChanged);
	Result->SetStringField(TEXT("scope"),
		TEXT("designer preview only: the runtime size is decided by the widget's parent at play time"));
	FinishWidgetWrite(Blueprint, Params, Result);
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandlePlayWidgetAnimationPreview(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'blueprint_name' parameter"));
	}
	FString AnimationName;
	if (!Params->TryGetStringField(TEXT("animation_name"), AnimationName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'animation_name' parameter"));
	}

	TSharedPtr<FJsonObject> Error;
	UWidgetBlueprint* Blueprint = LoadWidgetBlueprintByParam(BlueprintName, Error);
	if (!Blueprint)
	{
		return Error;
	}

	if (!FindAssetAnimation(Blueprint, AnimationName))
	{
		return MakeListError(TEXT("animation_not_found"), FString::Printf(
			TEXT("'%s' has no animation named '%s'"), *Blueprint->GetName(), *AnimationName),
			TEXT("animations"), AnimationNameList(Blueprint));
	}

	FWidgetBlueprintEditor* Editor = FindWidgetBlueprintEditor(Blueprint);
	if (!Editor)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("editor_not_open"),
			TEXT("The designer is not open; call open_umg_designer first - the preview only exists in a live designer session"));
	}

	UUserWidget* Preview = Editor->GetPreview();
	if (!Preview)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("preview_not_ready"),
			TEXT("The designer has no preview widget yet; retry once the designer has drawn"));
	}

	UWidgetAnimation* PreviewAnimation = FindPreviewAnimation(Preview, AnimationName);
	if (!PreviewAnimation)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("animation_not_in_preview"),
			TEXT("The preview holds its own copies of the animations and does not have this one yet; call compile_umg_widget and retry (the preview refreshes on the designer's next tick)"));
	}

	double StartAtTime = 0.0;
	Params->TryGetNumberField(TEXT("start_at_time"), StartAtTime);
	double Loops = 1.0;
	Params->TryGetNumberField(TEXT("loops"), Loops);
	double Speed = 1.0;
	Params->TryGetNumberField(TEXT("speed"), Speed);
	FString ModeRaw;
	Params->TryGetStringField(TEXT("play_mode"), ModeRaw);
	bool bModeValid = true;
	const EUMGSequencePlayMode::Type PlayMode = ParsePlayMode(ModeRaw, bModeValid);
	if (!bModeValid)
	{
		TArray<TSharedPtr<FJsonValue>> Modes;
		Modes.Add(MakeShared<FJsonValueString>(TEXT("forward")));
		Modes.Add(MakeShared<FJsonValueString>(TEXT("reverse")));
		Modes.Add(MakeShared<FJsonValueString>(TEXT("pingpong")));
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error_code"), TEXT("invalid_value"));
		Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Unknown play_mode '%s'"), *ModeRaw));
		Result->SetArrayField(TEXT("modes"), Modes);
		return Result;
	}

	UUMGSequencePlayer* Player = Preview->PlayAnimation(PreviewAnimation,
		(float)StartAtTime, FMath::Max(1, (int32)Loops), PlayMode, (float)Speed, /*bRestoreState=*/false);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetBoolField(TEXT("playing"), Player != nullptr);
	Result->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
	Result->SetStringField(TEXT("animation_name"), AnimationName);
	Result->SetStringField(TEXT("preview_animation_name"), PreviewAnimation->GetName());
	Result->SetStringField(TEXT("playback_context"), TEXT("designer_preview"));
	Result->SetStringField(TEXT("play_mode"),
		PlayMode == EUMGSequencePlayMode::Reverse ? TEXT("reverse")
		: PlayMode == EUMGSequencePlayMode::PingPong ? TEXT("pingpong") : TEXT("forward"));
	Result->SetNumberField(TEXT("start_at_time"), StartAtTime);
	Result->SetNumberField(TEXT("loops"), FMath::Max(1, (int32)Loops));
	Result->SetNumberField(TEXT("speed"), Speed);
	// The preview is a runtime instance in its own world: nothing here touches the asset.
	Result->SetBoolField(TEXT("asset_modified"), false);
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleStopWidgetAnimationPreview(const TSharedPtr<FJsonObject>& Params)
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

	FWidgetBlueprintEditor* Editor = FindWidgetBlueprintEditor(Blueprint);
	if (!Editor)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("editor_not_open"),
			TEXT("The designer is not open; call open_umg_designer first"));
	}

	UUserWidget* Preview = Editor->GetPreview();
	if (!Preview)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("preview_not_ready"),
			TEXT("The designer has no preview widget yet; retry once the designer has drawn"));
	}

	FString AnimationName;
	const bool bNamed = Params->TryGetStringField(TEXT("animation_name"), AnimationName) && !AnimationName.IsEmpty();
	FString StoppedName;
	if (bNamed)
	{
		UWidgetAnimation* PreviewAnimation = FindPreviewAnimation(Preview, AnimationName);
		if (!PreviewAnimation)
		{
			return MakeListError(TEXT("animation_not_in_preview"), FString::Printf(
				TEXT("The preview does not hold '%s'; call compile_umg_widget and retry"), *AnimationName),
				TEXT("animations"), AnimationNameList(Blueprint));
		}
		Preview->StopAnimation(PreviewAnimation);
		StoppedName = PreviewAnimation->GetName();
	}
	else
	{
		Preview->StopAllAnimations();
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetBoolField(TEXT("stopped"), true);
	Result->SetStringField(TEXT("mode"), bNamed ? TEXT("one") : TEXT("all"));
	Result->SetStringField(TEXT("animation_name"), bNamed ? AnimationName : FString());
	Result->SetStringField(TEXT("preview_animation_name"), StoppedName);
	Result->SetStringField(TEXT("playback_context"), TEXT("designer_preview"));
	return Result;
}
