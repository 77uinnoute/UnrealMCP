#include "Commands/Material/UnrealMCPMaterialOps.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionOutput.h"

namespace
{
    // ========================================================================
    // 处理材质/材质函数节点：遍历 UMaterialExpression 数组（材质表达式图）
    // 节点没有 NodeGuid，用名称+类型标识；pin 来自 FExpressionInput/FExpressionOutput
    // 额外构造虚拟"输出节点"：Material Output（材质属性 pin）或 Function Output
    // 适用对象：UMaterial、UMaterialFunction
    // ========================================================================
    TSharedPtr<FJsonObject> SerializeMaterialNodes(UObject* Asset, TArrayView<const TObjectPtr<UMaterialExpression>> Expressions, const TSharedPtr<FJsonObject>& Params)
    {
        int32 MaxNodes = 200;
        if (Params->HasField(TEXT("max_nodes")))
        {
            MaxNodes = FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("max_nodes"))), 1, 1000);
        }

        TArray<TSharedPtr<FJsonValue>> NodesArray;
        int32 NodeCount = 0;
        TSet<UMaterialExpression*> SeenExpressions; // Deduplicate by pointer

        for (UMaterialExpression* Expression : Expressions)
        {
            if (!Expression) continue;
            if (NodeCount >= MaxNodes) break;

            if (SeenExpressions.Contains(Expression)) continue;
            SeenExpressions.Add(Expression);
            TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
            // Display name: ParameterName (for parameter nodes), then Desc, then GetCaption, fallback to class name
            FString DisplayName;
            if (Expression->HasAParameterName())
            {
                FName ParamName = Expression->GetParameterName();
                if (!ParamName.IsNone())
                {
                    DisplayName = ParamName.ToString();
                }
            }
            if (DisplayName.IsEmpty() && !Expression->Desc.IsEmpty())
            {
                DisplayName = Expression->Desc;
            }
            if (DisplayName.IsEmpty())
            {
                TArray<FString> Captions;
                Expression->GetCaption(Captions);
                DisplayName = Captions.Num() > 0 && !Captions[0].IsEmpty() ? Captions[0] : Expression->GetClass()->GetName();
            }
            NodeObj->SetStringField(TEXT("name"), DisplayName);
            NodeObj->SetStringField(TEXT("type"), Expression->GetClass()->GetName());
            NodeObj->SetNumberField(TEXT("index"), NodeCount);

            // Reflect all UPROPERTY values (exclude internal/editor-only bookkeeping)
            TSharedPtr<FJsonObject> PropsObj = MakeShared<FJsonObject>();
            UClass* ExprClass = Expression->GetClass();
            for (TFieldIterator<FProperty> It(ExprClass, EFieldIterationFlags::IncludeSuper); It; ++It)
            {
                FProperty* Prop = *It;
                if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;

                FString PropName = Prop->GetName();
                // Skip noise fields
                if (PropName == TEXT("Desc") ||
                    PropName == TEXT("MaterialExpressionGuid") ||
                    PropName == TEXT("MaterialExpressionEditorX") ||
                    PropName == TEXT("MaterialExpressionEditorY") ||
                    PropName == TEXT("GraphNode") ||
                    PropName == TEXT("Group") ||
                    PropName == TEXT("SortPriority") ||
                    PropName == TEXT("ExpressionGUID") ||
                    PropName == TEXT("Outputs") ||
                    PropName == TEXT("bRealtimePreview") ||
                    PropName == TEXT("bNeedToUpdatePreview") ||
                    PropName == TEXT("bCommentBubbleVisible") ||
                    PropName == TEXT("bCommentBubblePinned") ||
                    PropName == TEXT("bShowOutputNameOnPin") ||
                    PropName == TEXT("bShowMaskOnPin") ||
                    PropName == TEXT("bShowTextureInputPin") ||
                    PropName.StartsWith(TEXT("bCollapsed")))
                    continue;

                // Skip Const* override values when the corresponding FExpressionInput pin is connected
                // E.g. ConstA is meaningless if pin A has a wired connection
                if (PropName.StartsWith(TEXT("Const")))
                {
                    FString InputPropName = PropName.RightChop(5); // "ConstA" → "A", "ConstB" → "B"
                    FProperty* InputProp = ExprClass->FindPropertyByName(*InputPropName);
                    if (InputProp)
                    {
                        FStructProperty* StructProp = CastField<FStructProperty>(InputProp);
                        if (StructProp)
                        {
                            FExpressionInput* Input = StructProp->ContainerPtrToValuePtr<FExpressionInput>(Expression);
                            if (Input && Input->Expression != nullptr)
                            {
                                // Pin is connected, skip the Const fallback value
                                continue;
                            }
                        }
                    }
                }

                FString ValueStr;
                if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
                {
                    PropsObj->SetNumberField(PropName, FloatProp->GetPropertyValue_InContainer(Expression));
                }
                else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
                {
                    PropsObj->SetNumberField(PropName, DoubleProp->GetPropertyValue_InContainer(Expression));
                }
                else if (FIntProperty* IntProp = CastField<FIntProperty>(Prop))
                {
                    PropsObj->SetNumberField(PropName, IntProp->GetPropertyValue_InContainer(Expression));
                }
                else if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
                {
                    PropsObj->SetNumberField(PropName, ByteProp->GetPropertyValue_InContainer(Expression));
                }
                else if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
                {
                    PropsObj->SetBoolField(PropName, BoolProp->GetPropertyValue_InContainer(Expression));
                }
                else if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
                {
                    ValueStr = StrProp->GetPropertyValue_InContainer(Expression);
                    if (!ValueStr.IsEmpty())
                    {
                        PropsObj->SetStringField(PropName, ValueStr);
                    }
                }
                else if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
                {
                    FName Val = NameProp->GetPropertyValue_InContainer(Expression);
                    if (!Val.IsNone())
                    {
                        PropsObj->SetStringField(PropName, Val.ToString());
                    }
                }
                else if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
                {
                    UObject* Obj = ObjProp->GetObjectPropertyValue_InContainer(Expression);
                    if (Obj)
                    {
                        PropsObj->SetStringField(PropName, Obj->GetPathName());
                    }
                }
                else if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
                {
                    FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
                    int64 Val = UnderlyingProp->GetSignedIntPropertyValue_InContainer(Expression);
                    PropsObj->SetNumberField(PropName, Val);
                }
                else if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
                {
                    if (StructProp->Struct == TBaseStructure<FVector>::Get())
                    {
                        FVector* Vec = StructProp->ContainerPtrToValuePtr<FVector>(Expression);
                        TArray<TSharedPtr<FJsonValue>> Arr;
                        Arr.Add(MakeShared<FJsonValueNumber>(Vec->X));
                        Arr.Add(MakeShared<FJsonValueNumber>(Vec->Y));
                        Arr.Add(MakeShared<FJsonValueNumber>(Vec->Z));
                        PropsObj->SetArrayField(PropName, Arr);
                    }
                    else if (StructProp->Struct == TBaseStructure<FLinearColor>::Get())
                    {
                        FLinearColor* Color = StructProp->ContainerPtrToValuePtr<FLinearColor>(Expression);
                        TArray<TSharedPtr<FJsonValue>> Arr;
                        Arr.Add(MakeShared<FJsonValueNumber>(Color->R));
                        Arr.Add(MakeShared<FJsonValueNumber>(Color->G));
                        Arr.Add(MakeShared<FJsonValueNumber>(Color->B));
                        Arr.Add(MakeShared<FJsonValueNumber>(Color->A));
                        PropsObj->SetArrayField(PropName, Arr);
                    }
                }
            }
            if (PropsObj->Values.Num() > 0)
            {
                NodeObj->SetObjectField(TEXT("properties"), PropsObj);
            }

            TArray<TSharedPtr<FJsonValue>> PinNamesArray;
            TArray<TSharedPtr<FJsonValue>> ConnectionsArray;

            // Iterate inputs: collect pin names and connections in one pass
            for (FExpressionInputIterator InputIt(Expression); InputIt; ++InputIt)
            {
                FName InputName = Expression->GetInputName(InputIt.Index);
                FString PinName = InputName.IsNone()
                    ? FString::Printf(TEXT("Input_%d"), InputIt.Index)
                    : InputName.ToString();
                PinNamesArray.Add(MakeShared<FJsonValueString>(PinName));

                if (InputIt.Input && InputIt.Input->Expression)
                {
                    UMaterialExpression* Src = InputIt.Input->Expression;
                    // Get display name for source node
                    FString SrcName;
                    if (Src->HasAParameterName())
                        SrcName = Src->GetParameterName().ToString();
                    else if (!Src->Desc.IsEmpty())
                        SrcName = Src->Desc;
                    else
                    {
                        TArray<FString> Cap;
                        Src->GetCaption(Cap);
                        SrcName = Cap.Num() > 0 ? Cap[0] : Src->GetClass()->GetName();
                    }

                    TSharedPtr<FJsonObject> ConnObj = MakeShared<FJsonObject>();
                    ConnObj->SetStringField(TEXT("from"), PinName);
                    ConnObj->SetStringField(TEXT("to"), SrcName);
                    ConnectionsArray.Add(MakeShared<FJsonValueObject>(ConnObj));
                }
            }

            // Iterate outputs
            TArray<FExpressionOutput>& Outputs = Expression->GetOutputs();
            for (int32 i = 0; i < Outputs.Num(); ++i)
            {
                FString PinName = Outputs[i].OutputName.IsNone()
                    ? FString::Printf(TEXT("Output_%d"), i)
                    : Outputs[i].OutputName.ToString();
                PinNamesArray.Add(MakeShared<FJsonValueString>(PinName));
            }

            NodeObj->SetArrayField(TEXT("pins"), PinNamesArray);
            if (ConnectionsArray.Num() > 0)
            {
                NodeObj->SetArrayField(TEXT("connections"), ConnectionsArray);
            }

            NodesArray.Add(MakeShared<FJsonValueObject>(NodeObj));
            NodeCount++;
        }

        // --- Add output node (Material Result / Function Outputs) ---
        // Material output is not a UMaterialExpression; it's a virtual node defined by the
        // material's property slots (BaseColor, Metallic, etc.) and their connected expressions.
        {
            TSharedPtr<FJsonObject> OutputNodeObj = MakeShared<FJsonObject>();
            TArray<TSharedPtr<FJsonValue>> OutPinArray;
            TArray<TSharedPtr<FJsonValue>> OutConnArray;

            if (UMaterial* Mat = Cast<UMaterial>(Asset))
            {
                OutputNodeObj->SetStringField(TEXT("name"), TEXT("Material Output"));
                OutputNodeObj->SetStringField(TEXT("type"), TEXT("MaterialOutput"));

                // Collect all visible material property inputs
                static const TArray<TPair<EMaterialProperty, const TCHAR*>> MaterialProperties = {
                    { MP_BaseColor,           TEXT("Base Color") },
                    { MP_Metallic,            TEXT("Metallic") },
                    { MP_Specular,            TEXT("Specular") },
                    { MP_Roughness,           TEXT("Roughness") },
                    { MP_Anisotropy,          TEXT("Anisotropy") },
                    { MP_EmissiveColor,       TEXT("Emissive Color") },
                    { MP_Opacity,             TEXT("Opacity") },
                    { MP_OpacityMask,         TEXT("Opacity Mask") },
                    { MP_Normal,              TEXT("Normal") },
                    { MP_Tangent,             TEXT("Tangent") },
                    { MP_WorldPositionOffset, TEXT("World Position Offset") },
                    { MP_SubsurfaceColor,     TEXT("Subsurface Color") },
                    { MP_AmbientOcclusion,    TEXT("Ambient Occlusion") },
                    { MP_Refraction,          TEXT("Refraction") },
                    { MP_PixelDepthOffset,    TEXT("Pixel Depth Offset") },
                    { MP_ShadingModel,        TEXT("Shading Model") },
                    { MP_FrontMaterial,       TEXT("Front Material") },
                    { MP_SurfaceThickness,    TEXT("Surface Thickness") },
                    { MP_Displacement,        TEXT("Displacement") },
                };

                for (const auto& Prop : MaterialProperties)
                {
                    EMaterialProperty MP = Prop.Key;
                    const TCHAR* DisplayName = Prop.Value;

                    FExpressionInput* Input = Mat->GetExpressionInputForProperty(MP);
                    if (Input)
                    {
                        OutPinArray.Add(MakeShared<FJsonValueString>(DisplayName));

                        if (Input->Expression)
                        {
                            // Get connected expression's display name
                            FString SrcName;
                            if (Input->Expression->HasAParameterName())
                                SrcName = Input->Expression->GetParameterName().ToString();
                            else if (!Input->Expression->Desc.IsEmpty())
                                SrcName = Input->Expression->Desc;
                            else
                            {
                                TArray<FString> Cap;
                                Input->Expression->GetCaption(Cap);
                                SrcName = Cap.Num() > 0 ? Cap[0] : Input->Expression->GetClass()->GetName();
                            }

                            TSharedPtr<FJsonObject> ConnObj = MakeShared<FJsonObject>();
                            ConnObj->SetStringField(TEXT("from"), DisplayName);
                            ConnObj->SetStringField(TEXT("to"), SrcName);
                            OutConnArray.Add(MakeShared<FJsonValueObject>(ConnObj));
                        }
                    }
                }
            }
            else if (UMaterialFunction* MF = Cast<UMaterialFunction>(Asset))
            {
                OutputNodeObj->SetStringField(TEXT("name"), TEXT("Function Output"));
                OutputNodeObj->SetStringField(TEXT("type"), TEXT("MaterialFunctionOutput"));

                TArray<FFunctionExpressionInput> DummyInputs;
                TArray<FFunctionExpressionOutput> FOOutputs;
                MF->GetInputsAndOutputs(DummyInputs, FOOutputs);

                for (const FFunctionExpressionOutput& FOut : FOOutputs)
                {
                    FString PinName = FOut.Output.OutputName.IsNone()
                        ? FString::Printf(TEXT("Output_%d"), OutPinArray.Num())
                        : FOut.Output.OutputName.ToString();
                    OutPinArray.Add(MakeShared<FJsonValueString>(PinName));

                    // Track connected expressions to this output
                    // FunctionOutputs are UMaterialExpression nodes themselves, their input pins carry connections
                    if (FOut.ExpressionOutput)
                    {
                        for (FExpressionInputIterator InputIt(FOut.ExpressionOutput); InputIt; ++InputIt)
                        {
                            if (InputIt.Input && InputIt.Input->Expression)
                            {
                                UMaterialExpression* Src = InputIt.Input->Expression;
                                FString SrcName;
                                if (Src->HasAParameterName())
                                    SrcName = Src->GetParameterName().ToString();
                                else if (!Src->Desc.IsEmpty())
                                    SrcName = Src->Desc;
                                else
                                {
                                    TArray<FString> Cap;
                                    Src->GetCaption(Cap);
                                    SrcName = Cap.Num() > 0 ? Cap[0] : Src->GetClass()->GetName();
                                }

                                TSharedPtr<FJsonObject> ConnObj = MakeShared<FJsonObject>();
                                ConnObj->SetStringField(TEXT("from"), PinName);
                                ConnObj->SetStringField(TEXT("to"), SrcName);
                                OutConnArray.Add(MakeShared<FJsonValueObject>(ConnObj));
                            }
                        }
                    }
                }
            }

            if (OutPinArray.Num() > 0)
            {
                OutputNodeObj->SetArrayField(TEXT("pins"), OutPinArray);
                if (OutConnArray.Num() > 0)
                {
                    OutputNodeObj->SetArrayField(TEXT("connections"), OutConnArray);
                }
                // Prepend output node so it's always first
                NodesArray.Insert(MakeShared<FJsonValueObject>(OutputNodeObj), 0);
            }
        }

        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetArrayField(TEXT("nodes"), NodesArray);
        return ResultObj;
    }
}

bool FUnrealMCPMaterialOps::IsMaterialGraphAsset(const UObject* Asset)
{
    return Asset && (Asset->IsA<UMaterial>() || Asset->IsA<UMaterialFunction>());
}

bool FUnrealMCPMaterialOps::TrySerializeGraphNodes(UObject* Asset, const FString& NodeType,
                                                   const TSharedPtr<FJsonObject>& Params,
                                                   TSharedPtr<FJsonObject>& OutResult)
{
    OutResult.Reset();

    if (UMaterial* Material = Cast<UMaterial>(Asset))
    {
        if (NodeType != TEXT("All"))
        {
            OutResult = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Material assets only support node_type='All'"));
            return true;
        }
        OutResult = SerializeMaterialNodes(Asset, Material->GetExpressions(), Params);
        return true;
    }

    if (UMaterialFunction* MaterialFunction = Cast<UMaterialFunction>(Asset))
    {
        if (NodeType != TEXT("All"))
        {
            OutResult = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("MaterialFunction assets only support node_type='All'"));
            return true;
        }
        OutResult = SerializeMaterialNodes(Asset, MaterialFunction->GetExpressions(), Params);
        return true;
    }

    return false;
}
