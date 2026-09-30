// UMG commands: widget construction, property and slot writing, and reading the widget tree back.
#include "Commands/UnrealMCPUMGCommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Blueprint/UserWidget.h"
#include "Components/TextBlock.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Image.h"
#include "Components/ProgressBar.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/OverlaySlot.h"
#include "Components/ScaleBoxSlot.h"
#include "Components/SizeBoxSlot.h"
#include "Components/ScrollBoxSlot.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "UObject/TextProperty.h"
#include "Components/Button.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Components/GridSlot.h"
#include "Components/GridPanel.h"
#include "Components/Slider.h"
#include "Components/CheckBox.h"
#include "Components/EditableText.h"
#include "Components/Widget.h"
#include "Components/SlateWrapperTypes.h"
#include "Blueprint/WidgetNavigation.h"
#include "EdGraph/EdGraph.h"
#include "Styling/SlateBrush.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#include "Commands/UMG/UnrealMCPUMGCommandHelpers.h"
#include "Kismet2/BlueprintEditorUtils.h"

namespace
{
	/** Result of writing one widget property, mirroring the applied[]/failed[] contract. */
	struct FPropWriteResult
	{
		bool bApplied = false;
		FString Code;
		FString Message;
		FString Property;
		FString ValueBefore;
		FString ValueAfter;
		TArray<FString> Candidates;
	};

	/** Declared here: the property writers below call it before its definition at the end of the group. */
	void GatherWidgetPropertyCandidates(UWidget* Widget, TArray<FString>& OutCandidates);

	bool JsonNumber(const TSharedPtr<FJsonValue>& Value, double& Out)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		switch (Value->Type)
		{
		case EJson::Number:  Out = Value->AsNumber(); return true;
		case EJson::Boolean: Out = Value->AsBool() ? 1.0 : 0.0; return true;
		case EJson::String:
		{
			const FString Text = Value->AsString().TrimStartAndEnd();
			if (Text.IsNumeric())
			{
				Out = FCString::Atod(*Text);
				return true;
			}
			return false;
		}
		default: return false;
		}
	}

	bool JsonBool(const TSharedPtr<FJsonValue>& Value, bool& Out)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		double Number = 0.0;
		if (Value->Type == EJson::Boolean)
		{
			Out = Value->AsBool();
			return true;
		}
		if (JsonNumber(Value, Number))
		{
			Out = !FMath::IsNearlyZero(Number);
			return true;
		}
		return false;
	}

	bool JsonText(const TSharedPtr<FJsonValue>& Value, FString& Out)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		switch (Value->Type)
		{
		case EJson::String:  Out = Value->AsString(); return true;
		case EJson::Number:  Out = FString::SanitizeFloat(Value->AsNumber()); return true;
		case EJson::Boolean: Out = Value->AsBool() ? TEXT("true") : TEXT("false"); return true;
		default: return false;
		}
	}

	/** [x, y] or {"x": .., "y": ..}. */
	bool JsonFloat2(const TSharedPtr<FJsonValue>& Value, FVector2D& Out)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		if (Value->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
			double X = 0.0;
			double Y = 0.0;
			if (Items.Num() < 2 || !JsonNumber(Items[0], X) || !JsonNumber(Items[1], Y))
			{
				return false;
			}
			Out = FVector2D(X, Y);
			return true;
		}
		if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject>& Obj = Value->AsObject();
			double X = 0.0;
			double Y = 0.0;
			const bool bHasX = JsonNumber(Obj->TryGetField(TEXT("x")), X) || JsonNumber(Obj->TryGetField(TEXT("X")), X);
			const bool bHasY = JsonNumber(Obj->TryGetField(TEXT("y")), Y) || JsonNumber(Obj->TryGetField(TEXT("Y")), Y);
			if (bHasX && bHasY)
			{
				Out = FVector2D(X, Y);
				return true;
			}
		}
		return false;
	}

	/** [r, g, b(, a)] or {"r": .., ...}. Values above 1 are read as 0..255 and normalised. */
	bool JsonColor(const TSharedPtr<FJsonValue>& Value, FLinearColor& Out)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		double Numbers[4] = { 0.0, 0.0, 0.0, 1.0 };
		bool bAny = false;

		if (Value->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
			const int32 Count = FMath::Min(Items.Num(), 4);
			if (Count < 3)
			{
				return false;
			}
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (!JsonNumber(Items[Index], Numbers[Index]))
				{
					return false;
				}
			}
			bAny = true;
		}
		else if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject>& Obj = Value->AsObject();
			const TCHAR* Keys[4] = { TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a") };
			for (int32 Index = 0; Index < 4; ++Index)
			{
				FString Upper = FString(Keys[Index]).ToUpper();
				bAny |= JsonNumber(Obj->TryGetField(Keys[Index]), Numbers[Index]);
				bAny |= JsonNumber(Obj->TryGetField(Upper), Numbers[Index]);
			}
		}
		if (!bAny)
		{
			return false;
		}

		if (Numbers[0] > 1.0 || Numbers[1] > 1.0 || Numbers[2] > 1.0)
		{
			Numbers[0] /= 255.0;
			Numbers[1] /= 255.0;
			Numbers[2] /= 255.0;
		}
		if (Numbers[3] > 1.0)
		{
			Numbers[3] /= 255.0;
		}
		Out = FLinearColor(Numbers[0], Numbers[1], Numbers[2], Numbers[3]);
		return true;
	}

	/** A single number (uniform), [left, top, right, bottom] (or 1/2/3 values), or an object. */
	bool JsonMargin(const TSharedPtr<FJsonValue>& Value, FMargin& Out)
	{
		double Single = 0.0;
		if (JsonNumber(Value, Single))
		{
			Out = FMargin(Single);
			return true;
		}
		if (!Value.IsValid())
		{
			return false;
		}
		if (Value->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
			double Numbers[4] = { 0.0, 0.0, 0.0, 0.0 };
			const int32 Count = FMath::Min(Items.Num(), 4);
			if (Count == 0)
			{
				return false;
			}
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (!JsonNumber(Items[Index], Numbers[Index]))
				{
					return false;
				}
			}
			if (Count == 1)
			{
				Out = FMargin(Numbers[0]);
			}
			else if (Count == 2)
			{
				Out = FMargin(Numbers[0], Numbers[1]);
			}
			else
			{
				Out = FMargin(Numbers[0], Numbers[1], Numbers[2], Numbers[3]);
			}
			return true;
		}
		if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject>& Obj = Value->AsObject();
			double Left = 0.0;
			double Top = 0.0;
			double Right = 0.0;
			double Bottom = 0.0;
			bool bAny = false;
			bAny |= JsonNumber(Obj->TryGetField(TEXT("left")), Left);
			bAny |= JsonNumber(Obj->TryGetField(TEXT("top")), Top);
			bAny |= JsonNumber(Obj->TryGetField(TEXT("right")), Right);
			bAny |= JsonNumber(Obj->TryGetField(TEXT("bottom")), Bottom);
			if (!bAny)
			{
				return false;
			}
			Out = FMargin(Left, Top, Right, Bottom);
			return true;
		}
		return false;
	}

	/** {"translation": [x, y], "scale": [x, y], "shear": [x, y], "angle": degrees} - any subset. */
	bool JsonWidgetTransform(const TSharedPtr<FJsonValue>& Value, FWidgetTransform& Out)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			return false;
		}
		const TSharedPtr<FJsonObject>& Obj = Value->AsObject();
		FVector2D Vector = FVector2D::ZeroVector;
		double Angle = 0.0;
		bool bAny = false;
		if (JsonFloat2(Obj->TryGetField(TEXT("translation")), Vector))
		{
			Out.Translation = Vector;
			bAny = true;
		}
		if (JsonFloat2(Obj->TryGetField(TEXT("scale")), Vector))
		{
			Out.Scale = Vector;
			bAny = true;
		}
		if (JsonFloat2(Obj->TryGetField(TEXT("shear")), Vector))
		{
			Out.Shear = Vector;
			bAny = true;
		}
		if (JsonNumber(Obj->TryGetField(TEXT("angle")), Angle))
		{
			Out.Angle = Angle;
			bAny = true;
		}
		return bAny;
	}

	/** A number (Fill value), "Fill" / "Automatic", or {"rule": .., "value": ..}. */
	bool JsonSizeRule(const TSharedPtr<FJsonValue>& Value, FSlateChildSize& Out)
	{
		double Number = 0.0;
		if (JsonNumber(Value, Number))
		{
			Out.SizeRule = ESlateSizeRule::Fill;
			Out.Value = Number;
			return true;
		}
		if (!Value.IsValid())
		{
			return false;
		}
		if (Value->Type == EJson::String)
		{
			const FString Rule = Value->AsString().TrimStartAndEnd();
			if (Rule.Equals(TEXT("Fill"), ESearchCase::IgnoreCase))
			{
				Out.SizeRule = ESlateSizeRule::Fill;
				return true;
			}
			if (Rule.Equals(TEXT("Automatic"), ESearchCase::IgnoreCase) || Rule.Equals(TEXT("Auto"), ESearchCase::IgnoreCase))
			{
				Out.SizeRule = ESlateSizeRule::Automatic;
				return true;
			}
			return false;
		}
		if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject>& Obj = Value->AsObject();
			FString Rule;
			bool bAny = false;
			if (JsonText(Obj->TryGetField(TEXT("rule")), Rule))
			{
				if (Rule.Equals(TEXT("Fill"), ESearchCase::IgnoreCase))
				{
					Out.SizeRule = ESlateSizeRule::Fill;
					bAny = true;
				}
				else if (Rule.Equals(TEXT("Automatic"), ESearchCase::IgnoreCase) || Rule.Equals(TEXT("Auto"), ESearchCase::IgnoreCase))
				{
					Out.SizeRule = ESlateSizeRule::Automatic;
					bAny = true;
				}
			}
			if (JsonNumber(Obj->TryGetField(TEXT("value")), Number))
			{
				Out.Value = Number;
				bAny = true;
			}
			return bAny;
		}
		return false;
	}

	/** Compare tokens that ignore case, underscores, scope prefixes and the "_MAX" sentinel. */
	void EnumTokens(const FString& In, TArray<FString>& Out)
	{
		Out.Reset();
		FString Full = In.ToLower();
		Full.ReplaceInline(TEXT("_"), TEXT(""));
		Full.ReplaceInline(TEXT(":"), TEXT(""));
		Out.AddUnique(Full);

		int32 Separator = INDEX_NONE;
		if (In.FindLastChar(TEXT('_'), Separator) && Separator + 1 < In.Len())
		{
			Out.AddUnique(In.Mid(Separator + 1).ToLower());
		}
		if (In.FindLastChar(TEXT(':'), Separator) && Separator + 1 < In.Len())
		{
			Out.AddUnique(In.Mid(Separator + 1).ToLower());
		}
	}

	bool JsonEnum(const TSharedPtr<FJsonValue>& Value, UEnum* Enum, int64& Out)
	{
		if (!Enum || !Value.IsValid())
		{
			return false;
		}
		if (Value->Type == EJson::Number)
		{
			Out = (int64)Value->AsNumber();
			return Enum->IsValidEnumValue(Out);
		}

		FString Requested;
		if (!JsonText(Value, Requested))
		{
			return false;
		}

		TArray<FString> Want;
		EnumTokens(Requested, Want);
		const int32 Count = Enum->NumEnums();

		// Pass 0 matches the whole name so that "VAlign_Center" cannot be read as "HAlign_Center";
		// pass 1 also accepts the trailing part ("Center") when the scope prefix is left off.
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			if (!Want.IsValidIndex(Pass))
			{
				break;
			}
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const FString EntryName = Enum->GetNameStringByIndex(Index);
				if (EntryName.EndsWith(TEXT("_MAX")))
				{
					continue;
				}
				TArray<FString> Have;
				EnumTokens(EntryName, Have);
				TArray<FString> DisplayTokens;
				EnumTokens(Enum->GetDisplayNameTextByIndex(Index).ToString(), DisplayTokens);
				Have.Append(DisplayTokens);
				if (Have.Contains(Want[Pass]))
				{
					Out = Enum->GetValueByIndex(Index);
					return true;
				}
			}
		}
		return false;
	}

	FString ColorToString(const FLinearColor& Color)
	{
		return FString::Printf(TEXT("%.3f,%.3f,%.3f,%.3f"), Color.R, Color.G, Color.B, Color.A);
	}

	FPropWriteResult PropFail(const FString& Code, const FString& Message)
	{
		FPropWriteResult Result;
		Result.Code = Code;
		Result.Message = Message;
		return Result;
	}

	FPropWriteResult PropOk(const FString& Property, const FString& Before, const FString& After)
	{
		FPropWriteResult Result;
		Result.bApplied = true;
		Result.Property = Property;
		Result.ValueBefore = Before;
		Result.ValueAfter = After;
		return Result;
	}

	UEnum* EnumOfProperty(FProperty* Property)
	{
		if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			return ByteProperty->Enum;
		}
		if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			return EnumProperty->GetEnum();
		}
		return nullptr;
	}

	FNumericProperty* EnumUnderlyingProperty(FProperty* Property)
	{
		if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			return EnumProperty->GetUnderlyingProperty();
		}
		return CastField<FNumericProperty>(Property);
	}

	/** Read any property as text: this is the read-back behind applied[].value_before / value_after. */
	FString DescribePropertyValue(FProperty* Property, const void* Address)
	{
		if (!Property || !Address)
		{
			return FString();
		}
		if (UEnum* Enum = EnumOfProperty(Property))
		{
			FNumericProperty* Numeric = EnumUnderlyingProperty(Property);
			return Numeric ? EnumLabel(Enum, (int64)Numeric->GetUnsignedIntPropertyValue(Address)) : FString();
		}
		if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			return BoolProperty->GetPropertyValue(Address) ? TEXT("true") : TEXT("false");
		}
		if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			return NumericProperty->IsFloatingPoint()
				? FString::SanitizeFloat(NumericProperty->GetFloatingPointPropertyValue(Address))
				: FString::Printf(TEXT("%lld"), NumericProperty->GetSignedIntPropertyValue(Address));
		}
		if (FStrProperty* StrProperty = CastField<FStrProperty>(Property))
		{
			return StrProperty->GetPropertyValue(Address);
		}
		if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			return NameProperty->GetPropertyValue(Address).ToString();
		}
		if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			return TextProperty->GetPropertyValue(Address).ToString();
		}
		if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			UObject* Object = ObjectProperty->GetObjectPropertyValue(Address);
			return Object ? Object->GetPathName() : TEXT("None");
		}
		return FString();
	}

	/** Several UMG properties have a getter or not depending on their release, so read them by reflection. */
	FString EnumPropLabel(UObject* Object, const TCHAR* PropertyName)
	{
		if (!Object)
		{
			return FString();
		}
		FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), PropertyName);
		return Property ? DescribePropertyValue(Property, Property->ContainerPtrToValuePtr<void>(Object)) : FString();
	}

	/** "Rule:WidgetName" for one navigation direction, read off the widget's navigation object. */
	FString NavigationLabel(UWidget* Widget, EUINavigation Direction)
	{
		if (!Widget || !Widget->Navigation)
		{
			return TEXT("<none>");
		}
		const FWidgetNavigationData& Data = Widget->Navigation->GetNavigationData(Direction);
		UEnum* RuleEnum = StaticEnum<EUINavigationRule>();
		return FString::Printf(TEXT("%s:%s"), RuleEnum ? *EnumLabel(RuleEnum, (int64)Data.Rule) : TEXT("?"),
			*Data.WidgetToFocus.ToString());
	}

	EUINavigation NavigationDirectionFromKey(const FString& Key, bool& bOutFound)
	{
		const FString Lower = Key.ToLower();
		bOutFound = true;
		if (Lower == TEXT("up")) { return EUINavigation::Up; }
		if (Lower == TEXT("down")) { return EUINavigation::Down; }
		if (Lower == TEXT("left")) { return EUINavigation::Left; }
		if (Lower == TEXT("right")) { return EUINavigation::Right; }
		if (Lower == TEXT("next")) { return EUINavigation::Next; }
		if (Lower == TEXT("previous")) { return EUINavigation::Previous; }
		bOutFound = false;
		return EUINavigation::Invalid;
	}

	/** Write one reflected property. OutError gets a short reason the caller turns into an error code. */
	bool WriteReflectedValue(FProperty* Property, const TSharedPtr<FJsonValue>& Value, void* Address, FString& OutError)
	{
		if (UEnum* Enum = EnumOfProperty(Property))
		{
			int64 EnumValue = 0;
			FNumericProperty* Numeric = EnumUnderlyingProperty(Property);
			if (!Numeric || !JsonEnum(Value, Enum, EnumValue))
			{
				OutError = FString::Printf(TEXT("takes a %s name or number"), *Enum->GetName());
				return false;
			}
			Numeric->SetIntPropertyValue(Address, EnumValue);
			return true;
		}
		if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			bool bBool = false;
			if (!JsonBool(Value, bBool))
			{
				OutError = TEXT("takes a bool");
				return false;
			}
			BoolProperty->SetPropertyValue(Address, bBool);
			return true;
		}
		if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			double Number = 0.0;
			if (!JsonNumber(Value, Number))
			{
				OutError = TEXT("takes a number");
				return false;
			}
			if (NumericProperty->IsFloatingPoint())
			{
				NumericProperty->SetFloatingPointPropertyValue(Address, Number);
			}
			else
			{
				NumericProperty->SetIntPropertyValue(Address, (int64)Number);
			}
			return true;
		}
		if (FStrProperty* StrProperty = CastField<FStrProperty>(Property))
		{
			FString Text;
			if (!JsonText(Value, Text)) { OutError = TEXT("takes a string"); return false; }
			StrProperty->SetPropertyValue(Address, Text);
			return true;
		}
		if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			FString Text;
			if (!JsonText(Value, Text)) { OutError = TEXT("takes a name"); return false; }
			NameProperty->SetPropertyValue(Address, FName(*Text));
			return true;
		}
		if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			FString Text;
			if (!JsonText(Value, Text)) { OutError = TEXT("takes a string"); return false; }
			TextProperty->SetPropertyValue(Address, FText::FromString(Text));
			return true;
		}
		if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			FString Path;
			if (!JsonText(Value, Path)) { OutError = TEXT("takes an asset path"); return false; }
			UObject* Object = (Path.IsEmpty() || Path == TEXT("None")) ? nullptr : FUnrealMCPCommonUtils::FindAsset(Path);
			if (!Object && Path != TEXT("None") && !Path.IsEmpty())
			{
				OutError = FString::Printf(TEXT("asset '%s' not found"), *Path);
				return false;
			}
			if (Object && ObjectProperty->PropertyClass && !Object->IsA(ObjectProperty->PropertyClass))
			{
				OutError = FString::Printf(TEXT("'%s' is a %s, expected a %s"), *Path,
					*Object->GetClass()->GetName(), *ObjectProperty->PropertyClass->GetName());
				return false;
			}
			ObjectProperty->SetObjectPropertyValue(Address, Object);
			return true;
		}
		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct == TBaseStructure<FLinearColor>::Get())
			{
				FLinearColor Color;
				if (!JsonColor(Value, Color)) { OutError = TEXT("takes [r, g, b, a]"); return false; }
				*static_cast<FLinearColor*>(Address) = Color;
				return true;
			}
			if (StructProperty->Struct == TBaseStructure<FVector2D>::Get())
			{
				FVector2D Vector = FVector2D::ZeroVector;
				if (!JsonFloat2(Value, Vector)) { OutError = TEXT("takes [x, y]"); return false; }
				*static_cast<FVector2D*>(Address) = Vector;
				return true;
			}
			if (StructProperty->Struct == TBaseStructure<FMargin>::Get())
			{
				FMargin Margin;
				if (!JsonMargin(Value, Margin)) { OutError = TEXT("takes {left, top, right, bottom}"); return false; }
				*static_cast<FMargin*>(Address) = Margin;
				return true;
			}
			if (StructProperty->Struct == FWidgetTransform::StaticStruct())
			{
				FWidgetTransform Transform;
				if (!JsonWidgetTransform(Value, Transform)) { OutError = TEXT("takes {translation, scale, shear, angle}"); return false; }
				*static_cast<FWidgetTransform*>(Address) = Transform;
				return true;
			}
			OutError = FString::Printf(TEXT("struct %s cannot be written from JSON"), *StructProperty->Struct->GetName());
			return false;
		}
		OutError = TEXT("property type is not writable from JSON");
		return false;
	}

	/** Lowercase, underscores dropped: "brush_color" and "BrushColor" meet at "brushcolor". */
	FString NormalizePropertyKey(const FString& Key)
	{
		FString Out = Key.ToLower();
		Out.ReplaceInline(TEXT("_"), TEXT(""));
		return Out;
	}

	/**
	 * Exact reflection name first; otherwise a case/underscore-insensitive match, where bool
	 * properties also answer without their engine 'b' prefix ("override_width_override" ->
	 * bOverride_WidthOverride). More than one normalized hit is reported, never guessed.
	 */
	FProperty* ResolveWidgetPropertyByKey(UClass* Class, const FString& Key, TArray<FString>& OutAmbiguous)
	{
		OutAmbiguous.Reset();
		if (FProperty* Exact = FindFProperty<FProperty>(Class, FName(*Key)))
		{
			return Exact;
		}
		const FString Wanted = NormalizePropertyKey(Key);
		TArray<FProperty*> Hits;
		for (TFieldIterator<FProperty> It(Class); It; ++It)
		{
			FProperty* Candidate = *It;
			const FString Name = NormalizePropertyKey(Candidate->GetName());
			const bool bBoolAlias = CastField<FBoolProperty>(Candidate) && Name.StartsWith(TEXT("b")) && Name.Mid(1) == Wanted;
			if (Name == Wanted || bBoolAlias)
			{
				Hits.AddUnique(Candidate);
			}
		}
		if (Hits.Num() == 1)
		{
			return Hits[0];
		}
		for (const FProperty* Hit : Hits)
		{
			OutAmbiguous.Add(Hit->GetName());
		}
		return nullptr;
	}

	/** Anything the fixed table does not cover goes through reflection on the widget class. */
	FPropWriteResult ApplyReflectedWidgetProperty(UWidget* Widget, const FString& Key, const TSharedPtr<FJsonValue>& Value)
	{
		TArray<FString> Ambiguous;
		FProperty* Property = ResolveWidgetPropertyByKey(Widget->GetClass(), Key, Ambiguous);
		if (!Property && Ambiguous.Num() > 1)
		{
			FPropWriteResult Result = PropFail(TEXT("ambiguous_property"), FString::Printf(
				TEXT("Key '%s' matches several properties of %s: %s. Use the exact name"),
				*Key, *Widget->GetClass()->GetName(), *FString::Join(Ambiguous, TEXT(", "))));
			Result.Candidates = Ambiguous;
			return Result;
		}
		if (!Property)
		{
			FPropWriteResult Result = PropFail(TEXT("unknown_property"), FString::Printf(
				TEXT("Widget '%s' (%s) has no property '%s'"), *Widget->GetName(), *Widget->GetClass()->GetName(), *Key));
			GatherWidgetPropertyCandidates(Widget, Result.Candidates);
			return Result;
		}
		if (Property->HasAnyPropertyFlags(CPF_EditConst | CPF_BlueprintReadOnly))
		{
			FPropWriteResult Result = PropFail(TEXT("property_not_writable"), FString::Printf(
				TEXT("Property '%s' is read-only on %s"), *Key, *Widget->GetClass()->GetName()));
			Result.Property = Property->GetName();
			return Result;
		}

		void* Address = Property->ContainerPtrToValuePtr<void>(Widget);
		const FString Before = DescribePropertyValue(Property, Address);
		FString Reason;
		if (!WriteReflectedValue(Property, Value, Address, Reason))
		{
			FPropWriteResult Result = PropFail(TEXT("unsupported_property_type"), FString::Printf(
				TEXT("Property '%s' (%s) %s"), *Key, *Property->GetCPPType(), *Reason));
			Result.Property = Property->GetName();
			return Result;
		}

		// A property write can change the widget's own defaults, but the visible effect lands on the
		// next SynchronizeProperties: ask the widget to re-read itself so the read-back is not a lie.
		Widget->SynchronizeProperties();
		return PropOk(Property->GetName(), Before, DescribePropertyValue(Property, Address));
	}

	FPropWriteResult ApplyWidgetProperty(UWidget* Widget, UWidgetTree* Tree, const FString& Key,
	                                     const TSharedPtr<FJsonValue>& Value)
	{
		if (!Widget)
		{
			return PropFail(TEXT("widget_not_found"), TEXT("No widget to write"));
		}

		// ---- common widget properties -------------------------------------------------
		if (Key == TEXT("visibility"))
		{
			int64 EnumValue = 0;
			if (!JsonEnum(Value, StaticEnum<ESlateVisibility>(), EnumValue))
			{
				return PropFail(TEXT("unsupported_property_type"), TEXT("'visibility' takes an ESlateVisibility name (Visible / Collapsed / Hidden / HitTestInvisible / SelfHitTestInvisible)"));
			}
			const FString Before = EnumLabel(StaticEnum<ESlateVisibility>(), (int64)Widget->GetVisibility());
			Widget->SetVisibility(static_cast<ESlateVisibility>(EnumValue));
			return PropOk(TEXT("Visibility"), Before, EnumLabel(StaticEnum<ESlateVisibility>(), (int64)Widget->GetVisibility()));
		}
		if (Key == TEXT("is_enabled"))
		{
			bool bEnabled = false;
			if (!JsonBool(Value, bEnabled))
			{
				return PropFail(TEXT("unsupported_property_type"), TEXT("'is_enabled' takes a bool"));
			}
			const FString Before = Widget->GetIsEnabled() ? TEXT("true") : TEXT("false");
			Widget->SetIsEnabled(bEnabled);
			return PropOk(TEXT("IsEnabled"), Before, Widget->GetIsEnabled() ? TEXT("true") : TEXT("false"));
		}
		if (Key == TEXT("tooltip"))
		{
			FString Text;
			if (!JsonText(Value, Text))
			{
				return PropFail(TEXT("unsupported_property_type"), TEXT("'tooltip' takes a string"));
			}
			const FString Before = Widget->GetToolTipText().ToString();
			Widget->SetToolTipText(FText::FromString(Text));
			return PropOk(TEXT("ToolTipText"), Before, Widget->GetToolTipText().ToString());
		}
		if (Key == TEXT("render_opacity"))
		{
			double Opacity = 0.0;
			if (!JsonNumber(Value, Opacity))
			{
				return PropFail(TEXT("unsupported_property_type"), TEXT("'render_opacity' takes a number"));
			}
			const FString Before = FString::SanitizeFloat(Widget->GetRenderOpacity());
			Widget->SetRenderOpacity((float)Opacity);
			return PropOk(TEXT("RenderOpacity"), Before, FString::SanitizeFloat(Widget->GetRenderOpacity()));
		}
		if (Key == TEXT("render_transform"))
		{
			FWidgetTransform Transform = Widget->GetRenderTransform();
			if (!JsonWidgetTransform(Value, Transform))
			{
				return PropFail(TEXT("unsupported_property_type"), TEXT("'render_transform' takes {\"translation\": [x, y], \"scale\": [x, y], \"shear\": [x, y], \"angle\": degrees}"));
			}
			const FWidgetTransform Before = Widget->GetRenderTransform();
			Widget->SetRenderTransform(Transform);
			const FWidgetTransform After = Widget->GetRenderTransform();
			return PropOk(TEXT("RenderTransform"),
				FString::Printf(TEXT("t(%.2f,%.2f) s(%.2f,%.2f) a%.2f"),
					Before.Translation.X, Before.Translation.Y, Before.Scale.X, Before.Scale.Y, Before.Angle),
				FString::Printf(TEXT("t(%.2f,%.2f) s(%.2f,%.2f) a%.2f"),
					After.Translation.X, After.Translation.Y, After.Scale.X, After.Scale.Y, After.Angle));
		}
		if (Key == TEXT("render_transform_pivot"))
		{
			FVector2D Pivot = FVector2D::ZeroVector;
			if (!JsonFloat2(Value, Pivot))
			{
				return PropFail(TEXT("unsupported_property_type"), TEXT("'render_transform_pivot' takes [x, y]"));
			}
			const FVector2D Before = Widget->GetRenderTransformPivot();
			Widget->SetRenderTransformPivot(Pivot);
			const FVector2D After = Widget->GetRenderTransformPivot();
			return PropOk(TEXT("RenderTransformPivot"),
				FString::Printf(TEXT("%.3f,%.3f"), Before.X, Before.Y),
				FString::Printf(TEXT("%.3f,%.3f"), After.X, After.Y));
		}
		if (Key == TEXT("clipping"))
		{
			int64 EnumValue = 0;
			if (!JsonEnum(Value, StaticEnum<EWidgetClipping>(), EnumValue))
			{
				return PropFail(TEXT("unsupported_property_type"), TEXT("'clipping' takes an EWidgetClipping name (Inherit / ClipToBounds / ClipToBoundsWithoutIntersecting / ClipToBoundsAlways / OnDemand)"));
			}
			const FString Before = EnumLabel(StaticEnum<EWidgetClipping>(), (int64)Widget->GetClipping());
			Widget->SetClipping(static_cast<EWidgetClipping>(EnumValue));
			return PropOk(TEXT("Clipping"), Before, EnumLabel(StaticEnum<EWidgetClipping>(), (int64)Widget->GetClipping()));
		}
		if (Key == TEXT("navigation_all") || Key == TEXT("navigation"))
		{
			TArray<TPair<EUINavigation, FString>> Wanted;
			if (Key == TEXT("navigation_all"))
			{
				FString Target;
				if (!JsonText(Value, Target))
				{
					return PropFail(TEXT("unsupported_property_type"), TEXT("'navigation_all' takes a widget name, \"stop\", \"wrap\", or \"\""));
				}
				for (EUINavigation Direction : { EUINavigation::Up, EUINavigation::Down, EUINavigation::Left,
					EUINavigation::Right, EUINavigation::Next, EUINavigation::Previous })
				{
					Wanted.Emplace(Direction, Target);
				}
			}
			else
			{
				if (!Value.IsValid() || Value->Type != EJson::Object)
				{
					return PropFail(TEXT("unsupported_property_type"), TEXT("'navigation' takes {\"up\": \"WidgetName\", \"left\": \"\", ...}"));
				}
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Value->AsObject()->Values)
				{
					bool bFound = false;
					const EUINavigation Direction = NavigationDirectionFromKey(Pair.Key, bFound);
					if (!bFound)
					{
						return PropFail(TEXT("invalid_value"), FString::Printf(
							TEXT("'%s' is not a navigation direction (up / down / left / right / next / previous)"), *Pair.Key));
					}
					FString Target;
					if (!JsonText(Pair.Value, Target))
					{
						return PropFail(TEXT("unsupported_property_type"), FString::Printf(TEXT("'navigation.%s' takes a widget name, \"stop\" or \"wrap\""), *Pair.Key));
					}
					Wanted.Emplace(Direction, Target);
				}
			}

			FString Before;
			FString After;
			for (const TPair<EUINavigation, FString>& Item : Wanted)
			{
				const FString CurrentLabel = NavigationLabel(Widget, Item.Key);
				if (!Before.IsEmpty())
				{
					Before += TEXT(" ");
					After += TEXT(" ");
				}
				Before += CurrentLabel;
				if (Item.Value.IsEmpty() || Item.Value.Equals(TEXT("stop"), ESearchCase::IgnoreCase)
					|| Item.Value.Equals(TEXT("escape"), ESearchCase::IgnoreCase))
				{
					Widget->SetNavigationRuleBase(Item.Key, EUINavigationRule::Escape);
				}
				else if (Item.Value.Equals(TEXT("wrap"), ESearchCase::IgnoreCase))
				{
					Widget->SetNavigationRuleBase(Item.Key, EUINavigationRule::Wrap);
				}
				else
				{
					UWidget* Target = Tree ? Tree->FindWidget(FName(*Item.Value)) : nullptr;
					if (!Target)
					{
						return PropFail(TEXT("widget_not_found"), FString::Printf(
							TEXT("Navigation target '%s' is not in the widget tree"), *Item.Value));
					}
					Widget->SetNavigationRuleExplicit(Item.Key, Target);
				}
				After += NavigationLabel(Widget, Item.Key);
			}
			return PropOk(TEXT("Navigation"), Before, After);
		}

		// ---- per type properties ------------------------------------------------------
		if (UTextBlock* TextBlock = Cast<UTextBlock>(Widget))
		{
			if (Key == TEXT("text"))
			{
				FString Text;
				if (!JsonText(Value, Text))
				{
					return PropFail(TEXT("unsupported_property_type"), TEXT("'text' takes a string"));
				}
				const FString Before = TextBlock->GetText().ToString();
				TextBlock->SetText(FText::FromString(Text));
				return PropOk(TEXT("Text"), Before, TextBlock->GetText().ToString());
			}
			if (Key == TEXT("auto_wrap"))
			{
				bool bWrap = false;
				if (!JsonBool(Value, bWrap))
				{
					return PropFail(TEXT("unsupported_property_type"), TEXT("'auto_wrap' takes a bool"));
				}
				const FString Before = TextBlock->GetAutoWrapText() ? TEXT("true") : TEXT("false");
				TextBlock->SetAutoWrapText(bWrap);
				return PropOk(TEXT("AutoWrapText"), Before, TextBlock->GetAutoWrapText() ? TEXT("true") : TEXT("false"));
			}
			if (Key == TEXT("wrap_text_at"))
			{
				double WrapAt = 0.0;
				if (!JsonNumber(Value, WrapAt))
				{
					return PropFail(TEXT("unsupported_property_type"), TEXT("'wrap_text_at' takes a number"));
				}
				const FString Before = FString::SanitizeFloat(TextBlock->GetWrapTextAt());
				TextBlock->SetWrapTextAt((float)WrapAt);
				return PropOk(TEXT("WrapTextAt"), Before, FString::SanitizeFloat(TextBlock->GetWrapTextAt()));
			}
			if (Key == TEXT("font_size"))
			{
				double Size = 0.0;
				if (!JsonNumber(Value, Size))
				{
					return PropFail(TEXT("unsupported_property_type"), TEXT("'font_size' takes a number"));
				}
				const FString Before = FString::SanitizeFloat(TextBlock->GetFont().Size);
				FSlateFontInfo Font = TextBlock->GetFont();
				Font.Size = (int32)Size;
				TextBlock->SetFont(Font);
				return PropOk(TEXT("Font.Size"), Before, FString::SanitizeFloat(TextBlock->GetFont().Size));
			}
			if (Key == TEXT("color") || Key == TEXT("color_and_opacity"))
			{
				FLinearColor Color;
				if (!JsonColor(Value, Color))
				{
					return PropFail(TEXT("unsupported_property_type"), TEXT("'color' takes [r, g, b, a] (0..1 or 0..255)"));
				}
				const FString Before = ColorToString(TextBlock->GetColorAndOpacity().GetSpecifiedColor());
				TextBlock->SetColorAndOpacity(FSlateColor(Color));
				return PropOk(TEXT("ColorAndOpacity"), Before, ColorToString(TextBlock->GetColorAndOpacity().GetSpecifiedColor()));
			}
			if (Key == TEXT("justification"))
			{
				int64 EnumValue = 0;
				if (!JsonEnum(Value, StaticEnum<ETextJustify::Type>(), EnumValue))
				{
					return PropFail(TEXT("unsupported_property_type"), TEXT("'justification' takes left / center / right"));
				}
				const FString Before = EnumPropLabel(TextBlock, TEXT("Justification"));
				TextBlock->SetJustification((ETextJustify::Type)EnumValue);
				return PropOk(TEXT("Justification"), Before, EnumPropLabel(TextBlock, TEXT("Justification")));
			}
		}

		if (UButton* Button = Cast<UButton>(Widget))
		{
			if (Key == TEXT("background_color"))
			{
				FLinearColor Color;
				if (!JsonColor(Value, Color))
				{
					return PropFail(TEXT("unsupported_property_type"), TEXT("'background_color' takes [r, g, b, a]"));
				}
				const FString Before = ColorToString(Button->GetBackgroundColor());
				Button->SetBackgroundColor(Color);
				return PropOk(TEXT("BackgroundColor"), Before, ColorToString(Button->GetBackgroundColor()));
			}
		}

		if (UImage* Image = Cast<UImage>(Widget))
		{
			if (Key == TEXT("tint") || Key == TEXT("color_and_opacity"))
			{
				FLinearColor Color;
				if (!JsonColor(Value, Color))
				{
					return PropFail(TEXT("unsupported_property_type"), TEXT("'tint' takes [r, g, b, a]"));
				}
				const FString Before = ColorToString(Image->GetColorAndOpacity());
				Image->SetColorAndOpacity(Color);
				return PropOk(TEXT("ColorAndOpacity"), Before, ColorToString(Image->GetColorAndOpacity()));
			}
			if (Key == TEXT("brush") || Key == TEXT("brush_resource"))
			{
				FString ResourcePath;
				FString DrawAs;
				FMargin BrushMargin;
				bool bHasMargin = false;
				if (!JsonText(Value, ResourcePath))
				{
					if (!Value.IsValid() || Value->Type != EJson::Object)
					{
						return PropFail(TEXT("unsupported_property_type"), TEXT("'brush' takes an asset path, or {\"resource\": path, \"margin\": {...}, \"draw_as\": \"Box\"}"));
					}
					const TSharedPtr<FJsonObject>& Obj = Value->AsObject();
					FString Field;
					if (JsonText(Obj->TryGetField(TEXT("resource")), Field)) { ResourcePath = Field; }
					else if (JsonText(Obj->TryGetField(TEXT("texture")), Field)) { ResourcePath = Field; }
					else if (JsonText(Obj->TryGetField(TEXT("material")), Field)) { ResourcePath = Field; }
					bHasMargin = JsonMargin(Obj->TryGetField(TEXT("margin")), BrushMargin);
					JsonText(Obj->TryGetField(TEXT("draw_as")), DrawAs);
				}

				UObject* Resource = nullptr;
				if (!ResourcePath.IsEmpty())
				{
					Resource = FUnrealMCPCommonUtils::FindAsset(ResourcePath);
					if (!Resource)
					{
						return PropFail(TEXT("load_failed"), FString::Printf(TEXT("Asset '%s' not found"), *ResourcePath));
					}
					if (!Resource->IsA<UTexture2D>() && !Resource->IsA<UMaterialInterface>())
					{
						return PropFail(TEXT("unsupported_property_type"), FString::Printf(
							TEXT("'%s' is a %s; a brush takes a texture or a material"), *ResourcePath, *Resource->GetClass()->GetName()));
					}
				}

				const FSlateBrush& Current = Image->GetBrush();
				const FString Before = Current.GetResourceObject() ? Current.GetResourceObject()->GetPathName() : TEXT("None");
				FSlateBrush Brush = Current;
				if (Resource)
				{
					Brush.SetResourceObject(Resource);
				}
				if (bHasMargin)
				{
					Brush.Margin = BrushMargin;
				}
				if (!DrawAs.IsEmpty())
				{
					if (DrawAs.Equals(TEXT("Box"), ESearchCase::IgnoreCase)) { Brush.DrawAs = ESlateBrushDrawType::Box; }
					else if (DrawAs.Equals(TEXT("Border"), ESearchCase::IgnoreCase)) { Brush.DrawAs = ESlateBrushDrawType::Border; }
					else if (DrawAs.Equals(TEXT("Image"), ESearchCase::IgnoreCase)) { Brush.DrawAs = ESlateBrushDrawType::Image; }
					else if (DrawAs.Equals(TEXT("RoundedBox"), ESearchCase::IgnoreCase)) { Brush.DrawAs = ESlateBrushDrawType::RoundedBox; }
					else if (DrawAs.Equals(TEXT("NoDrawType"), ESearchCase::IgnoreCase)) { Brush.DrawAs = ESlateBrushDrawType::NoDrawType; }
					else
					{
						return PropFail(TEXT("invalid_value"), FString::Printf(TEXT("'%s' is not a brush draw type (Box / Border / Image / RoundedBox / NoDrawType)"), *DrawAs));
					}
				}
				Image->SetBrush(Brush);
				const FSlateBrush& After = Image->GetBrush();
				return PropOk(TEXT("Brush"), Before, After.GetResourceObject() ? After.GetResourceObject()->GetPathName() : TEXT("None"));
			}
		}

		if (UProgressBar* Bar = Cast<UProgressBar>(Widget))
		{
			if (Key == TEXT("percent"))
			{
				double Percent = 0.0;
				if (!JsonNumber(Value, Percent))
				{
					return PropFail(TEXT("unsupported_property_type"), TEXT("'percent' takes a number between 0 and 1"));
				}
				const FString Before = FString::SanitizeFloat(Bar->GetPercent());
				Bar->SetPercent((float)Percent);
				return PropOk(TEXT("Percent"), Before, FString::SanitizeFloat(Bar->GetPercent()));
			}
			if (Key == TEXT("fill_color"))
			{
				FLinearColor Color;
				if (!JsonColor(Value, Color))
				{
					return PropFail(TEXT("unsupported_property_type"), TEXT("'fill_color' takes [r, g, b, a]"));
				}
				const FString Before = ColorToString(Bar->GetFillColorAndOpacity());
				Bar->SetFillColorAndOpacity(Color);
				return PropOk(TEXT("FillColorAndOpacity"), Before, ColorToString(Bar->GetFillColorAndOpacity()));
			}
		}

		if (USlider* Slider = Cast<USlider>(Widget))
		{
			if (Key == TEXT("value"))
			{
				double SliderValue = 0.0;
				if (!JsonNumber(Value, SliderValue))
				{
					return PropFail(TEXT("unsupported_property_type"), TEXT("'value' takes a number"));
				}
				const FString Before = FString::SanitizeFloat(Slider->GetValue());
				Slider->SetValue((float)SliderValue);
				return PropOk(TEXT("Value"), Before, FString::SanitizeFloat(Slider->GetValue()));
			}
		}

		if (UCheckBox* CheckBox = Cast<UCheckBox>(Widget))
		{
			if (Key == TEXT("checked_state"))
			{
				int64 EnumValue = 0;
				if (!JsonEnum(Value, StaticEnum<ECheckBoxState>(), EnumValue))
				{
					return PropFail(TEXT("unsupported_property_type"), TEXT("'checked_state' takes Unchecked / Checked / Undetermined"));
				}
				const FString Before = EnumLabel(StaticEnum<ECheckBoxState>(), (int64)CheckBox->GetCheckedState());
				CheckBox->SetCheckedState((ECheckBoxState)EnumValue);
				return PropOk(TEXT("CheckedState"), Before, EnumLabel(StaticEnum<ECheckBoxState>(), (int64)CheckBox->GetCheckedState()));
			}
		}

		if (UEditableText* EditableText = Cast<UEditableText>(Widget))
		{
			if (Key == TEXT("text"))
			{
				FString Text;
				if (!JsonText(Value, Text))
				{
					return PropFail(TEXT("unsupported_property_type"), TEXT("'text' takes a string"));
				}
				const FString Before = EditableText->GetText().ToString();
				EditableText->SetText(FText::FromString(Text));
				return PropOk(TEXT("Text"), Before, EditableText->GetText().ToString());
			}
		}

		return ApplyReflectedWidgetProperty(Widget, Key, Value);
	}

	void GatherWidgetPropertyCandidates(UWidget* Widget, TArray<FString>& OutCandidates)
	{
		OutCandidates.Reset();
		for (const TCHAR* Key : { TEXT("visibility"), TEXT("is_enabled"), TEXT("tooltip"), TEXT("render_opacity"),
			TEXT("render_transform"), TEXT("render_transform_pivot"), TEXT("clipping"), TEXT("navigation"), TEXT("navigation_all") })
		{
			OutCandidates.Add(Key);
		}
		if (Cast<UTextBlock>(Widget))
		{
			for (const TCHAR* Key : { TEXT("text"), TEXT("font_size"), TEXT("color"), TEXT("justification"), TEXT("auto_wrap"), TEXT("wrap_text_at") })
			{
				OutCandidates.Add(Key);
			}
		}
		if (Cast<UButton>(Widget))
		{
			OutCandidates.Add(TEXT("background_color"));
		}
		if (Cast<UImage>(Widget))
		{
			OutCandidates.Add(TEXT("brush"));
			OutCandidates.Add(TEXT("tint"));
		}
		if (Cast<UProgressBar>(Widget))
		{
			OutCandidates.Add(TEXT("percent"));
			OutCandidates.Add(TEXT("fill_color"));
		}
		if (Cast<USlider>(Widget))
		{
			OutCandidates.Add(TEXT("value"));
		}
		if (Cast<UCheckBox>(Widget))
		{
			OutCandidates.Add(TEXT("checked_state"));
		}
		if (Cast<UEditableText>(Widget))
		{
			OutCandidates.Add(TEXT("text"));
		}
		if (Widget)
		{
			for (TFieldIterator<FProperty> It(Widget->GetClass()); It; ++It)
			{
				FProperty* Property = *It;
				if (Property && Property->HasAnyPropertyFlags(CPF_Edit))
				{
					OutCandidates.AddUnique(Property->GetName());
				}
			}
		}
		OutCandidates.Sort();
	}

	void GatherSlotFieldCandidates(UPanelSlot* Slot, TArray<FString>& OutFields)
	{
		OutFields.Reset();
		if (!Slot)
		{
			return;
		}
		if (Slot->IsA<UCanvasPanelSlot>())
		{
			for (const TCHAR* Key : { TEXT("anchors"), TEXT("alignment"), TEXT("position"), TEXT("size"), TEXT("auto_size"), TEXT("z_order") })
			{
				OutFields.Add(Key);
			}
			return;
		}
		if (Slot->IsA<UVerticalBoxSlot>() || Slot->IsA<UHorizontalBoxSlot>() || Slot->IsA<UScrollBoxSlot>())
		{
			for (const TCHAR* Key : { TEXT("padding"), TEXT("horizontal_alignment"), TEXT("vertical_alignment"), TEXT("size_rule") })
			{
				OutFields.Add(Key);
			}
			return;
		}
		if (Slot->IsA<UOverlaySlot>() || Slot->IsA<USizeBoxSlot>())
		{
			for (const TCHAR* Key : { TEXT("padding"), TEXT("horizontal_alignment"), TEXT("vertical_alignment") })
			{
				OutFields.Add(Key);
			}
			return;
		}
		// ScaleBoxSlot: alignments only. Its Padding is deprecated in 5.5 and the two alignments are
		// BlueprintReadOnly + private, so this is the one slot python cannot touch at all - which is
		// what makes it command work rather than a python one-liner.
		if (Slot->IsA<UScaleBoxSlot>())
		{
			for (const TCHAR* Key : { TEXT("horizontal_alignment"), TEXT("vertical_alignment") })
			{
				OutFields.Add(Key);
			}
			return;
		}
		if (Slot->IsA<UGridSlot>())
		{
			for (const TCHAR* Key : { TEXT("padding"), TEXT("horizontal_alignment"), TEXT("vertical_alignment"),
				TEXT("row"), TEXT("column"), TEXT("row_span"), TEXT("column_span"), TEXT("layer"), TEXT("nudge") })
			{
				OutFields.Add(Key);
			}
		}
	}

	bool ReadNumberArray(const TSharedPtr<FJsonValue>& Value, TArray<double>& Out)
	{
		Out.Reset();
		if (!Value.IsValid() || Value->Type != EJson::Array)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
		{
			double Number = 0.0;
			if (!JsonNumber(Item, Number))
			{
				return false;
			}
			Out.Add(Number);
		}
		return Out.Num() > 0;
	}

	/**
	 * Write a slot from a 'slot' object. Every key is validated against the slot class first, so a request
	 * that mixes canvas and box fields is rejected whole instead of leaving half a placement behind.
	 */
	bool ApplySlotFields(UPanelSlot* Slot, const TSharedPtr<FJsonObject>& SlotParams, TArray<FString>& OutApplied,
	                     TSharedPtr<FJsonObject>& OutError)
	{
		OutApplied.Reset();
		if (!Slot || !SlotParams.IsValid())
		{
			OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unsupported_slot"), TEXT("No slot to write"));
			return false;
		}

		TArray<FString> Fields;
		GatherSlotFieldCandidates(Slot, Fields);
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : SlotParams->Values)
		{
			if (!Fields.Contains(Pair.Key))
			{
				OutError = MakeListError(TEXT("unsupported_slot"), FString::Printf(
					TEXT("Slot class %s does not take '%s'"), *Slot->GetClass()->GetName(), *Pair.Key),
					TEXT("fields"), Fields);
				return false;
			}
		}

		if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Slot))
		{
			TArray<double> Numbers;
			FVector2D Vector = FVector2D::ZeroVector;
			if (SlotParams->HasField(TEXT("anchors")))
			{
				if (!ReadNumberArray(SlotParams->TryGetField(TEXT("anchors")), Numbers) || Numbers.Num() < 4)
				{
					OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_value"), TEXT("'anchors' takes [min_x, min_y, max_x, max_y]"));
					return false;
				}
				CanvasSlot->SetAnchors(FAnchors((float)Numbers[0], (float)Numbers[1], (float)Numbers[2], (float)Numbers[3]));
				OutApplied.Add(TEXT("anchors"));
			}
			if (SlotParams->HasField(TEXT("alignment")))
			{
				if (!JsonFloat2(SlotParams->TryGetField(TEXT("alignment")), Vector))
				{
					OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_value"), TEXT("'alignment' takes [x, y]"));
					return false;
				}
				CanvasSlot->SetAlignment(Vector);
				OutApplied.Add(TEXT("alignment"));
			}
			if (SlotParams->HasField(TEXT("position")))
			{
				if (!JsonFloat2(SlotParams->TryGetField(TEXT("position")), Vector))
				{
					OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_value"), TEXT("'position' takes [x, y]"));
					return false;
				}
				CanvasSlot->SetPosition(Vector);
				OutApplied.Add(TEXT("position"));
			}
			if (SlotParams->HasField(TEXT("size")))
			{
				if (!JsonFloat2(SlotParams->TryGetField(TEXT("size")), Vector))
				{
					OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_value"), TEXT("'size' takes [width, height]"));
					return false;
				}
				CanvasSlot->SetAutoSize(false);
				CanvasSlot->SetSize(Vector);
				OutApplied.Add(TEXT("size"));
			}
			if (SlotParams->HasField(TEXT("auto_size")))
			{
				bool bAutoSize = false;
				if (!JsonBool(SlotParams->TryGetField(TEXT("auto_size")), bAutoSize))
				{
					OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_value"), TEXT("'auto_size' takes a bool"));
					return false;
				}
				CanvasSlot->SetAutoSize(bAutoSize);
				OutApplied.Add(TEXT("auto_size"));
			}
			if (SlotParams->HasField(TEXT("z_order")))
			{
				double ZOrder = 0.0;
				if (!JsonNumber(SlotParams->TryGetField(TEXT("z_order")), ZOrder))
				{
					OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_value"), TEXT("'z_order' takes an int"));
					return false;
				}
				CanvasSlot->SetZOrder((int32)ZOrder);
				OutApplied.Add(TEXT("z_order"));
			}
			CanvasSlot->SynchronizeProperties();
			return true;
		}

		// Non-canvas slots share padding plus the two alignments; the box slots add a size rule, the grid
		// slot adds its cell. Each concrete class is spelled out rather than guessed off UPanelSlot.
		FMargin Padding;
		if (SlotParams->HasField(TEXT("padding")))
		{
			if (!JsonMargin(SlotParams->TryGetField(TEXT("padding")), Padding))
			{
				OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_value"), TEXT("'padding' takes a number, [left, top, right, bottom], or {left, top, right, bottom}"));
				return false;
			}
		}
		int64 HAlignValue = 0;
		const bool bHasHAlign = SlotParams->HasField(TEXT("horizontal_alignment"));
		if (bHasHAlign && !JsonEnum(SlotParams->TryGetField(TEXT("horizontal_alignment")), StaticEnum<EHorizontalAlignment>(), HAlignValue))
		{
			OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_value"), TEXT("'horizontal_alignment' takes Fill / Left / Center / Right"));
			return false;
		}
		int64 VAlignValue = 0;
		const bool bHasVAlign = SlotParams->HasField(TEXT("vertical_alignment"));
		if (bHasVAlign && !JsonEnum(SlotParams->TryGetField(TEXT("vertical_alignment")), StaticEnum<EVerticalAlignment>(), VAlignValue))
		{
			OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_value"), TEXT("'vertical_alignment' takes Fill / Top / Center / Bottom"));
			return false;
		}

		if (UVerticalBoxSlot* VBoxSlot = Cast<UVerticalBoxSlot>(Slot))
		{
			if (SlotParams->HasField(TEXT("padding"))) { VBoxSlot->SetPadding(Padding); OutApplied.Add(TEXT("padding")); }
			if (bHasHAlign) { VBoxSlot->SetHorizontalAlignment((EHorizontalAlignment)HAlignValue); OutApplied.Add(TEXT("horizontal_alignment")); }
			if (bHasVAlign) { VBoxSlot->SetVerticalAlignment((EVerticalAlignment)VAlignValue); OutApplied.Add(TEXT("vertical_alignment")); }
			if (SlotParams->HasField(TEXT("size_rule")))
			{
				FSlateChildSize SizeRule;
				if (!JsonSizeRule(SlotParams->TryGetField(TEXT("size_rule")), SizeRule))
				{
					OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_value"), TEXT("'size_rule' takes a number (Fill), \"Fill\" / \"Automatic\", or {\"rule\": .., \"value\": ..}"));
					return false;
				}
				VBoxSlot->SetSize(SizeRule);
				OutApplied.Add(TEXT("size_rule"));
			}
			VBoxSlot->SynchronizeProperties();
			return true;
		}
		if (UHorizontalBoxSlot* HBoxSlot = Cast<UHorizontalBoxSlot>(Slot))
		{
			if (SlotParams->HasField(TEXT("padding"))) { HBoxSlot->SetPadding(Padding); OutApplied.Add(TEXT("padding")); }
			if (bHasHAlign) { HBoxSlot->SetHorizontalAlignment((EHorizontalAlignment)HAlignValue); OutApplied.Add(TEXT("horizontal_alignment")); }
			if (bHasVAlign) { HBoxSlot->SetVerticalAlignment((EVerticalAlignment)VAlignValue); OutApplied.Add(TEXT("vertical_alignment")); }
			if (SlotParams->HasField(TEXT("size_rule")))
			{
				FSlateChildSize SizeRule;
				if (!JsonSizeRule(SlotParams->TryGetField(TEXT("size_rule")), SizeRule))
				{
					OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_value"), TEXT("'size_rule' takes a number (Fill), \"Fill\" / \"Automatic\", or {\"rule\": .., \"value\": ..}"));
					return false;
				}
				HBoxSlot->SetSize(SizeRule);
				OutApplied.Add(TEXT("size_rule"));
			}
			HBoxSlot->SynchronizeProperties();
			return true;
		}
		if (UScrollBoxSlot* ScrollSlot = Cast<UScrollBoxSlot>(Slot))
		{
			if (SlotParams->HasField(TEXT("padding"))) { ScrollSlot->SetPadding(Padding); OutApplied.Add(TEXT("padding")); }
			if (bHasHAlign) { ScrollSlot->SetHorizontalAlignment((EHorizontalAlignment)HAlignValue); OutApplied.Add(TEXT("horizontal_alignment")); }
			if (bHasVAlign) { ScrollSlot->SetVerticalAlignment((EVerticalAlignment)VAlignValue); OutApplied.Add(TEXT("vertical_alignment")); }
			if (SlotParams->HasField(TEXT("size_rule")))
			{
				FSlateChildSize SizeRule;
				if (!JsonSizeRule(SlotParams->TryGetField(TEXT("size_rule")), SizeRule))
				{
					OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_value"), TEXT("'size_rule' takes a number (Fill), \"Fill\" / \"Automatic\", or {\"rule\": .., \"value\": ..}"));
					return false;
				}
				ScrollSlot->SetSize(SizeRule);
				OutApplied.Add(TEXT("size_rule"));
			}
			ScrollSlot->SynchronizeProperties();
			return true;
		}
		if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(Slot))
		{
			if (SlotParams->HasField(TEXT("padding"))) { OverlaySlot->SetPadding(Padding); OutApplied.Add(TEXT("padding")); }
			if (bHasHAlign) { OverlaySlot->SetHorizontalAlignment((EHorizontalAlignment)HAlignValue); OutApplied.Add(TEXT("horizontal_alignment")); }
			if (bHasVAlign) { OverlaySlot->SetVerticalAlignment((EVerticalAlignment)VAlignValue); OutApplied.Add(TEXT("vertical_alignment")); }
			OverlaySlot->SynchronizeProperties();
			return true;
		}
		if (USizeBoxSlot* SizeBoxSlot = Cast<USizeBoxSlot>(Slot))
		{
			if (SlotParams->HasField(TEXT("padding"))) { SizeBoxSlot->SetPadding(Padding); OutApplied.Add(TEXT("padding")); }
			if (bHasHAlign) { SizeBoxSlot->SetHorizontalAlignment((EHorizontalAlignment)HAlignValue); OutApplied.Add(TEXT("horizontal_alignment")); }
			if (bHasVAlign) { SizeBoxSlot->SetVerticalAlignment((EVerticalAlignment)VAlignValue); OutApplied.Add(TEXT("vertical_alignment")); }
			SizeBoxSlot->SynchronizeProperties();
			return true;
		}
		if (UGridSlot* GridSlot = Cast<UGridSlot>(Slot))
		{
			if (SlotParams->HasField(TEXT("padding"))) { GridSlot->SetPadding(Padding); OutApplied.Add(TEXT("padding")); }
			if (bHasHAlign) { GridSlot->SetHorizontalAlignment((EHorizontalAlignment)HAlignValue); OutApplied.Add(TEXT("horizontal_alignment")); }
			if (bHasVAlign) { GridSlot->SetVerticalAlignment((EVerticalAlignment)VAlignValue); OutApplied.Add(TEXT("vertical_alignment")); }
			double Number = 0.0;
			if (SlotParams->HasField(TEXT("row")) && JsonNumber(SlotParams->TryGetField(TEXT("row")), Number))
			{
				GridSlot->SetRow((int32)Number);
				OutApplied.Add(TEXT("row"));
			}
			if (SlotParams->HasField(TEXT("column")) && JsonNumber(SlotParams->TryGetField(TEXT("column")), Number))
			{
				GridSlot->SetColumn((int32)Number);
				OutApplied.Add(TEXT("column"));
			}
			if (SlotParams->HasField(TEXT("row_span")) && JsonNumber(SlotParams->TryGetField(TEXT("row_span")), Number))
			{
				GridSlot->SetRowSpan((int32)Number);
				OutApplied.Add(TEXT("row_span"));
			}
			if (SlotParams->HasField(TEXT("column_span")) && JsonNumber(SlotParams->TryGetField(TEXT("column_span")), Number))
			{
				GridSlot->SetColumnSpan((int32)Number);
				OutApplied.Add(TEXT("column_span"));
			}
			if (SlotParams->HasField(TEXT("layer")) && JsonNumber(SlotParams->TryGetField(TEXT("layer")), Number))
			{
				GridSlot->SetLayer((int32)Number);
				OutApplied.Add(TEXT("layer"));
			}
			if (SlotParams->HasField(TEXT("nudge")))
			{
				FVector2D Nudge = FVector2D::ZeroVector;
				if (!JsonFloat2(SlotParams->TryGetField(TEXT("nudge")), Nudge))
				{
					OutError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_value"), TEXT("'nudge' takes [x, y]"));
					return false;
				}
				GridSlot->SetNudge(Nudge);
				OutApplied.Add(TEXT("nudge"));
			}
			GridSlot->SynchronizeProperties();
			return true;
		}

		if (UScaleBoxSlot* ScaleBoxSlot = Cast<UScaleBoxSlot>(Slot))
		{
			// Alignments only: 'padding' is not offered because UScaleBoxSlot::Padding is deprecated in
			// 5.5 (and set_editor_property cannot reach either field - see the field list above).
			if (bHasHAlign) { ScaleBoxSlot->SetHorizontalAlignment((EHorizontalAlignment)HAlignValue); OutApplied.Add(TEXT("horizontal_alignment")); }
			if (bHasVAlign) { ScaleBoxSlot->SetVerticalAlignment((EVerticalAlignment)VAlignValue); OutApplied.Add(TEXT("vertical_alignment")); }
			ScaleBoxSlot->SynchronizeProperties();
			return true;
		}

		OutError = MakeListError(TEXT("unsupported_slot"), FString::Printf(
			TEXT("Slot class %s has no writable fields"), *Slot->GetClass()->GetName()), TEXT("fields"), Fields);
		return false;
	}
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleSetWidgetSlot(const TSharedPtr<FJsonObject>& Params)
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

	// Load the Widget Blueprint (in-memory lookup, supports not-yet-saved assets)
	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(FUnrealMCPCommonUtils::FindAsset(BlueprintName));
	if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("widget_blueprint_not_found"), FString::Printf(TEXT("Widget Blueprint '%s' not found"), *BlueprintName));
	}

	UWidget* Widget = WidgetBlueprint->WidgetTree->FindWidget(FName(*WidgetName));
	if (!Widget)
	{
		TArray<UWidget*> AllWidgets;
		WidgetBlueprint->WidgetTree->GetAllWidgets(AllWidgets);
		TArray<FString> Names;
		for (UWidget* Candidate : AllWidgets)
		{
			if (Candidate)
			{
				Names.Add(Candidate->GetName());
			}
		}
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("widget_not_found"), FString::Printf(
			TEXT("Widget '%s' not found in '%s' (widgets: %s)"), *WidgetName, *BlueprintName,
			*FString::Join(Names, TEXT(", "))));
	}

	UPanelSlot* WidgetSlot = Widget->Slot;
	if (!WidgetSlot)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unsupported_slot"), FString::Printf(
			TEXT("Widget '%s' has no slot: it is the root widget, and only panel children carry slot values"), *WidgetName));
	}

	const TSharedPtr<FJsonObject>* SlotParamsPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("slot"), SlotParamsPtr) || !SlotParamsPtr->IsValid())
	{
		TArray<FString> Fields;
		GatherSlotFieldCandidates(WidgetSlot, Fields);
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), FString::Printf(
			TEXT("Missing 'slot' object parameter for '%s' (slot class %s, keys: %s)"),
			*WidgetName, *WidgetSlot->GetClass()->GetName(), *FString::Join(Fields, TEXT(", "))));
	}

	// An empty slot object asks for nothing: answer with the current slot, no compile, no save.
	if ((*SlotParamsPtr)->Values.Num() == 0)
	{
		TSharedPtr<FJsonObject> Unchanged = MakeShared<FJsonObject>();
		Unchanged->SetStringField(TEXT("blueprint_name"), BlueprintName);
		Unchanged->SetStringField(TEXT("widget_name"), WidgetName);
		Unchanged->SetObjectField(TEXT("slot"), SlotToJson(WidgetSlot));
		Unchanged->SetArrayField(TEXT("applied"), TArray<TSharedPtr<FJsonValue>>());
		Unchanged->SetBoolField(TEXT("changed"), false);
		Unchanged->SetBoolField(TEXT("saved"), false);
		return Unchanged;
	}

	// Every key must belong to this slot class: the whole write is rejected rather than half applied.
	TArray<FString> Applied;
	TSharedPtr<FJsonObject> SlotError;
	if (!ApplySlotFields(WidgetSlot, *SlotParamsPtr, Applied, SlotError))
	{
		return SlotError;
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("changed"), true);
	ResultObj->SetStringField(TEXT("blueprint_name"), BlueprintName);
	ResultObj->SetStringField(TEXT("widget_name"), WidgetName);
	ResultObj->SetObjectField(TEXT("slot"), SlotToJson(WidgetSlot));
	TArray<TSharedPtr<FJsonValue>> AppliedValues;
	for (const FString& Key : Applied)
	{
		AppliedValues.Add(MakeShared<FJsonValueString>(Key));
	}
	ResultObj->SetArrayField(TEXT("applied"), AppliedValues);
	FinishWidgetEdit(WidgetBlueprint, Params, ResultObj);
	return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleAddWidget(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'blueprint_name' parameter"));
	}
	FString WidgetClassName;
	if (!Params->TryGetStringField(TEXT("widget_class"), WidgetClassName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'widget_class' parameter"));
	}
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'widget_name' parameter"));
	}

	TArray<FString> Candidates;
	UClass* WidgetClass = ResolveWidgetClassByName(WidgetClassName, UWidget::StaticClass(), Candidates);
	if (!WidgetClass)
	{
		return MakeListError(TEXT("unknown_widget_class"), FString::Printf(
			TEXT("Widget class '%s' does not resolve to a UWidget subclass"), *WidgetClassName),
			TEXT("candidates"), Candidates);
	}

	TSharedPtr<FJsonObject> Error;
	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByParam(BlueprintName, Error);
	if (!WidgetBlueprint)
	{
		return Error;
	}

	// No silent renaming: a taken name is an error the caller has to resolve.
	if (WidgetBlueprint->WidgetTree->FindWidget(FName(*WidgetName)))
	{
		TArray<FString> Names;
		GatherWidgetNames(WidgetBlueprint->WidgetTree, Names);
		return MakeListError(TEXT("name_collision"), FString::Printf(
			TEXT("A widget named '%s' already exists in '%s'"), *WidgetName, *BlueprintName),
			TEXT("widget_names"), Names);
	}

	FString ParentName;
	UPanelWidget* Parent = ResolveParentPanel(WidgetBlueprint, Params, ParentName, Error);
	if (Error.IsValid())
	{
		return Error;
	}

	bool bAsVariable = false;
	Params->TryGetBoolField(TEXT("as_variable"), bAsVariable);

	// ConstructWidget is the only path that gives the tree ownership; NewObject would not register the widget.
	UWidget* NewWidget = WidgetClass->IsChildOf(UUserWidget::StaticClass())
		? Cast<UWidget>(WidgetBlueprint->WidgetTree->ConstructWidget<UUserWidget>(WidgetClass, FName(*WidgetName)))
		: Cast<UWidget>(WidgetBlueprint->WidgetTree->ConstructWidget<UWidget>(WidgetClass, FName(*WidgetName)));
	if (!NewWidget)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("create_failed"), FString::Printf(
			TEXT("Failed to construct a %s named '%s'"), *WidgetClass->GetName(), *WidgetName));
	}
	NewWidget->bIsVariable = bAsVariable;

	if (!Parent)
	{
		// The tree had no root, so the new widget is it - the same thing the designer's first drop does.
		WidgetBlueprint->WidgetTree->RootWidget = NewWidget;
		ParentName.Reset();
	}
	else
	{
		if (!Parent->AddChild(NewWidget))
		{
			// A single-child panel that is already full: drop the half-built widget rather than orphan it.
			NewWidget->Rename(nullptr, GetTransientPackage());
			TArray<FString> Panels;
			GatherPanelNames(WidgetBlueprint->WidgetTree, Panels);
			return MakeListError(TEXT("unsupported_parent"), FString::Printf(
				TEXT("Panel '%s' (%s) refused the child; it holds a limited number of children"),
				*ParentName, *Parent->GetClass()->GetName()), TEXT("panels"), Panels);
		}
	}

	// Optional slot, written in the same call. The slot class only exists once the parent has
	// adopted the widget, so a rejected slot unwinds exactly like a refused AddChild: the widget
	// leaves the panel and the tree, and the tree is as before the call.
	const TSharedPtr<FJsonObject>* SlotParamsPtr = nullptr;
	const bool bHasSlot = Params->TryGetObjectField(TEXT("slot"), SlotParamsPtr) && SlotParamsPtr && SlotParamsPtr->IsValid()
		&& (*SlotParamsPtr)->Values.Num() > 0;
	if (bHasSlot)
	{
		TSharedPtr<FJsonObject> SlotError;
		TArray<FString> SlotApplied;
		if (!Parent)
		{
			SlotError = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unsupported_slot"),
				TEXT("The new widget became the root, and the root widget has no slot"));
		}
		else if (!ApplySlotFields(NewWidget->Slot, *SlotParamsPtr, SlotApplied, SlotError))
		{
			// SlotError carries the slot class' full field list.
		}
		if (SlotError.IsValid())
		{
			if (Parent)
			{
				Parent->RemoveChild(NewWidget);
			}
			else
			{
				WidgetBlueprint->WidgetTree->RootWidget = nullptr;
			}
			NewWidget->Rename(nullptr, GetTransientPackage());
			return SlotError;
		}
	}

	// UniformGridSlot is not a slot this command writes (python sets its row/column directly), so the
	// warning is only raised where the command itself offers the fix.
	TArray<TSharedPtr<FJsonValue>> Warnings;
	if (Parent && Parent->IsA<UGridPanel>())
	{
		const bool bPlaced = bHasSlot && (*SlotParamsPtr)->HasField(TEXT("row")) && (*SlotParamsPtr)->HasField(TEXT("column"));
		if (!bPlaced)
		{
			TSharedPtr<FJsonObject> Warning = MakeShared<FJsonObject>();
			Warning->SetStringField(TEXT("code"), TEXT("grid_slot_unplaced"));
			Warning->SetStringField(TEXT("message"), FString::Printf(
				TEXT("'%s' sits at row 0 / column 0 of %s: AddChild does not lay grid children out, so children without row/column stack in one cell. Pass slot={row, column} (here or via set_widget_slot)."),
				*NewWidget->GetName(), *ParentName));
			Warnings.Add(MakeShared<FJsonValueObject>(Warning));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("warnings"), Warnings);
	Result->SetStringField(TEXT("blueprint_name"), BlueprintName);
	Result->SetStringField(TEXT("widget_name"), NewWidget->GetName());
	Result->SetStringField(TEXT("widget_class"), NewWidget->GetClass()->GetName());
	Result->SetStringField(TEXT("parent_widget"), ParentName);
	Result->SetBoolField(TEXT("became_root"), ParentName.IsEmpty());
	Result->SetBoolField(TEXT("is_variable"), NewWidget->bIsVariable);
	if (TSharedPtr<FJsonObject> SlotJson = SlotToJson(NewWidget->Slot))
	{
		Result->SetObjectField(TEXT("slot"), SlotJson);
	}
	FinishWidgetEdit(WidgetBlueprint, Params, Result);
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleSetWidgetProperties(const TSharedPtr<FJsonObject>& Params)
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
	const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("props"), PropsPtr) || !PropsPtr->IsValid())
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'props' object parameter"));
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

	TArray<TSharedPtr<FJsonValue>> Applied;
	TArray<TSharedPtr<FJsonValue>> Failed;
	TArray<TPair<TSharedPtr<FJsonObject>, FString>> AppliedProperties;
	TSet<FString> RequestedProperties;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PropsPtr)->Values)
	{
		const FPropWriteResult Write = ApplyWidgetProperty(Widget, WidgetBlueprint->WidgetTree, Pair.Key, Pair.Value);
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("key"), Pair.Key);
		Entry->SetStringField(TEXT("property"), Write.Property);
		if (!Write.Property.IsEmpty())
		{
			RequestedProperties.Add(Write.Property);
		}
		if (Write.bApplied)
		{
			Entry->SetStringField(TEXT("value_before"), Write.ValueBefore);
			Entry->SetStringField(TEXT("value_after"), Write.ValueAfter);
			Applied.Add(MakeShared<FJsonValueObject>(Entry));
			AppliedProperties.Emplace(Entry, Write.Property);
		}
		else
		{
			Entry->SetStringField(TEXT("error"), Write.Code);
			Entry->SetStringField(TEXT("message"), Write.Message);
			TArray<TSharedPtr<FJsonValue>> Candidates;
			for (const FString& Candidate : Write.Candidates)
			{
				Candidates.Add(MakeShared<FJsonValueString>(Candidate));
			}
			Entry->SetArrayField(TEXT("candidates"), Candidates);
			Failed.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	// Reflection writes skip the engine setters, and the setters of override-gated properties
	// (USizeBox::SetWidthOverride, ...) also flip bOverride_<X>; without it the value is inert.
	// Do the same here unless the caller wrote that flag itself.
	for (const TPair<TSharedPtr<FJsonObject>, FString>& Written : AppliedProperties)
	{
		const FString FlagName = TEXT("bOverride_") + Written.Value;
		FBoolProperty* Flag = FindFProperty<FBoolProperty>(Widget->GetClass(), FName(*FlagName));
		if (!Flag || RequestedProperties.Contains(FlagName))
		{
			continue;
		}
		void* FlagAddress = Flag->ContainerPtrToValuePtr<void>(Widget);
		if (Flag->GetPropertyValue(FlagAddress))
		{
			continue;
		}
		Flag->SetPropertyValue(FlagAddress, true);
		Widget->SynchronizeProperties();
		TSharedPtr<FJsonObject> Implied = MakeShared<FJsonObject>();
		Implied->SetStringField(TEXT("property"), FlagName);
		Implied->SetBoolField(TEXT("value_before"), false);
		Implied->SetBoolField(TEXT("value_after"), Flag->GetPropertyValue(FlagAddress));
		TArray<TSharedPtr<FJsonValue>> ImpliedList;
		ImpliedList.Add(MakeShared<FJsonValueObject>(Implied));
		Written.Key->SetArrayField(TEXT("implied"), ImpliedList);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("blueprint_name"), BlueprintName);
	Result->SetStringField(TEXT("widget_name"), Widget->GetName());
	Result->SetArrayField(TEXT("applied"), Applied);
	Result->SetNumberField(TEXT("applied_count"), Applied.Num());
	Result->SetArrayField(TEXT("failed"), Failed);
	Result->SetNumberField(TEXT("failed_count"), Failed.Num());
	Result->SetObjectField(TEXT("properties"), WidgetPropsToJson(Widget));
	FinishWidgetEdit(WidgetBlueprint, Params, Result);
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleGetWidgetTree(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'blueprint_name' parameter"));
	}

	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(FUnrealMCPCommonUtils::FindAsset(BlueprintName));
	if (!WidgetBlueprint)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("widget_blueprint_not_found"), FString::Printf(
			TEXT("Widget Blueprint '%s' not found"), *BlueprintName));
	}

	int32 WidgetCount = 0;
	TArray<TSharedPtr<FJsonValue>> WidgetNames;

	TFunction<TSharedPtr<FJsonObject>(UWidget*)> Walk = [&](UWidget* Widget) -> TSharedPtr<FJsonObject>
	{
		if (!Widget)
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("name"), Widget->GetName());
		Node->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
		Node->SetStringField(TEXT("visibility"),
			EnumLabel(StaticEnum<ESlateVisibility>(), (int64)Widget->GetVisibility()));
		Node->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);
		Node->SetBoolField(TEXT("is_enabled"), Widget->GetIsEnabled());
		Node->SetNumberField(TEXT("render_opacity"), Widget->GetRenderOpacity());

		const FWidgetTransform Transform = Widget->GetRenderTransform();
		TSharedPtr<FJsonObject> Xf = MakeShared<FJsonObject>();
		Xf->SetArrayField(TEXT("translation"), {JNum(Transform.Translation.X), JNum(Transform.Translation.Y)});
		Xf->SetArrayField(TEXT("scale"), {JNum(Transform.Scale.X), JNum(Transform.Scale.Y)});
		Xf->SetArrayField(TEXT("shear"), {JNum(Transform.Shear.X), JNum(Transform.Shear.Y)});
		Xf->SetNumberField(TEXT("angle"), Transform.Angle);
		Node->SetObjectField(TEXT("render_transform"), Xf);

		const FText ToolTip = Widget->GetToolTipText();
		if (!ToolTip.IsEmpty())
		{
			Node->SetStringField(TEXT("tooltip"), ToolTip.ToString());
		}

		TSharedPtr<FJsonObject> Props = WidgetPropsToJson(Widget);
		if (Props->Values.Num() > 0)
		{
			Node->SetObjectField(TEXT("properties"), Props);
		}
		if (TSharedPtr<FJsonObject> SlotJson = SlotToJson(Widget->Slot))
		{
			Node->SetObjectField(TEXT("slot"), SlotJson);
		}

		if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
		{
			TArray<TSharedPtr<FJsonValue>> Children;
			for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
			{
				if (TSharedPtr<FJsonObject> Child = Walk(Panel->GetChildAt(Index)))
				{
					Children.Add(MakeShared<FJsonValueObject>(Child));
				}
			}
			Node->SetNumberField(TEXT("child_count"), Children.Num());
			Node->SetArrayField(TEXT("children"), Children);
		}

		++WidgetCount;
		WidgetNames.Add(MakeShared<FJsonValueString>(Widget->GetName()));
		return Node;
	};

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("blueprint_name"), WidgetBlueprint->GetName());
	Result->SetStringField(TEXT("path"), WidgetBlueprint->GetPathName());
	Result->SetStringField(TEXT("parent_class"), WidgetBlueprint->ParentClass ? WidgetBlueprint->ParentClass->GetName() : FString());
	const bool bHasRoot = WidgetBlueprint->WidgetTree && WidgetBlueprint->WidgetTree->RootWidget;
	Result->SetBoolField(TEXT("has_root"), bHasRoot);
	if (bHasRoot)
	{
		Result->SetObjectField(TEXT("root"), Walk(WidgetBlueprint->WidgetTree->RootWidget));
	}
	Result->SetNumberField(TEXT("widget_count"), WidgetCount);
	Result->SetArrayField(TEXT("widget_names"), WidgetNames);

	UEnum* KindEnum = StaticEnum<EBindingKind>();
	// effective = the same (widget, property) pair is also in the runtime table the widget applies.
	const TArray<TSharedPtr<FJsonValue>> Bindings = BindingsToJsonArray(WidgetBlueprint, /*bIncludeEffective=*/true);
	Result->SetArrayField(TEXT("bindings"), Bindings);
	Result->SetNumberField(TEXT("binding_count"), Bindings.Num());

	// Runtime side: what the generated class applies when the widget is constructed. A binding that
	// only shows up in the editor array above is not live - this is the field that proves it works.
	if (UWidgetBlueprintGeneratedClass* GeneratedClass = Cast<UWidgetBlueprintGeneratedClass>(WidgetBlueprint->GeneratedClass))
	{
		TArray<TSharedPtr<FJsonValue>> RuntimeBindings;
		for (const FDelegateRuntimeBinding& Binding : GeneratedClass->Bindings)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("widget"), Binding.ObjectName);
			Obj->SetStringField(TEXT("property"), Binding.PropertyName.ToString());
			Obj->SetStringField(TEXT("function"), Binding.FunctionName.ToString());
			Obj->SetStringField(TEXT("kind"), EnumLabel(KindEnum, (int64)Binding.Kind));
			RuntimeBindings.Add(MakeShared<FJsonValueObject>(Obj));
		}
		Result->SetArrayField(TEXT("runtime_bindings"), RuntimeBindings);
		Result->SetNumberField(TEXT("runtime_binding_count"), RuntimeBindings.Num());
	}

	TArray<UEdGraph*> Graphs;
	WidgetBlueprint->GetAllGraphs(Graphs);
	TArray<TSharedPtr<FJsonValue>> Events;
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph)
		{
			continue;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_ComponentBoundEvent* BoundEvent = Cast<UK2Node_ComponentBoundEvent>(Node))
			{
				TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetStringField(TEXT("widget"), BoundEvent->GetComponentPropertyName().ToString());
				Obj->SetStringField(TEXT("event"), BoundEvent->DelegatePropertyName.ToString());
				Obj->SetStringField(TEXT("graph"), Graph->GetName());
				Events.Add(MakeShared<FJsonValueObject>(Obj));
			}
		}
	}
	Result->SetArrayField(TEXT("events"), Events);
	Result->SetNumberField(TEXT("event_count"), Events.Num());
	// Same compile verdict as every other UMG command: the enum name is the stable field, the number is
	// the code (a bare "3" was the historical shape here and meant the same thing under a different name).
	Result->SetStringField(TEXT("status"), BlueprintStatusLabel(WidgetBlueprint));
	Result->SetNumberField(TEXT("status_code"), (int32)WidgetBlueprint->Status);
	Result->SetBoolField(TEXT("compiled"),
		WidgetBlueprint->Status == BS_UpToDate || WidgetBlueprint->Status == BS_UpToDateWithWarnings);
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleSetWidgetVariable(const TSharedPtr<FJsonObject>& Params)
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
	bool bAsVariable = false;
	if (!Params->TryGetBoolField(TEXT("as_variable"), bAsVariable))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"),
			TEXT("Missing 'as_variable' parameter (true exposes the widget as a variable)"));
	}

	TSharedPtr<FJsonObject> Error;
	UWidgetBlueprint* Blueprint = LoadWidgetBlueprintByParam(BlueprintName, Error);
	if (!Blueprint)
	{
		return Error;
	}
	UWidget* Widget = FindWidgetOrError(Blueprint, WidgetName, Blueprint->GetName(), Error);
	if (!Widget)
	{
		return Error;
	}

	const bool bWasVariable = Widget->bIsVariable != 0;
	const FName WidgetFName(*WidgetName);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
	Result->SetStringField(TEXT("widget_name"), Widget->GetName());
	Result->SetBoolField(TEXT("was_variable"), bWasVariable);
	Result->SetBoolField(TEXT("is_variable"), bWasVariable);
	Result->SetBoolField(TEXT("generated_property_present"),
		Blueprint->GeneratedClass && Blueprint->GeneratedClass->FindPropertyByName(WidgetFName) != nullptr);

	if (bWasVariable == bAsVariable)
	{
		// Already in the requested state: report it instead of writing (and saving) for nothing.
		Result->SetBoolField(TEXT("changed"), false);
		Result->SetStringField(TEXT("status"), BlueprintStatusLabel(Blueprint));
		Result->SetStringField(TEXT("note"), FString::Printf(TEXT("'%s' is already %sa widget variable"),
			*Widget->GetName(), bAsVariable ? TEXT("") : TEXT("not ")));
		FinishWidgetWrite(Blueprint, Params, Result);
		return Result;
	}

	Blueprint->Modify();
	Widget->Modify();
	Widget->bIsVariable = bAsVariable;
	// The generated class property (i.e. what the graph and the runtime see) only appears or disappears
	// on a compile - the flag on its own changes nothing, exactly like the property binding table.
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

	const bool bPropertyPresent = Blueprint->GeneratedClass
		&& Blueprint->GeneratedClass->FindPropertyByName(WidgetFName) != nullptr;

	Result->SetBoolField(TEXT("changed"), true);
	Result->SetBoolField(TEXT("is_variable"), Widget->bIsVariable != 0);
	Result->SetBoolField(TEXT("generated_property_present"), bPropertyPresent);
	Result->SetBoolField(TEXT("compiled"), bCompiled);
	Result->SetNumberField(TEXT("num_errors"), Errors);
	Result->SetNumberField(TEXT("num_warnings"), Warnings);
	Result->SetArrayField(TEXT("compile_errors"), ErrorItems);

	if (bPropertyPresent != bAsVariable)
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error_code"), TEXT("variable_not_effective"));
		Result->SetStringField(TEXT("message"), FString::Printf(
			TEXT("'%s' is %s a generated class property after compiling (bIsVariable=%d)"),
			*Widget->GetName(), bPropertyPresent ? TEXT("still") : TEXT("not"),
			Widget->bIsVariable ? 1 : 0));
	}

	FinishWidgetWrite(Blueprint, Params, Result);
	return Result;
}