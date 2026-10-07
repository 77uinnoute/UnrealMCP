#include "Commands/Animation/UnrealMCPAnimationCommands.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Commands/UnrealMCPEditorCommands.h"
#include "Core/MCPCommandRegistry.h"
#include "Reflection/MCPPropertyReflector.h"

#include "Animation/AnimCurveMetadata.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimTypes.h"
#include "Animation/BlendSpace.h"
#include "Animation/Skeleton.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/IToolkitHost.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"
#include "Misc/Char.h"
#include "Misc/EngineVersion.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealMCPAnimationCommands, Log, All);

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

    bool GetBoolParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, bool bDefaultValue)
    {
        bool bValue = bDefaultValue;
        if (Params.IsValid() && Params->TryGetBoolField(FieldName, bValue))
        {
            return bValue;
        }
        return bDefaultValue;
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

    bool HasParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName)
    {
        if (Params.IsValid())
        {
            // TryGetNumberField accepts numbers stored as strings too; probe each concrete type
            // so "the caller passed time" is answered by the JSON, not by a default.
            return Params->HasField(FieldName);
        }
        return false;
    }

    TSharedPtr<FJsonValue> GetValueParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName)
    {
        if (Params.IsValid())
        {
            return Params->TryGetField(FieldName);
        }
        return nullptr;
    }

    // -----------------------------------------------------------------------
    // Value readers: the command layer reads the JSON shapes the tool surface
    // documents ([x,y,z], {roll,pitch,yaw}, {x,y,z,w}, hex colours).
    // -----------------------------------------------------------------------

    bool ParseVectorValue(const TSharedPtr<FJsonValue>& Value, FVector& OutVector, FString& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
        if (Value.IsValid() && Value->TryGetArray(Array) && Array && Array->Num() >= 3)
        {
            double X = 0.0;
            double Y = 0.0;
            double Z = 0.0;
            if ((*Array)[0]->TryGetNumber(X) && (*Array)[1]->TryGetNumber(Y) && (*Array)[2]->TryGetNumber(Z))
            {
                OutVector = FVector(X, Y, Z);
                return true;
            }
        }

        OutError = TEXT("value must be an [x,y,z] array");
        return false;
    }

    bool ParseRotationValue(const TSharedPtr<FJsonValue>& Value, FQuat& OutRotation, FString& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
        if (Value.IsValid() && Value->TryGetArray(Array) && Array)
        {
            TArray<double> Numbers;
            for (const TSharedPtr<FJsonValue>& Item : *Array)
            {
                double Number = 0.0;
                if (!Item->TryGetNumber(Number))
                {
                    OutError = TEXT("rotation array must hold numbers");
                    return false;
                }
                Numbers.Add(Number);
            }

            if (Numbers.Num() == 4)
            {
                OutRotation = FQuat(Numbers[0], Numbers[1], Numbers[2], Numbers[3]);
                OutRotation.Normalize();
                return true;
            }
            if (Numbers.Num() == 3)
            {
                // Euler degrees in roll/pitch/yaw order, like the sampled readback reports them.
                OutRotation = FRotator(Numbers[1], Numbers[2], Numbers[0]).Quaternion();
                return true;
            }

            OutError = TEXT("rotation array must hold 3 euler degrees or 4 quaternion components");
            return false;
        }

        const TSharedPtr<FJsonObject>* Object = nullptr;
        if (Value.IsValid() && Value->TryGetObject(Object) && Object)
        {
            double X = 0.0;
            double Y = 0.0;
            double Z = 0.0;
            double W = 0.0;
            if ((*Object)->TryGetNumberField(TEXT("x"), X) && (*Object)->TryGetNumberField(TEXT("y"), Y) &&
                (*Object)->TryGetNumberField(TEXT("z"), Z) && (*Object)->TryGetNumberField(TEXT("w"), W))
            {
                OutRotation = FQuat(X, Y, Z, W);
                OutRotation.Normalize();
                return true;
            }

            double Roll = 0.0;
            double Pitch = 0.0;
            double Yaw = 0.0;
            if ((*Object)->TryGetNumberField(TEXT("roll"), Roll) || (*Object)->TryGetNumberField(TEXT("pitch"), Pitch) ||
                (*Object)->TryGetNumberField(TEXT("yaw"), Yaw))
            {
                OutRotation = FRotator(Pitch, Yaw, Roll).Quaternion();
                return true;
            }

            OutError = TEXT("rotation object must hold x/y/z/w or roll/pitch/yaw");
            return false;
        }

        OutError = TEXT("rotation must be an array or an object");
        return false;
    }

    bool ParseColorValue(const TSharedPtr<FJsonValue>& Value, FLinearColor& OutColor, FString& OutError)
    {
        if (!Value.IsValid())
        {
            OutError = TEXT("color value is missing");
            return false;
        }

        if (Value->Type == EJson::String)
        {
            const FString Text = Value->AsString();
            // FColor::FromHex reads '#RGB' / '#RRGGBB' / '#RRGGBBAA' and turns anything else into
            // black without complaining, so the shape is validated before it is called.
            FString Hex = Text;
            if (Hex.StartsWith(TEXT("#")))
            {
                Hex.RightChopInline(1);
            }
            bool bValidHex = Hex.Len() == 3 || Hex.Len() == 6 || Hex.Len() == 8;
            for (int32 Index = 0; bValidHex && Index < Hex.Len(); ++Index)
            {
                bValidHex = FChar::IsHexDigit(Hex[Index]);
            }
            if (!bValidHex)
            {
                OutError = FString::Printf(TEXT("color '%s' is not a hex value like '#RRGGBB(AA)'"), *Text);
                return false;
            }

            OutColor = FLinearColor::FromSRGBColor(FColor::FromHex(Text));
            return true;
        }

        const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
        if (Value->TryGetArray(Array) && Array && Array->Num() >= 3)
        {
            TArray<double> Numbers;
            for (const TSharedPtr<FJsonValue>& Item : *Array)
            {
                double Number = 0.0;
                if (!Item->TryGetNumber(Number))
                {
                    OutError = TEXT("color array must hold numbers");
                    return false;
                }
                Numbers.Add(Number);
            }

            // Values above 1 mean the caller used the 0-255 form.
            const bool bByteRange = Numbers.ContainsByPredicate([](double Number) { return Number > 1.0; });
            const float Scale = bByteRange ? 1.0f / 255.0f : 1.0f;
            OutColor = FLinearColor(
                static_cast<float>(Numbers[0]) * Scale,
                static_cast<float>(Numbers[1]) * Scale,
                static_cast<float>(Numbers[2]) * Scale,
                Numbers.IsValidIndex(3) ? static_cast<float>(Numbers[3]) * Scale : 1.0f);
            return true;
        }

        OutError = TEXT("color must be a hex string or an [r,g,b(,a)] array");
        return false;
    }

    // -----------------------------------------------------------------------
    // JSON shaping: the command layer is the only place that knows about JSON.
    // -----------------------------------------------------------------------

    TSharedPtr<FJsonObject> MakeErrorJson(const FUnrealMCPAnimError& Error)
    {
        TSharedPtr<FJsonObject> Json = FUnrealMCPCommonUtils::CreateErrorResponse(Error.Code, Error.Message);

        if (Error.Candidates.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Candidates;
            for (const FString& Candidate : Error.Candidates)
            {
                Candidates.Add(MakeShared<FJsonValueString>(Candidate));
            }
            Json->SetArrayField(TEXT("candidates"), Candidates);
        }
        if (Error.AvailableFields.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Fields;
            for (const FString& Field : Error.AvailableFields)
            {
                Fields.Add(MakeShared<FJsonValueString>(Field));
            }
            Json->SetArrayField(TEXT("available_fields"), Fields);
        }
        return Json;
    }

    TSharedPtr<FJsonObject> MakeSuccessJson()
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetBoolField(TEXT("success"), true);
        return Json;
    }

    void ApplySequenceInfo(const FUnrealMCPAnimSequenceInfo& Info, const TSharedPtr<FJsonObject>& Json)
    {
        Json->SetStringField(TEXT("asset_path"), Info.AssetPath);
        Json->SetStringField(TEXT("name"), Info.AssetName);
        Json->SetStringField(TEXT("skeleton"), Info.SkeletonPath);
        Json->SetNumberField(TEXT("length"), Info.Length);
        Json->SetNumberField(TEXT("frame_rate"), Info.FrameRate);
        Json->SetNumberField(TEXT("frame_count"), Info.FrameCount);
        Json->SetNumberField(TEXT("bone_track_count"), Info.BoneTrackCount);
        Json->SetNumberField(TEXT("curve_count"), Info.CurveCount);
        Json->SetNumberField(TEXT("notify_count"), Info.NotifyCount);
        Json->SetNumberField(TEXT("notify_track_count"), Info.NotifyTrackCount);
        Json->SetNumberField(TEXT("sync_marker_count"), Info.SyncMarkerCount);
        Json->SetNumberField(TEXT("rate_scale"), Info.RateScale);
        Json->SetBoolField(TEXT("enable_root_motion"), Info.bEnableRootMotion);
        Json->SetBoolField(TEXT("force_root_lock"), Info.bForceRootLock);
        Json->SetStringField(TEXT("root_motion_root_lock"), Info.RootMotionRootLock);
        Json->SetStringField(TEXT("additive_anim_type"), Info.AdditiveAnimType);
        Json->SetStringField(TEXT("additive_base_pose"), Info.AdditiveBasePose);
        Json->SetNumberField(TEXT("raw_size"), static_cast<double>(Info.RawSize));
        Json->SetNumberField(TEXT("compressed_size"), Info.CompressedSize);
        Json->SetStringField(TEXT("compression_scheme"), Info.CompressionScheme);
    }

    TSharedPtr<FJsonObject> SequenceInfoToJson(const FUnrealMCPAnimSequenceInfo& Info)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        ApplySequenceInfo(Info, Json);
        return Json;
    }

    /** Fill the sequence summary of a loaded asset, so a write reports the state it left behind. */
    void SetSequenceReadback(UAnimSequence* Sequence, const TSharedPtr<FJsonObject>& Json)
    {
        FUnrealMCPAnimSequenceInfo Info;
        FUnrealMCPAnimationOps::FillSequenceInfo(Sequence, Info);
        ApplySequenceInfo(Info, Json);
    }

    TSharedPtr<FJsonObject> TransformToJson(const FTransform& Transform)
    {
        const FVector Location = Transform.GetLocation();
        const FRotator Rotation = Transform.Rotator();
        const FVector Scale = Transform.GetScale3D();

        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetArrayField(TEXT("position"), {
            MakeShared<FJsonValueNumber>(Location.X),
            MakeShared<FJsonValueNumber>(Location.Y),
            MakeShared<FJsonValueNumber>(Location.Z)});
        Json->SetArrayField(TEXT("rotation"), {
            MakeShared<FJsonValueNumber>(Rotation.Roll),
            MakeShared<FJsonValueNumber>(Rotation.Pitch),
            MakeShared<FJsonValueNumber>(Rotation.Yaw)});
        Json->SetArrayField(TEXT("quaternion"), {
            MakeShared<FJsonValueNumber>(Transform.GetRotation().X),
            MakeShared<FJsonValueNumber>(Transform.GetRotation().Y),
            MakeShared<FJsonValueNumber>(Transform.GetRotation().Z),
            MakeShared<FJsonValueNumber>(Transform.GetRotation().W)});
        Json->SetArrayField(TEXT("scale"), {
            MakeShared<FJsonValueNumber>(Scale.X),
            MakeShared<FJsonValueNumber>(Scale.Y),
            MakeShared<FJsonValueNumber>(Scale.Z)});
        return Json;
    }

    TSharedPtr<FJsonObject> CurveInfoToJson(const FUnrealMCPAnimCurveInfo& Info)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("curve_name"), Info.CurveName);
        Json->SetNumberField(TEXT("key_count"), Info.KeyCount);
        Json->SetNumberField(TEXT("default_value"), Info.DefaultValue);
        Json->SetNumberField(TEXT("curve_type_flags"), Info.CurveTypeFlags);
        Json->SetBoolField(TEXT("editable"), Info.bEditable);
        Json->SetBoolField(TEXT("metadata"), Info.bMetadata);
        Json->SetBoolField(TEXT("disabled"), Info.bDisabled);
        return Json;
    }

    TSharedPtr<FJsonObject> CurveKeyToJson(const FUnrealMCPAnimCurveKey& Key)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetNumberField(TEXT("time"), Key.Time);
        Json->SetNumberField(TEXT("value"), Key.Value);
        Json->SetNumberField(TEXT("arrive_tangent"), Key.ArriveTangent);
        Json->SetNumberField(TEXT("leave_tangent"), Key.LeaveTangent);
        Json->SetStringField(TEXT("interp"), Key.InterpMode);
        Json->SetStringField(TEXT("tangent"), Key.TangentMode);
        return Json;
    }

    TSharedPtr<FJsonObject> NotifyToJson(const FUnrealMCPAnimNotifyInfo& Info)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetNumberField(TEXT("notify_index"), Info.NotifyIndex);
        Json->SetStringField(TEXT("notify_name"), Info.NotifyName);
        Json->SetStringField(TEXT("notify_class"), Info.NotifyClass);
        Json->SetStringField(TEXT("entry_type"), Info.EntryType);
        Json->SetNumberField(TEXT("trigger_time"), Info.TriggerTime);
        Json->SetNumberField(TEXT("duration"), Info.Duration);
        Json->SetNumberField(TEXT("end_time"), Info.EndTime);
        Json->SetNumberField(TEXT("track_index"), Info.TrackIndex);
        Json->SetStringField(TEXT("track_name"), Info.TrackName);
        Json->SetNumberField(TEXT("trigger_chance"), Info.TriggerChance);
        Json->SetNumberField(TEXT("weight_threshold"), Info.WeightThreshold);
        Json->SetBoolField(TEXT("trigger_on_server"), Info.bTriggerOnServer);
        Json->SetBoolField(TEXT("trigger_on_follower"), Info.bTriggerOnFollower);
        Json->SetStringField(TEXT("filter_type"), Info.FilterType);
        Json->SetNumberField(TEXT("filter_lod"), Info.FilterLOD);
        Json->SetStringField(TEXT("color"), Info.Color);
        Json->SetStringField(TEXT("guid"), Info.Guid);
        return Json;
    }

    TSharedPtr<FJsonObject> NotifyTrackToJson(const FUnrealMCPAnimNotifyTrackInfo& Info)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetNumberField(TEXT("track_index"), Info.TrackIndex);
        Json->SetStringField(TEXT("track_name"), Info.TrackName);
        Json->SetStringField(TEXT("color"), Info.Color);
        Json->SetNumberField(TEXT("notify_count"), Info.NotifyCount);
        Json->SetNumberField(TEXT("sync_marker_count"), Info.SyncMarkerCount);
        return Json;
    }

    TSharedPtr<FJsonObject> SyncMarkerToJson(const FUnrealMCPAnimSyncMarkerInfo& Info)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("marker_name"), Info.MarkerName);
        Json->SetNumberField(TEXT("time"), Info.Time);
        Json->SetNumberField(TEXT("track_index"), Info.TrackIndex);
        return Json;
    }

    void SetSequenceListJson(const TArray<FUnrealMCPAnimSequenceInfo>& Infos, int32 FoundCount, int32 MaxResults,
                             const TSharedPtr<FJsonObject>& Json)
    {
        TArray<TSharedPtr<FJsonValue>> SequenceValues;
        for (const FUnrealMCPAnimSequenceInfo& Info : Infos)
        {
            SequenceValues.Add(MakeShared<FJsonValueObject>(SequenceInfoToJson(Info)));
        }
        Json->SetArrayField(TEXT("sequences"), SequenceValues);
        Json->SetNumberField(TEXT("found_count"), FoundCount);
        Json->SetNumberField(TEXT("returned_count"), Infos.Num());
        Json->SetBoolField(TEXT("truncated"), FoundCount > Infos.Num());
        Json->SetNumberField(TEXT("max_results"), MaxResults);
    }

    /** Re-read one notify out of the sequence so a write reports what actually landed. */
    void SetNotifyReadback(UAnimSequence* Sequence, const TSharedPtr<FJsonObject>& Json, int32 NotifyIndex)
    {
        TArray<FUnrealMCPAnimNotifyInfo> Notifies;
        FUnrealMCPAnimationOps::ListNotifies(Sequence, Notifies);
        if (Notifies.IsValidIndex(NotifyIndex))
        {
            Json->SetObjectField(TEXT("notify"), NotifyToJson(Notifies[NotifyIndex]));
        }
        Json->SetNumberField(TEXT("notify_count"), Notifies.Num());
    }

    /** Time in seconds for either a `time` or a `frame` parameter. */
    float ResolveTimeParam(const TSharedPtr<FJsonObject>& Params, UAnimSequence* Sequence, FUnrealMCPAnimError& OutError,
                           bool& bOutResolved)
    {
        bOutResolved = false;

        if (HasParam(Params, TEXT("time")))
        {
            bOutResolved = true;
            return GetFloatParam(Params, TEXT("time"), 0.0f);
        }

        if (HasParam(Params, TEXT("frame")))
        {
            const int32 Frame = GetIntParam(Params, TEXT("frame"), 0);
            // The asset's own rate, not the project default (see GetSequenceFrameRate).
            const float FrameRate = Sequence
                ? static_cast<float>(FUnrealMCPAnimationOps::GetSequenceFrameRate(Sequence).AsDecimal())
                : 30.0f;
            bOutResolved = true;
            return FrameRate > 0.0f ? static_cast<float>(Frame) / FrameRate : 0.0f;
        }

        OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("pass either 'time' (seconds) or 'frame'"));
        return 0.0f;
    }

    /** Resolve the notify a write command targets from its locator parameters. */
    bool ResolveNotifyParam(const TSharedPtr<FJsonObject>& Params, UAnimSequence* Sequence, int32& OutNotifyIndex,
                            FUnrealMCPAnimError& OutError)
    {
        return FUnrealMCPAnimationOps::ResolveNotify(
            Sequence,
            GetIntParam(Params, TEXT("notify_index"), INDEX_NONE),
            GetStringParam(Params, TEXT("notify_name")),
            GetStringParam(Params, TEXT("guid")),
            GetFloatParam(Params, TEXT("time"), 0.0f),
            HasParam(Params, TEXT("time")),
            OutNotifyIndex,
            OutError);
    }

    /** Resolve the notify track a command targets from index or name. */
    bool ResolveTrackParam(const TSharedPtr<FJsonObject>& Params, UAnimSequence* Sequence, int32& OutTrackIndex,
                           FUnrealMCPAnimError& OutError)
    {
        return FUnrealMCPAnimationOps::ResolveNotifyTrack(
            Sequence,
            GetIntParam(Params, TEXT("track_index"), INDEX_NONE),
            GetStringParam(Params, TEXT("track_name")),
            OutTrackIndex,
            OutError);
    }

    bool ResolveMarkerParam(const TSharedPtr<FJsonObject>& Params, UAnimSequence* Sequence, int32& OutMarkerIndex,
                            FUnrealMCPAnimError& OutError)
    {
        return FUnrealMCPAnimationOps::ResolveSyncMarker(
            Sequence,
            GetStringParam(Params, TEXT("marker_name")),
            GetFloatParam(Params, TEXT("time"), 0.0f),
            HasParam(Params, TEXT("time")),
            GetIntParam(Params, TEXT("marker_index"), INDEX_NONE),
            OutMarkerIndex,
            OutError);
    }

    /** Parse the `keys` array of the bone track / curve writers. */
    bool ReadKeyframesParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName,
                            TArray<FUnrealMCPAnimKeyframe>& OutKeys, FUnrealMCPAnimError& OutError)
    {
        OutKeys.Reset();

        const TArray<TSharedPtr<FJsonValue>>* KeyValues = nullptr;
        if (!Params.IsValid() || !Params->TryGetArrayField(FieldName, KeyValues) || !KeyValues)
        {
            OutError.Set(EUnrealMCPAnimError::InvalidParams,
                         FString::Printf(TEXT("'%s' must be an array of keys"), *FieldName));
            return false;
        }

        for (const TSharedPtr<FJsonValue>& KeyValue : *KeyValues)
        {
            const TSharedPtr<FJsonObject>* KeyObject = nullptr;
            if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObject) || !KeyObject)
            {
                OutError.Set(EUnrealMCPAnimError::InvalidValue, TEXT("every key must be an object"));
                return false;
            }

            FUnrealMCPAnimKeyframe Key;
            double Time = 0.0;
            if (!(*KeyObject)->TryGetNumberField(TEXT("time"), Time))
            {
                OutError.Set(EUnrealMCPAnimError::InvalidValue, TEXT("every key needs a 'time' in seconds"));
                return false;
            }
            Key.Time = static_cast<float>(Time);

            FString ParseError;
            if (const TSharedPtr<FJsonValue> Position = (*KeyObject)->TryGetField(TEXT("position")))
            {
                if (!ParseVectorValue(Position, Key.Position, ParseError))
                {
                    OutError.Set(EUnrealMCPAnimError::InvalidValue, ParseError);
                    return false;
                }
            }
            if (const TSharedPtr<FJsonValue> Rotation = (*KeyObject)->TryGetField(TEXT("rotation")))
            {
                if (!ParseRotationValue(Rotation, Key.Rotation, ParseError))
                {
                    OutError.Set(EUnrealMCPAnimError::InvalidValue, ParseError);
                    return false;
                }
            }
            if (const TSharedPtr<FJsonValue> Scale = (*KeyObject)->TryGetField(TEXT("scale")))
            {
                if (!ParseVectorValue(Scale, Key.Scale, ParseError))
                {
                    OutError.Set(EUnrealMCPAnimError::InvalidValue, ParseError);
                    return false;
                }
            }

            OutKeys.Add(Key);
        }

        if (OutKeys.Num() == 0)
        {
            OutError.Set(EUnrealMCPAnimError::InvalidValue,
                         FString::Printf(TEXT("'%s' must hold at least one key"), *FieldName));
            return false;
        }

        return true;
    }

    bool ReadCurveKeysParam(const TSharedPtr<FJsonObject>& Params, TArray<FUnrealMCPAnimCurveKey>& OutKeys,
                            FUnrealMCPAnimError& OutError)
    {
        OutKeys.Reset();

        const TArray<TSharedPtr<FJsonValue>>* KeyValues = nullptr;
        if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("keys"), KeyValues) || !KeyValues)
        {
            OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("'keys' must be an array of {time, value} objects"));
            return false;
        }

        for (const TSharedPtr<FJsonValue>& KeyValue : *KeyValues)
        {
            const TSharedPtr<FJsonObject>* KeyObject = nullptr;
            if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObject) || !KeyObject)
            {
                OutError.Set(EUnrealMCPAnimError::InvalidValue, TEXT("every key must be an object"));
                return false;
            }

            FUnrealMCPAnimCurveKey Key;
            double Time = 0.0;
            double Value = 0.0;
            if (!(*KeyObject)->TryGetNumberField(TEXT("time"), Time) ||
                !(*KeyObject)->TryGetNumberField(TEXT("value"), Value))
            {
                OutError.Set(EUnrealMCPAnimError::InvalidValue, TEXT("every key needs 'time' and 'value'"));
                return false;
            }

            Key.Time = static_cast<float>(Time);
            Key.Value = static_cast<float>(Value);
            double Tangent = 0.0;
            if ((*KeyObject)->TryGetNumberField(TEXT("arrive_tangent"), Tangent))
            {
                Key.ArriveTangent = static_cast<float>(Tangent);
            }
            if ((*KeyObject)->TryGetNumberField(TEXT("leave_tangent"), Tangent))
            {
                Key.LeaveTangent = static_cast<float>(Tangent);
            }
            (*KeyObject)->TryGetStringField(TEXT("interp"), Key.InterpMode);
            (*KeyObject)->TryGetStringField(TEXT("tangent"), Key.TangentMode);
            OutKeys.Add(Key);
        }

    if (OutKeys.Num() == 0)
    {
        OutError.Set(EUnrealMCPAnimError::InvalidValue, TEXT("'keys' must hold at least one key"));
        return false;
    }

    return true;
}
}

// ---------------------------------------------------------------------------
// Montage helpers
//
// The notify commands are shared between the two categories: a montage is an animation asset and
// keeps its notifies in the very same array, so those commands resolve an "animation host" and
// then dispatch to the sequence or the montage kernel. Everything else in anim_montage is
// montage-only and lives behind FUnrealMCPAnimationMontageOps.
// ---------------------------------------------------------------------------

namespace
{
    TSharedPtr<FJsonObject> MontageInfoToJson(const FUnrealMCPAnimMontageInfo& Info)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("asset_path"), Info.AssetPath);
        Json->SetStringField(TEXT("asset_name"), Info.AssetName);
        Json->SetStringField(TEXT("skeleton_path"), Info.SkeletonPath);
        Json->SetNumberField(TEXT("length"), Info.Length);
        Json->SetNumberField(TEXT("section_count"), Info.SectionCount);
        Json->SetNumberField(TEXT("slot_track_count"), Info.SlotTrackCount);
        Json->SetNumberField(TEXT("segment_count"), Info.SegmentCount);
        Json->SetNumberField(TEXT("notify_count"), Info.NotifyCount);
        Json->SetNumberField(TEXT("branching_point_count"), Info.BranchingPointCount);
        Json->SetNumberField(TEXT("rate_scale"), Info.RateScale);
        Json->SetStringField(TEXT("sync_group"), Info.SyncGroup);
        Json->SetBoolField(TEXT("enable_root_motion_translation"), Info.bEnableRootMotionTranslation);
        Json->SetBoolField(TEXT("enable_root_motion_rotation"), Info.bEnableRootMotionRotation);
        Json->SetStringField(TEXT("info_source"), Info.InfoSource);
        return Json;
    }

    TSharedPtr<FJsonObject> SectionToJson(const FUnrealMCPAnimSectionInfo& Info)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetNumberField(TEXT("section_index"), Info.SectionIndex);
        Json->SetStringField(TEXT("section_name"), Info.SectionName);
        Json->SetNumberField(TEXT("start_time"), Info.StartTime);
        Json->SetNumberField(TEXT("end_time"), Info.EndTime);
        Json->SetNumberField(TEXT("length"), Info.Length);
        Json->SetStringField(TEXT("next_section_name"), Info.NextSectionName);
        Json->SetBoolField(TEXT("is_looping"), Info.bIsLooping);
        Json->SetNumberField(TEXT("slot_index"), Info.SlotIndex);
        Json->SetNumberField(TEXT("segment_index"), Info.SegmentIndex);
        Json->SetStringField(TEXT("link_method"), Info.LinkMethod);
        return Json;
    }

    TSharedPtr<FJsonObject> SlotTrackToJson(const FUnrealMCPAnimSlotTrackInfo& Info)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetNumberField(TEXT("track_index"), Info.TrackIndex);
        Json->SetStringField(TEXT("slot_name"), Info.SlotName);
        Json->SetStringField(TEXT("group_name"), Info.GroupName);
        Json->SetNumberField(TEXT("segment_count"), Info.SegmentCount);
        Json->SetNumberField(TEXT("track_length"), Info.TrackLength);
        return Json;
    }

    TSharedPtr<FJsonObject> SegmentToJson(const FUnrealMCPAnimSegmentInfo& Info)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetNumberField(TEXT("track_index"), Info.TrackIndex);
        Json->SetNumberField(TEXT("segment_index"), Info.SegmentIndex);
        Json->SetStringField(TEXT("anim_path"), Info.AnimPath);
        Json->SetStringField(TEXT("anim_name"), Info.AnimName);
        Json->SetNumberField(TEXT("start_time"), Info.StartPos);
        Json->SetNumberField(TEXT("end_time"), Info.EndPos);
        Json->SetNumberField(TEXT("length"), Info.Length);
        Json->SetNumberField(TEXT("start_position"), Info.AnimStartTime);
        Json->SetNumberField(TEXT("end_position"), Info.AnimEndTime);
        Json->SetNumberField(TEXT("play_rate"), Info.PlayRate);
        Json->SetNumberField(TEXT("loop_count"), Info.LoopCount);
        Json->SetBoolField(TEXT("valid"), Info.bValid);
        return Json;
    }

    TSharedPtr<FJsonObject> BranchingPointToJson(const FUnrealMCPAnimBranchingPointInfo& Info)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetNumberField(TEXT("branching_point_index"), Info.BranchingPointIndex);
        Json->SetNumberField(TEXT("notify_index"), Info.NotifyIndex);
        Json->SetStringField(TEXT("notify_name"), Info.NotifyName);
        Json->SetStringField(TEXT("notify_class"), Info.NotifyClass);
        Json->SetNumberField(TEXT("trigger_time"), Info.TriggerTime);
        Json->SetNumberField(TEXT("track_index"), Info.TrackIndex);
        Json->SetStringField(TEXT("section_name"), Info.SectionName);
        return Json;
    }

    TSharedPtr<FJsonObject> BlendToJson(const FUnrealMCPAnimBlendInfo& Info)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetNumberField(TEXT("blend_in_time"), Info.BlendInTime);
        Json->SetStringField(TEXT("blend_in_option"), Info.BlendInOption);
        Json->SetStringField(TEXT("blend_in_mode"), Info.BlendInMode);
        Json->SetNumberField(TEXT("blend_out_time"), Info.BlendOutTime);
        Json->SetStringField(TEXT("blend_out_option"), Info.BlendOutOption);
        Json->SetStringField(TEXT("blend_out_mode"), Info.BlendOutMode);
        Json->SetNumberField(TEXT("blend_out_trigger_time"), Info.BlendOutTriggerTime);
        Json->SetBoolField(TEXT("enable_auto_blend_out"), Info.bEnableAutoBlendOut);
        return Json;
    }

    /** The sequence notify JSON plus the two fields only a montage can answer. */
    TSharedPtr<FJsonObject> MontageNotifyToJson(const FUnrealMCPAnimNotifyInfo& Info)
    {
        TSharedPtr<FJsonObject> Json = NotifyToJson(Info);
        Json->SetStringField(TEXT("linked_section_name"), Info.LinkedSectionName);
        Json->SetBoolField(TEXT("is_branching_point"), Info.bIsBranchingPoint);
        return Json;
    }

    /** Re-read the montage so a write reports what actually landed, not what was asked for. */
    void SetMontageReadback(UAnimMontage* Montage, const TSharedPtr<FJsonObject>& Json)
    {
        FUnrealMCPAnimMontageInfo Info;
        FUnrealMCPAnimationMontageOps::FillMontageInfo(Montage, Info);

        Json->SetNumberField(TEXT("length"), Info.Length);
        Json->SetNumberField(TEXT("section_count"), Info.SectionCount);
        Json->SetNumberField(TEXT("slot_track_count"), Info.SlotTrackCount);
        Json->SetNumberField(TEXT("segment_count"), Info.SegmentCount);
        Json->SetNumberField(TEXT("notify_count"), Info.NotifyCount);
        Json->SetNumberField(TEXT("branching_point_count"), Info.BranchingPointCount);
        Json->SetNumberField(TEXT("rate_scale"), Info.RateScale);
        Json->SetBoolField(TEXT("enable_root_motion_translation"), Info.bEnableRootMotionTranslation);
        Json->SetBoolField(TEXT("enable_root_motion_rotation"), Info.bEnableRootMotionRotation);
    }

    void SetSectionReadback(UAnimMontage* Montage, const TSharedPtr<FJsonObject>& Json, int32 SectionIndex)
    {
        FUnrealMCPAnimSectionInfo Info;
        FUnrealMCPAnimationMontageOps::FillSectionInfo(Montage, SectionIndex, Info);
        Json->SetObjectField(TEXT("section"), SectionToJson(Info));
        Json->SetNumberField(TEXT("section_count"), Montage->CompositeSections.Num());
    }

    void SetSlotReadback(UAnimMontage* Montage, const TSharedPtr<FJsonObject>& Json, int32 TrackIndex)
    {
        TArray<FUnrealMCPAnimSlotTrackInfo> Tracks;
        FUnrealMCPAnimationMontageOps::ListSlotTracks(Montage, Tracks);
        if (Tracks.IsValidIndex(TrackIndex))
        {
            Json->SetObjectField(TEXT("slot_track"), SlotTrackToJson(Tracks[TrackIndex]));
        }
        Json->SetNumberField(TEXT("slot_track_count"), Tracks.Num());
    }

    void SetSegmentReadback(UAnimMontage* Montage, const TSharedPtr<FJsonObject>& Json, int32 TrackIndex,
                            int32 SegmentIndex)
    {
        TArray<FUnrealMCPAnimSegmentInfo> Segments;
        FUnrealMCPAnimError Ignored;
        FUnrealMCPAnimationMontageOps::ListAnimSegments(Montage, TrackIndex, Segments, Ignored);
        if (Segments.IsValidIndex(SegmentIndex))
        {
            Json->SetObjectField(TEXT("segment"), SegmentToJson(Segments[SegmentIndex]));
        }
        Json->SetNumberField(TEXT("segment_count"), Segments.Num());
        Json->SetNumberField(TEXT("length"), Montage->GetPlayLength());
    }

    void SetMontageNotifyReadback(UAnimMontage* Montage, const TSharedPtr<FJsonObject>& Json, int32 NotifyIndex)
    {
        TArray<FUnrealMCPAnimNotifyInfo> Notifies;
        FUnrealMCPAnimationMontageOps::ListMontageNotifies(Montage, Notifies);
        if (Notifies.IsValidIndex(NotifyIndex))
        {
            Json->SetObjectField(TEXT("notify"), MontageNotifyToJson(Notifies[NotifyIndex]));
        }
        Json->SetNumberField(TEXT("notify_count"), Notifies.Num());

        TArray<FUnrealMCPAnimBranchingPointInfo> BranchingPoints;
        FUnrealMCPAnimationMontageOps::ListBranchingPoints(Montage, BranchingPoints);
        Json->SetNumberField(TEXT("branching_point_count"), BranchingPoints.Num());
    }

    /**
     * D8: Persona's preview holds an FAnimMontageInstance with raw pointers and indices into
     * SlotAnimTracks / AnimSegments / CompositeSections. A structural change leaves those dangling,
     * so the editor of the asset is closed before the write - and the response says so.
     */
    int32 CloseMontageEditorsFor(UAnimMontage* Montage)
    {
        return FUnrealMCPEditorCommands::CloseAssetEditorsFor(Montage);
    }

    void ReportEditorClosed(const TSharedPtr<FJsonObject>& Json, int32 ClosedCount)
    {
        Json->SetBoolField(TEXT("editor_closed"), true);
        Json->SetNumberField(TEXT("closed_editor_count"), ClosedCount);
    }

    bool ResolveSectionParam(const TSharedPtr<FJsonObject>& Params, UAnimMontage* Montage, int32& OutSectionIndex,
                             FUnrealMCPAnimError& OutError)
    {
        return FUnrealMCPAnimationMontageOps::ResolveSection(
            Montage,
            GetStringParam(Params, TEXT("section_name")),
            GetIntParam(Params, TEXT("section_index"), INDEX_NONE),
            OutSectionIndex,
            OutError);
    }

    bool ResolveSlotParam(const TSharedPtr<FJsonObject>& Params, UAnimMontage* Montage, int32& OutTrackIndex,
                          FUnrealMCPAnimError& OutError)
    {
        return FUnrealMCPAnimationMontageOps::ResolveSlotTrack(
            Montage,
            GetIntParam(Params, TEXT("track_index"), INDEX_NONE),
            GetStringParam(Params, TEXT("slot_name")),
            OutTrackIndex,
            OutError);
    }

    bool ResolveSegmentParam(const TSharedPtr<FJsonObject>& Params, UAnimMontage* Montage, int32& OutTrackIndex,
                             int32& OutSegmentIndex, FUnrealMCPAnimError& OutError)
    {
        if (!ResolveSlotParam(Params, Montage, OutTrackIndex, OutError))
        {
            return false;
        }

        OutSegmentIndex = GetIntParam(Params, TEXT("segment_index"), INDEX_NONE);
        if (!Montage->SlotAnimTracks[OutTrackIndex].AnimTrack.AnimSegments.IsValidIndex(OutSegmentIndex))
        {
            OutError.Set(EUnrealMCPAnimError::SegmentNotFound,
                         FString::Printf(TEXT("segment_index %d is out of range (%d segment(s) in slot '%s')"),
                                         OutSegmentIndex,
                                         Montage->SlotAnimTracks[OutTrackIndex].AnimTrack.AnimSegments.Num(),
                                         *Montage->SlotAnimTracks[OutTrackIndex].SlotName.ToString()));
            return false;
        }

        return true;
    }

    /**
     * The notify fields the shared notify commands write on a montage. `link_to_section` is not here
     * because it has its own command (it means "move the notify to a section start").
     */
    const TArray<FString> MontageNotifyProperties = {TEXT("trigger_time")};

    /** The preview session state, shared by every editor-session command's response. */
    void ApplyPreviewState(const FUnrealMCPAnimPreviewState& State, const TSharedPtr<FJsonObject>& Json)
    {
        Json->SetBoolField(TEXT("editor_open"), State.bEditorOpen);
        Json->SetBoolField(TEXT("preview_available"), State.bPreviewAvailable);
        Json->SetBoolField(TEXT("is_montage"), State.bIsMontage);
        Json->SetStringField(TEXT("editor_name"), State.EditorName);
        Json->SetNumberField(TEXT("preview_time"), State.PreviewTime);
        Json->SetNumberField(TEXT("length"), State.PlayLength);
        Json->SetBoolField(TEXT("playing"), State.bIsPlaying);
        Json->SetBoolField(TEXT("looping"), State.bIsLooping);
        Json->SetNumberField(TEXT("play_rate"), State.PlayRate);
        Json->SetStringField(TEXT("current_section_name"), State.CurrentSectionName);
        Json->SetNumberField(TEXT("current_section_index"), State.CurrentSectionIndex);
        Json->SetNumberField(TEXT("preview_components_bound"), State.PreviewComponentsBound);
    }

    /** The asset a shared command targets: a sequence or a montage, told apart by its class. */
    UAnimSequenceBase* ResolveAnimationHost(const TSharedPtr<FJsonObject>& Params, UAnimMontage*& OutMontage,
                                         UAnimSequence*& OutSequence, FUnrealMCPAnimError& OutError)
    {
        OutMontage = nullptr;
        OutSequence = nullptr;

        const FString AssetPath = GetStringParam(Params, TEXT("asset_path"));
        if (AssetPath.IsEmpty())
        {
            OutError.Set(EUnrealMCPAnimError::InvalidParams, TEXT("asset_path is required"));
            return nullptr;
        }

        UObject* Asset = FUnrealMCPCommonUtils::FindAsset(AssetPath);
        if (!Asset)
        {
            OutError.Set(EUnrealMCPAnimError::AssetNotFound,
                         FString::Printf(TEXT("'%s' was not found; pass an object path (/Game/A/B.B) or a package path (/Game/A/B)"),
                                         *AssetPath));
            return nullptr;
        }

        UAnimSequenceBase* Host = Cast<UAnimSequenceBase>(Asset);
        if (!Host)
        {
            OutError.Set(EUnrealMCPAnimError::AssetNotAnimSequence,
                         FString::Printf(TEXT("'%s' is a %s, not an animation asset"), *Asset->GetPathName(),
                                         *Asset->GetClass()->GetName()));
            return nullptr;
        }

        OutMontage = Cast<UAnimMontage>(Host);
        OutSequence = OutMontage ? nullptr : Cast<UAnimSequence>(Host);
        if (!OutMontage && !OutSequence)
        {
            OutError.Set(EUnrealMCPAnimError::AssetNotAnimSequence,
                         FString::Printf(TEXT("notify commands edit a sequence or a montage; '%s' is a %s"),
                                         *Asset->GetPathName(), *Asset->GetClass()->GetName()));
            return nullptr;
        }

        return Host;
    }

    /** Read a `value` parameter as a number (the shape every notify time / segment field has). */
    bool ReadNumberValue(const TSharedPtr<FJsonObject>& Params, double& OutNumber, FUnrealMCPAnimError& OutError)
    {
        const TSharedPtr<FJsonValue> Value = GetValueParam(Params, TEXT("value"));
        if (!Value.IsValid() || !Value->TryGetNumber(OutNumber))
        {
            OutError.Set(EUnrealMCPAnimError::InvalidValue, TEXT("'value' must be a number"));
            return false;
        }
        return true;
    }
}

FUnrealMCPAnimationCommands::FUnrealMCPAnimationCommands()
{
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::RunCommand(const FString& CommandType,
                                                               const TSharedPtr<FJsonObject>& Params,
                                                               const TFunction<TSharedPtr<FJsonObject>(const TSharedPtr<FJsonObject>&)>& Body)
{
    const FMCPCommandEntry* Entry = FMCPCommandRegistry::Get().Find(CommandType);
    const bool bMutating = Entry && Entry->Flags.bMutatesGraph;

    // auto_close: an open Persona editor keeps its own copy of the graph the animation data lives in,
    // so a structural write through it can be rolled back by the editor. Closing it is what a caller
    // would have to do by hand; it can be opted into instead of remembered.
    TArray<FString> ClosedEditors;
    bool bAutoClose = false;
    if (bMutating && Params.IsValid() && Params->TryGetBoolField(TEXT("auto_close"), bAutoClose) && bAutoClose)
    {
        const FString AutoCloseAssetPath = GetStringParam(Params, TEXT("asset_path"));
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
        Transaction = MakeUnique<FScopedTransaction>(FText::FromString(FString::Printf(TEXT("UnrealMCP %s"), *CommandType)));
    }

    TSharedPtr<FJsonObject> Result = Body(Params);

    const bool bSuccess = FUnrealMCPCommonUtils::ResponseIndicatesSuccess(Result);
    if (bMutating && !bSuccess && Transaction.IsValid())
    {
        Transaction->Cancel();
    }
    Transaction.Reset();

    // Persist immediately on success: the editor is routinely killed by the build script, so a write
    // that only lives in memory is a write that never happened. Create commands have no asset_path in
    // their params, so the response's own asset_path is the fallback.
    // An explicit `persist=false` lets a batch of writes be flushed once by the caller instead of
    // writing the asset per command.
    bool bPersist = true;
    Params->TryGetBoolField(TEXT("persist"), bPersist);

    bool bSaved = false;
    if (bMutating && bSuccess)
    {
        if (bPersist)
        {
            FString AssetPath = GetStringParam(Params, TEXT("asset_path"));
            if (AssetPath.IsEmpty())
            {
                Result->TryGetStringField(TEXT("asset_path"), AssetPath);
            }

            if (!AssetPath.IsEmpty())
            {
                if (UObject* Asset = FUnrealMCPCommonUtils::FindAsset(AssetPath))
                {
                    bSaved = FUnrealMCPCommonUtils::SaveAssetForObject(Asset);
                }
            }
        }
        if (!Result->HasField(TEXT("saved")))
        {
            Result->SetBoolField(TEXT("saved"), bSaved);
        }
        if (!Result->HasField(TEXT("persist_requested")))
        {
            Result->SetBoolField(TEXT("persist_requested"), bPersist);
        }
    }

    if (bSuccess && ClosedEditors.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> ClosedJson;
        for (const FString& ClosedEditor : ClosedEditors)
        {
            ClosedJson.Add(MakeShared<FJsonValueString>(ClosedEditor));
        }
        Result->SetArrayField(TEXT("closed_editors"), ClosedJson);
    }

    return Result;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void FUnrealMCPAnimationCommands::RegisterCommands(FMCPCommandRegistry& Registry)
{
    const TArray<FMCPParamSpec> LocatorParams = {
        MCPParamOpt(TEXT("notify_index"), TEXT("int"), TEXT("Notify index as returned by list_notifies")),
        MCPParamOpt(TEXT("notify_name"), TEXT("string"), TEXT("Notify name (preferred over the index)")),
        MCPParamOpt(TEXT("guid"), TEXT("string"), TEXT("Editor guid of the notify")),
    };

    const auto AssetPathParam = []() { return MCPParamOpt(TEXT("asset_path"), TEXT("string"), TEXT("Animation sequence asset path")); };

    // Shared parameter tables. They are all declared here, before the first registration: the
    // static consistency check reads one registration block per command (from its macro to the
    // next one), so a table declared between two registrations would be read as the previous
    // command's parameters.
    const TArray<FMCPParamSpec> SampleParams = {
        AssetPathParam(),
        MCPParam(TEXT("bone_name"), TEXT("string"), TEXT("Bone to sample")),
        MCPParamOpt(TEXT("time"), TEXT("float"), TEXT("Time in seconds (or pass 'frame')")),
        MCPParamOpt(TEXT("frame"), TEXT("int"), TEXT("Frame number (alternative to 'time')")),
    };

    const TArray<FMCPParamSpec> PoseParams = {
        AssetPathParam(),
        MCPParamOpt(TEXT("time"), TEXT("float"), TEXT("Time in seconds (or pass 'frame')")),
        MCPParamOpt(TEXT("frame"), TEXT("int"), TEXT("Frame number (alternative to 'time')")),
    };

    const TArray<FMCPParamSpec> CurveParams = {
        AssetPathParam(),
        MCPParam(TEXT("curve_name"), TEXT("string"), TEXT("Curve name")),
    };

    const TArray<FMCPParamSpec> CreateParams = {
        MCPParam(TEXT("name"), TEXT("string"), TEXT("Asset name")),
        MCPParam(TEXT("skeleton"), TEXT("string"), TEXT("Skeleton asset path")),
        MCPParamOpt(TEXT("folder"), TEXT("string"), TEXT("Content folder; default /Game")),
        MCPParamOpt(TEXT("frame_rate"), TEXT("float"), TEXT("Sampling frame rate; default 30")),
        MCPParamOpt(TEXT("duration"), TEXT("float"), TEXT("Length in seconds; default 1")),
        MCPParamOpt(TEXT("tracks"), TEXT("array"), TEXT("Optional bone tracks [{bone_name, keys:[{time,position,rotation,scale}]}]")),
    };

    // Locator plus the one field a set_notify_* command writes.
    const auto NotifyWriteParams = [&LocatorParams, &AssetPathParam]()
    {
        return TArray<FMCPParamSpec>{
            AssetPathParam(),
            LocatorParams[0],
            LocatorParams[1],
            LocatorParams[2],
            MCPParamOpt(TEXT("time"), TEXT("float"), TEXT("Trigger time in seconds (also disambiguates a name)")),
            MCPParam(TEXT("value"), TEXT("object"), TEXT("New value of the field")),
        };
    };

    // --- montage: shared parameter tables (also declared before the first registration)
    const auto MontagePathParam = []() { return MCPParamOpt(TEXT("asset_path"), TEXT("string"), TEXT("Animation montage asset path")); };

    const TArray<FMCPParamSpec> SectionLocatorParams = {
        MontagePathParam(),
        MCPParamOpt(TEXT("section_name"), TEXT("string"), TEXT("Section name (preferred over the index)")),
        MCPParamOpt(TEXT("section_index"), TEXT("int"), TEXT("Section index as returned by list_sections")),
    };

    const TArray<FMCPParamSpec> SlotLocatorParams = {
        MontagePathParam(),
        MCPParamOpt(TEXT("track_index"), TEXT("int"), TEXT("Slot track index as returned by list_slot_tracks")),
        MCPParamOpt(TEXT("slot_name"), TEXT("string"), TEXT("Slot name (preferred over the index)")),
    };

    const TArray<FMCPParamSpec> SegmentLocatorParams = {
        MontagePathParam(),
        MCPParamOpt(TEXT("track_index"), TEXT("int"), TEXT("Slot track index as returned by list_slot_tracks")),
        MCPParamOpt(TEXT("slot_name"), TEXT("string"), TEXT("Slot name (preferred over the index)")),
        MCPParamOpt(TEXT("segment_index"), TEXT("int"), TEXT("Segment index as returned by list_anim_segments")),
    };

    // Locator plus the one field a set_segment_* command writes.
    const auto SegmentValueParams = [&SegmentLocatorParams]()
    {
        TArray<FMCPParamSpec> Params = SegmentLocatorParams;
        Params.Add(MCPParam(TEXT("value"), TEXT("number"), TEXT("New value of the field")));
        return Params;
    };

    // --- probe and discovery
    MCP_REGISTER_COMMAND(Registry, "anim_self_check", "anim_sequence",
        "Liveness probe for the animation domain: engine version and registered command count.",
        (TArray<FMCPParamSpec>{}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("anim_self_check"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAnimSelfCheck(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "list_anim_sequences", "anim_sequence",
        "List animation sequence assets below a folder (registry metadata only, assets are not loaded).",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("search_path"), TEXT("string"), TEXT("Content folder to search; default /Game")),
            MCPParamOpt(TEXT("skeleton"), TEXT("string"), TEXT("Only sequences using this skeleton")),
            MCPParamOpt(TEXT("max_results"), TEXT("int"), TEXT("Upper limit on returned entries; default 200")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("list_anim_sequences"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleListAnimSequences(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "get_anim_sequence_info", "anim_sequence",
        "Full summary of one animation sequence (length, frame rate, tracks, curves, notifies, compression).",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("asset_path"), TEXT("string"), TEXT("Animation sequence asset path")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_anim_sequence_info"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetAnimSequenceInfo(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "find_animations_for_skeleton", "anim_sequence",
        "List the animation sequences that use a skeleton.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("skeleton"), TEXT("string"), TEXT("Skeleton asset path")),
            MCPParamOpt(TEXT("search_path"), TEXT("string"), TEXT("Content folder to search; default /Game")),
            MCPParamOpt(TEXT("max_results"), TEXT("int"), TEXT("Upper limit on returned entries; default 200")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("find_animations_for_skeleton"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleFindAnimationsForSkeleton(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "search_animations", "anim_sequence",
        "Find animation sequences by asset name (substring or wildcard).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("query"), TEXT("string"), TEXT("Name fragment or wildcard pattern")),
            MCPParamOpt(TEXT("search_path"), TEXT("string"), TEXT("Content folder to search; default /Game")),
            MCPParamOpt(TEXT("max_results"), TEXT("int"), TEXT("Upper limit on returned entries; default 200")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("search_animations"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSearchAnimations(P); }); }));

    // --- properties
    MCP_REGISTER_COMMAND(Registry, "get_animation_length", "anim_sequence",
        "Read the play length of a sequence in seconds.",
        (TArray<FMCPParamSpec>{AssetPathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_animation_length"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetAnimationLength(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "get_animation_frame_rate", "anim_sequence",
        "Read the sampling frame rate of a sequence.",
        (TArray<FMCPParamSpec>{AssetPathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_animation_frame_rate"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetAnimationFrameRate(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "get_animation_frame_count", "anim_sequence",
        "Read the number of sampled keys of a sequence.",
        (TArray<FMCPParamSpec>{AssetPathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_animation_frame_count"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetAnimationFrameCount(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "get_animation_skeleton", "anim_sequence",
        "Read the skeleton a sequence uses.",
        (TArray<FMCPParamSpec>{AssetPathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_animation_skeleton"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetAnimationSkeleton(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "get_rate_scale", "anim_sequence",
        "Read the playback rate scale of a sequence.",
        (TArray<FMCPParamSpec>{AssetPathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_rate_scale"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetRateScale(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "get_animated_bones", "anim_sequence",
        "List the skeleton bone names of a sequence (reference pose order).",
        (TArray<FMCPParamSpec>{AssetPathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_animated_bones"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetAnimatedBones(P); }); }));

    // --- sampling
    MCP_REGISTER_COMMAND(Registry, "get_bone_transform_at_time", "anim_sequence",
        "Sample one bone's local transform at a time (or frame) in the sequence.",
        SampleParams, MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_bone_transform_at_time"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetBoneTransform(P, false); }); }));
    MCP_REGISTER_COMMAND(Registry, "get_bone_transform_at_frame", "anim_sequence",
        "Sample one bone's local transform at a frame.",
        SampleParams, MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_bone_transform_at_frame"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetBoneTransform(P, true); }); }));

    MCP_REGISTER_COMMAND(Registry, "get_pose_at_time", "anim_sequence",
        "Sample the whole local-space poseor frame). Raw data, one entry per skeleton bone.",
        PoseParams, MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_pose_at_time"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetPose(P, false); }); }));
    MCP_REGISTER_COMMAND(Registry, "get_pose_at_frame", "anim_sequence",
        "Sample the whole local-space pose at a frame.",
        PoseParams, MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_pose_at_frame"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetPose(P, true); }); }));

    MCP_REGISTER_COMMAND(Registry, "get_root_motion_at_time", "anim_sequence",
        "Root motion accumulated from the start of the sequence up to a time.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("asset_path"), TEXT("string"), TEXT("Animation sequence asset path")),
            MCPParamOpt(TEXT("time"), TEXT("float"), TEXT("Time in seconds (or pass 'frame')")),
            MCPParamOpt(TEXT("frame"), TEXT("int"), TEXT("Frame number (alternative to 'time')")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_root_motion_at_time"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetRootMotionAtTime(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "get_total_root_motion", "anim_sequence",
        "Root motion accumulated over the whole sequence.",
        (TArray<FMCPParamSpec>{AssetPathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_total_root_motion"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetTotalRootMotion(P); }); }));

    // --- curves (reads)
    MCP_REGISTER_COMMAND(Registry, "list_curves", "anim_sequence",
        "List the float curves of a sequence.",
        (TArray<FMCPParamSpec>{AssetPathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("list_curves"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleListCurves(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "get_curve_info", "anim_sequence",
        "Read one curve's key count, default value and flags.",
        CurveParams, MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_curve_info"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetCurveInfo(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "get_curve_value_at_time", "anim_sequence",
        "Evaluate one curve at a time.",
        (TArray<FMCPParamSpec>{
            AssetPathParam(),
            MCPParam(TEXT("curve_name"), TEXT("string"), TEXT("Curve name")),
            MCPParam(TEXT("time"), TEXT("float"), TEXT("Time in seconds")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_curve_value_at_time"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetCurveValueAtTime(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "get_curve_keyframes", "anim_sequence",
        "List the keys of one curve.",
        CurveParams, MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_curve_keyframes"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetCurveKeyframes(P); }); }));

    // --- notifies and sync markers (reads)
    MCP_REGISTER_COMMAND(Registry, "list_notifies", "anim_sequence",
        "List every notify (instant and state) of a sequence.",
        (TArray<FMCPParamSpec>{AssetPathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("list_notifies"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleListNotifies(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "get_notify_info", "anim_sequence",
        "Read one notify, located by index, name or guid.",
        (TArray<FMCPParamSpec>{
            AssetPathParam(),
            LocatorParams[0],
            LocatorParams[1],
            LocatorParams[2],
            MCPParamOpt(TEXT("time"), TEXT("float"), TEXT("Trigger time, used to disambiguate a name")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_notify_info"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetNotifyInfo(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "list_notify_tracks", "anim_sequence",
        "List the notify tracks of a sequence (5.5 tracks carry real names).",
        (TArray<FMCPParamSpec>{AssetPathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("list_notify_tracks"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleListNotifyTracks(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "get_notify_track_count", "anim_sequence",
        "Read how many notify tracks a sequence has.",
        (TArray<FMCPParamSpec>{AssetPathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_notify_track_count"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetNotifyTrackCount(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "list_sync_markers", "anim_sequence",
        "List the authored sync markers of a sequence.",
        (TArray<FMCPParamSpec>{AssetPathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("list_sync_markers"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleListSyncMarkers(P); }); }));

    // --- settings (reads)
    // Registered one by one (not from a loop) so the registration stays greppable: the static
    // consistency check reads the registry macros literally.
    MCP_REGISTER_COMMAND(Registry, "get_additive_anim_type", "anim_sequence",
        "Read the additive animation type of a sequence.",
        (TArray<FMCPParamSpec>{AssetPathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_additive_anim_type"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetSetting(P, TEXT("additive_anim_type")); }); }));
    MCP_REGISTER_COMMAND(Registry, "get_additive_base_pose", "anim_sequence",
        "Read the additive base pose sequence of a sequence.",
        (TArray<FMCPParamSpec>{AssetPathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_additive_base_pose"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetSetting(P, TEXT("additive_base_pose")); }); }));
    MCP_REGISTER_COMMAND(Registry, "get_enable_root_motion", "anim_sequence",
        "Read whether root motion extraction is enabled.",
        (TArray<FMCPParamSpec>{AssetPathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_enable_root_motion"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetSetting(P, TEXT("enable_root_motion")); }); }));
    MCP_REGISTER_COMMAND(Registry, "get_root_motion_root_lock", "anim_sequence",
        "Read the root motion root lock mode.",
        (TArray<FMCPParamSpec>{AssetPathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_root_motion_root_lock"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetSetting(P, TEXT("root_motion_root_lock")); }); }));
    MCP_REGISTER_COMMAND(Registry, "get_force_root_lock", "anim_sequence",
        "Read whether the root lock is forced.",
        (TArray<FMCPParamSpec>{AssetPathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_force_root_lock"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetSetting(P, TEXT("force_root_lock")); }); }));

    MCP_REGISTER_COMMAND(Registry, "get_compression_info", "anim_sequence",
        "Read compression settings, raw/compressed size and whether the compressed data is valid.",
        (TArray<FMCPParamSpec>{AssetPathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_compression_info"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetCompressionInfo(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "get_source_files", "anim_sequence",
        "List the source files a sequence was imported from.",
        (TArray<FMCPParamSpec>{AssetPathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_source_files"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetSourceFiles(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "export_animation_to_json", "anim_sequence",
        "Dump a sequence's bones, curves, notifies and sync markers as one JSON document.",
        (TArray<FMCPParamSpec>{AssetPathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("export_animation_to_json"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleExportAnimationToJson(P); }); }));

    // --- writes: creation and properties
    MCP_REGISTER_COMMAND(Registry, "create_anim_sequence", "anim_sequence",
        "Create an animation sequence, optionally with authored bone tracks.",
        CreateParams, MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("create_anim_sequence"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleCreateAnimSequence(P, false); }); }));
    MCP_REGISTER_COMMAND(Registry, "create_anim_sequence_from_pose", "anim_sequence",
        "Create an animation sequence holding the skeleton reference pose on every bone track.",
        CreateParams, MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("create_anim_sequence_from_pose"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleCreateAnimSequence(P, true); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_animation_frame_rate", "anim_sequence",
        "Set the sampling frame rate of a sequence.",
        (TArray<FMCPParamSpec>{
            AssetPathParam(),
            MCPParam(TEXT("frame_rate"), TEXT("float"), TEXT("New sampling frame rate")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_animation_frame_rate"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetAnimationFrameRate(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "set_rate_scale", "anim_sequence",
        "Set the playback rate scale of a sequence.",
        (TArray<FMCPParamSpec>{
            AssetPathParam(),
            MCPParam(TEXT("rate_scale"), TEXT("float"), TEXT("New rate scale")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_rate_scale"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetRateScale(P); }); }));

    // --- writes: bone tracks
    MCP_REGISTER_COMMAND(Registry, "add_bone_track", "anim_sequence",
        "Add an empty bone track to a sequence.",
        (TArray<FMCPParamSpec>{
            AssetPathParam(),
            MCPParam(TEXT("bone_name"), TEXT("string"), TEXT("Bone name")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_bone_track"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddBoneTrack(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "remove_bone_track", "anim_sequence",
        "Remove a bone track from a sequence.",
        (TArray<FMCPParamSpec>{
            AssetPathParam(),
            MCPParam(TEXT("bone_name"), TEXT("string"), TEXT("Bone name")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("remove_bone_track"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRemoveBoneTrack(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "set_bone_track_keys", "anim_sequence",
        "Write the keyframes of one bone; bake_every_frame resamples them onto every frame.",
        (TArray<FMCPParamSpec>{
            AssetPathParam(),
            MCPParam(TEXT("bone_name"), TEXT("string"), TEXT("Bone name")),
            MCPParam(TEXT("keys"), TEXT("array"), TEXT("Keys [{time, position, rotation, scale}]")),
            MCPParamOpt(TEXT("bake_every_frame"), TEXT("bool"), TEXT("Resample onto every frame; default true")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_bone_track_keys"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetBoneTrackKeys(P); }); }));

    // --- writes: curves
    MCP_REGISTER_COMMAND(Registry, "add_curve", "anim_sequence",
        "Add an empty float curve to a sequence.",
        CurveParams, MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_curve"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddCurve(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "remove_curve", "anim_sequence",
        "Remove a float curve from a sequence.",
        CurveParams, MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("remove_curve"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRemoveCurve(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "set_curve_keys", "anim_sequence",
        "Write a curve's keys (replace_all=false appends to the existing keys).",
        (TArray<FMCPParamSpec>{
            AssetPathParam(),
            MCPParam(TEXT("curve_name"), TEXT("string"), TEXT("Curve name")),
            MCPParam(TEXT("keys"), TEXT("array"), TEXT("Keys [{time, value, arrive_tangent, leave_tangent, interp, tangent}]")),
            MCPParamOpt(TEXT("replace_all"), TEXT("bool"), TEXT("Replace every key; default true")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_curve_keys"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetCurveKeys(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "add_curve_key", "anim_sequence",
        "Append one key to an existing curve.",
        (TArray<FMCPParamSpec>{
            AssetPathParam(),
            MCPParam(TEXT("curve_name"), TEXT("string"), TEXT("Curve name")),
            MCPParam(TEXT("time"), TEXT("float"), TEXT("Key time in seconds")),
            MCPParam(TEXT("value"), TEXT("float"), TEXT("Key value")),
            MCPParamOpt(TEXT("interp"), TEXT("string"), TEXT("constant / linear / cubic; default cubic")),
            MCPParamOpt(TEXT("tangent"), TEXT("string"), TEXT("auto / user / break; default auto")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_curve_key"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddCurveKey(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "set_curve_metadata_flags", "anim_sequence",
        "Flag curve names as morph-target / material curves in the asset's skeleton curve metadata.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Skeleton, skeletal mesh or anim sequence asset path")),
            MCPParam(TEXT("curve_names"), TEXT("array"), TEXT("Curve names to flag")),
            MCPParamOpt(TEXT("morphtarget"), TEXT("bool"), TEXT("Set the morph target flag; default true")),
            MCPParamOpt(TEXT("material"), TEXT("bool"), TEXT("Set the material flag; default false")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save the skeleton after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_curve_metadata_flags"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetCurveMetadataFlags(P); }); }));

    // --- writes: notifies
    MCP_REGISTER_COMMAND(Registry, "add_notify", "anim_sequence",
        "Add an instant notify at a time, optionally with a concrete UAnimNotify class.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("asset_path"), TEXT("string"), TEXT("Animation sequence asset path")),
            MCPParamOpt(TEXT("notify_name"), TEXT("string"), TEXT("Notify name; defaults to the class name")),
            MCPParamOpt(TEXT("notify_class"), TEXT("string"), TEXT("UAnimNotify subclass (omit for a skeleton notify)")),
            MCPParam(TEXT("time"), TEXT("float"), TEXT("Trigger time in seconds")),
            MCPParamOpt(TEXT("track_index"), TEXT("int"), TEXT("Notify track index; default 0")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_notify"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddNotify(P, false); }); }));
    MCP_REGISTER_COMMAND(Registry, "add_notify_state", "anim_sequence",
        "Add a notify state (a notify with a duration).",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("asset_path"), TEXT("string"), TEXT("Animation sequence asset path")),
            MCPParamOpt(TEXT("notify_name"), TEXT("string"), TEXT("Notify name; defaults to the class name")),
            MCPParamOpt(TEXT("notify_class"), TEXT("string"), TEXT("UAnimNotifyState subclass")),
            MCPParam(TEXT("time"), TEXT("float"), TEXT("Start time in seconds")),
            MCPParamOpt(TEXT("duration"), TEXT("float"), TEXT("State duration in seconds")),
            MCPParamOpt(TEXT("track_index"), TEXT("int"), TEXT("Notify track index; default 0")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_notify_state"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddNotify(P, true); }); }));
    MCP_REGISTER_COMMAND(Registry, "remove_notify", "anim_sequence",
        "Remove one notify, located by index, name or guid.",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("asset_path"), TEXT("string"), TEXT("Animation sequence asset path")),
            LocatorParams[0],
            LocatorParams[1],
            LocatorParams[2],
            MCPParamOpt(TEXT("time"), TEXT("float"), TEXT("Trigger time, used to disambiguate a name")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("remove_notify"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRemoveNotify(P); }); }));

    // One command per writable notify field. Each is registered with its own literal name and
    // its own handler, so the registry stays the single greppable declaration site.
    MCP_REGISTER_COMMAND(Registry, "set_notify_trigger_time", "anim_sequence",
        "Move a notify to a new trigger time.",
        NotifyWriteParams(), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_notify_trigger_time"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetNotifyProperty(P, TEXT("trigger_time")); }); }));
    MCP_REGISTER_COMMAND(Registry, "set_notify_duration", "anim_sequence",
        "Set the duration of a notify state.",
        NotifyWriteParams(), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_notify_duration"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetNotifyProperty(P, TEXT("duration")); }); }));
    MCP_REGISTER_COMMAND(Registry, "set_notify_track", "anim_sequence",
        "Move a notify to another notify track.",
        NotifyWriteParams(), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_notify_track"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetNotifyProperty(P, TEXT("track_index")); }); }));
    MCP_REGISTER_COMMAND(Registry, "set_notify_name", "anim_sequence",
        "Rename a notify.",
        NotifyWriteParams(), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_notify_name"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetNotifyProperty(P, TEXT("name")); }); }));
    MCP_REGISTER_COMMAND(Registry, "set_notify_color", "anim_sequence",
        "Set a notify's editor colour (#RRGGBB or [r,g,b,a]).",
        NotifyWriteParams(), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_notify_color"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetNotifyProperty(P, TEXT("color")); }); }));
    MCP_REGISTER_COMMAND(Registry, "set_notify_trigger_chance", "anim_sequence",
        "Set the trigger chance in [0,1].",
        NotifyWriteParams(), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_notify_trigger_chance"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetNotifyProperty(P, TEXT("trigger_chance")); }); }));
    MCP_REGISTER_COMMAND(Registry, "set_notify_trigger_on_server", "anim_sequence",
        "Whether the notify triggers on dedicated servers.",
        NotifyWriteParams(), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_notify_trigger_on_server"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetNotifyProperty(P, TEXT("trigger_on_server")); }); }));
    MCP_REGISTER_COMMAND(Registry, "set_notify_trigger_on_follower", "anim_sequence",
        "Whether the notify triggers for sync group followers.",
        NotifyWriteParams(), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_notify_trigger_on_follower"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetNotifyProperty(P, TEXT("trigger_on_follower")); }); }));
    MCP_REGISTER_COMMAND(Registry, "set_notify_trigger_weight_threshold", "anim_sequence",
        "Set the blend weight threshold in [0,1].",
        NotifyWriteParams(), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_notify_trigger_weight_threshold"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetNotifyProperty(P, TEXT("trigger_weight_threshold")); }); }));
    MCP_REGISTER_COMMAND(Registry, "set_notify_lod_filter", "anim_sequence",
        "Set the LOD filter ('none'/'lod') and its LOD level.",
        NotifyWriteParams(), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_notify_lod_filter"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetNotifyProperty(P, TEXT("lod_filter")); }); }));

    // --- writes: notify tracks and sync markers
    MCP_REGISTER_COMMAND(Registry, "add_notify_track", "anim_sequence",
        "Add a notify track to a sequence.",
        (TArray<FMCPParamSpec>{
            AssetPathParam(),
            MCPParamOpt(TEXT("name"), TEXT("string"), TEXT("Track name; defaults to the next number")),
            MCPParamOpt(TEXT("color"), TEXT("object"), TEXT("Track colour (#RRGGBB or [r,g,b,a])")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_notify_track"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddNotifyTrack(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "rename_notify_track", "anim_sequence",
        "Rename a notify track (5.5 tracks have real names).",
        (TArray<FMCPParamSpec>{
            AssetPathParam(),
            MCPParamOpt(TEXT("track_index"), TEXT("int"), TEXT("Track index")),
            MCPParamOpt(TEXT("track_name"), TEXT("string"), TEXT("Current track name (alternative to the index)")),
            MCPParam(TEXT("name"), TEXT("string"), TEXT("New track name")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("rename_notify_track"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRenameNotifyTrack(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "remove_notify_track", "anim_sequence",
        "Remove a notify track; notifies on it move to track 0.",
        (TArray<FMCPParamSpec>{
            AssetPathParam(),
            MCPParamOpt(TEXT("track_index"), TEXT("int"), TEXT("Track index")),
            MCPParamOpt(TEXT("track_name"), TEXT("string"), TEXT("Track name (alternative to the index)")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("remove_notify_track"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRemoveNotifyTrack(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "add_sync_marker", "anim_sequence",
        "Add a sync marker at a time.",
        (TArray<FMCPParamSpec>{
            AssetPathParam(),
            MCPParam(TEXT("marker_name"), TEXT("string"), TEXT("Marker name")),
            MCPParam(TEXT("time"), TEXT("float"), TEXT("Marker time in seconds")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_sync_marker"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddSyncMarker(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "remove_sync_marker", "anim_sequence",
        "Remove a sync marker, located by index or by name (plus optional time).",
        (TArray<FMCPParamSpec>{
            AssetPathParam(),
            MCPParamOpt(TEXT("marker_name"), TEXT("string"), TEXT("Marker name")),
            MCPParamOpt(TEXT("marker_index"), TEXT("int"), TEXT("Marker index")),
            MCPParamOpt(TEXT("time"), TEXT("float"), TEXT("Marker time, used to disambiguate a name")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("remove_sync_marker"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRemoveSyncMarker(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "set_sync_marker_time", "anim_sequence",
        "Move a sync marker, located by index or by name (plus optional time).",
        (TArray<FMCPParamSpec>{
            AssetPathParam(),
            MCPParamOpt(TEXT("marker_name"), TEXT("string"), TEXT("Marker name")),
            MCPParamOpt(TEXT("marker_index"), TEXT("int"), TEXT("Marker index")),
            MCPParamOpt(TEXT("time"), TEXT("float"), TEXT("Current marker time, used to disambiguate a name")),
            MCPParam(TEXT("new_time"), TEXT("float"), TEXT("New marker time in seconds")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_sync_marker_time"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetSyncMarkerTime(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "set_sync_marker_time_by_name", "anim_sequence",
        "Move a sync marker addressed by its name (plus its current time when the name repeats).",
        (TArray<FMCPParamSpec>{
            AssetPathParam(),
            MCPParam(TEXT("marker_name"), TEXT("string"), TEXT("Marker name")),
            MCPParam(TEXT("new_time"), TEXT("float"), TEXT("New marker time in seconds")),
            MCPParamOpt(TEXT("time"), TEXT("float"), TEXT("Current marker time, used to disambiguate the name")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_sync_marker_time_by_name"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetSyncMarkerTime(P); }); }));

    // --- writes: additive, root motion, compression
    MCP_REGISTER_COMMAND(Registry, "set_additive_anim_type", "anim_sequence",
        "Set the additive animation type (None / LocalSpace / MeshSpace).",
        (TArray<FMCPParamSpec>{
            AssetPathParam(),
            MCPParam(TEXT("type"), TEXT("string"), TEXT("Additive type")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_additive_anim_type"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetAdditiveAnimType(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "set_additive_base_pose", "anim_sequence",
        "Set the additive base pose sequence (empty path = reference pose).",
        (TArray<FMCPParamSpec>{
            AssetPathParam(),
            MCPParamOpt(TEXT("base_pose"), TEXT("string"), TEXT("Base pose sequence path; empty for the reference pose")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_additive_base_pose"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetAdditiveBasePose(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "set_enable_root_motion", "anim_sequence",
        "Enable or disable root motion extraction.",
        (TArray<FMCPParamSpec>{
            AssetPathParam(),
            MCPParam(TEXT("enable"), TEXT("bool"), TEXT("Whether root motion extraction is enabled")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_enable_root_motion"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetEnableRootMotion(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "set_root_motion_root_lock", "anim_sequence",
        "Set the root motion root lock mode (RefPose / AnimFirstFrame / Zero).",
        (TArray<FMCPParamSpec>{
            AssetPathParam(),
            MCPParam(TEXT("lock_type"), TEXT("string"), TEXT("Root lock mode")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_root_motion_root_lock"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetRootMotionRootLock(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "set_force_root_lock", "anim_sequence",
        "Force the root lock even without root motion.",
        (TArray<FMCPParamSpec>{
            AssetPathParam(),
            MCPParam(TEXT("force"), TEXT("bool"), TEXT("Whether the root lock is forced")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_force_root_lock"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetForceRootLock(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "set_compression_scheme", "anim_sequence",
        "Assign a bone compression settings asset to a sequence.",
        (TArray<FMCPParamSpec>{
            AssetPathParam(),
            MCPParam(TEXT("compression_scheme"), TEXT("string"), TEXT("UAnimBoneCompressionSettings asset path")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_compression_scheme"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetCompressionScheme(P); }); }));
    MCP_REGISTER_COMMAND(Registry, "compress_animation", "anim_sequence",
        "Recompress a sequence synchronously and report the resulting compressed state.",
        (TArray<FMCPParamSpec>{AssetPathParam(),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("compress_animation"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleCompressAnimation(P); }); }));

    // =======================================================================
    // AnimMontage (category anim_montage). The notify commands are shared with
    // the sequence category and dispatch on the asset type - see ResolveAnimationHost.
    // =======================================================================

    // --- montage: discovery and creation
    MCP_REGISTER_COMMAND(Registry, "list_montages", "anim_montage",
        "List animation montage assets below a folder (registry metadata only, assets are not loaded).",
        (TArray<FMCPParamSpec>{
            MCPParamOpt(TEXT("search_path"), TEXT("string"), TEXT("Content folder to search; default /Game")),
            MCPParamOpt(TEXT("skeleton"), TEXT("string"), TEXT("Only montages using this skeleton")),
            MCPParamOpt(TEXT("max_results"), TEXT("int"), TEXT("Upper limit on returned entries; default 200")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("list_montages"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleListMontages(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "get_montage_info", "anim_montage",
        "Full summary of one montage (length, sections, slot tracks, segments, notifies, blend, root motion).",
        (TArray<FMCPParamSpec>{MontagePathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_montage_info"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetMontageInfo(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "find_montages_for_skeleton", "anim_montage",
        "List the montages that use a skeleton.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("skeleton"), TEXT("string"), TEXT("Skeleton asset path")),
            MCPParamOpt(TEXT("search_path"), TEXT("string"), TEXT("Content folder to search; default /Game")),
            MCPParamOpt(TEXT("max_results"), TEXT("int"), TEXT("Upper limit on returned entries; default 200")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("find_montages_for_skeleton"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleFindMontagesForSkeleton(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "find_montages_using_animation", "anim_montage",
        "List the montages that play an animation (registry referencers first, bounded scan as fallback).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("anim_path"), TEXT("string"), TEXT("Animation asset path")),
            MCPParamOpt(TEXT("max_results"), TEXT("int"), TEXT("Upper limit on returned entries; default 200")),
            MCPParamOpt(TEXT("max_scan"), TEXT("int"), TEXT("Montages to load when the registry has no dependency data; default 100")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("find_montages_using_animation"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleFindMontagesUsingAnimation(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "get_montage_length", "anim_montage",
        "Read the play length of a montage in seconds.",
        (TArray<FMCPParamSpec>{MontagePathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_montage_length"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetMontageLength(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "get_montage_skeleton", "anim_montage",
        "Read the skeleton a montage uses.",
        (TArray<FMCPParamSpec>{MontagePathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_montage_skeleton"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetMontageSkeleton(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "create_montage_from_animation", "anim_montage",
        "Create a montage that plays one animation; the result always has a Default section.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("anim_path"), TEXT("string"), TEXT("Animation sequence to play")),
            MCPParam(TEXT("name"), TEXT("string"), TEXT("Asset name")),
            MCPParamOpt(TEXT("folder"), TEXT("string"), TEXT("Content folder; default /Game")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("create_montage_from_animation"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleCreateMontageFromAnimation(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "create_empty_montage", "anim_montage",
        "Create an empty montage on a skeleton (one Default section, one DefaultSlot track).",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("name"), TEXT("string"), TEXT("Asset name")),
            MCPParam(TEXT("skeleton"), TEXT("string"), TEXT("Skeleton asset path")),
            MCPParamOpt(TEXT("folder"), TEXT("string"), TEXT("Content folder; default /Game")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("create_empty_montage"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleCreateEmptyMontage(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "duplicate_montage", "anim_montage",
        "Duplicate a montage asset under a new name.",
        (TArray<FMCPParamSpec>{
            MCPParam(TEXT("asset_path"), TEXT("string"), TEXT("Montage to copy")),
            MCPParam(TEXT("name"), TEXT("string"), TEXT("Name of the copy")),
            MCPParamOpt(TEXT("folder"), TEXT("string"), TEXT("Content folder; default /Game")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("duplicate_montage"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleDuplicateMontage(P); }); }));

    // --- montage: sections (reads)
    MCP_REGISTER_COMMAND(Registry, "list_sections", "anim_montage",
        "List the composite sections of a montage in time order.",
        (TArray<FMCPParamSpec>{MontagePathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("list_sections"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleListSections(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "get_section_info", "anim_montage",
        "Read one section by name (or by index).",
        SectionLocatorParams, MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_section_info"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetSectionInfo(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "get_section_index_at_time", "anim_montage",
        "Index of the section that contains a montage time.",
        (TArray<FMCPParamSpec>{
            MontagePathParam(),
            MCPParam(TEXT("time"), TEXT("float"), TEXT("Time in seconds")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_section_index_at_time"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetSectionAtTime(P, false); }); }));

    MCP_REGISTER_COMMAND(Registry, "get_section_name_at_time", "anim_montage",
        "Name of the section that contains a montage time.",
        (TArray<FMCPParamSpec>{
            MontagePathParam(),
            MCPParam(TEXT("time"), TEXT("float"), TEXT("Time in seconds")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_section_name_at_time"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetSectionAtTime(P, true); }); }));

    MCP_REGISTER_COMMAND(Registry, "get_section_length", "anim_montage",
        "Length of one section in seconds.",
        SectionLocatorParams, MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_section_length"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetSectionLength(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "get_next_section", "anim_montage",
        "Read the next section of one section.",
        SectionLocatorParams, MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_next_section"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetNextSection(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "get_all_section_links", "anim_montage",
        "List every (section -> next section) link the montage has.",
        (TArray<FMCPParamSpec>{MontagePathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_all_section_links"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetAllSectionLinks(P); }); }));

    // --- montage: sections (writes; the structural ones close the montage editor first)
    MCP_REGISTER_COMMAND(Registry, "add_section", "anim_montage",
        "Add a section at a start time; the sections are re-sorted by time.",
        (TArray<FMCPParamSpec>{
            MontagePathParam(),
            MCPParam(TEXT("section_name"), TEXT("string"), TEXT("New section name")),
            MCPParam(TEXT("start_time"), TEXT("float"), TEXT("Section start time in seconds")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_section"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddSection(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "remove_section", "anim_montage",
        "Remove one section; the montage keeps at least one section.",
        SectionLocatorParams, MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("remove_section"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRemoveSection(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "rename_section", "anim_montage",
        "Rename one section; section links that pointed at the old name follow it.",
        (TArray<FMCPParamSpec>{
            MontagePathParam(),
            MCPParamOpt(TEXT("section_name"), TEXT("string"), TEXT("Current section name")),
            MCPParamOpt(TEXT("section_index"), TEXT("int"), TEXT("Section index")),
            MCPParam(TEXT("new_name"), TEXT("string"), TEXT("New section name")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("rename_section"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRenameSection(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_section_start_time", "anim_montage",
        "Move one section's start time.",
        (TArray<FMCPParamSpec>{
            MontagePathParam(),
            MCPParamOpt(TEXT("section_name"), TEXT("string"), TEXT("Section name")),
            MCPParamOpt(TEXT("section_index"), TEXT("int"), TEXT("Section index")),
            MCPParam(TEXT("time"), TEXT("float"), TEXT("New start time in seconds")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_section_start_time"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetSectionStartTime(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_next_section", "anim_montage",
        "Set the section that follows one section (empty clears the link).",
        (TArray<FMCPParamSpec>{
            MontagePathParam(),
            MCPParamOpt(TEXT("section_name"), TEXT("string"), TEXT("Section name")),
            MCPParamOpt(TEXT("section_index"), TEXT("int"), TEXT("Section index")),
            MCPParamOpt(TEXT("next_section"), TEXT("string"), TEXT("Next section name; empty clears the link")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_next_section"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetNextSection(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_section_loop", "anim_montage",
        "Make a section loop on itself, or stop it looping.",
        (TArray<FMCPParamSpec>{
            MontagePathParam(),
            MCPParamOpt(TEXT("section_name"), TEXT("string"), TEXT("Section name")),
            MCPParamOpt(TEXT("section_index"), TEXT("int"), TEXT("Section index")),
            MCPParam(TEXT("loop"), TEXT("bool"), TEXT("Whether the section loops on itself")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_section_loop"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetSectionLoop(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "clear_section_link", "anim_montage",
        "Clear the next-section link of one section.",
        SectionLocatorParams, MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("clear_section_link"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleClearSectionLink(P); }); }));

    // --- montage: slot tracks
    MCP_REGISTER_COMMAND(Registry, "list_slot_tracks", "anim_montage",
        "List the slot tracks of a montage.",
        (TArray<FMCPParamSpec>{MontagePathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("list_slot_tracks"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleListSlotTracks(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "get_slot_track_info", "anim_montage",
        "Read one slot track by index or by name.",
        SlotLocatorParams, MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_slot_track_info"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetSlotTrackInfo(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "add_slot_track", "anim_montage",
        "Add a slot track to a montage.",
        (TArray<FMCPParamSpec>{
            MontagePathParam(),
            MCPParam(TEXT("slot_name"), TEXT("string"), TEXT("Slot name")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_slot_track"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddSlotTrack(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "remove_slot_track", "anim_montage",
        "Remove one slot track; the montage keeps at least one.",
        SlotLocatorParams, MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("remove_slot_track"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRemoveSlotTrack(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_slot_name", "anim_montage",
        "Rename one slot track.",
        (TArray<FMCPParamSpec>{
            MontagePathParam(),
            MCPParamOpt(TEXT("track_index"), TEXT("int"), TEXT("Slot track index")),
            MCPParamOpt(TEXT("slot_name"), TEXT("string"), TEXT("Current slot name")),
            MCPParam(TEXT("new_name"), TEXT("string"), TEXT("New slot name")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_slot_name"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetSlotName(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "get_all_used_slot_names", "anim_montage",
        "Slot names the montage uses, plus the slot names the skeleton defines.",
        (TArray<FMCPParamSpec>{MontagePathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_all_used_slot_names"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetAllUsedSlotNames(P); }); }));

    // --- montage: animation segments
    MCP_REGISTER_COMMAND(Registry, "list_anim_segments", "anim_montage",
        "List the animation segments of one slot track.",
        SlotLocatorParams, MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("list_anim_segments"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleListAnimSegments(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "get_anim_segment_info", "anim_montage",
        "Read one animation segment.",
        SegmentLocatorParams, MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_anim_segment_info"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetAnimSegmentInfo(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "add_anim_segment", "anim_montage",
        "Add an animation segment to a slot track (start_time / play_rate / loop_count optional).",
        (TArray<FMCPParamSpec>{
            MontagePathParam(),
            MCPParamOpt(TEXT("track_index"), TEXT("int"), TEXT("Slot track index")),
            MCPParamOpt(TEXT("slot_name"), TEXT("string"), TEXT("Slot name (preferred over the index)")),
            MCPParam(TEXT("anim_path"), TEXT("string"), TEXT("Animation to play")),
            MCPParamOpt(TEXT("start_time"), TEXT("float"), TEXT("Position in the montage; default 0")),
            MCPParamOpt(TEXT("play_rate"), TEXT("float"), TEXT("Segment play rate; default 1")),
            MCPParamOpt(TEXT("loop_count"), TEXT("int"), TEXT("Loops of the animation; default 1")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_anim_segment"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddAnimSegment(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "remove_anim_segment", "anim_montage",
        "Remove one animation segment from a slot track.",
        SegmentLocatorParams, MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("remove_anim_segment"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRemoveAnimSegment(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_segment_start_time", "anim_montage",
        "Move one segment's position in the montage.",
        SegmentValueParams(), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_segment_start_time"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetSegmentProperty(P, TEXT("start_time")); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_segment_play_rate", "anim_montage",
        "Set one segment's play rate.",
        SegmentValueParams(), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_segment_play_rate"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetSegmentProperty(P, TEXT("play_rate")); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_segment_start_position", "anim_montage",
        "Set the time inside the animation one segment starts at.",
        SegmentValueParams(), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_segment_start_position"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetSegmentProperty(P, TEXT("start_position")); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_segment_end_position", "anim_montage",
        "Set the time inside the animation one segment ends at.",
        SegmentValueParams(), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_segment_end_position"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetSegmentProperty(P, TEXT("end_position")); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_segment_loop_count", "anim_montage",
        "Set how often one segment loops its animation.",
        SegmentValueParams(), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_segment_loop_count"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetSegmentProperty(P, TEXT("loop_count")); }); }));

    // --- montage: notifies (montage-only) and branching points
    MCP_REGISTER_COMMAND(Registry, "set_notify_link_to_section", "anim_montage",
        "Move a montage notify to a section start (what linking a notify to a section means).",
        (TArray<FMCPParamSpec>{
            MontagePathParam(),
            MCPParamOpt(TEXT("notify_index"), TEXT("int"), TEXT("Notify index as returned by list_notifies")),
            MCPParamOpt(TEXT("notify_name"), TEXT("string"), TEXT("Notify name (preferred over the index)")),
            MCPParamOpt(TEXT("guid"), TEXT("string"), TEXT("Editor guid of the notify")),
            MCPParamOpt(TEXT("time"), TEXT("float"), TEXT("Trigger time, used to disambiguate a name")),
            MCPParam(TEXT("section"), TEXT("string"), TEXT("Section to link the notify to")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_notify_link_to_section"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetMontageNotifyProperty(P, TEXT("link_to_section")); }); }));

    MCP_REGISTER_COMMAND(Registry, "list_branching_points", "anim_montage",
        "List the branching points of a montage (notifies whose montage tick type is BranchingPoint).",
        (TArray<FMCPParamSpec>{MontagePathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("list_branching_points"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleListBranchingPoints(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "add_branching_point", "anim_montage",
        "Add a branching point to a montage.",
        (TArray<FMCPParamSpec>{
            MontagePathParam(),
            MCPParam(TEXT("name"), TEXT("string"), TEXT("Branching point name")),
            MCPParam(TEXT("trigger_time"), TEXT("float"), TEXT("Trigger time in seconds")),
            MCPParamOpt(TEXT("track_index"), TEXT("int"), TEXT("Notify track index; default 0")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("add_branching_point"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleAddBranchingPoint(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "remove_branching_point", "anim_montage",
        "Remove one branching point (a plain notify at the same position is not touched).",
        (TArray<FMCPParamSpec>{
            MontagePathParam(),
            MCPParam(TEXT("branching_point_index"), TEXT("int"), TEXT("Index among the branching points, from list_branching_points")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("remove_branching_point"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRemoveBranchingPoint(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "is_branching_point_at_time", "anim_montage",
        "Whether a branching point sits at a montage time.",
        (TArray<FMCPParamSpec>{
            MontagePathParam(),
            MCPParam(TEXT("time"), TEXT("float"), TEXT("Time in seconds")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("is_branching_point_at_time"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleIsBranchingPointAtTime(P); }); }));

    // --- montage: blend settings and root motion
    MCP_REGISTER_COMMAND(Registry, "set_blend_in", "anim_montage",
        "Set the blend-in time and/or curve of a montage.",
        (TArray<FMCPParamSpec>{
            MontagePathParam(),
            MCPParamOpt(TEXT("blend_time"), TEXT("float"), TEXT("Blend time in seconds")),
            MCPParamOpt(TEXT("blend_option"), TEXT("string"), TEXT("Alpha blend option, e.g. Linear / Cubic")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_blend_in"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetBlend(P, true); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_blend_out", "anim_montage",
        "Set the blend-out time and/or curve of a montage.",
        (TArray<FMCPParamSpec>{
            MontagePathParam(),
            MCPParamOpt(TEXT("blend_time"), TEXT("float"), TEXT("Blend time in seconds")),
            MCPParamOpt(TEXT("blend_option"), TEXT("string"), TEXT("Alpha blend option, e.g. Linear / Cubic")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_blend_out"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetBlend(P, false); }); }));

    MCP_REGISTER_COMMAND(Registry, "get_blend_settings", "anim_montage",
        "Read the blend in/out settings of a montage.",
        (TArray<FMCPParamSpec>{MontagePathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_blend_settings"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetBlendSettings(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_blend_out_trigger_time", "anim_montage",
        "Set the time from the end at which the montage starts to blend out (negative = use the blend-out time).",
        (TArray<FMCPParamSpec>{
            MontagePathParam(),
            MCPParam(TEXT("trigger_time"), TEXT("float"), TEXT("Seconds before the end; negative uses the blend-out time")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_blend_out_trigger_time"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetBlendOutTriggerTime(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "get_enable_root_motion_translation", "anim_montage",
        "Read whether the montage allows root motion translation.",
        (TArray<FMCPParamSpec>{MontagePathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_enable_root_motion_translation"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetMontageRootMotionSetting(P, true); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_enable_root_motion_translation", "anim_montage",
        "Allow or forbid root motion translation on the montage.",
        (TArray<FMCPParamSpec>{
            MontagePathParam(),
            MCPParam(TEXT("enable"), TEXT("bool"), TEXT("Whether root motion translation is allowed")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_enable_root_motion_translation"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetMontageRootMotion(P, true); }); }));

    MCP_REGISTER_COMMAND(Registry, "get_enable_root_motion_rotation", "anim_montage",
        "Read whether the montage allows root motion rotation.",
        (TArray<FMCPParamSpec>{MontagePathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("get_enable_root_motion_rotation"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleGetMontageRootMotionSetting(P, false); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_enable_root_motion_rotation", "anim_montage",
        "Allow or forbid root motion rotation on the montage.",
        (TArray<FMCPParamSpec>{
            MontagePathParam(),
            MCPParam(TEXT("enable"), TEXT("bool"), TEXT("Whether root motion rotation is allowed")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save after the command; default true. Pass false to batch writes and flush once yourself.")),
        }), MCPFlags(false, true, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_enable_root_motion_rotation"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetMontageRootMotion(P, false); }); }));

    // =======================================================================
    // Editor session and preview. These commands change what the editor shows,
    // not what the asset holds, so they neither transact nor save.
    // =======================================================================

    MCP_REGISTER_COMMAND(Registry, "open_animation_editor", "anim_sequence",
        "Open the asset editor (Persona) of a sequence or montage and report whether its preview is live.",
        (TArray<FMCPParamSpec>{AssetPathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("open_animation_editor"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleOpenAnimationEditor(P, false); }); }));

    MCP_REGISTER_COMMAND(Registry, "open_montage_editor", "anim_montage",
        "Open the montage editor of a montage and report whether its preview is live.",
        (TArray<FMCPParamSpec>{MontagePathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("open_montage_editor"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleOpenAnimationEditor(P, true); }); }));

    MCP_REGISTER_COMMAND(Registry, "refresh_montage_editor", "anim_montage",
        "Close and reopen the montage editor of a montage so the tab re-reads the asset.",
        (TArray<FMCPParamSpec>{MontagePathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("refresh_montage_editor"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleRefreshMontageEditor(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_preview_time", "anim_sequence",
        "Move the preview of a sequence or montage to a time; the response reads the preview back.",
        (TArray<FMCPParamSpec>{
            AssetPathParam(),
            MCPParam(TEXT("time"), TEXT("float"), TEXT("Preview time in seconds")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_preview_time"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetPreviewTime(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "play_preview", "anim_sequence",
        "Start (or resume) the preview of a sequence or montage.",
        (TArray<FMCPParamSpec>{
            AssetPathParam(),
            MCPParamOpt(TEXT("loop"), TEXT("bool"), TEXT("Whether the preview loops; sequences only")),
            MCPParamOpt(TEXT("play_rate"), TEXT("float"), TEXT("Preview play rate; sequences only")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("play_preview"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandlePlayPreview(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "stop_preview", "anim_sequence",
        "Pause the preview of a sequence or montage where it currently is.",
        (TArray<FMCPParamSpec>{AssetPathParam()}), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("stop_preview"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleStopPreview(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "jump_to_section", "anim_montage",
        "Jump the montage preview to a section, restoring the play state it had before.",
        (TArray<FMCPParamSpec>{
            MontagePathParam(),
            MCPParam(TEXT("section"), TEXT("string"), TEXT("Section to jump to")),
        }), MCPFlags(false, false, false),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("jump_to_section"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleJumpToSection(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "finalize_blend_space", "anim_sequence",
        "Force a BlendSpace's runtime data (segment/triangle table) to be built and verify it by reading the samples back, then save. "
        "A BlendSpace whose SampleData was written by script has no runtime data at all - the engine only rebuilds it from an asset editor - "
        "so the blend output is empty (a T-pose) until this runs.",
        (TArray<FMCPParamSpec>{
            AssetPathParam(),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save the asset after a successful finalize; default true")),
        }), MCPFlags(false, false, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("finalize_blend_space"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleFinalizeBlendSpace(P); }); }));

    MCP_REGISTER_COMMAND(Registry, "set_blend_space_samples", "anim_sequence",
        "Write a BlendSpace's sample list and (unless finalize=false) build the runtime segment/triangle table. "
        "The array is replaced whole, so the request IS the final sample list; the reply reads the samples back from the "
        "asset, so 'saved but not persisted' and 'persisted but no runtime data' cannot pass unnoticed.",
        (TArray<FMCPParamSpec>{
            AssetPathParam(),
            MCPParam(TEXT("samples"), TEXT("array"),
                TEXT("Whole sample list [{animation: <asset path>|null, sample_value: {x, y, z}}, ...]")),
            MCPParamOpt(TEXT("finalize"), TEXT("bool"), TEXT("Rebuild the runtime data after writing; default true")),
            MCPParamOpt(TEXT("persist"), TEXT("bool"), TEXT("Save the asset after a successful write; default true")),
        }), MCPFlags(false, true, false, true),
        ([this](const TSharedPtr<FJsonObject>& Params) { return RunCommand(TEXT("set_blend_space_samples"), Params, [this](const TSharedPtr<FJsonObject>& P) { return HandleSetBlendSpaceSamples(P); }); }));
}

// ---------------------------------------------------------------------------
// BlendSpace runtime data
// ---------------------------------------------------------------------------

namespace
{
    struct FBlendSpaceValidity
    {
        int32 SampleCount = 0;
        int32 ValidSampleCount = 0;
        int32 TriangleCount = 0;
        TArray<FString> InvalidSamples;
    };

    FBlendSpaceValidity ReadBlendSpaceValidity(UBlendSpace* BlendSpace)
    {
        FBlendSpaceValidity Validity;
        if (!BlendSpace)
        {
            return Validity;
        }

        // GetBlendSamples() is the public view of the protected SampleData array.
        const TArray<FBlendSample>& Samples = BlendSpace->GetBlendSamples();
        Validity.SampleCount = Samples.Num();
        for (const FBlendSample& Sample : Samples)
        {
            if (Sample.bIsValid)
            {
                ++Validity.ValidSampleCount;
            }
            else
            {
                Validity.InvalidSamples.Add(Sample.Animation ? Sample.Animation->GetName() : FString(TEXT("<no animation>")));
            }
        }
        Validity.TriangleCount = BlendSpace->GetBlendSpaceData().Triangles.Num();
        return Validity;
    }

    /** Ready for the runtime: every sample usable and a triangle table built. */
    bool IsBlendSpaceFinalized(const FBlendSpaceValidity& In)
    {
        return In.SampleCount > 0 && In.ValidSampleCount == In.SampleCount && In.TriangleCount > 0;
    }

    /**
     * Slots a full cartesian coverage would have: the product of the distinct values per axis that
     * actually varies (an axis with a single value is not a blend axis).
     *
     * Derived from the samples rather than from `BlendParameters[].GridNum` on purpose. GridNum is the
     * grid the EDITOR snaps to, not what is authored: BS_Fei_Locomotion declares 8/4/4 (=225 slots) while
     * its authored coverage is 9x3 = 27, so a GridNum-based check reports a mismatch on a perfectly
     * healthy asset - i.e. it would be noise on every normal asset.
     */
    int32 ComputeSampleCoverage(const UBlendSpace* BlendSpace, TArray<int32>& OutValuesPerAxis)
    {
        OutValuesPerAxis.Reset();
        if (!BlendSpace)
        {
            return 0;
        }

        const TArray<FBlendSample>& Samples = BlendSpace->GetBlendSamples();
        int32 Product = 1;
        int32 ActiveAxes = 0;

        for (int32 Axis = 0; Axis < 3; ++Axis)
        {
            TSet<int32> DistinctValues;
            for (const FBlendSample& Sample : Samples)
            {
                const double Value = Axis == 0 ? Sample.SampleValue.X
                    : (Axis == 1 ? Sample.SampleValue.Y : Sample.SampleValue.Z);
                // Hundredths of a unit: sample values are authored on grid steps, so rounding there keeps
                // float noise from reading as a new distinct value.
                DistinctValues.Add(FMath::RoundToInt(Value * 100.0));
            }

            OutValuesPerAxis.Add(DistinctValues.Num());
            if (DistinctValues.Num() > 1)
            {
                Product *= DistinctValues.Num();
                ++ActiveAxes;
            }
        }

        return ActiveAxes > 0 ? Product : 0;
    }

    /** "" when the samples cover a full grid, otherwise a one-line description of how they do not. */
    FString DescribeGridMismatch(const UBlendSpace* BlendSpace, int32 SampleCount)
    {
        TArray<int32> ValuesPerAxis;
        const int32 Expected = ComputeSampleCoverage(BlendSpace, ValuesPerAxis);
        if (Expected <= 0 || Expected == SampleCount)
        {
            return FString();
        }

        TArray<FString> Parts;
        for (const int32 ValueCount : ValuesPerAxis)
        {
            Parts.Add(FString::FromInt(ValueCount));
        }
        return FString::Printf(
            TEXT("%d samples where the authored axis values imply a full grid of %d (%s distinct values per axis)"),
            SampleCount, Expected, *FString::Join(Parts, TEXT("/")));
    }

    /**
     * Build the runtime segment/triangle table, and read it back.
     *
     * Rebuilding happens in the asset editor's construction path (SAnimationBlendSpace.cpp "Force a
     * resampling of the data on construction"), which SampleData written by script never sees: the table
     * stays empty, the blend output is nothing, i.e. a T-pose. ValidateSampleData + ResampleData are the
     * same two calls that path makes, so they run directly first and the editor stays the fallback.
     *
     * bOutEditorOpened reports an editor this call opened - the caller owns closing it.
     */
    void FinalizeBlendSpaceInPlace(UBlendSpace* BlendSpace, FBlendSpaceValidity& OutValidity,
                                   FString& OutResampleSource, bool& bOutEditorOpened)
    {
        bOutEditorOpened = false;
        OutResampleSource = TEXT("direct");

        BlendSpace->ValidateSampleData();
        BlendSpace->ResampleData();
        OutValidity = ReadBlendSpaceValidity(BlendSpace);

        UAssetEditorSubsystem* EditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
        if (IsBlendSpaceFinalized(OutValidity) || !EditorSubsystem)
        {
            return;
        }

        OutResampleSource = TEXT("editor");
        EditorSubsystem->OpenEditorForAsset(BlendSpace, EToolkitMode::Standalone, TSharedPtr<IToolkitHost>(),
            /*bShowProgressWindow=*/ false);
        bOutEditorOpened = EditorSubsystem->FindEditorForAsset(BlendSpace, /*bFocusIfOpen=*/ false) != nullptr;

        // The editor's own construction may not have run yet within this command, so ask again explicitly
        // instead of reporting a failure that the very next call would have passed.
        BlendSpace->ValidateSampleData();
        BlendSpace->ResampleData();
        OutValidity = ReadBlendSpaceValidity(BlendSpace);
    }

    /** Executable next steps derived from the read-back, for a resample that did not take. */
    void CollectFinalizeSuggestions(const UBlendSpace* BlendSpace, const FBlendSpaceValidity& Validity,
                                    TArray<FString>& OutSuggestions)
    {
        OutSuggestions.Reset();

        if (Validity.ValidSampleCount < Validity.SampleCount)
        {
            OutSuggestions.Add(FString::Printf(
                TEXT("%d of %d samples hold no animation, or an animation outside this BlendSpace's own skeleton: "
                     "rewrite them with set_blend_space_samples"),
                Validity.SampleCount - Validity.ValidSampleCount, Validity.SampleCount));
        }

        if (Validity.ValidSampleCount == Validity.SampleCount && Validity.TriangleCount == 0)
        {
            OutSuggestions.Add(TEXT("every sample is valid but no triangle was built: each axis in use needs at least "
                                    "two distinct sample values inside its min/max range"));
        }

        const FString GridMismatch = DescribeGridMismatch(BlendSpace, Validity.SampleCount);
        if (!GridMismatch.IsEmpty())
        {
            OutSuggestions.Add(FString::Printf(
                TEXT("%s: a full grid needs one sample per axis-value combination, so add the missing combinations "
                     "or drop the extras"), *GridMismatch));
        }
    }
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleFinalizeBlendSpace(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("invalid_params"), TEXT("'asset_path' (string) is required"));
    }

    UObject* AssetObject = FUnrealMCPCommonUtils::FindAsset(AssetPath);
    UBlendSpace* BlendSpace = Cast<UBlendSpace>(AssetObject);
    if (!BlendSpace)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("asset_not_blend_space"),
            FString::Printf(TEXT("'%s' resolved to %s, not a BlendSpace"),
                *AssetPath, AssetObject ? *AssetObject->GetClass()->GetName() : TEXT("<nothing>")));
    }

    // Rebuilding the runtime table is done by the asset editor on construction (SAnimationBlendSpace.cpp
    // "Force a resampling of the data on construction"), which SampleData written by script never sees:
    // the table stays empty and the blend output is nothing, i.e. a T-pose. ValidateSampleData +
    // ResampleData are the same two calls that path makes, so try them directly first and keep the
    // editor as the fallback rather than the only way. Shared with set_blend_space_samples.
    FBlendSpaceValidity Validity;
    FString ResampleSource;
    bool bEditorOpened = false;
    FinalizeBlendSpaceInPlace(BlendSpace, Validity, ResampleSource, bEditorOpened);

    const bool bFinalized = IsBlendSpaceFinalized(Validity);
    const bool bPersist = FUnrealMCPCommonUtils::IsPersistRequested(Params);
    bool bSaved = false;
    if (bFinalized && bPersist)
    {
        bSaved = FUnrealMCPCommonUtils::SaveAssetForObject(BlendSpace);
    }

    if (bEditorOpened)
    {
        if (UAssetEditorSubsystem* EditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
        {
            EditorSubsystem->CloseAllEditorsForAsset(BlendSpace);
        }
    }

    TArray<TSharedPtr<FJsonValue>> InvalidJson;
    for (const FString& Invalid : Validity.InvalidSamples)
    {
        InvalidJson.Add(MakeShared<FJsonValueString>(Invalid));
    }

    if (!bFinalized)
    {
        TSharedPtr<FJsonObject> Failure = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("resample_failed"),
            FString::Printf(TEXT("'%s' still has no usable runtime data: %d/%d samples valid, %d triangles"),
                *BlendSpace->GetName(), Validity.ValidSampleCount, Validity.SampleCount, Validity.TriangleCount));
        Failure->SetNumberField(TEXT("sample_count"), Validity.SampleCount);
        Failure->SetNumberField(TEXT("valid_sample_count"), Validity.ValidSampleCount);
        Failure->SetNumberField(TEXT("triangle_count"), Validity.TriangleCount);
        Failure->SetArrayField(TEXT("invalid_samples"), InvalidJson);
        Failure->SetStringField(TEXT("resample_source"), ResampleSource);

        TArray<FString> Suggestions;
        CollectFinalizeSuggestions(BlendSpace, Validity, Suggestions);
        TArray<TSharedPtr<FJsonValue>> SuggestionJson;
        for (const FString& Suggestion : Suggestions)
        {
            SuggestionJson.Add(MakeShared<FJsonValueString>(Suggestion));
        }
        Failure->SetArrayField(TEXT("suggestions"), SuggestionJson);

        Failure->SetStringField(TEXT("hint"),
            TEXT("check that every sample points at an animation of this BlendSpace's own skeleton and that both axes have at least two distinct values"));
        return Failure;
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), BlendSpace->GetPathName());
    Result->SetBoolField(TEXT("samples_valid"), true);
    Result->SetNumberField(TEXT("sample_count"), Validity.SampleCount);
    Result->SetNumberField(TEXT("valid_sample_count"), Validity.ValidSampleCount);
    Result->SetNumberField(TEXT("triangle_count"), Validity.TriangleCount);
    Result->SetStringField(TEXT("resample_source"), ResampleSource);
    Result->SetBoolField(TEXT("editor_opened"), bEditorOpened);
    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetBoolField(TEXT("persist_requested"), bPersist);
    return Result;
}

// set_blend_space_samples: the write and the finalize as ONE command.
//
// The raw path is three steps that each look successful on their own: python's set_editor_property
// silently does not persist (save_asset still answers true), the reflector write does persist, and the
// runtime segment/triangle table only appears after finalize_blend_space. A caller that stops early gets
// an asset the editor reads back correctly and PIE renders as a T-pose (measured 2026-10-02).
TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleSetBlendSpaceSamples(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = GetStringParam(Params, TEXT("asset_path"));
    if (AssetPath.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPAnimError::InvalidParams,
            TEXT("'asset_path' (string) is required"));
    }

    const TArray<TSharedPtr<FJsonValue>>* SamplesJson = nullptr;
    if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("samples"), SamplesJson) || !SamplesJson
        || SamplesJson->Num() == 0)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPAnimError::InvalidParams,
            TEXT("'samples' must be a non-empty array of {animation: <asset path>|null, sample_value: {x, y, z}}"));
    }

    UObject* AssetObject = FUnrealMCPCommonUtils::FindAsset(AssetPath);
    UBlendSpace* BlendSpace = Cast<UBlendSpace>(AssetObject);
    if (!BlendSpace)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPAnimError::AssetNotBlendSpace,
            FString::Printf(TEXT("'%s' resolved to %s, not a BlendSpace"), *AssetPath,
                AssetObject ? *AssetObject->GetClass()->GetName() : TEXT("<nothing>")));
    }

    // Structural write: the sample array is replaced whole, and an open Persona tab holds raw pointers
    // and indices into it.
    if (UAssetEditorSubsystem* EditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr)
    {
        EditorSubsystem->CloseAllEditorsForAsset(BlendSpace);
    }

    // The reflector's container semantics REPLACE the array, so this request is the final sample list:
    // every entry has to be spelled out, and a dropped entry is a removed sample.
    TArray<TSharedPtr<FJsonValue>> SampleEntries;
    SampleEntries.Reserve(SamplesJson->Num());
    for (int32 Index = 0; Index < SamplesJson->Num(); ++Index)
    {
        const TSharedPtr<FJsonObject>* SampleObject = nullptr;
        if (!(*SamplesJson)[Index].IsValid() || !(*SamplesJson)[Index]->TryGetObject(SampleObject)
            || !SampleObject || !SampleObject->IsValid())
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPAnimError::InvalidParams,
                FString::Printf(TEXT("samples[%d] is not an object"), Index));
        }

        // FBlendSample's own field names, with the tool-facing snake_case spelling accepted too.
        const TSharedPtr<FJsonValue>* Animation = (*SampleObject)->Values.Find(TEXT("animation"));
        if (!Animation)
        {
            Animation = (*SampleObject)->Values.Find(TEXT("Animation"));
        }

        const TSharedPtr<FJsonValue>* SampleValue = (*SampleObject)->Values.Find(TEXT("sample_value"));
        if (!SampleValue)
        {
            SampleValue = (*SampleObject)->Values.Find(TEXT("SampleValue"));
        }
        if (!SampleValue || !SampleValue->IsValid())
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPAnimError::InvalidParams,
                FString::Printf(TEXT("samples[%d] needs a 'sample_value' {x, y, z}"), Index));
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        // A missing/anonymous animation is written as an explicit null rather than as an absent field.
        Entry->SetField(TEXT("Animation"),
            (Animation && Animation->IsValid()) ? *Animation : MakeShared<FJsonValueNull>());
        Entry->SetField(TEXT("SampleValue"), *SampleValue);
        SampleEntries.Add(MakeShared<FJsonValueObject>(Entry));
    }

    FArrayProperty* SampleDataProperty = CastField<FArrayProperty>(
        UBlendSpace::StaticClass()->FindPropertyByName(FName(TEXT("SampleData"))));
    if (!SampleDataProperty)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(EUnrealMCPAnimError::WriteFailed,
            TEXT("BlendSpace.SampleData is not reflected on this engine version; nothing was written"));
    }

    BlendSpace->Modify();
    void* SampleDataAddress = SampleDataProperty->ContainerPtrToValuePtr<void>(BlendSpace);
    const FWriteResult WriteResult = FMCPPropertyReflector::FromJson(
        SampleDataProperty, SampleDataAddress, TEXT("SampleData"),
        MakeShared<FJsonValueArray>(SampleEntries));

    if (!WriteResult.bSuccess)
    {
        TSharedPtr<FJsonObject> Failure = FUnrealMCPCommonUtils::CreateErrorResponse(
            WriteResult.ErrorCode.IsEmpty() ? FString(EUnrealMCPAnimError::WriteFailed) : WriteResult.ErrorCode,
            WriteResult.ErrorMessage);
        TArray<TSharedPtr<FJsonValue>> ShapesJson;
        for (const FString& Shape : WriteResult.SupportedShapes)
        {
            ShapesJson.Add(MakeShared<FJsonValueString>(Shape));
        }
        Failure->SetArrayField(TEXT("supported_shapes"), ShapesJson);
        if (WriteResult.FailedIndex != INDEX_NONE)
        {
            Failure->SetNumberField(TEXT("failed_index"), WriteResult.FailedIndex);
        }
        return Failure;
    }

    FPropertyChangedEvent ChangeEvent(SampleDataProperty);
    BlendSpace->PostEditChangeProperty(ChangeEvent);

    bool bFinalize = true;
    Params->TryGetBoolField(TEXT("finalize"), bFinalize);

    FBlendSpaceValidity Validity = ReadBlendSpaceValidity(BlendSpace);
    FString ResampleSource = TEXT("skipped");
    bool bEditorOpened = false;
    if (bFinalize)
    {
        FinalizeBlendSpaceInPlace(BlendSpace, Validity, ResampleSource, bEditorOpened);
        if (bEditorOpened)
        {
            if (UAssetEditorSubsystem* EditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr)
            {
                EditorSubsystem->CloseAllEditorsForAsset(BlendSpace);
            }
        }
    }

    // Read back from the asset, never echoed from the request.
    const TArray<FBlendSample>& Samples = BlendSpace->GetBlendSamples();
    int32 NoneCount = 0;
    TArray<TSharedPtr<FJsonValue>> ReadbackJson;
    for (int32 Index = 0; Index < Samples.Num(); ++Index)
    {
        const FBlendSample& Sample = Samples[Index];
        if (!Sample.Animation)
        {
            ++NoneCount;
        }

        TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetNumberField(TEXT("index"), Index);
        Item->SetStringField(TEXT("animation"),
            Sample.Animation ? Sample.Animation->GetPathName() : FString(TEXT("None")));
        Item->SetBoolField(TEXT("valid"), Sample.bIsValid);
        Item->SetNumberField(TEXT("sample_value_x"), Sample.SampleValue.X);
        Item->SetNumberField(TEXT("sample_value_y"), Sample.SampleValue.Y);
        Item->SetNumberField(TEXT("sample_value_z"), Sample.SampleValue.Z);
        ReadbackJson.Add(MakeShared<FJsonValueObject>(Item));
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), BlendSpace->GetPathName());
    Result->SetBoolField(TEXT("applied"), true);
    Result->SetNumberField(TEXT("sample_count"), Samples.Num());
    Result->SetNumberField(TEXT("none_count"), NoneCount);
    Result->SetNumberField(TEXT("valid_sample_count"), Validity.ValidSampleCount);
    Result->SetNumberField(TEXT("triangle_count"), Validity.TriangleCount);
    Result->SetStringField(TEXT("resample_source"), ResampleSource);
    Result->SetBoolField(TEXT("requires_finalize"), !IsBlendSpaceFinalized(Validity));
    Result->SetArrayField(TEXT("samples"), ReadbackJson);

    // Warnings only: the requested data IS written, these say what will look wrong at runtime.
    TArray<TSharedPtr<FJsonValue>> WarningsJson;

    const FString GridMismatch = DescribeGridMismatch(BlendSpace, Samples.Num());
    if (!GridMismatch.IsEmpty())
    {
        TSharedPtr<FJsonObject> Warning = MakeShared<FJsonObject>();
        Warning->SetStringField(TEXT("code"), TEXT("grid_sample_count_mismatch"));
        Warning->SetStringField(TEXT("detail"), FString::Printf(
            TEXT("%s; the samples were written as requested"), *GridMismatch));
        WarningsJson.Add(MakeShared<FJsonValueObject>(Warning));
    }

    // A sequence of another skeleton compiles but never blends (ValidateSampleData marks it invalid).
    const USkeleton* BlendSpaceSkeleton = BlendSpace->GetSkeleton();
    for (int32 Index = 0; Index < Samples.Num(); ++Index)
    {
        const UAnimSequence* Animation = Samples[Index].Animation;
        if (Animation && BlendSpaceSkeleton && Animation->GetSkeleton() != BlendSpaceSkeleton)
        {
            TSharedPtr<FJsonObject> Warning = MakeShared<FJsonObject>();
            Warning->SetStringField(TEXT("code"), TEXT("sample_skeleton_mismatch"));
            Warning->SetStringField(TEXT("detail"), FString::Printf(
                TEXT("samples[%d] '%s' uses skeleton %s, this BlendSpace uses %s"),
                Index, *Animation->GetName(),
                Animation->GetSkeleton() ? *Animation->GetSkeleton()->GetPathName() : TEXT("<none>"),
                *BlendSpaceSkeleton->GetPathName()));
            WarningsJson.Add(MakeShared<FJsonValueObject>(Warning));
        }
    }

    if (WarningsJson.Num() > 0)
    {
        Result->SetArrayField(TEXT("warnings"), WarningsJson);
    }

    return Result;
}

// ---------------------------------------------------------------------------
// Reads
// ---------------------------------------------------------------------------
TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleAnimSelfCheck(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetBoolField(TEXT("ok"), true);
    Result->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString(EVersionComponent::Patch));
    Result->SetNumberField(TEXT("registered_anim_commands"),
                           FMCPCommandRegistry::Get().ByCategory(TEXT("anim_sequence")).Num()
                               + FMCPCommandRegistry::Get().ByCategory(TEXT("anim_montage")).Num());
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleListAnimSequences(const TSharedPtr<FJsonObject>& Params)
{
    const FString SearchPath = GetStringParam(Params, TEXT("search_path"), TEXT("/Game"));
    const FString Skeleton = GetStringParam(Params, TEXT("skeleton"));
    const int32 MaxResults = GetIntParam(Params, TEXT("max_results"), 0);

    TArray<FUnrealMCPAnimSequenceInfo> Infos;
    int32 FoundCount = 0;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ListSequences(SearchPath, Skeleton, MaxResults, Infos, FoundCount, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("search_path"), SearchPath);
    Result->SetStringField(TEXT("skeleton"), Skeleton);
    SetSequenceListJson(Infos, FoundCount, MaxResults > 0 ? MaxResults : 200, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetAnimSequenceInfo(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    SetSequenceReadback(Sequence, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleFindAnimationsForSkeleton(const TSharedPtr<FJsonObject>& Params)
{
    const FString Skeleton = GetStringParam(Params, TEXT("skeleton"));
    if (Skeleton.IsEmpty())
    {
        return MakeErrorJson({ EUnrealMCPAnimError::InvalidParams, TEXT("skeleton is required"), {}, {} });
    }

    TArray<FUnrealMCPAnimSequenceInfo> Infos;
    int32 FoundCount = 0;
    FUnrealMCPAnimError Error;
    const int32 MaxResults = GetIntParam(Params, TEXT("max_results"), 0);
    const FString SearchPath = GetStringParam(Params, TEXT("search_path"), TEXT("/Game"));
    if (!FUnrealMCPAnimationOps::ListSequences(SearchPath, Skeleton, MaxResults, Infos, FoundCount, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("skeleton"), Skeleton);
    Result->SetStringField(TEXT("search_path"), SearchPath);
    SetSequenceListJson(Infos, FoundCount, MaxResults > 0 ? MaxResults : 200, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleSearchAnimations(const TSharedPtr<FJsonObject>& Params)
{
    TArray<FUnrealMCPAnimSequenceInfo> Infos;
    int32 FoundCount = 0;
    FUnrealMCPAnimError Error;
    const int32 MaxResults = GetIntParam(Params, TEXT("max_results"), 0);
    if (!FUnrealMCPAnimationOps::SearchSequences(GetStringParam(Params, TEXT("search_path"), TEXT("/Game")),
                                                 GetStringParam(Params, TEXT("query")), MaxResults, Infos, FoundCount, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("query"), GetStringParam(Params, TEXT("query")));
    SetSequenceListJson(Infos, FoundCount, MaxResults > 0 ? MaxResults : 200, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetAnimationLength(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetNumberField(TEXT("length"), Sequence->GetPlayLength());
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetAnimationFrameRate(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    const FFrameRate Rate = FUnrealMCPAnimationOps::GetSequenceFrameRate(Sequence);
    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetNumberField(TEXT("frame_rate"), Rate.AsDecimal());
    Result->SetStringField(TEXT("frame_rate_fraction"), FString::Printf(TEXT("%d/%d"), Rate.Numerator, Rate.Denominator));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetAnimationFrameCount(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetNumberField(TEXT("frame_count"), FUnrealMCPAnimationOps::GetSequenceFrameCount(Sequence));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetAnimationSkeleton(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetStringField(TEXT("skeleton"),
                           Sequence->GetSkeleton() ? Sequence->GetSkeleton()->GetPathName() : FString());
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetRateScale(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetNumberField(TEXT("rate_scale"), Sequence->RateScale);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetAnimatedBones(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<FString> BoneNames;
    if (!FUnrealMCPAnimationOps::GetBoneNames(Sequence, BoneNames, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<TSharedPtr<FJsonValue>> BoneValues;
    for (const FString& BoneName : BoneNames)
    {
        BoneValues.Add(MakeShared<FJsonValueString>(BoneName));
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetNumberField(TEXT("bone_count"), BoneNames.Num());
    Result->SetArrayField(TEXT("bones"), BoneValues);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetBoneTransform(const TSharedPtr<FJsonObject>& Params,
                                                                            bool bByFrame)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    bool bResolved = false;
    const float Time = ResolveTimeParam(Params, Sequence, Error, bResolved);
    if (!bResolved)
    {
        return MakeErrorJson(Error);
    }

    int32 BoneIndex = INDEX_NONE;
    if (!FUnrealMCPAnimationOps::ResolveBoneIndex(Sequence, GetStringParam(Params, TEXT("bone_name")), BoneIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    FTransform Transform;
    if (!FUnrealMCPAnimationOps::SampleBoneTransform(Sequence, BoneIndex, Time, Transform, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetStringField(TEXT("bone_name"), GetStringParam(Params, TEXT("bone_name")));
    Result->SetNumberField(TEXT("bone_index"), BoneIndex);
    Result->SetNumberField(TEXT("time"), Time);
    Result->SetNumberField(TEXT("frame"),
                           FMath::RoundToInt(Time * FUnrealMCPAnimationOps::GetSequenceFrameRate(Sequence).AsDecimal()));
    Result->SetBoolField(TEXT("sampled_by_frame"), bByFrame);
    Result->SetObjectField(TEXT("transform"), TransformToJson(Transform));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetPose(const TSharedPtr<FJsonObject>& Params, bool bByFrame)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    bool bResolved = false;
    const float Time = ResolveTimeParam(Params, Sequence, Error, bResolved);
    if (!bResolved)
    {
        return MakeErrorJson(Error);
    }

    TArray<FUnrealMCPAnimBonePose> Pose;
    if (!FUnrealMCPAnimationOps::SamplePose(Sequence, Time, Pose, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<TSharedPtr<FJsonValue>> BoneValues;
    for (const FUnrealMCPAnimBonePose& Bone : Pose)
    {
        TSharedPtr<FJsonObject> BoneJson = MakeShared<FJsonObject>();
        BoneJson->SetStringField(TEXT("bone_name"), Bone.BoneName);
        BoneJson->SetNumberField(TEXT("bone_index"), Bone.BoneIndex);
        BoneJson->SetObjectField(TEXT("transform"), TransformToJson(Bone.Transform));
        BoneValues.Add(MakeShared<FJsonValueObject>(BoneJson));
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetNumberField(TEXT("time"), Time);
    Result->SetBoolField(TEXT("sampled_by_frame"), bByFrame);
    Result->SetNumberField(TEXT("bone_count"), Pose.Num());
    Result->SetArrayField(TEXT("bones"), BoneValues);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetRootMotionAtTime(const TSharedPtr<FJsonObject>& Params)
{
    // Shared with the montage category: the same read answers for a montage's segments.
    UAnimMontage* Montage = nullptr;
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    UAnimSequenceBase* Host = ResolveAnimationHost(Params, Montage, Sequence, Error);
    if (!Host)
    {
        return MakeErrorJson(Error);
    }

    bool bResolved = false;
    const float Time = ResolveTimeParam(Params, Sequence, Error, bResolved);
    if (!bResolved)
    {
        return MakeErrorJson(Error);
    }

    FTransform RootMotion;
    if (Montage)
    {
        if (!FUnrealMCPAnimationMontageOps::SampleRootMotion(Montage, Time, RootMotion, Error))
        {
            return MakeErrorJson(Error);
        }
    }
    else if (!FUnrealMCPAnimationOps::SampleRootMotion(Sequence, Time, false, RootMotion, Error))
    {
        return MakeErrorJson(Error);
    }

    bool bTranslation = false;
    bool bRotation = false;
    FUnrealMCPAnimationMontageOps::GetEnableRootMotion(Montage, bTranslation, bRotation);

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Host->GetPathName());
    Result->SetBoolField(TEXT("is_montage"), Montage != nullptr);
    Result->SetNumberField(TEXT("time"), Time);
    Result->SetBoolField(TEXT("enable_root_motion"), Montage ? (bTranslation || bRotation) : Sequence->bEnableRootMotion);
    Result->SetObjectField(TEXT("root_motion"), TransformToJson(RootMotion));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetTotalRootMotion(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    FTransform RootMotion;
    if (!FUnrealMCPAnimationOps::SampleRootMotion(Sequence, 0.0f, true, RootMotion, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetNumberField(TEXT("length"), Sequence->GetPlayLength());
    Result->SetBoolField(TEXT("enable_root_motion"), Sequence->bEnableRootMotion);
    Result->SetObjectField(TEXT("root_motion"), TransformToJson(RootMotion));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleListCurves(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<FUnrealMCPAnimCurveInfo> Curves;
    int32 TransformCurveCount = 0;
    if (!FUnrealMCPAnimationOps::ListCurves(Sequence, Curves, TransformCurveCount, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<TSharedPtr<FJsonValue>> CurveValues;
    for (const FUnrealMCPAnimCurveInfo& Curve : Curves)
    {
        CurveValues.Add(MakeShared<FJsonValueObject>(CurveInfoToJson(Curve)));
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetNumberField(TEXT("curve_count"), Curves.Num());
    Result->SetNumberField(TEXT("transform_curve_count"), TransformCurveCount);
    Result->SetArrayField(TEXT("curves"), CurveValues);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetCurveInfo(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    const FString CurveName = GetStringParam(Params, TEXT("curve_name"));
    TArray<FUnrealMCPAnimCurveInfo> Curves;
    int32 TransformCurveCount = 0;
    if (!FUnrealMCPAnimationOps::ListCurves(Sequence, Curves, TransformCurveCount, Error))
    {
        return MakeErrorJson(Error);
    }

    for (const FUnrealMCPAnimCurveInfo& Curve : Curves)
    {
        if (Curve.CurveName.Equals(CurveName, ESearchCase::IgnoreCase))
        {
            TSharedPtr<FJsonObject> Result = MakeSuccessJson();
            Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
            const TSharedPtr<FJsonObject> CurveJson = CurveInfoToJson(Curve);
            for (const auto& Pair : CurveJson->Values)
            {
                Result->SetField(Pair.Key, Pair.Value);
            }
            return Result;
        }
    }

    Error.Set(EUnrealMCPAnimError::CurveNotFound,
              FString::Printf(TEXT("curve '%s' does not exist on '%s'"), *CurveName, *Sequence->GetName()));
    for (const FUnrealMCPAnimCurveInfo& Curve : Curves)
    {
        Error.Candidates.Add(Curve.CurveName);
    }
    return MakeErrorJson(Error);
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetCurveValueAtTime(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    float Value = 0.0f;
    if (!FUnrealMCPAnimationOps::GetCurveValue(Sequence, GetStringParam(Params, TEXT("curve_name")),
                                               GetFloatParam(Params, TEXT("time"), 0.0f), Value, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetStringField(TEXT("curve_name"), GetStringParam(Params, TEXT("curve_name")));
    Result->SetNumberField(TEXT("time"), GetFloatParam(Params, TEXT("time"), 0.0f));
    Result->SetNumberField(TEXT("value"), Value);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetCurveKeyframes(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<FUnrealMCPAnimCurveKey> Keys;
    if (!FUnrealMCPAnimationOps::GetCurveKeys(Sequence, GetStringParam(Params, TEXT("curve_name")), Keys, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<TSharedPtr<FJsonValue>> KeyValues;
    for (const FUnrealMCPAnimCurveKey& Key : Keys)
    {
        KeyValues.Add(MakeShared<FJsonValueObject>(CurveKeyToJson(Key)));
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetStringField(TEXT("curve_name"), GetStringParam(Params, TEXT("curve_name")));
    Result->SetNumberField(TEXT("key_count"), Keys.Num());
    Result->SetArrayField(TEXT("keys"), KeyValues);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleListNotifies(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    UAnimSequenceBase* Host = ResolveAnimationHost(Params, Montage, Sequence, Error);
    if (!Host)
    {
        return MakeErrorJson(Error);
    }

    TArray<FUnrealMCPAnimNotifyInfo> Notifies;
    if (Montage)
    {
        FUnrealMCPAnimationMontageOps::ListMontageNotifies(Montage, Notifies);
    }
    else
    {
        FUnrealMCPAnimationOps::ListNotifies(Sequence, Notifies);
    }

    TArray<TSharedPtr<FJsonValue>> NotifyValues;
    for (const FUnrealMCPAnimNotifyInfo& Notify : Notifies)
    {
        NotifyValues.Add(MakeShared<FJsonValueObject>(Montage ? MontageNotifyToJson(Notify) : NotifyToJson(Notify)));
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Host->GetPathName());
    Result->SetBoolField(TEXT("is_montage"), Montage != nullptr);
    Result->SetNumberField(TEXT("notify_count"), Notifies.Num());
    Result->SetNumberField(TEXT("track_count"), Host->AnimNotifyTracks.Num());
    Result->SetArrayField(TEXT("notifies"), NotifyValues);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetNotifyInfo(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    int32 NotifyIndex = INDEX_NONE;
    if (!ResolveNotifyParam(Params, Sequence, NotifyIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<FUnrealMCPAnimNotifyInfo> Notifies;
    FUnrealMCPAnimationOps::ListNotifies(Sequence, Notifies);
    if (!Notifies.IsValidIndex(NotifyIndex))
    {
        Error.Set(EUnrealMCPAnimError::NotifyNotFound, TEXT("the notify disappeared while reading it"));
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetObjectField(TEXT("notify"), NotifyToJson(Notifies[NotifyIndex]));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleListNotifyTracks(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<FUnrealMCPAnimNotifyTrackInfo> Tracks;
    FUnrealMCPAnimationOps::ListNotifyTracks(Sequence, Tracks);

    TArray<TSharedPtr<FJsonValue>> TrackValues;
    for (const FUnrealMCPAnimNotifyTrackInfo& Track : Tracks)
    {
        TrackValues.Add(MakeShared<FJsonValueObject>(NotifyTrackToJson(Track)));
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetNumberField(TEXT("track_count"), Tracks.Num());
    Result->SetArrayField(TEXT("tracks"), TrackValues);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetNotifyTrackCount(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetNumberField(TEXT("track_count"), Sequence->AnimNotifyTracks.Num());
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleListSyncMarkers(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<FUnrealMCPAnimSyncMarkerInfo> Markers;
    FUnrealMCPAnimationOps::ListSyncMarkers(Sequence, Markers);

    TArray<TSharedPtr<FJsonValue>> MarkerValues;
    for (const FUnrealMCPAnimSyncMarkerInfo& Marker : Markers)
    {
        MarkerValues.Add(MakeShared<FJsonValueObject>(SyncMarkerToJson(Marker)));
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetNumberField(TEXT("marker_count"), Markers.Num());
    Result->SetArrayField(TEXT("markers"), MarkerValues);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetSetting(const TSharedPtr<FJsonObject>& Params,
                                                                      const FString& Which)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());

    if (Which == TEXT("additive_anim_type"))
    {
        Result->SetStringField(TEXT("additive_anim_type"),
                               FUnrealMCPAnimationOps::AdditiveTypeToString(static_cast<int32>(Sequence->AdditiveAnimType.GetValue())));
    }
    else if (Which == TEXT("additive_base_pose"))
    {
        Result->SetStringField(TEXT("additive_base_pose"), Sequence->RefPoseSeq ? Sequence->RefPoseSeq->GetPathName() : FString());
    }
    else if (Which == TEXT("enable_root_motion"))
    {
        Result->SetBoolField(TEXT("enable_root_motion"), Sequence->bEnableRootMotion);
    }
    else if (Which == TEXT("root_motion_root_lock"))
    {
        Result->SetStringField(TEXT("root_motion_root_lock"),
                               FUnrealMCPAnimationOps::RootLockToString(static_cast<int32>(Sequence->RootMotionRootLock.GetValue())));
    }
    else if (Which == TEXT("force_root_lock"))
    {
        Result->SetBoolField(TEXT("force_root_lock"), Sequence->bForceRootLock);
    }

    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetCompressionInfo(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    FUnrealMCPAnimCompressionInfo Info;
    FUnrealMCPAnimationOps::FillCompressionInfo(Sequence, Info);

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetStringField(TEXT("compression_scheme"), Info.CompressionScheme);
    Result->SetStringField(TEXT("curve_compression_scheme"), Info.CurveCompressionScheme);
    Result->SetNumberField(TEXT("raw_size"), static_cast<double>(Info.RawSize));
    Result->SetNumberField(TEXT("compressed_size"), Info.CompressedSize);
    Result->SetNumberField(TEXT("ratio"), Info.Ratio);
    Result->SetBoolField(TEXT("compressed_data_valid"), Sequence->IsCompressedDataValid());
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetSourceFiles(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<FString> Files;
    if (!FUnrealMCPAnimationOps::GetSourceFiles(Sequence, Files, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<TSharedPtr<FJsonValue>> FileValues;
    for (const FString& File : Files)
    {
        FileValues.Add(MakeShared<FJsonValueString>(File));
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetNumberField(TEXT("file_count"), Files.Num());
    Result->SetArrayField(TEXT("files"), FileValues);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleExportAnimationToJson(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    const FString Document = FUnrealMCPAnimationOps::ExportToJson(Sequence);

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetNumberField(TEXT("json_length"), Document.Len());
    Result->SetStringField(TEXT("json"), Document);
    return Result;
}

// ---------------------------------------------------------------------------
// Writes
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleCreateAnimSequence(const TSharedPtr<FJsonObject>& Params,
                                                                              bool bFromReferencePose)
{
    FString AssetPath;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::CreateSequence(GetStringParam(Params, TEXT("name")),
                                                GetStringParam(Params, TEXT("folder"), TEXT("/Game")),
                                                GetStringParam(Params, TEXT("skeleton")),
                                                GetFloatParam(Params, TEXT("frame_rate"), 30.0f),
                                                GetFloatParam(Params, TEXT("duration"), 1.0f),
                                                bFromReferencePose, AssetPath, Error))
    {
        return MakeErrorJson(Error);
    }

    UAnimSequence* Sequence = Cast<UAnimSequence>(FUnrealMCPCommonUtils::FindAsset(AssetPath));
    if (!Sequence)
    {
        Error.Set(EUnrealMCPAnimError::CreateFailed,
                  FString::Printf(TEXT("'%s' was created but cannot be loaded back"), *AssetPath));
        return MakeErrorJson(Error);
    }

    // Optional authored tracks in the same call: creating an asset and then filling it is one intent,
    // and doing it here keeps a failure from leaving a half-configured asset behind unnoticed.
    int32 TracksWritten = 0;
    const TArray<TSharedPtr<FJsonValue>>* TrackValues = nullptr;
    if (Params.IsValid() && Params->TryGetArrayField(TEXT("tracks"), TrackValues) && TrackValues)
    {
        for (const TSharedPtr<FJsonValue>& TrackValue : *TrackValues)
        {
            const TSharedPtr<FJsonObject>* TrackObject = nullptr;
            if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackObject) || !TrackObject)
            {
                Error.Set(EUnrealMCPAnimError::InvalidValue, TEXT("every entry of 'tracks' must be an object"));
                return MakeErrorJson(Error);
            }

            const FString BoneName = GetStringParam(*TrackObject, TEXT("bone_name"));
            if (BoneName.IsEmpty())
            {
                Error.Set(EUnrealMCPAnimError::InvalidValue, TEXT("every track needs a 'bone_name'"));
                return MakeErrorJson(Error);
            }

            TArray<FUnrealMCPAnimKeyframe> Keys;
            if (!ReadKeyframesParam(*TrackObject, TEXT("keys"), Keys, Error))
            {
                return MakeErrorJson(Error);
            }

            if (!FUnrealMCPAnimationOps::SetBoneTrackKeys(Sequence, BoneName, Keys, true, Error))
            {
                return MakeErrorJson(Error);
            }
            ++TracksWritten;
        }
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetBoolField(TEXT("created"), true);
    Result->SetBoolField(TEXT("from_reference_pose"), bFromReferencePose);
    Result->SetNumberField(TEXT("tracks_written"), TracksWritten);
    SetSequenceReadback(Sequence, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleSetAnimationFrameRate(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    float Readback = 0.0f;
    if (!FUnrealMCPAnimationOps::SetFrameRate(Sequence, GetFloatParam(Params, TEXT("frame_rate"), 0.0f), Readback, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetNumberField(TEXT("frame_rate"), Readback);
    Result->SetNumberField(TEXT("length"), Sequence->GetPlayLength());
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleSetRateScale(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    float Readback = 0.0f;
    if (!FUnrealMCPAnimationOps::SetRateScale(Sequence, GetFloatParam(Params, TEXT("rate_scale"), 0.0f), Readback, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetNumberField(TEXT("rate_scale"), Readback);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleAddBoneTrack(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    const FString BoneName = GetStringParam(Params, TEXT("bone_name"));
    if (!FUnrealMCPAnimationOps::AddBoneTrack(Sequence, BoneName, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetStringField(TEXT("bone_name"), BoneName);
    SetSequenceReadback(Sequence, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleRemoveBoneTrack(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    const FString BoneName = GetStringParam(Params, TEXT("bone_name"));
    if (!FUnrealMCPAnimationOps::RemoveBoneTrack(Sequence, BoneName, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetStringField(TEXT("bone_name"), BoneName);
    SetSequenceReadback(Sequence, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleSetBoneTrackKeys(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<FUnrealMCPAnimKeyframe> Keys;
    if (!ReadKeyframesParam(Params, TEXT("keys"), Keys, Error))
    {
        return MakeErrorJson(Error);
    }

    const FString BoneName = GetStringParam(Params, TEXT("bone_name"));
    const bool bBakeEveryFrame = GetBoolParam(Params, TEXT("bake_every_frame"), true);
    if (!FUnrealMCPAnimationOps::SetBoneTrackKeys(Sequence, BoneName, Keys, bBakeEveryFrame, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetStringField(TEXT("bone_name"), BoneName);
    Result->SetNumberField(TEXT("keys_written"), Keys.Num());
    Result->SetBoolField(TEXT("bake_every_frame"), bBakeEveryFrame);
    SetSequenceReadback(Sequence, Result);

    // Sample the bone back at the first key's time: a key count alone would not show a mis-ordered write.
    int32 BoneIndex = INDEX_NONE;
    if (FUnrealMCPAnimationOps::ResolveBoneIndex(Sequence, BoneName, BoneIndex, Error))
    {
        FTransform Sampled;
        if (FUnrealMCPAnimationOps::SampleBoneTransform(Sequence, BoneIndex, Keys[0].Time, Sampled, Error))
        {
            TSharedPtr<FJsonObject> Sample = MakeShared<FJsonObject>();
            Sample->SetNumberField(TEXT("time"), Keys[0].Time);
            Sample->SetObjectField(TEXT("transform"), TransformToJson(Sampled));
            Result->SetObjectField(TEXT("sampled_at_first_key"), Sample);
        }
    }

    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleAddCurve(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    const FString CurveName = GetStringParam(Params, TEXT("curve_name"));
    if (!FUnrealMCPAnimationOps::AddCurve(Sequence, CurveName, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetStringField(TEXT("curve_name"), CurveName);
    SetSequenceReadback(Sequence, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleRemoveCurve(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    const FString CurveName = GetStringParam(Params, TEXT("curve_name"));
    if (!FUnrealMCPAnimationOps::RemoveCurve(Sequence, CurveName, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetStringField(TEXT("curve_name"), CurveName);
    SetSequenceReadback(Sequence, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleSetCurveKeys(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<FUnrealMCPAnimCurveKey> Keys;
    if (!ReadCurveKeysParam(Params, Keys, Error))
    {
        return MakeErrorJson(Error);
    }

    const FString CurveName = GetStringParam(Params, TEXT("curve_name"));
    const bool bReplaceAll = GetBoolParam(Params, TEXT("replace_all"), true);
    if (!FUnrealMCPAnimationOps::SetCurveKeys(Sequence, CurveName, Keys, bReplaceAll, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<FUnrealMCPAnimCurveKey> Readback;
    if (!FUnrealMCPAnimationOps::GetCurveKeys(Sequence, CurveName, Readback, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetStringField(TEXT("curve_name"), CurveName);
    Result->SetNumberField(TEXT("key_count"), Readback.Num());
    Result->SetBoolField(TEXT("replaced"), bReplaceAll);

    TArray<TSharedPtr<FJsonValue>> KeyValues;
    for (const FUnrealMCPAnimCurveKey& Key : Readback)
    {
        KeyValues.Add(MakeShared<FJsonValueObject>(CurveKeyToJson(Key)));
    }
    Result->SetArrayField(TEXT("keys"), KeyValues);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleAddCurveKey(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    FUnrealMCPAnimCurveKey Key;
    Key.Time = GetFloatParam(Params, TEXT("time"), 0.0f);
    Key.Value = GetFloatParam(Params, TEXT("value"), 0.0f);
    Key.InterpMode = GetStringParam(Params, TEXT("interp"));
    Key.TangentMode = GetStringParam(Params, TEXT("tangent"));

    TArray<FUnrealMCPAnimCurveKey> Keys;
    Keys.Add(Key);

    const FString CurveName = GetStringParam(Params, TEXT("curve_name"));
    if (!FUnrealMCPAnimationOps::SetCurveKeys(Sequence, CurveName, Keys, /*bReplaceAll*/ false, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<FUnrealMCPAnimCurveKey> Readback;
    if (!FUnrealMCPAnimationOps::GetCurveKeys(Sequence, CurveName, Readback, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetStringField(TEXT("curve_name"), CurveName);
    Result->SetNumberField(TEXT("key_count"), Readback.Num());
    float Value = 0.0f;
    if (FUnrealMCPAnimationOps::GetCurveValue(Sequence, CurveName, Key.Time, Value, Error))
    {
        Result->SetNumberField(TEXT("value_at_key"), Value);
    }
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleSetCurveMetadataFlags(const TSharedPtr<FJsonObject>& Params)
{
    // A float curve only drives a morph target when the *skeleton* (or the sibling mesh) marks that
    // curve name with FAnimCurveType::bMorphtarget: FAnimInstanceProxy::UpdateCurvesToEvaluationContext
    // routes evaluated curves into MorphTargetCurves by intersecting them with
    // RequiredBones->GetCurveFlags(), which is built from that metadata (BoneContainer.cpp:288-385).
    // Curves carry no flags of their own when evaluated from the data model
    // (AnimSequenceHelpers.cpp:56 EvaluateFloatCurvesFromModel -> FCurveUtils::BuildUnsorted, no flags),
    // and the metadata map itself is not writable through the property reflector, hence this command.
    FUnrealMCPAnimError Error;

    UObject* Asset = FUnrealMCPCommonUtils::FindAsset(GetStringParam(Params, TEXT("asset_path")));
    if (!Asset)
    {
        Error.Set(EUnrealMCPAnimError::AssetNotFound, FString::Printf(
            TEXT("asset '%s' not found"), *GetStringParam(Params, TEXT("asset_path"))));
        return MakeErrorJson(Error);
    }

    USkeleton* Skeleton = Cast<USkeleton>(Asset);
    if (!Skeleton)
    {
        if (UAnimSequenceBase* Sequence = Cast<UAnimSequenceBase>(Asset))
        {
            Skeleton = Sequence->GetSkeleton();
        }
        else if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset))
        {
            Skeleton = Mesh->GetSkeleton();
        }
    }
    if (!Skeleton)
    {
        Error.Set(EUnrealMCPAnimError::AssetNotSkeleton, FString::Printf(
            TEXT("'%s' does not resolve to a skeleton (pass a USkeleton, USkeletalMesh or UAnimSequence)"),
            *Asset->GetPathName()));
        return MakeErrorJson(Error);
    }

    const TArray<TSharedPtr<FJsonValue>>* NameValues = nullptr;
    if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("curve_names"), NameValues) || !NameValues || NameValues->Num() == 0)
    {
        Error.Set(EUnrealMCPAnimError::InvalidParams, TEXT("curve_names must be a non-empty array of strings"));
        return MakeErrorJson(Error);
    }

    const bool bMorphtarget = GetBoolParam(Params, TEXT("morphtarget"), true);
    const bool bMaterial = GetBoolParam(Params, TEXT("material"), false);
    if (!bMorphtarget && !bMaterial)
    {
        Error.Set(EUnrealMCPAnimError::InvalidParams, TEXT(
            "at least one of morphtarget/material must be true (AccumulateCurveMetaData can only set flags, never clear them)"));
        return MakeErrorJson(Error);
    }

    TArray<TSharedPtr<FJsonValue>> Curves;
    int32 AddedCount = 0;
    for (const TSharedPtr<FJsonValue>& Value : *NameValues)
    {
        FString CurveName;
        if (!Value.IsValid() || !Value->TryGetString(CurveName) || CurveName.IsEmpty())
        {
            continue;
        }

        const bool bExistedBefore = Skeleton->GetCurveMetaData(FName(*CurveName)) != nullptr;
        Skeleton->AccumulateCurveMetaData(FName(*CurveName), bMaterial, bMorphtarget);

        const FCurveMetaData* MetaData = Skeleton->GetCurveMetaData(FName(*CurveName));
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("curve_name"), CurveName);
        Entry->SetBoolField(TEXT("existed_before"), bExistedBefore);
        Entry->SetBoolField(TEXT("morphtarget"), MetaData ? MetaData->Type.bMorphtarget : false);
        Entry->SetBoolField(TEXT("material"), MetaData ? MetaData->Type.bMaterial : false);
        Entry->SetNumberField(TEXT("max_lod"), MetaData ? MetaData->MaxLOD : 0);
        Curves.Add(MakeShared<FJsonValueObject>(Entry));
        if (!bExistedBefore)
        {
            ++AddedCount;
        }
    }

    if (Curves.Num() == 0)
    {
        Error.Set(EUnrealMCPAnimError::InvalidParams, TEXT("curve_names contained no usable strings"));
        return MakeErrorJson(Error);
    }

    Skeleton->MarkPackageDirty();
    const bool bPersist = FUnrealMCPCommonUtils::IsPersistRequested(Params);
    const bool bSaved = bPersist ? FUnrealMCPCommonUtils::SaveAssetForObject(Skeleton) : false;

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Asset->GetPathName());
    Result->SetStringField(TEXT("skeleton_path"), Skeleton->GetPathName());
    Result->SetNumberField(TEXT("applied_count"), Curves.Num());
    Result->SetNumberField(TEXT("added_count"), AddedCount);
    Result->SetNumberField(TEXT("metadata_count"), Skeleton->GetNumCurveMetaData());
    Result->SetArrayField(TEXT("curves"), Curves);
    Result->SetBoolField(TEXT("persist_requested"), bPersist);
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleAddNotify(const TSharedPtr<FJsonObject>& Params, bool bState)
{
    UAnimMontage* Montage = nullptr;
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    UAnimSequenceBase* Host = ResolveAnimationHost(Params, Montage, Sequence, Error);
    if (!Host)
    {
        return MakeErrorJson(Error);
    }

    int32 NotifyIndex = INDEX_NONE;
    const bool bAdded = Montage
        ? FUnrealMCPAnimationMontageOps::AddMontageNotify(Montage,
                                                         GetStringParam(Params, TEXT("notify_name")),
                                                         GetStringParam(Params, TEXT("notify_class")),
                                                         GetFloatParam(Params, TEXT("time"), 0.0f),
                                                         GetFloatParam(Params, TEXT("duration"), 0.0f),
                                                         GetIntParam(Params, TEXT("track_index"), 0),
                                                         bState, NotifyIndex, Error)
        : FUnrealMCPAnimationOps::AddNotify(Sequence,
                                            GetStringParam(Params, TEXT("notify_name")),
                                            GetStringParam(Params, TEXT("notify_class")),
                                            GetFloatParam(Params, TEXT("time"), 0.0f),
                                            GetFloatParam(Params, TEXT("duration"), 0.0f),
                                            GetIntParam(Params, TEXT("track_index"), 0),
                                            bState, NotifyIndex, Error);
    if (!bAdded)
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Host->GetPathName());
    Result->SetBoolField(TEXT("is_montage"), Montage != nullptr);
    Result->SetNumberField(TEXT("notify_index"), NotifyIndex);
    if (Montage)
    {
        SetMontageNotifyReadback(Montage, Result, NotifyIndex);
    }
    else
    {
        SetNotifyReadback(Sequence, Result, NotifyIndex);
    }
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleRemoveNotify(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    UAnimSequenceBase* Host = ResolveAnimationHost(Params, Montage, Sequence, Error);
    if (!Host)
    {
        return MakeErrorJson(Error);
    }

    int32 NotifyIndex = INDEX_NONE;
    if (Montage)
    {
        if (!FUnrealMCPAnimationMontageOps::ResolveMontageNotify(
                Montage,
                GetIntParam(Params, TEXT("notify_index"), INDEX_NONE),
                GetStringParam(Params, TEXT("notify_name")),
                GetStringParam(Params, TEXT("guid")),
                GetFloatParam(Params, TEXT("time"), 0.0f),
                HasParam(Params, TEXT("time")),
                NotifyIndex, Error))
        {
            return MakeErrorJson(Error);
        }

        if (!FUnrealMCPAnimationMontageOps::RemoveMontageNotify(Montage, NotifyIndex, Error))
        {
            return MakeErrorJson(Error);
        }
    }
    else
    {
        if (!ResolveNotifyParam(Params, Sequence, NotifyIndex, Error))
        {
            return MakeErrorJson(Error);
        }

        if (!FUnrealMCPAnimationOps::RemoveNotify(Sequence, NotifyIndex, Error))
        {
            return MakeErrorJson(Error);
        }
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Host->GetPathName());
    Result->SetBoolField(TEXT("is_montage"), Montage != nullptr);
    Result->SetNumberField(TEXT("removed_notify_index"), NotifyIndex);
    Result->SetNumberField(TEXT("notify_count"), Host->Notifies.Num());
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleSetNotifyProperty(const TSharedPtr<FJsonObject>& Params,
                                                                            const FString& PropertyName)
{
    UAnimMontage* Montage = nullptr;
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    UAnimSequenceBase* Host = ResolveAnimationHost(Params, Montage, Sequence, Error);
    if (!Host)
    {
        return MakeErrorJson(Error);
    }

    int32 NotifyIndex = INDEX_NONE;
    if (Montage)
    {
        // A montage notify only takes a time through these commands; its other fields are the same
        // properties the sequence path writes, but nothing here needs them yet.
        if (!MontageNotifyProperties.Contains(PropertyName))
        {
            Error.Set(EUnrealMCPAnimError::UnsupportedProperty,
                      FString::Printf(TEXT("'%s' is not a montage notify property"), *PropertyName));
            Error.AvailableFields = MontageNotifyProperties;
            return MakeErrorJson(Error);
        }

        if (!FUnrealMCPAnimationMontageOps::ResolveMontageNotify(
                Montage,
                GetIntParam(Params, TEXT("notify_index"), INDEX_NONE),
                GetStringParam(Params, TEXT("notify_name")),
                GetStringParam(Params, TEXT("guid")),
                GetFloatParam(Params, TEXT("time"), 0.0f),
                HasParam(Params, TEXT("time")),
                NotifyIndex, Error))
        {
            return MakeErrorJson(Error);
        }

        double NewTime = 0.0;
        if (!ReadNumberValue(Params, NewTime, Error))
        {
            return MakeErrorJson(Error);
        }

        if (!FUnrealMCPAnimationMontageOps::SetMontageNotifyTime(Montage, NotifyIndex, static_cast<float>(NewTime),
                                                                 true, FString(), Error))
        {
            return MakeErrorJson(Error);
        }
    }
    else
    {
        if (!ResolveNotifyParam(Params, Sequence, NotifyIndex, Error))
        {
            return MakeErrorJson(Error);
        }

        if (!FUnrealMCPAnimationOps::SetNotifyProperty(Sequence, NotifyIndex, PropertyName,
                                                       GetValueParam(Params, TEXT("value")), Error))
        {
            return MakeErrorJson(Error);
        }
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Host->GetPathName());
    Result->SetBoolField(TEXT("is_montage"), Montage != nullptr);
    Result->SetStringField(TEXT("property_name"), PropertyName);
    if (Montage)
    {
        SetMontageNotifyReadback(Montage, Result, NotifyIndex);
    }
    else
    {
        SetNotifyReadback(Sequence, Result, NotifyIndex);
    }
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleAddNotifyTrack(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    // A track colour is optional; the engine default is white.
    FLinearColor Color = FLinearColor::White;
    if (const TSharedPtr<FJsonValue> ColorValue = GetValueParam(Params, TEXT("color")))
    {
        FString ColorError;
        if (!ParseColorValue(ColorValue, Color, ColorError))
        {
            Error.Set(EUnrealMCPAnimError::InvalidValue, ColorError);
            return MakeErrorJson(Error);
        }
    }

    int32 TrackIndex = INDEX_NONE;
    if (!FUnrealMCPAnimationOps::AddNotifyTrack(Sequence, GetStringParam(Params, TEXT("name")), Color, TrackIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<FUnrealMCPAnimNotifyTrackInfo> Tracks;
    FUnrealMCPAnimationOps::ListNotifyTracks(Sequence, Tracks);

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetNumberField(TEXT("track_index"), TrackIndex);
    Result->SetNumberField(TEXT("track_count"), Tracks.Num());
    if (Tracks.IsValidIndex(TrackIndex))
    {
        Result->SetObjectField(TEXT("track"), NotifyTrackToJson(Tracks[TrackIndex]));
    }
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleRenameNotifyTrack(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    int32 TrackIndex = INDEX_NONE;
    if (!ResolveTrackParam(Params, Sequence, TrackIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    const FString NewName = GetStringParam(Params, TEXT("name"));
    if (!FUnrealMCPAnimationOps::RenameNotifyTrack(Sequence, TrackIndex, NewName, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<FUnrealMCPAnimNotifyTrackInfo> Tracks;
    FUnrealMCPAnimationOps::ListNotifyTracks(Sequence, Tracks);

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetNumberField(TEXT("track_index"), TrackIndex);
    if (Tracks.IsValidIndex(TrackIndex))
    {
        Result->SetObjectField(TEXT("track"), NotifyTrackToJson(Tracks[TrackIndex]));
    }
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleRemoveNotifyTrack(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    int32 TrackIndex = INDEX_NONE;
    if (!ResolveTrackParam(Params, Sequence, TrackIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    if (!FUnrealMCPAnimationOps::RemoveNotifyTrack(Sequence, TrackIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetNumberField(TEXT("removed_track_index"), TrackIndex);
    Result->SetNumberField(TEXT("track_count"), Sequence->AnimNotifyTracks.Num());
    Result->SetNumberField(TEXT("notify_count"), Sequence->Notifies.Num());
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleAddSyncMarker(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    int32 MarkerIndex = INDEX_NONE;
    if (!FUnrealMCPAnimationOps::AddSyncMarker(Sequence, GetStringParam(Params, TEXT("marker_name")),
                                               GetFloatParam(Params, TEXT("time"), 0.0f), MarkerIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<FUnrealMCPAnimSyncMarkerInfo> Markers;
    FUnrealMCPAnimationOps::ListSyncMarkers(Sequence, Markers);

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetNumberField(TEXT("marker_index"), MarkerIndex);
    Result->SetNumberField(TEXT("marker_count"), Markers.Num());
    if (Markers.IsValidIndex(MarkerIndex))
    {
        Result->SetObjectField(TEXT("marker"), SyncMarkerToJson(Markers[MarkerIndex]));
    }
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleRemoveSyncMarker(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    int32 MarkerIndex = INDEX_NONE;
    if (!ResolveMarkerParam(Params, Sequence, MarkerIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<FUnrealMCPAnimSyncMarkerInfo> Markers;
    FUnrealMCPAnimationOps::ListSyncMarkers(Sequence, Markers);
    const FString RemovedName = Markers.IsValidIndex(MarkerIndex) ? Markers[MarkerIndex].MarkerName : FString();

    if (!FUnrealMCPAnimationOps::RemoveSyncMarker(Sequence, MarkerIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetStringField(TEXT("removed_marker_name"), RemovedName);
    Result->SetNumberField(TEXT("marker_count"), Sequence->AuthoredSyncMarkers.Num());
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleSetSyncMarkerTime(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    int32 MarkerIndex = INDEX_NONE;
    if (!ResolveMarkerParam(Params, Sequence, MarkerIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    if (!FUnrealMCPAnimationOps::SetSyncMarkerTime(Sequence, MarkerIndex,
                                                   GetFloatParam(Params, TEXT("new_time"), 0.0f), Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<FUnrealMCPAnimSyncMarkerInfo> Markers;
    FUnrealMCPAnimationOps::ListSyncMarkers(Sequence, Markers);

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    // The markers are re-sorted, so the index the caller passed may no longer point at this marker.
    Result->SetNumberField(TEXT("marker_count"), Markers.Num());
    for (int32 Index = 0; Index < Markers.Num(); ++Index)
    {
        if (FMath::IsNearlyEqual(Markers[Index].Time, GetFloatParam(Params, TEXT("new_time"), 0.0f), 0.01f))
        {
            Result->SetNumberField(TEXT("marker_index"), Index);
            Result->SetObjectField(TEXT("marker"), SyncMarkerToJson(Markers[Index]));
            break;
        }
    }
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleSetAdditiveAnimType(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    if (!FUnrealMCPAnimationOps::SetAdditiveAnimType(Sequence, GetStringParam(Params, TEXT("type")), Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetStringField(TEXT("additive_anim_type"),
                           FUnrealMCPAnimationOps::AdditiveTypeToString(static_cast<int32>(Sequence->AdditiveAnimType.GetValue())));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleSetAdditiveBasePose(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    if (!FUnrealMCPAnimationOps::SetAdditiveBasePose(Sequence, GetStringParam(Params, TEXT("base_pose")), Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetStringField(TEXT("additive_base_pose"), Sequence->RefPoseSeq ? Sequence->RefPoseSeq->GetPathName() : FString());
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleSetEnableRootMotion(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    if (!FUnrealMCPAnimationOps::SetEnableRootMotion(Sequence, GetBoolParam(Params, TEXT("enable"), false), Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetBoolField(TEXT("enable_root_motion"), Sequence->bEnableRootMotion);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleSetRootMotionRootLock(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    if (!FUnrealMCPAnimationOps::SetRootMotionRootLock(Sequence, GetStringParam(Params, TEXT("lock_type")), Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetStringField(TEXT("root_motion_root_lock"),
                           FUnrealMCPAnimationOps::RootLockToString(static_cast<int32>(Sequence->RootMotionRootLock.GetValue())));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleSetForceRootLock(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    if (!FUnrealMCPAnimationOps::SetForceRootLock(Sequence, GetBoolParam(Params, TEXT("force"), false), Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetBoolField(TEXT("force_root_lock"), Sequence->bForceRootLock);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleSetCompressionScheme(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    FString Readback;
    if (!FUnrealMCPAnimationOps::SetCompressionScheme(Sequence, GetStringParam(Params, TEXT("compression_scheme")),
                                                      Readback, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetStringField(TEXT("compression_scheme"), Readback);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleCompressAnimation(const TSharedPtr<FJsonObject>& Params)
{
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationOps::ResolveSequence(GetStringParam(Params, TEXT("asset_path")), Sequence, Error))
    {
        return MakeErrorJson(Error);
    }

    const int32 RawSize = static_cast<int32>(Sequence->GetApproxRawSize());

    int32 CompressedSize = 0;
    bool bDataValid = false;
    if (!FUnrealMCPAnimationOps::CompressSequence(Sequence, CompressedSize, bDataValid, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Result->SetBoolField(TEXT("compressed_data_valid"), bDataValid);
    Result->SetNumberField(TEXT("compressed_size"), CompressedSize);
    Result->SetNumberField(TEXT("raw_size"), RawSize);
    return Result;
}

// ---------------------------------------------------------------------------
// Montage reads
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleListMontages(const TSharedPtr<FJsonObject>& Params)
{
    TArray<FUnrealMCPAnimMontageInfo> Infos;
    int32 FoundCount = 0;
    FUnrealMCPAnimError Error;
    const int32 MaxResults = GetIntParam(Params, TEXT("max_results"), 200);
    if (!FUnrealMCPAnimationMontageOps::ListMontages(GetStringParam(Params, TEXT("search_path"), TEXT("/Game")),
                                                     GetStringParam(Params, TEXT("skeleton")),
                                                     MaxResults, Infos, FoundCount, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<TSharedPtr<FJsonValue>> Values;
    for (const FUnrealMCPAnimMontageInfo& Info : Infos)
    {
        Values.Add(MakeShared<FJsonValueObject>(MontageInfoToJson(Info)));
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetArrayField(TEXT("montages"), Values);
    Result->SetNumberField(TEXT("found_count"), FoundCount);
    Result->SetNumberField(TEXT("returned_count"), Infos.Num());
    Result->SetBoolField(TEXT("truncated"), FoundCount > Infos.Num());
    Result->SetNumberField(TEXT("max_results"), MaxResults > 0 ? MaxResults : 200);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetMontageInfo(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    FUnrealMCPAnimMontageInfo Info;
    FUnrealMCPAnimationMontageOps::FillMontageInfo(Montage, Info);
    Result->SetObjectField(TEXT("montage"), MontageInfoToJson(Info));

    TArray<FUnrealMCPAnimSectionInfo> Sections;
    FUnrealMCPAnimationMontageOps::ListSections(Montage, Sections);
    TArray<TSharedPtr<FJsonValue>> SectionValues;
    for (const FUnrealMCPAnimSectionInfo& Section : Sections)
    {
        SectionValues.Add(MakeShared<FJsonValueObject>(SectionToJson(Section)));
    }
    Result->SetArrayField(TEXT("sections"), SectionValues);

    TArray<FUnrealMCPAnimSlotTrackInfo> Tracks;
    FUnrealMCPAnimationMontageOps::ListSlotTracks(Montage, Tracks);
    TArray<TSharedPtr<FJsonValue>> TrackValues;
    for (const FUnrealMCPAnimSlotTrackInfo& Track : Tracks)
    {
        TrackValues.Add(MakeShared<FJsonValueObject>(SlotTrackToJson(Track)));
    }
    Result->SetArrayField(TEXT("slot_tracks"), TrackValues);

    FUnrealMCPAnimBlendInfo Blend;
    FUnrealMCPAnimationMontageOps::GetBlendInfo(Montage, Blend);
    Result->SetObjectField(TEXT("blend"), BlendToJson(Blend));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleFindMontagesForSkeleton(const TSharedPtr<FJsonObject>& Params)
{
    TArray<FUnrealMCPAnimMontageInfo> Infos;
    int32 FoundCount = 0;
    FUnrealMCPAnimError Error;
    const int32 MaxResults = GetIntParam(Params, TEXT("max_results"), 200);
    if (!FUnrealMCPAnimationMontageOps::ListMontages(GetStringParam(Params, TEXT("search_path"), TEXT("/Game")),
                                                     GetStringParam(Params, TEXT("skeleton")),
                                                     MaxResults, Infos, FoundCount, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<TSharedPtr<FJsonValue>> Values;
    for (const FUnrealMCPAnimMontageInfo& Info : Infos)
    {
        Values.Add(MakeShared<FJsonValueObject>(MontageInfoToJson(Info)));
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("skeleton"), GetStringParam(Params, TEXT("skeleton")));
    Result->SetArrayField(TEXT("montages"), Values);
    Result->SetNumberField(TEXT("found_count"), FoundCount);
    Result->SetNumberField(TEXT("returned_count"), Infos.Num());
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleFindMontagesUsingAnimation(const TSharedPtr<FJsonObject>& Params)
{
    TArray<FUnrealMCPAnimMontageInfo> Infos;
    int32 FoundCount = 0;
    FString Strategy;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::FindMontagesUsingAnimation(GetStringParam(Params, TEXT("anim_path")),
                                                                   GetIntParam(Params, TEXT("max_results"), 200),
                                                                   GetIntParam(Params, TEXT("max_scan"), 100),
                                                                   Infos, FoundCount, Strategy, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<TSharedPtr<FJsonValue>> Values;
    for (const FUnrealMCPAnimMontageInfo& Info : Infos)
    {
        Values.Add(MakeShared<FJsonValueObject>(MontageInfoToJson(Info)));
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("anim_path"), GetStringParam(Params, TEXT("anim_path")));
    Result->SetStringField(TEXT("scan_strategy"), Strategy);
    Result->SetArrayField(TEXT("montages"), Values);
    Result->SetNumberField(TEXT("found_count"), FoundCount);
    Result->SetNumberField(TEXT("returned_count"), Infos.Num());
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetMontageLength(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetNumberField(TEXT("length"), Montage->GetPlayLength());
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetMontageSkeleton(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    const USkeleton* Skeleton = Montage->GetSkeleton();
    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetStringField(TEXT("skeleton_path"), Skeleton ? Skeleton->GetPathName() : FString());
    Result->SetStringField(TEXT("skeleton_name"), Skeleton ? Skeleton->GetName() : FString());
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleListSections(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<FUnrealMCPAnimSectionInfo> Sections;
    FUnrealMCPAnimationMontageOps::ListSections(Montage, Sections);

    TArray<TSharedPtr<FJsonValue>> Values;
    for (const FUnrealMCPAnimSectionInfo& Section : Sections)
    {
        Values.Add(MakeShared<FJsonValueObject>(SectionToJson(Section)));
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetNumberField(TEXT("section_count"), Sections.Num());
    Result->SetArrayField(TEXT("sections"), Values);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetSectionInfo(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    int32 SectionIndex = INDEX_NONE;
    if (!ResolveSectionParam(Params, Montage, SectionIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    SetSectionReadback(Montage, Result, SectionIndex);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetSectionAtTime(const TSharedPtr<FJsonObject>& Params,
                                                                            bool bByName)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    const float Time = GetFloatParam(Params, TEXT("time"), 0.0f);
    const int32 SectionIndex = FUnrealMCPAnimationMontageOps::GetSectionIndexAtTime(Montage, Time);
    if (SectionIndex == INDEX_NONE)
    {
        Error.Set(EUnrealMCPAnimError::SectionNotFound,
                  FString::Printf(TEXT("no section of '%s' contains %.4fs"), *Montage->GetName(), Time));
        for (int32 Index = 0; Index < Montage->CompositeSections.Num(); ++Index)
        {
            Error.Candidates.Add(FString::Printf(TEXT("%d: %s @ %.4fs"), Index,
                                                 *Montage->CompositeSections[Index].SectionName.ToString(),
                                                 Montage->CompositeSections[Index].GetTime()));
        }
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetNumberField(TEXT("time"), Time);
    Result->SetNumberField(TEXT("section_index"), SectionIndex);
    Result->SetStringField(TEXT("section_name"),
                           Montage->CompositeSections[SectionIndex].SectionName.ToString());
    if (bByName)
    {
        Result->SetStringField(TEXT("matched_section_name"),
                               Montage->CompositeSections[SectionIndex].SectionName.ToString());
    }
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetSectionLength(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    int32 SectionIndex = INDEX_NONE;
    if (!ResolveSectionParam(Params, Montage, SectionIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetNumberField(TEXT("section_index"), SectionIndex);
    Result->SetNumberField(TEXT("length"), Montage->GetSectionLength(SectionIndex));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetNextSection(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    int32 SectionIndex = INDEX_NONE;
    if (!ResolveSectionParam(Params, Montage, SectionIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    const FCompositeSection& Section = Montage->CompositeSections[SectionIndex];
    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetNumberField(TEXT("section_index"), SectionIndex);
    Result->SetStringField(TEXT("section_name"), Section.SectionName.ToString());
    Result->SetStringField(TEXT("next_section_name"),
                           Section.NextSectionName.IsNone() ? FString() : Section.NextSectionName.ToString());
    Result->SetBoolField(TEXT("is_looping"),
                         !Section.NextSectionName.IsNone() && Section.NextSectionName == Section.SectionName);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetAllSectionLinks(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<TPair<FString, FString>> Links;
    FUnrealMCPAnimationMontageOps::ListSectionLinks(Montage, Links);

    TArray<TSharedPtr<FJsonValue>> Values;
    for (const TPair<FString, FString>& Link : Links)
    {
        TSharedPtr<FJsonObject> LinkJson = MakeShared<FJsonObject>();
        LinkJson->SetStringField(TEXT("section"), Link.Key);
        LinkJson->SetStringField(TEXT("next_section"), Link.Value);
        Values.Add(MakeShared<FJsonValueObject>(LinkJson));
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetNumberField(TEXT("link_count"), Links.Num());
    Result->SetArrayField(TEXT("links"), Values);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleListSlotTracks(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<FUnrealMCPAnimSlotTrackInfo> Tracks;
    FUnrealMCPAnimationMontageOps::ListSlotTracks(Montage, Tracks);

    TArray<TSharedPtr<FJsonValue>> Values;
    for (const FUnrealMCPAnimSlotTrackInfo& Track : Tracks)
    {
        Values.Add(MakeShared<FJsonValueObject>(SlotTrackToJson(Track)));
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetNumberField(TEXT("slot_track_count"), Tracks.Num());
    Result->SetArrayField(TEXT("slot_tracks"), Values);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetSlotTrackInfo(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    int32 TrackIndex = INDEX_NONE;
    if (!ResolveSlotParam(Params, Montage, TrackIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    SetSlotReadback(Montage, Result, TrackIndex);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetAllUsedSlotNames(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<FString> Used;
    TArray<FString> SkeletonSlots;
    FUnrealMCPAnimationMontageOps::ListUsedSlotNames(Montage, Used, SkeletonSlots);

    TArray<TSharedPtr<FJsonValue>> UsedValues;
    for (const FString& Name : Used)
    {
        UsedValues.Add(MakeShared<FJsonValueString>(Name));
    }
    TArray<TSharedPtr<FJsonValue>> SkeletonValues;
    for (const FString& Name : SkeletonSlots)
    {
        SkeletonValues.Add(MakeShared<FJsonValueString>(Name));
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetArrayField(TEXT("used_slot_names"), UsedValues);
    Result->SetArrayField(TEXT("skeleton_slot_names"), SkeletonValues);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleListAnimSegments(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    int32 TrackIndex = INDEX_NONE;
    if (!ResolveSlotParam(Params, Montage, TrackIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<FUnrealMCPAnimSegmentInfo> Segments;
    FUnrealMCPAnimationMontageOps::ListAnimSegments(Montage, TrackIndex, Segments, Error);
    if (Error.IsError())
    {
        return MakeErrorJson(Error);
    }

    TArray<TSharedPtr<FJsonValue>> Values;
    for (const FUnrealMCPAnimSegmentInfo& Segment : Segments)
    {
        Values.Add(MakeShared<FJsonValueObject>(SegmentToJson(Segment)));
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetNumberField(TEXT("track_index"), TrackIndex);
    Result->SetStringField(TEXT("slot_name"), Montage->SlotAnimTracks[TrackIndex].SlotName.ToString());
    Result->SetNumberField(TEXT("segment_count"), Segments.Num());
    Result->SetArrayField(TEXT("segments"), Values);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetAnimSegmentInfo(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    int32 TrackIndex = INDEX_NONE;
    int32 SegmentIndex = INDEX_NONE;
    if (!ResolveSegmentParam(Params, Montage, TrackIndex, SegmentIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    SetSegmentReadback(Montage, Result, TrackIndex, SegmentIndex);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleListBranchingPoints(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<FUnrealMCPAnimBranchingPointInfo> Points;
    FUnrealMCPAnimationMontageOps::ListBranchingPoints(Montage, Points);

    TArray<TSharedPtr<FJsonValue>> Values;
    for (const FUnrealMCPAnimBranchingPointInfo& Point : Points)
    {
        Values.Add(MakeShared<FJsonValueObject>(BranchingPointToJson(Point)));
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetNumberField(TEXT("branching_point_count"), Points.Num());
    Result->SetNumberField(TEXT("notify_count"), Montage->Notifies.Num());
    Result->SetArrayField(TEXT("branching_points"), Values);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleIsBranchingPointAtTime(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    const float Time = GetFloatParam(Params, TEXT("time"), 0.0f);
    FString NotifyName;
    const bool bFound = FUnrealMCPAnimationMontageOps::IsBranchingPointAtTime(Montage, Time, NotifyName);

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetNumberField(TEXT("time"), Time);
    Result->SetBoolField(TEXT("is_branching_point"), bFound);
    Result->SetStringField(TEXT("notify_name"), NotifyName);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetBlendSettings(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    FUnrealMCPAnimBlendInfo Blend;
    FUnrealMCPAnimationMontageOps::GetBlendInfo(Montage, Blend);

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetNumberField(TEXT("blend_in_time"), Blend.BlendInTime);
    Result->SetStringField(TEXT("blend_in_option"), Blend.BlendInOption);
    Result->SetStringField(TEXT("blend_in_mode"), Blend.BlendInMode);
    Result->SetNumberField(TEXT("blend_out_time"), Blend.BlendOutTime);
    Result->SetStringField(TEXT("blend_out_option"), Blend.BlendOutOption);
    Result->SetStringField(TEXT("blend_out_mode"), Blend.BlendOutMode);
    Result->SetNumberField(TEXT("blend_out_trigger_time"), Blend.BlendOutTriggerTime);
    Result->SetBoolField(TEXT("enable_auto_blend_out"), Blend.bEnableAutoBlendOut);
    Result->SetObjectField(TEXT("blend"), BlendToJson(Blend));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleGetMontageRootMotionSetting(const TSharedPtr<FJsonObject>& Params,
                                                                                       bool bTranslation)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    bool bTranslationEnabled = false;
    bool bRotationEnabled = false;
    FUnrealMCPAnimationMontageOps::GetEnableRootMotion(Montage, bTranslationEnabled, bRotationEnabled);

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetBoolField(TEXT("enable_root_motion_translation"), bTranslationEnabled);
    Result->SetBoolField(TEXT("enable_root_motion_rotation"), bRotationEnabled);
    Result->SetBoolField(TEXT("enable"), bTranslation ? bTranslationEnabled : bRotationEnabled);
    return Result;
}

// ---------------------------------------------------------------------------
// Montage writes
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleCreateMontageFromAnimation(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::CreateMontageFromAnimation(GetStringParam(Params, TEXT("anim_path")),
                                                                   GetStringParam(Params, TEXT("name")),
                                                                   GetStringParam(Params, TEXT("folder"), TEXT("/Game")),
                                                                   AssetPath, Error))
    {
        return MakeErrorJson(Error);
    }

    UAnimMontage* Montage = Cast<UAnimMontage>(FUnrealMCPCommonUtils::FindAsset(AssetPath));
    if (!Montage)
    {
        Error.Set(EUnrealMCPAnimError::CreateFailed,
                  FString::Printf(TEXT("'%s' was created but cannot be loaded back"), *AssetPath));
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetBoolField(TEXT("created"), true);
    Result->SetStringField(TEXT("anim_path"), GetStringParam(Params, TEXT("anim_path")));
    SetMontageReadback(Montage, Result);

    TArray<FUnrealMCPAnimSectionInfo> Sections;
    FUnrealMCPAnimationMontageOps::ListSections(Montage, Sections);
    TArray<TSharedPtr<FJsonValue>> SectionValues;
    for (const FUnrealMCPAnimSectionInfo& Section : Sections)
    {
        SectionValues.Add(MakeShared<FJsonValueObject>(SectionToJson(Section)));
    }
    Result->SetArrayField(TEXT("sections"), SectionValues);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleCreateEmptyMontage(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::CreateEmptyMontage(GetStringParam(Params, TEXT("name")),
                                                           GetStringParam(Params, TEXT("folder"), TEXT("/Game")),
                                                           GetStringParam(Params, TEXT("skeleton")),
                                                           AssetPath, Error))
    {
        return MakeErrorJson(Error);
    }

    UAnimMontage* Montage = Cast<UAnimMontage>(FUnrealMCPCommonUtils::FindAsset(AssetPath));
    if (!Montage)
    {
        Error.Set(EUnrealMCPAnimError::CreateFailed,
                  FString::Printf(TEXT("'%s' was created but cannot be loaded back"), *AssetPath));
        return MakeErrorJson(Error);
    }

    TArray<FUnrealMCPAnimSectionInfo> Sections;
    FUnrealMCPAnimationMontageOps::ListSections(Montage, Sections);
    TArray<TSharedPtr<FJsonValue>> SectionValues;
    for (const FUnrealMCPAnimSectionInfo& Section : Sections)
    {
        SectionValues.Add(MakeShared<FJsonValueObject>(SectionToJson(Section)));
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetBoolField(TEXT("created"), true);
    SetMontageReadback(Montage, Result);
    Result->SetArrayField(TEXT("sections"), SectionValues);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleDuplicateMontage(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::DuplicateMontage(GetStringParam(Params, TEXT("asset_path")),
                                                         GetStringParam(Params, TEXT("name")),
                                                         GetStringParam(Params, TEXT("folder"), TEXT("/Game")),
                                                         AssetPath, Error))
    {
        return MakeErrorJson(Error);
    }

    UAnimMontage* Montage = Cast<UAnimMontage>(FUnrealMCPCommonUtils::FindAsset(AssetPath));
    if (!Montage)
    {
        Error.Set(EUnrealMCPAnimError::CreateFailed,
                  FString::Printf(TEXT("'%s' was created but cannot be loaded back"), *AssetPath));
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetStringField(TEXT("source_path"), GetStringParam(Params, TEXT("asset_path")));
    Result->SetBoolField(TEXT("created"), true);
    SetMontageReadback(Montage, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleAddSection(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    const int32 ClosedEditors = CloseMontageEditorsFor(Montage);

    int32 SectionIndex = INDEX_NONE;
    if (!FUnrealMCPAnimationMontageOps::AddSection(Montage, GetStringParam(Params, TEXT("section_name")),
                                                   GetFloatParam(Params, TEXT("start_time"), 0.0f),
                                                   SectionIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetNumberField(TEXT("section_index"), SectionIndex);
    SetSectionReadback(Montage, Result, SectionIndex);
    SetMontageReadback(Montage, Result);
    ReportEditorClosed(Result, ClosedEditors);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleRemoveSection(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    int32 SectionIndex = INDEX_NONE;
    if (!ResolveSectionParam(Params, Montage, SectionIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    const FString RemovedName = Montage->CompositeSections[SectionIndex].SectionName.ToString();
    const int32 ClosedEditors = CloseMontageEditorsFor(Montage);

    if (!FUnrealMCPAnimationMontageOps::RemoveSection(Montage, SectionIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetStringField(TEXT("removed_section_name"), RemovedName);
    Result->SetNumberField(TEXT("section_count"), Montage->CompositeSections.Num());

    TArray<FUnrealMCPAnimSectionInfo> Sections;
    FUnrealMCPAnimationMontageOps::ListSections(Montage, Sections);
    TArray<TSharedPtr<FJsonValue>> SectionValues;
    for (const FUnrealMCPAnimSectionInfo& Section : Sections)
    {
        SectionValues.Add(MakeShared<FJsonValueObject>(SectionToJson(Section)));
    }
    Result->SetArrayField(TEXT("sections"), SectionValues);
    ReportEditorClosed(Result, ClosedEditors);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleRenameSection(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    int32 SectionIndex = INDEX_NONE;
    if (!ResolveSectionParam(Params, Montage, SectionIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    const FString OldName = Montage->CompositeSections[SectionIndex].SectionName.ToString();
    const int32 ClosedEditors = CloseMontageEditorsFor(Montage);

    if (!FUnrealMCPAnimationMontageOps::RenameSection(Montage, SectionIndex, GetStringParam(Params, TEXT("new_name")),
                                                      Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetStringField(TEXT("old_section_name"), OldName);
    SetSectionReadback(Montage, Result, SectionIndex);
    SetMontageReadback(Montage, Result);
    ReportEditorClosed(Result, ClosedEditors);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleSetSectionStartTime(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    int32 SectionIndex = INDEX_NONE;
    if (!ResolveSectionParam(Params, Montage, SectionIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    const FString SectionName = Montage->CompositeSections[SectionIndex].SectionName.ToString();
    if (!FUnrealMCPAnimationMontageOps::SetSectionStartTime(Montage, SectionIndex,
                                                            GetFloatParam(Params, TEXT("time"), 0.0f), Error))
    {
        return MakeErrorJson(Error);
    }

    // Moving a section re-sorts the list, so the section is looked up by name again afterwards.
    const int32 NewIndex = Montage->GetSectionIndex(FName(*SectionName));
    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetNumberField(TEXT("section_index"), NewIndex);
    SetSectionReadback(Montage, Result, NewIndex);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleSetNextSection(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    int32 SectionIndex = INDEX_NONE;
    if (!ResolveSectionParam(Params, Montage, SectionIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    if (!FUnrealMCPAnimationMontageOps::SetNextSection(Montage, SectionIndex,
                                                       GetStringParam(Params, TEXT("next_section")), Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    SetSectionReadback(Montage, Result, SectionIndex);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleSetSectionLoop(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    int32 SectionIndex = INDEX_NONE;
    if (!ResolveSectionParam(Params, Montage, SectionIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    const bool bLoop = GetBoolParam(Params, TEXT("loop"), true);
    const FString SectionName = Montage->CompositeSections[SectionIndex].SectionName.ToString();
    // Looping is expressed as a next-section link onto the section itself.
    const FString NextSection = bLoop ? SectionName : FString();
    if (!FUnrealMCPAnimationMontageOps::SetNextSection(Montage, SectionIndex, NextSection, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetBoolField(TEXT("loop"), bLoop);
    SetSectionReadback(Montage, Result, SectionIndex);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleClearSectionLink(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    int32 SectionIndex = INDEX_NONE;
    if (!ResolveSectionParam(Params, Montage, SectionIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    if (!FUnrealMCPAnimationMontageOps::SetNextSection(Montage, SectionIndex, FString(), Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    SetSectionReadback(Montage, Result, SectionIndex);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleAddSlotTrack(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    const int32 ClosedEditors = CloseMontageEditorsFor(Montage);

    int32 TrackIndex = INDEX_NONE;
    if (!FUnrealMCPAnimationMontageOps::AddSlotTrack(Montage, GetStringParam(Params, TEXT("slot_name")), TrackIndex,
                                                     Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetNumberField(TEXT("track_index"), TrackIndex);
    SetSlotReadback(Montage, Result, TrackIndex);
    ReportEditorClosed(Result, ClosedEditors);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleRemoveSlotTrack(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    int32 TrackIndex = INDEX_NONE;
    if (!ResolveSlotParam(Params, Montage, TrackIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    const FString RemovedSlotName = Montage->SlotAnimTracks[TrackIndex].SlotName.ToString();
    const int32 ClosedEditors = CloseMontageEditorsFor(Montage);

    if (!FUnrealMCPAnimationMontageOps::RemoveSlotTrack(Montage, TrackIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetStringField(TEXT("removed_slot_name"), RemovedSlotName);
    SetMontageReadback(Montage, Result);
    ReportEditorClosed(Result, ClosedEditors);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleSetSlotName(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    int32 TrackIndex = INDEX_NONE;
    if (!ResolveSlotParam(Params, Montage, TrackIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    const FString OldName = Montage->SlotAnimTracks[TrackIndex].SlotName.ToString();
    if (!FUnrealMCPAnimationMontageOps::SetSlotName(Montage, TrackIndex, GetStringParam(Params, TEXT("new_name")),
                                                    Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetStringField(TEXT("old_slot_name"), OldName);
    SetSlotReadback(Montage, Result, TrackIndex);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleAddAnimSegment(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    int32 TrackIndex = INDEX_NONE;
    if (!ResolveSlotParam(Params, Montage, TrackIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    const int32 ClosedEditors = CloseMontageEditorsFor(Montage);

    int32 SegmentIndex = INDEX_NONE;
    if (!FUnrealMCPAnimationMontageOps::AddAnimSegment(Montage, TrackIndex, GetStringParam(Params, TEXT("anim_path")),
                                                       GetFloatParam(Params, TEXT("start_time"), 0.0f),
                                                       GetFloatParam(Params, TEXT("play_rate"), 1.0f),
                                                       GetIntParam(Params, TEXT("loop_count"), 1),
                                                       HasParam(Params, TEXT("start_time")),
                                                       HasParam(Params, TEXT("play_rate")),
                                                       HasParam(Params, TEXT("loop_count")),
                                                       SegmentIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetNumberField(TEXT("track_index"), TrackIndex);
    Result->SetNumberField(TEXT("segment_index"), SegmentIndex);
    SetSegmentReadback(Montage, Result, TrackIndex, SegmentIndex);
    SetMontageReadback(Montage, Result);
    ReportEditorClosed(Result, ClosedEditors);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleRemoveAnimSegment(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    int32 TrackIndex = INDEX_NONE;
    int32 SegmentIndex = INDEX_NONE;
    if (!ResolveSegmentParam(Params, Montage, TrackIndex, SegmentIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    const FAnimSegment& RemovedSegment = Montage->SlotAnimTracks[TrackIndex].AnimTrack.AnimSegments[SegmentIndex];
    const FString RemovedAnim = RemovedSegment.GetAnimReference() ? RemovedSegment.GetAnimReference()->GetPathName()
                                                                 : FString();
    const int32 ClosedEditors = CloseMontageEditorsFor(Montage);

    if (!FUnrealMCPAnimationMontageOps::RemoveAnimSegment(Montage, TrackIndex, SegmentIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetNumberField(TEXT("track_index"), TrackIndex);
    Result->SetStringField(TEXT("removed_anim_path"), RemovedAnim);
    Result->SetNumberField(TEXT("removed_segment_index"), SegmentIndex);

    TArray<FUnrealMCPAnimSegmentInfo> Segments;
    FUnrealMCPAnimError Ignored;
    FUnrealMCPAnimationMontageOps::ListAnimSegments(Montage, TrackIndex, Segments, Ignored);
    Result->SetNumberField(TEXT("segment_count"), Segments.Num());
    SetMontageReadback(Montage, Result);
    ReportEditorClosed(Result, ClosedEditors);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleSetSegmentProperty(const TSharedPtr<FJsonObject>& Params,
                                                                             const FString& PropertyName)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    int32 TrackIndex = INDEX_NONE;
    int32 SegmentIndex = INDEX_NONE;
    if (!ResolveSegmentParam(Params, Montage, TrackIndex, SegmentIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    if (!FUnrealMCPAnimationMontageOps::SetSegmentProperty(Montage, TrackIndex, SegmentIndex, PropertyName,
                                                           GetValueParam(Params, TEXT("value")), Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetStringField(TEXT("property_name"), PropertyName);
    SetSegmentReadback(Montage, Result, TrackIndex, SegmentIndex);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleSetMontageNotifyProperty(const TSharedPtr<FJsonObject>& Params,
                                                                                    const FString& PropertyName)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    int32 NotifyIndex = INDEX_NONE;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontageNotify(
            Montage,
            GetIntParam(Params, TEXT("notify_index"), INDEX_NONE),
            GetStringParam(Params, TEXT("notify_name")),
            GetStringParam(Params, TEXT("guid")),
            GetFloatParam(Params, TEXT("time"), 0.0f),
            HasParam(Params, TEXT("time")),
            NotifyIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    const FString SectionName = GetStringParam(Params, TEXT("section"));
    if (!FUnrealMCPAnimationMontageOps::SetMontageNotifyTime(Montage, NotifyIndex, 0.0f, false, SectionName, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetStringField(TEXT("property_name"), PropertyName);
    Result->SetStringField(TEXT("section"), SectionName);
    SetMontageNotifyReadback(Montage, Result, NotifyIndex);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleAddBranchingPoint(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    const int32 ClosedEditors = CloseMontageEditorsFor(Montage);

    int32 NotifyIndex = INDEX_NONE;
    if (!FUnrealMCPAnimationMontageOps::AddBranchingPoint(Montage, GetStringParam(Params, TEXT("name")),
                                                          GetFloatParam(Params, TEXT("trigger_time"), 0.0f),
                                                          GetIntParam(Params, TEXT("track_index"), 0), NotifyIndex,
                                                          Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetNumberField(TEXT("notify_index"), NotifyIndex);
    SetMontageNotifyReadback(Montage, Result, NotifyIndex);
    ReportEditorClosed(Result, ClosedEditors);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleRemoveBranchingPoint(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    const int32 BranchingPointIndex = GetIntParam(Params, TEXT("branching_point_index"), INDEX_NONE);
    const int32 ClosedEditors = CloseMontageEditorsFor(Montage);

    if (!FUnrealMCPAnimationMontageOps::RemoveBranchingPoint(Montage, BranchingPointIndex, Error))
    {
        return MakeErrorJson(Error);
    }

    TArray<FUnrealMCPAnimBranchingPointInfo> Points;
    FUnrealMCPAnimationMontageOps::ListBranchingPoints(Montage, Points);

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetNumberField(TEXT("removed_branching_point_index"), BranchingPointIndex);
    Result->SetNumberField(TEXT("branching_point_count"), Points.Num());
    Result->SetNumberField(TEXT("notify_count"), Montage->Notifies.Num());
    ReportEditorClosed(Result, ClosedEditors);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleSetBlend(const TSharedPtr<FJsonObject>& Params, bool bBlendIn)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    if (!FUnrealMCPAnimationMontageOps::SetBlendSettings(Montage, bBlendIn, HasParam(Params, TEXT("blend_time")),
                                                         GetFloatParam(Params, TEXT("blend_time"), 0.0f),
                                                         HasParam(Params, TEXT("blend_option")),
                                                         GetStringParam(Params, TEXT("blend_option")), Error))
    {
        return MakeErrorJson(Error);
    }

    FUnrealMCPAnimBlendInfo Blend;
    FUnrealMCPAnimationMontageOps::GetBlendInfo(Montage, Blend);

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetBoolField(TEXT("blend_in"), bBlendIn);
    Result->SetNumberField(TEXT("blend_time"), bBlendIn ? Blend.BlendInTime : Blend.BlendOutTime);
    Result->SetStringField(TEXT("blend_option"), bBlendIn ? Blend.BlendInOption : Blend.BlendOutOption);
    Result->SetObjectField(TEXT("blend"), BlendToJson(Blend));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleSetBlendOutTriggerTime(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    if (!FUnrealMCPAnimationMontageOps::SetBlendOutTriggerTime(Montage, GetFloatParam(Params, TEXT("trigger_time"), 0.0f),
                                                               Error))
    {
        return MakeErrorJson(Error);
    }

    FUnrealMCPAnimBlendInfo Blend;
    FUnrealMCPAnimationMontageOps::GetBlendInfo(Montage, Blend);

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetNumberField(TEXT("blend_out_trigger_time"), Blend.BlendOutTriggerTime);
    Result->SetObjectField(TEXT("blend"), BlendToJson(Blend));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleSetMontageRootMotion(const TSharedPtr<FJsonObject>& Params,
                                                                                bool bTranslation)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    const bool bEnable = GetBoolParam(Params, TEXT("enable"), true);
    if (!FUnrealMCPAnimationMontageOps::SetEnableRootMotion(Montage, bTranslation, bEnable, Error))
    {
        return MakeErrorJson(Error);
    }

    bool bTranslationEnabled = false;
    bool bRotationEnabled = false;
    FUnrealMCPAnimationMontageOps::GetEnableRootMotion(Montage, bTranslationEnabled, bRotationEnabled);

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetStringField(TEXT("property_name"),
                           bTranslation ? TEXT("enable_root_motion_translation") : TEXT("enable_root_motion_rotation"));
    Result->SetBoolField(TEXT("enable_root_motion_translation"), bTranslationEnabled);
    Result->SetBoolField(TEXT("enable_root_motion_rotation"), bRotationEnabled);
    Result->SetBoolField(TEXT("enable"), bTranslation ? bTranslationEnabled : bRotationEnabled);
    return Result;
}

// ---------------------------------------------------------------------------
// Editor session and preview
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleOpenAnimationEditor(const TSharedPtr<FJsonObject>& Params,
                                                                               bool bMontageOnly)
{
    UAnimMontage* Montage = nullptr;
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    UAnimSequenceBase* Host = ResolveAnimationHost(Params, Montage, Sequence, Error);
    if (!Host)
    {
        return MakeErrorJson(Error);
    }

    if (bMontageOnly && !Montage)
    {
        Error.Set(EUnrealMCPAnimError::AssetNotAnimMontage,
                  FString::Printf(TEXT("'%s' is a %s, not an animation montage"), *Host->GetPathName(),
                                  *Host->GetClass()->GetName()));
        return MakeErrorJson(Error);
    }

    FUnrealMCPAnimPreviewState State;
    if (!FUnrealMCPAnimationEditorOps::OpenEditor(Host, State, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Host->GetPathName());
    Result->SetBoolField(TEXT("opened"), State.bEditorOpen);
    ApplyPreviewState(State, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleRefreshMontageEditor(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    FUnrealMCPAnimPreviewState State;
    if (!FUnrealMCPAnimationEditorOps::RefreshEditor(Montage, State, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetBoolField(TEXT("refreshed"), State.bEditorOpen);
    ApplyPreviewState(State, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleSetPreviewTime(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    UAnimSequenceBase* Host = ResolveAnimationHost(Params, Montage, Sequence, Error);
    if (!Host)
    {
        return MakeErrorJson(Error);
    }

    FUnrealMCPAnimPreviewState State;
    if (!FUnrealMCPAnimationEditorOps::SetPreviewTime(Host, GetFloatParam(Params, TEXT("time"), 0.0f), State, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Host->GetPathName());
    ApplyPreviewState(State, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandlePlayPreview(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    UAnimSequenceBase* Host = ResolveAnimationHost(Params, Montage, Sequence, Error);
    if (!Host)
    {
        return MakeErrorJson(Error);
    }

    FUnrealMCPAnimPreviewState State;
    if (!FUnrealMCPAnimationEditorOps::SetPreviewPlaying(Host, /*bPlay=*/true,
                                                         HasParam(Params, TEXT("loop")),
                                                         GetBoolParam(Params, TEXT("loop"), false),
                                                         HasParam(Params, TEXT("play_rate")),
                                                         GetFloatParam(Params, TEXT("play_rate"), 1.0f),
                                                         State, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Host->GetPathName());
    Result->SetBoolField(TEXT("played"), State.bIsPlaying);
    ApplyPreviewState(State, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleStopPreview(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    UAnimSequence* Sequence = nullptr;
    FUnrealMCPAnimError Error;
    UAnimSequenceBase* Host = ResolveAnimationHost(Params, Montage, Sequence, Error);
    if (!Host)
    {
        return MakeErrorJson(Error);
    }

    FUnrealMCPAnimPreviewState State;
    if (!FUnrealMCPAnimationEditorOps::SetPreviewPlaying(Host, /*bPlay=*/false, false, false, false, 1.0f, State, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Host->GetPathName());
    Result->SetBoolField(TEXT("stopped"), !State.bIsPlaying);
    ApplyPreviewState(State, Result);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimationCommands::HandleJumpToSection(const TSharedPtr<FJsonObject>& Params)
{
    UAnimMontage* Montage = nullptr;
    FUnrealMCPAnimError Error;
    if (!FUnrealMCPAnimationMontageOps::ResolveMontage(GetStringParam(Params, TEXT("asset_path")), Montage, Error))
    {
        return MakeErrorJson(Error);
    }

    FUnrealMCPAnimPreviewState State;
    if (!FUnrealMCPAnimationEditorOps::JumpToSection(Montage, GetStringParam(Params, TEXT("section")), State, Error))
    {
        return MakeErrorJson(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeSuccessJson();
    Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
    Result->SetStringField(TEXT("section"), GetStringParam(Params, TEXT("section")));
    ApplyPreviewState(State, Result);
    return Result;
}
