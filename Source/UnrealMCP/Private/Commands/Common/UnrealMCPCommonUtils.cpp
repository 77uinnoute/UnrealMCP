#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Commands/Blueprint/UnrealMCPBlueprintGraphOps.h"
#include "Reflection/MCPPropertyReflector.h"
#include "GameFramework/Actor.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_InputAction.h"
#include "K2Node_Self.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/StaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "UObject/UObjectIterator.h"
#include "Engine/Selection.h"
#include "EditorAssetLibrary.h"
#include "FileHelpers.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintActionDatabase.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UnrealType.h"
#include "UObject/TopLevelAssetPath.h"
#include "UObject/Package.h"
#include "UObject/GCObjectScopeGuard.h"
#include "Misc/PackageName.h"
#include "PackageTools.h"
#include "Factories/Factory.h"
#include "Math/Vector4.h"
#include "Math/Color.h"

// JSON Utilities
TSharedPtr<FJsonObject> FUnrealMCPCommonUtils::CreateErrorResponse(const FString& Message)
{
    TSharedPtr<FJsonObject> ResponseObject = MakeShared<FJsonObject>();
    ResponseObject->SetBoolField(TEXT("success"), false);
    ResponseObject->SetStringField(TEXT("error"), Message);
    return ResponseObject;
}

TSharedPtr<FJsonObject> FUnrealMCPCommonUtils::CreateErrorResponse(const FString& ErrorCode, const FString& Message)
{
    TSharedPtr<FJsonObject> ResponseObject = CreateErrorResponse(Message);
    ResponseObject->SetStringField(TEXT("error_code"), ErrorCode);
    return ResponseObject;
}

TSharedPtr<FJsonObject> FUnrealMCPCommonUtils::CreateSuccessResponse(const TSharedPtr<FJsonObject>& Data)
{
    TSharedPtr<FJsonObject> ResponseObject = MakeShared<FJsonObject>();
    ResponseObject->SetBoolField(TEXT("success"), true);
    
    if (Data.IsValid())
    {
        ResponseObject->SetObjectField(TEXT("data"), Data);
    }
    
    return ResponseObject;
}

void FUnrealMCPCommonUtils::AddStringArrayField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName,
                                                const TArray<FString>& Values)
{
    if (!Object.IsValid() || Values.Num() == 0)
    {
        return;
    }
    TArray<TSharedPtr<FJsonValue>> Items;
    for (const FString& Value : Values)
    {
        Items.Add(MakeShared<FJsonValueString>(Value));
    }
    Object->SetArrayField(FieldName, Items);
}

void FUnrealMCPCommonUtils::GetIntArrayFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, TArray<int32>& OutArray)
{
    OutArray.Reset();
    
    if (!JsonObject->HasField(FieldName))
    {
        return;
    }
    
    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray))
    {
        for (const TSharedPtr<FJsonValue>& Value : *JsonArray)
        {
            OutArray.Add((int32)Value->AsNumber());
        }
    }
}

void FUnrealMCPCommonUtils::GetFloatArrayFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, TArray<float>& OutArray)
{
    OutArray.Reset();
    
    if (!JsonObject->HasField(FieldName))
    {
        return;
    }
    
    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray))
    {
        for (const TSharedPtr<FJsonValue>& Value : *JsonArray)
        {
            OutArray.Add((float)Value->AsNumber());
        }
    }
}

bool FUnrealMCPCommonUtils::ReadNumbersFromJson(const TSharedPtr<FJsonValue>& Value, const TArray<FString>& ObjectKeys,
                                                TArray<double>& OutNumbers, FString& OutErrorMessage)
{
    // One implementation: the reflector owns the struct compact-form readers.
    return FMCPPropertyReflector::ReadNumbersFromJson(Value, ObjectKeys, OutNumbers, OutErrorMessage);
}
FVector2D FUnrealMCPCommonUtils::GetVector2DFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
{
    FVector2D Result(0.0f, 0.0f);

    if (!JsonObject->HasField(FieldName))
    {
        return Result;
    }
    
    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray) && JsonArray->Num() >= 2)
    {
        Result.X = (float)(*JsonArray)[0]->AsNumber();
        Result.Y = (float)(*JsonArray)[1]->AsNumber();
    }
    
    return Result;
}

FVector FUnrealMCPCommonUtils::GetVectorFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
{
    FVector Result(0.0f, 0.0f, 0.0f);
    
    if (!JsonObject->HasField(FieldName))
    {
        return Result;
    }
    
    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray) && JsonArray->Num() >= 3)
    {
        Result.X = (float)(*JsonArray)[0]->AsNumber();
        Result.Y = (float)(*JsonArray)[1]->AsNumber();
        Result.Z = (float)(*JsonArray)[2]->AsNumber();
    }
    
    return Result;
}

FRotator FUnrealMCPCommonUtils::GetRotatorFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
{
    FRotator Result(0.0f, 0.0f, 0.0f);
    
    if (!JsonObject->HasField(FieldName))
    {
        return Result;
    }
    
    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray) && JsonArray->Num() >= 3)
    {
        Result.Pitch = (float)(*JsonArray)[0]->AsNumber();
        Result.Yaw = (float)(*JsonArray)[1]->AsNumber();
        Result.Roll = (float)(*JsonArray)[2]->AsNumber();
    }
    
    return Result;
}

// Blueprint Utilities
UBlueprint* FUnrealMCPCommonUtils::FindBlueprint(const FString& BlueprintName)
{
    // Special value: use currently opened blueprint
    if (BlueprintName == TEXT("__current__"))
    {
        return GetCurrentBlueprint();
    }
    
    return FindBlueprintByName(BlueprintName);
}

UBlueprint* FUnrealMCPCommonUtils::GetCurrentBlueprint()
{
    // Try to get the blueprint currently being edited
    if (GEditor)
    {
        UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
        if (AssetEditorSubsystem)
        {
            TArray<UObject*> EditedAssets = AssetEditorSubsystem->GetAllEditedAssets();
            for (UObject* Asset : EditedAssets)
            {
                if (UBlueprint* BP = Cast<UBlueprint>(Asset))
                {
                    UE_LOG(LogTemp, Display, TEXT("GetCurrentBlueprint: Using currently opened blueprint: %s"), *BP->GetName());
                    return BP;
                }
            }
        }
    }
    
    return nullptr;
}

UBlueprint* FUnrealMCPCommonUtils::FindBlueprintByName(const FString& BlueprintName)
{
    // Step 1: If BlueprintName looks like an asset path (starts with /), try to load it directly
    if (BlueprintName.StartsWith(TEXT("/")))
    {
        FString CleanPath = BlueprintName;
        if (CleanPath.EndsWith(TEXT("_C")))
        {
            CleanPath = CleanPath.Left(CleanPath.Len() - 2);
        }
        
        if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *CleanPath))
        {
            UE_LOG(LogTemp, Display, TEXT("FindBlueprintByName: Loaded blueprint by path: %s"), *CleanPath);
            return BP;
        }
        
        // Path-style name but not a Blueprint — don't fall through to name-based search
        return nullptr;
    }
    
    // Step 2: Search all packages using AssetRegistry by exact name match
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
    
    // Ensure asset registry is fully loaded
    AssetRegistry.SearchAllAssets(true);
    
    FARFilter Filter;
    Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
    Filter.bRecursivePaths = true;
    // Blueprint subclasses must match too: creating with parent_class="AnimInstance" makes the factory
    // produce a UAnimBlueprint, which an exact-class filter hides (that is why compile_blueprint used to
    // answer "Blueprint not found" for a blueprint it had just created).
    Filter.bRecursiveClasses = true;
    
    TArray<FAssetData> AssetDataList;
    AssetRegistry.GetAssets(Filter, AssetDataList);
    
    // First try exact name match
    for (const FAssetData& AssetData : AssetDataList)
    {
        if (AssetData.AssetName.ToString() == BlueprintName)
        {
            UE_LOG(LogTemp, Display, TEXT("FindBlueprintByName: Found blueprint by exact name match: %s at %s"), 
                *BlueprintName, *AssetData.GetObjectPathString());
            return Cast<UBlueprint>(AssetData.GetAsset());
        }
    }
    
    // Step 4: Try case-insensitive name match
    for (const FAssetData& AssetData : AssetDataList)
    {
        if (AssetData.AssetName.ToString().Equals(BlueprintName, ESearchCase::IgnoreCase))
        {
            UE_LOG(LogTemp, Display, TEXT("FindBlueprintByName: Found blueprint by case-insensitive name match: %s at %s"), 
                *BlueprintName, *AssetData.GetObjectPathString());
            return Cast<UBlueprint>(AssetData.GetAsset());
        }
    }
    
    // Step 5: Try partial match (name contains BlueprintName)
    for (const FAssetData& AssetData : AssetDataList)
    {
        if (AssetData.AssetName.ToString().Contains(BlueprintName))
        {
            UE_LOG(LogTemp, Display, TEXT("FindBlueprintByName: Found blueprint by partial name match: %s at %s"), 
                *BlueprintName, *AssetData.GetObjectPathString());
            return Cast<UBlueprint>(AssetData.GetAsset());
        }
    }
    
    return nullptr;
}

UMaterial* FUnrealMCPCommonUtils::FindMaterial(const FString& MaterialName)
{
    // Step 1: If looks like an asset path (starts with /), try to load directly
    if (MaterialName.StartsWith(TEXT("/")))
    {
        if (UMaterial* Mat = LoadObject<UMaterial>(nullptr, *MaterialName))
        {
            UE_LOG(LogTemp, Display, TEXT("FindMaterial: Loaded by path: %s"), *MaterialName);
            return Mat;
        }
    }
    
    // Step 2: Search using AssetRegistry
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
    AssetRegistry.SearchAllAssets(true);
    
    FARFilter Filter;
    Filter.ClassPaths.Add(UMaterial::StaticClass()->GetClassPathName());
    Filter.bRecursivePaths = true;
    
    TArray<FAssetData> AssetDataList;
    AssetRegistry.GetAssets(Filter, AssetDataList);
    
    // Exact name match
    for (const FAssetData& AssetData : AssetDataList)
    {
        if (AssetData.AssetName.ToString() == MaterialName)
        {
            UE_LOG(LogTemp, Display, TEXT("FindMaterial: Found by exact name: %s at %s"), 
                *MaterialName, *AssetData.GetObjectPathString());
            return Cast<UMaterial>(AssetData.GetAsset());
        }
    }
    
    // Case-insensitive
    for (const FAssetData& AssetData : AssetDataList)
    {
        if (AssetData.AssetName.ToString().Equals(MaterialName, ESearchCase::IgnoreCase))
        {
            UE_LOG(LogTemp, Display, TEXT("FindMaterial: Found by case-insensitive name: %s"), *MaterialName);
            return Cast<UMaterial>(AssetData.GetAsset());
        }
    }
    
    // Partial match
    for (const FAssetData& AssetData : AssetDataList)
    {
        if (AssetData.AssetName.ToString().Contains(MaterialName))
        {
            UE_LOG(LogTemp, Display, TEXT("FindMaterial: Found by partial name: %s at %s"), 
                *MaterialName, *AssetData.GetObjectPathString());
            return Cast<UMaterial>(AssetData.GetAsset());
        }
    }
    
    return nullptr;
}

UMaterialFunction* FUnrealMCPCommonUtils::FindMaterialFunction(const FString& FunctionName)
{
    // Step 1: If looks like an asset path, try to load directly
    if (FunctionName.StartsWith(TEXT("/")))
    {
        if (UMaterialFunction* Func = LoadObject<UMaterialFunction>(nullptr, *FunctionName))
        {
            UE_LOG(LogTemp, Display, TEXT("FindMaterialFunction: Loaded by path: %s"), *FunctionName);
            return Func;
        }
    }
    
    // Step 2: Search using AssetRegistry
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
    AssetRegistry.SearchAllAssets(true);
    
    FARFilter Filter;
    Filter.ClassPaths.Add(UMaterialFunction::StaticClass()->GetClassPathName());
    Filter.bRecursivePaths = true;
    
    TArray<FAssetData> AssetDataList;
    AssetRegistry.GetAssets(Filter, AssetDataList);
    
    // Exact name match
    for (const FAssetData& AssetData : AssetDataList)
    {
        if (AssetData.AssetName.ToString() == FunctionName)
        {
            UE_LOG(LogTemp, Display, TEXT("FindMaterialFunction: Found by exact name: %s at %s"), 
                *FunctionName, *AssetData.GetObjectPathString());
            return Cast<UMaterialFunction>(AssetData.GetAsset());
        }
    }
    
    // Case-insensitive
    for (const FAssetData& AssetData : AssetDataList)
    {
        if (AssetData.AssetName.ToString().Equals(FunctionName, ESearchCase::IgnoreCase))
        {
            UE_LOG(LogTemp, Display, TEXT("FindMaterialFunction: Found by case-insensitive name: %s"), *FunctionName);
            return Cast<UMaterialFunction>(AssetData.GetAsset());
        }
    }
    
    // Partial match
    for (const FAssetData& AssetData : AssetDataList)
    {
        if (AssetData.AssetName.ToString().Contains(FunctionName))
        {
            UE_LOG(LogTemp, Display, TEXT("FindMaterialFunction: Found by partial name: %s"), *FunctionName);
            return Cast<UMaterialFunction>(AssetData.GetAsset());
        }
    }
    
    return nullptr;
}

UEdGraph* FUnrealMCPCommonUtils::FindOrCreateEventGraph(UBlueprint* Blueprint)
{
    // Single implementation lives in the graph kernel.
    UEdGraph* Graph = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!FUnrealMCPBlueprintGraphOps::EnsureEventGraph(Blueprint, Graph, ErrorCode, ErrorMessage))
    {
        return nullptr;
    }
    return Graph;
}

// Blueprint component utilities
USCS_Node* FUnrealMCPCommonUtils::FindBlueprintComponentNode(UBlueprint* Blueprint, const FString& ComponentName,
                                                            FString& OutErrorCode, FString& OutErrorMessage)
{
    OutErrorCode.Reset();
    OutErrorMessage.Reset();

    if (!Blueprint)
    {
        OutErrorCode = TEXT("blueprint_not_ready");
        OutErrorMessage = TEXT("Invalid blueprint");
        return nullptr;
    }

    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
    if (!SCS || SCS->GetBlueprint() != Blueprint)
    {
        OutErrorCode = TEXT("blueprint_not_ready");
        OutErrorMessage = FString::Printf(
            TEXT("Blueprint '%s' has no usable SimpleConstructionScript (compile the blueprint first)"),
            *Blueprint->GetName());
        return nullptr;
    }

    TArray<FString> AvailableNames;
    USCS_Node* FoundNode = nullptr;
    for (USCS_Node* Node : SCS->GetAllNodes())
    {
        if (!Node)
        {
            continue;
        }
        const FString NodeName = Node->GetVariableName().ToString();
        AvailableNames.Add(NodeName);
        if (!FoundNode && NodeName == ComponentName && Node->ComponentTemplate)
        {
            FoundNode = Node;
        }
    }

    if (FoundNode)
    {
        return FoundNode;
    }

    OutErrorCode = TEXT("component_not_found");
    OutErrorMessage = FString::Printf(TEXT("Component '%s' not found in blueprint '%s'. Available components: %s"),
        *ComponentName, *Blueprint->GetName(),
        AvailableNames.Num() > 0 ? *FString::Join(AvailableNames, TEXT(", ")) : TEXT("<none>"));
    return nullptr;
}

UActorComponent* FUnrealMCPCommonUtils::FindWritableComponentTemplate(UBlueprint* Blueprint, const FString& ComponentName,
                                                                    FString& OutErrorCode, FString& OutErrorMessage,
                                                                    UClass** OutOwnerClass)
{
    if (OutOwnerClass)
    {
        *OutOwnerClass = nullptr;
    }

    FString NodeErrorCode;
    FString NodeErrorMessage;
    if (USCS_Node* Node = FindBlueprintComponentNode(Blueprint, ComponentName, NodeErrorCode, NodeErrorMessage))
    {
        OutErrorCode.Reset();
        OutErrorMessage.Reset();
        if (OutOwnerClass)
        {
            *OutOwnerClass = Blueprint ? Blueprint->GeneratedClass : nullptr;
        }
        return Node->ComponentTemplate;
    }
    OutErrorCode = NodeErrorCode;
    OutErrorMessage = NodeErrorMessage;

    // Inherited component: declared by a parent blueprint (SCS node) or native to a parent class
    // (ACharacter::Mesh). InheritableComponentHandler keys only cover SCS nodes and UCS AddComponent nodes
    // (Editor.cpp:1310-1338), so for the rest the writable template is THIS class's own instance of it -
    // writing the parent's template would change the parent class for every other user of it. Measured:
    // such a write survives a recompile and a package reload
    // (Saved/MCPScripts/inherited_component_persistence_probe.py).
    if (Blueprint && Blueprint->GeneratedClass && Blueprint->ParentClass)
    {
        for (UClass* OwnerClass = Blueprint->ParentClass; OwnerClass; OwnerClass = OwnerClass->GetSuperClass())
        {
            FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(OwnerClass, FName(*ComponentName));
            if (!Property || !Property->PropertyClass || !Property->PropertyClass->IsChildOf(UActorComponent::StaticClass()))
            {
                continue;
            }

            if (AActor* CDO = Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject()))
            {
                if (UActorComponent* Inherited = Cast<UActorComponent>(Property->GetObjectPropertyValue_InContainer(CDO)))
                {
                    OutErrorCode.Reset();
                    OutErrorMessage.Reset();
                    if (OutOwnerClass)
                    {
                        *OutOwnerClass = OwnerClass;
                    }
                    return Inherited;
                }
            }
            break;
        }
    }
    return nullptr;
}

bool FUnrealMCPCommonUtils::EnsureBlueprintComponentsReady(UBlueprint* Blueprint, FString& OutErrorCode, FString& OutErrorMessage)
{
    OutErrorCode.Reset();
    OutErrorMessage.Reset();

    if (!Blueprint)
    {
        OutErrorCode = TEXT("blueprint_not_ready");
        OutErrorMessage = TEXT("Invalid blueprint");
        return false;
    }

    auto IsReady = [Blueprint]()
    {
        return Blueprint->SimpleConstructionScript && Blueprint->SimpleConstructionScript->GetBlueprint() == Blueprint;
    };

    if (IsReady())
    {
        return true;
    }

    // A freshly created (or just re-created) blueprint may not have bound its SCS yet: compile once and re-check.
    FKismetEditorUtilities::CompileBlueprint(Blueprint);
    if (IsReady())
    {
        return true;
    }

    OutErrorCode = TEXT("blueprint_not_ready");
    OutErrorMessage = FString::Printf(
        TEXT("Blueprint '%s' still has no usable SimpleConstructionScript after compiling; component commands are unavailable"),
        *Blueprint->GetName());
    return false;
}

bool FUnrealMCPCommonUtils::ConnectGraphNodes(UEdGraph* Graph, UEdGraphNode* SourceNode, const FString& SourcePinName,
                                           UEdGraphNode* TargetNode, const FString& TargetPinName,
                                           FString& OutErrorCode, FString& OutErrorMessage)
{
    // Single implementation lives in the graph kernel.
    TArray<FString> Candidates;
    return FUnrealMCPBlueprintGraphOps::ConnectNodes(Graph, SourceNode, SourcePinName, TargetNode, TargetPinName,
                                                    OutErrorCode, OutErrorMessage, Candidates);
}

UEdGraphPin* FUnrealMCPCommonUtils::FindPin(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction)
{
    return FUnrealMCPBlueprintGraphOps::FindPin(Node, PinName, Direction);
}

bool FUnrealMCPCommonUtils::SetPinDefaultValue(UEdGraphNode* Node, const FString& PinName,
                                              const TSharedPtr<FJsonValue>& Value,
                                              FString& OutErrorCode, FString& OutErrorMessage)
{
    // Single implementation lives in the graph kernel.
    TArray<FString> Candidates;
    return FUnrealMCPBlueprintGraphOps::SetPinDefaultValue(Node, PinName, Value,
                                                          OutErrorCode, OutErrorMessage, Candidates);
}

// Actor utilities
TSharedPtr<FJsonValue> FUnrealMCPCommonUtils::ActorToJson(AActor* Actor)
{
    if (!Actor)
    {
        return MakeShared<FJsonValueNull>();
    }
    
    TSharedPtr<FJsonObject> ActorObject = MakeShared<FJsonObject>();
    ActorObject->SetStringField(TEXT("name"), Actor->GetName());
    ActorObject->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
    
    FVector Location = Actor->GetActorLocation();
    TArray<TSharedPtr<FJsonValue>> LocationArray;
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.X));
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Y));
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Z));
    ActorObject->SetArrayField(TEXT("location"), LocationArray);
    
    FRotator Rotation = Actor->GetActorRotation();
    TArray<TSharedPtr<FJsonValue>> RotationArray;
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
    ActorObject->SetArrayField(TEXT("rotation"), RotationArray);
    
    FVector Scale = Actor->GetActorScale3D();
    TArray<TSharedPtr<FJsonValue>> ScaleArray;
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.X));
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Y));
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Z));
    ActorObject->SetArrayField(TEXT("scale"), ScaleArray);
    
    return MakeShared<FJsonValueObject>(ActorObject);
}

TSharedPtr<FJsonObject> FUnrealMCPCommonUtils::ActorToJsonObject(AActor* Actor, bool bDetailed)
{
    if (!Actor)
    {
        return nullptr;
    }
    
    TSharedPtr<FJsonObject> ActorObject = MakeShared<FJsonObject>();
    ActorObject->SetStringField(TEXT("name"), Actor->GetName());
    ActorObject->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
    
    FVector Location = Actor->GetActorLocation();
    TArray<TSharedPtr<FJsonValue>> LocationArray;
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.X));
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Y));
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Z));
    ActorObject->SetArrayField(TEXT("location"), LocationArray);
    
    FRotator Rotation = Actor->GetActorRotation();
    TArray<TSharedPtr<FJsonValue>> RotationArray;
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
    ActorObject->SetArrayField(TEXT("rotation"), RotationArray);
    
    FVector Scale = Actor->GetActorScale3D();
    TArray<TSharedPtr<FJsonValue>> ScaleArray;
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.X));
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Y));
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Z));
    ActorObject->SetArrayField(TEXT("scale"), ScaleArray);
    
    // If detailed info requested, iterate all properties
    if (bDetailed)
    {
        TSharedPtr<FJsonObject> PropertiesObj = MakeShared<FJsonObject>();
        TArray<FString> OmittedProperties;
        int32 ReflectedPropertyCount = 0;

        for (TFieldIterator<FProperty> PropIt(Actor->GetClass()); PropIt; ++PropIt)
        {
            FProperty* Property = *PropIt;
            if (!Property) continue;

            ++ReflectedPropertyCount;
            FString PropName = Property->GetName();
            const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Actor);
            if (!ValuePtr)
            {
                OmittedProperties.Add(PropName);
                continue;
            }
            
            // Numeric types
            if (const FByteProperty* ByteProp = CastField<FByteProperty>(Property))
            {
                PropertiesObj->SetNumberField(PropName, ByteProp->GetPropertyValue(ValuePtr));
            }
            else if (const FIntProperty* IntProp = CastField<FIntProperty>(Property))
            {
                PropertiesObj->SetNumberField(PropName, IntProp->GetPropertyValue(ValuePtr));
            }
            else if (const FInt64Property* Int64Prop = CastField<FInt64Property>(Property))
            {
                PropertiesObj->SetNumberField(PropName, Int64Prop->GetPropertyValue(ValuePtr));
            }
            else if (const FFloatProperty* FloatProp = CastField<FFloatProperty>(Property))
            {
                PropertiesObj->SetNumberField(PropName, FloatProp->GetPropertyValue(ValuePtr));
            }
            else if (const FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Property))
            {
                PropertiesObj->SetNumberField(PropName, DoubleProp->GetPropertyValue(ValuePtr));
            }
            else if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
            {
                PropertiesObj->SetBoolField(PropName, BoolProp->GetPropertyValue(ValuePtr));
            }
            else if (const FStrProperty* StrProp = CastField<FStrProperty>(Property))
            {
                PropertiesObj->SetStringField(PropName, StrProp->GetPropertyValue(ValuePtr));
            }
            else if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
            {
                const UEnum* EnumDef = EnumProp->GetEnum();
                if (EnumDef)
                {
                    int64 EnumVal = EnumProp->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
                    PropertiesObj->SetStringField(PropName, EnumDef->GetNameStringByValue(EnumVal));
                }
            }
            // Object family. Subclass order matters: FSoftClassProperty derives from FSoftObjectProperty,
            // which derives from FObjectPropertyBase, and FClassProperty derives from FObjectProperty.
            else if (const FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(Property))
            {
                PropertiesObj->SetStringField(PropName, SoftClassProp->GetPropertyValue(ValuePtr).ToString());
            }
            else if (const FClassProperty* ClassProp = CastField<FClassProperty>(Property))
            {
                const UObject* ClassValue = ClassProp->GetObjectPropertyValue(ValuePtr);
                PropertiesObj->SetStringField(PropName, ClassValue ? ClassValue->GetPathName() : FString());
            }
            else if (const FSoftObjectProperty* SoftObjectProp = CastField<FSoftObjectProperty>(Property))
            {
                PropertiesObj->SetStringField(PropName, SoftObjectProp->GetPropertyValue(ValuePtr).ToString());
            }
            else if (const FObjectProperty* ObjectProp = CastField<FObjectProperty>(Property))
            {
                const UObject* ObjectValue = ObjectProp->GetObjectPropertyValue(ValuePtr);
                PropertiesObj->SetStringField(PropName, ObjectValue ? ObjectValue->GetPathName() : FString());
            }

            // Anything the dispatch above did not turn into a JSON value is registered by name instead of
            // being silently dropped: "this class has no such property" and "the property was not listed"
            // must stay distinguishable (that difference cost a GameMode overwrite on 2026-09-30).
            if (!PropertiesObj->HasField(PropName))
            {
                OmittedProperties.Add(PropName);
            }
        }
        
        ActorObject->SetObjectField(TEXT("properties"), PropertiesObj);
        ActorObject->SetNumberField(TEXT("properties_count"), PropertiesObj->Values.Num());
        ActorObject->SetNumberField(TEXT("omitted_count"), OmittedProperties.Num());
        ActorObject->SetNumberField(TEXT("reflected_property_count"), ReflectedPropertyCount);

        TArray<TSharedPtr<FJsonValue>> OmittedArray;
        OmittedArray.Reserve(OmittedProperties.Num());
        for (const FString& Name : OmittedProperties)
        {
            OmittedArray.Add(MakeShared<FJsonValueString>(Name));
        }
        ActorObject->SetArrayField(TEXT("omitted_properties"), OmittedArray);
    }
    
    return ActorObject;
}

void FUnrealMCPCommonUtils::FindPropertyNameSuggestions(UObject* Object, const FString& PropertyName,
                                                       TArray<FString>& OutSuggestions)
{
    OutSuggestions.Reset();

    if (!Object || PropertyName.IsEmpty())
    {
        return;
    }

    // Exact match means nothing to suggest
    if (Object->GetClass()->FindPropertyByName(*PropertyName))
    {
        return;
    }

    FString Normalized;
    Normalized.Reserve(PropertyName.Len());
    for (const TCHAR Char : PropertyName)
    {
        if (Char != TEXT('_'))
        {
            Normalized.AppendChar(FChar::ToLower(Char));
        }
    }
    // python callers routinely drop the C++ `b` prefix (HiddenInGame vs bHiddenInGame)
    const FString NormalizedWithPrefixB = TEXT("b") + Normalized;

    for (TFieldIterator<FProperty> PropertyIt(Object->GetClass()); PropertyIt; ++PropertyIt)
    {
        FString Candidate;
        Candidate.Reserve(PropertyIt->GetName().Len());
        for (const TCHAR Char : PropertyIt->GetName())
        {
            if (Char != TEXT('_'))
            {
                Candidate.AppendChar(FChar::ToLower(Char));
            }
        }

        if (Candidate == Normalized || Candidate == NormalizedWithPrefixB)
        {
            OutSuggestions.Add(PropertyIt->GetName());
        }
    }
}

namespace
{
    void SetWriteErrorCode(FString* OutErrorCode, const TCHAR* Code)
    {
        if (OutErrorCode)
        {
            *OutErrorCode = Code;
        }
    }

    /** Case/underscore-insensitive form, with the `b` prefix kept out of the comparison. */
    FString NormalizePropertyName(const FString& In)
    {
        FString Out;
        Out.Reserve(In.Len());
        for (const TCHAR Char : In)
        {
            if (Char != TEXT('_'))
            {
                Out.AppendChar(FChar::ToLower(Char));
            }
        }
        return Out;
    }

}

/**
 * Callers pass python-style names (snake_case, often without the C++ `b` prefix), so an exact-only
 * lookup rejects names another call site accepts. Resolve exact first, then by normalized name with
 * an optional `b` prefix - the same rule the reflector applies to struct fields (MCPPropertyReflector.cpp).
 * An ambiguous normalized match resolves to nothing so the caller still reports it as unknown.
 */
FProperty* FUnrealMCPCommonUtils::FindPropertyByNameNormalized(UClass* Class, const FString& PropertyName)
    {
        if (!Class || PropertyName.IsEmpty())
        {
            return nullptr;
        }
        if (FProperty* Exact = Class->FindPropertyByName(*PropertyName))
        {
            return Exact;
        }

        const FString Normalized = NormalizePropertyName(PropertyName);
        const FString NormalizedWithPrefixB = TEXT("b") + Normalized;

        FProperty* Match = nullptr;
        for (TFieldIterator<FProperty> It(Class); It; ++It)
        {
            const FString Candidate = NormalizePropertyName(It->GetName());
            if (Candidate == Normalized || Candidate == NormalizedWithPrefixB)
            {
                if (Match)
                {
                    return nullptr;
                }
                Match = *It;
            }
        }
        return Match;
    }

/**
 * A `UPROPERTY(Setter = Foo)` field is frequently only a mirror: USkeletalMeshComponent::SkeletalMeshAsset
 * is transient/editor-only and the engine reads SkinnedAsset, so a raw field write reports success while
 * the component keeps no mesh (see Docs/MCP_Findings_2026-09-25_blueprint-component-property-write.md).
 * The property itself knows its setter (FProperty::HasSetter/CallSetter, UnrealType.h:314-335) - ask it
 * rather than guessing a function name (`Setter =` is consumed by UHT and is not metadata).
 */
void FUnrealMCPCommonUtils::InvokePropertySetter(UObject* Object, FProperty* Property, void* ValueAddr)
    {
        if (Object && Property && ValueAddr && Property->HasSetter())
        {
            Property->CallSetter(Object, ValueAddr);
        }
    }

bool FUnrealMCPCommonUtils::SetObjectProperty(UObject* Object, const FString& PropertyName,
                                     const TSharedPtr<FJsonValue>& Value, FString& OutErrorMessage,
                                     TArray<FString>* OutAvailableFields, FString* OutErrorCode,
                                     FWriteResult* OutWriteResult)
{
    if (!Object)
    {
        OutErrorMessage = TEXT("Invalid object");
        SetWriteErrorCode(OutErrorCode, TEXT("write_failed"));
        return false;
    }

    FProperty* Property = FindPropertyByNameNormalized(Object->GetClass(), PropertyName);
    if (!Property)
    {
        TArray<FString> Suggestions;
        FindPropertyNameSuggestions(Object, PropertyName, Suggestions);
        OutErrorMessage = Suggestions.Num() > 0
            ? FString::Printf(TEXT("Property not found: %s. Did you mean: %s"), *PropertyName, *FString::Join(Suggestions, TEXT(", ")))
            : FString::Printf(TEXT("Property not found: %s"), *PropertyName);
        SetWriteErrorCode(OutErrorCode, TEXT("unknown_property"));
        return false;
    }

    // Address arithmetic belongs next to the FProperty that owns it: the reflector is the single place
    // that computes it, so no caller can accidentally pass a container pointer where a value address is
    // expected (which used to write to offset 0 and clobber unrelated properties).
    void* PropertyAddr = Property->ContainerPtrToValuePtr<void>(Object);

    const FWriteResult Result = FMCPPropertyReflector::FromJson(Property, PropertyAddr, PropertyName, Value);
    if (Result.bSuccess)
    {
        // `UPROPERTY(Setter = ...)` fields are often only a mirror of the state the engine reads
        // (USkeletalMeshComponent::SkeletalMeshAsset -> SkinnedAsset), so run the declared setter too.
        InvokePropertySetter(Object, Property, PropertyAddr);
        return true;
    }

    OutErrorMessage = Result.ErrorMessage;
    if (OutAvailableFields)
    {
        // Enum members are the useful candidates when the target is an enum, struct fields otherwise.
        *OutAvailableFields = Result.AvailableFields.Num() > 0 ? Result.AvailableFields : Result.Candidates;
    }
    SetWriteErrorCode(OutErrorCode, *Result.ErrorCode);
    if (OutWriteResult)
    {
        *OutWriteResult = Result;
    }
    return false;
}

// Resolve a class reference in every form callers actually write it in. The old create_blueprint path
// prepended "A" and then only recognised "APawn"/"AActor", so everything else (Character, AnimNotify,
// AnimInstance, a typo) silently became an Actor blueprint. Resolution is therefore: full path first,
// then the class name as it really is, then the legacy "A"/"U" prefixed spellings.
UClass* FUnrealMCPCommonUtils::ResolveUClass(const FString& ClassReference, TArray<FString>& OutTried, FString& OutResolvedPath)
{
    OutTried.Reset();
    OutResolvedPath.Reset();

    const FString Reference = ClassReference.TrimStartAndEnd();
    if (Reference.IsEmpty())
    {
        return nullptr;
    }

    auto TryForm = [&OutTried](const FString& Form) -> UClass*
    {
        OutTried.AddUnique(Form);

        // Bare names (no path) are looked up among loaded classes first; full paths go through FindObject.
        if (!Form.StartsWith(TEXT("/")))
        {
            if (UClass* Found = FindFirstObject<UClass>(*Form, EFindFirstObjectOptions::NativeFirst))
            {
                return Found;
            }
        }
        else if (UClass* Found = FindObject<UClass>(nullptr, *Form))
        {
            return Found;
        }
        return LoadClass<UClass>(nullptr, *Form);
    };

    UClass* Resolved = nullptr;

    // 1. A path: engine class ("/Script/Engine.Character") or a blueprint generated class ("/Game/BP_X.BP_X_C").
    if (Reference.StartsWith(TEXT("/")))
    {
        Resolved = TryForm(Reference);

        // "/Game/Dir/BP_X" -> its generated class, which is what a parent class reference means.
        if (!Resolved && !Reference.Contains(TEXT(".")))
        {
            int32 LastSlash = INDEX_NONE;
            const FString ShortName = Reference.FindLastChar('/', LastSlash)
                ? Reference.RightChop(LastSlash + 1)
                : Reference;
            Resolved = TryForm(FString::Printf(TEXT("%s.%s_C"), *Reference, *ShortName));
        }
    }

    // 2. The class name as it really is ("Character", "AnimNotify", "AnimInstance").
    if (!Resolved)
    {
        Resolved = TryForm(Reference);
        if (!Resolved)
        {
            Resolved = TryForm(FString::Printf(TEXT("/Script/Engine.%s"), *Reference));
        }
    }

    // 3. Legacy spellings that were the only ones the old code understood ("Pawn" -> "APawn",
    //    "UserWidget" -> "UUserWidget").
    if (!Resolved && !Reference.Contains(TEXT(".")))
    {
        Resolved = TryForm(TEXT("A") + Reference);
        if (!Resolved)
        {
            Resolved = TryForm(TEXT("U") + Reference);
        }
    }

    if (Resolved)
    {
        OutResolvedPath = Resolved->GetPathName();
    }
    return Resolved;
}

UObject* FUnrealMCPCommonUtils::FindAsset(const FString& AssetName)
{
    if (AssetName.IsEmpty())
    {
        return nullptr;
    }

    // Extract the short name from either shape: "/Voxel/.../MC_Quixel_Tess" -> "MC_Quixel_Tess" and
    // "/Game/A/B.B" -> "B". Carrying the ".B" part into the short name made every in-memory and
    // registry lookup miss for the object-path form, which is the form the commands return.
    FString ShortName = AssetName;
    int32 LastSlash = INDEX_NONE;
    if (ShortName.FindLastChar('/', LastSlash))
    {
        ShortName = ShortName.RightChop(LastSlash + 1);
    }
    int32 FirstDot = INDEX_NONE;
    if (ShortName.FindChar(TEXT('.'), FirstDot))
    {
        ShortName = ShortName.Left(FirstDot);
    }

    // 1. Already in memory, by path or by package: a just-created asset has no file on disk yet, and
    //    the create commands read their result back before the save happens.
    if (AssetName.StartsWith(TEXT("/")))
    {
        const FString FullPath = AssetName.Contains(TEXT("."))
            ? AssetName
            : FString::Printf(TEXT("%s.%s"), *AssetName, *ShortName);
        const FString PackageName = FPackageName::ObjectPathToPackageName(FullPath);

        if (UPackage* Package = FindPackage(/*Outer=*/nullptr, *PackageName))
        {
            if (UObject* Asset = StaticFindObjectFast(UObject::StaticClass(), Package, FName(*ShortName)))
            {
                return Asset;
            }
        }
    }

    // 2. Load from disk, but only when there is a file. Loading a path whose package is gone is NOT
    //    harmless: the engine's loader keeps a failed marker per package id (AsyncLoading2's
    //    FLoadedPackageRef::bHasFailed), and recreating the same path afterwards trips
    //    check(!bHasFailed) - the editor dies or the game thread stops ticking. So the disk decides
    //    before anything is loaded.
    if (AssetName.StartsWith(TEXT("/")))
    {
        FString FullPath = AssetName.Contains(TEXT("."))
            ? AssetName
            : FString::Printf(TEXT("%s.%s"), *AssetName, *ShortName);

        const FString PackageName = FPackageName::ObjectPathToPackageName(FullPath);
        if (FPackageName::DoesPackageExist(PackageName))
        {
            FTopLevelAssetPath AssetPath(FullPath);
            if (AssetPath.IsValid())
            {
                UObject* Asset = StaticLoadAsset(UObject::StaticClass(), AssetPath);
                if (Asset)
                {
                    UE_LOG(LogTemp, Display, TEXT("FindAsset: Loaded: %s"), *FullPath);
                    return Asset;
                }
            }
        }
    }

    // 3. Fallback: AssetRegistry by name
    {
        FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
        IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

        // Refresh the registry so newly created (not yet saved) assets are discoverable.
        AssetRegistry.SearchAllAssets(true);

        // Use GetAllAssets (a bare bRecursivePaths filter without ClassPaths/PackagePaths
        // enumerates nothing in UE5.x, so short-name lookup always failed).
        TArray<FAssetData> AssetDataList;
        AssetRegistry.GetAllAssets(AssetDataList);

        for (const FAssetData& AssetData : AssetDataList)
        {
            if (!AssetData.AssetName.ToString().Equals(ShortName, ESearchCase::IgnoreCase))
            {
                continue;
            }

            // A registry entry whose package file is gone is a leftover: resolving it would load a
            // missing package, which is exactly what step 2 refuses to do.
            if (!FPackageName::DoesPackageExist(AssetData.PackageName.ToString()))
            {
                continue;
            }

            UE_LOG(LogTemp, Display, TEXT("FindAsset: Found by name: %s at %s"),
                *ShortName, *AssetData.GetObjectPathString());
            return AssetData.GetAsset();
        }
    }

    return nullptr;
}

UObject* FUnrealMCPCommonUtils::CreateAssetDirect(const FString& AssetName, const FString& AssetFolder,
                                                  UClass* AssetClass, UFactory* Factory)
{
    if (!Factory || AssetName.IsEmpty())
    {
        return nullptr;
    }

    // Keep the factory alive for the duration of the call, like IAssetTools::CreateAsset does.
    FGCObjectScopeGuard DontGCFactory(Factory);

    const FString PackageName = UPackageTools::SanitizePackageName(AssetFolder + TEXT("/") + AssetName);
    UPackage* Package = CreatePackage(*PackageName);
    if (!Package)
    {
        return nullptr;
    }

    UObject* NewAsset = Factory->FactoryCreateNew(AssetClass, Package, FName(*AssetName),
                                                 RF_Public | RF_Standalone | RF_Transactional,
                                                 /*Context=*/nullptr, GWarn);
    if (!NewAsset)
    {
        return nullptr;
    }

    // Same tail as IAssetTools::CreateAsset, minus the package flag it derives from project settings.
    FAssetRegistryModule::AssetCreated(NewAsset);
    Package->MarkPackageDirty();
    return NewAsset;
}

TSharedPtr<FJsonValue> FUnrealMCPCommonUtils::PropertyValueToJson(FProperty* Property, const void* ValuePtr)
{
    // Single read implementation: the reflector owns the type dispatch and the value shapes.
    return FMCPPropertyReflector::ToJson(Property, ValuePtr);
}

TSharedPtr<FJsonObject> FUnrealMCPCommonUtils::ObjectPropertiesToJson(UObject* Object)
{
    if (!Object)
    {
        return MakeShared<FJsonObject>();
    }

    TSharedPtr<FJsonObject> PropertiesObj = MakeShared<FJsonObject>();

    UClass* Class = Object->GetClass();
    for (TFieldIterator<FProperty> PropIt(Class); PropIt; ++PropIt)
    {
        FProperty* Property = *PropIt;
        if (!Property) continue;

        FString PropName = Property->GetName();
        const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
        if (!ValuePtr) continue;

        PropertiesObj->SetField(PropName, PropertyValueToJson(Property, ValuePtr));
    }

    return PropertiesObj;
}
// Blueprint persistence: our own build script kills the editor, so unsaved graph /
// component edits are routinely lost. Every successful mutation is written out.
bool FUnrealMCPCommonUtils::SaveAssetForObject(UObject* Object)
{
    if (!Object)
    {
        return false;
    }

    // Write the package out unconditionally, i.e. ignore the dirty flag entirely. Both flags that a sanity
    // check would look at can be wrong for a graph edit: an edit that happens while the blueprint is being
    // compiled never reaches Blueprint->MarkPackageDirty() because
    // FBlueprintEditorUtils::MarkBlueprintAsModified returns early on bBeingCompiled
    // (BlueprintEditorUtils.cpp:1986) - and every structural graph edit compiles the skeleton right away
    // (BlueprintEditorUtils.cpp:1967), so "edit and compile" is one call stack. The editor's build script
    // then kills the process, so "looks clean" must not mean "skip the write": that is how
    // BP_TPSDemoGameMode kept losing its HUD wiring. UEditorLoadingAndSavingUtils::SavePackages saves the
    // package regardless of its dirty state once bOnlyDirty is false.
    UPackage* Package = Object->GetOutermost();
    if (!Package || Package == GetTransientPackage())
    {
        return false;
    }

    Package->MarkPackageDirty();
    return UEditorLoadingAndSavingUtils::SavePackages({ Package }, /*bOnlyDirty=*/false);
}

bool FUnrealMCPCommonUtils::IsPersistRequested(const TSharedPtr<FJsonObject>& Params)
{
    bool bPersist = true;
    if (Params.IsValid())
    {
        Params->TryGetBoolField(TEXT("persist"), bPersist);
    }
    return bPersist;
}

namespace
{
    // Dispatch runs on the game thread only, so a plain flag is enough - and nesting (a command that
    // reaches another command) restores the outer value when the inner scope exits.
    bool GDispatchPersist = true;
}

bool FUnrealMCPCommonUtils::IsPersistEnabled()
{
    return GDispatchPersist;
}

FUnrealMCPCommonUtils::FMCPPersistScope::FMCPPersistScope(bool bInPersist)
    : bPrevious(GDispatchPersist)
{
    GDispatchPersist = bInPersist;
}

FUnrealMCPCommonUtils::FMCPPersistScope::~FMCPPersistScope()
{
    GDispatchPersist = bPrevious;
}

bool FUnrealMCPCommonUtils::SaveBlueprintFromParams(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return false;
    }

    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return false;
    }

    if (UBlueprint* Blueprint = FindBlueprint(BlueprintName))
    {
        return SaveAssetForObject(Blueprint);
    }
    return false;
}

bool FUnrealMCPCommonUtils::ResponseIndicatesSuccess(const TSharedPtr<FJsonObject>& Response)
{
    return Response.IsValid() && !Response->HasField(TEXT("error"));
}
