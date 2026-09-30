// UMG commands: widget events and property bindings (generalized bind / unbind / prune).
#include "Commands/UnrealMCPUMGCommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Commands/Blueprint/UnrealMCPBlueprintGraphOps.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "UObject/TextProperty.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Variable.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Components/Widget.h"
#include "EdGraph/EdGraph.h"
#include "UObject/UnrealType.h"
#include "Commands/UMG/UnrealMCPUMGCommandHelpers.h"

namespace
{
	/** What a bindable property is: the name the caller writes, plus the type its delegate returns. */
	struct FBindablePropertyInfo
	{
		FString Name;
		FString Type;
		FString SubClass;
		FString TypeLabel;
	};

	/** Declared here: the enumeration below filters through it before its own definition. */
	bool DescribeDelegateReturnType(const FProperty* ReturnProperty, FBindablePropertyInfo& OutInfo);

	void GatherBindableProperties(UWidget* Widget, TArray<FString>& OutNames)
	{
		OutNames.Reset();
		if (!Widget)
		{
			return;
		}
		for (TFieldIterator<FDelegateProperty> It(Widget->GetClass()); It; ++It)
		{
			const FDelegateProperty* Delegate = *It;
			if (!Delegate)
			{
				continue;
			}
			const FString DelegateName = Delegate->GetName();
			if (!DelegateName.EndsWith(TEXT("Delegate")))
			{
				continue;
			}
			const FString PropertyName = DelegateName.LeftChop(8);
			if (PropertyName.IsEmpty() || !FindFProperty<FProperty>(Widget->GetClass(), FName(*PropertyName)))
			{
				continue;
			}
			UFunction* Signature = Delegate->SignatureFunction;
			if (!Signature || Signature->NumParms != 1)
			{
				continue;
			}
			FBindablePropertyInfo Info;
			if (DescribeDelegateReturnType(Signature->GetReturnProperty(), Info))
			{
				OutNames.AddUnique(PropertyName);
			}
		}
		OutNames.Sort();
	}

	bool DescribeDelegateReturnType(const FProperty* ReturnProperty, FBindablePropertyInfo& OutInfo)
	{
		if (!ReturnProperty)
		{
			return false;
		}
		if (CastField<FTextProperty>(ReturnProperty))
		{
			OutInfo.Type = TEXT("text");
			OutInfo.TypeLabel = TEXT("Text");
			return true;
		}
		if (CastField<FFloatProperty>(ReturnProperty))
		{
			OutInfo.Type = TEXT("float");
			OutInfo.TypeLabel = TEXT("Float");
			return true;
		}
		if (CastField<FDoubleProperty>(ReturnProperty))
		{
			OutInfo.Type = TEXT("double");
			OutInfo.TypeLabel = TEXT("Double");
			return true;
		}
		if (CastField<FBoolProperty>(ReturnProperty))
		{
			OutInfo.Type = TEXT("bool");
			OutInfo.TypeLabel = TEXT("Bool");
			return true;
		}
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(ReturnProperty))
		{
			if (ByteProperty->Enum)
			{
				OutInfo.Type = TEXT("enum");
				OutInfo.SubClass = ByteProperty->Enum->GetPathName();
				OutInfo.TypeLabel = ByteProperty->Enum->GetName();
			}
			else
			{
				OutInfo.Type = TEXT("byte");
				OutInfo.TypeLabel = TEXT("Byte");
			}
			return true;
		}
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(ReturnProperty))
		{
			UEnum* Enum = EnumProperty->GetEnum();
			OutInfo.Type = TEXT("enum");
			OutInfo.SubClass = Enum ? Enum->GetPathName() : FString();
			OutInfo.TypeLabel = Enum ? Enum->GetName() : TEXT("Enum");
			return !OutInfo.SubClass.IsEmpty();
		}
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(ReturnProperty))
		{
			OutInfo.Type = TEXT("struct");
			OutInfo.SubClass = StructProperty->Struct ? StructProperty->Struct->GetPathName() : FString();
			OutInfo.TypeLabel = StructProperty->Struct ? StructProperty->Struct->GetName() : TEXT("Struct");
			return !OutInfo.SubClass.IsEmpty();
		}
		if (const FClassProperty* ClassProperty = CastField<FClassProperty>(ReturnProperty))
		{
			const UClass* MetaClass = ClassProperty->MetaClass ? ClassProperty->MetaClass.Get() : ClassProperty->PropertyClass.Get();
			OutInfo.Type = TEXT("class");
			OutInfo.SubClass = MetaClass ? MetaClass->GetPathName() : FString();
			OutInfo.TypeLabel = MetaClass ? MetaClass->GetName() : TEXT("Class");
			return true;
		}
		if (const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(ReturnProperty))
		{
			OutInfo.Type = TEXT("object");
			OutInfo.SubClass = ObjectProperty->PropertyClass ? ObjectProperty->PropertyClass->GetPathName() : FString();
			OutInfo.TypeLabel = ObjectProperty->PropertyClass ? ObjectProperty->PropertyClass->GetName() : TEXT("Object");
			return true;
		}
		return false;
	}

	bool DescribeBindableProperty(UWidget* Widget, const FString& PropertyName, FBindablePropertyInfo& OutInfo,
	                              bool& bOutDelegateFound, TArray<FString>& OutCandidates)
	{
		OutCandidates.Reset();
		bOutDelegateFound = false;
		OutInfo = FBindablePropertyInfo();
		OutInfo.Name = PropertyName;
		if (!Widget || PropertyName.IsEmpty())
		{
			return false;
		}

		FDelegateProperty* Delegate = FindFProperty<FDelegateProperty>(Widget->GetClass(),
			FName(*(PropertyName + TEXT("Delegate"))));
		if (!Delegate)
		{
			GatherBindableProperties(Widget, OutCandidates);
			return false;
		}
		bOutDelegateFound = true;

		UFunction* Signature = Delegate->SignatureFunction;
		const FProperty* ReturnProperty = Signature ? Signature->GetReturnProperty() : nullptr;
		if (!Signature || Signature->NumParms != 1 || !DescribeDelegateReturnType(ReturnProperty, OutInfo))
		{
			GatherBindableProperties(Widget, OutCandidates);
			return false;
		}
		return true;
	}

	/**
	 * Who keeps a binding artifact alive: another binding entry pointing at the same getter, a node
	 * calling the getter, or a node reading the backing variable (outside the getter's own graph).
	 * IgnoreBindingIndex skips the entry the caller is about to delete itself.
	 */
	void GatherBindingArtifactReferences(UWidgetBlueprint* Blueprint, const FString& FunctionName,
	                                     const FString& VariableName, int32 IgnoreBindingIndex,
	                                     TArray<FWidgetReferenceInfo>& OutRefs)
	{
		OutRefs.Reset();
		if (!Blueprint)
		{
			return;
		}
		const FName FunctionFName(*FunctionName);
		const FName VariableFName(*VariableName);
		const bool bHasFunction = !FunctionName.IsEmpty();
		const bool bHasVariable = !VariableName.IsEmpty();

		for (int32 Index = 0; Index < Blueprint->Bindings.Num(); ++Index)
		{
			if (Index == IgnoreBindingIndex)
			{
				continue;
			}
			const FDelegateEditorBinding& Binding = Blueprint->Bindings[Index];
			if (bHasFunction && Binding.FunctionName == FunctionFName)
			{
				FWidgetReferenceInfo Info;
				Info.Kind = TEXT("binding_entry");
				Info.Graph = TEXT("<widget blueprint>");
				Info.Node = FString::Printf(TEXT("%s.%s"), *Binding.ObjectName, *Binding.PropertyName.ToString());
				Info.Detail = FunctionName;
				OutRefs.Add(Info);
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
			const bool bInsideGetter = bHasFunction && Graph->GetName() == FunctionName;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}
				if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
				{
					if (bHasFunction && CallNode->FunctionReference.GetMemberName() == FunctionFName)
					{
						FWidgetReferenceInfo Info;
						Info.Kind = TEXT("function_node");
						Info.Graph = Graph->GetName();
						Info.Node = CallNode->GetName();
						Info.Detail = FunctionName;
						OutRefs.Add(Info);
					}
					continue;
				}
				if (bHasVariable && !bInsideGetter)
				{
					if (UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node))
					{
						if (VariableNode->GetVarName() == VariableFName)
						{
							FWidgetReferenceInfo Info;
							Info.Kind = TEXT("variable_node");
							Info.Graph = Graph->GetName();
							Info.Node = VariableNode->GetName();
							Info.Detail = VariableName;
							OutRefs.Add(Info);
						}
					}
				}
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> ReferencesToJson(const TArray<FWidgetReferenceInfo>& References)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FWidgetReferenceInfo& Reference : References)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("kind"), Reference.Kind);
			Obj->SetStringField(TEXT("graph"), Reference.Graph);
			Obj->SetStringField(TEXT("node"), Reference.Node);
			Obj->SetStringField(TEXT("detail"), Reference.Detail);
			Items.Add(MakeShared<FJsonValueObject>(Obj));
		}
		return Items;
	}
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::SetWidgetPropertyBindingInternal(const TSharedPtr<FJsonObject>& Params,
                                                                                bool bAliasCommand)
{
	// Get required parameters
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing blueprint_name parameter"));
	}

	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing widget_name parameter"));
	}

	FString BindingName;
	if (!Params->TryGetStringField(TEXT("binding_name"), BindingName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing binding_name parameter"));
	}

	TSharedPtr<FJsonObject> Error;
	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByParam(BlueprintName, Error);
	if (!WidgetBlueprint)
	{
		return Error;
	}

	// The deprecated alias keeps its old default (Text); the general command wants the property named.
	FString PropertyName;
	Params->TryGetStringField(TEXT("property_name"), PropertyName);
	if (PropertyName.IsEmpty())
	{
		if (!bAliasCommand)
		{
			return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'property_name' parameter"));
		}
		PropertyName = TEXT("Text");
	}

	// Resolve the target widget FIRST - a wrong widget name must not leave a dangling variable behind
	UWidget* Widget = FindWidgetOrError(WidgetBlueprint, WidgetName, BlueprintName, Error);
	if (!Widget)
	{
		return Error;
	}

	// A binding is only applied at runtime when the property has a <Property>Delegate companion
	// (WidgetBlueprintGeneratedClass.cpp:157) and the target widget is a blueprint variable (:150).
	FBindablePropertyInfo Bindable;
	bool bDelegateFound = false;
	TArray<FString> Candidates;
	if (!DescribeBindableProperty(Widget, PropertyName, Bindable, bDelegateFound, Candidates))
	{
		TArray<TSharedPtr<FJsonValue>> BindableJson;
		for (const FString& Candidate : Candidates)
		{
			BindableJson.Add(MakeShared<FJsonValueString>(Candidate));
		}
		if (!bDelegateFound)
		{
			// No <Property>Delegate at all: the caller named something that cannot be bound.
			TSharedPtr<FJsonObject> Err = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unsupported_property"),
				FString::Printf(TEXT("Property '%s' is not bindable on %s (no %sDelegate)"), *PropertyName,
					*Widget->GetClass()->GetName(), *PropertyName));
			Err->SetArrayField(TEXT("bindable_properties"), BindableJson);
			return Err;
		}
		// The delegate exists but its return type is off the mapping table: do not silently downgrade
		// to Text, say so.
		FDelegateProperty* Delegate = FindFProperty<FDelegateProperty>(Widget->GetClass(),
			FName(*(PropertyName + TEXT("Delegate"))));
		const FProperty* ReturnProperty = (Delegate && Delegate->SignatureFunction)
			? Delegate->SignatureFunction->GetReturnProperty() : nullptr;
		TSharedPtr<FJsonObject> Err = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unsupported_property_type"),
			FString::Printf(TEXT("Property '%s' returns %s, which has no binding type mapping"), *PropertyName,
				ReturnProperty ? *ReturnProperty->GetCPPType() : TEXT("nothing")));
		Err->SetArrayField(TEXT("bindable_properties"), BindableJson);
		return Err;
	}
	if (!Widget->bIsVariable)
	{
		Widget->bIsVariable = true;
	}

	// Create the backing variable with the type the delegate returns - the binding is worthless if the
	// variable cannot hold that value.
	FEdGraphPinType PinType;
	FString TypeError;
	TArray<FString> SupportedTypes;
	if (!FUnrealMCPBlueprintGraphOps::BuildVariablePinType(Bindable.Type, Bindable.SubClass, PinType,
	                                                      TypeError, SupportedTypes))
	{
		TSharedPtr<FJsonObject> Err = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("unsupported_property_type"),
			FString::Printf(TEXT("Cannot build a '%s' variable for binding '%s.%s': %s"), *Bindable.Type,
				*WidgetName, *PropertyName, *TypeError));
		TArray<TSharedPtr<FJsonValue>> SupportedJson;
		for (const FString& Supported : SupportedTypes)
		{
			SupportedJson.Add(MakeShared<FJsonValueString>(Supported));
		}
		Err->SetArrayField(TEXT("supported_types"), SupportedJson);
		return Err;
	}
	// The variable may already be there (re-binding, or a binding whose getter was re-pointed). Adding it
	// twice would leave two entries with the same name, so reuse it and check the type instead.
	const FName BindingVariableName(*BindingName);
	bool bVariableExists = false;
	for (const FBPVariableDescription& Variable : WidgetBlueprint->NewVariables)
	{
		if (Variable.VarName != BindingVariableName)
		{
			continue;
		}
		bVariableExists = true;
		if (Variable.VarType != PinType)
		{
			const FString ExistingSubClass = Variable.VarType.PinSubCategoryObject.IsValid()
				? Variable.VarType.PinSubCategoryObject->GetName() : FString(TEXT("-"));
			return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("variable_type_mismatch"),
				FString::Printf(TEXT("Variable '%s' already exists as %s/%s, but binding '%s.%s' needs %s; "
					"unbind the existing binding first or use another binding_name"),
					*BindingName, *Variable.VarType.PinCategory.ToString(), *ExistingSubClass,
					*WidgetName, *PropertyName, *Bindable.TypeLabel));
		}
		break;
	}
	if (!bVariableExists)
	{
		FBlueprintEditorUtils::AddMemberVariable(WidgetBlueprint, BindingVariableName, PinType);
	}

	// Create binding function
	const FString FunctionName = FString::Printf(TEXT("Get%s"), *BindingName);
	UEdGraph* FuncGraph = nullptr;
	FString GraphErrorCode;
	FString GraphErrorMessage;
	if (!FUnrealMCPBlueprintGraphOps::AddFunctionGraph(WidgetBlueprint, FunctionName, FuncGraph,
	                                                  GraphErrorCode, GraphErrorMessage))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("binding_function_failed"), FString::Printf(
			TEXT("Failed to create the binding function '%s': %s"), *FunctionName, *GraphErrorMessage));
	}

	// The getter must be a real, compiling function graph. Build it through the graph ops (entry node comes
	// from the engine's AddFunctionGraph) - the previous implementation hand-made its own entry node on top
	// of that one, leaving the graph with two entry nodes (and nodes without GUIDs), so the widget never
	// compiled: "the graph needs exactly one function entry node, found two".
	FUnrealMCPBlueprintGraphOps::FFunctionParamRequest ResultParam;
	ResultParam.Name = TEXT("ReturnValue");
	ResultParam.Type = Bindable.Type;
	ResultParam.SubClass = Bindable.SubClass;
	ResultParam.bIsOutput = true;

	// A re-bind (or a binding re-pointed at an existing getter) reuses the function graph, so the return
	// value is only added when it is missing - and its type has to match what this binding needs.
	bool bHasReturnPin = false;
	UEdGraphPin* ExistingReturnPin = nullptr;
	for (UEdGraphNode* Node : FuncGraph->Nodes)
	{
		UK2Node_FunctionResult* ExistingResult = Cast<UK2Node_FunctionResult>(Node);
		if (!ExistingResult)
		{
			continue;
		}
		for (UEdGraphPin* Pin : ExistingResult->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input && Pin->PinName == FName(*ResultParam.Name))
			{
				bHasReturnPin = true;
				ExistingReturnPin = Pin;
				if (Pin->PinType != PinType)
				{
					return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("variable_type_mismatch"),
						FString::Printf(TEXT("Getter '%s' already returns %s, but binding '%s.%s' needs %s; "
							"unbind the existing binding first or use another binding_name"),
							*FunctionName, *Pin->PinType.PinCategory.ToString(), *WidgetName, *PropertyName,
							*Bindable.TypeLabel));
				}
				break;
			}
		}
		break;
	}
	if (!bHasReturnPin && !FUnrealMCPBlueprintGraphOps::AddFunctionParam(WidgetBlueprint, FuncGraph, ResultParam,
	                                                                     GraphErrorCode, GraphErrorMessage, Candidates))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("binding_function_failed"), FString::Printf(
			TEXT("Failed to add the return value to '%s': %s"), *FunctionName, *GraphErrorMessage));
	}

	// Wiring only when the getter is not wired yet: a re-bind must not pile a second variable node
	// next to the one the graph already reads.
	const bool bNeedsWiring = !ExistingReturnPin || ExistingReturnPin->LinkedTo.Num() == 0;
	UEdGraphNode* GetterNode = nullptr;
	if (bNeedsWiring && !FUnrealMCPBlueprintGraphOps::CreateVariableNode(FuncGraph, BindingName, /*bSet=*/false,
	                                                                    FVector2D(260.0, 0.0), GetterNode,
	                                                                    GraphErrorCode, GraphErrorMessage, Candidates))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("binding_function_failed"), FString::Printf(
			TEXT("Failed to read the binding variable '%s': %s"), *BindingName, *GraphErrorMessage));
	}

	UK2Node_FunctionEntry* EntryNode = FUnrealMCPBlueprintGraphOps::FindFunctionEntryNode(FuncGraph);
	UK2Node_FunctionResult* ResultNode = nullptr;
	for (UEdGraphNode* Node : FuncGraph->Nodes)
	{
		ResultNode = Cast<UK2Node_FunctionResult>(Node);
		if (ResultNode)
		{
			break;
		}
	}

	if (!EntryNode || !ResultNode)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("binding_function_failed"), FString::Printf(
			TEXT("Binding function '%s' is missing its entry or result node"), *FunctionName));
	}

	// UMG only accepts a PURE function as a property binding (IsBindingValid, WidgetBlueprint.cpp:503
	// "needs to be bound to a pure function") - and it drops the binding SILENTLY otherwise, because that
	// check only runs on a full compile (WidgetBlueprintCompiler.cpp:966-976). Mark the entry node pure
	// and rebuild the pins before wiring: a pure function graph is data-only (no exec pins).
	EntryNode->AddExtraFlags(FUNC_BlueprintPure);
	EntryNode->ReconstructNode();
	ResultNode->ReconstructNode();
	if (bNeedsWiring)
	{
		FUnrealMCPBlueprintGraphOps::ConnectNodes(FuncGraph, GetterNode, BindingName, ResultNode,
		                                         ResultParam.Name, GraphErrorCode, GraphErrorMessage, Candidates);
	}

	// Register the real UMG property binding - without this entry the widget's property is never driven
	FDelegateEditorBinding NewBinding;
	NewBinding.ObjectName = WidgetName;
	NewBinding.PropertyName = FName(*PropertyName);
	NewBinding.FunctionName = FName(*FunctionName);
	NewBinding.Kind = EBindingKind::Function;
	WidgetBlueprint->Bindings.RemoveAll([&NewBinding](const FDelegateEditorBinding& Existing)
	{
		return Existing.ObjectName == NewBinding.ObjectName && Existing.PropertyName == NewBinding.PropertyName;
	});
	WidgetBlueprint->Bindings.Add(NewBinding);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

	// Compile ourselves and keep the log: the editor -> runtime copy only happens on a full compile, it
	// validates each entry and drops the failures silently (WidgetBlueprintCompiler.cpp:966-976), so a
	// "success" that never reached the runtime table would be a lie.
	TArray<FString> Messages;
	CompileWithMessages(WidgetBlueprint, Messages);

	TArray<TSharedPtr<FJsonObject>> RuntimeBindings;
	UWidgetBlueprintGeneratedClass* GeneratedClass = Cast<UWidgetBlueprintGeneratedClass>(WidgetBlueprint->GeneratedClass);
	const bool bEffective = RuntimeBindingMatches(WidgetBlueprint, WidgetName, PropertyName, &RuntimeBindings);
	if (!bEffective)
	{
		if (!GeneratedClass)
		{
			TSharedPtr<FJsonObject> Err = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("compile_failed"),
				TEXT("The widget blueprint has no generated class after compiling"));
			Err->SetArrayField(TEXT("messages"), [&Messages]()
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				for (const FString& Message : Messages)
				{
					Items.Add(MakeShared<FJsonValueString>(Message));
				}
				return Items;
			}());
			return Err;
		}

		FString Summary;
		for (const FString& Message : Messages)
		{
			if (Message.Contains(TEXT("Binding"), ESearchCase::IgnoreCase))
			{
				Summary = Message;
				break;
			}
		}
		if (Summary.IsEmpty() && Messages.Num() > 0)
		{
			Summary = Messages[0];
		}
		TSharedPtr<FJsonObject> Err = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("binding_not_effective"),
			FString::Printf(TEXT("Binding '%s.%s' did not reach the runtime binding table; the engine dropped it at compile time%s"),
				*WidgetName, *PropertyName, Summary.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(": %s"), *Summary)));
		Err->SetStringField(TEXT("widget_name"), WidgetName);
		Err->SetStringField(TEXT("property_name"), PropertyName);
		Err->SetStringField(TEXT("function_name"), FunctionName);
		Err->SetStringField(TEXT("message"), Summary);
		Err->SetArrayField(TEXT("bindings"), BindingsToJsonArray(WidgetBlueprint, true));
		Err->SetNumberField(TEXT("runtime_binding_count"), RuntimeBindings.Num());
		Err->SetBoolField(TEXT("compiled"),
			WidgetBlueprint->Status == BS_UpToDate || WidgetBlueprint->Status == BS_UpToDateWithWarnings);
		return Err;
	}

	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("success"), true);
	Response->SetStringField(TEXT("binding_name"), BindingName);
	Response->SetStringField(TEXT("widget_name"), WidgetName);
	Response->SetStringField(TEXT("property_name"), PropertyName);
	Response->SetStringField(TEXT("function_name"), FunctionName);
	Response->SetStringField(TEXT("variable_type"), Bindable.TypeLabel);
	Response->SetBoolField(TEXT("effective"), true);
	Response->SetNumberField(TEXT("binding_count"), WidgetBlueprint->Bindings.Num());
	Response->SetNumberField(TEXT("runtime_binding_count"), RuntimeBindings.Num());
	if (bAliasCommand)
	{
		Response->SetStringField(TEXT("deprecated_command"), TEXT("set_text_block_binding"));
		Response->SetStringField(TEXT("use_instead"), TEXT("set_widget_property_binding"));
	}
	FinishWidgetWrite(WidgetBlueprint, Params, Response);
	return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleSetWidgetPropertyBinding(const TSharedPtr<FJsonObject>& Params)
{
	return SetWidgetPropertyBindingInternal(Params, /*bAliasCommand=*/false);
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleSetTextBlockBinding(const TSharedPtr<FJsonObject>& Params)
{
	return SetWidgetPropertyBindingInternal(Params, /*bAliasCommand=*/true);
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleBindWidgetEvent(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();

	// Get required parameters
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing blueprint_name parameter"));
	}

	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing widget_name parameter"));
	}

	FString EventName;
	if (!Params->TryGetStringField(TEXT("event_name"), EventName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing event_name parameter"));
	}

	// Load the Widget Blueprint (in-memory lookup, supports not-yet-saved assets)
	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(FUnrealMCPCommonUtils::FindAsset(BlueprintName));
	if (!WidgetBlueprint)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("widget_blueprint_not_found"), FString::Printf(TEXT("Failed to load Widget Blueprint: %s"), *BlueprintName));
	}

	// Create the event graph if it doesn't exist
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(WidgetBlueprint);
	if (!EventGraph)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("event_graph_unavailable"), TEXT("Failed to find or create event graph"));
	}

	// Find the widget in the blueprint
	UWidget* Widget = WidgetBlueprint->WidgetTree->FindWidget(*WidgetName);
	if (!Widget)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("widget_not_found"), FString::Printf(TEXT("Failed to find widget: %s"), *WidgetName));
	}

	// Create the event node (e.g., OnClicked for buttons) as a UK2Node_ComponentBoundEvent,
	// the same node type the UMG details panel creates when binding widget events.
	UK2Node_ComponentBoundEvent* EventNode = nullptr;

	// The widget must be exposed as a variable so a matching FObjectProperty exists
	// in the skeleton class (required by CreateNewBoundEventForClass).
	FObjectProperty* VariableProperty = FindFProperty<FObjectProperty>(WidgetBlueprint->SkeletonGeneratedClass, *WidgetName);
	if (!VariableProperty)
	{
		// Force the flag on and structurally recompile so the skeleton class
		// exposes the widget as a variable, then re-resolve the widget from the tree
		// (it may have been re-instanced by the compile).
		Widget->Modify();
		Widget->bIsVariable = true;
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
		Widget = WidgetBlueprint->WidgetTree->FindWidget(*WidgetName);
		if (Widget)
		{
			VariableProperty = FindFProperty<FObjectProperty>(WidgetBlueprint->SkeletonGeneratedClass, *WidgetName);
		}
	}
	if (!VariableProperty || !Widget)
	{
		FString SkelProps;
		if (WidgetBlueprint->SkeletonGeneratedClass)
		{
			for (TFieldIterator<FObjectProperty> It(WidgetBlueprint->SkeletonGeneratedClass); It; ++It)
			{
				SkelProps += It->GetName() + TEXT(", ");
			}
		}
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("widget_not_exposed"), FString::Printf(
			TEXT("Widget '%s' is not exposed as a variable after recompile (bIsVariable=%d, skeleton=%s, vars=[%s])"),
			*WidgetName, Widget ? (Widget->bIsVariable ? 1 : 0) : -1,
			WidgetBlueprint->SkeletonGeneratedClass ? *WidgetBlueprint->SkeletonGeneratedClass->GetName() : TEXT("null"),
			*SkelProps));
	}

	// The event must be a dynamic multicast delegate on the widget class
	FMulticastDelegateProperty* DelegateProperty = FindFProperty<FMulticastDelegateProperty>(Widget->GetClass(), FName(*EventName));
	if (!DelegateProperty)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("event_not_found"), FString::Printf(TEXT("Event '%s' not found on widget class '%s'"), *EventName, *Widget->GetClass()->GetName()));
	}

	// Reuse an existing bound event node if there is one
	EventNode = const_cast<UK2Node_ComponentBoundEvent*>(
		FKismetEditorUtilities::FindBoundEventForComponent(WidgetBlueprint, FName(*EventName), VariableProperty->GetFName()));

	if (!EventNode)
	{
		// Calculate position - place it below existing nodes
		float MaxHeight = 0.0f;
		for (UEdGraphNode* Node : EventGraph->Nodes)
		{
			MaxHeight = FMath::Max(MaxHeight, Node->NodePosY);
		}

		const FVector2D NodePos(200, MaxHeight + 200);

		// Creates the UK2Node_ComponentBoundEvent in the uber graph (same as the UMG details panel)
		FKismetEditorUtilities::CreateNewBoundEventForClass(
			Widget->GetClass(),
			FName(*EventName),
			WidgetBlueprint,
			VariableProperty
		);

		EventNode = const_cast<UK2Node_ComponentBoundEvent*>(
			FKismetEditorUtilities::FindBoundEventForComponent(WidgetBlueprint, FName(*EventName), VariableProperty->GetFName()));

		if (EventNode)
		{
			EventNode->NodePosX = NodePos.X;
			EventNode->NodePosY = NodePos.Y;
		}
	}

	if (!EventNode)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("create_failed"), TEXT("Failed to create event node"));
	}

	// Save the Widget Blueprint
	FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);

	Response->SetBoolField(TEXT("success"), true);
	Response->SetStringField(TEXT("event_name"), EventName);
	FinishWidgetWrite(WidgetBlueprint, Params, Response);
	return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleUnbindWidgetProperty(const TSharedPtr<FJsonObject>& Params)
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
	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'property_name' parameter"));
	}
	bool bRemoveFunction = false;
	Params->TryGetBoolField(TEXT("remove_function"), bRemoveFunction);
	bool bRemoveVariable = false;
	Params->TryGetBoolField(TEXT("remove_variable"), bRemoveVariable);

	TSharedPtr<FJsonObject> Error;
	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByParam(BlueprintName, Error);
	if (!WidgetBlueprint)
	{
		return Error;
	}

	int32 BindingIndex = INDEX_NONE;
	for (int32 Index = 0; Index < WidgetBlueprint->Bindings.Num(); ++Index)
	{
		const FDelegateEditorBinding& Binding = WidgetBlueprint->Bindings[Index];
		if (Binding.ObjectName == WidgetName && Binding.PropertyName.ToString() == PropertyName)
		{
			BindingIndex = Index;
			break;
		}
	}
	if (BindingIndex == INDEX_NONE)
	{
		TSharedPtr<FJsonObject> Err = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("binding_not_found"),
			FString::Printf(TEXT("'%s.%s' has no property binding"), *WidgetName, *PropertyName));
		Err->SetArrayField(TEXT("bindings"), BindingsToJsonArray(WidgetBlueprint, true));
		return Err;
	}

	const FString FunctionName = WidgetBlueprint->Bindings[BindingIndex].FunctionName.ToString();
	FString VariableName;
	if (FunctionName.StartsWith(TEXT("Get"), ESearchCase::CaseSensitive) && FunctionName.Len() > 3)
	{
		VariableName = FunctionName.RightChop(3);
	}
	const bool bVariableExists = !VariableName.IsEmpty()
		&& FBlueprintEditorUtils::FindMemberVariableGuidByName(WidgetBlueprint, FName(*VariableName)).IsValid();

	// What would be left dangling by the requested deletions? Checked before anything is removed.
	TArray<FWidgetReferenceInfo> References;
	GatherBindingArtifactReferences(WidgetBlueprint,
	                                bRemoveFunction ? FunctionName : FString(),
	                                (bRemoveVariable && bVariableExists) ? VariableName : FString(),
	                                BindingIndex, References);
	if (References.Num() > 0)
	{
		TSharedPtr<FJsonObject> Blocked = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("blocked_by_references"),
			FString::Printf(TEXT("'%s.%s' is still referenced by %d item(s); unbind without remove_function/remove_variable or clear those first"),
				*WidgetName, *PropertyName, References.Num()));
		Blocked->SetArrayField(TEXT("blockers"), ReferencesToJson(References));
		Blocked->SetStringField(TEXT("widget_name"), WidgetName);
		Blocked->SetStringField(TEXT("property_name"), PropertyName);
		Blocked->SetStringField(TEXT("function_name"), FunctionName);
		Blocked->SetStringField(TEXT("variable_name"), VariableName);
		return Blocked;
	}

	WidgetBlueprint->Bindings.RemoveAt(BindingIndex);

	bool bFunctionRemoved = false;
	if (bRemoveFunction && !FunctionName.IsEmpty())
	{
		for (int32 Index = WidgetBlueprint->FunctionGraphs.Num() - 1; Index >= 0; --Index)
		{
			UEdGraph* Graph = WidgetBlueprint->FunctionGraphs[Index];
			if (Graph && Graph->GetName() == FunctionName)
			{
				FBlueprintEditorUtils::RemoveGraph(WidgetBlueprint, Graph, EGraphRemoveFlags::Default);
				bFunctionRemoved = true;
				break;
			}
		}
	}
	bool bVariableRemoved = false;
	if (bRemoveVariable && bVariableExists)
	{
		const FName VariableFName(*VariableName);
		FBlueprintEditorUtils::RemoveVariableNodes(WidgetBlueprint, VariableFName);
		FBlueprintEditorUtils::RemoveMemberVariable(WidgetBlueprint, VariableFName);
		bVariableRemoved = true;
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

	TArray<FString> Messages;
	CompileWithMessages(WidgetBlueprint, Messages);

	TArray<TSharedPtr<FJsonObject>> RuntimeBindings;
	RuntimeBindingMatches(WidgetBlueprint, FString(), FString(), &RuntimeBindings);

	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("success"), true);
	Response->SetStringField(TEXT("blueprint_name"), BlueprintName);
	Response->SetStringField(TEXT("widget_name"), WidgetName);
	Response->SetStringField(TEXT("property_name"), PropertyName);
	Response->SetStringField(TEXT("function_name"), FunctionName);
	Response->SetStringField(TEXT("variable_name"), VariableName);
	Response->SetBoolField(TEXT("function_removed"), bFunctionRemoved);
	Response->SetBoolField(TEXT("variable_removed"), bVariableRemoved);
	Response->SetNumberField(TEXT("binding_count"), WidgetBlueprint->Bindings.Num());
	Response->SetNumberField(TEXT("runtime_binding_count"), RuntimeBindings.Num());
	Response->SetArrayField(TEXT("bindings"), BindingsToJsonArray(WidgetBlueprint, true));
	Response->SetBoolField(TEXT("compiled"),
		WidgetBlueprint->Status == BS_UpToDate || WidgetBlueprint->Status == BS_UpToDateWithWarnings);
	Response->SetArrayField(TEXT("messages"), [&Messages]()
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FString& Message : Messages)
		{
			Items.Add(MakeShared<FJsonValueString>(Message));
		}
			return Items;
	}());
	FinishWidgetWrite(WidgetBlueprint, Params, Response);
	return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandlePruneWidgetBindings(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("missing_parameter"), TEXT("Missing 'blueprint_name' parameter"));
	}
	bool bDryRun = true;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

	TSharedPtr<FJsonObject> Error;
	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByParam(BlueprintName, Error);
	if (!WidgetBlueprint)
	{
		return Error;
	}

	/** One leftover. "name" is what the caller sees; widget/property only exist for stale bindings. */
	struct FPruneItem
	{
		FString Kind;
		FString Name;
		FString WidgetName;
		FString PropertyName;
		FString Detail;
	};

	TArray<FPruneItem> Plan;
	TArray<FString> WidgetNames;
	GatherWidgetNames(WidgetBlueprint->WidgetTree, WidgetNames);

	// 1. Bindings whose widget is no longer in the tree.
	for (const FDelegateEditorBinding& Binding : WidgetBlueprint->Bindings)
	{
		if (WidgetNames.Contains(Binding.ObjectName))
		{
			continue;
		}
		FPruneItem Item;
		Item.Kind = TEXT("stale_binding");
		Item.Name = FString::Printf(TEXT("%s.%s"), *Binding.ObjectName, *Binding.PropertyName.ToString());
		Item.WidgetName = Binding.ObjectName;
		Item.PropertyName = Binding.PropertyName.ToString();
		Item.Detail = Binding.FunctionName.ToString();
		Plan.Add(Item);
	}

	// 2 + 3. Get<X> graphs nobody keeps alive: the function is an orphan and its backing variable is
	// unused. Variables that are not part of this pattern (a user's own variable, a widget variable) are
	// deliberately left alone - this command cleans up binding leftovers, not arbitrary variables.
	for (UEdGraph* Graph : WidgetBlueprint->FunctionGraphs)
	{
		if (!Graph)
		{
			continue;
		}
		const FString FunctionName = Graph->GetName();
		if (!FunctionName.StartsWith(TEXT("Get"), ESearchCase::CaseSensitive) || FunctionName.Len() <= 3)
		{
			continue;
		}
		const FString VariableName = FunctionName.RightChop(3);

		TArray<FWidgetReferenceInfo> References;
		GatherBindingArtifactReferences(WidgetBlueprint, FunctionName, VariableName, INDEX_NONE, References);
		if (References.Num() > 0)
		{
			continue;
		}

		FPruneItem FunctionItem;
		FunctionItem.Kind = TEXT("orphan_function");
		FunctionItem.Name = FunctionName;
		FunctionItem.Detail = FString::Printf(TEXT("nothing calls it and no binding uses it"));
		Plan.Add(FunctionItem);

		if (FBlueprintEditorUtils::FindMemberVariableGuidByName(WidgetBlueprint, FName(*VariableName)).IsValid())
		{
			FPruneItem VariableItem;
			VariableItem.Kind = TEXT("unused_variable");
			VariableItem.Name = VariableName;
			VariableItem.Detail = FString::Printf(TEXT("only %s used it"), *FunctionName);
			Plan.Add(VariableItem);
		}
	}

	TArray<TSharedPtr<FJsonValue>> ItemsJson;
	for (const FPruneItem& Item : Plan)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("kind"), Item.Kind);
		Obj->SetStringField(TEXT("name"), Item.Name);
		if (!Item.WidgetName.IsEmpty())
		{
			Obj->SetStringField(TEXT("widget_name"), Item.WidgetName);
			Obj->SetStringField(TEXT("property_name"), Item.PropertyName);
		}
		Obj->SetStringField(TEXT("detail"), Item.Detail);
		ItemsJson.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TArray<TSharedPtr<FJsonValue>> RemovedJson;
	if (!bDryRun && Plan.Num() > 0)
	{
		// Apply in the safe order: bindings first (they are what keeps the getters alive), then the
		// functions, then the variables.
		for (const FPruneItem& Item : Plan)
		{
			if (Item.Kind != TEXT("stale_binding"))
			{
				continue;
			}
			for (int32 Index = WidgetBlueprint->Bindings.Num() - 1; Index >= 0; --Index)
			{
				const FDelegateEditorBinding& Binding = WidgetBlueprint->Bindings[Index];
				if (Binding.ObjectName == Item.WidgetName && Binding.PropertyName.ToString() == Item.PropertyName)
				{
					WidgetBlueprint->Bindings.RemoveAt(Index);
					RemovedJson.Add(MakeShared<FJsonValueString>(Item.Name));
					break;
				}
			}
		}
		for (const FPruneItem& Item : Plan)
		{
			if (Item.Kind != TEXT("orphan_function"))
			{
				continue;
			}
			for (int32 Index = WidgetBlueprint->FunctionGraphs.Num() - 1; Index >= 0; --Index)
			{
				UEdGraph* Graph = WidgetBlueprint->FunctionGraphs[Index];
				if (Graph && Graph->GetName() == Item.Name)
				{
					FBlueprintEditorUtils::RemoveGraph(WidgetBlueprint, Graph, EGraphRemoveFlags::Default);
					RemovedJson.Add(MakeShared<FJsonValueString>(Item.Name));
					break;
				}
			}
		}
		for (const FPruneItem& Item : Plan)
		{
			if (Item.Kind != TEXT("unused_variable"))
			{
				continue;
			}
			const FName VariableName(*Item.Name);
			if (FBlueprintEditorUtils::FindMemberVariableGuidByName(WidgetBlueprint, VariableName).IsValid())
			{
				FBlueprintEditorUtils::RemoveVariableNodes(WidgetBlueprint, VariableName);
				FBlueprintEditorUtils::RemoveMemberVariable(WidgetBlueprint, VariableName);
				RemovedJson.Add(MakeShared<FJsonValueString>(Item.Name));
			}
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
	}

	TArray<FString> Messages;
	CompileWithMessages(WidgetBlueprint, Messages);

	TArray<TSharedPtr<FJsonObject>> RuntimeBindings;
	RuntimeBindings.Reset();
	RuntimeBindingMatches(WidgetBlueprint, FString(), FString(), &RuntimeBindings);

	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("success"), true);
	Response->SetStringField(TEXT("blueprint_name"), BlueprintName);
	Response->SetBoolField(TEXT("dry_run"), bDryRun);
	Response->SetArrayField(TEXT("items"), ItemsJson);
	Response->SetNumberField(TEXT("item_count"), ItemsJson.Num());
	Response->SetArrayField(TEXT("removed"), RemovedJson);
	Response->SetNumberField(TEXT("removed_count"), RemovedJson.Num());
	Response->SetNumberField(TEXT("binding_count"), WidgetBlueprint->Bindings.Num());
	Response->SetNumberField(TEXT("runtime_binding_count"), RuntimeBindings.Num());
	Response->SetBoolField(TEXT("compiled"),
		WidgetBlueprint->Status == BS_UpToDate || WidgetBlueprint->Status == BS_UpToDateWithWarnings);
	if (!bDryRun)
	{
		FinishWidgetWrite(WidgetBlueprint, Params, Response);
	}
	return Response;
} 