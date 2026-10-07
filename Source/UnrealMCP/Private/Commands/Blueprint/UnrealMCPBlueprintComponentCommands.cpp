#include "Commands/Blueprint/UnrealMCPBlueprintCommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Commands/Blueprint/UnrealMCPBlueprintGraphOps.h"
#include "Commands/Blueprint/UnrealMCPBlueprintCommandHelpers.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/PrimitiveComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"

// The component tree of a Blueprint: reading the hierarchy and the edits that re-shape it (remove,
// re-root, detach, collision). Still the same handler class - only the file changed.

using namespace UnrealMCPBlueprintHelpers;

namespace
{
    /**
     * Enum member lookup that accepts the name callers actually write: the member name
     * ("ECC_Visibility"), the stripped form ("Visibility") and the authored display name.
     * ECollisionChannel / ECollisionResponse are prefixed in C++ but not in the details panel,
     * so accepting only the prefixed spelling would refuse the natural one.
     */
    bool ResolveEnumValueByName(const UEnum* Enum, const TCHAR* Prefix, const FString& Requested, int64& OutValue)
    {
        OutValue = INDEX_NONE;
        if (!Enum || Requested.IsEmpty())
        {
            return false;
        }

        const FString PrefixString(Prefix);
        OutValue = Enum->GetValueByNameString(Requested);
        if (OutValue == INDEX_NONE && !PrefixString.IsEmpty())
        {
            OutValue = Enum->GetValueByNameString(PrefixString + Requested);
        }
        if (OutValue != INDEX_NONE)
        {
            return true;
        }

        auto Normalize = [](const FString& In)
        {
            FString Out = In.ToLower();
            Out.ReplaceInline(TEXT("_"), TEXT(""));
            return Out;
        };

        const FString Wanted = Normalize(Requested);
        const int32 Count = Enum->NumEnums();
        for (int32 Index = 0; Index < Count; ++Index)
        {
            FString MemberName = Enum->GetNameStringByIndex(Index);
            if (MemberName.EndsWith(TEXT("_MAX")))
            {
                continue;
            }
            if (!PrefixString.IsEmpty() && MemberName.StartsWith(PrefixString))
            {
                MemberName.RightChopInline(PrefixString.Len());
            }
            if (Normalize(MemberName) == Wanted || Normalize(Enum->GetAuthoredNameStringByIndex(Index)) == Wanted)
            {
                OutValue = Enum->GetValueByIndex(Index);
                return true;
            }
        }
        return false;
    }

    // ---------------------------------------------------------------------------------
    // Component tree helpers
    // ---------------------------------------------------------------------------------

    bool IsInheritedComponent(const UBlueprint* Blueprint, const USCS_Node* Node)
    {
        if (!Blueprint || !Node)
        {
            return false;
        }

        // A component is inherited when an ancestor blueprint's construction script declares the same
        // variable: that is where the template and its defaults come from.
        for (UClass* Parent = Blueprint->ParentClass; Parent; Parent = Parent->GetSuperClass())
        {
            UBlueprint* ParentBlueprint = Cast<UBlueprint>(Parent->ClassGeneratedBy);
            if (ParentBlueprint && ParentBlueprint->SimpleConstructionScript
                && ParentBlueprint->SimpleConstructionScript->FindSCSNode(Node->GetVariableName()))
            {
                return true;
            }
        }
        return false;
    }

    TSharedPtr<FJsonObject> MakeComponentJson(const UBlueprint* Blueprint, USimpleConstructionScript* SCS, USCS_Node* Node)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("component_name"), Node->GetVariableName().ToString());
        Obj->SetStringField(TEXT("component_class"),
            Node->ComponentTemplate ? Node->ComponentTemplate->GetClass()->GetName() : FString());
        Obj->SetStringField(TEXT("component_template"),
            Node->ComponentTemplate ? Node->ComponentTemplate->GetPathName() : FString());

        // Relative location of the component TEMPLATE - the construction script's rest value - in the
        // same frame the entry already reports through `parent` / `parent_inherited`. Read from the
        // template rather than from a generated-class instance: the template is what this command
        // already hands out as `component_template`, and an uncompiled blueprint has no usable instance.
        // A component with no transform (a plain UActorComponent) gets an explicit null instead of a
        // missing field, so "no transform" stays distinguishable from "this build forgot the field".
        if (const USceneComponent* SceneTemplate = Cast<USceneComponent>(Node->ComponentTemplate))
        {
            const FVector Location = SceneTemplate->GetRelativeLocation();
            TArray<TSharedPtr<FJsonValue>> LocationJson;
            LocationJson.Add(MakeShared<FJsonValueNumber>(Location.X));
            LocationJson.Add(MakeShared<FJsonValueNumber>(Location.Y));
            LocationJson.Add(MakeShared<FJsonValueNumber>(Location.Z));
            Obj->SetArrayField(TEXT("relative_location"), LocationJson);
        }
        else
        {
            Obj->SetField(TEXT("relative_location"), MakeShared<FJsonValueNull>());
        }

        // Effective parent: an SCS parent node, or - for a component hanging under an inherited one
        // (a native component such as ACharacter::Mesh, or a parent blueprint's SCS component, e.g.
        // BP_ThirdPersonCharacter's CameraBoom under the native capsule) - the name recorded on the node
        // itself. Without this the node looks like a scene root.
        USCS_Node* ParentNode = SCS ? SCS->FindParentNode(Node) : nullptr;
        FString ParentName;
        if (ParentNode)
        {
            ParentName = ParentNode->GetVariableName().ToString();
        }
        else if (!Node->ParentComponentOrVariableName.IsNone())
        {
            ParentName = Node->ParentComponentOrVariableName.ToString();
        }

        if (ParentName.IsEmpty())
        {
            Obj->SetField(TEXT("parent"), MakeShared<FJsonValueNull>());
        }
        else
        {
            Obj->SetStringField(TEXT("parent"), ParentName);
        }

        if (Node->AttachToName.IsNone())
        {
            Obj->SetField(TEXT("attach_socket"), MakeShared<FJsonValueNull>());
        }
        else
        {
            Obj->SetStringField(TEXT("attach_socket"), Node->AttachToName.ToString());
        }

        Obj->SetBoolField(TEXT("is_root"), ParentName.IsEmpty());
        Obj->SetBoolField(TEXT("parent_inherited"),
            ParentNode == nullptr && !Node->ParentComponentOrVariableName.IsNone());

        TArray<FString> ChildNames;
        for (USCS_Node* Child : Node->GetChildNodes())
        {
            if (Child)
            {
                ChildNames.Add(Child->GetVariableName().ToString());
            }
        }
        Obj->SetArrayField(TEXT("children"), MakeStringArray(ChildNames));
        Obj->SetBoolField(TEXT("is_inherited"), IsInheritedComponent(Blueprint, Node));
        return Obj;
    }

    void CollectComponentJsons(const UBlueprint* Blueprint, USimpleConstructionScript* SCS,
                               TArray<TSharedPtr<FJsonValue>>& OutComponents)
    {
        if (!SCS)
        {
            return;
        }
        for (USCS_Node* Node : SCS->GetAllNodes())
        {
            if (Node)
            {
                OutComponents.Add(MakeShared<FJsonValueObject>(MakeComponentJson(Blueprint, SCS, Node)));
            }
        }
    }

    TArray<FString> MakeComponentNames(USimpleConstructionScript* SCS)
    {
        TArray<FString> Names;
        if (SCS)
        {
            for (USCS_Node* Node : SCS->GetAllNodes())
            {
                if (Node)
                {
                    Names.Add(Node->GetVariableName().ToString());
                }
            }
        }
        return Names;
    }

    /** The component commands share one ready-check plus one lookup, so they answer identically. */
    USCS_Node* ResolveComponentNode(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params,
                                    FString& OutComponentName, TSharedPtr<FJsonObject>& OutError)
    {
        if (!Params->TryGetStringField(TEXT("component_name"), OutComponentName))
        {
            OutError = FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
                TEXT("Missing 'component_name' parameter"));
            return nullptr;
        }

        FString ReadyErrorCode;
        FString ReadyErrorMessage;
        if (!FUnrealMCPCommonUtils::EnsureBlueprintComponentsReady(Blueprint, ReadyErrorCode, ReadyErrorMessage))
        {
            OutError = FUnrealMCPCommonUtils::CreateErrorResponse(ReadyErrorCode, ReadyErrorMessage);
            return nullptr;
        }

        FString LookupErrorCode;
        FString LookupErrorMessage;
        USCS_Node* Node = FUnrealMCPCommonUtils::FindBlueprintComponentNode(
            Blueprint, OutComponentName, LookupErrorCode, LookupErrorMessage);
        if (!Node)
        {
            OutError = MakeCandidatesError(LookupErrorCode, LookupErrorMessage,
                MakeComponentNames(Blueprint->SimpleConstructionScript));
            return nullptr;
        }
        return Node;
    }

    /** Readback shared by every component write: the tree as it stands after the edit. */
    TSharedPtr<FJsonObject> MakeComponentWriteResult(UBlueprint* Blueprint, const FString& ComponentName)
    {
        USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;

        TArray<TSharedPtr<FJsonValue>> Components;
        CollectComponentJsons(Blueprint, SCS, Components);

        int32 RootCount = 0;
        TArray<FString> RootNames;
        for (USCS_Node* Node : SCS->GetRootNodes())
        {
            if (Node)
            {
                ++RootCount;
                RootNames.Add(Node->GetVariableName().ToString());
            }
        }

        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
        if (!ComponentName.IsEmpty())
        {
            ResultObj->SetStringField(TEXT("component_name"), ComponentName);
        }
        ResultObj->SetArrayField(TEXT("components"), Components);
        ResultObj->SetNumberField(TEXT("component_count"), Components.Num());
        ResultObj->SetNumberField(TEXT("root_count"), RootCount);
        ResultObj->SetArrayField(TEXT("root_components"), MakeStringArray(RootNames));
        ResultObj->SetBoolField(TEXT("unique_root"), RootCount == 1);
        AppendCompileResult(Blueprint, ResultObj);
        return ResultObj;
    }
}

// =====================================================================================
// Component hierarchy
// =====================================================================================

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleGetBlueprintComponentHierarchy(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintOrNull(Params, TEXT("blueprint_name"), Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString ReadyErrorCode;
    FString ReadyErrorMessage;
    if (!FUnrealMCPCommonUtils::EnsureBlueprintComponentsReady(Blueprint, ReadyErrorCode, ReadyErrorMessage))
    {
        // Non-Actor blueprints (AnimNotify / AnimInstance) have no construction script at all.
        return FUnrealMCPCommonUtils::CreateErrorResponse(ReadyErrorCode, ReadyErrorMessage);
    }

    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;

    TArray<TSharedPtr<FJsonValue>> Components;
    CollectComponentJsons(Blueprint, SCS, Components);

    const TArray<FString> Names = MakeComponentNames(SCS);
    TArray<FString> Duplicates;
    {
        TSet<FString> Seen;
        for (const FString& Name : Names)
        {
            if (Seen.Contains(Name))
            {
                Duplicates.AddUnique(Name);
            }
            Seen.Add(Name);
        }
    }

    TArray<FString> RootNames;
    for (USCS_Node* Node : SCS->GetRootNodes())
    {
        if (Node)
        {
            RootNames.Add(Node->GetVariableName().ToString());
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetArrayField(TEXT("components"), Components);
    ResultObj->SetNumberField(TEXT("component_count"), Components.Num());
    ResultObj->SetArrayField(TEXT("root_components"), MakeStringArray(RootNames));
    ResultObj->SetNumberField(TEXT("root_count"), RootNames.Num());
    ResultObj->SetBoolField(TEXT("unique_root"), RootNames.Num() == 1);
    ResultObj->SetArrayField(TEXT("duplicate_components"), MakeStringArray(Duplicates));

    // How many entries carry no transform, so "every entry is null" can be told apart from "this
    // response has no such field" without a caller walking every entry by hand.
    int32 NoTransformCount = 0;
    for (const TSharedPtr<FJsonValue>& ComponentValue : Components)
    {
        const TSharedPtr<FJsonObject>* ComponentObject = nullptr;
        if (!ComponentValue.IsValid() || !ComponentValue->TryGetObject(ComponentObject) || !ComponentObject)
        {
            continue;
        }
        const TSharedPtr<FJsonValue> LocationField = (*ComponentObject)->TryGetField(TEXT("relative_location"));
        if (!LocationField.IsValid() || LocationField->IsNull())
        {
            ++NoTransformCount;
        }
    }
    ResultObj->SetNumberField(TEXT("no_transform_count"), NoTransformCount);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleRemoveComponentFromBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintOrNull(Params, TEXT("blueprint_name"), Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString ComponentName;
    USCS_Node* Node = ResolveComponentNode(Blueprint, Params, ComponentName, Error);
    if (!Node)
    {
        return Error;
    }

    bool bRecursive = true;
    if (Params->HasField(TEXT("recursive")))
    {
        Params->TryGetBoolField(TEXT("recursive"), bRecursive);
    }
    bool bForce = false;
    if (Params->HasField(TEXT("force")))
    {
        Params->TryGetBoolField(TEXT("force"), bForce);
    }

    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;

    TArray<FString> ChildNames;
    for (USCS_Node* Child : Node->GetChildNodes())
    {
        if (Child)
        {
            ChildNames.Add(Child->GetVariableName().ToString());
        }
    }

    if (ChildNames.Num() > 0 && !bRecursive)
    {
        TSharedPtr<FJsonObject> ErrorObj = MakeCandidatesError(EUnrealMCPGraphError::ComponentHasChildren,
            FString::Printf(TEXT("Component '%s' has children (%s). Pass recursive=true to remove them with it, "
                "or detach them first"), *ComponentName, *FString::Join(ChildNames, TEXT(", "))), ChildNames);
        ErrorObj->SetStringField(TEXT("hint"), TEXT("recursive=true"));
        return ErrorObj;
    }

    const bool bIsRoot = SCS->FindParentNode(Node) == nullptr;
    if (bIsRoot && SCS->GetRootNodes().Num() <= 1 && !bForce)
    {
        TSharedPtr<FJsonObject> ErrorObj = FUnrealMCPCommonUtils::CreateErrorResponse(
            EUnrealMCPGraphError::RootComponentProtected,
            FString::Printf(TEXT("'%s' is the blueprint's only root component; removing it needs force=true"),
                *ComponentName));
        ErrorObj->SetStringField(TEXT("hint"), TEXT("force=true"));
        return ErrorObj;
    }

    TArray<FString> RemovedNames;
    TFunction<void(USCS_Node*)> RemoveSubtree = [&](USCS_Node* SubtreeNode)
    {
        if (!SubtreeNode)
        {
            return;
        }
        // Children first: removing a parent would orphan them out of AllNodes, and RemoveNode then
        // has no parent to unlink them from.
        TArray<USCS_Node*> Children = SubtreeNode->GetChildNodes();
        for (USCS_Node* Child : Children)
        {
            RemoveSubtree(Child);
        }
        RemovedNames.Add(SubtreeNode->GetVariableName().ToString());
        SCS->RemoveNode(SubtreeNode, /*bValidateSceneRootNodes=*/false);
    };
    RemoveSubtree(Node);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeComponentWriteResult(Blueprint, ComponentName);
    ResultObj->SetArrayField(TEXT("removed_components"), MakeStringArray(RemovedNames));
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleSetBlueprintRootComponent(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintOrNull(Params, TEXT("blueprint_name"), Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString ComponentName;
    USCS_Node* Node = ResolveComponentNode(Blueprint, Params, ComponentName, Error);
    if (!Node)
    {
        return Error;
    }

    USceneComponent* NodeTemplate = Cast<USceneComponent>(Node->ComponentTemplate);
    if (!NodeTemplate)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::ComponentNotScene,
            FString::Printf(TEXT("'%s' is not a scene component, so it cannot be the SCS root"), *ComponentName));
    }

    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
    USCS_Node* ParentNode = SCS->FindParentNode(Node);

    if (!ParentNode && SCS->GetRootNodes().Num() == 1 && SCS->GetRootNodes()[0] == Node)
    {
        // Already the only root: idempotent.
        TSharedPtr<FJsonObject> ResultObj = MakeComponentWriteResult(Blueprint, ComponentName);
        ResultObj->SetBoolField(TEXT("already_root"), true);
        return ResultObj;
    }

    SCS->Modify();

    // 1. Take the promoted node out of its current place. AllNodes has to be updated here, because
    //    AddNode() below re-registers unconditionally and would otherwise list the node twice.
    if (ParentNode)
    {
        ParentNode->RemoveChildNode(Node, /*bRemoveFromAllNodes=*/true);
    }
    else if (SCS->GetRootNodes().Contains(Node))
    {
        SCS->RemoveNode(Node, /*bValidateSceneRootNodes=*/false);
    }

    // 2. Remember the nodes that were roots, then empty the root set.
    TArray<USCS_Node*> PreviousRoots;
    TArray<USCS_Node*> OtherRoots;
    for (USCS_Node* Root : SCS->GetRootNodes())
    {
        if (!Root || Root == Node)
        {
            continue;
        }
        // Only scene components can become children: a plain component (no transform) has to stay
        // at the top level, otherwise the SCS tree would hold a parent link it cannot express.
        if (Cast<USceneComponent>(Root->ComponentTemplate))
        {
            PreviousRoots.Add(Root);
        }
        else
        {
            OtherRoots.Add(Root);
        }
    }
    for (USCS_Node* Root : PreviousRoots)
    {
        SCS->RemoveNode(Root, /*bValidateSceneRootNodes=*/false);
    }
    for (USCS_Node* Root : OtherRoots)
    {
        SCS->RemoveNode(Root, /*bValidateSceneRootNodes=*/false);
    }

    // 3. Promote, then keep the old scene roots in the tree as its children.
    SCS->AddNode(Node);
    for (USCS_Node* Root : PreviousRoots)
    {
        Node->AddChildNode(Root, /*bAddToAllNodes=*/true);

        Root->bIsParentComponentNative = false;
        Root->ParentComponentOrVariableName = NAME_None;
        Root->ParentComponentOwnerClassName = NAME_None;
        Root->AttachToName = NAME_None;
        if (USceneComponent* RootTemplate = Cast<USceneComponent>(Root->ComponentTemplate))
        {
            RootTemplate->Modify();
            RootTemplate->SetupAttachment(NodeTemplate, NAME_None);
        }
    }
    for (USCS_Node* Root : OtherRoots)
    {
        SCS->AddNode(Root);
    }

    // 4. The new root has no parent and no offset (the engine's own "Make New Root" resets both).
    Node->bIsParentComponentNative = false;
    Node->ParentComponentOrVariableName = NAME_None;
    Node->ParentComponentOwnerClassName = NAME_None;
    Node->AttachToName = NAME_None;

    NodeTemplate->Modify();
    NodeTemplate->SetupAttachment(nullptr);
    NodeTemplate->SetRelativeLocation(FVector::ZeroVector);
    NodeTemplate->SetRelativeRotation(FRotator::ZeroRotator);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    return MakeComponentWriteResult(Blueprint, ComponentName);
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleDetachComponent(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintOrNull(Params, TEXT("blueprint_name"), Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString ComponentName;
    USCS_Node* Node = ResolveComponentNode(Blueprint, Params, ComponentName, Error);
    if (!Node)
    {
        return Error;
    }

    USceneComponent* NodeTemplate = Cast<USceneComponent>(Node->ComponentTemplate);
    if (!NodeTemplate)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::ComponentNotScene,
            FString::Printf(TEXT("'%s' is not a scene component, so it cannot be attached to the SCS root"),
                *ComponentName));
    }

    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
    USCS_Node* ParentNode = SCS->FindParentNode(Node);
    if (!ParentNode)
    {
        // Already a root: nothing to detach, but still answer with the readback.
        TSharedPtr<FJsonObject> ResultObj = MakeComponentWriteResult(Blueprint, ComponentName);
        ResultObj->SetBoolField(TEXT("already_root"), true);
        return ResultObj;
    }

    SCS->Modify();

    // Unlink from the parent, then register as a root. The AllNodes bookkeeping follows the same
    // rule as the attach command: only re-register when the detach actually removed the node.
    ParentNode->RemoveChildNode(Node, /*bRemoveFromAllNodes=*/true);
    SCS->AddNode(Node);

    Node->bIsParentComponentNative = false;
    Node->ParentComponentOrVariableName = NAME_None;
    Node->ParentComponentOwnerClassName = NAME_None;
    Node->AttachToName = NAME_None;

    NodeTemplate->Modify();
    NodeTemplate->SetupAttachment(nullptr);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    return MakeComponentWriteResult(Blueprint, ComponentName);
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintCommands::HandleSetComponentCollision(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = ResolveBlueprintOrNull(Params, TEXT("blueprint_name"), Error);
    if (!Blueprint)
    {
        return Error;
    }

    FString ComponentName;
    USCS_Node* Node = ResolveComponentNode(Blueprint, Params, ComponentName, Error);
    if (!Node)
    {
        return Error;
    }

    UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Node->ComponentTemplate);
    if (!Primitive)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::ComponentNotPrimitive,
            FString::Printf(TEXT("Component '%s' (%s) is not a primitive component and has no collision to configure"),
                *ComponentName, Node->ComponentTemplate ? *Node->ComponentTemplate->GetClass()->GetName() : TEXT("null")));
    }

    const TSharedPtr<FJsonObject>* Props = nullptr;
    if (!Params->TryGetObjectField(TEXT("props"), Props) || !Props || !Props->IsValid())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
            TEXT("Missing 'props' parameter (object of collision field to value)"));
    }

    // The friendly names are mapped onto the FBodyInstance fields the reflector writes. "responses" is
    // the exception: the response container is transient, so the reflector cannot write it, and the
    // engine's own SetResponseToChannels is used instead.
    const TMap<FString, FString> FieldAliases = {
        { TEXT("collision_enabled"), TEXT("CollisionEnabled") },
        { TEXT("collision_profile"), TEXT("CollisionProfileName") },
        { TEXT("object_type"), TEXT("ObjectType") },
    };
    const TArray<FString> KnownKeys = {
        TEXT("collision_enabled"), TEXT("collision_profile"), TEXT("object_type"), TEXT("responses")
    };

    FBodyInstance* Body = &Primitive->BodyInstance;
    TArray<TSharedPtr<FJsonValue>> Applied;
    TArray<TSharedPtr<FJsonValue>> Failed;

    // NOTE: the fields are written through the reflector, so the raw field changes without the
    // engine's private FBodyInstance::InvalidateCollisionProfileName() bookkeeping; the response
    // reads back what the fields actually hold, including a profile name that no longer matches
    // them. Callers who want profile-consistent settings write collision_profile.

    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Props)->Values)
    {
        const FString Key = Pair.Key;
        const TSharedPtr<FJsonValue> Value = Pair.Value;

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("property"), Key);

        if (!KnownKeys.Contains(Key))
        {
            Entry->SetStringField(TEXT("error_code"), EUnrealMCPGraphError::InvalidParams);
            Entry->SetStringField(TEXT("error"), FString::Printf(TEXT("Unknown collision field '%s'"), *Key));
            Entry->SetArrayField(TEXT("candidates"), MakeStringArray(KnownKeys));
            Failed.Add(MakeShared<FJsonValueObject>(Entry));
            continue;
        }

        if (Key == TEXT("responses"))
        {
            const TSharedPtr<FJsonObject>* ResponseMap = nullptr;
            if (!Value.IsValid() || Value->Type != EJson::Object || !Value->TryGetObject(ResponseMap) || !ResponseMap)
            {
                Entry->SetStringField(TEXT("error_code"), EUnrealMCPGraphError::InvalidParams);
                Entry->SetStringField(TEXT("error"), TEXT("'responses' expects an object of channel name to response name"));
                Failed.Add(MakeShared<FJsonValueObject>(Entry));
                continue;
            }

            const UEnum* ChannelEnum = StaticEnum<ECollisionChannel>();
            const UEnum* ResponseEnum = StaticEnum<ECollisionResponse>();
            FCollisionResponseContainer Container = Body->GetResponseToChannels();

            TSharedPtr<FJsonObject> AppliedResponses = MakeShared<FJsonObject>();
            bool bAnyResponseFailed = false;
            for (const TPair<FString, TSharedPtr<FJsonValue>>& ResponsePair : (*ResponseMap)->Values)
            {
                int64 ChannelValue = INDEX_NONE;
                if (!ResolveEnumValueByName(ChannelEnum, TEXT("ECC_"), ResponsePair.Key, ChannelValue))
                {
                    Entry->SetStringField(TEXT("error_code"), EUnrealMCPGraphError::InvalidParams);
                    Entry->SetStringField(TEXT("error"),
                        FString::Printf(TEXT("Unknown collision channel '%s'"), *ResponsePair.Key));
                    bAnyResponseFailed = true;
                    break;
                }
                if (!ResponsePair.Value.IsValid() || ResponsePair.Value->Type != EJson::String)
                {
                    Entry->SetStringField(TEXT("error_code"), EUnrealMCPGraphError::InvalidParams);
                    Entry->SetStringField(TEXT("error"), TEXT("A channel response expects a response name string"));
                    bAnyResponseFailed = true;
                    break;
                }
                int64 ResponseValue = INDEX_NONE;
                if (!ResolveEnumValueByName(ResponseEnum, TEXT("ECR_"), ResponsePair.Value->AsString(), ResponseValue))
                {
                    Entry->SetStringField(TEXT("error_code"), EUnrealMCPGraphError::InvalidParams);
                    Entry->SetStringField(TEXT("error"),
                        FString::Printf(TEXT("Unknown collision response '%s'"), *ResponsePair.Value->AsString()));
                    bAnyResponseFailed = true;
                    break;
                }

                const ECollisionChannel Channel = static_cast<ECollisionChannel>(ChannelValue);
                Container.SetResponse(Channel, static_cast<ECollisionResponse>(ResponseValue));
                AppliedResponses->SetStringField(ResponsePair.Key,
                    ResponseEnum ? ResponseEnum->GetNameStringByValue(ResponseValue) : ResponsePair.Value->AsString());
            }

            if (bAnyResponseFailed)
            {
                Failed.Add(MakeShared<FJsonValueObject>(Entry));
                continue;
            }

            if (!Body->SetResponseToChannels(Container))
            {
                Entry->SetStringField(TEXT("error_code"), EUnrealMCPGraphError::InvalidValue);
                Entry->SetStringField(TEXT("error"), TEXT("The engine refused the new response container"));
                Failed.Add(MakeShared<FJsonValueObject>(Entry));
                continue;
            }

            Entry->SetObjectField(TEXT("applied_responses"), AppliedResponses);
            Entry->SetBoolField(TEXT("applied"), true);
            Applied.Add(MakeShared<FJsonValueObject>(Entry));
            continue;
        }

        FString FieldName = FieldAliases.FindRef(Key);
        TSharedPtr<FJsonValue> WriteValue = Value;

        if (Key == TEXT("object_type") && Value.IsValid() && Value->Type == EJson::String)
        {
            // ECollisionChannel members are all named "ECC_..." and carry no authored display name,
            // so the reflector cannot resolve the natural "WorldStatic". Resolve it here and hand the
            // reflector the member name it does understand.
            int64 ChannelValue = INDEX_NONE;
            if (!ResolveEnumValueByName(StaticEnum<ECollisionChannel>(), TEXT("ECC_"), Value->AsString(), ChannelValue))
            {
                Entry->SetStringField(TEXT("error_code"), EUnrealMCPGraphError::InvalidParams);
                Entry->SetStringField(TEXT("error"),
                    FString::Printf(TEXT("Unknown collision channel '%s'"), *Value->AsString()));
                Failed.Add(MakeShared<FJsonValueObject>(Entry));
                continue;
            }
            FieldName = TEXT("ObjectType");
            WriteValue = MakeShared<FJsonValueString>(
                StaticEnum<ECollisionChannel>()->GetNameStringByValue(ChannelValue));
        }

        // One write per key, through the same reflector set_component_property uses. The value is the
        // struct's *field* object: the reflector resolves the property name and then writes the
        // named fields into it.
        TSharedPtr<FJsonObject> BodyFields = MakeShared<FJsonObject>();
        BodyFields->SetField(FieldName, WriteValue);

        FString WriteError;
        FString WriteErrorCode;
        TArray<FString> AvailableFields;
        Primitive->Modify();

        if (!FUnrealMCPCommonUtils::SetObjectProperty(Primitive, TEXT("BodyInstance"),
            MakeShared<FJsonValueObject>(BodyFields), WriteError, &AvailableFields, &WriteErrorCode, nullptr))
        {
            Entry->SetStringField(TEXT("error_code"),
                WriteErrorCode.IsEmpty() ? FString(EUnrealMCPGraphError::InvalidValue) : WriteErrorCode);
            Entry->SetStringField(TEXT("error"), WriteError);
            if (AvailableFields.Num() > 0)
            {
                Entry->SetArrayField(TEXT("candidates"), MakeStringArray(AvailableFields));
            }
            Failed.Add(MakeShared<FJsonValueObject>(Entry));
            continue;
        }

        Entry->SetBoolField(TEXT("applied"), true);
        Applied.Add(MakeShared<FJsonValueObject>(Entry));
    }

    Primitive->PostEditChange();
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    const UEnum* EnabledEnum = StaticEnum<ECollisionEnabled::Type>();
    const UEnum* ChannelEnum = StaticEnum<ECollisionChannel>();

    TSharedPtr<FJsonObject> CollisionObj = MakeShared<FJsonObject>();
    CollisionObj->SetStringField(TEXT("collision_enabled"),
        EnabledEnum ? EnabledEnum->GetNameStringByValue(static_cast<int64>(Body->GetCollisionEnabled(false))) : FString());
    CollisionObj->SetStringField(TEXT("collision_profile"), Body->GetCollisionProfileName().ToString());
    CollisionObj->SetStringField(TEXT("object_type"),
        ChannelEnum ? ChannelEnum->GetNameStringByValue(static_cast<int64>(Body->GetObjectType())) : FString());

    TSharedPtr<FJsonObject> ResponseObj = MakeShared<FJsonObject>();
    if (const UEnum* ResponseEnum = StaticEnum<ECollisionResponse>())
    {
        for (const TCHAR* ChannelName : { TEXT("WorldStatic"), TEXT("WorldDynamic"), TEXT("Pawn"),
            TEXT("Visibility"), TEXT("Camera"), TEXT("PhysicsBody") })
        {
            int64 ChannelValue = INDEX_NONE;
            if (!ResolveEnumValueByName(ChannelEnum, TEXT("ECC_"), ChannelName, ChannelValue))
            {
                continue;
            }
            const ECollisionChannel Channel = static_cast<ECollisionChannel>(ChannelValue);
            ResponseObj->SetStringField(ChannelName,
                ResponseEnum->GetNameStringByValue(static_cast<int64>(Body->GetResponseToChannel(Channel))));
        }
    }
    CollisionObj->SetObjectField(TEXT("responses"), ResponseObj);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetStringField(TEXT("component_name"), ComponentName);
    ResultObj->SetArrayField(TEXT("applied"), Applied);
    ResultObj->SetArrayField(TEXT("failed"), Failed);
    ResultObj->SetNumberField(TEXT("applied_count"), Applied.Num());
    ResultObj->SetNumberField(TEXT("failed_count"), Failed.Num());
    ResultObj->SetObjectField(TEXT("collision"), CollisionObj);
    AppendCompileResult(Blueprint, ResultObj);
    // Same consequence as set_component_property: the write landed on the SCS template, and the instances
    // already standing in the level keep their own collision instead of following it.
    FUnrealMCPCommonUtils::AddTemplateInstanceReport(Blueprint, ResultObj);
    return ResultObj;
}

