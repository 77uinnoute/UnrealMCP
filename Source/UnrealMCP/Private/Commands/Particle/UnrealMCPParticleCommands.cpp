#include "Commands/Particle/UnrealMCPParticleCommands.h"
#include "Commands/Particle/UnrealMCPParticleOps.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Commands/UnrealMCPEditorCommands.h"
#include "Core/MCPCommandRegistry.h"

#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Object.h"
#include "UObject/UnrealType.h"
#include "Particles/ParticleSystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealMCPParticleCommands, Log, All);

namespace
{
    // -----------------------------------------------------------------------
    // Parameter helpers
    // -----------------------------------------------------------------------

    int32 GetIntParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, int32 DefaultValue)
    {
        int32 Value = DefaultValue;
        if (Params.IsValid() && Params->TryGetNumberField(FieldName, Value))
        {
            return Value;
        }
        return DefaultValue;
    }

    float GetFloatParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, float DefaultValue)
    {
        double Value = DefaultValue;
        if (Params.IsValid() && Params->TryGetNumberField(FieldName, Value))
        {
            return static_cast<float>(Value);
        }
        return DefaultValue;
    }

    FString GetStringParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName,
                           const FString& DefaultValue = FString())
    {
        FString Value;
        if (Params.IsValid() && Params->TryGetStringField(FieldName, Value))
        {
            return Value;
        }
        return DefaultValue;
    }

    FString GetAssetPathParam(const TSharedPtr<FJsonObject>& Params)
    {
        return GetStringParam(Params, TEXT("asset_path"));
    }

    // -----------------------------------------------------------------------
    // JSON shaping: the command layer is the only place that knows about JSON,
    // the particle logic lives in FUnrealMCPParticleOps.
    // -----------------------------------------------------------------------

    TSharedPtr<FJsonObject> MakeParticleErrorJson(const FString& ErrorCode, const FString& ErrorMessage,
                                      const TArray<FString>& Candidates = TArray<FString>(),
                                      const TArray<FString>& AvailableFields = TArray<FString>(),
                                      int32 FailedIndex = INDEX_NONE)
    {
        TSharedPtr<FJsonObject> Error = FUnrealMCPCommonUtils::CreateErrorResponse(ErrorCode, ErrorMessage);

        TArray<TSharedPtr<FJsonValue>> CandidateArray;
        for (const FString& Candidate : Candidates)
        {
            CandidateArray.Add(MakeShared<FJsonValueString>(Candidate));
        }
        if (CandidateArray.Num() > 0)
        {
            Error->SetArrayField(TEXT("candidates"), CandidateArray);
        }

        if (AvailableFields.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> FieldsArray;
            for (const FString& Field : AvailableFields)
            {
                FieldsArray.Add(MakeShared<FJsonValueString>(Field));
            }
            Error->SetArrayField(TEXT("available_fields"), FieldsArray);
        }

        if (FailedIndex != INDEX_NONE)
        {
            Error->SetNumberField(TEXT("failed_index"), FailedIndex);
        }
        return Error;
    }

    /**
     * Property values reach the command layer as text (JsonValueToText). Scalars stay text so
     * existing callers keep comparing them literally, while structs and arrays are handed back as
     * real JSON, so a readback can be fed straight back into a write.
     */
    void SetPropertyValueField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, const FString& Text)
    {
        if (Text.IsEmpty())
        {
            return;
        }

        if (Text.StartsWith(TEXT("{")) || Text.StartsWith(TEXT("[")))
        {
            TSharedPtr<FJsonValue> Parsed;
            const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
            if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
            {
                Object->SetField(FieldName, Parsed);
                return;
            }
        }

        Object->SetStringField(FieldName, Text);
    }

    const TCHAR* const DetailSummary = TEXT("summary");
    const TCHAR* const DetailFull = TEXT("full");

    /**
     * Readback shaping for the current command. Write commands default to "summary": a 7 emitter
     * system makes every write response tens of kilobytes of unchanged state, which is what pushed
     * callers into trimming it themselves.
     */
    struct FParticleResponseOptions
    {
        bool bSummary = false;
        bool bIncludeModules = true;
    };

    FParticleResponseOptions& CurrentResponseOptions()
    {
        static FParticleResponseOptions Options;
        return Options;
    }

    /** Installs the options of one command and restores the previous ones on the way out. */
    struct FScopedResponseOptions
    {
        FParticleResponseOptions Previous;

        explicit FScopedResponseOptions(const FParticleResponseOptions& Options)
        {
            Previous = CurrentResponseOptions();
            CurrentResponseOptions() = Options;
        }

        ~FScopedResponseOptions()
        {
            CurrentResponseOptions() = Previous;
        }
    };

    TArray<FString> DetailCandidates()
    {
        return TArray<FString>{ DetailSummary, DetailFull };
    }

    /** Resolve "detail" (and the equivalent return_state=false). Invalid values are rejected. */
    bool ResolveDetailParam(const FString& RequestedDetail, bool bWriteCommand, FString& OutDetail, FString& OutError)
    {
        if (RequestedDetail.IsEmpty())
        {
            OutDetail = bWriteCommand ? DetailSummary : DetailFull;
            return true;
        }
        if (RequestedDetail.Equals(DetailSummary, ESearchCase::IgnoreCase))
        {
            OutDetail = DetailSummary;
            return true;
        }
        if (RequestedDetail.Equals(DetailFull, ESearchCase::IgnoreCase))
        {
            OutDetail = DetailFull;
            return true;
        }

        OutError = FString::Printf(TEXT("'detail' must be '%s' or '%s', got '%s'"),
            DetailSummary, DetailFull, *RequestedDetail);
        return false;
    }

    /** Every top level field a particle response can carry, used to validate "fields". */
    const TArray<FString>& KnownResponseFields()
    {
        static const TArray<FString> Fields = {
            TEXT("success"), TEXT("asset_path"), TEXT("emitter_count"), TEXT("emitters"),
            TEXT("emitter_index"), TEXT("lod_index"), TEXT("module_class"), TEXT("module_index"),
            TEXT("property_name"), TEXT("property_type"), TEXT("property_value_before"),
            TEXT("property_value_after"), TEXT("resolved_from"), TEXT("units"), TEXT("hint"),
            TEXT("cleared_slots"), TEXT("adjusted_emitters"), TEXT("closed_editors"), TEXT("module"), TEXT("detail"),
            TEXT("burst_count"), TEXT("sprite_hidden"), TEXT("b_use_max_draw_count"), TEXT("max_draw_count"),
            TEXT("from_index"), TEXT("to_index"), TEXT("module_order"), TEXT("added"), TEXT("failed"),
            TEXT("name_before"), TEXT("name_after"), TEXT("lod_enabled"), TEXT("module_counts"),
            TEXT("check_count"), TEXT("issue_count"), TEXT("checks"),
        };
        return Fields;
    }

    /** Requested "fields" list; empty when the parameter is absent. */
    bool ReadRequestedFields(const TSharedPtr<FJsonObject>& Params, TArray<FString>& OutFields, FString& OutError)
    {
        OutFields.Reset();
        OutError.Reset();

        if (!Params.IsValid() || !Params->HasField(TEXT("fields")))
        {
            return true;
        }

        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Params->TryGetArrayField(TEXT("fields"), Values) || !Values)
        {
            OutError = TEXT("'fields' must be an array of response field names");
            return false;
        }

        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            if (!Value.IsValid() || Value->Type != EJson::String)
            {
                OutError = TEXT("'fields' entries must be strings");
                return false;
            }

            const FString Field = Value->AsString();
            if (!KnownResponseFields().Contains(Field))
            {
                OutError = FString::Printf(TEXT("Unknown response field '%s'"), *Field);
                return false;
            }
            OutFields.AddUnique(Field);
        }
        return true;
    }

    /** Keep only the requested top level fields; "success" and "detail" are never trimmed. */
    void ApplyFieldProjection(const TSharedPtr<FJsonObject>& Result, const TArray<FString>& RequestedFields)
    {
        if (!Result.IsValid() || RequestedFields.Num() == 0)
        {
            return;
        }

        TSet<FString> Keep(RequestedFields);
        Keep.Add(TEXT("success"));
        Keep.Add(TEXT("detail"));

        TSharedPtr<FJsonObject> Filtered = MakeShared<FJsonObject>();
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Result->Values)
        {
            if (Keep.Contains(Pair.Key))
            {
                Filtered->SetField(Pair.Key, Pair.Value);
            }
        }

        Result->Values = Filtered->Values;
    }

    TSharedPtr<FJsonObject> PropertyInfoToJson(const FUnrealMCPParticlePropertyInfo& Property)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("name"), Property.Name);
        Object->SetStringField(TEXT("type"), Property.Type);
        Object->SetStringField(TEXT("value"), Property.Value);
        if (!Property.Units.IsEmpty())
        {
            Object->SetStringField(TEXT("units"), Property.Units);
        }
        if (!Property.Hint.IsEmpty())
        {
            Object->SetStringField(TEXT("hint"), Property.Hint);
        }
        return Object;
    }

    TSharedPtr<FJsonObject> DistributionKeyToJson(const FUnrealMCPParticleDistributionKey& Key)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetNumberField(TEXT("time"), Key.Time);

        TArray<TSharedPtr<FJsonValue>> Values;
        for (const float Value : Key.Value)
        {
            Values.Add(MakeShared<FJsonValueNumber>(Value));
        }
        Object->SetArrayField(TEXT("value"), Values);
        Object->SetStringField(TEXT("interp"), Key.Interp);
        return Object;
    }

    TArray<TSharedPtr<FJsonValue>> NumbersToJson(const TArray<float>& Numbers)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const float Number : Numbers)
        {
            Values.Add(MakeShared<FJsonValueNumber>(Number));
        }
        return Values;
    }

    TSharedPtr<FJsonObject> DistributionInfoToJson(const FUnrealMCPParticleDistributionInfo& Distribution)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("property_name"), Distribution.PropertyName);
        Object->SetStringField(TEXT("kind"), Distribution.Kind);
        Object->SetStringField(TEXT("distribution_class"), Distribution.DistributionClass);

        if (Distribution.Constants.Num() > 0)
        {
            Object->SetArrayField(TEXT("constants"), NumbersToJson(Distribution.Constants));
        }
        if (Distribution.MinMax.Num() > 0)
        {
            Object->SetArrayField(TEXT("min_max"), NumbersToJson(Distribution.MinMax));
        }
        if (Distribution.Keys.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Keys;
            for (const FUnrealMCPParticleDistributionKey& Key : Distribution.Keys)
            {
                Keys.Add(MakeShared<FJsonValueObject>(DistributionKeyToJson(Key)));
            }
            Object->SetArrayField(TEXT("keys"), Keys);
        }
        if (Distribution.SampledValue.Num() > 0)
        {
            Object->SetArrayField(TEXT("sampled_value"), NumbersToJson(Distribution.SampledValue));
        }
        if (!Distribution.Units.IsEmpty())
        {
            Object->SetStringField(TEXT("units"), Distribution.Units);
        }
        if (!Distribution.Hint.IsEmpty())
        {
            Object->SetStringField(TEXT("hint"), Distribution.Hint);
        }
        return Object;
    }

    /**
     * Typed property values of a live module. FUnrealMCPParticleOps is JSON-free, so the
     * typed form is produced here from the module object the info struct carries back.
     */
    TSharedPtr<FJsonObject> ModulePropertyValuesToJson(const FUnrealMCPParticleModuleInfo& Module)
    {
        TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
        if (!Module.Module)
        {
            return Properties;
        }

        for (TFieldIterator<FProperty> It(Module.Module->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
        {
            FProperty* Property = *It;
            if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Transient))
            {
                continue;
            }

            bool bIsDistribution = false;
            for (const FUnrealMCPParticleDistributionInfo& Distribution : Module.Distributions)
            {
                if (Distribution.PropertyName == Property->GetName())
                {
                    bIsDistribution = true;
                    break;
                }
            }
            if (bIsDistribution)
            {
                continue;
            }

            Properties->SetField(Property->GetName(),
                FUnrealMCPCommonUtils::PropertyValueToJson(Property, Property->ContainerPtrToValuePtr<void>(Module.Module)));
        }
        return Properties;
    }

    TSharedPtr<FJsonObject> ModuleInfoToJson(const FUnrealMCPParticleModuleInfo& Module)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetNumberField(TEXT("module_index"), Module.ModuleIndex);
        Object->SetStringField(TEXT("module_class"), Module.ModuleClass);
        Object->SetStringField(TEXT("module_name"), Module.ModuleName);
        Object->SetStringField(TEXT("object_path"), Module.ObjectPath);
        Object->SetStringField(TEXT("slot"), Module.Slot);
        Object->SetNumberField(TEXT("lod_validity"), Module.LODValidity);
        Object->SetBoolField(TEXT("editable"), Module.Editable);

        TArray<TSharedPtr<FJsonValue>> ActiveLods;
        for (const int32 LODIndex : Module.ActiveLods)
        {
            ActiveLods.Add(MakeShared<FJsonValueNumber>(LODIndex));
        }
        Object->SetArrayField(TEXT("active_lods"), ActiveLods);

        Object->SetObjectField(TEXT("properties"), ModulePropertyValuesToJson(Module));

        TArray<TSharedPtr<FJsonValue>> Properties;
        for (const FUnrealMCPParticlePropertyInfo& Property : Module.Properties)
        {
            Properties.Add(MakeShared<FJsonValueObject>(PropertyInfoToJson(Property)));
        }
        Object->SetArrayField(TEXT("property_details"), Properties);

        TArray<TSharedPtr<FJsonValue>> Distributions;
        for (const FUnrealMCPParticleDistributionInfo& Distribution : Module.Distributions)
        {
            Distributions.Add(MakeShared<FJsonValueObject>(DistributionInfoToJson(Distribution)));
        }
        Object->SetArrayField(TEXT("distributions"), Distributions);
        return Object;
    }

    TSharedPtr<FJsonObject> LODInfoToJson(const FUnrealMCPParticleLODInfo& LOD)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetNumberField(TEXT("lod_index"), LOD.LODIndex);
        Object->SetNumberField(TEXT("level"), LOD.Level);
        Object->SetNumberField(TEXT("module_count"), LOD.ModuleCount);
        Object->SetBoolField(TEXT("enabled"), LOD.Enabled);

        auto SetSlotField = [&Object](const TCHAR* FieldName, const FString& ClassName)
        {
            if (ClassName.IsEmpty())
            {
                Object->SetField(FieldName, MakeShared<FJsonValueNull>());
            }
            else
            {
                Object->SetStringField(FieldName, ClassName);
            }
        };
        SetSlotField(TEXT("required_module"), LOD.RequiredModuleClass);
        SetSlotField(TEXT("type_data_module"), LOD.TypeDataModuleClass);
        SetSlotField(TEXT("spawn_module"), LOD.SpawnModuleClass);
        SetSlotField(TEXT("event_generator"), LOD.EventGeneratorClass);

        auto ClassesToJson = [](const TArray<FString>& Classes)
        {
            TArray<TSharedPtr<FJsonValue>> Values;
            for (const FString& ClassName : Classes)
            {
                Values.Add(MakeShared<FJsonValueString>(ClassName));
            }
            return Values;
        };
        Object->SetArrayField(TEXT("spawning_modules"), ClassesToJson(LOD.SpawningModuleClasses));
        Object->SetArrayField(TEXT("spawn_modules"), ClassesToJson(LOD.SpawnModuleClasses));
        Object->SetArrayField(TEXT("update_modules"), ClassesToJson(LOD.UpdateModuleClasses));

        if (CurrentResponseOptions().bIncludeModules)
        {
            TArray<TSharedPtr<FJsonValue>> Modules;
            for (const FUnrealMCPParticleModuleInfo& Module : LOD.Modules)
            {
                Modules.Add(MakeShared<FJsonValueObject>(ModuleInfoToJson(Module)));
            }
            Object->SetArrayField(TEXT("modules"), Modules);
        }
        return Object;
    }

    TSharedPtr<FJsonObject> EmitterInfoToJson(const FUnrealMCPParticleEmitterInfo& Emitter)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetNumberField(TEXT("emitter_index"), Emitter.EmitterIndex);
        Object->SetStringField(TEXT("emitter_class"), Emitter.EmitterClass);
        Object->SetStringField(TEXT("emitter_name"), Emitter.EmitterName);
        Object->SetStringField(TEXT("object_path"), Emitter.ObjectPath);
        Object->SetNumberField(TEXT("lod_count"), Emitter.LODCount);

        TArray<TSharedPtr<FJsonValue>> Lods;
        for (const FUnrealMCPParticleLODInfo& LOD : Emitter.Lods)
        {
            Lods.Add(MakeShared<FJsonValueObject>(LODInfoToJson(LOD)));
        }
        Object->SetArrayField(TEXT("lods"), Lods);
        return Object;
    }

    TArray<TSharedPtr<FJsonValue>> EmittersToJson(const TArray<FUnrealMCPParticleEmitterInfo>& Emitters)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const FUnrealMCPParticleEmitterInfo& Emitter : Emitters)
        {
            Values.Add(MakeShared<FJsonValueObject>(EmitterInfoToJson(Emitter)));
        }
        return Values;
    }

    void AddEmitterReadback(const TSharedPtr<FJsonObject>& Result, const TArray<FUnrealMCPParticleEmitterInfo>& Emitters)
    {
        Result->SetNumberField(TEXT("emitter_count"), Emitters.Num());
        if (CurrentResponseOptions().bSummary)
        {
            return;
        }
        Result->SetArrayField(TEXT("emitters"), EmittersToJson(Emitters));
    }

    TSharedPtr<FJsonObject> ListResultToJson(const FUnrealMCPParticleListResult& List)
    {
        if (!List.Success)
        {
            return MakeParticleErrorJson(List.ErrorCode, List.ErrorMessage, List.Candidates);
        }

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("success"), true);
        Result->SetStringField(TEXT("asset_path"), List.AssetPath);
        Result->SetStringField(TEXT("detail"), CurrentResponseOptions().bSummary ? DetailSummary : DetailFull);
        AddEmitterReadback(Result, List.Emitters);
        return Result;
    }

    TSharedPtr<FJsonObject> OpResultToJson(const FUnrealMCPParticleOpResult& Op)
    {
        if (!Op.Success)
        {
            TSharedPtr<FJsonObject> Error = MakeParticleErrorJson(Op.ErrorCode, Op.ErrorMessage, Op.Candidates,
                                                                  Op.AvailableFields);
            // A refused write still reports what it resolved: an alias that landed on a
            // distribution is exactly the case where the caller needs the real name back.
            if (!Op.PropertyName.IsEmpty())
            {
                Error->SetStringField(TEXT("property_name"), Op.PropertyName);
            }
            if (!Op.ResolvedFrom.IsEmpty())
            {
                Error->SetStringField(TEXT("resolved_from"), Op.ResolvedFrom);
            }
            if (!Op.Units.IsEmpty())
            {
                Error->SetStringField(TEXT("units"), Op.Units);
            }
            if (!Op.Hint.IsEmpty())
            {
                Error->SetStringField(TEXT("hint"), Op.Hint);
            }
            // Container writes whose element was refused report which element, so the caller can fix
            // the exact entry instead of the whole value.
            if (Op.FailedIndex >= 0)
            {
                Error->SetNumberField(TEXT("failed_index"), Op.FailedIndex);
            }
            if (Op.bUnchanged)
            {
                Error->SetBoolField(TEXT("unchanged"), true);
            }
            return Error;
        }

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("success"), true);
        if (!Op.AssetPath.IsEmpty())
        {
            Result->SetStringField(TEXT("asset_path"), Op.AssetPath);
        }
        if (Op.EmitterIndex >= 0)
        {
            Result->SetNumberField(TEXT("emitter_index"), Op.EmitterIndex);
        }
        if (Op.LODIndex >= 0)
        {
            Result->SetNumberField(TEXT("lod_index"), Op.LODIndex);
        }
        if (!Op.ModuleClass.IsEmpty())
        {
            Result->SetStringField(TEXT("module_class"), Op.ModuleClass);
        }
        if (Op.ModuleIndex >= 0)
        {
            Result->SetNumberField(TEXT("module_index"), Op.ModuleIndex);
        }
        if (!Op.PropertyName.IsEmpty())
        {
            Result->SetStringField(TEXT("property_name"), Op.PropertyName);
        }
        if (!Op.PropertyType.IsEmpty())
        {
            Result->SetStringField(TEXT("property_type"), Op.PropertyType);
        }
        SetPropertyValueField(Result, TEXT("property_value_before"), Op.ValueBefore);
        SetPropertyValueField(Result, TEXT("property_value_after"), Op.ValueAfter);
        if (!Op.ResolvedFrom.IsEmpty())
        {
            Result->SetStringField(TEXT("resolved_from"), Op.ResolvedFrom);
        }
        if (!Op.Units.IsEmpty())
        {
            Result->SetStringField(TEXT("units"), Op.Units);
        }
        if (!Op.Hint.IsEmpty())
        {
            Result->SetStringField(TEXT("hint"), Op.Hint);
        }
        if (Op.ClearedSlots.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Cleared;
            for (const FString& Slot : Op.ClearedSlots)
            {
                Cleared.Add(MakeShared<FJsonValueString>(Slot));
            }
            Result->SetArrayField(TEXT("cleared_slots"), Cleared);
        }
        if (Op.BurstCount >= 0)
        {
            Result->SetNumberField(TEXT("burst_count"), Op.BurstCount);
        }
        if (Op.SpriteHidden)
        {
            Result->SetBoolField(TEXT("sprite_hidden"), true);
            Result->SetBoolField(TEXT("b_use_max_draw_count"), Op.bUseMaxDrawCount);
            Result->SetNumberField(TEXT("max_draw_count"), Op.MaxDrawCount);
        }
        if (Op.FromIndex >= 0 || Op.ToIndex >= 0)
        {
            Result->SetNumberField(TEXT("from_index"), Op.FromIndex);
            Result->SetNumberField(TEXT("to_index"), Op.ToIndex);
        }
        if (Op.ModuleOrder.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Order;
            for (const FString& ModuleClass : Op.ModuleOrder)
            {
                Order.Add(MakeShared<FJsonValueString>(ModuleClass));
            }
            Result->SetArrayField(TEXT("module_order"), Order);
        }
        if (Op.AddedModules.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Added;
            for (const FUnrealMCPParticleAddedModule& AddedModule : Op.AddedModules)
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("module_class"), AddedModule.ModuleClass);
                Entry->SetNumberField(TEXT("module_index"), AddedModule.ModuleIndex);
                Added.Add(MakeShared<FJsonValueObject>(Entry));
            }
            Result->SetArrayField(TEXT("added"), Added);
            // A batch add is all-or-nothing, so a successful response never has failures.
            Result->SetArrayField(TEXT("failed"), TArray<TSharedPtr<FJsonValue>>());
        }
        if (!Op.NameBefore.IsEmpty() || !Op.NameAfter.IsEmpty())
        {
            Result->SetStringField(TEXT("name_before"), Op.NameBefore);
            Result->SetStringField(TEXT("name_after"), Op.NameAfter);
        }
        if (Op.bLODEnabledSet)
        {
            Result->SetBoolField(TEXT("lod_enabled"), Op.LODEnabled);
        }
        if (Op.ModuleCounts.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Counts;
            for (const int32 Count : Op.ModuleCounts)
            {
                Counts.Add(MakeShared<FJsonValueNumber>(Count));
            }
            Result->SetArrayField(TEXT("module_counts"), Counts);
        }
        if (Op.ClosedEditors.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Closed;
            for (const FString& Editor : Op.ClosedEditors)
            {
                Closed.Add(MakeShared<FJsonValueString>(Editor));
            }
            Result->SetArrayField(TEXT("closed_editors"), Closed);
        }
        if (Op.AdjustedEmitters.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Adjusted;
            for (const int32 AdjustedIndex : Op.AdjustedEmitters)
            {
                Adjusted.Add(MakeShared<FJsonValueNumber>(AdjustedIndex));
            }
            Result->SetArrayField(TEXT("adjusted_emitters"), Adjusted);
        }
        if (Op.Module.ModuleClass.IsEmpty() == false)
        {
            Result->SetObjectField(TEXT("module"), ModuleInfoToJson(Op.Module));
        }
        Result->SetStringField(TEXT("detail"), CurrentResponseOptions().bSummary ? DetailSummary : DetailFull);
        AddEmitterReadback(Result, Op.Emitters);
        return Result;
    }

    /** Parse the distribution keys array: [{time, value, interp}]. */
    bool ReadDistributionKeys(const TSharedPtr<FJsonObject>& Params, TArray<FUnrealMCPParticleDistributionKey>& OutKeys,
                              FString& OutErrorMessage)
    {
        OutKeys.Reset();
        OutErrorMessage.Reset();

        if (!Params.IsValid() || !Params->HasField(TEXT("keys")))
        {
            return true;
        }

        const TArray<TSharedPtr<FJsonValue>>* KeyValues = nullptr;
        if (!Params->TryGetArrayField(TEXT("keys"), KeyValues) || !KeyValues)
        {
            OutErrorMessage = TEXT("'keys' must be an array of {time, value, interp} objects");
            return false;
        }

        for (const TSharedPtr<FJsonValue>& KeyValue : *KeyValues)
        {
            const TSharedPtr<FJsonObject>* KeyObject = nullptr;
            if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObject) || !KeyObject)
            {
                OutErrorMessage = TEXT("'keys' entries must be objects with 'time' and 'value'");
                return false;
            }

            FUnrealMCPParticleDistributionKey Key;
            double TimeValue = 0.0;
            if (!(*KeyObject)->TryGetNumberField(TEXT("time"), TimeValue))
            {
                OutErrorMessage = TEXT("each key needs a numeric 'time'");
                return false;
            }
            Key.Time = static_cast<float>(TimeValue);

            const TArray<TSharedPtr<FJsonValue>>* Numbers = nullptr;
            if ((*KeyObject)->TryGetArrayField(TEXT("value"), Numbers) && Numbers)
            {
                for (const TSharedPtr<FJsonValue>& Number : *Numbers)
                {
                    if (!Number.IsValid() || Number->Type != EJson::Number)
                    {
                        OutErrorMessage = TEXT("each key 'value' must be an array of numbers");
                        return false;
                    }
                    Key.Value.Add(static_cast<float>(Number->AsNumber()));
                }
            }
            else
            {
                double Single = 0.0;
                if ((*KeyObject)->TryGetNumberField(TEXT("value"), Single))
                {
                    Key.Value.Add(static_cast<float>(Single));
                }
            }

            (*KeyObject)->TryGetStringField(TEXT("interp"), Key.Interp);
            OutKeys.Add(MoveTemp(Key));
        }
        return true;
    }

    /** Parse the flat values array of a distribution write. */
    bool ReadDistributionValues(const TSharedPtr<FJsonObject>& Params, TArray<float>& OutValues,
                                FString& OutErrorMessage)
    {
        OutValues.Reset();
        OutErrorMessage.Reset();

        if (!Params.IsValid() || !Params->HasField(TEXT("values")))
        {
            return true;
        }

        const TSharedPtr<FJsonValue> Value = Params->Values.FindRef(TEXT("values"));
        if (!Value.IsValid())
        {
            return true;
        }

        if (Value->Type == EJson::Number)
        {
            OutValues.Add(static_cast<float>(Value->AsNumber()));
            return true;
        }

        if (Value->Type == EJson::Array)
        {
            const TArray<TSharedPtr<FJsonValue>>* Numbers = nullptr;
            if (Value->TryGetArray(Numbers) && Numbers)
            {
                for (const TSharedPtr<FJsonValue>& Number : *Numbers)
                {
                    if (!Number.IsValid() || Number->Type != EJson::Number)
                    {
                        OutErrorMessage = TEXT("'values' must be numbers or an array of numbers");
                        return false;
                    }
                    OutValues.Add(static_cast<float>(Number->AsNumber()));
                }
                return true;
            }
        }

        OutErrorMessage = TEXT("'values' must be a number or an array of numbers");
        return false;
    }
}

FUnrealMCPParticleCommands::FUnrealMCPParticleCommands()
{
}

void FUnrealMCPParticleCommands::RegisterCommands(FMCPCommandRegistry& Registry)
{
    MCP_REGISTER_COMMAND(Registry, "create_particle_system", "particle",
        "Create a Cascade particle system asset with an optional starter emitter.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("name"), TEXT("string"), TEXT("Asset name")),
            MCPParamOpt(TEXT("folder"), TEXT("string"), TEXT("Content folder to create the asset in")),
            MCPParamOpt(TEXT("emitter_class"), TEXT("string"), TEXT("Starter emitter class")),
            MCPParamOpt(TEXT("lod_count"), TEXT("int"), TEXT("LOD level count; default 1")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("create_particle_system"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleCreateParticleSystem(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "list_particle_emitters", "particle",
        "List the emitters and their LOD levels in a particle system.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("asset_path"), TEXT("string"), TEXT("Particle system asset path")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("list_particle_emitters"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleListParticleEmitters(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "list_particle_modules", "particle",
        "List the modules of one emitter LOD level.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("asset_path"), TEXT("string"), TEXT("Particle system asset path")),
            MCPParam(TEXT("emitter_index"), TEXT("int"), TEXT("Emitter index")),
            MCPParamOpt(TEXT("lod_index"), TEXT("int"), TEXT("LOD level index; default 0")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("list_particle_modules"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleListParticleModules(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "get_particle_module", "particle",
        "Read one module's properties from an emitter LOD level.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("asset_path"), TEXT("string"), TEXT("Particle system asset path")),
            MCPParam(TEXT("emitter_index"), TEXT("int"), TEXT("Emitter index")),
            MCPParamOpt(TEXT("lod_index"), TEXT("int"), TEXT("LOD level index; default 0")),
            MCPParamOpt(TEXT("module_index"), TEXT("int"), TEXT("Module index within the LOD level")),
            MCPParamOpt(TEXT("module_class"), TEXT("string"), TEXT("Module class name to match")),
            MCPParamOpt(TEXT("slot"), TEXT("string"), TEXT("Module slot name to match")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_particle_module"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetParticleModule(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "validate_particle_system", "particle",
        "Run structural checks on a particle system and report issues.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("asset_path"), TEXT("string"), TEXT("Particle system asset path")),
            MCPParamOpt(TEXT("emitter_index"), TEXT("int"), TEXT("Emitter index; -1 for all")),
            MCPParamOpt(TEXT("checks"), TEXT("array"), TEXT("Check ids to run; defaults to all")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("validate_particle_system"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleValidateParticleSystem(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "add_particle_emitter", "particle",
        "Add a new emitter to a particle system.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("asset_path"), TEXT("string"), TEXT("Particle system asset path")),
            MCPParamOpt(TEXT("emitter_class"), TEXT("string"), TEXT("Emitter class to instantiate")),
            MCPParamOpt(TEXT("insert_index"), TEXT("int"), TEXT("Insertion position; -1 appends")),
            MCPParamOpt(TEXT("lod_count"), TEXT("int"), TEXT("LOD level count; -1 adopts the system count")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_particle_emitter"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddParticleEmitter(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "remove_particle_emitter", "particle",
        "Remove an emitter from a particle system.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("asset_path"), TEXT("string"), TEXT("Particle system asset path")),
            MCPParam(TEXT("emitter_index"), TEXT("int"), TEXT("Emitter index")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("remove_particle_emitter"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRemoveParticleEmitter(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "add_particle_module", "particle",
        "Add a module to an emitter LOD level, by class name, class list or copied from another emitter.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("asset_path"), TEXT("string"), TEXT("Particle system asset path")),
            MCPParam(TEXT("emitter_index"), TEXT("int"), TEXT("Emitter index")),
            MCPParamOpt(TEXT("module_class"), TEXT("string"), TEXT("Single module class to add")),
            MCPParamOpt(TEXT("module_classes"), TEXT("array"), TEXT("Module class names to add")),
            MCPParamOpt(TEXT("copy_modules_from_emitter"), TEXT("int"), TEXT("Emitter index to copy modules from")),
            MCPParamOpt(TEXT("lod_index"), TEXT("int"), TEXT("LOD level index; default 0")),
            MCPParamOpt(TEXT("insert_index"), TEXT("int"), TEXT("Insertion position; -1 appends")),
            MCPParamOpt(TEXT("slot"), TEXT("string"), TEXT("Target module slot")),
            MCPParamOpt(TEXT("hide_sprite"), TEXT("bool"), TEXT("Hide the sprite when adding a Sprite module")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_particle_module"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddParticleModule(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "move_particle_module", "particle",
        "Reorder a module within an emitter LOD level.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("asset_path"), TEXT("string"), TEXT("Particle system asset path")),
            MCPParam(TEXT("emitter_index"), TEXT("int"), TEXT("Emitter index")),
            MCPParamOpt(TEXT("module_index"), TEXT("int"), TEXT("Module index to move")),
            MCPParamOpt(TEXT("module_class"), TEXT("string"), TEXT("Module class name to move")),
            MCPParamOpt(TEXT("lod_index"), TEXT("int"), TEXT("LOD level index; default 0")),
            MCPParam(TEXT("to_index"), TEXT("int"), TEXT("Zero-based target position in the evaluation order")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("move_particle_module"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleMoveParticleModule(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "duplicate_particle_emitter", "particle",
        "Duplicate an emitter, optionally renaming the copy.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("asset_path"), TEXT("string"), TEXT("Particle system asset path")),
            MCPParam(TEXT("emitter_index"), TEXT("int"), TEXT("Emitter index to duplicate")),
            MCPParamOpt(TEXT("name"), TEXT("string"), TEXT("Name for the duplicated emitter")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("duplicate_particle_emitter"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleDuplicateParticleEmitter(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "set_particle_emitter_name", "particle",
        "Rename an emitter.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("asset_path"), TEXT("string"), TEXT("Particle system asset path")),
            MCPParam(TEXT("emitter_index"), TEXT("int"), TEXT("Emitter index")),
            MCPParamOpt(TEXT("name"), TEXT("string"), TEXT("New emitter name")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_particle_emitter_name"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetParticleEmitterName(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "set_particle_lod_enabled", "particle",
        "Enable or disable one emitter LOD level.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("asset_path"), TEXT("string"), TEXT("Particle system asset path")),
            MCPParam(TEXT("emitter_index"), TEXT("int"), TEXT("Emitter index")),
            MCPParam(TEXT("lod_index"), TEXT("int"), TEXT("LOD level index")),
            MCPParam(TEXT("enabled"), TEXT("bool"), TEXT("Whether the LOD level is enabled")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_particle_lod_enabled"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetParticleLODEnabled(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "remove_particle_module", "particle",
        "Remove a module from an emitter LOD level.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("asset_path"), TEXT("string"), TEXT("Particle system asset path")),
            MCPParam(TEXT("emitter_index"), TEXT("int"), TEXT("Emitter index")),
            MCPParamOpt(TEXT("module_class"), TEXT("string"), TEXT("Module class name to remove")),
            MCPParamOpt(TEXT("module_index"), TEXT("int"), TEXT("Module index to remove")),
            MCPParamOpt(TEXT("lod_index"), TEXT("int"), TEXT("LOD level index; default 0")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("remove_particle_module"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRemoveParticleModule(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "set_particle_module_property", "particle",
        "Write one module property to the given JSON value.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("asset_path"), TEXT("string"), TEXT("Particle system asset path")),
            MCPParam(TEXT("emitter_index"), TEXT("int"), TEXT("Emitter index")),
            MCPParamOpt(TEXT("module_class"), TEXT("string"), TEXT("Module class name")),
            MCPParamOpt(TEXT("module_index"), TEXT("int"), TEXT("Module index")),
            MCPParamOpt(TEXT("lod_index"), TEXT("int"), TEXT("LOD level index; default 0")),
            MCPParamOpt(TEXT("property_name"), TEXT("string"), TEXT("Property name to write")),
            MCPParam(TEXT("property_value"), TEXT("object"), TEXT("JSON value to assign to the property")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_particle_module_property"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetParticleModuleProperty(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "set_particle_bursts", "particle",
        "Replace an emitter LOD level's burst list.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("asset_path"), TEXT("string"), TEXT("Particle system asset path")),
            MCPParam(TEXT("emitter_index"), TEXT("int"), TEXT("Emitter index")),
            MCPParamOpt(TEXT("lod_index"), TEXT("int"), TEXT("LOD level index; default 0")),
            MCPParam(TEXT("bursts"), TEXT("array"), TEXT("Array of {count, time, count_low?} objects")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_particle_bursts"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetParticleBursts(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "set_particle_distribution", "particle",
        "Write a distribution (kind plus values or keys) to a module property.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("asset_path"), TEXT("string"), TEXT("Particle system asset path")),
            MCPParam(TEXT("emitter_index"), TEXT("int"), TEXT("Emitter index")),
            MCPParamOpt(TEXT("module_class"), TEXT("string"), TEXT("Module class name")),
            MCPParamOpt(TEXT("module_index"), TEXT("int"), TEXT("Module index")),
            MCPParamOpt(TEXT("lod_index"), TEXT("int"), TEXT("LOD level index; default 0")),
            MCPParam(TEXT("property_name"), TEXT("string"), TEXT("Distribution property name")),
            MCPParam(TEXT("kind"), TEXT("string"), TEXT("Distribution kind (constant/uniform/curve)")),
            MCPParamOpt(TEXT("values"), TEXT("array"), TEXT("Flat distribution values")),
            MCPParamOpt(TEXT("keys"), TEXT("array"), TEXT("Curve keys [{time, value, interp}]")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_particle_distribution"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetParticleDistribution(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "set_particle_lod_count", "particle",
        "Add or remove LOD levels of an emitter.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("asset_path"), TEXT("string"), TEXT("Particle system asset path")),
            MCPParam(TEXT("emitter_index"), TEXT("int"), TEXT("Emitter index")),
            MCPParam(TEXT("lod_count"), TEXT("int"), TEXT("New LOD level count")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_particle_lod_count"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetParticleLODCount(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "copy_particle_lod", "particle",
        "Copy an emitter LOD level to a new index.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("asset_path"), TEXT("string"), TEXT("Particle system asset path")),
            MCPParam(TEXT("emitter_index"), TEXT("int"), TEXT("Emitter index")),
            MCPParam(TEXT("source_lod_index"), TEXT("int"), TEXT("LOD level index to copy")),
            MCPParamOpt(TEXT("insert_index"), TEXT("int"), TEXT("Insertion position; -1 appends")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("copy_particle_lod"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleCopyParticleLOD(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "set_particle_lod_distance", "particle",
        "Set the distance threshold of one emitter LOD level.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("asset_path"), TEXT("string"), TEXT("Particle system asset path")),
            MCPParam(TEXT("emitter_index"), TEXT("int"), TEXT("Emitter index")),
            MCPParam(TEXT("lod_index"), TEXT("int"), TEXT("LOD level index")),
            MCPParam(TEXT("distance"), TEXT("float"), TEXT("Distance threshold")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_particle_lod_distance"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetParticleLODDistance(P); }); }));}

TSharedPtr<FJsonObject> FUnrealMCPParticleCommands::RunCommand(const FString& CommandType,
                                                              const TSharedPtr<FJsonObject>& Params,
                                                              const TFunction<TSharedPtr<FJsonObject>(const TSharedPtr<FJsonObject>&)>& Body)
{
    // One MCP command = one undo step, so a wrong structural edit can be taken back with Ctrl+Z.
    // Which commands mutate comes from their registry flag (read-only ones stay out of the stack).
    const FMCPCommandEntry* Entry = FMCPCommandRegistry::Get().Find(CommandType);
    const bool bMutating = Entry && Entry->Flags.bMutatesGraph;

    // Response shaping is resolved up front: an unusable "detail" / "fields" must be rejected
    // before anything is modified, not after the write already happened.
    FString RequestedDetail = GetStringParam(Params, TEXT("detail"));
    if (RequestedDetail.IsEmpty() && Params.IsValid())
    {
        bool bReturnState = true;
        if (Params->TryGetBoolField(TEXT("return_state"), bReturnState) && !bReturnState)
        {
            RequestedDetail = DetailSummary;
        }
    }

    TArray<FString> RequestedFields;
    FString FieldsError;
    if (!ReadRequestedFields(Params, RequestedFields, FieldsError))
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::UnknownResponseField, FieldsError,
            KnownResponseFields());
    }
    if (RequestedDetail.IsEmpty() &&
        (RequestedFields.Contains(TEXT("emitters")) || RequestedFields.Contains(TEXT("emitter_count"))))
    {
        // Asking for the tree explicitly overrides the write command default.
        RequestedDetail = DetailFull;
    }

    FString Detail;
    FString DetailError;
    if (!ResolveDetailParam(RequestedDetail, bMutating, Detail, DetailError))
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidDetail, DetailError, DetailCandidates());
    }

    FParticleResponseOptions Options;
    Options.bSummary = Detail == DetailSummary;
    if (Params.IsValid())
    {
        bool bIncludeModules = true;
        if (Params->TryGetBoolField(TEXT("include_modules"), bIncludeModules))
        {
            Options.bIncludeModules = bIncludeModules;
        }
    }
    FScopedResponseOptions ScopedOptions(Options);

    // auto_close: the session guard refuses a structural write while the asset editor is open,
    // because the editor keeps a stale view of the arrays. Closing that editor first is exactly
    // what a caller would have to do by hand, so it can be opted into instead of remembered.
    TArray<FString> ClosedEditors;
    bool bAutoClose = false;
    if (bMutating && Params.IsValid() && Params->TryGetBoolField(TEXT("auto_close"), bAutoClose) && bAutoClose)
    {
        const FString AutoCloseAssetPath = GetAssetPathParam(Params);
        if (!AutoCloseAssetPath.IsEmpty())
        {
            if (UObject* Asset = FUnrealMCPCommonUtils::FindAsset(AutoCloseAssetPath))
            {
                const int32 ClosedCount = FUnrealMCPEditorCommands::CloseAssetEditorsFor(Asset);
                for (int32 Index = 0; Index < ClosedCount; ++Index)
                {
                    ClosedEditors.Add(Asset->GetPathName());
                }
            }
        }
    }

    TUniquePtr<FScopedTransaction> Transaction;
    if (bMutating)
    {
        Transaction = MakeUnique<FScopedTransaction>(
            FText::FromString(FString::Printf(TEXT("UnrealMCP %s"), *CommandType)));
    }

    TSharedPtr<FJsonObject> Result = Body(Params);

    const bool bSuccess = FUnrealMCPCommonUtils::ResponseIndicatesSuccess(Result);
    if (bMutating && !bSuccess && Transaction.IsValid())
    {
        Transaction->Cancel();
    }
    Transaction.Reset();

    // Persist immediately: the editor is routinely killed by the build script. The kernel
    // saves too; saving an already-saved asset is a no-op.
    // An explicit `persist=false` lets a batch of writes be flushed once by the caller instead of
    // writing the asset per command.
    bool bPersist = true;
    Params->TryGetBoolField(TEXT("persist"), bPersist);

    bool bSaved = false;
    if (bMutating && bSuccess && bPersist)
    {
        const FString AssetPath = GetAssetPathParam(Params);
        if (!AssetPath.IsEmpty())
        {
            if (UObject* Asset = FUnrealMCPCommonUtils::FindAsset(AssetPath))
            {
                bSaved = FUnrealMCPCommonUtils::SaveAssetForObject(Asset);
            }
        }
    }

    if (bMutating && bSuccess && Result.IsValid())
    {
        if (!Result->HasField(TEXT("saved")))
        {
            Result->SetBoolField(TEXT("saved"), bSaved);
        }
        if (!Result->HasField(TEXT("persist_requested")))
        {
            Result->SetBoolField(TEXT("persist_requested"), bPersist);
        }
    }

    if (bSuccess)
    {
        if (ClosedEditors.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> ClosedJson;
            for (const FString& ClosedEditor : ClosedEditors)
            {
                ClosedJson.Add(MakeShared<FJsonValueString>(ClosedEditor));
            }
            Result->SetArrayField(TEXT("closed_editors"), ClosedJson);
        }
        ApplyFieldProjection(Result, RequestedFields);
    }

    return Result;
}

// ---------------------------------------------------------------------------
// Reads
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPParticleCommands::HandleListParticleEmitters(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = GetAssetPathParam(Params);

    UParticleSystem* System = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!FUnrealMCPParticleOps::ResolveParticleSystem(AssetPath, System, ErrorCode, ErrorMessage))
    {
        return MakeParticleErrorJson(ErrorCode, ErrorMessage, TArray<FString>());
    }

    TArray<FUnrealMCPParticleEmitterInfo> Emitters;
    TArray<FString> Candidates;
    if (!FUnrealMCPParticleOps::ListEmitters(System, Emitters, ErrorCode, ErrorMessage, Candidates))
    {
        return MakeParticleErrorJson(ErrorCode, ErrorMessage, Candidates);
    }

    FUnrealMCPParticleListResult ListResult;
    ListResult.Success = true;
    ListResult.AssetPath = System->GetPathName();
    ListResult.EmitterCount = Emitters.Num();
    ListResult.Emitters = MoveTemp(Emitters);
    return ListResultToJson(ListResult);
}

TSharedPtr<FJsonObject> FUnrealMCPParticleCommands::HandleListParticleModules(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = GetAssetPathParam(Params);
    const int32 EmitterIndex = GetIntParam(Params, TEXT("emitter_index"), -1);
    const int32 LODIndex = GetIntParam(Params, TEXT("lod_index"), 0);

    if (EmitterIndex < 0)
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            TEXT("Missing 'emitter_index' parameter"), TArray<FString>());
    }

    UParticleSystem* System = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!FUnrealMCPParticleOps::ResolveParticleSystem(AssetPath, System, ErrorCode, ErrorMessage))
    {
        return MakeParticleErrorJson(ErrorCode, ErrorMessage, TArray<FString>());
    }

    TArray<FString> Candidates;
    UParticleEmitter* Emitter = nullptr;
    if (!FUnrealMCPParticleOps::ResolveEmitter(System, EmitterIndex, Emitter, ErrorCode, ErrorMessage, Candidates))
    {
        return MakeParticleErrorJson(ErrorCode, ErrorMessage, Candidates);
    }

    UParticleLODLevel* LODLevel = nullptr;
    if (!FUnrealMCPParticleOps::ResolveLODLevel(Emitter, LODIndex, LODLevel, ErrorCode, ErrorMessage, Candidates))
    {
        return MakeParticleErrorJson(ErrorCode, ErrorMessage, Candidates);
    }

    FUnrealMCPParticleEmitterInfo EmitterInfo;
    FUnrealMCPParticleOps::MakeEmitterInfo(System, EmitterIndex, EmitterInfo);
    if (EmitterInfo.Lods.IsValidIndex(LODIndex))
    {
        TArray<FUnrealMCPParticleLODInfo> SingleLOD;
        SingleLOD.Add(EmitterInfo.Lods[LODIndex]);
        EmitterInfo.Lods = MoveTemp(SingleLOD);
    }

    FUnrealMCPParticleListResult ListResult;
    ListResult.Success = true;
    ListResult.AssetPath = System->GetPathName();
    ListResult.EmitterCount = 1;
    ListResult.Emitters.Add(MoveTemp(EmitterInfo));
    return ListResultToJson(ListResult);
}

TSharedPtr<FJsonObject> FUnrealMCPParticleCommands::HandleGetParticleModule(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = GetAssetPathParam(Params);
    const int32 EmitterIndex = GetIntParam(Params, TEXT("emitter_index"), -1);
    const int32 LODIndex = GetIntParam(Params, TEXT("lod_index"), 0);
    const int32 ModuleIndex = GetIntParam(Params, TEXT("module_index"), -1);
    const FString ModuleClass = GetStringParam(Params, TEXT("module_class"));
    const FString Slot = GetStringParam(Params, TEXT("slot"));

    if (EmitterIndex < 0)
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            TEXT("Missing 'emitter_index' parameter"), TArray<FString>());
    }

    UParticleSystem* System = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!FUnrealMCPParticleOps::ResolveParticleSystem(AssetPath, System, ErrorCode, ErrorMessage))
    {
        return MakeParticleErrorJson(ErrorCode, ErrorMessage, TArray<FString>());
    }

    TArray<FString> Candidates;
    UParticleEmitter* Emitter = nullptr;
    if (!FUnrealMCPParticleOps::ResolveEmitter(System, EmitterIndex, Emitter, ErrorCode, ErrorMessage, Candidates))
    {
        return MakeParticleErrorJson(ErrorCode, ErrorMessage, Candidates);
    }

    UParticleModule* Module = nullptr;
    int32 ResolvedModuleIndex = -1;
    if (!FUnrealMCPParticleOps::ResolveModule(Emitter, LODIndex, ModuleClass, ModuleIndex, Slot, Module,
                                              ResolvedModuleIndex, ErrorCode, ErrorMessage, Candidates))
    {
        return MakeParticleErrorJson(ErrorCode, ErrorMessage, Candidates);
    }

    FUnrealMCPParticleModuleInfo ModuleInfo;
    FUnrealMCPParticleOps::MakeModuleInfo(Module, ResolvedModuleIndex,
        Slot.IsEmpty() ? FUnrealMCPParticleOps::DetermineModuleSlot(Emitter, LODIndex, Module) : Slot, ModuleInfo);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("asset_path"), System->GetPathName());
    Result->SetNumberField(TEXT("emitter_index"), EmitterIndex);
    Result->SetNumberField(TEXT("lod_index"), LODIndex);
    Result->SetObjectField(TEXT("module"), ModuleInfoToJson(ModuleInfo));
    return Result;
}

// ---------------------------------------------------------------------------
// Writes
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPParticleCommands::HandleCreateParticleSystem(const TSharedPtr<FJsonObject>& Params)
{
    const FString Name = GetStringParam(Params, TEXT("name"));
    const FString Folder = GetStringParam(Params, TEXT("folder"));
    const FString EmitterClass = GetStringParam(Params, TEXT("emitter_class"));
    const int32 LODCount = GetIntParam(Params, TEXT("lod_count"), 1);

    FUnrealMCPParticleOpResult OpResult;
    FUnrealMCPParticleOps::CreateParticleSystem(Name, Folder, EmitterClass, LODCount, OpResult);
    return OpResultToJson(OpResult);
}

TSharedPtr<FJsonObject> FUnrealMCPParticleCommands::HandleAddParticleEmitter(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = GetAssetPathParam(Params);
    const FString EmitterClass = GetStringParam(Params, TEXT("emitter_class"));
    const int32 InsertIndex = GetIntParam(Params, TEXT("insert_index"), -1);
    // -1 (or any value <= 0) means "adopt the system's current LOD count"; a positive value is an
    // explicit request that brings the other emitters along.
    const int32 LODCount = GetIntParam(Params, TEXT("lod_count"), -1);

    FUnrealMCPParticleOpResult OpResult;
    FUnrealMCPParticleOps::AddEmitter(AssetPath, EmitterClass, InsertIndex, LODCount, OpResult);
    return OpResultToJson(OpResult);
}

TSharedPtr<FJsonObject> FUnrealMCPParticleCommands::HandleRemoveParticleEmitter(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = GetAssetPathParam(Params);
    const int32 EmitterIndex = GetIntParam(Params, TEXT("emitter_index"), -1);

    if (EmitterIndex < 0)
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            TEXT("Missing 'emitter_index' parameter"), TArray<FString>());
    }

    FUnrealMCPParticleOpResult OpResult;
    FUnrealMCPParticleOps::RemoveEmitter(AssetPath, EmitterIndex, OpResult);
    return OpResultToJson(OpResult);
}

TSharedPtr<FJsonObject> FUnrealMCPParticleCommands::HandleAddParticleModule(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = GetAssetPathParam(Params);
    const int32 EmitterIndex = GetIntParam(Params, TEXT("emitter_index"), -1);
    const FString ModuleClass = GetStringParam(Params, TEXT("module_class"));
    const int32 LODIndex = GetIntParam(Params, TEXT("lod_index"), 0);
    const int32 InsertIndex = GetIntParam(Params, TEXT("insert_index"), -1);
    const FString Slot = GetStringParam(Params, TEXT("slot"));

    bool bHideSprite = false;
    if (Params.IsValid())
    {
        Params->TryGetBoolField(TEXT("hide_sprite"), bHideSprite);
    }

    if (EmitterIndex < 0)
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            TEXT("Missing 'emitter_index' parameter"), TArray<FString>());
    }

    // Three mutually exclusive shapes: one class, a list of classes, or another emitter's set.
    TArray<FString> ModuleClasses;
    if (Params.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* ModuleClassValues = nullptr;
        if (Params->TryGetArrayField(TEXT("module_classes"), ModuleClassValues) && ModuleClassValues)
        {
            for (const TSharedPtr<FJsonValue>& Value : *ModuleClassValues)
            {
                if (!Value.IsValid() || Value->Type != EJson::String)
                {
                    return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
                        TEXT("'module_classes' entries must be strings"), TArray<FString>());
                }
                ModuleClasses.Add(Value->AsString());
            }
        }
    }

    const int32 CopyFromEmitter = GetIntParam(Params, TEXT("copy_modules_from_emitter"), -1);

    int32 ShapeCount = 0;
    if (!ModuleClass.IsEmpty()) { ++ShapeCount; }
    if (ModuleClasses.Num() > 0) { ++ShapeCount; }
    if (CopyFromEmitter >= 0) { ++ShapeCount; }
    if (ShapeCount != 1)
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            TEXT("Pass exactly one of 'module_class', 'module_classes' or 'copy_modules_from_emitter'"),
            TArray<FString>{ TEXT("module_class"), TEXT("module_classes"), TEXT("copy_modules_from_emitter") });
    }

    FUnrealMCPParticleOpResult OpResult;
    if (!ModuleClasses.IsEmpty())
    {
        FUnrealMCPParticleOps::AddModules(AssetPath, EmitterIndex, ModuleClasses, LODIndex, InsertIndex, Slot,
                                          bHideSprite, OpResult);
    }
    else if (CopyFromEmitter >= 0)
    {
        FUnrealMCPParticleOps::AddModulesFromEmitter(AssetPath, EmitterIndex, CopyFromEmitter, LODIndex, OpResult);
    }
    else
    {
        FUnrealMCPParticleOps::AddModule(AssetPath, EmitterIndex, ModuleClass, LODIndex, InsertIndex, Slot,
                                         bHideSprite, OpResult);
    }
    return OpResultToJson(OpResult);
}

TSharedPtr<FJsonObject> FUnrealMCPParticleCommands::HandleMoveParticleModule(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = GetAssetPathParam(Params);
    const int32 EmitterIndex = GetIntParam(Params, TEXT("emitter_index"), -1);
    const int32 ModuleIndex = GetIntParam(Params, TEXT("module_index"), -1);
    const int32 LODIndex = GetIntParam(Params, TEXT("lod_index"), 0);
    const FString ModuleClass = GetStringParam(Params, TEXT("module_class"));

    if (EmitterIndex < 0)
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            TEXT("Missing 'emitter_index' parameter"));
    }
    if (ModuleClass.IsEmpty() && ModuleIndex < 0)
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            TEXT("Pass 'module_class' or 'module_index'"));
    }
    if (!Params.IsValid() || !Params->HasField(TEXT("to_index")))
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            TEXT("Missing 'to_index' parameter (0 based position in the evaluation order)"));
    }

    const int32 ToIndex = GetIntParam(Params, TEXT("to_index"), -1);

    FUnrealMCPParticleOpResult OpResult;
    FUnrealMCPParticleOps::MoveModule(AssetPath, EmitterIndex, ModuleClass, ModuleIndex, LODIndex, ToIndex, OpResult);
    return OpResultToJson(OpResult);
}

TSharedPtr<FJsonObject> FUnrealMCPParticleCommands::HandleSetParticleEmitterName(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = GetAssetPathParam(Params);
    const int32 EmitterIndex = GetIntParam(Params, TEXT("emitter_index"), -1);
    const FString Name = GetStringParam(Params, TEXT("name"));

    if (EmitterIndex < 0)
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            TEXT("Missing 'emitter_index' parameter"));
    }

    FUnrealMCPParticleOpResult OpResult;
    FUnrealMCPParticleOps::SetEmitterName(AssetPath, EmitterIndex, Name, OpResult);
    return OpResultToJson(OpResult);
}

TSharedPtr<FJsonObject> FUnrealMCPParticleCommands::HandleSetParticleLODEnabled(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = GetAssetPathParam(Params);
    const int32 EmitterIndex = GetIntParam(Params, TEXT("emitter_index"), -1);
    const int32 LODIndex = GetIntParam(Params, TEXT("lod_index"), -1);

    bool bEnabled = true;
    if (!Params.IsValid() || !Params->TryGetBoolField(TEXT("enabled"), bEnabled))
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            TEXT("Missing 'enabled' parameter (true or false)"));
    }

    if (EmitterIndex < 0)
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            TEXT("Missing 'emitter_index' parameter"));
    }
    if (LODIndex < 0)
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            TEXT("Missing 'lod_index' parameter"));
    }

    FUnrealMCPParticleOpResult OpResult;
    FUnrealMCPParticleOps::SetLODEnabled(AssetPath, EmitterIndex, LODIndex, bEnabled, OpResult);
    return OpResultToJson(OpResult);
}

TSharedPtr<FJsonObject> FUnrealMCPParticleCommands::HandleDuplicateParticleEmitter(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = GetAssetPathParam(Params);
    const int32 EmitterIndex = GetIntParam(Params, TEXT("emitter_index"), -1);
    const FString Name = GetStringParam(Params, TEXT("name"));

    if (EmitterIndex < 0)
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            TEXT("Missing 'emitter_index' parameter"));
    }

    FUnrealMCPParticleOpResult OpResult;
    FUnrealMCPParticleOps::DuplicateEmitter(AssetPath, EmitterIndex, Name, OpResult);
    return OpResultToJson(OpResult);
}

TSharedPtr<FJsonObject> FUnrealMCPParticleCommands::HandleSetParticleBursts(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = GetAssetPathParam(Params);
    const int32 EmitterIndex = GetIntParam(Params, TEXT("emitter_index"), -1);
    const int32 LODIndex = GetIntParam(Params, TEXT("lod_index"), 0);

    if (EmitterIndex < 0)
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            TEXT("Missing 'emitter_index' parameter"), TArray<FString>());
    }

    // Accepted entry keys; anything else is a typo worth reporting rather than a silent default.
    static const TArray<FString> BurstKeys = { TEXT("count"), TEXT("count_low"), TEXT("time") };

    // This command parses the burst entries itself (they feed an engine struct directly), so an entry
    // that cannot be parsed has to report what the shared writer would have reported: which element is
    // wrong, the fields that would have been accepted, and that nothing was written.
    const auto BurstFailure = [](const FString& Message, int32 FailedIndex)
    {
        TSharedPtr<FJsonObject> Error = MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            Message, TArray<FString>(), BurstKeys, FailedIndex);
        Error->SetBoolField(TEXT("unchanged"), true);
        return Error;
    };

    const TArray<TSharedPtr<FJsonValue>>* BurstValues = nullptr;
    if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("bursts"), BurstValues) || !BurstValues)
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            TEXT("Missing 'bursts' parameter: an array of {count, time, count_low?} objects"), BurstKeys);
    }

    TArray<FUnrealMCPParticleBurst> Bursts;
    for (int32 Index = 0; Index < BurstValues->Num(); ++Index)
    {
        const TSharedPtr<FJsonObject>* BurstObject = nullptr;
        if (!(*BurstValues)[Index].IsValid() || !(*BurstValues)[Index]->TryGetObject(BurstObject) || !BurstObject)
        {
            return BurstFailure(FString::Printf(TEXT("'bursts[%d]' must be an object with 'count' and 'time'"), Index), Index);
        }

        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*BurstObject)->Values)
        {
            if (!BurstKeys.Contains(Pair.Key))
            {
                return BurstFailure(FString::Printf(TEXT("Unknown burst field '%s' in 'bursts[%d]'"), *Pair.Key, Index), Index);
            }
        }

        FUnrealMCPParticleBurst Burst;
        double Number = 0.0;
        if (!(*BurstObject)->TryGetNumberField(TEXT("count"), Number))
        {
            return BurstFailure(FString::Printf(TEXT("'bursts[%d].count' must be a number"), Index), Index);
        }
        Burst.Count = static_cast<int32>(Number);

        if (!(*BurstObject)->TryGetNumberField(TEXT("time"), Number))
        {
            return BurstFailure(FString::Printf(TEXT("'bursts[%d].time' must be a number"), Index), Index);
        }
        Burst.Time = static_cast<float>(Number);

        if ((*BurstObject)->TryGetNumberField(TEXT("count_low"), Number))
        {
            Burst.CountLow = static_cast<int32>(Number);
        }
        Bursts.Add(Burst);
    }

    FUnrealMCPParticleOpResult OpResult;
    FUnrealMCPParticleOps::SetParticleBursts(AssetPath, EmitterIndex, LODIndex, Bursts, OpResult);
    return OpResultToJson(OpResult);
}

TSharedPtr<FJsonObject> FUnrealMCPParticleCommands::HandleValidateParticleSystem(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = GetAssetPathParam(Params);
    const int32 EmitterIndex = GetIntParam(Params, TEXT("emitter_index"), -1);

    TArray<FString> CheckIds;
    if (Params.IsValid() && Params->HasField(TEXT("checks")))
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Params->TryGetArrayField(TEXT("checks"), Values) || !Values)
        {
            return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
                TEXT("'checks' must be an array of check ids"), FUnrealMCPParticleOps::SupportedChecks());
        }

        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            if (!Value.IsValid() || Value->Type != EJson::String)
            {
                return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
                    TEXT("'checks' entries must be check id strings"), FUnrealMCPParticleOps::SupportedChecks());
            }
            CheckIds.Add(Value->AsString());
        }
    }

    FUnrealMCPParticleListResult ListResult;
    if (!FUnrealMCPParticleOps::ValidateParticleSystem(AssetPath, EmitterIndex, CheckIds, ListResult))
    {
        return MakeParticleErrorJson(ListResult.ErrorCode, ListResult.ErrorMessage, ListResult.Candidates);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("asset_path"), ListResult.AssetPath);
    Result->SetStringField(TEXT("detail"), CurrentResponseOptions().bSummary ? DetailSummary : DetailFull);
    Result->SetNumberField(TEXT("emitter_count"), ListResult.EmitterCount);
    Result->SetNumberField(TEXT("check_count"), ListResult.Checks.Num());

    int32 IssueCount = 0;
    TArray<TSharedPtr<FJsonValue>> ChecksJson;
    for (const FUnrealMCPParticleCheck& Check : ListResult.Checks)
    {
        if (Check.Severity != TEXT("info"))
        {
            ++IssueCount;
        }

        TSharedPtr<FJsonObject> CheckJson = MakeShared<FJsonObject>();
        CheckJson->SetStringField(TEXT("id"), Check.Id);
        CheckJson->SetStringField(TEXT("severity"), Check.Severity);
        CheckJson->SetStringField(TEXT("target"), Check.Target);
        CheckJson->SetStringField(TEXT("message"), Check.Message);
        CheckJson->SetStringField(TEXT("fix_hint"), Check.FixHint);
        ChecksJson.Add(MakeShared<FJsonValueObject>(CheckJson));
    }

    Result->SetNumberField(TEXT("issue_count"), IssueCount);
    Result->SetArrayField(TEXT("checks"), ChecksJson);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPParticleCommands::HandleRemoveParticleModule(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = GetAssetPathParam(Params);
    const int32 EmitterIndex = GetIntParam(Params, TEXT("emitter_index"), -1);
    const FString ModuleClass = GetStringParam(Params, TEXT("module_class"));
    const int32 ModuleIndex = GetIntParam(Params, TEXT("module_index"), -1);
    const int32 LODIndex = GetIntParam(Params, TEXT("lod_index"), 0);

    if (EmitterIndex < 0)
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            TEXT("Missing 'emitter_index' parameter"), TArray<FString>());
    }

    FUnrealMCPParticleOpResult OpResult;
    FUnrealMCPParticleOps::RemoveModule(AssetPath, EmitterIndex, ModuleClass, ModuleIndex, LODIndex, OpResult);
    return OpResultToJson(OpResult);
}

TSharedPtr<FJsonObject> FUnrealMCPParticleCommands::HandleSetParticleModuleProperty(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = GetAssetPathParam(Params);
    const int32 EmitterIndex = GetIntParam(Params, TEXT("emitter_index"), -1);
    const FString ModuleClass = GetStringParam(Params, TEXT("module_class"));
    const int32 ModuleIndex = GetIntParam(Params, TEXT("module_index"), -1);
    const int32 LODIndex = GetIntParam(Params, TEXT("lod_index"), 0);
    const FString PropertyName = GetStringParam(Params, TEXT("property_name"));

    if (EmitterIndex < 0)
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            TEXT("Missing 'emitter_index' parameter"), TArray<FString>());
    }
    if (!Params.IsValid() || !Params->HasField(TEXT("property_value")))
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            TEXT("Missing 'property_value' parameter"), TArray<FString>());
    }
    const TSharedPtr<FJsonValue> Value = Params->Values.FindRef(TEXT("property_value"));

    FUnrealMCPParticleOpResult OpResult;
    FUnrealMCPParticleOps::SetModuleProperty(AssetPath, EmitterIndex, ModuleClass, ModuleIndex, LODIndex,
        PropertyName, Value, OpResult);
    return OpResultToJson(OpResult);
}

TSharedPtr<FJsonObject> FUnrealMCPParticleCommands::HandleSetParticleDistribution(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = GetAssetPathParam(Params);
    const int32 EmitterIndex = GetIntParam(Params, TEXT("emitter_index"), -1);
    const FString ModuleClass = GetStringParam(Params, TEXT("module_class"));
    const int32 ModuleIndex = GetIntParam(Params, TEXT("module_index"), -1);
    const int32 LODIndex = GetIntParam(Params, TEXT("lod_index"), 0);
    const FString PropertyName = GetStringParam(Params, TEXT("property_name"));
    const FString Kind = GetStringParam(Params, TEXT("kind"));

    if (EmitterIndex < 0)
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            TEXT("Missing 'emitter_index' parameter"), TArray<FString>());
    }
    if (PropertyName.IsEmpty())
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            TEXT("Missing 'property_name' parameter"), TArray<FString>());
    }
    if (Kind.IsEmpty())
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            TEXT("Missing 'kind' parameter"), FUnrealMCPParticleOps::SupportedDistributionKinds());
    }

    TArray<float> Values;
    FString ValueErrorMessage;
    if (!ReadDistributionValues(Params, Values, ValueErrorMessage))
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidValue, ValueErrorMessage, TArray<FString>());
    }

    TArray<FUnrealMCPParticleDistributionKey> Keys;
    FString KeyErrorMessage;
    if (!ReadDistributionKeys(Params, Keys, KeyErrorMessage))
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidValue, KeyErrorMessage, TArray<FString>());
    }

    FUnrealMCPParticleOpResult OpResult;
    FUnrealMCPParticleOps::SetDistribution(AssetPath, EmitterIndex, ModuleClass, ModuleIndex, LODIndex,
        PropertyName, Kind, Values, Keys, OpResult);
    return OpResultToJson(OpResult);
}

TSharedPtr<FJsonObject> FUnrealMCPParticleCommands::HandleSetParticleLODCount(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = GetAssetPathParam(Params);
    const int32 EmitterIndex = GetIntParam(Params, TEXT("emitter_index"), -1);
    const int32 LODCount = GetIntParam(Params, TEXT("lod_count"), -1);

    if (EmitterIndex < 0)
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            TEXT("Missing 'emitter_index' parameter"), TArray<FString>());
    }
    if (LODCount < 1)
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            FString::Printf(TEXT("Missing or invalid 'lod_count' (%d)"), LODCount), TArray<FString>());
    }

    FUnrealMCPParticleOpResult OpResult;
    FUnrealMCPParticleOps::SetLODCount(AssetPath, EmitterIndex, LODCount, OpResult);
    return OpResultToJson(OpResult);
}

TSharedPtr<FJsonObject> FUnrealMCPParticleCommands::HandleCopyParticleLOD(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = GetAssetPathParam(Params);
    const int32 EmitterIndex = GetIntParam(Params, TEXT("emitter_index"), -1);
    const int32 SourceLODIndex = GetIntParam(Params, TEXT("source_lod_index"), -1);
    const int32 InsertIndex = GetIntParam(Params, TEXT("insert_index"), -1);

    if (EmitterIndex < 0)
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            TEXT("Missing 'emitter_index' parameter"), TArray<FString>());
    }
    if (SourceLODIndex < 0)
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            TEXT("Missing 'source_lod_index' parameter"), TArray<FString>());
    }

    FUnrealMCPParticleOpResult OpResult;
    FUnrealMCPParticleOps::CopyLOD(AssetPath, EmitterIndex, SourceLODIndex, InsertIndex, OpResult);
    return OpResultToJson(OpResult);
}

TSharedPtr<FJsonObject> FUnrealMCPParticleCommands::HandleSetParticleLODDistance(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = GetAssetPathParam(Params);
    const int32 EmitterIndex = GetIntParam(Params, TEXT("emitter_index"), -1);
    const int32 LODIndex = GetIntParam(Params, TEXT("lod_index"), -1);
    const float Distance = GetFloatParam(Params, TEXT("distance"), 0.0f);

    if (EmitterIndex < 0)
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            TEXT("Missing 'emitter_index' parameter"), TArray<FString>());
    }
    if (LODIndex < 0)
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            TEXT("Missing 'lod_index' parameter"), TArray<FString>());
    }
    if (!Params.IsValid() || !Params->HasField(TEXT("distance")))
    {
        return MakeParticleErrorJson(EUnrealMCPParticleError::InvalidParams,
            TEXT("Missing 'distance' parameter"), TArray<FString>());
    }

    FUnrealMCPParticleOpResult OpResult;
    FUnrealMCPParticleOps::SetLODDistance(AssetPath, EmitterIndex, LODIndex, Distance, OpResult);
    return OpResultToJson(OpResult);
}
