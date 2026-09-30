#pragma once

#include "CoreMinimal.h"
#include "Json.h"
#include "Compat/UnrealMCPVersionCompat.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Commands/Blueprint/UnrealMCPBlueprintGraphOps.h"
#include "Engine/Blueprint.h"
#include "K2Node_FunctionEntry.h"
#include "Reflection/MCPPropertyReflector.h"
#include "UObject/UnrealType.h"
#include "Kismet2/BlueprintEditorUtils.h"

/**
 * Response shapers shared by the blueprint command .cpp files.
 *
 * These used to exist twice - `MakeCandidatesError` next to `MakeGraphOpError`, and two copies of the
 * local variable name list - which is how the two halves of the same domain drift apart. One
 * definition keeps the error envelope and the compile read-back identical whichever file (or
 * domain) produced the response, which is what the acceptance scripts assert on.
 */
namespace UnrealMCPBlueprintHelpers
{
    inline TArray<TSharedPtr<FJsonValue>> MakeStringArray(const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> Items;
        for (const FString& Value : Values)
        {
            Items.Add(MakeShared<FJsonValueString>(Value));
        }
        return Items;
    }

    /** Structured error carrying the candidate names the caller can retry with. */
    inline TSharedPtr<FJsonObject> MakeCandidatesError(const FString& ErrorCode, const FString& ErrorMessage,
                                                       const TArray<FString>& Candidates)
    {
        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(ErrorCode, ErrorMessage);
        if (Candidates.Num() > 0)
        {
            Error->SetArrayField(TEXT("candidates"), MakeStringArray(Candidates));
        }
        return Error;
    }

    /** Same envelope, plus the structured details a failed property write carries. */
    inline TSharedPtr<FJsonObject> MakeCandidatesError(const FWriteResult& Write, const TArray<FString>& Candidates)
    {
        TSharedPtr<FJsonObject> Error = MakeCandidatesError(
            Write.ErrorCode.IsEmpty() ? FString(EUnrealMCPGraphError::InvalidValue) : Write.ErrorCode,
            Write.ErrorMessage, Candidates);

        const auto AddStrings = [&Error](const TCHAR* FieldName, const TArray<FString>& Values)
        {
            if (Values.Num() == 0)
            {
                return;
            }
            Error->SetArrayField(FieldName, MakeStringArray(Values));
        };
        AddStrings(TEXT("supported_shapes"), Write.SupportedShapes);
        AddStrings(TEXT("available_fields"), Write.AvailableFields);
        AddStrings(TEXT("enum_candidates"), Write.Candidates);
        if (Write.FailedIndex != INDEX_NONE)
        {
            Error->SetNumberField(TEXT("failed_index"), Write.FailedIndex);
        }
        if (!Write.Hint.IsEmpty())
        {
            Error->SetStringField(TEXT("hint"), Write.Hint);
        }
        if (!Write.CurrentValueJson.IsEmpty())
        {
            Error->SetStringField(TEXT("current_value"), Write.CurrentValueJson);
        }
        Error->SetBoolField(TEXT("unchanged"), Write.bUnchanged);
        return Error;
    }

    /** Read the compile result back into a response: every write command reports it. */
    inline void AppendCompileResult(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ResultObj)
    {
        if (!Blueprint || !ResultObj.IsValid())
        {
            return;
        }

        FString Status;
        bool bCompiled = false;
        TArray<FString> Errors;
        TArray<FString> Warnings;
        FUnrealMCPBlueprintGraphOps::CompileChecked(Blueprint, Status, bCompiled, Errors, Warnings);

        ResultObj->SetStringField(TEXT("status"), Status);
        ResultObj->SetBoolField(TEXT("compiled"), bCompiled);
        ResultObj->SetArrayField(TEXT("errors"), MakeStringArray(Errors));
    }

    /** Names of the local variables a function entry node already carries (for error candidates). */
    inline TArray<FString> MakeLocalVariableNames(const UK2Node_FunctionEntry* Entry)
    {
        TArray<FString> Names;
        if (Entry)
        {
            for (const FBPVariableDescription& Variable : Entry->LocalVariables)
            {
                Names.Add(Variable.VarName.ToString());
            }
        }
        return Names;
    }

    inline FString MakeContainerName(const FEdGraphPinType& PinType)
    {
        if (PinType.IsArray())
        {
            return TEXT("array");
        }
        if (PinType.IsSet())
        {
            return TEXT("set");
        }
        if (PinType.IsMap())
        {
            return TEXT("map");
        }
        return TEXT("none");
    }

    inline FString MakeSubClassString(const FEdGraphPinType& PinType)
    {
        if (PinType.PinSubCategoryObject.IsValid())
        {
            return PinType.PinSubCategoryObject->GetPathName();
        }
        return PinType.PinSubCategory.IsNone() ? FString() : PinType.PinSubCategory.ToString();
    }

    inline UBlueprint* ResolveBlueprintOrNull(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName,
                                       TSharedPtr<FJsonObject>& OutError)
    {
        FString BlueprintName;
        if (!Params->TryGetStringField(FieldName, BlueprintName))
        {
            OutError = FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::InvalidParams,
                FString::Printf(TEXT("Missing '%s' parameter"), FieldName));
            return nullptr;
        }

        UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
        if (!Blueprint)
        {
            OutError = FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPGraphError::BlueprintNotFound,
                FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
            return nullptr;
        }
        return Blueprint;
    }

    inline FBPVariableDescription* FindMutableMemberVariable(UBlueprint* Blueprint, const FName& VariableName)
    {
        const int32 Index = Blueprint ? FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VariableName) : INDEX_NONE;
        return (Index == INDEX_NONE) ? nullptr : &Blueprint->NewVariables[Index];
    }

    /** The member's FProperty on the generated class (null until a compile has created it). */
    inline FProperty* FindGeneratedMemberProperty(const UBlueprint* Blueprint, const FName& VariableName)
    {
        UClass* Class = Blueprint ? Blueprint->GeneratedClass.Get() : nullptr;
        return Class ? FindFProperty<FProperty>(Class, VariableName) : nullptr;
    }

    /**
     * The value the class default object actually holds, as export text. The compiler moves
     * FBPVariableDescription::DefaultValue into the CDO and then clears the description for
     * struct/container types, so the description alone is not a readback.
     */
    inline bool ReadCdoDefaultText(const UBlueprint* Blueprint, const FName& VariableName, FString& OutText)
    {
        FProperty* Property = FindGeneratedMemberProperty(Blueprint, VariableName);
        UObject* Cdo = Property ? Blueprint->GeneratedClass->GetDefaultObject(false) : nullptr;
        if (!Cdo)
        {
            return false;
        }
        OutText.Reset();
        Property->ExportTextItem_Direct(OutText, Property->ContainerPtrToValuePtr<void>(Cdo), nullptr, Cdo, PPF_None);
        return true;
    }

    /**
     * Parse a requested default through the property's own import path into a scratch value and
     * export it, so it can be compared with the CDO's export text. Returns false when the text
     * does not import.
     */
    inline bool NormalizeDefaultText(const FProperty* Property, const FString& Requested, FString& OutText)
    {
        if (!Property)
        {
            return false;
        }
        void* Scratch = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
        Property->InitializeValue(Scratch);
        FStringOutputDevice ImportErrors;
        const TCHAR* End = Property->ImportText_Direct(*Requested, Scratch, nullptr, PPF_None, &ImportErrors);
        const bool bOk = End != nullptr && ImportErrors.Len() == 0;
        if (bOk)
        {
            OutText.Reset();
            Property->ExportTextItem_Direct(OutText, Scratch, nullptr, nullptr, PPF_None);
        }
        Property->DestroyValue(Scratch);
        FMemory::Free(Scratch);
        return bOk;
    }

    /** default_value + default_value_source for a member variable (CDO first, description otherwise). */
    inline void WriteMemberDefaultValue(const UBlueprint* Blueprint, const FBPVariableDescription& Variable,
                                        const TSharedPtr<FJsonObject>& Obj)
    {
        FString CdoText;
        if (ReadCdoDefaultText(Blueprint, Variable.VarName, CdoText))
        {
            Obj->SetStringField(TEXT("default_value"), CdoText);
            Obj->SetStringField(TEXT("default_value_source"), TEXT("cdo"));
        }
        else
        {
            Obj->SetStringField(TEXT("default_value"), Variable.DefaultValue);
            Obj->SetStringField(TEXT("default_value_source"), TEXT("description"));
        }
    }

}