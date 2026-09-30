// UMG widget animation authoring: the animations on a widget blueprint, the widget+property tracks
// inside them, and the keyframes on.
#include "Commands/UnrealMCPUMGCommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Compat/UnrealMCPVersionCompat.h"
#include "WidgetBlueprint.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/Widget.h"
#include "Components/SlateWrapperTypes.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "MovieScene.h"
#include "MovieSceneSection.h"
#include "Tracks/MovieSceneByteTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Sections/MovieSceneByteSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "WidgetBlueprintEditor.h"
#include "Commands/UMG/UnrealMCPUMGCommandHelpers.h"

namespace
{
	/** How a widget property is stored inside a movie scene. */
	enum class EUMGTrackKind
	{
		Float,      // RenderOpacity     -> UMovieSceneFloatTrack
		Byte,       // Visibility        -> UMovieSceneByteTrack (an enum property)
		Transform,  // RenderTransform   -> UMovieScene2DTransformTrack (FWidgetTransform channels)
	};

	struct FUMGTrackSpec
	{
		EUMGTrackKind Kind = EUMGTrackKind::Float;
		FName PropertyName;
	};

	void SupportedProperties(TArray<FString>& Out)
	{
		Out.Reset();
		Out.Add(TEXT("RenderOpacity"));
		Out.Add(TEXT("Visibility"));
		Out.Add(TEXT("RenderTransform"));
	}

	/** The properties this command set can animate, and the track each one needs. */
	bool ResolveTrackSpec(const FString& Raw, FUMGTrackSpec& Out)
	{
		FString Key = Raw.TrimStartAndEnd();
		Key.ReplaceInline(TEXT("_"), TEXT(""));
		if (Key.Equals(TEXT("RenderOpacity"), ESearchCase::IgnoreCase) || Key.Equals(TEXT("Opacity"), ESearchCase::IgnoreCase))
		{
			Out.Kind = EUMGTrackKind::Float;
			Out.PropertyName = FName(TEXT("RenderOpacity"));
			return true;
		}
		if (Key.Equals(TEXT("Visibility"), ESearchCase::IgnoreCase))
		{
			Out.Kind = EUMGTrackKind::Byte;
			Out.PropertyName = FName(TEXT("Visibility"));
			return true;
		}
		if (Key.Equals(TEXT("RenderTransform"), ESearchCase::IgnoreCase) || Key.Equals(TEXT("Transform"), ESearchCase::IgnoreCase))
		{
			Out.Kind = EUMGTrackKind::Transform;
			Out.PropertyName = FName(TEXT("RenderTransform"));
			return true;
		}
		return false;
	}

	UWidgetAnimation* FindAnimation(UWidgetBlueprint* Blueprint, const FString& AnimationName)
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

	TArray<FString> AnimationNames(UWidgetBlueprint* Blueprint)
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

	/** The possessable that carries a widget: guid -> widget name lives in the animation's bindings. */
	FGuid FindWidgetPossessable(UWidgetAnimation* Animation, const FName WidgetName)
	{
		if (!Animation)
		{
			return FGuid();
		}
		for (const FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
		{
			if (Binding.WidgetName == WidgetName)
			{
				return Binding.AnimationGuid;
			}
		}
		return FGuid();
	}

	/**
	 * The object UWidgetAnimation::BindPossessableObject wants as its context. The designer preview
	 * when there is one; otherwise a transient instance of the same class, which is enough because
	 * the engine only compares that context against the possessed object.
	 */
	UObject* ResolvePlaybackContext(UWidgetBlueprint* Blueprint)
	{
		if (FWidgetBlueprintEditor* Editor = FindWidgetBlueprintEditor(Blueprint))
		{
			if (UUserWidget* Preview = Editor->GetPreview())
			{
				return Preview;
			}
		}
		return Blueprint->GeneratedClass
			? NewObject<UUserWidget>(GetTransientPackage(), Blueprint->GeneratedClass) : nullptr;
	}

	/** Add the possessable for a widget and register the binding the runtime resolves the widget by. */
	FGuid EnsureWidgetPossessable(UWidgetBlueprint* Blueprint, UWidgetAnimation* Animation, UWidget* Widget,
	                              TArray<FString>& OutWarnings)
	{
		if (const FGuid Existing = FindWidgetPossessable(Animation, Widget->GetFName()); Existing.IsValid())
		{
			return Existing;
		}

		UMovieScene* Scene = Animation->GetMovieScene();
		const FGuid Guid = Scene->AddPossessable(Widget->GetName(), UWidget::StaticClass());
		// Always route through the engine's own writer: it fills WidgetName / SlotWidgetName /
		// bIsRootWidget exactly the way the designer does. A hand-written binding that does not match
		// the widget's FName is the classic "animation that silently never plays".
		UObject* Context = ResolvePlaybackContext(Blueprint);
		if (!Context)
		{
			OutWarnings.Add(TEXT("no playback context: the animation binding was written by hand"));
			FWidgetAnimationBinding Binding;
			Binding.AnimationGuid = Guid;
			Binding.WidgetName = Widget->GetFName();
			Animation->AnimationBindings.Add(Binding);
			return Guid;
		}
		Animation->BindPossessableObject(Guid, *Widget, Context);
		return Guid;
	}

	/** The track class a property needs, plus the section it creates. */
	UClass* TrackClassFor(const FUMGTrackSpec& Spec)
	{
		switch (Spec.Kind)
		{
		case EUMGTrackKind::Byte:       return UMovieSceneByteTrack::StaticClass();
		case EUMGTrackKind::Transform:  return UMovieScene2DTransformTrack::StaticClass();
		default:                        return UMovieSceneFloatTrack::StaticClass();
		}
	}

	/**
	 * Find a widget's track for one property. Looked up by (class, possessable) and then matched on
	 * the property name: the track's display name is not the property name unless we set it.
	 */
	UMovieSceneTrack* FindPropertyTrack(UMovieScene* Scene, UClass* TrackClass, const FGuid& Guid,
	                                    const FName PropertyName)
	{
		if (!Scene || !Guid.IsValid())
		{
			return nullptr;
		}
		for (UMovieSceneTrack* Candidate : Scene->FindTracks(TrackClass, Guid))
		{
			if (const UMovieScenePropertyTrack* PropertyCandidate = Cast<UMovieScenePropertyTrack>(Candidate))
			{
				if (PropertyCandidate->GetPropertyName() == PropertyName)
				{
					return Candidate;
				}
			}
		}
		return nullptr;
	}

	/** Seconds -> the movie scene's own frame numbering. */
	FFrameNumber TimeToFrame(const UMovieScene* Scene, double Time)
	{
		return Scene->GetTickResolution().AsFrameNumber(FMath::Max(0.0, Time));
	}

	double FrameToTime(const UMovieScene* Scene, FFrameNumber Frame)
	{
		return Scene->GetTickResolution().AsSeconds(FFrameTime(Frame));
	}

	/** Values of an ESlateVisibility by member name, "ESlateVisibility::X" or number. */
	bool ParseVisibility(const TSharedPtr<FJsonValue>& Value, UEnum* Enum, uint8& Out)
	{
		if (!Enum || !Value.IsValid())
		{
			return false;
		}
		if (Value->Type == EJson::Number)
		{
			Out = (uint8)FMath::Clamp((int32)Value->AsNumber(), 0, 255);
			return true;
		}
		if (Value->Type != EJson::String)
		{
			return false;
		}
		FString Text = Value->AsString().TrimStartAndEnd();
		int32 Separator = INDEX_NONE;
		if (Text.FindLastChar(TEXT(':'), Separator))
		{
			Text = Text.Mid(Separator + 1);
		}
		for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
		{
			const FString Entry = Enum->GetNameStringByIndex(Index);
			const FString Short = Entry.Contains(TEXT("::")) ? Entry.Mid(Entry.Find(TEXT("::")) + 2) : Entry;
			if (Short.Equals(Text, ESearchCase::IgnoreCase) || Entry.Equals(Text, ESearchCase::IgnoreCase))
			{
				Out = (uint8)Enum->GetValueByIndex(Index);
				return true;
			}
		}
		return false;
	}

	bool JsonNumber(const TSharedPtr<FJsonValue>& Value, double& Out)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		if (Value->Type == EJson::Number)
		{
			Out = Value->AsNumber();
			return true;
		}
		if (Value->Type == EJson::String && Value->AsString().TrimStartAndEnd().IsNumeric())
		{
			Out = FCString::Atod(*Value->AsString());
			return true;
		}
		return false;
	}

	/** One float channel of a transform section, for the read-back. */
	struct FChannelRead
	{
		FString Name;
		TArray<double> Times;
		TArray<double> Values;
	};

	void ReadChannel(const FString& Name, const FMovieSceneFloatChannel& Channel, const UMovieScene* Scene,
	                 TArray<FChannelRead>& Out)
	{
		FChannelRead Read;
		Read.Name = Name;
		const TArrayView<const FFrameNumber> Times = Channel.GetTimes();
		const TArrayView<const FMovieSceneFloatValue> Values = Channel.GetValues();
		const int32 Count = FMath::Min(Times.Num(), Values.Num());
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Read.Times.Add(FrameToTime(Scene, Times[Index]));
			Read.Values.Add(Values[Index].Value);
		}
		Out.Add(MoveTemp(Read));
	}

	TArray<TSharedPtr<FJsonValue>> FloatKeysToJson(const FMovieSceneFloatChannel& Channel, const UMovieScene* Scene)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		const TArrayView<const FFrameNumber> Times = Channel.GetTimes();
		const TArrayView<const FMovieSceneFloatValue> Values = Channel.GetValues();
		const int32 Count = FMath::Min(Times.Num(), Values.Num());
		for (int32 Index = 0; Index < Count; ++Index)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("time"), FrameToTime(Scene, Times[Index]));
			Item->SetNumberField(TEXT("value"), Values[Index].Value);
			Items.Add(MakeShared<FJsonValueObject>(Item));
		}
		return Items;
	}

	/** The animation's own copies (the runtime reads the generated class, never the asset object). */
	void AddGeneratedAnimationInfo(UWidgetBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Result)
	{
		UWidgetBlueprintGeneratedClass* Generated = Blueprint
			? Cast<UWidgetBlueprintGeneratedClass>(Blueprint->GeneratedClass) : nullptr;
		Result->SetNumberField(TEXT("generated_animation_count"), Generated ? Generated->Animations.Num() : 0);
	}

	/**
	 * Structural change -> compile -> prove the animation reached the generated class. Without the
	 * compile the runtime and the designer never see it (the same silent-failure shape as bindings).
	 */
	void CommitAnimationChange(UWidgetBlueprint* Blueprint, const FString& AnimationName,
	                           const TSharedPtr<FJsonObject>& Result)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		TArray<FUMGCompileMessage> Messages;
		int32 Errors = 0;
		int32 Warnings = 0;
		const bool bCompiled = CompileWidgetWithMessages(Blueprint, Messages, Errors, Warnings);

		TArray<TSharedPtr<FJsonValue>> ErrorItems;
		for (const FUMGCompileMessage& Message : Messages)
		{
			if (Message.Severity == TEXT("Error"))
			{
				ErrorItems.Add(MakeShared<FJsonValueString>(Message.Message));
			}
		}

		UWidgetBlueprintGeneratedClass* Generated = Cast<UWidgetBlueprintGeneratedClass>(Blueprint->GeneratedClass);
		bool bEffective = false;
		if (Generated)
		{
			for (UWidgetAnimation* Animation : Generated->Animations)
			{
				if (Animation && (Animation->GetName() == AnimationName || Animation->GetName().StartsWith(AnimationName + TEXT("_"))))
				{
					bEffective = true;
					break;
				}
			}
		}

		Result->SetBoolField(TEXT("compiled"), bCompiled);
		Result->SetNumberField(TEXT("num_errors"), Errors);
		Result->SetNumberField(TEXT("num_warnings"), Warnings);
		Result->SetArrayField(TEXT("compile_errors"), ErrorItems);
		Result->SetBoolField(TEXT("animation_effective"), bEffective);
		AddGeneratedAnimationInfo(Blueprint, Result);

		if (!bEffective)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error_code"), TEXT("animation_not_effective"));
			Result->SetStringField(TEXT("message"), FString::Printf(
				TEXT("'%s' is not in the generated class animations after compiling; the runtime reads that copy, so the animation would never play"),
				*AnimationName));
		}
	}
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleCreateWidgetAnimation(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'blueprint_name' parameter"));
	}
	FString AnimationName;
	if (!Params->TryGetStringField(TEXT("animation_name"), AnimationName)
		|| AnimationName.TrimStartAndEnd().IsEmpty())
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'animation_name' parameter"));
	}

	TSharedPtr<FJsonObject> Error;
	UWidgetBlueprint* Blueprint = LoadWidgetBlueprintByParam(BlueprintName, Error);
	if (!Blueprint)
	{
		return Error;
	}

	if (FindAnimation(Blueprint, AnimationName))
	{
		return MakeListError(TEXT("name_collision"), FString::Printf(
			TEXT("'%s' already has an animation named '%s'"), *Blueprint->GetName(), *AnimationName),
			TEXT("animations"), AnimationNames(Blueprint));
	}

	// Same order the designer's own "add animation" uses: create unnamed, name it, then attach a
	// movie scene and only then add it to the blueprint (an animation outside Blueprint->Animations
	// is invisible everywhere).
	UWidgetAnimation* Animation = NewObject<UWidgetAnimation>(Blueprint, NAME_None, RF_Transactional);
	Animation->SetDisplayLabel(AnimationName);
	Animation->Rename(*AnimationName);

	const FName AnimationFName(*AnimationName);
	UMovieScene* Scene = NewObject<UMovieScene>(Animation, AnimationFName, RF_Transactional);
	Animation->MovieScene = Scene;

	const FFrameRate DisplayRate(20, 1);
	Scene->SetDisplayRate(DisplayRate);
	const FFrameNumber DefaultLength = Scene->GetTickResolution().AsFrameNumber(2.0);
	Scene->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), DefaultLength + 1));

	Blueprint->Modify();
	Blueprint->Animations.Add(Animation);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
	Result->SetStringField(TEXT("animation_name"), Animation->GetName());
	Result->SetStringField(TEXT("display_label"), Animation->GetDisplayLabel());
	Result->SetArrayField(TEXT("animations"), [Blueprint]()
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FString& Name : AnimationNames(Blueprint))
		{
			Items.Add(MakeShared<FJsonValueString>(Name));
		}
		return Items;
	}());
	Result->SetNumberField(TEXT("animation_count"), Blueprint->Animations.Num());
	Result->SetNumberField(TEXT("display_rate"), DisplayRate.AsDecimal());
	Result->SetNumberField(TEXT("playback_length"), FrameToTime(Scene, DefaultLength));
	CommitAnimationChange(Blueprint, Animation->GetName(), Result);
	FinishWidgetWrite(Blueprint, Params, Result);
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleAddWidgetAnimationTrack(const TSharedPtr<FJsonObject>& Params)
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
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'widget_name' parameter"));
	}
	FString PropertyRaw;
	if (!Params->TryGetStringField(TEXT("property"), PropertyRaw) || PropertyRaw.TrimStartAndEnd().IsEmpty())
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'property' parameter"));
	}

	TSharedPtr<FJsonObject> Error;
	UWidgetBlueprint* Blueprint = LoadWidgetBlueprintByParam(BlueprintName, Error);
	if (!Blueprint)
	{
		return Error;
	}

	UWidgetAnimation* Animation = FindAnimation(Blueprint, AnimationName);
	if (!Animation)
	{
		return MakeListError(TEXT("animation_not_found"), FString::Printf(
			TEXT("'%s' has no animation named '%s'"), *Blueprint->GetName(), *AnimationName),
			TEXT("animations"), AnimationNames(Blueprint));
	}

	UWidget* Widget = Blueprint->WidgetTree ? Blueprint->WidgetTree->FindWidget(FName(*WidgetName)) : nullptr;
	if (!Widget)
	{
		TArray<FString> Names;
		GatherWidgetNames(Blueprint->WidgetTree, Names);
		return MakeListError(TEXT("widget_not_found"), FString::Printf(
			TEXT("Widget '%s' is not in '%s'"), *WidgetName, *Blueprint->GetName()), TEXT("widgets"), Names);
	}

	FUMGTrackSpec Spec;
	if (!ResolveTrackSpec(PropertyRaw, Spec))
	{
		TArray<FString> Supported;
		SupportedProperties(Supported);
		return MakeListError(TEXT("unsupported_property"), FString::Printf(
			TEXT("'%s' has no animation track mapping"), *PropertyRaw), TEXT("supported_properties"), Supported);
	}

	UMovieScene* Scene = Animation->GetMovieScene();
	if (!Scene)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("animation_has_no_scene"),
			FString::Printf(TEXT("'%s' has no movie scene"), *AnimationName));
	}

	TArray<FString> Warnings;
	const FGuid Guid = EnsureWidgetPossessable(Blueprint, Animation, Widget, Warnings);

	UClass* TrackClass = TrackClassFor(Spec);
	UMovieSceneTrack* Track = FindPropertyTrack(Scene, TrackClass, Guid, Spec.PropertyName);
	const bool bReused = Track != nullptr;
	if (!Track)
	{
		Track = Scene->AddTrack(TrackClass, Guid);
	}
	if (!Track)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("track_not_created"),
			FString::Printf(TEXT("Failed to add a %s track for '%s'"), *TrackClass->GetName(), *PropertyRaw));
	}

	if (UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track))
	{
		// The engine's own track editors set name and path from the property; the UMG component
		// registry matches on this pair, so a wrong path means a track that never binds.
		PropertyTrack->SetPropertyNameAndPath(Spec.PropertyName, Spec.PropertyName.ToString());
		UNREALMCP_SET_PROPERTY_TRACK_UNIQUE_NAME(PropertyTrack, Spec.PropertyName);
	}
	if (UMovieSceneByteTrack* ByteTrack = Cast<UMovieSceneByteTrack>(Track))
	{
		ByteTrack->SetEnum(StaticEnum<ESlateVisibility>());
	}

	UMovieSceneSection* Section = Track->GetAllSections().Num() > 0 ? Track->GetAllSections()[0] : nullptr;
	bool bSectionCreated = false;
	if (!Section)
	{
		Section = Track->CreateNewSection();
		if (Section)
		{
			Track->AddSection(*Section);
			bSectionCreated = true;
		}
	}
	if (!Section)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("section_not_created"),
			FString::Printf(TEXT("Failed to add a section to the %s track"), *TrackClass->GetName()));
	}
	if (UMovieScene2DTransformSection* TransformSection = Cast<UMovieScene2DTransformSection>(Section))
	{
		// Every channel is animatable; the mask decides which ones the track actually applies.
		TransformSection->SetMask(FMovieScene2DTransformMask(EMovieScene2DTransformChannel::AllTransform));
	}
	Section->SetRange(Scene->GetPlaybackRange());

	TSharedPtr<FJsonObject> TrackJson = MakeShared<FJsonObject>();
	TrackJson->SetStringField(TEXT("widget_name"), Widget->GetName());
	TrackJson->SetStringField(TEXT("property"), Spec.PropertyName.ToString());
	TrackJson->SetStringField(TEXT("track_class"), TrackClass->GetName());
	TrackJson->SetStringField(TEXT("section_class"), Section->GetClass()->GetName());
	TrackJson->SetStringField(TEXT("possessable_guid"), Guid.ToString());
	TrackJson->SetBoolField(TEXT("reused"), bReused && !bSectionCreated);

	// Scene->GetTracks() only counts the master tracks; a track that drives a possessed widget lives
	// inside its binding, so both have to be counted for the number to mean anything.
	int32 TrackCount = Scene->GetTracks().Num();
	for (const FMovieSceneBinding& Binding : UNREALMCP_SCENE_BINDINGS(Scene))
	{
		TrackCount += Binding.GetTracks().Num();
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
	Result->SetStringField(TEXT("animation_name"), Animation->GetName());
	Result->SetObjectField(TEXT("track"), TrackJson);
	Result->SetNumberField(TEXT("track_count"), TrackCount);
	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarningItems;
		for (const FString& Warning : Warnings)
		{
			WarningItems.Add(MakeShared<FJsonValueString>(Warning));
		}
		Result->SetArrayField(TEXT("warnings"), WarningItems);
	}
	CommitAnimationChange(Blueprint, Animation->GetName(), Result);
	FinishWidgetWrite(Blueprint, Params, Result);
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleSetWidgetAnimationKeyframes(const TSharedPtr<FJsonObject>& Params)
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
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'widget_name' parameter"));
	}
	FString PropertyRaw;
	if (!Params->TryGetStringField(TEXT("property"), PropertyRaw))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'property' parameter"));
	}
	const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
	if (!Params->TryGetArrayField(TEXT("keys"), Keys) || !Keys || Keys->Num() == 0)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"),
			TEXT("Missing 'keys' (a non-empty list of {time, value})"));
	}

	TSharedPtr<FJsonObject> Error;
	UWidgetBlueprint* Blueprint = LoadWidgetBlueprintByParam(BlueprintName, Error);
	if (!Blueprint)
	{
		return Error;
	}
	UWidgetAnimation* Animation = FindAnimation(Blueprint, AnimationName);
	if (!Animation)
	{
		return MakeListError(TEXT("animation_not_found"), FString::Printf(
			TEXT("'%s' has no animation named '%s'"), *Blueprint->GetName(), *AnimationName),
			TEXT("animations"), AnimationNames(Blueprint));
	}
	UWidget* Widget = Blueprint->WidgetTree ? Blueprint->WidgetTree->FindWidget(FName(*WidgetName)) : nullptr;
	if (!Widget)
	{
		TArray<FString> Names;
		GatherWidgetNames(Blueprint->WidgetTree, Names);
		return MakeListError(TEXT("widget_not_found"), FString::Printf(
			TEXT("Widget '%s' is not in '%s'"), *WidgetName, *Blueprint->GetName()), TEXT("widgets"), Names);
	}
	FUMGTrackSpec Spec;
	if (!ResolveTrackSpec(PropertyRaw, Spec))
	{
		TArray<FString> Supported;
		SupportedProperties(Supported);
		return MakeListError(TEXT("unsupported_property"), FString::Printf(
			TEXT("'%s' has no animation track mapping"), *PropertyRaw), TEXT("supported_properties"), Supported);
	}

	UMovieScene* Scene = Animation->GetMovieScene();
	const FGuid Guid = FindWidgetPossessable(Animation, Widget->GetFName());
	UMovieSceneTrack* Track = Scene ? FindPropertyTrack(Scene, TrackClassFor(Spec), Guid, Spec.PropertyName) : nullptr;
	if (!Track || Track->GetAllSections().Num() == 0)
	{
		return MakeListError(TEXT("track_not_found"), FString::Printf(
			TEXT("'%s' has no %s track for widget '%s'; call add_widget_animation_track first"),
			*AnimationName, *Spec.PropertyName.ToString(), *WidgetName),
			TEXT("property"), TArray<FString>{ Spec.PropertyName.ToString() });
	}
	UMovieSceneSection* Section = Track->GetAllSections()[0];

	UEnum* VisibilityEnum = StaticEnum<ESlateVisibility>();
	UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section);
	UMovieSceneByteSection* ByteSection = Cast<UMovieSceneByteSection>(Section);
	UMovieScene2DTransformSection* TransformSection = Cast<UMovieScene2DTransformSection>(Section);

	/** One key, already parsed: nothing is written until every key in the list validates. */
	struct FParsedKey
	{
		FFrameNumber Frame;
		double Time = 0.0;
		double Float = 0.0;
		uint8 Byte = 0;
		bool bTranslation = false;
		bool bScale = false;
		bool bShear = false;
		bool bRotation = false;
		FVector2D Translation = FVector2D::ZeroVector;
		FVector2D Scale = FVector2D::ZeroVector;
		FVector2D Shear = FVector2D::ZeroVector;
		double Rotation = 0.0;
	};

	TArray<FParsedKey> Parsed;
	Parsed.Reserve(Keys->Num());
	for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
	{
		if (!KeyValue.IsValid() || KeyValue->Type != EJson::Object)
		{
			return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_value"),
				TEXT("Every key must be an object with 'time' and 'value'"));
		}
		const TSharedPtr<FJsonObject> Key = KeyValue->AsObject();
		FParsedKey ParsedKey;
		if (!JsonNumber(Key->TryGetField(TEXT("time")), ParsedKey.Time))
		{
			return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_value"),
				TEXT("Every key needs a numeric 'time' (seconds)"));
		}
		ParsedKey.Frame = TimeToFrame(Scene, ParsedKey.Time);

		if (FloatSection)
		{
			if (!JsonNumber(Key->TryGetField(TEXT("value")), ParsedKey.Float))
			{
				return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_value"),
					FString::Printf(TEXT("'%s' keys need a numeric 'value'"), *Spec.PropertyName.ToString()));
			}
		}
		else if (ByteSection)
		{
			if (!ParseVisibility(Key->TryGetField(TEXT("value")), VisibilityEnum, ParsedKey.Byte))
			{
				TArray<FString> Candidates;
				if (VisibilityEnum)
				{
					for (int32 Index = 0; Index < VisibilityEnum->NumEnums(); ++Index)
					{
						const FString Entry = VisibilityEnum->GetNameStringByIndex(Index);
						Candidates.AddUnique(Entry.Contains(TEXT("::")) ? Entry.Mid(Entry.Find(TEXT("::")) + 2) : Entry);
					}
				}
				return MakeListError(TEXT("invalid_value"), FString::Printf(
					TEXT("'%s' keys need a visibility name or number"), *Spec.PropertyName.ToString()),
					TEXT("visibility_values"), Candidates);
			}
		}
		else if (TransformSection)
		{
			const TSharedPtr<FJsonValue> ValueField = Key->TryGetField(TEXT("value"));
			if (!ValueField.IsValid() || ValueField->Type != EJson::Object)
			{
				return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_value"),
					TEXT("RenderTransform keys need a 'value' object: translation / scale / shear / angle"));
			}
			const TSharedPtr<FJsonObject> ValueObject = ValueField->AsObject();
			auto ReadPair = [&ValueObject](const TCHAR* Field, FVector2D& Out) -> bool
			{
				const TArray<TSharedPtr<FJsonValue>>* Pair = nullptr;
				double X = 0.0;
				double Y = 0.0;
				if (!ValueObject->TryGetArrayField(Field, Pair) || !Pair || Pair->Num() < 2
					|| !JsonNumber((*Pair)[0], X) || !JsonNumber((*Pair)[1], Y))
				{
					return false;
				}
				Out = FVector2D(X, Y);
				return true;
			};
			ParsedKey.bTranslation = ReadPair(TEXT("translation"), ParsedKey.Translation);
			ParsedKey.bScale = ReadPair(TEXT("scale"), ParsedKey.Scale);
			ParsedKey.bShear = ReadPair(TEXT("shear"), ParsedKey.Shear);
			ParsedKey.bRotation = JsonNumber(ValueObject->TryGetField(TEXT("angle")), ParsedKey.Rotation);
			if (!ParsedKey.bTranslation && !ParsedKey.bScale && !ParsedKey.bShear && !ParsedKey.bRotation)
			{
				TArray<FString> Fields;
				Fields.Add(TEXT("translation"));
				Fields.Add(TEXT("scale"));
				Fields.Add(TEXT("shear"));
				Fields.Add(TEXT("angle"));
				return MakeListError(TEXT("invalid_value"),
					TEXT("A RenderTransform key needs at least one of these"), TEXT("value_fields"), Fields);
			}
		}
		Parsed.Add(MoveTemp(ParsedKey));
	}

	// Replace, not append: the caller hands over the whole key list (the same contract as the slot
	// writer - no hidden leftovers). Only now, after every key validated, is anything cleared.
	int32 ClearedKeys = 0;
	if (FloatSection)
	{
		ClearedKeys = FloatSection->GetChannel().GetNumKeys();
		FloatSection->GetChannel().Reset();
	}
	else if (ByteSection)
	{
		ClearedKeys = ByteSection->ByteCurve.GetNumKeys();
		ByteSection->ByteCurve.Reset();
	}
	else if (TransformSection)
	{
		for (int32 Index = 0; Index < 2; ++Index)
		{
			ClearedKeys += TransformSection->Translation[Index].GetNumKeys();
			ClearedKeys += TransformSection->Scale[Index].GetNumKeys();
			ClearedKeys += TransformSection->Shear[Index].GetNumKeys();
		}
		ClearedKeys += TransformSection->Rotation.GetNumKeys();
		TransformSection->Translation[0].Reset();
		TransformSection->Translation[1].Reset();
		TransformSection->Scale[0].Reset();
		TransformSection->Scale[1].Reset();
		TransformSection->Shear[0].Reset();
		TransformSection->Shear[1].Reset();
		TransformSection->Rotation.Reset();
	}
	else
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unsupported_section"),
			FString::Printf(TEXT("Section class %s is not supported"), *Section->GetClass()->GetName()));
	}

	const int32 Written = Parsed.Num();
	TArray<double> Times;
	Times.Reserve(Parsed.Num());
	for (const FParsedKey& Key : Parsed)
	{
		if (FloatSection)
		{
			FloatSection->GetChannel().AddCubicKey(Key.Frame, (float)Key.Float);
		}
		else if (ByteSection)
		{
			ByteSection->ByteCurve.GetData().AddKey(Key.Frame, Key.Byte);
		}
		else if (TransformSection)
		{
			if (Key.bTranslation)
			{
				TransformSection->Translation[0].AddCubicKey(Key.Frame, (float)Key.Translation.X);
				TransformSection->Translation[1].AddCubicKey(Key.Frame, (float)Key.Translation.Y);
			}
			if (Key.bScale)
			{
				TransformSection->Scale[0].AddCubicKey(Key.Frame, (float)Key.Scale.X);
				TransformSection->Scale[1].AddCubicKey(Key.Frame, (float)Key.Scale.Y);
			}
			if (Key.bShear)
			{
				TransformSection->Shear[0].AddCubicKey(Key.Frame, (float)Key.Shear.X);
				TransformSection->Shear[1].AddCubicKey(Key.Frame, (float)Key.Shear.Y);
			}
			if (Key.bRotation)
			{
				TransformSection->Rotation.AddCubicKey(Key.Frame, (float)Key.Rotation);
			}
		}
		Times.Add(Key.Time);
	}

	// A key beyond the playback range would never play, so the range follows the keys - and says so.
	bool bRangeExtended = false;
	if (Written > 0)
	{
		const double LastTime = FMath::Max<double>(Times);
		const TRange<FFrameNumber> Range = Scene->GetPlaybackRange();
		if (Range.GetUpperBound().IsClosed() && FrameToTime(Scene, Range.GetUpperBound().GetValue()) < LastTime)
		{
			Scene->SetPlaybackRange(TRange<FFrameNumber>(Range.GetLowerBound().GetValue(),
				TimeToFrame(Scene, LastTime) + 1));
			bRangeExtended = true;
		}
	}
	if (Section->GetRange().GetUpperBound().IsClosed())
	{
		Section->SetRange(TRange<FFrameNumber>::Hull(Section->GetRange(),
			TRange<FFrameNumber>::Inclusive(FFrameNumber(0), TimeToFrame(Scene, FMath::Max<double>(Times)) + 1)));
	}

	TArray<TSharedPtr<FJsonValue>> KeyItems;
	if (FloatSection)
	{
		KeyItems = FloatKeysToJson(FloatSection->GetChannel(), Scene);
	}
	else if (ByteSection)
	{
		const TArrayView<const FFrameNumber> KeyTimes = ByteSection->ByteCurve.GetTimes();
		const TArrayView<const uint8> KeyValues = ByteSection->ByteCurve.GetValues();
		const int32 Count = FMath::Min(KeyTimes.Num(), KeyValues.Num());
		for (int32 Index = 0; Index < Count; ++Index)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("time"), FrameToTime(Scene, KeyTimes[Index]));
			Item->SetNumberField(TEXT("value"), KeyValues[Index]);
			Item->SetStringField(TEXT("enum_label"), EnumLabel(VisibilityEnum, KeyValues[Index]));
			KeyItems.Add(MakeShared<FJsonValueObject>(Item));
		}
	}
	else if (TransformSection)
	{
		TArray<FChannelRead> Channels;
		ReadChannel(TEXT("translation_x"), TransformSection->Translation[0], Scene, Channels);
		ReadChannel(TEXT("translation_y"), TransformSection->Translation[1], Scene, Channels);
		ReadChannel(TEXT("scale_x"), TransformSection->Scale[0], Scene, Channels);
		ReadChannel(TEXT("scale_y"), TransformSection->Scale[1], Scene, Channels);
		ReadChannel(TEXT("shear_x"), TransformSection->Shear[0], Scene, Channels);
		ReadChannel(TEXT("shear_y"), TransformSection->Shear[1], Scene, Channels);
		ReadChannel(TEXT("rotation"), TransformSection->Rotation, Scene, Channels);

		int32 MaxKeys = 0;
		for (const FChannelRead& Read : Channels)
		{
			MaxKeys = FMath::Max(MaxKeys, Read.Times.Num());
		}
		for (int32 Index = 0; Index < MaxKeys; ++Index)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			double Time = 0.0;
			bool bHasTime = false;
			TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
			for (const FChannelRead& Read : Channels)
			{
				if (Read.Times.Num() > Index)
				{
					Time = Read.Times[Index];
					bHasTime = true;
					Value->SetNumberField(Read.Name, Read.Values[Index]);
				}
			}
			if (!bHasTime)
			{
				continue;
			}
			Item->SetNumberField(TEXT("time"), Time);
			Item->SetObjectField(TEXT("value"), Value);
			KeyItems.Add(MakeShared<FJsonValueObject>(Item));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
	Result->SetStringField(TEXT("animation_name"), Animation->GetName());
	Result->SetStringField(TEXT("widget_name"), Widget->GetName());
	Result->SetStringField(TEXT("property"), Spec.PropertyName.ToString());
	Result->SetNumberField(TEXT("keys_written"), Written);
	Result->SetNumberField(TEXT("keys_replaced"), ClearedKeys);
	Result->SetArrayField(TEXT("keys"), KeyItems);
	Result->SetNumberField(TEXT("key_count"), KeyItems.Num());
	Result->SetBoolField(TEXT("playback_range_extended"), bRangeExtended);
	Result->SetNumberField(TEXT("playback_length"), FrameToTime(Scene, Scene->GetPlaybackRange().GetUpperBound().GetValue()));
	CommitAnimationChange(Blueprint, Animation->GetName(), Result);
	FinishWidgetWrite(Blueprint, Params, Result);
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleListWidgetAnimations(const TSharedPtr<FJsonObject>& Params)
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

	TArray<TSharedPtr<FJsonValue>> AnimationItems;
	int32 OrphanTrackCount = 0;
	for (UWidgetAnimation* Animation : Blueprint->Animations)
	{
		if (!Animation)
		{
			continue;
		}
		UMovieScene* Scene = Animation->GetMovieScene();
		TSharedPtr<FJsonObject> AnimationJson = MakeShared<FJsonObject>();
		AnimationJson->SetStringField(TEXT("animation_name"), Animation->GetName());
		AnimationJson->SetStringField(TEXT("display_label"), Animation->GetDisplayLabel());
		AnimationJson->SetNumberField(TEXT("start_time"), Animation->GetStartTime());
		AnimationJson->SetNumberField(TEXT("end_time"), Animation->GetEndTime());
		AnimationJson->SetNumberField(TEXT("display_rate"), Scene ? Scene->GetDisplayRate().AsDecimal() : 0.0);

		TArray<TSharedPtr<FJsonValue>> TrackItems;
		if (Scene)
		{
			for (const FMovieSceneBinding& Binding : UNREALMCP_SCENE_BINDINGS(Scene))
			{
				FName WidgetName;
				FName SlotWidgetName;
				for (const FWidgetAnimationBinding& AnimationBinding : Animation->AnimationBindings)
				{
					if (AnimationBinding.AnimationGuid == Binding.GetObjectGuid())
					{
						WidgetName = AnimationBinding.WidgetName;
						SlotWidgetName = AnimationBinding.SlotWidgetName;
						break;
					}
				}
				for (UMovieSceneTrack* Track : Binding.GetTracks())
				{
					UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track);
					TSharedPtr<FJsonObject> TrackJson = MakeShared<FJsonObject>();
					TrackJson->SetStringField(TEXT("widget_name"), WidgetName.ToString());
					TrackJson->SetStringField(TEXT("slot_widget_name"), SlotWidgetName.ToString());
					TrackJson->SetStringField(TEXT("property"), PropertyTrack ? PropertyTrack->GetPropertyName().ToString() : FString());
					TrackJson->SetStringField(TEXT("track_class"), Track ? Track->GetClass()->GetName() : FString());
					TrackJson->SetStringField(TEXT("possessable_guid"), Binding.GetObjectGuid().ToString());
					// A track whose widget is no longer in the tree is a leftover - it says nothing at
					// playback time, so it has to be visible here (that is the whole point of a report).
					const bool bWidgetExists = WidgetName.IsNone()
						? false : Blueprint->WidgetTree && Blueprint->WidgetTree->FindWidget(WidgetName) != nullptr;
					TrackJson->SetBoolField(TEXT("widget_exists"), bWidgetExists);
					if (!bWidgetExists)
					{
						++OrphanTrackCount;
					}

					TArray<TSharedPtr<FJsonValue>> TrackKeys;
					if (Track && Track->GetAllSections().Num() > 0)
					{
						UMovieSceneSection* Section = Track->GetAllSections()[0];
						TrackJson->SetStringField(TEXT("section_class"), Section->GetClass()->GetName());
						TrackJson->SetNumberField(TEXT("section_start"), FrameToTime(Scene, Section->GetRange().GetLowerBound().GetValue()));
						TrackJson->SetNumberField(TEXT("section_end"), FrameToTime(Scene, Section->GetRange().GetUpperBound().GetValue()));
						if (UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section))
						{
							TrackKeys = FloatKeysToJson(FloatSection->GetChannel(), Scene);
						}
						else if (UMovieSceneByteSection* ByteSection = Cast<UMovieSceneByteSection>(Section))
						{
							UEnum* VisibilityEnum = StaticEnum<ESlateVisibility>();
							const TArrayView<const FFrameNumber> KeyTimes = ByteSection->ByteCurve.GetTimes();
							const TArrayView<const uint8> KeyValues = ByteSection->ByteCurve.GetValues();
							const int32 Count = FMath::Min(KeyTimes.Num(), KeyValues.Num());
							for (int32 Index = 0; Index < Count; ++Index)
							{
								TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
								Item->SetNumberField(TEXT("time"), FrameToTime(Scene, KeyTimes[Index]));
								Item->SetNumberField(TEXT("value"), KeyValues[Index]);
								Item->SetStringField(TEXT("enum_label"), EnumLabel(VisibilityEnum, KeyValues[Index]));
								TrackKeys.Add(MakeShared<FJsonValueObject>(Item));
							}
						}
						else if (UMovieScene2DTransformSection* TransformSection = Cast<UMovieScene2DTransformSection>(Section))
						{
							TArray<FChannelRead> Channels;
							ReadChannel(TEXT("translation_x"), TransformSection->Translation[0], Scene, Channels);
							ReadChannel(TEXT("translation_y"), TransformSection->Translation[1], Scene, Channels);
							ReadChannel(TEXT("scale_x"), TransformSection->Scale[0], Scene, Channels);
							ReadChannel(TEXT("scale_y"), TransformSection->Scale[1], Scene, Channels);
							ReadChannel(TEXT("shear_x"), TransformSection->Shear[0], Scene, Channels);
							ReadChannel(TEXT("shear_y"), TransformSection->Shear[1], Scene, Channels);
							ReadChannel(TEXT("rotation"), TransformSection->Rotation, Scene, Channels);
							TSharedPtr<FJsonObject> ChannelJson = MakeShared<FJsonObject>();
							for (const FChannelRead& Read : Channels)
							{
								TArray<TSharedPtr<FJsonValue>> ChannelKeys;
								for (int32 Index = 0; Index < Read.Times.Num() && Index < Read.Values.Num(); ++Index)
								{
									TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
									Item->SetNumberField(TEXT("time"), Read.Times[Index]);
									Item->SetNumberField(TEXT("value"), Read.Values[Index]);
									ChannelKeys.Add(MakeShared<FJsonValueObject>(Item));
								}
								ChannelJson->SetArrayField(Read.Name, ChannelKeys);
							}
							TrackJson->SetObjectField(TEXT("channel_keys"), ChannelJson);
							TrackJson->SetNumberField(TEXT("key_count"), TrackKeys.Num());
							TrackItems.Add(MakeShared<FJsonValueObject>(TrackJson));
							continue;
						}
					}
					TrackJson->SetArrayField(TEXT("keys"), TrackKeys);
					TrackJson->SetNumberField(TEXT("key_count"), TrackKeys.Num());
					TrackItems.Add(MakeShared<FJsonValueObject>(TrackJson));
				}
			}
		}
		AnimationJson->SetArrayField(TEXT("tracks"), TrackItems);
		AnimationJson->SetNumberField(TEXT("track_count"), TrackItems.Num());

		bool bInGenerated = false;
		if (UWidgetBlueprintGeneratedClass* Generated = Cast<UWidgetBlueprintGeneratedClass>(Blueprint->GeneratedClass))
		{
			for (UWidgetAnimation* GeneratedAnimation : Generated->Animations)
			{
				if (GeneratedAnimation
					&& (GeneratedAnimation->GetName() == Animation->GetName()
						|| GeneratedAnimation->GetName().StartsWith(Animation->GetName() + TEXT("_"))))
				{
					bInGenerated = true;
					break;
				}
			}
		}
		AnimationJson->SetBoolField(TEXT("in_generated_class"), bInGenerated);
		AnimationItems.Add(MakeShared<FJsonValueObject>(AnimationJson));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
	Result->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Result->SetArrayField(TEXT("animations"), AnimationItems);
	Result->SetNumberField(TEXT("animation_count"), Blueprint->Animations.Num());
	Result->SetNumberField(TEXT("orphan_track_count"), OrphanTrackCount);
	AddGeneratedAnimationInfo(Blueprint, Result);
	Result->SetArrayField(TEXT("supported_properties"), [](TArray<FString> Supported)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FString& Name : Supported)
		{
			Items.Add(MakeShared<FJsonValueString>(Name));
		}
		return Items;
	}([]()
	{
		TArray<FString> Supported;
		SupportedProperties(Supported);
		return Supported;
	}()));
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleRemoveWidgetAnimation(const TSharedPtr<FJsonObject>& Params)
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
	UWidgetAnimation* Animation = FindAnimation(Blueprint, AnimationName);
	if (!Animation)
	{
		return MakeListError(TEXT("animation_not_found"), FString::Printf(
			TEXT("'%s' has no animation named '%s'"), *Blueprint->GetName(), *AnimationName),
			TEXT("animations"), AnimationNames(Blueprint));
	}

	// Graph nodes hold the animation as a direct object reference: deleting it under them would leave
	// nodes pointing at nothing. Refuse and name the nodes instead (the same contract as
	// unbind_widget_property's blocked_by_references).
	TArray<TSharedPtr<FJsonValue>> Blockers;
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph)
		{
			continue;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->DefaultObject == Animation)
				{
					TSharedPtr<FJsonObject> Blocker = MakeShared<FJsonObject>();
					Blocker->SetStringField(TEXT("graph"), Graph->GetName());
					Blocker->SetStringField(TEXT("node"), Node->GetName());
					Blocker->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
					Blocker->SetStringField(TEXT("pin"), Pin->PinName.ToString());
					Blockers.Add(MakeShared<FJsonValueObject>(Blocker));
					break;
				}
			}
		}
	}
	if (Blockers.Num() > 0)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error_code"), TEXT("blocked_by_references"));
		Result->SetStringField(TEXT("message"), FString::Printf(
			TEXT("'%s' is referenced by %d graph node pin(s); delete or rewire them first"),
			*AnimationName, Blockers.Num()));
		Result->SetArrayField(TEXT("blockers"), Blockers);
		return Result;
	}

	Blueprint->Modify();
	// Out of Blueprint->Animations is what "deleted" means: the object (and its movie scene) then has no
	// owner left and is collected, exactly like the designer's own delete.
	Blueprint->Animations.Remove(Animation);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	TArray<FUMGCompileMessage> Messages;
	int32 Errors = 0;
	int32 Warnings = 0;
	const bool bCompiled = CompileWidgetWithMessages(Blueprint, Messages, Errors, Warnings);
	TArray<TSharedPtr<FJsonValue>> ErrorItems;
	for (const FUMGCompileMessage& Message : Messages)
	{
		if (Message.Severity == TEXT("Error"))
		{
			ErrorItems.Add(MakeShared<FJsonValueString>(Message.Message));
		}
	}

	// It has to be gone from the generated class too - that copy is what plays at runtime.
	bool bStillGenerated = false;
	if (UWidgetBlueprintGeneratedClass* Generated = Cast<UWidgetBlueprintGeneratedClass>(Blueprint->GeneratedClass))
	{
		for (UWidgetAnimation* GeneratedAnimation : Generated->Animations)
		{
			if (GeneratedAnimation
				&& (GeneratedAnimation->GetName() == AnimationName
					|| GeneratedAnimation->GetName().StartsWith(AnimationName + TEXT("_"))))
			{
				bStillGenerated = true;
				break;
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), !bStillGenerated);
	Result->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
	Result->SetStringField(TEXT("animation_name"), AnimationName);
	Result->SetBoolField(TEXT("removed"), true);
	Result->SetBoolField(TEXT("compiled"), bCompiled);
	Result->SetNumberField(TEXT("num_errors"), Errors);
	Result->SetNumberField(TEXT("num_warnings"), Warnings);
	Result->SetArrayField(TEXT("compile_errors"), ErrorItems);
	Result->SetArrayField(TEXT("animations"), [Blueprint]()
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FString& Name : AnimationNames(Blueprint))
		{
			Items.Add(MakeShared<FJsonValueString>(Name));
		}
		return Items;
	}());
	Result->SetNumberField(TEXT("animation_count"), Blueprint->Animations.Num());
	AddGeneratedAnimationInfo(Blueprint, Result);
	if (bStillGenerated)
	{
		Result->SetStringField(TEXT("error_code"), TEXT("animation_still_present"));
		Result->SetStringField(TEXT("message"), FString::Printf(
			TEXT("'%s' is still in the generated class animations after compiling"), *AnimationName));
	}

	FinishWidgetWrite(Blueprint, Params, Result);
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleRenameWidgetAnimation(const TSharedPtr<FJsonObject>& Params)
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
	FString NewName;
	if (!Params->TryGetStringField(TEXT("new_name"), NewName) || NewName.TrimStartAndEnd().IsEmpty())
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'new_name' parameter"));
	}

	TSharedPtr<FJsonObject> Error;
	UWidgetBlueprint* Blueprint = LoadWidgetBlueprintByParam(BlueprintName, Error);
	if (!Blueprint)
	{
		return Error;
	}
	UWidgetAnimation* Animation = FindAnimation(Blueprint, AnimationName);
	if (!Animation)
	{
		return MakeListError(TEXT("animation_not_found"), FString::Printf(
			TEXT("'%s' has no animation named '%s'"), *Blueprint->GetName(), *AnimationName),
			TEXT("animations"), AnimationNames(Blueprint));
	}
	if (NewName != AnimationName && FindAnimation(Blueprint, NewName))
	{
		return MakeListError(TEXT("name_collision"), FString::Printf(
			TEXT("'%s' already has an animation named '%s'"), *Blueprint->GetName(), *NewName),
			TEXT("animations"), AnimationNames(Blueprint));
	}

	Blueprint->Modify();
	Animation->Modify();
	Animation->SetDisplayLabel(NewName);
	Animation->Rename(*NewName);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TArray<FUMGCompileMessage> Messages;
	int32 Errors = 0;
	int32 Warnings = 0;
	const bool bCompiled = CompileWidgetWithMessages(Blueprint, Messages, Errors, Warnings);
	TArray<TSharedPtr<FJsonValue>> ErrorItems;
	for (const FUMGCompileMessage& Message : Messages)
	{
		if (Message.Severity == TEXT("Error"))
		{
			ErrorItems.Add(MakeShared<FJsonValueString>(Message.Message));
		}
	}

	bool bGenerated = false;
	if (UWidgetBlueprintGeneratedClass* Generated = Cast<UWidgetBlueprintGeneratedClass>(Blueprint->GeneratedClass))
	{
		for (UWidgetAnimation* GeneratedAnimation : Generated->Animations)
		{
			if (GeneratedAnimation
				&& (GeneratedAnimation->GetName() == NewName
					|| GeneratedAnimation->GetName().StartsWith(NewName + TEXT("_"))))
			{
				bGenerated = true;
				break;
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), bGenerated);
	Result->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
	Result->SetStringField(TEXT("previous_name"), AnimationName);
	Result->SetStringField(TEXT("animation_name"), Animation->GetName());
	Result->SetStringField(TEXT("display_label"), Animation->GetDisplayLabel());
	Result->SetBoolField(TEXT("renamed"), true);
	Result->SetBoolField(TEXT("animation_effective"), bGenerated);
	Result->SetBoolField(TEXT("compiled"), bCompiled);
	Result->SetNumberField(TEXT("num_errors"), Errors);
	Result->SetNumberField(TEXT("num_warnings"), Warnings);
	Result->SetArrayField(TEXT("compile_errors"), ErrorItems);
	Result->SetArrayField(TEXT("animations"), [Blueprint]()
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FString& Name : AnimationNames(Blueprint))
		{
			Items.Add(MakeShared<FJsonValueString>(Name));
		}
		return Items;
	}());
	Result->SetNumberField(TEXT("animation_count"), Blueprint->Animations.Num());
	AddGeneratedAnimationInfo(Blueprint, Result);
	if (!bGenerated)
	{
		Result->SetStringField(TEXT("error_code"), TEXT("animation_not_effective"));
		Result->SetStringField(TEXT("message"), FString::Printf(
			TEXT("'%s' is not in the generated class animations after compiling"), *NewName));
	}

	FinishWidgetWrite(Blueprint, Params, Result);
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleSetWidgetAnimationPlaybackRange(const TSharedPtr<FJsonObject>& Params)
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

	double Length = 0.0;
	double StartTime = 0.0;
	double EndTime = 0.0;
	const bool bHasLength = Params->TryGetNumberField(TEXT("length"), Length);
	const bool bHasEnd = Params->TryGetNumberField(TEXT("end_time"), EndTime);
	const bool bHasStart = Params->TryGetNumberField(TEXT("start_time"), StartTime);
	if (!bHasLength && !bHasEnd)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"),
			TEXT("Pass 'length' (seconds from the start) or 'end_time' (absolute seconds)"));
	}

	TSharedPtr<FJsonObject> Error;
	UWidgetBlueprint* Blueprint = LoadWidgetBlueprintByParam(BlueprintName, Error);
	if (!Blueprint)
	{
		return Error;
	}
	UWidgetAnimation* Animation = FindAnimation(Blueprint, AnimationName);
	if (!Animation)
	{
		return MakeListError(TEXT("animation_not_found"), FString::Printf(
			TEXT("'%s' has no animation named '%s'"), *Blueprint->GetName(), *AnimationName),
			TEXT("animations"), AnimationNames(Blueprint));
	}
	UMovieScene* Scene = Animation->GetMovieScene();
	if (!Scene)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("animation_has_no_scene"),
			FString::Printf(TEXT("'%s' has no movie scene"), *AnimationName));
	}

	const TRange<FFrameNumber> RangeBefore = Scene->GetPlaybackRange();
	const double StartBefore = FrameToTime(Scene, RangeBefore.GetLowerBound().GetValue());
	const double EndBefore = FrameToTime(Scene, RangeBefore.GetUpperBound().GetValue());

	const double UsedStart = bHasStart ? StartTime : StartBefore;
	const double UsedEnd = bHasEnd ? EndTime : (UsedStart + Length);
	if (UsedEnd <= UsedStart)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_value"),
			FString::Printf(TEXT("The end (%f) must be after the start (%f)"), UsedEnd, UsedStart));
	}

	Animation->Modify();
	Scene->Modify();
	// The upper bound is exclusive, so it is set to the requested end exactly: reading the range back
	// then returns what the caller asked for (unlike the +1 the designer applies to its frame range).
	Scene->SetPlaybackRange(TRange<FFrameNumber>(TimeToFrame(Scene, UsedStart), TimeToFrame(Scene, UsedEnd)));
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TArray<FUMGCompileMessage> Messages;
	int32 Errors = 0;
	int32 Warnings = 0;
	const bool bCompiled = CompileWidgetWithMessages(Blueprint, Messages, Errors, Warnings);
	TArray<TSharedPtr<FJsonValue>> ErrorItems;
	for (const FUMGCompileMessage& Message : Messages)
	{
		if (Message.Severity == TEXT("Error"))
		{
			ErrorItems.Add(MakeShared<FJsonValueString>(Message.Message));
		}
	}

	// Keys outside the range still exist, they simply never play: say how many rather than trimming
	// them behind the caller's back.
	int32 KeysBeyondRange = 0;
	TArray<double> KeyTimes;
	for (const FMovieSceneBinding& Binding : UNREALMCP_SCENE_BINDINGS(Scene))
	{
		for (UMovieSceneTrack* Track : Binding.GetTracks())
		{
			if (!Track || Track->GetAllSections().Num() == 0)
			{
				continue;
			}
			UMovieSceneSection* Section = Track->GetAllSections()[0];
			if (UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section))
			{
				for (const FFrameNumber& Key : FloatSection->GetChannel().GetTimes())
				{
					KeyTimes.Add(FrameToTime(Scene, Key));
				}
			}
			else if (UMovieSceneByteSection* ByteSection = Cast<UMovieSceneByteSection>(Section))
			{
				for (const FFrameNumber& Key : ByteSection->ByteCurve.GetTimes())
				{
					KeyTimes.Add(FrameToTime(Scene, Key));
				}
			}
			else if (UMovieScene2DTransformSection* TransformSection = Cast<UMovieScene2DTransformSection>(Section))
			{
				for (int32 Index = 0; Index < 2; ++Index)
				{
					for (const FFrameNumber& Key : TransformSection->Translation[Index].GetTimes())
					{
						KeyTimes.Add(FrameToTime(Scene, Key));
					}
				}
			}
		}
	}
	for (const double KeyTime : KeyTimes)
	{
		if (KeyTime > UsedEnd)
		{
			++KeysBeyondRange;
		}
	}

	const TRange<FFrameNumber> RangeAfter = Scene->GetPlaybackRange();
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
	Result->SetStringField(TEXT("animation_name"), Animation->GetName());
	Result->SetNumberField(TEXT("playback_start_before"), StartBefore);
	Result->SetNumberField(TEXT("playback_end_before"), EndBefore);
	Result->SetNumberField(TEXT("playback_start"), FrameToTime(Scene, RangeAfter.GetLowerBound().GetValue()));
	Result->SetNumberField(TEXT("playback_end"), FrameToTime(Scene, RangeAfter.GetUpperBound().GetValue()));
	Result->SetNumberField(TEXT("playback_length"), FrameToTime(Scene, RangeAfter.GetUpperBound().GetValue())
		- FrameToTime(Scene, RangeAfter.GetLowerBound().GetValue()));
	Result->SetNumberField(TEXT("key_count"), KeyTimes.Num());
	Result->SetNumberField(TEXT("keys_beyond_range"), KeysBeyondRange);
	Result->SetBoolField(TEXT("compiled"), bCompiled);
	Result->SetNumberField(TEXT("num_errors"), Errors);
	Result->SetNumberField(TEXT("num_warnings"), Warnings);
	Result->SetArrayField(TEXT("compile_errors"), ErrorItems);
	Result->SetStringField(TEXT("hint"),
		TEXT("the playback range is what 'the animation's length' means; keys past its end stay in the asset but never play (see keys_beyond_range)"));

	FinishWidgetWrite(Blueprint, Params, Result);
	return Result;
}
