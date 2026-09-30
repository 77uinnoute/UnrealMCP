#include "Commands/Particle/UnrealMCPParticleOps.h"
#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Commands/Blueprint/UnrealMCPBlueprintGraphOps.h"
#include "Reflection/MCPPropertyCodecs.h"

#include "Particles/ParticleSystem.h"
#include "Particles/ParticleEmitter.h"
#include "Particles/ParticleSpriteEmitter.h"
#include "Particles/ParticleLODLevel.h"
#include "Particles/ParticleModule.h"
#include "Particles/ParticleModuleRequired.h"
#include "Particles/Event/ParticleModuleEventGenerator.h"
#include "Particles/Spawn/ParticleModuleSpawn.h"
#include "Particles/TypeData/ParticleModuleTypeDataBase.h"
#include "Particles/TypeData/ParticleModuleTypeDataMesh.h"
#include "Particles/Light/ParticleModuleLight.h"
#include "Particles/Velocity/ParticleModuleVelocity.h"
#include "Particles/Velocity/ParticleModuleVelocityOverLifetime.h"
#include "Particles/Size/ParticleModuleSizeScaleBySpeed.h"

#include "Distributions/DistributionFloat.h"
#include "Distributions/DistributionVector.h"
#include "Distributions/DistributionFloatConstant.h"
#include "Distributions/DistributionFloatUniform.h"
#include "Distributions/DistributionFloatConstantCurve.h"
#include "Distributions/DistributionFloatUniformCurve.h"
#include "Distributions/DistributionVectorConstant.h"
#include "Distributions/DistributionVectorUniform.h"
#include "Distributions/DistributionVectorConstantCurve.h"
#include "Distributions/DistributionVectorUniformCurve.h"
#include "Math/InterpCurve.h"
#include "Math/TwoVectors.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "MaterialShared.h"
#include "Factories/ParticleSystemFactoryNew.h"
#include "AssetToolsModule.h"
#include "EditorAssetLibrary.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealMCPParticle, Log, All);

namespace
{
    // -----------------------------------------------------------------------
    // Small helpers
    // -----------------------------------------------------------------------

    FString ClassNameOf(const UObject* Object)
    {
        return Object ? Object->GetClass()->GetName() : FString();
    }

    /** Text of a class name, or an empty string for a null object. */
    FString SlotClassName(const UObject* Object)
    {
        return Object ? Object->GetClass()->GetName() : FString();
    }

    /** Numbers of the sample value of a raw distribution, for readback. */
    void AppendValue(float Value, TArray<float>& Out)
    {
        Out.Add(Value);
    }

    void AppendValue(const FVector& Value, TArray<float>& Out)
    {
        Out.Add(Value.X);
        Out.Add(Value.Y);
        Out.Add(Value.Z);
    }

    void AppendValue(const FVector2D& Value, TArray<float>& Out)
    {
        Out.Add(Value.X);
        Out.Add(Value.Y);
    }

    void AppendValue(const FTwoVectors& Value, TArray<float>& Out)
    {
        AppendValue(Value.v1, Out);
        AppendValue(Value.v2, Out);
    }

    // Curve values arrive as a flat component list; the element type decides how many
    // entries are required. Declared before the templates that call them so ordinary
    // lookup finds the overloads at template definition time.
    bool MakeCurveValue(float& Out, const TArray<float>& Values, int32 KeyIndex, const FString& PropertyName, FString& OutError);
    bool MakeCurveValue(FVector& Out, const TArray<float>& Values, int32 KeyIndex, const FString& PropertyName, FString& OutError);
    bool MakeCurveValue(FVector2D& Out, const TArray<float>& Values, int32 KeyIndex, const FString& PropertyName, FString& OutError);
    bool MakeCurveValue(FTwoVectors& Out, const TArray<float>& Values, int32 KeyIndex, const FString& PropertyName, FString& OutError);

    FString InterpModeToString(EInterpCurveMode Mode)
    {
        switch (Mode)
        {
        case CIM_Linear:           return TEXT("linear");
        case CIM_CurveAuto:        return TEXT("curve_auto");
        case CIM_Constant:         return TEXT("constant");
        case CIM_CurveUser:        return TEXT("curve_user");
        case CIM_CurveBreak:       return TEXT("curve_break");
        case CIM_CurveAutoClamped: return TEXT("curve_auto_clamped");
        default:                   return TEXT("curve_auto");
        }
    }

    EInterpCurveMode InterpModeFromString(const FString& Text, EInterpCurveMode Default = CIM_Linear)
    {
        const FString Value = Text.ToLower();
        if (Value == TEXT("linear"))             return CIM_Linear;
        if (Value == TEXT("curve_auto"))         return CIM_CurveAuto;
        if (Value == TEXT("constant"))           return CIM_Constant;
        if (Value == TEXT("curve_user"))         return CIM_CurveUser;
        if (Value == TEXT("curve_break"))        return CIM_CurveBreak;
        if (Value == TEXT("curve_auto_clamped")) return CIM_CurveAutoClamped;
        return Default;
    }

    template <typename T>
    void ReadCurveKeys(const FInterpCurve<T>& Curve, TArray<FUnrealMCPParticleDistributionKey>& Out)
    {
        Out.Reset();
        for (const FInterpCurvePoint<T>& Point : Curve.Points)
        {
            FUnrealMCPParticleDistributionKey Key;
            Key.Time = Point.InVal;
            Key.Interp = InterpModeToString(Point.InterpMode);
            AppendValue(Point.OutVal, Key.Value);
            Out.Add(MoveTemp(Key));
        }
    }

    template <typename T>
    bool WriteCurveKeys(FInterpCurve<T>& Curve, const TArray<FUnrealMCPParticleDistributionKey>& Keys,
                        const FString& PropertyName, FString& OutError)
    {
        TArray<FInterpCurvePoint<T>> NewPoints;
        NewPoints.Reserve(Keys.Num());
        for (int32 Index = 0; Index < Keys.Num(); ++Index)
        {
            const FUnrealMCPParticleDistributionKey& Key = Keys[Index];
            FInterpCurvePoint<T> Point;
            Point.InVal = Key.Time;
            Point.InterpMode = InterpModeFromString(Key.Interp);
            if (!MakeCurveValue(Point.OutVal, Key.Value, Index, PropertyName, OutError))
            {
                return false;
            }
            NewPoints.Add(Point);
        }

        Curve.Points = MoveTemp(NewPoints);
        return true;
    }

    // -----------------------------------------------------------------------
    // Distribution values
    // -----------------------------------------------------------------------

    bool IsRawDistributionProperty(FProperty* Property, bool& bOutVector)
    {
        bOutVector = false;
        FStructProperty* StructProperty = CastField<FStructProperty>(Property);
        if (!StructProperty || !StructProperty->Struct)
        {
            return false;
        }

        if (StructProperty->Struct == FRawDistributionFloat::StaticStruct())
        {
            return true;
        }
        if (StructProperty->Struct == FRawDistributionVector::StaticStruct())
        {
            bOutVector = true;
            return true;
        }
        return false;
    }

    /** The authored distribution object behind a FRawDistribution* property. */
    UDistribution* GetRawDistributionObject(UParticleModule* Module, FProperty* Property, bool bVector)
    {
        void* Address = Property->ContainerPtrToValuePtr<void>(Module);
        if (!Address)
        {
            return nullptr;
        }
        if (bVector)
        {
            return static_cast<FRawDistributionVector*>(Address)->Distribution;
        }
        return static_cast<FRawDistributionFloat*>(Address)->Distribution;
    }

    void SetRawDistributionObject(UParticleModule* Module, FProperty* Property, bool bVector, UDistribution* Distribution)
    {
        void* Address = Property->ContainerPtrToValuePtr<void>(Module);
        if (!Address)
        {
            return;
        }
        if (bVector)
        {
            static_cast<FRawDistributionVector*>(Address)->Distribution = Cast<UDistributionVector>(Distribution);
        }
        else
        {
            static_cast<FRawDistributionFloat*>(Address)->Distribution = Cast<UDistributionFloat>(Distribution);
        }
    }

    /** Rebuild the baked lookup table so the values are really the ones just written. */
    void RebuildRawDistribution(UParticleModule* Module, FProperty* Property, bool bVector)
    {
        void* Address = Property->ContainerPtrToValuePtr<void>(Module);
        if (!Address)
        {
            return;
        }

        UDistribution* Distribution = GetRawDistributionObject(Module, Property, bVector);
        if (Distribution)
        {
            if (UDistributionFloat* FloatDistribution = Cast<UDistributionFloat>(Distribution))
            {
                FloatDistribution->bIsDirty = true;
            }
            else if (UDistributionVector* VectorDistribution = Cast<UDistributionVector>(Distribution))
            {
                VectorDistribution->bIsDirty = true;
            }
        }

        if (bVector)
        {
            static_cast<FRawDistributionVector*>(Address)->Initialize();
        }
        else
        {
            static_cast<FRawDistributionFloat*>(Address)->Initialize();
        }
    }

    /**
     * Property codec for FRawDistribution* properties. A raw distribution is authored through
     * set_particle_distribution, not through the generic property writer, so registering the codec is
     * what lets EVERY write entry point refuse it with the same code and hint instead of each caller
     * keeping its own if-branch.
     */
    struct FParticlePropertyCodecRegistrar
    {
        FParticlePropertyCodecRegistrar()
        {
            TArray<FString> Shapes;
            Shapes.Add(TEXT("distribution kind + keys, see set_particle_distribution"));

            const TCHAR* const Hint = TEXT("改用 set_particle_distribution 设置分布曲线");

            FPropertyCodec Codec;
            Codec.Matches = [](const FProperty* Property)
            {
                bool bVector = false;
                return IsRawDistributionProperty(const_cast<FProperty*>(Property), bVector);
            };
            Codec.Shapes = Shapes;
            Codec.Hint = Hint;
            Codec.ToJson = [](const FProperty* Property, const void* ValuePtr) -> TSharedPtr<FJsonValue>
            {
                bool bVector = false;
                IsRawDistributionProperty(const_cast<FProperty*>(Property), bVector);

                // The generic struct expansion used to dump engine state (transient bake data). This is
                // the same summary the particle read paths report: which distribution is attached plus
                // the value sampled at time 0.
                TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
                Object->SetStringField(TEXT("kind"), bVector ? TEXT("vector") : TEXT("float"));

                void* MutableValuePtr = const_cast<void*>(ValuePtr);
                if (bVector)
                {
                    FRawDistributionVector* Raw = static_cast<FRawDistributionVector*>(MutableValuePtr);
                    Object->SetStringField(TEXT("distribution"),
                        (Raw && Raw->Distribution) ? Raw->Distribution->GetPathName() : TEXT("None"));
                    if (Raw && Raw->Distribution)
                    {
                        const FVector Sampled = Raw->GetValue(0.0f);
                        TArray<TSharedPtr<FJsonValue>> Arr;
                        Arr.Add(MakeShared<FJsonValueNumber>(Sampled.X));
                        Arr.Add(MakeShared<FJsonValueNumber>(Sampled.Y));
                        Arr.Add(MakeShared<FJsonValueNumber>(Sampled.Z));
                        Object->SetField(TEXT("sampled_value"), MakeShared<FJsonValueArray>(Arr));
                    }
                }
                else
                {
                    FRawDistributionFloat* Raw = static_cast<FRawDistributionFloat*>(MutableValuePtr);
                    Object->SetStringField(TEXT("distribution"),
                        (Raw && Raw->Distribution) ? Raw->Distribution->GetPathName() : TEXT("None"));
                    if (Raw && Raw->Distribution)
                    {
                        Object->SetNumberField(TEXT("sampled_value"), Raw->GetValue(0.0f));
                    }
                }
                return MakeShared<FJsonValueObject>(Object);
            };
            Codec.FromJson = [Shapes, Hint](const FProperty* Property, void*, const TSharedPtr<FJsonValue>&,
                                            FWriteResult& Out) -> bool
            {
                Out = FWriteResult::Failure(EUnrealMCPParticleError::UnsupportedModuleProperty,
                    FString::Printf(TEXT("Property '%s' is a raw distribution; use set_particle_distribution"),
                        *Property->GetName()))
                    .WithShapes(Shapes)
                    .WithHint(Hint);
                return false;
            };
            Codec.bWritable = false;
            FMCPPropertyCodecs::RegisterPropertyCodec(MoveTemp(Codec));
        }
    };

    static FParticlePropertyCodecRegistrar GParticlePropertyCodecRegistrar;

    FString KindOfDistribution(const UDistribution* Distribution)
    {
        if (Cast<UDistributionFloatConstant>(Distribution) || Cast<UDistributionVectorConstant>(Distribution))
        {
            return EUnrealMCPParticleDistributionKind::Constant;
        }
        if (Cast<UDistributionFloatUniform>(Distribution) || Cast<UDistributionVectorUniform>(Distribution))
        {
            return EUnrealMCPParticleDistributionKind::Uniform;
        }
        if (Cast<UDistributionFloatConstantCurve>(Distribution) || Cast<UDistributionVectorConstantCurve>(Distribution))
        {
            return EUnrealMCPParticleDistributionKind::ConstantCurve;
        }
        if (Cast<UDistributionFloatUniformCurve>(Distribution) || Cast<UDistributionVectorUniformCurve>(Distribution))
        {
            return EUnrealMCPParticleDistributionKind::UniformCurve;
        }
        return TEXT("unsupported");
    }

    UClass* DistributionClassFor(bool bVector, const FString& Kind)
    {
        if (Kind == EUnrealMCPParticleDistributionKind::Constant)
        {
            return bVector ? UDistributionVectorConstant::StaticClass() : UDistributionFloatConstant::StaticClass();
        }
        if (Kind == EUnrealMCPParticleDistributionKind::Uniform)
        {
            return bVector ? UDistributionVectorUniform::StaticClass() : UDistributionFloatUniform::StaticClass();
        }
        if (Kind == EUnrealMCPParticleDistributionKind::ConstantCurve)
        {
            return bVector ? UDistributionVectorConstantCurve::StaticClass() : UDistributionFloatConstantCurve::StaticClass();
        }
        if (Kind == EUnrealMCPParticleDistributionKind::UniformCurve)
        {
            return bVector ? UDistributionVectorUniformCurve::StaticClass() : UDistributionFloatUniformCurve::StaticClass();
        }
        return nullptr;
    }

    FString DistributionToText(const FUnrealMCPParticleDistributionInfo& Info)
    {
        auto NumbersToText = [](const TArray<float>& Numbers)
        {
            TArray<FString> Parts;
            for (const float Number : Numbers)
            {
                Parts.Add(FString::Printf(TEXT("%g"), Number));
            }
            return FString::Join(Parts, TEXT(","));
        };

        if (Info.Kind == EUnrealMCPParticleDistributionKind::Constant)
        {
            return FString::Printf(TEXT("constant(%s)"), *NumbersToText(Info.Constants));
        }
        if (Info.Kind == EUnrealMCPParticleDistributionKind::Uniform)
        {
            return FString::Printf(TEXT("uniform(%s)"), *NumbersToText(Info.MinMax));
        }
        if (Info.Kind == EUnrealMCPParticleDistributionKind::ConstantCurve ||
            Info.Kind == EUnrealMCPParticleDistributionKind::UniformCurve)
        {
            TArray<FString> Keys;
            for (const FUnrealMCPParticleDistributionKey& Key : Info.Keys)
            {
                Keys.Add(FString::Printf(TEXT("%g:%s"), Key.Time, *NumbersToText(Key.Value)));
            }
            return FString::Printf(TEXT("%s[%s]"), *Info.Kind, *FString::Join(Keys, TEXT(";")));
        }
        return FString::Printf(TEXT("%s(%s)"), *Info.Kind, *Info.DistributionClass);
    }

    /**
     * Value text of a written property. Scalars (and the vector / colour structs, which read as
     * plain arrays) keep the short text form callers already compare against, while structs and
     * struct arrays are serialized as JSON so the readback can be fed straight back into a write.
     */
    FString PropertyValueToResponseText(FProperty* Property, const void* ValuePtr)
    {
        const TSharedPtr<FJsonValue> JsonValue = FUnrealMCPCommonUtils::PropertyValueToJson(Property, ValuePtr);

        bool bSerializeAsJson = false;
        if (JsonValue.IsValid())
        {
            if (JsonValue->Type == EJson::Object)
            {
                bSerializeAsJson = true;
            }
            else if (JsonValue->Type == EJson::Array)
            {
                const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
                bSerializeAsJson = JsonValue->TryGetArray(Items) && Items && Items->Num() > 0 &&
                    (*Items)[0].IsValid() && (*Items)[0]->Type == EJson::Object;
            }
        }

        if (bSerializeAsJson)
        {
            FString Serialized;
            const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
            const bool bWritten = JsonValue->Type == EJson::Object
                ? FJsonSerializer::Serialize(JsonValue->AsObject().ToSharedRef(), Writer)
                : FJsonSerializer::Serialize(JsonValue->AsArray(), Writer);
            if (bWritten)
            {
                return Serialized;
            }
        }

        return FUnrealMCPBlueprintGraphOps::JsonValueToText(JsonValue);
    }

    // -----------------------------------------------------------------------
    // Class resolution
    // -----------------------------------------------------------------------

    FString NormalizeName(const FString& InName)
    {
        FString Out;
        Out.Reserve(InName.Len());
        for (const TCHAR Char : InName)
        {
            if (Char != TEXT('_'))
            {
                Out.AppendChar(FChar::ToLower(Char));
            }
        }
        return Out;
    }

    /**
     * Forms of a class name a caller may reasonably write: the full name, without the
     * "Particle"/"ParticleModule" prefix, without the "Emitter"/"Module" suffix, and without
     * both — so "Velocity" matches ParticleModuleVelocity and "Sprite" matches
     * ParticleSpriteEmitter.
     */
    void CollectNameForms(const FString& ClassName, TArray<FString>& OutForms)
    {
        OutForms.Reset();

        auto StripPrefix = [](const FString& In)
        {
            if (In.StartsWith(TEXT("ParticleModule")))
            {
                return In.RightChop(14);
            }
            if (In.StartsWith(TEXT("Particle")))
            {
                return In.RightChop(8);
            }
            return In;
        };
        auto StripSuffix = [](const FString& In)
        {
            if (In.EndsWith(TEXT("Emitter")))
            {
                return In.LeftChop(7);
            }
            if (In.EndsWith(TEXT("Module")))
            {
                return In.LeftChop(6);
            }
            return In;
        };

        OutForms.AddUnique(ClassName);
        OutForms.AddUnique(StripPrefix(ClassName));
        OutForms.AddUnique(StripSuffix(ClassName));
        OutForms.AddUnique(StripSuffix(StripPrefix(ClassName)));
        OutForms.Remove(TEXT(""));
    }

    /**
     * Resolve a class by path, by short name, by "ParticleModule"-less short name, or by
     * case/underscore insensitive match. Candidates list every class of the base type.
     */
    UClass* FindClassByShortName(const FString& InClassName, UClass* BaseClass, bool bAllowSeeded,
                                 const TCHAR* NotFoundCode, FString& OutErrorCode, FString& OutErrorMessage,
                                 TArray<FString>& OutCandidates)
    {
        OutCandidates.Reset();
        OutErrorCode.Reset();
        OutErrorMessage.Reset();

        FString ClassName = InClassName;
        if (ClassName.EndsWith(TEXT("_C")))
        {
            ClassName.LeftChopInline(2);
        }
        ClassName.TrimStartAndEndInline();
        if (ClassName.IsEmpty())
        {
            OutErrorCode = EUnrealMCPParticleError::InvalidParams;
            OutErrorMessage = TEXT("Module/emitter class name must not be empty");
            return nullptr;
        }

        if (ClassName.StartsWith(TEXT("/")))
        {
            if (UClass* Loaded = LoadObject<UClass>(nullptr, *ClassName))
            {
                if (Loaded->IsChildOf(BaseClass))
                {
                    return Loaded;
                }
                OutErrorCode = NotFoundCode;
                OutErrorMessage = FString::Printf(TEXT("Class '%s' is not a %s"), *ClassName, *BaseClass->GetName());
                return nullptr;
            }
            OutErrorCode = NotFoundCode;
            OutErrorMessage = FString::Printf(TEXT("Class not found: %s"), *ClassName);
            OutCandidates.Add(BaseClass->GetName());
            return nullptr;
        }

        // Exact name first, then with the conventional prefix (ParticleModule / Particle).
        TArray<FString> SearchNames;
        SearchNames.Add(ClassName);
        if (!ClassName.StartsWith(TEXT("Particle")))
        {
            SearchNames.Add(FString::Printf(TEXT("Particle%s"), *ClassName));
        }
        for (const FString& SearchName : SearchNames)
        {
            const FString Path = FString::Printf(TEXT("/Script/Engine.%s"), *SearchName);
            if (UClass* Found = FindObject<UClass>(nullptr, *Path))
            {
                if (Found->IsChildOf(BaseClass))
                {
                    return Found;
                }
            }
        }

        const FString WantedNormalized = NormalizeName(ClassName);
        UClass* Match = nullptr;
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Candidate = *It;
            if (!Candidate || !Candidate->IsChildOf(BaseClass) ||
                Candidate->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
            {
                continue;
            }

            const FString CandidateName = Candidate->GetName();
            const bool bSeeded = CandidateName.EndsWith(TEXT("_Seeded"));
            if (bSeeded && !bAllowSeeded)
            {
                continue;
            }
            OutCandidates.AddUnique(CandidateName);

            if (Match)
            {
                continue;
            }

            TArray<FString> Forms;
            CollectNameForms(CandidateName, Forms);
            for (const FString& Form : Forms)
            {
                if (NormalizeName(Form) == WantedNormalized)
                {
                    Match = Candidate;
                    break;
                }
            }
        }

        if (Match)
        {
            return Match;
        }

        OutCandidates.Sort();
        OutErrorCode = NotFoundCode;
        OutErrorMessage = FString::Printf(TEXT("No %s subclass matches '%s'"), *BaseClass->GetName(), *ClassName);
        return nullptr;
    }

    // -----------------------------------------------------------------------
    // Slot helpers
    // -----------------------------------------------------------------------

    bool IsKnownSlot(const FString& Slot)
    {
        return Slot == EUnrealMCPParticleSlot::Modules || Slot == EUnrealMCPParticleSlot::Required ||
               Slot == EUnrealMCPParticleSlot::TypeData || Slot == EUnrealMCPParticleSlot::Spawn ||
               Slot == EUnrealMCPParticleSlot::EventGenerator;
    }

    /**
     * Candidates for an unknown property name: the normalised suggestions when there are any,
     * otherwise every editable property of the module, so a typo is always self correctable
     * (same convention the blueprint node property writer uses).
     */
    void CollectPropertyCandidates(UParticleModule* Module, const FString& PropertyName,
                                   TArray<FString>& OutCandidates)
    {
        OutCandidates.Reset();
        if (!Module)
        {
            return;
        }

        FUnrealMCPCommonUtils::FindPropertyNameSuggestions(Module, PropertyName, OutCandidates);
        if (OutCandidates.Num() > 0)
        {
            return;
        }

        for (TFieldIterator<FProperty> It(Module->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
        {
            FProperty* Property = *It;
            if (Property && Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_Transient))
            {
                OutCandidates.Add(Property->GetName());
            }
        }
        OutCandidates.Sort();
    }

    /**
     * Aliases for properties whose real name is not the one an author reaches for: Cascade's UI and
     * the notes use the short name ("RotationRate") while the reflected property is not it
     * ("RotationRateAmount"), and guessing wrong used to cost a whole round trip.
     */
    const TMap<FString, FString>& GetParticlePropertyAliases()
    {
        static const TMap<FString, FString> Aliases = {
            { TEXT("rotationrate"), TEXT("RotationRateAmount") },
        };
        return Aliases;
    }

    /** Resolve a property alias (case/underscore insensitive); false when it is not an alias. */
    bool ResolveParticlePropertyAlias(const FString& PropertyName, FString& OutPropertyName)
    {
        if (PropertyName.IsEmpty())
        {
            return false;
        }

        const FString* Alias = GetParticlePropertyAliases().Find(NormalizeName(PropertyName));
        if (!Alias)
        {
            return false;
        }

        OutPropertyName = *Alias;
        return true;
    }

    /**
     * Unit / reading hint for the properties whose value meaning is not self evident (turn per
     * second, a multiplier applied to another value, ...). Left empty when there is nothing to say,
     * so an uncovered property never reports a made up unit.
     */
    void FindParticlePropertyHint(const FString& PropertyName, FString& OutUnits, FString& OutHint)
    {
        OutUnits.Reset();
        OutHint.Reset();

        struct FPropertyHint
        {
            const TCHAR* Units;
            const TCHAR* Hint;
        };

        static const TMap<FString, FPropertyHint> Hints = {
            { TEXT("rotationrateamount"), { TEXT("turns per second"), TEXT("0.55 is about 198 degrees per second") } },
            { TEXT("rotationamount"),     { TEXT("turns"),           TEXT("orbit rotation applied per particle") } },
            { TEXT("radiusscale"),        { TEXT("multiplier"),      TEXT("world radius = this value x particle Size") } },
            { TEXT("maxdrawcount"),       { TEXT("particle count"),  TEXT("0 with bUseMaxDrawCount=true draws no sprite (light-only emitter)") } },
            { TEXT("busemaxdrawcount"),   { nullptr,                 TEXT("true plus MaxDrawCount=0 hides the sprite of a light emitter") } },
        };

        if (const FPropertyHint* Found = Hints.Find(NormalizeName(PropertyName)))
        {
            if (Found->Units)
            {
                OutUnits = Found->Units;
            }
            OutHint = Found->Hint;
        }
    }

    /** Slots of one LOD level that reference the given module. */    void CollectModuleSlots(UParticleLODLevel* LODLevel, UParticleModule* Module, TArray<FString>& OutSlots)
    {
        if (!LODLevel || !Module)
        {
            return;
        }
        if (LODLevel->RequiredModule == Module)
        {
            OutSlots.AddUnique(EUnrealMCPParticleSlot::Required);
        }
        if (LODLevel->TypeDataModule == Module)
        {
            OutSlots.AddUnique(EUnrealMCPParticleSlot::TypeData);
        }
        if (LODLevel->SpawnModule == Module)
        {
            OutSlots.AddUnique(EUnrealMCPParticleSlot::Spawn);
        }
        if (LODLevel->EventGenerator == Module)
        {
            OutSlots.AddUnique(EUnrealMCPParticleSlot::EventGenerator);
        }
    }
}

// ---------------------------------------------------------------------------
// Asset / element resolution
// ---------------------------------------------------------------------------

bool FUnrealMCPParticleOps::ResolveParticleSystem(const FString& AssetPath, UParticleSystem*& OutSystem,
                                                  FString& OutErrorCode, FString& OutErrorMessage)
{
    OutSystem = nullptr;
    OutErrorCode.Reset();
    OutErrorMessage.Reset();

    if (AssetPath.IsEmpty())
    {
        OutErrorCode = EUnrealMCPParticleError::InvalidParams;
        OutErrorMessage = TEXT("Missing 'asset_path' parameter");
        return false;
    }

    UObject* Asset = FUnrealMCPCommonUtils::FindAsset(AssetPath);
    if (!Asset)
    {
        OutErrorCode = EUnrealMCPParticleError::AssetNotFound;
        OutErrorMessage = FString::Printf(TEXT("Asset not found: %s"), *AssetPath);
        return false;
    }

    OutSystem = Cast<UParticleSystem>(Asset);
    if (!OutSystem)
    {
        OutErrorCode = EUnrealMCPParticleError::AssetNotParticleSystem;
        OutErrorMessage = FString::Printf(TEXT("Asset '%s' is a %s, not a UParticleSystem"),
            *AssetPath, *Asset->GetClass()->GetName());
        return false;
    }

    return true;
}

bool FUnrealMCPParticleOps::ResolveEmitter(UParticleSystem* System, int32 EmitterIndex, UParticleEmitter*& OutEmitter,
                                           FString& OutErrorCode, FString& OutErrorMessage,
                                           TArray<FString>& OutCandidates)
{
    OutEmitter = nullptr;
    OutErrorCode.Reset();
    OutErrorMessage.Reset();
    OutCandidates.Reset();

    if (!System)
    {
        OutErrorCode = EUnrealMCPParticleError::ParticleSystemNotReady;
        OutErrorMessage = TEXT("Invalid particle system");
        return false;
    }

    if (System->Emitters.IsValidIndex(EmitterIndex))
    {
        OutEmitter = System->Emitters[EmitterIndex];
        if (!OutEmitter)
        {
            OutErrorCode = EUnrealMCPParticleError::ParticleSystemNotReady;
            OutErrorMessage = FString::Printf(TEXT("Emitter %d of '%s' is null"), EmitterIndex, *System->GetName());
            return false;
        }
        return true;
    }

    for (int32 Index = 0; Index < System->Emitters.Num(); ++Index)
    {
        OutCandidates.Add(FString::FromInt(Index));
    }

    OutErrorCode = EUnrealMCPParticleError::EmitterIndexOutOfRange;
    OutErrorMessage = FString::Printf(TEXT("Emitter index %d is out of range for '%s' (%d emitter(s); valid: %s)"),
        EmitterIndex, *System->GetName(), System->Emitters.Num(),
        OutCandidates.Num() > 0 ? *FString::Join(OutCandidates, TEXT(", ")) : TEXT("<none>"));
    return false;
}

bool FUnrealMCPParticleOps::ResolveLODLevel(UParticleEmitter* Emitter, int32 LODIndex, UParticleLODLevel*& OutLODLevel,
                                            FString& OutErrorCode, FString& OutErrorMessage,
                                            TArray<FString>& OutCandidates)
{
    OutLODLevel = nullptr;
    OutErrorCode.Reset();
    OutErrorMessage.Reset();
    OutCandidates.Reset();

    if (!Emitter)
    {
        OutErrorCode = EUnrealMCPParticleError::ParticleSystemNotReady;
        OutErrorMessage = TEXT("Invalid emitter");
        return false;
    }

    if (Emitter->LODLevels.IsValidIndex(LODIndex) && Emitter->LODLevels[LODIndex])
    {
        OutLODLevel = Emitter->LODLevels[LODIndex];
        return true;
    }

    for (int32 Index = 0; Index < Emitter->LODLevels.Num(); ++Index)
    {
        OutCandidates.Add(FString::FromInt(Index));
    }

    OutErrorCode = EUnrealMCPParticleError::LODIndexOutOfRange;
    OutErrorMessage = FString::Printf(TEXT("LOD index %d is out of range for emitter '%s' (%d LOD(s); valid: %s)"),
        LODIndex, *Emitter->GetName(), Emitter->LODLevels.Num(),
        OutCandidates.Num() > 0 ? *FString::Join(OutCandidates, TEXT(", ")) : TEXT("<none>"));
    return false;
}

bool FUnrealMCPParticleOps::ResolveModuleClass(const FString& ModuleClassName, UClass*& OutClass,
                                               FString& OutErrorCode, FString& OutErrorMessage,
                                               TArray<FString>& OutCandidates)
{
    OutClass = FindClassByShortName(ModuleClassName, UParticleModule::StaticClass(), /*bAllowSeeded=*/true,
        EUnrealMCPParticleError::ModuleClassNotFound, OutErrorCode, OutErrorMessage, OutCandidates);
    return OutClass != nullptr;
}

bool FUnrealMCPParticleOps::ResolveEmitterClass(const FString& EmitterClassName, UClass*& OutClass,
                                                FString& OutErrorCode, FString& OutErrorMessage,
                                                TArray<FString>& OutCandidates)
{
    OutClass = FindClassByShortName(EmitterClassName, UParticleEmitter::StaticClass(), /*bAllowSeeded=*/true,
        EUnrealMCPParticleError::EmitterClassNotFound, OutErrorCode, OutErrorMessage, OutCandidates);
    return OutClass != nullptr;
}

/**
 * Resolve a module inside one specific slot of one LOD level.
 * ModuleIndex < 0 means "not given": a class that matches several instances of the module
 * list then reports ambiguous_module with every instance's index as candidate.
 */
static bool ResolveModuleInList(UParticleEmitter* Emitter, int32 LODIndex, const FString& ModuleClassName,
                                int32 ModuleIndex, const FString& Slot, UParticleModule*& OutModule,
                                int32& OutModuleIndex, FString& OutErrorCode, FString& OutErrorMessage,
                                TArray<FString>& OutCandidates)
{
    OutModule = nullptr;
    OutModuleIndex = -1;
    OutErrorCode.Reset();
    OutErrorMessage.Reset();
    OutCandidates.Reset();

    UParticleLODLevel* LODLevel = nullptr;
    if (!FUnrealMCPParticleOps::ResolveLODLevel(Emitter, LODIndex, LODLevel, OutErrorCode, OutErrorMessage, OutCandidates))
    {
        return false;
    }

    const FString SearchSlot = Slot.IsEmpty() ? FString(EUnrealMCPParticleSlot::Modules) : Slot;
    if (!IsKnownSlot(SearchSlot))
    {
        OutErrorCode = EUnrealMCPParticleError::UnsupportedModuleSlot;
        OutErrorMessage = FString::Printf(TEXT("Unknown module slot '%s'"), *SearchSlot);
        OutCandidates = FUnrealMCPParticleOps::SupportedModuleSlots();
        return false;
    }

    // A slot other than "modules" holds at most one module: resolve it by field.
    if (SearchSlot != EUnrealMCPParticleSlot::Modules)
    {
        UParticleModule* SlotModule = nullptr;
        if (SearchSlot == EUnrealMCPParticleSlot::Required)
        {
            SlotModule = LODLevel->RequiredModule;
        }
        else if (SearchSlot == EUnrealMCPParticleSlot::TypeData)
        {
            SlotModule = LODLevel->TypeDataModule;
        }
        else if (SearchSlot == EUnrealMCPParticleSlot::Spawn)
        {
            SlotModule = LODLevel->SpawnModule;
        }
        else if (SearchSlot == EUnrealMCPParticleSlot::EventGenerator)
        {
            SlotModule = LODLevel->EventGenerator;
        }

        if (!SlotModule)
        {
            OutErrorCode = EUnrealMCPParticleError::ModuleNotFound;
            OutErrorMessage = FString::Printf(TEXT("Slot '%s' of emitter '%s' LOD %d is empty"),
                *SearchSlot, *Emitter->GetName(), LODIndex);
            OutCandidates = FUnrealMCPParticleOps::SupportedModuleSlots();
            return false;
        }

        if (!ModuleClassName.IsEmpty())
        {
            UClass* WantedClass = nullptr;
            if (!FUnrealMCPParticleOps::ResolveModuleClass(ModuleClassName, WantedClass, OutErrorCode, OutErrorMessage, OutCandidates))
            {
                return false;
            }
            if (!SlotModule->IsA(WantedClass))
            {
                OutErrorCode = EUnrealMCPParticleError::ModuleNotFound;
                OutErrorMessage = FString::Printf(TEXT("Slot '%s' holds %s, not %s"),
                    *SearchSlot, *ClassNameOf(SlotModule), *WantedClass->GetName());
                OutCandidates.Add(ClassNameOf(SlotModule));
                return false;
            }
        }

        OutModule = SlotModule;
        OutModuleIndex = -1;
        return true;
    }

    if (ModuleClassName.IsEmpty() && ModuleIndex < 0)
    {
        OutErrorCode = EUnrealMCPParticleError::InvalidParams;
        OutErrorMessage = TEXT("Pass 'module_class' or 'module_index' to identify a module");
        return false;
    }

    UClass* WantedClass = nullptr;
    if (!ModuleClassName.IsEmpty() &&
        !FUnrealMCPParticleOps::ResolveModuleClass(ModuleClassName, WantedClass, OutErrorCode, OutErrorMessage, OutCandidates))
    {
        return false;
    }

    TArray<int32> Matches;
    for (int32 Index = 0; Index < LODLevel->Modules.Num(); ++Index)
    {
        UParticleModule* Candidate = LODLevel->Modules[Index];
        if (!Candidate)
        {
            continue;
        }
        if (WantedClass && !Candidate->IsA(WantedClass))
        {
            continue;
        }
        Matches.Add(Index);
    }

    if (Matches.Num() == 0)
    {
        OutCandidates.Reset();
        OutErrorCode = EUnrealMCPParticleError::ModuleNotFound;
        OutErrorMessage = FString::Printf(TEXT("No module %s in emitter '%s' LOD %d"),
            ModuleClassName.IsEmpty() ? TEXT("at that index") : *FString::Printf(TEXT("of class %s"), *ModuleClassName),
            *Emitter->GetName(), LODIndex);
        for (int32 Index = 0; Index < LODLevel->Modules.Num(); ++Index)
        {
            OutCandidates.Add(FString::Printf(TEXT("%d:%s"), Index,
                LODLevel->Modules[Index] ? *ClassNameOf(LODLevel->Modules[Index]) : TEXT("<null>")));
        }
        return false;
    }

    if (ModuleIndex >= 0)
    {
        if (!Matches.Contains(ModuleIndex))
        {
            OutCandidates.Reset();
            OutErrorCode = EUnrealMCPParticleError::ModuleNotFound;
            OutErrorMessage = FString::Printf(TEXT("No module at index %d of emitter '%s' LOD %d matching %s"),
                ModuleIndex, *Emitter->GetName(), LODIndex,
                ModuleClassName.IsEmpty() ? TEXT("any class") : *ModuleClassName);
            for (const int32 Match : Matches)
            {
                OutCandidates.Add(FString::FromInt(Match));
            }
            return false;
        }

        OutModule = LODLevel->Modules[ModuleIndex];
        OutModuleIndex = ModuleIndex;
        return true;
    }

    if (Matches.Num() > 1)
    {
        OutCandidates.Reset();
        OutErrorCode = EUnrealMCPParticleError::AmbiguousModule;
        OutErrorMessage = FString::Printf(
            TEXT("%d modules of class %s in emitter '%s' LOD %d; pass 'module_index'"),
            Matches.Num(), *ModuleClassName, *Emitter->GetName(), LODIndex);
        for (const int32 Match : Matches)
        {
            UParticleModule* Candidate = LODLevel->Modules[Match];
            OutCandidates.Add(FString::Printf(TEXT("%d:lod_validity=%d"), Match,
                Candidate ? static_cast<int32>(Candidate->LODValidity) : 0));
        }
        return false;
    }

    OutModuleIndex = Matches[0];
    OutModule = LODLevel->Modules[OutModuleIndex];
    return true;
}

bool FUnrealMCPParticleOps::ResolveModule(UParticleEmitter* Emitter, int32 LODIndex, const FString& ModuleClassName,
                                          int32 ModuleIndex, const FString& Slot, UParticleModule*& OutModule,
                                          int32& OutModuleIndex, FString& OutErrorCode, FString& OutErrorMessage,
                                          TArray<FString>& OutCandidates)
{
    OutModule = nullptr;
    OutModuleIndex = -1;
    OutErrorCode.Reset();
    OutErrorMessage.Reset();
    OutCandidates.Reset();

    // An explicit slot (or an index, which only means something for the module list) is taken
    // literally.
    if (!Slot.IsEmpty() || ModuleIndex >= 0)
    {
        return ResolveModuleInList(Emitter, LODIndex, ModuleClassName, ModuleIndex, Slot, OutModule,
            OutModuleIndex, OutErrorCode, OutErrorMessage, OutCandidates);
    }

    FString ListErrorCode;
    FString ListErrorMessage;
    TArray<FString> ListCandidates;
    if (ResolveModuleInList(Emitter, LODIndex, ModuleClassName, ModuleIndex,
                            FString(EUnrealMCPParticleSlot::Modules), OutModule, OutModuleIndex,
                            ListErrorCode, ListErrorMessage, ListCandidates))
    {
        return true;
    }

    // A class can also live in one of the single-module slots (Required, TypeData, Spawn, event
    // generator); those modules are not in the module list, so a class lookup falls back to them.
    // An ambiguous module list is reported as is - the caller has to disambiguate first - and an
    // empty class name is not a lookup at all (the module list already reported invalid_params).
    if (!ModuleClassName.IsEmpty() && ListErrorCode != EUnrealMCPParticleError::AmbiguousModule)
    {
        const TCHAR* FallbackSlots[] = {
            EUnrealMCPParticleSlot::Required,
            EUnrealMCPParticleSlot::TypeData,
            EUnrealMCPParticleSlot::Spawn,
            EUnrealMCPParticleSlot::EventGenerator
        };
        for (const TCHAR* FallbackSlot : FallbackSlots)
        {
            FString SlotErrorCode;
            FString SlotErrorMessage;
            TArray<FString> SlotCandidates;
            if (ResolveModuleInList(Emitter, LODIndex, ModuleClassName, -1, FString(FallbackSlot), OutModule,
                                    OutModuleIndex, SlotErrorCode, SlotErrorMessage, SlotCandidates))
            {
                OutErrorCode.Reset();
                OutErrorMessage.Reset();
                OutCandidates.Reset();
                return true;
            }
        }
    }

    OutErrorCode = ListErrorCode;
    OutErrorMessage = ListErrorMessage;
    OutCandidates = ListCandidates;
    return false;
}

// ---------------------------------------------------------------------------
// Reads
// ---------------------------------------------------------------------------

void FUnrealMCPParticleOps::MakeLODInfo(UParticleSystem* System, UParticleEmitter* Emitter, int32 LODIndex,
                                        FUnrealMCPParticleLODInfo& OutInfo)
{
    OutInfo = FUnrealMCPParticleLODInfo();
    OutInfo.LODIndex = LODIndex;

    UParticleLODLevel* LODLevel = (Emitter && Emitter->LODLevels.IsValidIndex(LODIndex))
        ? Emitter->LODLevels[LODIndex] : nullptr;
    if (!LODLevel)
    {
        return;
    }

    OutInfo.Level = LODLevel->Level;
    OutInfo.Enabled = LODLevel->bEnabled != 0;
    OutInfo.ModuleCount = LODLevel->Modules.Num();
    OutInfo.RequiredModuleClass = SlotClassName(LODLevel->RequiredModule);
    OutInfo.TypeDataModuleClass = SlotClassName(LODLevel->TypeDataModule);
    OutInfo.SpawnModuleClass = SlotClassName(LODLevel->SpawnModule);
    OutInfo.EventGeneratorClass = SlotClassName(LODLevel->EventGenerator);

    for (const UParticleModuleSpawnBase* Module : LODLevel->SpawningModules)
    {
        OutInfo.SpawningModuleClasses.Add(SlotClassName(Module));
    }
    for (const UParticleModule* Module : LODLevel->SpawnModules)
    {
        OutInfo.SpawnModuleClasses.Add(SlotClassName(Module));
    }
    for (const UParticleModule* Module : LODLevel->UpdateModules)
    {
        OutInfo.UpdateModuleClasses.Add(SlotClassName(Module));
    }

    for (int32 ModuleIndex = 0; ModuleIndex < LODLevel->Modules.Num(); ++ModuleIndex)
    {
        UParticleModule* Module = LODLevel->Modules[ModuleIndex];
        if (!Module)
        {
            continue;
        }

        TArray<FString> Slots;
        CollectModuleSlots(LODLevel, Module, Slots);
        const FString Slot = Slots.Num() > 0 ? Slots[0] : FString(EUnrealMCPParticleSlot::Modules);

        FUnrealMCPParticleModuleInfo ModuleInfo;
        MakeModuleInfo(Module, ModuleIndex, Slot, ModuleInfo);
        ModuleInfo.Editable = LODLevel->IsModuleEditable(Module);
        OutInfo.Modules.Add(MoveTemp(ModuleInfo));
    }
}

void FUnrealMCPParticleOps::MakeEmitterInfo(UParticleSystem* System, int32 EmitterIndex,
                                            FUnrealMCPParticleEmitterInfo& OutInfo)
{
    OutInfo = FUnrealMCPParticleEmitterInfo();
    OutInfo.EmitterIndex = EmitterIndex;

    UParticleEmitter* Emitter = (System && System->Emitters.IsValidIndex(EmitterIndex))
        ? System->Emitters[EmitterIndex] : nullptr;
    if (!Emitter)
    {
        return;
    }

    OutInfo.EmitterClass = ClassNameOf(Emitter);
    OutInfo.EmitterName = Emitter->GetEmitterName().ToString();
    OutInfo.ObjectPath = Emitter->GetPathName();
    OutInfo.Emitter = Emitter;
    OutInfo.LODCount = Emitter->LODLevels.Num();
    for (int32 LODIndex = 0; LODIndex < Emitter->LODLevels.Num(); ++LODIndex)
    {
        FUnrealMCPParticleLODInfo LODInfo;
        MakeLODInfo(System, Emitter, LODIndex, LODInfo);
        OutInfo.Lods.Add(MoveTemp(LODInfo));
    }
}

void FUnrealMCPParticleOps::MakeModuleInfo(UParticleModule* Module, int32 ModuleIndex,
                                           const FString& Slot, FUnrealMCPParticleModuleInfo& OutInfo)
{
    OutInfo = FUnrealMCPParticleModuleInfo();
    OutInfo.ModuleIndex = ModuleIndex;
    OutInfo.Slot = Slot;
    if (!Module)
    {
        return;
    }

    OutInfo.ModuleClass = ClassNameOf(Module);
    OutInfo.ModuleName = Module->GetName();
    OutInfo.Module = Module;
    OutInfo.ObjectPath = Module->GetPathName();
    OutInfo.LODValidity = static_cast<int32>(Module->LODValidity);
    for (int32 Bit = 0; Bit < 8; ++Bit)
    {
        if ((Module->LODValidity & (1 << Bit)) != 0)
        {
            OutInfo.ActiveLods.Add(Bit);
        }
    }

    for (TFieldIterator<FProperty> It(Module->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
    {
        FProperty* Property = *It;
        if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Transient))
        {
            continue;
        }

        bool bVectorDistribution = false;
        if (IsRawDistributionProperty(Property, bVectorDistribution))
        {
            FUnrealMCPParticleDistributionInfo DistributionInfo;
            if (MakeDistributionInfo(Module, Property, DistributionInfo))
            {
                OutInfo.Distributions.Add(MoveTemp(DistributionInfo));
            }
            continue;
        }

        FUnrealMCPParticlePropertyInfo PropertyInfo;
        PropertyInfo.Name = Property->GetName();
        PropertyInfo.Type = Property->GetCPPType();
        PropertyInfo.Value = PropertyValueToResponseText(Property, Property->ContainerPtrToValuePtr<void>(Module));
        FindParticlePropertyHint(PropertyInfo.Name, PropertyInfo.Units, PropertyInfo.Hint);
        OutInfo.Properties.Add(MoveTemp(PropertyInfo));
    }
}

bool FUnrealMCPParticleOps::MakeDistributionInfo(UParticleModule* Module, FProperty* Property,
                                                 FUnrealMCPParticleDistributionInfo& OutInfo)
{
    OutInfo = FUnrealMCPParticleDistributionInfo();
    if (!Module || !Property)
    {
        return false;
    }

    bool bVector = false;
    if (!IsRawDistributionProperty(Property, bVector))
    {
        return false;
    }

    OutInfo.PropertyName = Property->GetName();
    FindParticlePropertyHint(OutInfo.PropertyName, OutInfo.Units, OutInfo.Hint);

    UDistribution* Distribution = GetRawDistributionObject(Module, Property, bVector);
    OutInfo.DistributionClass = Distribution ? Distribution->GetClass()->GetName() : FString();
    OutInfo.Kind = Distribution ? KindOfDistribution(Distribution) : TEXT("none");

    if (const UDistributionFloatConstant* Constant = Cast<UDistributionFloatConstant>(Distribution))
    {
        OutInfo.Constants.Add(Constant->Constant);
    }
    else if (const UDistributionVectorConstant* VectorConstant = Cast<UDistributionVectorConstant>(Distribution))
    {
        AppendValue(VectorConstant->Constant, OutInfo.Constants);
    }
    else if (const UDistributionFloatUniform* Uniform = Cast<UDistributionFloatUniform>(Distribution))
    {
        OutInfo.MinMax.Add(Uniform->Min);
        OutInfo.MinMax.Add(Uniform->Max);
    }
    else if (const UDistributionVectorUniform* VectorUniform = Cast<UDistributionVectorUniform>(Distribution))
    {
        AppendValue(VectorUniform->Min, OutInfo.MinMax);
        AppendValue(VectorUniform->Max, OutInfo.MinMax);
    }
    else if (const UDistributionFloatConstantCurve* Curve = Cast<UDistributionFloatConstantCurve>(Distribution))
    {
        ReadCurveKeys(Curve->ConstantCurve, OutInfo.Keys);
    }
    else if (const UDistributionFloatUniformCurve* UniformCurve = Cast<UDistributionFloatUniformCurve>(Distribution))
    {
        ReadCurveKeys(UniformCurve->ConstantCurve, OutInfo.Keys);
    }
    else if (const UDistributionVectorConstantCurve* VectorCurve = Cast<UDistributionVectorConstantCurve>(Distribution))
    {
        ReadCurveKeys(VectorCurve->ConstantCurve, OutInfo.Keys);
    }
    else if (const UDistributionVectorUniformCurve* VectorUniformCurve = Cast<UDistributionVectorUniformCurve>(Distribution))
    {
        ReadCurveKeys(VectorUniformCurve->ConstantCurve, OutInfo.Keys);
    }

    if (bVector)
    {
        void* Address = Property->ContainerPtrToValuePtr<void>(Module);
        FRawDistributionVector* Raw = Address ? static_cast<FRawDistributionVector*>(Address) : nullptr;
        if (Raw && Raw->Distribution)
        {
            AppendValue(Raw->GetValue(0.0f), OutInfo.SampledValue);
        }
    }
    else
    {
        void* Address = Property->ContainerPtrToValuePtr<void>(Module);
        FRawDistributionFloat* Raw = Address ? static_cast<FRawDistributionFloat*>(Address) : nullptr;
        if (Raw && Raw->Distribution)
        {
            OutInfo.SampledValue.Add(Raw->GetValue(0.0f));
        }
    }

    return true;
}

TArray<FString> FUnrealMCPParticleOps::SupportedDistributionKinds()
{
    TArray<FString> Kinds;
    Kinds.Add(EUnrealMCPParticleDistributionKind::Constant);
    Kinds.Add(EUnrealMCPParticleDistributionKind::Uniform);
    Kinds.Add(EUnrealMCPParticleDistributionKind::ConstantCurve);
    Kinds.Add(EUnrealMCPParticleDistributionKind::UniformCurve);
    return Kinds;
}

FString FUnrealMCPParticleOps::DetermineModuleSlot(UParticleEmitter* Emitter, int32 LODIndex, UParticleModule* Module)
{
    UParticleLODLevel* LODLevel = (Emitter && Emitter->LODLevels.IsValidIndex(LODIndex))
        ? Emitter->LODLevels[LODIndex] : nullptr;

    TArray<FString> Slots;
    CollectModuleSlots(LODLevel, Module, Slots);
    return Slots.Num() > 0 ? Slots[0] : FString(EUnrealMCPParticleSlot::Modules);
}

TArray<FString> FUnrealMCPParticleOps::SupportedModuleSlots()
{
    TArray<FString> Slots;
    Slots.Add(EUnrealMCPParticleSlot::Modules);
    Slots.Add(EUnrealMCPParticleSlot::Required);
    Slots.Add(EUnrealMCPParticleSlot::TypeData);
    Slots.Add(EUnrealMCPParticleSlot::Spawn);
    Slots.Add(EUnrealMCPParticleSlot::EventGenerator);
    return Slots;
}

bool FUnrealMCPParticleOps::ListEmitters(UParticleSystem* System, TArray<FUnrealMCPParticleEmitterInfo>& OutEmitters,
                                         FString& OutErrorCode, FString& OutErrorMessage,
                                         TArray<FString>& OutCandidates)
{
    OutEmitters.Reset();
    OutErrorCode.Reset();
    OutErrorMessage.Reset();
    OutCandidates.Reset();

    if (!System)
    {
        OutErrorCode = EUnrealMCPParticleError::ParticleSystemNotReady;
        OutErrorMessage = TEXT("Invalid particle system");
        return false;
    }

    for (int32 EmitterIndex = 0; EmitterIndex < System->Emitters.Num(); ++EmitterIndex)
    {
        FUnrealMCPParticleEmitterInfo EmitterInfo;
        MakeEmitterInfo(System, EmitterIndex, EmitterInfo);
        OutEmitters.Add(MoveTemp(EmitterInfo));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Writes
// ---------------------------------------------------------------------------

void FUnrealMCPParticleOps::Fail(FUnrealMCPParticleOpResult& OutResult, const FString& ErrorCode,
                                 const FString& ErrorMessage, const TArray<FString>& Candidates,
                                 const TArray<FString>& AvailableFields)
{
    OutResult.Success = false;
    OutResult.ErrorCode = ErrorCode;
    OutResult.ErrorMessage = ErrorMessage;
    OutResult.Candidates = Candidates;
    OutResult.AvailableFields = AvailableFields;
}

void FUnrealMCPParticleOps::FillEmitterReadback(UParticleSystem* System, FUnrealMCPParticleOpResult& OutResult)
{
    if (!System)
    {
        return;
    }

    OutResult.AssetPath = System->GetPathName();
    OutResult.Emitters.Reset();
    for (int32 EmitterIndex = 0; EmitterIndex < System->Emitters.Num(); ++EmitterIndex)
    {
        FUnrealMCPParticleEmitterInfo EmitterInfo;
        MakeEmitterInfo(System, EmitterIndex, EmitterInfo);
        OutResult.Emitters.Add(MoveTemp(EmitterInfo));
    }
}

namespace
{
    /** Defined below; AddEmitterToSystem (and the other structural writes) need them. */
    void RefreshSoloing(UParticleSystem* System);
    int32 SharedLODCount(UParticleSystem* System);
    bool EqualizeLODCounts(UParticleSystem* System, int32 TargetCount, TArray<int32>& OutAdjustedEmitters,
                           FString& OutErrorCode, FString& OutErrorMessage);
    bool ResizeEmitterLODLevels(UParticleEmitter* Emitter, int32 TargetCount, FString& OutErrorCode,
                                FString& OutErrorMessage);

    // -----------------------------------------------------------------------
    // Curve values
    // -----------------------------------------------------------------------

    bool MakeCurveValue(float& Out, const TArray<float>& Values, int32 KeyIndex, const FString& PropertyName, FString& OutError)
    {
        if (Values.Num() != 1)
        {
            OutError = FString::Printf(TEXT("Property '%s' key %d: expects 1 value, got %d"),
                *PropertyName, KeyIndex, Values.Num());
            return false;
        }
        Out = Values[0];
        return true;
    }

    bool MakeCurveValue(FVector& Out, const TArray<float>& Values, int32 KeyIndex, const FString& PropertyName, FString& OutError)
    {
        if (Values.Num() == 1)
        {
            Out = FVector(Values[0], Values[0], Values[0]);
            return true;
        }
        if (Values.Num() != 3)
        {
            OutError = FString::Printf(TEXT("Property '%s' key %d: expects 3 values (or 1 to broadcast), got %d"),
                *PropertyName, KeyIndex, Values.Num());
            return false;
        }
        Out = FVector(Values[0], Values[1], Values[2]);
        return true;
    }

    bool MakeCurveValue(FVector2D& Out, const TArray<float>& Values, int32 KeyIndex, const FString& PropertyName, FString& OutError)
    {
        if (Values.Num() != 2)
        {
            OutError = FString::Printf(TEXT("Property '%s' key %d: a uniform curve expects 2 values (min, max), got %d"),
                *PropertyName, KeyIndex, Values.Num());
            return false;
        }
        Out = FVector2D(Values[0], Values[1]);
        return true;
    }

    bool MakeCurveValue(FTwoVectors& Out, const TArray<float>& Values, int32 KeyIndex, const FString& PropertyName, FString& OutError)
    {
        if (Values.Num() != 6)
        {
            OutError = FString::Printf(TEXT("Property '%s' key %d: a vector uniform curve expects 6 values (min xyz, max xyz), got %d"),
                *PropertyName, KeyIndex, Values.Num());
            return false;
        }
        Out = FTwoVectors(FVector(Values[0], Values[1], Values[2]), FVector(Values[3], Values[4], Values[5]));
        return true;
    }

    bool MakeConstantVector(const TArray<float>& Values, FVector& Out, const FString& PropertyName, FString& OutError)
    {
        if (Values.Num() == 1)
        {
            Out = FVector(Values[0], Values[0], Values[0]);
            return true;
        }
        if (Values.Num() != 3)
        {
            OutError = FString::Printf(TEXT("Property '%s': a vector constant expects 3 values (or 1 to broadcast), got %d"),
                *PropertyName, Values.Num());
            return false;
        }
        Out = FVector(Values[0], Values[1], Values[2]);
        return true;
    }

    /** Write the values of the requested kind into an existing distribution object. */
    bool ApplyDistributionValues(UDistribution* Distribution, bool bVector, const FString& Kind,
                                 const TArray<float>& Values, const TArray<FUnrealMCPParticleDistributionKey>& Keys,
                                 const FString& PropertyName, FString& OutError)
    {
        OutError.Reset();
        if (!Distribution)
        {
            OutError = TEXT("Invalid distribution object");
            return false;
        }

        if (Kind == EUnrealMCPParticleDistributionKind::Constant)
        {
            if (bVector)
            {
                UDistributionVectorConstant* ConstantDistribution = Cast<UDistributionVectorConstant>(Distribution);
                if (!ConstantDistribution)
                {
                    OutError = FString::Printf(TEXT("Property '%s': distribution class mismatch"), *PropertyName);
                    return false;
                }
                FVector Constant;
                if (!MakeConstantVector(Values, Constant, PropertyName, OutError))
                {
                    return false;
                }
                ConstantDistribution->Constant = Constant;
                return true;
            }

            UDistributionFloatConstant* ConstantDistribution = Cast<UDistributionFloatConstant>(Distribution);
            if (!ConstantDistribution)
            {
                OutError = FString::Printf(TEXT("Property '%s': distribution class mismatch"), *PropertyName);
                return false;
            }
            if (Values.Num() != 1)
            {
                OutError = FString::Printf(TEXT("Property '%s': a float constant expects 1 value, got %d"),
                    *PropertyName, Values.Num());
                return false;
            }
            ConstantDistribution->Constant = Values[0];
            return true;
        }

        if (Kind == EUnrealMCPParticleDistributionKind::Uniform)
        {
            if (bVector)
            {
                UDistributionVectorUniform* UniformDistribution = Cast<UDistributionVectorUniform>(Distribution);
                if (!UniformDistribution)
                {
                    OutError = FString::Printf(TEXT("Property '%s': distribution class mismatch"), *PropertyName);
                    return false;
                }
                if (Values.Num() == 2)
                {
                    UniformDistribution->Min = FVector(Values[0], Values[0], Values[0]);
                    UniformDistribution->Max = FVector(Values[1], Values[1], Values[1]);
                    return true;
                }
                if (Values.Num() != 6)
                {
                    OutError = FString::Printf(
                        TEXT("Property '%s': a vector uniform expects 2 values (min, max) or 6 (min xyz, max xyz), got %d"),
                        *PropertyName, Values.Num());
                    return false;
                }
                UniformDistribution->Min = FVector(Values[0], Values[1], Values[2]);
                UniformDistribution->Max = FVector(Values[3], Values[4], Values[5]);
                return true;
            }

            UDistributionFloatUniform* UniformDistribution = Cast<UDistributionFloatUniform>(Distribution);
            if (!UniformDistribution)
            {
                OutError = FString::Printf(TEXT("Property '%s': distribution class mismatch"), *PropertyName);
                return false;
            }
            if (Values.Num() != 2)
            {
                OutError = FString::Printf(TEXT("Property '%s': a float uniform expects 2 values (min, max), got %d"),
                    *PropertyName, Values.Num());
                return false;
            }
            UniformDistribution->Min = Values[0];
            UniformDistribution->Max = Values[1];
            return true;
        }

        if (Kind == EUnrealMCPParticleDistributionKind::ConstantCurve)
        {
            if (Keys.Num() == 0)
            {
                OutError = FString::Printf(TEXT("Property '%s': kind 'constant_curve' requires 'keys'"), *PropertyName);
                return false;
            }
            if (bVector)
            {
                UDistributionVectorConstantCurve* CurveDistribution = Cast<UDistributionVectorConstantCurve>(Distribution);
                if (!CurveDistribution)
                {
                    OutError = FString::Printf(TEXT("Property '%s': distribution class mismatch"), *PropertyName);
                    return false;
                }
                return WriteCurveKeys(CurveDistribution->ConstantCurve, Keys, PropertyName, OutError);
            }

            UDistributionFloatConstantCurve* CurveDistribution = Cast<UDistributionFloatConstantCurve>(Distribution);
            if (!CurveDistribution)
            {
                OutError = FString::Printf(TEXT("Property '%s': distribution class mismatch"), *PropertyName);
                return false;
            }
            return WriteCurveKeys(CurveDistribution->ConstantCurve, Keys, PropertyName, OutError);
        }

        if (Kind == EUnrealMCPParticleDistributionKind::UniformCurve)
        {
            if (Keys.Num() == 0)
            {
                OutError = FString::Printf(TEXT("Property '%s': kind 'uniform_curve' requires 'keys'"), *PropertyName);
                return false;
            }
            if (bVector)
            {
                UDistributionVectorUniformCurve* CurveDistribution = Cast<UDistributionVectorUniformCurve>(Distribution);
                if (!CurveDistribution)
                {
                    OutError = FString::Printf(TEXT("Property '%s': distribution class mismatch"), *PropertyName);
                    return false;
                }
                return WriteCurveKeys(CurveDistribution->ConstantCurve, Keys, PropertyName, OutError);
            }

            UDistributionFloatUniformCurve* CurveDistribution = Cast<UDistributionFloatUniformCurve>(Distribution);
            if (!CurveDistribution)
            {
                OutError = FString::Printf(TEXT("Property '%s': distribution class mismatch"), *PropertyName);
                return false;
            }
            return WriteCurveKeys(CurveDistribution->ConstantCurve, Keys, PropertyName, OutError);
        }

        OutError = FString::Printf(TEXT("Unsupported distribution kind '%s'"), *Kind);
        return false;
    }

    /**
     * Add an emitter the way the Cascade editor does: create it with the particle system as
     * outer, let CreateLODLevel build the LOD levels (it generates the module data of the
     * levels it can copy from), give it editor defaults, then register it in the system.
     */
    bool AddEmitterToSystem(UParticleSystem* System, UClass* EmitterClass, int32 InsertIndex, int32 LODCount,
                            int32& OutInsertedIndex, TArray<int32>& OutAdjustedEmitters, FString& OutErrorCode,
                            FString& OutErrorMessage)
    {
        OutInsertedIndex = -1;
        OutAdjustedEmitters.Reset();
        if (!System || !EmitterClass)
        {
            OutErrorCode = EUnrealMCPParticleError::InvalidParams;
            OutErrorMessage = TEXT("Invalid particle system or emitter class");
            return false;
        }

        System->PreEditChange(nullptr);

        UParticleEmitter* NewEmitter = NewObject<UParticleEmitter>(System, EmitterClass, NAME_None, RF_Transactional);
        if (!NewEmitter)
        {
            OutErrorCode = EUnrealMCPParticleError::CreateFailed;
            OutErrorMessage = FString::Printf(TEXT("Failed to create an emitter of class %s"), *EmitterClass->GetName());
            return false;
        }

        // Sensible defaults first: they are what the higher LOD levels are generated from, so
        // creating the extra levels before them would leave those levels without modules.
        if (NewEmitter->CreateLODLevel(0, /*bGenerateModuleData=*/true) < 0)
        {
            OutErrorCode = EUnrealMCPParticleError::CreateFailed;
            OutErrorMessage = TEXT("Failed to create LOD 0 on the new emitter");
            return false;
        }

        NewEmitter->SetToSensibleDefaults();

        // All emitters of a system share one LOD count (PostLoad enforces it), so a new emitter
        // adopts the system's count unless the caller asked for a specific one.
        const int32 SystemLODCount = FMath::Max(1, SharedLODCount(System));
        const int32 WantedLods = (LODCount > 0) ? LODCount : SystemLODCount;
        for (int32 LODIndex = 1; LODIndex < WantedLods; ++LODIndex)
        {
            if (NewEmitter->CreateLODLevel(LODIndex, /*bGenerateModuleData=*/true) < 0)
            {
                OutErrorCode = EUnrealMCPParticleError::CreateFailed;
                OutErrorMessage = FString::Printf(TEXT("Failed to create LOD %d on the new emitter"), LODIndex);
                return false;
            }
        }

#if WITH_EDITORONLY_DATA
        NewEmitter->EmitterEditorColor = FColor::MakeRandomColor();
        NewEmitter->EmitterEditorColor.A = 255;
#endif

        // A sprite emitter renders nothing without a material on its required module.
        if (NewEmitter->IsA<UParticleSpriteEmitter>())
        {
            UMaterialInterface* DefaultMaterial = LoadObject<UMaterialInterface>(nullptr,
                TEXT("/Engine/EngineMaterials/DefaultParticle.DefaultParticle"));
            for (UParticleLODLevel* LODLevel : NewEmitter->LODLevels)
            {
                if (LODLevel && LODLevel->RequiredModule && !LODLevel->RequiredModule->Material)
                {
                    LODLevel->RequiredModule->Material = DefaultMaterial;
                }
            }
        }

        const int32 InsertAt = (InsertIndex < 0 || InsertIndex > System->Emitters.Num())
            ? System->Emitters.Num() : InsertIndex;
        System->Emitters.Insert(NewEmitter, InsertAt);

        // An explicit LOD count that differs from the rest of the system is legal input; the other
        // emitters are brought to it so the whole system keeps one shared count.
        if (!EqualizeLODCounts(System, WantedLods, OutAdjustedEmitters, OutErrorCode, OutErrorMessage))
        {
            return false;
        }

        System->UpdateAllModuleLists();
        RefreshSoloing(System);
        System->PostEditChange();
        NewEmitter->MarkPackageDirty();
        System->MarkPackageDirty();

        OutInsertedIndex = InsertAt;
        return true;
    }

    void PersistSystem(UParticleSystem* System)
    {
        // Go through the shared helper: the dirty flag is not trustworthy for programmatic edits.
        // Gated on the dispatch's persist decision so a batched particle edit flushes once.
        if (System && FUnrealMCPCommonUtils::IsPersistEnabled())
        {
            FUnrealMCPCommonUtils::SaveAssetForObject(System);
        }
    }

    /**
     * Keep UParticleSystem::SoloTracking sized to the emitter/LOD structure, exactly the way the
     * Cascade editor does after its own structural edits ("we may have changed the number of LODs,
     * so our soloing information could be invalid", Cascade.cpp:3291).
     *
     * SoloHandling is otherwise only filled from PostLoad (ParticleComponents.cpp:2704), so an
     * asset that was created in memory and never loaded from disk has an empty SoloTracking - and
     * UParticleSystem::TurnOffSoloing() indexes it without a bounds check
     * (ParticleComponents.cpp:3121), while ~FCascade calls TurnOffSoloing unconditionally
     * (Cascade.cpp:162). Skipping this step therefore crashes the editor the moment the particle
     * system editor is closed.
     */
    void RefreshSoloing(UParticleSystem* System)
    {
        if (System)
        {
            System->SetupSoloing();
        }
    }

    /** Grow or shrink one emitter's LOD level list, keeping the module validity bits consistent. */
    bool ResizeEmitterLODLevels(UParticleEmitter* Emitter, int32 TargetCount, FString& OutErrorCode,
                                FString& OutErrorMessage)
    {
        if (!Emitter || TargetCount < 1)
        {
            OutErrorCode = EUnrealMCPParticleError::InvalidParams;
            OutErrorMessage = FString::Printf(TEXT("Invalid LOD count %d"), TargetCount);
            return false;
        }

        const int32 CurrentCount = Emitter->LODLevels.Num();
        if (TargetCount > CurrentCount)
        {
            for (int32 LODIndex = CurrentCount; LODIndex < TargetCount; ++LODIndex)
            {
                if (Emitter->CreateLODLevel(LODIndex, /*bGenerateModuleData=*/true) < 0)
                {
                    OutErrorCode = EUnrealMCPParticleError::WriteFailed;
                    OutErrorMessage = FString::Printf(TEXT("Failed to create LOD %d on emitter '%s'"),
                        LODIndex, *Emitter->GetName());
                    return false;
                }
            }
            return true;
        }

        // Drop the tail levels, then clear the validity bits those levels used to set.
        Emitter->LODLevels.SetNum(TargetCount);
        const uint8 ValidMask = static_cast<uint8>((1 << TargetCount) - 1);
        for (UParticleLODLevel* LODLevel : Emitter->LODLevels)
        {
            if (!LODLevel)
            {
                continue;
            }
            for (UParticleModule* Module : LODLevel->Modules)
            {
                if (Module)
                {
                    Module->LODValidity &= ValidMask;
                }
            }
            if (LODLevel->RequiredModule)
            {
                LODLevel->RequiredModule->LODValidity &= ValidMask;
            }
            if (LODLevel->TypeDataModule)
            {
                LODLevel->TypeDataModule->LODValidity &= ValidMask;
            }
            if (LODLevel->SpawnModule)
            {
                LODLevel->SpawnModule->LODValidity &= ValidMask;
            }
            if (LODLevel->EventGenerator)
            {
                LODLevel->EventGenerator->LODValidity &= ValidMask;
            }
        }
        return true;
    }

    /** The LOD count the whole system has to share: the first emitter's. 0 when there is none. */
    int32 SharedLODCount(UParticleSystem* System)
    {
        if (!System)
        {
            return 0;
        }
        for (UParticleEmitter* Emitter : System->Emitters)
        {
            if (Emitter)
            {
                return Emitter->LODLevels.Num();
            }
        }
        return 0;
    }

    /**
     * Bring every emitter to the shared LOD count.
     *
     * UParticleSystem::PostLoad enforces this invariant by silently fixing mismatches up when the
     * editor loads the asset ("Due to there still being some ways that LODLevel counts get
     * mismatched, when loading in the editor LOD levels will always be checked and fixed up...",
     * ParticleComponents.cpp:2607-2649). Keeping it here means the asset on disk is already the
     * asset the engine would load, and no "mismatched LOD level count" warning is produced.
     */
    bool EqualizeLODCounts(UParticleSystem* System, int32 TargetCount, TArray<int32>& OutAdjustedEmitters,
                           FString& OutErrorCode, FString& OutErrorMessage)
    {
        OutAdjustedEmitters.Reset();

        if (TargetCount < 1)
        {
            return true;
        }

        for (int32 EmitterIndex = 0; EmitterIndex < System->Emitters.Num(); ++EmitterIndex)
        {
            UParticleEmitter* Emitter = System->Emitters[EmitterIndex];
            if (!Emitter || Emitter->LODLevels.Num() == TargetCount)
            {
                continue;
            }

            if (!ResizeEmitterLODLevels(Emitter, TargetCount, OutErrorCode, OutErrorMessage))
            {
                return false;
            }
            OutAdjustedEmitters.Add(EmitterIndex);
        }
        return true;
    }

    bool IsAssetEditorOpen(const UObject* Asset)
    {
        if (!GEditor || !Asset)
        {
            return false;
        }

        UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
        return AssetEditorSubsystem && AssetEditorSubsystem->FindEditorForAsset(const_cast<UObject*>(Asset),
            /*bFocusIfOpen=*/false) != nullptr;
    }

    /**
     * Structural edits while the Cascade editor holds the asset leave that editor with a stale
     * view of the emitter and module arrays, so writes are refused until it is closed. Closing it
     * is safe for every asset this kernel created or edited, because RefreshSoloing keeps the
     * SoloHandling invariant that the engine's own editor maintains.
     */
    bool RejectIfEditorOpen(UParticleSystem* System, FUnrealMCPParticleOpResult& OutResult)
    {
        if (!IsAssetEditorOpen(System))
        {
            return false;
        }

        FUnrealMCPParticleOps::Fail(OutResult, EUnrealMCPParticleError::ParticleEditorOpen,
            FString::Printf(TEXT("'%s' is open in the particle system editor; changes made behind the "
                                 "editor's back leave it with a stale view of the emitter and module "
                                 "arrays. Close that editor (close_asset_editors), or pass "
                                 "auto_close=true to let this command close it first"), *System->GetName()),
            TArray<FString>{ TEXT("close_asset_editors"), TEXT("auto_close") });
        return true;
    }
}

// ---------------------------------------------------------------------------
// Writes
// ---------------------------------------------------------------------------

bool FUnrealMCPParticleOps::CreateParticleSystem(const FString& Name, const FString& Folder,
                                                 const FString& EmitterClassName, int32 LODCount,
                                                 FUnrealMCPParticleOpResult& OutResult)
{
    OutResult = FUnrealMCPParticleOpResult();

    if (Name.IsEmpty())
    {
        Fail(OutResult, EUnrealMCPParticleError::InvalidParams, TEXT("Missing 'name' parameter"));
        return false;
    }

    FString PackagePath = Folder.IsEmpty() ? FString(TEXT("/Game/Particles")) : Folder;
    PackagePath.RemoveFromEnd(TEXT("/"));
    if (!PackagePath.StartsWith(TEXT("/")))
    {
        PackagePath = TEXT("/") + PackagePath;
    }
    if (!FPackageName::IsValidLongPackageName(PackagePath))
    {
        Fail(OutResult, EUnrealMCPParticleError::InvalidParams,
            FString::Printf(TEXT("Invalid folder '%s'"), *Folder));
        return false;
    }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
    // FindAsset, not LoadObject: a load of a path whose file is gone leaves a failed-load marker in
    // the engine's loader (and recreating the same path afterwards trips its check). A deleted but
    // still resident object is treated as a leftover to reclaim, not as an existing asset.
    if (UObject* Existing = FUnrealMCPCommonUtils::FindAsset(FullPath))
    {
        const bool bLeftover = !IsValid(Existing) || Existing->IsUnreachable();
        if (bLeftover)
        {
            Existing->MarkAsGarbage();
            CollectGarbage(RF_NoFlags);
        }
        else
        {
            TArray<FString> Candidates;
            Candidates.Add(Existing->GetClass()->GetName());
            Fail(OutResult, EUnrealMCPParticleError::AssetExists,
                FString::Printf(TEXT("Asset already exists: %s (%s); delete it or pass another name"),
                    *FullPath, *Existing->GetClass()->GetName()), Candidates);
            return false;
        }
    }

    UClass* EmitterClass = nullptr;
    FString ClassErrorCode;
    FString ClassErrorMessage;
    TArray<FString> ClassCandidates;
    if (!ResolveEmitterClass(EmitterClassName.IsEmpty() ? FString(TEXT("Sprite")) : EmitterClassName, EmitterClass,
                             ClassErrorCode, ClassErrorMessage, ClassCandidates))
    {
        Fail(OutResult, ClassErrorCode, ClassErrorMessage, ClassCandidates);
        return false;
    }

    UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), UParticleSystemFactoryNew::StaticClass());
    // Not IAssetTools::CreateAsset: its CanCreateAsset step fully loads the target package, which
    // never returns for a path whose file was deleted in this session.
    UObject* NewAsset = FUnrealMCPCommonUtils::CreateAssetDirect(Name, PackagePath, UParticleSystem::StaticClass(), Factory);

    UParticleSystem* System = Cast<UParticleSystem>(NewAsset);
    if (!System)
    {
        Fail(OutResult, EUnrealMCPParticleError::CreateFailed,
            FString::Printf(TEXT("Failed to create a particle system asset at %s"), *FullPath));
        return false;
    }

    int32 InsertedIndex = -1;
    TArray<int32> AdjustedEmitters;
    FString AddErrorCode;
    FString AddErrorMessage;
    if (!AddEmitterToSystem(System, EmitterClass, /*InsertIndex=*/-1, LODCount, InsertedIndex, AdjustedEmitters,
                            AddErrorCode, AddErrorMessage))
    {
        Fail(OutResult, AddErrorCode, AddErrorMessage);
        return false;
    }

    OutResult.Success = true;
    OutResult.EmitterIndex = InsertedIndex;
    OutResult.AdjustedEmitters = AdjustedEmitters;
    PersistSystem(System);
    FillEmitterReadback(System, OutResult);
    return true;
}

bool FUnrealMCPParticleOps::AddEmitter(const FString& AssetPath, const FString& EmitterClassName, int32 InsertIndex,
                                       int32 LODCount, FUnrealMCPParticleOpResult& OutResult)
{
    OutResult = FUnrealMCPParticleOpResult();

    UParticleSystem* System = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!ResolveParticleSystem(AssetPath, System, ErrorCode, ErrorMessage))
    {
        Fail(OutResult, ErrorCode, ErrorMessage);
        return false;
    }

    if (RejectIfEditorOpen(System, OutResult))
    {
        return false;
    }

    UClass* EmitterClass = nullptr;
    TArray<FString> ClassCandidates;
    if (!ResolveEmitterClass(EmitterClassName.IsEmpty() ? FString(TEXT("Sprite")) : EmitterClassName, EmitterClass,
                             ErrorCode, ErrorMessage, ClassCandidates))
    {
        Fail(OutResult, ErrorCode, ErrorMessage, ClassCandidates);
        return false;
    }

    int32 InsertedIndex = -1;
    TArray<int32> AdjustedEmitters;
    if (!AddEmitterToSystem(System, EmitterClass, InsertIndex, LODCount, InsertedIndex, AdjustedEmitters,
                            ErrorCode, ErrorMessage))
    {
        Fail(OutResult, ErrorCode, ErrorMessage);
        return false;
    }

    OutResult.Success = true;
    OutResult.EmitterIndex = InsertedIndex;
    OutResult.AdjustedEmitters = AdjustedEmitters;
    PersistSystem(System);
    FillEmitterReadback(System, OutResult);
    return true;
}

bool FUnrealMCPParticleOps::RemoveEmitter(const FString& AssetPath, int32 EmitterIndex,
                                          FUnrealMCPParticleOpResult& OutResult)
{
    OutResult = FUnrealMCPParticleOpResult();

    UParticleSystem* System = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!ResolveParticleSystem(AssetPath, System, ErrorCode, ErrorMessage))
    {
        Fail(OutResult, ErrorCode, ErrorMessage);
        return false;
    }

    if (RejectIfEditorOpen(System, OutResult))
    {
        return false;
    }

    TArray<FString> Candidates;
    UParticleEmitter* Emitter = nullptr;
    if (!ResolveEmitter(System, EmitterIndex, Emitter, ErrorCode, ErrorMessage, Candidates))
    {
        Fail(OutResult, ErrorCode, ErrorMessage, Candidates);
        return false;
    }

    System->PreEditChange(nullptr);
    System->Emitters.RemoveSingle(Emitter);
    System->UpdateAllModuleLists();
    RefreshSoloing(System);
    System->PostEditChange();
    System->MarkPackageDirty();

    OutResult.Success = true;
    OutResult.EmitterIndex = EmitterIndex;
    PersistSystem(System);
    FillEmitterReadback(System, OutResult);
    return true;
}

bool FUnrealMCPParticleOps::SetLODCount(const FString& AssetPath, int32 EmitterIndex, int32 LODCount,
                                        FUnrealMCPParticleOpResult& OutResult)
{
    OutResult = FUnrealMCPParticleOpResult();

    if (LODCount < 1)
    {
        Fail(OutResult, EUnrealMCPParticleError::InvalidParams,
            FString::Printf(TEXT("'lod_count' must be >= 1, got %d"), LODCount));
        return false;
    }

    UParticleSystem* System = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!ResolveParticleSystem(AssetPath, System, ErrorCode, ErrorMessage))
    {
        Fail(OutResult, ErrorCode, ErrorMessage);
        return false;
    }

    if (RejectIfEditorOpen(System, OutResult))
    {
        return false;
    }

    TArray<FString> Candidates;
    UParticleEmitter* Emitter = nullptr;
    if (!ResolveEmitter(System, EmitterIndex, Emitter, ErrorCode, ErrorMessage, Candidates))
    {
        Fail(OutResult, ErrorCode, ErrorMessage, Candidates);
        return false;
    }

    const int32 CurrentCount = Emitter->LODLevels.Num();
    if (CurrentCount == LODCount)
    {
        OutResult.Success = true;
        OutResult.EmitterIndex = EmitterIndex;
        OutResult.LODIndex = LODCount - 1;
        FillEmitterReadback(System, OutResult);
        return true;
    }

    System->PreEditChange(nullptr);

    // The requested emitter defines the target, then everyone else is brought along: the whole
    // system shares one LOD count.
    if (!ResizeEmitterLODLevels(Emitter, LODCount, ErrorCode, ErrorMessage))
    {
        Fail(OutResult, ErrorCode, ErrorMessage);
        return false;
    }

    TArray<int32> AdjustedEmitters;
    if (!EqualizeLODCounts(System, LODCount, AdjustedEmitters, ErrorCode, ErrorMessage))
    {
        Fail(OutResult, ErrorCode, ErrorMessage);
        return false;
    }

    System->UpdateAllModuleLists();
    RefreshSoloing(System);
    System->PostEditChange();
    System->MarkPackageDirty();

    OutResult.Success = true;
    OutResult.EmitterIndex = EmitterIndex;
    OutResult.LODIndex = LODCount - 1;
    OutResult.AdjustedEmitters = AdjustedEmitters;
    PersistSystem(System);
    FillEmitterReadback(System, OutResult);
    return true;
}

bool FUnrealMCPParticleOps::CopyLOD(const FString& AssetPath, int32 EmitterIndex, int32 SourceLODIndex,
                                    int32 InsertIndex, FUnrealMCPParticleOpResult& OutResult)
{
    OutResult = FUnrealMCPParticleOpResult();

    UParticleSystem* System = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!ResolveParticleSystem(AssetPath, System, ErrorCode, ErrorMessage))
    {
        Fail(OutResult, ErrorCode, ErrorMessage);
        return false;
    }

    if (RejectIfEditorOpen(System, OutResult))
    {
        return false;
    }

    TArray<FString> Candidates;
    UParticleEmitter* Emitter = nullptr;
    if (!ResolveEmitter(System, EmitterIndex, Emitter, ErrorCode, ErrorMessage, Candidates))
    {
        Fail(OutResult, ErrorCode, ErrorMessage, Candidates);
        return false;
    }

    UParticleLODLevel* SourceLODLevel = nullptr;
    if (!ResolveLODLevel(Emitter, SourceLODIndex, SourceLODLevel, ErrorCode, ErrorMessage, Candidates))
    {
        Fail(OutResult, ErrorCode, ErrorMessage, Candidates);
        return false;
    }

    const int32 InsertAt = (InsertIndex < 0) ? (SourceLODIndex + 1) : InsertIndex;
    if (InsertAt < 0 || InsertAt > Emitter->LODLevels.Num())
    {
        TArray<FString> ValidIndices;
        for (int32 Index = 0; Index <= Emitter->LODLevels.Num(); ++Index)
        {
            ValidIndices.Add(FString::FromInt(Index));
        }
        Fail(OutResult, EUnrealMCPParticleError::InvalidParams,
            FString::Printf(TEXT("'insert_index' %d is out of range (0..%d)"), InsertAt, Emitter->LODLevels.Num()),
            ValidIndices);
        return false;
    }

    System->PreEditChange(nullptr);

    // Every emitter keeps the shared LOD count, so the new level is inserted into all of them -
    // each generated from its own source level.
    TArray<UParticleLODLevel*> SourceLevels;
    SourceLevels.Reserve(System->Emitters.Num());
    for (UParticleEmitter* OtherEmitter : System->Emitters)
    {
        SourceLevels.Add((OtherEmitter && OtherEmitter->LODLevels.IsValidIndex(SourceLODIndex))
            ? OtherEmitter->LODLevels[SourceLODIndex] : nullptr);
    }

    TArray<int32> AdjustedEmitters;
    for (int32 OtherIndex = 0; OtherIndex < System->Emitters.Num(); ++OtherIndex)
    {
        UParticleEmitter* OtherEmitter = System->Emitters[OtherIndex];
        if (!OtherEmitter)
        {
            continue;
        }

        const int32 OtherCreatedIndex = OtherEmitter->CreateLODLevel(InsertAt, /*bGenerateModuleData=*/true);
        if (OtherCreatedIndex < 0 || !OtherEmitter->LODLevels.IsValidIndex(OtherCreatedIndex))
        {
            Fail(OutResult, EUnrealMCPParticleError::WriteFailed,
                FString::Printf(TEXT("Failed to create LOD %d on emitter %d"), InsertAt, OtherIndex));
            return false;
        }

        // CreateLODLevel copies from the level above the insertion point; regenerate from the
        // requested source so "copy LOD n" always means LOD n.
        UParticleLODLevel* NewLevel = OtherEmitter->LODLevels[OtherCreatedIndex];
        UParticleLODLevel* SourceLevel = SourceLevels.IsValidIndex(OtherIndex) ? SourceLevels[OtherIndex] : nullptr;
        if (NewLevel && SourceLevel && NewLevel != SourceLevel)
        {
            NewLevel->GenerateFromLODLevel(SourceLevel, 100.0f, /*bGenerateModuleData=*/true);
        }

        if (OtherIndex != EmitterIndex)
        {
            AdjustedEmitters.Add(OtherIndex);
        }
    }

    System->UpdateAllModuleLists();
    RefreshSoloing(System);
    System->PostEditChange();
    System->MarkPackageDirty();

    OutResult.Success = true;
    OutResult.EmitterIndex = EmitterIndex;
    OutResult.LODIndex = InsertAt;
    OutResult.AdjustedEmitters = AdjustedEmitters;
    PersistSystem(System);
    FillEmitterReadback(System, OutResult);
    return true;
}

bool FUnrealMCPParticleOps::SetLODDistance(const FString& AssetPath, int32 EmitterIndex, int32 LODIndex,
                                           float Distance, FUnrealMCPParticleOpResult& OutResult)
{
    OutResult = FUnrealMCPParticleOpResult();

    UParticleSystem* System = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!ResolveParticleSystem(AssetPath, System, ErrorCode, ErrorMessage))
    {
        Fail(OutResult, ErrorCode, ErrorMessage);
        return false;
    }

    if (RejectIfEditorOpen(System, OutResult))
    {
        return false;
    }

    TArray<FString> Candidates;
    UParticleEmitter* Emitter = nullptr;
    if (!ResolveEmitter(System, EmitterIndex, Emitter, ErrorCode, ErrorMessage, Candidates))
    {
        Fail(OutResult, ErrorCode, ErrorMessage, Candidates);
        return false;
    }

    TArray<FString> ValidIndices;
    for (int32 Index = 0; Index <= System->LODDistances.Num(); ++Index)
    {
        ValidIndices.Add(FString::FromInt(Index));
    }

    if (System->LODDistances.IsValidIndex(LODIndex))
    {
        OutResult.ValueBefore = FString::Printf(TEXT("%g"), System->LODDistances[LODIndex]);
        System->LODDistances[LODIndex] = Distance;
    }
    else if (LODIndex == System->LODDistances.Num())
    {
        System->LODDistances.Add(Distance);
    }
    else
    {
        Fail(OutResult, EUnrealMCPParticleError::LODIndexOutOfRange,
            FString::Printf(TEXT("LOD distance index %d is out of range (%d entries; valid: %s)"),
                LODIndex, System->LODDistances.Num(), *FString::Join(ValidIndices, TEXT(", "))),
            ValidIndices);
        return false;
    }

    System->PostEditChange();
    System->MarkPackageDirty();

    OutResult.Success = true;
    OutResult.EmitterIndex = EmitterIndex;
    OutResult.LODIndex = LODIndex;
    OutResult.PropertyName = TEXT("LODDistances");
    OutResult.PropertyType = TEXT("float");
    OutResult.ValueAfter = FString::Printf(TEXT("%g"), System->LODDistances.IsValidIndex(LODIndex) ? System->LODDistances[LODIndex] : Distance);
    PersistSystem(System);
    FillEmitterReadback(System, OutResult);
    return true;
}

bool FUnrealMCPParticleOps::AddModule(const FString& AssetPath, int32 EmitterIndex, const FString& ModuleClassName,
                                      int32 LODIndex, int32 InsertIndex, const FString& Slot, bool bHideSprite,
                                      FUnrealMCPParticleOpResult& OutResult)
{
    OutResult = FUnrealMCPParticleOpResult();

    UParticleSystem* System = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!ResolveParticleSystem(AssetPath, System, ErrorCode, ErrorMessage))
    {
        Fail(OutResult, ErrorCode, ErrorMessage);
        return false;
    }

    if (RejectIfEditorOpen(System, OutResult))
    {
        return false;
    }

    TArray<FString> Candidates;
    UParticleEmitter* Emitter = nullptr;
    if (!ResolveEmitter(System, EmitterIndex, Emitter, ErrorCode, ErrorMessage, Candidates))
    {
        Fail(OutResult, ErrorCode, ErrorMessage, Candidates);
        return false;
    }

    UParticleLODLevel* TargetLODLevel = nullptr;
    if (!ResolveLODLevel(Emitter, LODIndex, TargetLODLevel, ErrorCode, ErrorMessage, Candidates))
    {
        Fail(OutResult, ErrorCode, ErrorMessage, Candidates);
        return false;
    }

    const FString TargetSlot = Slot.IsEmpty() ? FString(EUnrealMCPParticleSlot::Modules) : Slot;
    if (!IsKnownSlot(TargetSlot))
    {
        Fail(OutResult, EUnrealMCPParticleError::UnsupportedModuleSlot,
            FString::Printf(TEXT("Unknown module slot '%s'"), *TargetSlot), SupportedModuleSlots());
        return false;
    }
    if (TargetSlot == EUnrealMCPParticleSlot::Required)
    {
        Fail(OutResult, EUnrealMCPParticleError::UnsupportedModuleSlot,
            TEXT("The required module is created with the emitter and cannot be added"),
            SupportedModuleSlots());
        return false;
    }

    UClass* ModuleClass = nullptr;
    if (!ResolveModuleClass(ModuleClassName, ModuleClass, ErrorCode, ErrorMessage, Candidates))
    {
        Fail(OutResult, ErrorCode, ErrorMessage, Candidates);
        return false;
    }

    if (TargetSlot == EUnrealMCPParticleSlot::TypeData &&
        !ModuleClass->IsChildOf(UParticleModuleTypeDataBase::StaticClass()))
    {
        Fail(OutResult, EUnrealMCPParticleError::UnsupportedModuleSlot,
            FString::Printf(TEXT("%s is not a UParticleModuleTypeDataBase subclass"), *ModuleClass->GetName()),
            TArray<FString>{ TEXT("ParticleModuleTypeDataMesh"), TEXT("ParticleModuleTypeDataBeam2"),
                             TEXT("ParticleModuleTypeDataAnimTrail"), TEXT("ParticleModuleTypeDataRibbon") });
        return false;
    }
    if (TargetSlot == EUnrealMCPParticleSlot::Spawn &&
        !ModuleClass->IsChildOf(UParticleModuleSpawn::StaticClass()))
    {
        Fail(OutResult, EUnrealMCPParticleError::UnsupportedModuleSlot,
            FString::Printf(TEXT("%s is not a UParticleModuleSpawn subclass"), *ModuleClass->GetName()),
            TArray<FString>{ TEXT("ParticleModuleSpawn"), TEXT("ParticleModuleSpawnPerUnit") });
        return false;
    }
    if (TargetSlot == EUnrealMCPParticleSlot::EventGenerator &&
        !ModuleClass->IsChildOf(UParticleModuleEventGenerator::StaticClass()))
    {
        Fail(OutResult, EUnrealMCPParticleError::UnsupportedModuleSlot,
            FString::Printf(TEXT("%s is not a UParticleModuleEventGenerator subclass"), *ModuleClass->GetName()),
            TArray<FString>{ TEXT("ParticleModuleEventGenerator") });
        return false;
    }

    System->PreEditChange(nullptr);

    UParticleModule* NewModule = NewObject<UParticleModule>(System, ModuleClass, NAME_None, RF_Transactional);
    if (!NewModule)
    {
        Fail(OutResult, EUnrealMCPParticleError::WriteFailed,
            FString::Printf(TEXT("Failed to create a module of class %s"), *ModuleClass->GetName()));
        return false;
    }

    NewModule->SetToSensibleDefaults(Emitter);
    NewModule->SetTransactionFlag();
#if WITH_EDITORONLY_DATA
    NewModule->ModuleEditorColor = FColor::MakeRandomColor();
#endif
    NewModule->LODValidity = 0;

    int32 InsertedModuleIndex = -1;
    for (int32 LevelIndex = 0; LevelIndex < Emitter->LODLevels.Num(); ++LevelIndex)
    {
        UParticleLODLevel* LODLevel = Emitter->LODLevels[LevelIndex];
        if (!LODLevel)
        {
            continue;
        }

        if (TargetSlot == EUnrealMCPParticleSlot::Modules)
        {
            const int32 InsertAt = (InsertIndex < 0)
                ? LODLevel->Modules.Num()
                : FMath::Clamp(InsertIndex, 0, LODLevel->Modules.Num());
            LODLevel->Modules.Insert(NewModule, InsertAt);
            if (LODLevel == TargetLODLevel)
            {
                InsertedModuleIndex = InsertAt;
            }
        }
        else if (TargetSlot == EUnrealMCPParticleSlot::EventGenerator)
        {
            LODLevel->Modules.Insert(NewModule, 0);
            LODLevel->EventGenerator = Cast<UParticleModuleEventGenerator>(NewModule);
        }
        else if (TargetSlot == EUnrealMCPParticleSlot::Spawn)
        {
            LODLevel->SpawnModule = Cast<UParticleModuleSpawn>(NewModule);
        }
        else if (TargetSlot == EUnrealMCPParticleSlot::TypeData)
        {
            LODLevel->TypeDataModule = Cast<UParticleModuleTypeDataBase>(NewModule);
        }

        NewModule->LODValidity |= static_cast<uint8>(1 << LevelIndex);
    }

    System->UpdateAllModuleLists();
    RefreshSoloing(System);
    System->PostEditChange();
    System->MarkPackageDirty();

    if (bHideSprite && TargetLODLevel && TargetLODLevel->RequiredModule)
    {
        // A light emitter draws nothing of its own: with bUseMaxDrawCount set and MaxDrawCount at 0
        // the engine skips the sprite's dynamic data entirely (ParticleEmitterInstances.cpp), which
        // is the two-step knowledge that used to be passed on by word of mouth.
        UParticleModuleRequired* Required = TargetLODLevel->RequiredModule;
        Required->bUseMaxDrawCount = true;
        Required->MaxDrawCount = 0;
        Required->PostEditChange();

        OutResult.SpriteHidden = true;
        OutResult.bUseMaxDrawCount = Required->bUseMaxDrawCount;
        OutResult.MaxDrawCount = Required->MaxDrawCount;
    }

    OutResult.Success = true;
    OutResult.EmitterIndex = EmitterIndex;
    OutResult.LODIndex = LODIndex;
    OutResult.ModuleClass = ModuleClass->GetName();
    OutResult.ModuleIndex = InsertedModuleIndex;
    PersistSystem(System);
    FillEmitterReadback(System, OutResult);
    return true;
}

bool FUnrealMCPParticleOps::RemoveModule(const FString& AssetPath, int32 EmitterIndex, const FString& ModuleClassName,
                                         int32 ModuleIndex, int32 LODIndex, FUnrealMCPParticleOpResult& OutResult)
{
    OutResult = FUnrealMCPParticleOpResult();

    UParticleSystem* System = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!ResolveParticleSystem(AssetPath, System, ErrorCode, ErrorMessage))
    {
        Fail(OutResult, ErrorCode, ErrorMessage);
        return false;
    }

    if (RejectIfEditorOpen(System, OutResult))
    {
        return false;
    }

    TArray<FString> Candidates;
    UParticleEmitter* Emitter = nullptr;
    if (!ResolveEmitter(System, EmitterIndex, Emitter, ErrorCode, ErrorMessage, Candidates))
    {
        Fail(OutResult, ErrorCode, ErrorMessage, Candidates);
        return false;
    }

    UParticleModule* Module = nullptr;
    int32 ResolvedModuleIndex = -1;
    // No slot on purpose: a class lookup also finds the single-module slots (TypeData / Spawn /
    // event generator / required), which are not part of the module list.
    if (!ResolveModule(Emitter, LODIndex, ModuleClassName, ModuleIndex, FString(), Module, ResolvedModuleIndex,
                       ErrorCode, ErrorMessage, Candidates))
    {
        Fail(OutResult, ErrorCode, ErrorMessage, Candidates);
        return false;
    }

    // Two slots are off limits. The required module is what makes an emitter work, and the
    // spawn module is one the engine insists on: UParticleSystem::PostEditChange asserts on a
    // missing SpawnModule for every enabled LOD whose required module loops forever
    // (ParticleComponents.cpp:2376), and the Cascade editor never clears that slot either
    // (Cascade.cpp:4631-4661 clears only TypeData and the event generator). Refuse before
    // touching anything, so a rejected removal cannot leave the emitter half-dismantled.
    for (UParticleLODLevel* LODLevel : Emitter->LODLevels)
    {
        if (!LODLevel)
        {
            continue;
        }
        if (LODLevel->RequiredModule == Module)
        {
            Fail(OutResult, EUnrealMCPParticleError::UnsupportedModuleSlot,
                TEXT("The required module cannot be removed"), SupportedModuleSlots());
            return false;
        }
        if (LODLevel->SpawnModule == Module)
        {
            Fail(OutResult, EUnrealMCPParticleError::UnsupportedModuleSlot,
                TEXT("The spawn module cannot be removed (the engine requires a spawn module on every "
                     "enabled LOD level); write its Rate distribution with set_particle_distribution instead"),
                SupportedModuleSlots());
            return false;
        }
    }

    System->PreEditChange(nullptr);

    TArray<FString> ClearedSlots;
    for (UParticleLODLevel* LODLevel : Emitter->LODLevels)
    {
        if (!LODLevel)
        {
            continue;
        }

        CollectModuleSlots(LODLevel, Module, ClearedSlots);

        if (LODLevel->TypeDataModule == Module)
        {
            LODLevel->TypeDataModule = nullptr;
        }
        if (LODLevel->EventGenerator == Module)
        {
            LODLevel->EventGenerator = nullptr;
        }

        LODLevel->Modules.Remove(Module);
    }

    System->UpdateAllModuleLists();
    RefreshSoloing(System);
    System->PostEditChange();
    System->MarkPackageDirty();

    OutResult.Success = true;
    OutResult.EmitterIndex = EmitterIndex;
    OutResult.LODIndex = LODIndex;
    OutResult.ModuleClass = ClassNameOf(Module);
    OutResult.ModuleIndex = ResolvedModuleIndex;
    OutResult.ClearedSlots = ClearedSlots;
    PersistSystem(System);
    FillEmitterReadback(System, OutResult);
    return true;
}

bool FUnrealMCPParticleOps::SetModuleProperty(const FString& AssetPath, int32 EmitterIndex,
                                              const FString& ModuleClassName, int32 ModuleIndex, int32 LODIndex,
                                              const FString& PropertyName, const TSharedPtr<FJsonValue>& Value,
                                              FUnrealMCPParticleOpResult& OutResult)
{
    OutResult = FUnrealMCPParticleOpResult();

    if (PropertyName.IsEmpty())
    {
        Fail(OutResult, EUnrealMCPParticleError::InvalidParams, TEXT("Missing 'property_name' parameter"));
        return false;
    }
    if (!Value.IsValid())
    {
        Fail(OutResult, EUnrealMCPParticleError::InvalidParams, TEXT("Missing 'property_value' parameter"));
        return false;
    }

    UParticleSystem* System = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!ResolveParticleSystem(AssetPath, System, ErrorCode, ErrorMessage))
    {
        Fail(OutResult, ErrorCode, ErrorMessage);
        return false;
    }

    if (RejectIfEditorOpen(System, OutResult))
    {
        return false;
    }

    TArray<FString> Candidates;
    UParticleEmitter* Emitter = nullptr;
    if (!ResolveEmitter(System, EmitterIndex, Emitter, ErrorCode, ErrorMessage, Candidates))
    {
        Fail(OutResult, ErrorCode, ErrorMessage, Candidates);
        return false;
    }

    UParticleModule* Module = nullptr;
    int32 ResolvedModuleIndex = -1;
    // Empty slot: single-module slots (required / type data / spawn / event generator) are
    // addressable by class too, since they carry properties worth writing.
    if (!ResolveModule(Emitter, LODIndex, ModuleClassName, ModuleIndex, FString(),
                       Module, ResolvedModuleIndex, ErrorCode, ErrorMessage, Candidates))
    {
        Fail(OutResult, ErrorCode, ErrorMessage, Candidates);
        return false;
    }

    FProperty* Property = Module->GetClass()->FindPropertyByName(*PropertyName);
    FString ResolvedFrom;
    if (!Property)
    {
        // Cascade names several properties differently from the way they are talked about
        // ("RotationRate" vs "RotationRateAmount"); the alias table keeps that off the caller.
        FString AliasTarget;
        if (ResolveParticlePropertyAlias(PropertyName, AliasTarget))
        {
            if (FProperty* AliasProperty = Module->GetClass()->FindPropertyByName(*AliasTarget))
            {
                Property = AliasProperty;
                ResolvedFrom = PropertyName;
            }
        }
    }

    TArray<FString> Suggestions;
    CollectPropertyCandidates(Module, ResolvedFrom.IsEmpty() ? PropertyName : Property->GetName(), Suggestions);

    if (!Property)
    {
        const FString Message = Suggestions.Num() > 0
            ? FString::Printf(TEXT("Property not found: %s. Did you mean: %s"), *PropertyName,
                *FString::Join(Suggestions, TEXT(", ")))
            : FString::Printf(TEXT("Property not found: %s"), *PropertyName);
        Fail(OutResult, EUnrealMCPParticleError::UnknownProperty, Message, Suggestions);
        return false;
    }

    // Reported even when the write is refused below, so an alias always resolves to a name the
    // caller can use (a distribution property has its own tool).
    OutResult.EmitterIndex = EmitterIndex;
    OutResult.LODIndex = LODIndex;
    OutResult.ModuleClass = ClassNameOf(Module);
    OutResult.ModuleIndex = ResolvedModuleIndex;
    OutResult.PropertyName = ResolvedFrom.IsEmpty() ? PropertyName : Property->GetName();
    OutResult.ResolvedFrom = ResolvedFrom;
    OutResult.PropertyType = Property->GetCPPType();
    FindParticlePropertyHint(Property->GetName(), OutResult.Units, OutResult.Hint);

    // A raw distribution is refused by the registered property codec (see GParticlePropertyCodecRegistrar),
    // which produces the same unsupported_module_property code and hint for every write entry point.

    OutResult.ValueBefore = PropertyValueToResponseText(Property, Property->ContainerPtrToValuePtr<void>(Module));

    FString WriteError;
    FString WriteErrorCode;
    TArray<FString> AvailableFields;
    FWriteResult WriteResult;
    if (!FUnrealMCPCommonUtils::SetObjectProperty(Module, Property->GetName(), Value, WriteError,
            &AvailableFields, &WriteErrorCode, &WriteResult))
    {
        // The code comes from the reflector, which classifies its own failures.
        Fail(OutResult, WriteErrorCode.IsEmpty() ? FString(EUnrealMCPParticleError::WriteFailed) : WriteErrorCode,
            WriteError, Suggestions, AvailableFields);
        OutResult.FailedIndex = WriteResult.FailedIndex;
        OutResult.bUnchanged = WriteResult.bUnchanged;
        return false;
    }

    Module->PostEditChange();
    System->UpdateAllModuleLists();
    RefreshSoloing(System);
    System->PostEditChange();
    System->MarkPackageDirty();

    OutResult.Success = true;
    OutResult.ValueAfter = PropertyValueToResponseText(Property, Property->ContainerPtrToValuePtr<void>(Module));
    PersistSystem(System);
    FillEmitterReadback(System, OutResult);
    return true;
}

bool FUnrealMCPParticleOps::SetParticleBursts(const FString& AssetPath, int32 EmitterIndex, int32 LODIndex,
                                              const TArray<FUnrealMCPParticleBurst>& Bursts,
                                              FUnrealMCPParticleOpResult& OutResult)
{
    OutResult = FUnrealMCPParticleOpResult();

    UParticleSystem* System = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!ResolveParticleSystem(AssetPath, System, ErrorCode, ErrorMessage))
    {
        Fail(OutResult, ErrorCode, ErrorMessage);
        return false;
    }

    if (RejectIfEditorOpen(System, OutResult))
    {
        return false;
    }

    TArray<FString> Candidates;
    UParticleEmitter* Emitter = nullptr;
    if (!ResolveEmitter(System, EmitterIndex, Emitter, ErrorCode, ErrorMessage, Candidates))
    {
        Fail(OutResult, ErrorCode, ErrorMessage, Candidates);
        return false;
    }

    // Bursts live on the spawn module, which is a single-module slot rather than a list entry.
    UParticleModule* SpawnModule = nullptr;
    int32 SpawnModuleIndex = -1;
    if (!ResolveModule(Emitter, LODIndex, FString(), -1, FString(EUnrealMCPParticleSlot::Spawn),
                       SpawnModule, SpawnModuleIndex, ErrorCode, ErrorMessage, Candidates))
    {
        Fail(OutResult, ErrorCode, ErrorMessage, Candidates);
        return false;
    }

    FProperty* BurstListProperty = SpawnModule->GetClass()->FindPropertyByName(TEXT("BurstList"));
    if (!BurstListProperty)
    {
        Fail(OutResult, EUnrealMCPParticleError::UnsupportedModuleProperty,
            FString::Printf(TEXT("%s has no BurstList property"), *SpawnModule->GetClass()->GetName()),
            TArray<FString>{ TEXT("BurstList") });
        return false;
    }

    TArray<TSharedPtr<FJsonValue>> BurstValues;
    for (const FUnrealMCPParticleBurst& Burst : Bursts)
    {
        TSharedPtr<FJsonObject> BurstObject = MakeShared<FJsonObject>();
        BurstObject->SetNumberField(TEXT("Count"), Burst.Count);
        BurstObject->SetNumberField(TEXT("CountLow"), Burst.CountLow);
        BurstObject->SetNumberField(TEXT("Time"), Burst.Time);
        BurstValues.Add(MakeShared<FJsonValueObject>(BurstObject));
    }

    void* BurstListAddr = BurstListProperty->ContainerPtrToValuePtr<void>(SpawnModule);

    OutResult.EmitterIndex = EmitterIndex;
    OutResult.LODIndex = LODIndex;
    OutResult.ModuleClass = ClassNameOf(SpawnModule);
    OutResult.ModuleIndex = SpawnModuleIndex;
    OutResult.PropertyName = TEXT("BurstList");
    OutResult.PropertyType = BurstListProperty->GetCPPType();
    OutResult.ValueBefore = PropertyValueToResponseText(BurstListProperty, BurstListAddr);

    FString WriteError;
    FString WriteErrorCode;
    TArray<FString> AvailableFields;
    FWriteResult WriteResult;
    if (!FUnrealMCPCommonUtils::SetObjectProperty(SpawnModule, TEXT("BurstList"),
            MakeShared<FJsonValueArray>(BurstValues), WriteError, &AvailableFields, &WriteErrorCode, &WriteResult))
    {
        Fail(OutResult, WriteErrorCode.IsEmpty() ? FString(EUnrealMCPParticleError::WriteFailed) : WriteErrorCode,
            WriteError, TArray<FString>(), AvailableFields);
        OutResult.FailedIndex = WriteResult.FailedIndex;
        OutResult.bUnchanged = WriteResult.bUnchanged;
        return false;
    }

    SpawnModule->PostEditChange();
    System->UpdateAllModuleLists();
    RefreshSoloing(System);
    System->PostEditChange();
    System->MarkPackageDirty();

    OutResult.Success = true;
    OutResult.BurstCount = Bursts.Num();
    OutResult.ValueAfter = PropertyValueToResponseText(BurstListProperty, BurstListAddr);
    PersistSystem(System);
    FillEmitterReadback(System, OutResult);
    return true;
}

bool FUnrealMCPParticleOps::SetDistribution(const FString& AssetPath, int32 EmitterIndex,
                                            const FString& ModuleClassName, int32 ModuleIndex, int32 LODIndex,
                                            const FString& PropertyName, const FString& Kind,
                                            const TArray<float>& Values,
                                            const TArray<FUnrealMCPParticleDistributionKey>& Keys,
                                            FUnrealMCPParticleOpResult& OutResult)
{
    OutResult = FUnrealMCPParticleOpResult();

    if (PropertyName.IsEmpty())
    {
        Fail(OutResult, EUnrealMCPParticleError::InvalidParams, TEXT("Missing 'property_name' parameter"));
        return false;
    }

    UParticleSystem* System = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!ResolveParticleSystem(AssetPath, System, ErrorCode, ErrorMessage))
    {
        Fail(OutResult, ErrorCode, ErrorMessage);
        return false;
    }

    if (RejectIfEditorOpen(System, OutResult))
    {
        return false;
    }

    TArray<FString> Candidates;
    UParticleEmitter* Emitter = nullptr;
    if (!ResolveEmitter(System, EmitterIndex, Emitter, ErrorCode, ErrorMessage, Candidates))
    {
        Fail(OutResult, ErrorCode, ErrorMessage, Candidates);
        return false;
    }

    UParticleModule* Module = nullptr;
    int32 ResolvedModuleIndex = -1;
    // Empty slot: a distribution can live on a slot module too (TypeData / Spawn / required).
    if (!ResolveModule(Emitter, LODIndex, ModuleClassName, ModuleIndex, FString(),
                       Module, ResolvedModuleIndex, ErrorCode, ErrorMessage, Candidates))
    {
        Fail(OutResult, ErrorCode, ErrorMessage, Candidates);
        return false;
    }

    FProperty* Property = Module->GetClass()->FindPropertyByName(*PropertyName);
    FString ResolvedFrom;
    if (!Property)
    {
        // Same alias table as set_particle_module_property: "RotationRate" is really
        // "RotationRateAmount", and on Orbit that one is a distribution.
        FString AliasTarget;
        if (ResolveParticlePropertyAlias(PropertyName, AliasTarget))
        {
            if (FProperty* AliasProperty = Module->GetClass()->FindPropertyByName(*AliasTarget))
            {
                Property = AliasProperty;
                ResolvedFrom = PropertyName;
            }
        }
    }

    if (!Property)
    {
        TArray<FString> Suggestions;
        CollectPropertyCandidates(Module, PropertyName, Suggestions);
        Fail(OutResult, EUnrealMCPParticleError::UnknownProperty,
            FString::Printf(TEXT("Property not found: %s"), *PropertyName), Suggestions);
        return false;
    }

    bool bVector = false;
    if (!IsRawDistributionProperty(Property, bVector))
    {
        TArray<FString> DistributionProperties;
        for (TFieldIterator<FProperty> It(Module->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
        {
            bool bIsVector = false;
            if (IsRawDistributionProperty(*It, bIsVector))
            {
                DistributionProperties.Add((*It)->GetName());
            }
        }
        Fail(OutResult, EUnrealMCPParticleError::NotADistributionProperty,
            FString::Printf(TEXT("Property '%s' is not a distribution"), *PropertyName), DistributionProperties);
        return false;
    }

    const FString NormalizedKind = Kind.ToLower();
    UClass* WantedClass = DistributionClassFor(bVector, NormalizedKind);
    if (!WantedClass)
    {
        Fail(OutResult, EUnrealMCPParticleError::UnsupportedDistributionKind,
            FString::Printf(TEXT("Unsupported distribution kind '%s'"), *Kind), SupportedDistributionKinds());
        return false;
    }

    OutResult.EmitterIndex = EmitterIndex;
    OutResult.LODIndex = LODIndex;
    OutResult.ModuleClass = ClassNameOf(Module);
    OutResult.ModuleIndex = ResolvedModuleIndex;
    OutResult.PropertyName = ResolvedFrom.IsEmpty() ? PropertyName : Property->GetName();
    OutResult.ResolvedFrom = ResolvedFrom;
    OutResult.PropertyType = Property->GetCPPType();
    FindParticlePropertyHint(Property->GetName(), OutResult.Units, OutResult.Hint);

    FUnrealMCPParticleDistributionInfo BeforeInfo;
    if (MakeDistributionInfo(Module, Property, BeforeInfo))
    {
        OutResult.ValueBefore = DistributionToText(BeforeInfo);
    }

    UDistribution* ExistingDistribution = GetRawDistributionObject(Module, Property, bVector);
    const bool bKindAlreadyMatches = ExistingDistribution && ExistingDistribution->GetClass() == WantedClass;

    UDistribution* TargetDistribution = bKindAlreadyMatches
        ? ExistingDistribution
        : NewObject<UDistribution>(Module, WantedClass, NAME_None, RF_Transactional);

    if (!TargetDistribution)
    {
        Fail(OutResult, EUnrealMCPParticleError::WriteFailed,
            FString::Printf(TEXT("Failed to create a %s distribution"), *WantedClass->GetName()));
        return false;
    }

    FString ApplyError;
    if (!ApplyDistributionValues(TargetDistribution, bVector, NormalizedKind, Values, Keys, PropertyName, ApplyError))
    {
        Fail(OutResult, EUnrealMCPParticleError::InvalidValue, ApplyError, SupportedDistributionKinds());
        return false;
    }

    if (!bKindAlreadyMatches)
    {
        SetRawDistributionObject(Module, Property, bVector, TargetDistribution);
    }

    RebuildRawDistribution(Module, Property, bVector);
    Module->PostEditChange();
    System->UpdateAllModuleLists();
    RefreshSoloing(System);
    System->PostEditChange();
    System->MarkPackageDirty();

    FUnrealMCPParticleDistributionInfo AfterInfo;
    if (MakeDistributionInfo(Module, Property, AfterInfo))
    {
        OutResult.ValueAfter = DistributionToText(AfterInfo);
    }

    OutResult.Success = true;
    PersistSystem(System);
    FillEmitterReadback(System, OutResult);
    return true;
}

// ---------------------------------------------------------------------------
// validate_particle_system
// ---------------------------------------------------------------------------

namespace
{
    FUnrealMCPParticleCheck MakeCheck(const TCHAR* Id, const TCHAR* Severity, const FString& Target,
                                      const FString& Message, const FString& FixHint = FString())
    {
        FUnrealMCPParticleCheck Check;
        Check.Id = Id;
        Check.Severity = Severity;
        Check.Target = Target;
        Check.Message = Message;
        Check.FixHint = FixHint;
        return Check;
    }

    FUnrealMCPParticleCheck PassCheck(const TCHAR* Id, const FString& Target, const FString& Message)
    {
        return MakeCheck(Id, TEXT("info"), Target, Message);
    }

    FString EmitterTarget(int32 EmitterIndex)
    {
        return FString::Printf(TEXT("emitter[%d]"), EmitterIndex);
    }

    FString LODTarget(int32 EmitterIndex, int32 LODIndex, const FString& Suffix = FString())
    {
        return FString::Printf(TEXT("emitter[%d].lod[%d]%s"), EmitterIndex, LODIndex, *Suffix);
    }

    FString MaterialPathOf(const UMaterialInterface* Material)
    {
        return Material ? Material->GetPathName() : FString(TEXT("None"));
    }

    /** The engine's mesh stand-in material: the look of an untextured grey board. */
    bool IsWorldGridMaterial(const UMaterialInterface* Material)
    {
        return Material && Material->GetPathName().Contains(TEXT("/Engine/EngineMaterials/WorldGridMaterial"));
    }

    /** The engine's default particle material: fine for a first render, never a final look. */
    bool IsDefaultParticleMaterial(const UMaterialInterface* Material)
    {
        return Material && Material->GetPathName().Contains(TEXT("/Engine/EngineMaterials/DefaultParticle"));
    }

    bool IsPlaceholderMaterial(const UMaterialInterface* Material)
    {
        return !Material || IsWorldGridMaterial(Material) || IsDefaultParticleMaterial(Material);
    }

    /** Compile errors of the material at one feature level (empty while the compile is pending). */
    void CollectMaterialErrors(const UMaterial* BaseMaterial, TArray<FString>& OutErrors)
    {
        OutErrors.Reset();
        if (!BaseMaterial)
        {
            return;
        }
        if (const FMaterialResource* Resource = BaseMaterial->GetMaterialResource(ERHIFeatureLevel::SM5))
        {
            OutErrors = Resource->GetCompileErrors();
        }
    }

    const UParticleModuleTypeDataMesh* AsMeshTypeData(const UParticleLODLevel* LODLevel)
    {
        return LODLevel ? Cast<UParticleModuleTypeDataMesh>(LODLevel->TypeDataModule) : nullptr;
    }

    bool EmitterHasLightModule(const UParticleEmitter* Emitter)
    {
        if (!Emitter)
        {
            return false;
        }
        for (const UParticleLODLevel* LODLevel : Emitter->LODLevels)
        {
            if (!LODLevel)
            {
                continue;
            }
            for (const UParticleModule* Module : LODLevel->Modules)
            {
                if (Module && Module->IsA(UParticleModuleLight::StaticClass()))
                {
                    return true;
                }
            }
        }
        return false;
    }

    /** True when a vector distribution property is a constant of zero on every axis. */
    bool IsZeroVectorConstant(UParticleModule* Module, FProperty* Property)
    {
        bool bVector = false;
        if (!Module || !Property || !IsRawDistributionProperty(Property, bVector))
        {
            return false;
        }
        const UDistribution* Distribution = GetRawDistributionObject(Module, Property, bVector);
        const UDistributionVectorConstant* Constant = Cast<UDistributionVectorConstant>(Distribution);
        if (!Constant)
        {
            return false;
        }
        const FVector Value = Constant->Constant;
        return Value.IsNearlyZero();
    }

    /** The sprite / mesh material used by an emitter's first LOD (that is the authored one). */
    UMaterialInterface* RequiredMaterialOf(const UParticleEmitter* Emitter)
    {
        const UParticleLODLevel* LODLevel = (Emitter && Emitter->LODLevels.Num() > 0) ? Emitter->LODLevels[0] : nullptr;
        return (LODLevel && LODLevel->RequiredModule) ? LODLevel->RequiredModule->Material : nullptr;
    }
}

TArray<FString> FUnrealMCPParticleOps::SupportedChecks()
{
    return TArray<FString>{
        EUnrealMCPParticleCheck::SpriteMaterial,
        EUnrealMCPParticleCheck::MeshMaterial,
        EUnrealMCPParticleCheck::LightSprite,
        EUnrealMCPParticleCheck::EmitterSpace,
        EUnrealMCPParticleCheck::LODStructure,
        EUnrealMCPParticleCheck::InheritedModules,
        EUnrealMCPParticleCheck::MaterialUsage,
        EUnrealMCPParticleCheck::AdditiveOpacity,
    };
}

bool FUnrealMCPParticleOps::ValidateParticleSystem(const FString& AssetPath, int32 EmitterIndex,
                                                   const TArray<FString>& CheckIds,
                                                   FUnrealMCPParticleListResult& OutResult)
{
    OutResult = FUnrealMCPParticleListResult();

    UParticleSystem* System = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!ResolveParticleSystem(AssetPath, System, ErrorCode, ErrorMessage))
    {
        OutResult.Success = false;
        OutResult.ErrorCode = ErrorCode;
        OutResult.ErrorMessage = ErrorMessage;
        return false;
    }

    const TArray<FString> AllChecks = SupportedChecks();
    for (const FString& RequestedCheck : CheckIds)
    {
        if (!AllChecks.Contains(RequestedCheck))
        {
            OutResult.Success = false;
            OutResult.ErrorCode = EUnrealMCPParticleError::UnknownCheck;
            OutResult.ErrorMessage = FString::Printf(TEXT("Unknown check '%s'"), *RequestedCheck);
            OutResult.Candidates = AllChecks;
            return false;
        }
    }

    auto RunsCheck = [&CheckIds](const TCHAR* Id)
    {
        return CheckIds.Num() == 0 || CheckIds.Contains(FString(Id));
    };

    TArray<int32> EmitterIndices;
    if (EmitterIndex >= 0)
    {
        if (!System->Emitters.IsValidIndex(EmitterIndex))
        {
            TArray<FString> Candidates;
            for (int32 Index = 0; Index < System->Emitters.Num(); ++Index)
            {
                Candidates.Add(FString::FromInt(Index));
            }
            OutResult.Success = false;
            OutResult.ErrorCode = EUnrealMCPParticleError::EmitterIndexOutOfRange;
            OutResult.ErrorMessage = FString::Printf(TEXT("emitter_index %d is out of range"), EmitterIndex);
            OutResult.Candidates = Candidates;
            return false;
        }
        EmitterIndices.Add(EmitterIndex);
    }
    else
    {
        for (int32 Index = 0; Index < System->Emitters.Num(); ++Index)
        {
            EmitterIndices.Add(Index);
        }
    }

    TArray<FUnrealMCPParticleCheck>& Checks = OutResult.Checks;

    // --- per emitter -------------------------------------------------------
    for (const int32 Index : EmitterIndices)
    {
        UParticleEmitter* Emitter = System->Emitters[Index];
        if (!Emitter)
        {
            Checks.Add(MakeCheck(EUnrealMCPParticleCheck::LODStructure, TEXT("error"), EmitterTarget(Index),
                TEXT("Emitter slot is empty"), TEXT("remove_particle_emitter to drop the slot")));
            continue;
        }

        const bool bSprite = Emitter->IsA(UParticleSpriteEmitter::StaticClass());
        UParticleLODLevel* LODLevel0 = Emitter->LODLevels.Num() > 0 ? Emitter->LODLevels[0] : nullptr;
        UParticleModuleRequired* Required = LODLevel0 ? LODLevel0->RequiredModule : nullptr;
        UMaterialInterface* Material = RequiredMaterialOf(Emitter);
        const UParticleModuleTypeDataMesh* MeshTypeData = AsMeshTypeData(LODLevel0);

        if (RunsCheck(EUnrealMCPParticleCheck::SpriteMaterial) && bSprite && !MeshTypeData)
        {
            if (!Material)
            {
                Checks.Add(MakeCheck(EUnrealMCPParticleCheck::SpriteMaterial, TEXT("error"), LODTarget(Index, 0),
                    TEXT("Sprite emitter has no material"), TEXT("set_particle_module_property on Required.Material with a particle material asset path")));
            }
            else if (IsWorldGridMaterial(Material))
            {
                Checks.Add(MakeCheck(EUnrealMCPParticleCheck::SpriteMaterial, TEXT("warning"), LODTarget(Index, 0),
                    FString::Printf(TEXT("Sprite emitter uses the mesh stand-in material %s"), *Material->GetName()),
                    TEXT("set_particle_module_property (slot=required, property_name=Material, property_value=<material path>)")));
            }
            else if (IsDefaultParticleMaterial(Material))
            {
                Checks.Add(MakeCheck(EUnrealMCPParticleCheck::SpriteMaterial, TEXT("info"), LODTarget(Index, 0),
                    FString::Printf(TEXT("Sprite emitter uses the engine default particle material %s"), *Material->GetName()),
                    TEXT("fine for a first render; write Required.Material for the final look")));
            }
            else
            {
                Checks.Add(PassCheck(EUnrealMCPParticleCheck::SpriteMaterial, LODTarget(Index, 0),
                    FString::Printf(TEXT("Sprite material is %s"), *Material->GetName())));
            }
        }

        if (RunsCheck(EUnrealMCPParticleCheck::MeshMaterial) && MeshTypeData)
        {
            const bool bHasMesh = MeshTypeData->Mesh != nullptr;
            if (!bHasMesh)
            {
                Checks.Add(MakeCheck(EUnrealMCPParticleCheck::MeshMaterial, TEXT("error"), LODTarget(Index, 0, TEXT(".TypeDataMesh")),
                    TEXT("Mesh emitter has no static mesh"), TEXT("set_particle_module_property on TypeDataMesh.Mesh")));
            }
            else if (!MeshTypeData->bOverrideMaterial && IsPlaceholderMaterial(Material))
            {
                Checks.Add(MakeCheck(EUnrealMCPParticleCheck::MeshMaterial, TEXT("warning"), LODTarget(Index, 0, TEXT(".TypeDataMesh")),
                    FString::Printf(TEXT("Mesh '%s' with bOverrideMaterial=false and the placeholder material %s"),
                        *MeshTypeData->Mesh->GetName(), *Material->GetName()),
                    TEXT("either set TypeDataMesh.bOverrideMaterial=true, or write Required.Material again after setting Mesh (assigning Mesh overwrites it)")));
            }
            else if (MeshTypeData->bOverrideMaterial && IsPlaceholderMaterial(Material))
            {
                Checks.Add(MakeCheck(EUnrealMCPParticleCheck::MeshMaterial, TEXT("warning"), LODTarget(Index, 0, TEXT(".TypeDataMesh")),
                    TEXT("TypeDataMesh overrides the material but Required.Material is still a placeholder"),
                    TEXT("write Required.Material with the intended particle material")));
            }
            else
            {
                Checks.Add(PassCheck(EUnrealMCPParticleCheck::MeshMaterial, LODTarget(Index, 0, TEXT(".TypeDataMesh")),
                    FString::Printf(TEXT("Mesh '%s', override material %s"), *MeshTypeData->Mesh->GetName(),
                        MeshTypeData->bOverrideMaterial ? TEXT("on") : TEXT("off"))));
            }
        }

        if (RunsCheck(EUnrealMCPParticleCheck::LightSprite) && EmitterHasLightModule(Emitter))
        {
            const bool bHidden = Required && Required->bUseMaxDrawCount && Required->MaxDrawCount == 0;
            if (bHidden)
            {
                Checks.Add(PassCheck(EUnrealMCPParticleCheck::LightSprite, EmitterTarget(Index),
                    TEXT("Light emitter draws no sprite (bUseMaxDrawCount=true, MaxDrawCount=0)")));
            }
            else
            {
                Checks.Add(MakeCheck(EUnrealMCPParticleCheck::LightSprite, TEXT("warning"), EmitterTarget(Index),
                    TEXT("Light emitter will also draw its sprite (a lit quad), which reads as a floating square"),
                    TEXT("add_particle_module(module_class=\"Light\", hide_sprite=true) or set Required.bUseMaxDrawCount=true and Required.MaxDrawCount=0")));
            }
        }

        if (RunsCheck(EUnrealMCPParticleCheck::InheritedModules))
        {
            for (const UParticleLODLevel* LODLevel : Emitter->LODLevels)
            {
                if (!LODLevel)
                {
                    continue;
                }
                for (const UParticleModule* Module : LODLevel->Modules)
                {
                    if (!Module)
                    {
                        continue;
                    }
                    const FString ModuleName = Module->GetClass()->GetName();
                    if (Module->IsA(UParticleModuleTypeDataBase::StaticClass()))
                    {
                        Checks.Add(MakeCheck(EUnrealMCPParticleCheck::InheritedModules, TEXT("warning"),
                            FString::Printf(TEXT("%s.%s"), *EmitterTarget(Index), *ModuleName),
                            TEXT("A type data module sits in the module list instead of the type_data slot"),
                            TEXT("remove_particle_module and re-add with slot=\"type_data\"")));
                    }
                    else if (Module->IsA(UParticleModuleSizeScaleBySpeed::StaticClass()))
                    {
                        Checks.Add(MakeCheck(EUnrealMCPParticleCheck::InheritedModules, TEXT("info"),
                            FString::Printf(TEXT("%s.%s"), *EmitterTarget(Index), *ModuleName),
                            TEXT("SizeScaleBySpeed only has an effect together with a SizeScale module"),
                            TEXT("check that the emitter also has ParticleModuleSizeScale")));
                    }
                    else if (Module->IsA(UParticleModuleVelocity::StaticClass()) ||
                             Module->IsA(UParticleModuleVelocityOverLifetime::StaticClass()))
                    {
                        FProperty* StartVelocity = Module->GetClass()->FindPropertyByName(TEXT("StartVelocity"));
                        if (IsZeroVectorConstant(const_cast<UParticleModule*>(Module), StartVelocity))
                        {
                            Checks.Add(MakeCheck(EUnrealMCPParticleCheck::InheritedModules, TEXT("warning"),
                                FString::Printf(TEXT("%s.%s"), *EmitterTarget(Index), *ModuleName),
                                TEXT("Velocity module starts at a constant zero, so it does nothing"),
                                TEXT("set_particle_distribution or remove_particle_module")));
                        }
                    }
                }
            }
        }

        // --- material: usage + compile errors + additive opacity -------------
        if (RunsCheck(EUnrealMCPParticleCheck::MaterialUsage) || RunsCheck(EUnrealMCPParticleCheck::AdditiveOpacity))
        {
            UMaterialInterface* CheckMaterial = MeshTypeData && MeshTypeData->bOverrideMaterial ? nullptr : Material;
            UMaterial* BaseMaterial = CheckMaterial ? CheckMaterial->GetBaseMaterial() : nullptr;

            if (RunsCheck(EUnrealMCPParticleCheck::MaterialUsage))
            {
                if (!BaseMaterial)
                {
                    Checks.Add(MakeCheck(EUnrealMCPParticleCheck::MaterialUsage, TEXT("info"), EmitterTarget(Index),
                        TEXT("No material to check (TypeDataMesh overrides it)"), FString()));
                }
                else
                {
                    const EMaterialUsage Usage = MeshTypeData ? MATUSAGE_InstancedStaticMeshes : MATUSAGE_ParticleSprites;
                    const bool bHasUsage = BaseMaterial->GetUsageByFlag(Usage);
                    if (!bHasUsage)
                    {
                        Checks.Add(MakeCheck(EUnrealMCPParticleCheck::MaterialUsage, TEXT("warning"), EmitterTarget(Index),
                            FString::Printf(TEXT("%s is not marked for %s, so the emitter may render with the default material"),
                                *BaseMaterial->GetName(), MeshTypeData ? TEXT("instanced static meshes") : TEXT("particle sprites")),
                            TEXT("enable the particle usage on the material (Usage flags in the material editor)")));
                    }

                    TArray<FString> CompileErrors;
                    CollectMaterialErrors(BaseMaterial, CompileErrors);
                    if (CompileErrors.Num() > 0)
                    {
                        Checks.Add(MakeCheck(EUnrealMCPParticleCheck::MaterialUsage, TEXT("error"), EmitterTarget(Index),
                            FString::Printf(TEXT("%s has %d compile error(s): %s"), *BaseMaterial->GetName(),
                                CompileErrors.Num(), *FString::Join(CompileErrors, TEXT(" | "))),
                            TEXT("get_material_compile_errors for the full per-feature-level report")));
                    }
                    else if (bHasUsage)
                    {
                        Checks.Add(PassCheck(EUnrealMCPParticleCheck::MaterialUsage, EmitterTarget(Index),
                            FString::Printf(TEXT("%s usage and compile state are fine"), *BaseMaterial->GetName())));
                    }
                }
            }

            if (RunsCheck(EUnrealMCPParticleCheck::AdditiveOpacity) && BaseMaterial)
            {
                if (BaseMaterial->BlendMode != BLEND_Additive)
                {
                    Checks.Add(PassCheck(EUnrealMCPParticleCheck::AdditiveOpacity, EmitterTarget(Index),
                        FString::Printf(TEXT("%s is not additive"), *BaseMaterial->GetName())));
                }
                else
                {
                    const UMaterialEditorOnlyData* EditorData = BaseMaterial->GetEditorOnlyData();
                    const bool bOpacityWired = EditorData && EditorData->Opacity.Expression != nullptr;
                    if (!bOpacityWired)
                    {
                        Checks.Add(MakeCheck(EUnrealMCPParticleCheck::AdditiveOpacity, TEXT("warning"), EmitterTarget(Index),
                            FString::Printf(TEXT("Additive material %s has no Opacity input, so AlphaOverLife has no effect"),
                                *BaseMaterial->GetName()),
                            TEXT("wire mask x ParticleColor.A into Opacity, or drop the AlphaOverLife curve from the expectation")));
                    }
                    else
                    {
                        Checks.Add(PassCheck(EUnrealMCPParticleCheck::AdditiveOpacity, EmitterTarget(Index),
                            FString::Printf(TEXT("Additive material %s drives Opacity"), *BaseMaterial->GetName())));
                    }
                }
            }
        }

        // --- LOD structure --------------------------------------------------
        if (RunsCheck(EUnrealMCPParticleCheck::LODStructure))
        {
            if (Emitter->LODLevels.Num() == 0)
            {
                Checks.Add(MakeCheck(EUnrealMCPParticleCheck::LODStructure, TEXT("error"), EmitterTarget(Index),
                    TEXT("Emitter has no LOD level, so the engine cannot evaluate it"),
                    TEXT("set_particle_lod_count with lod_count >= 1")));
            }

            for (int32 LODIndex = 0; LODIndex < Emitter->LODLevels.Num(); ++LODIndex)
            {
                const UParticleLODLevel* LODLevel = Emitter->LODLevels[LODIndex];
                if (!LODLevel)
                {
                    Checks.Add(MakeCheck(EUnrealMCPParticleCheck::LODStructure, TEXT("error"), LODTarget(Index, LODIndex),
                        TEXT("Empty LOD slot"), TEXT("set_particle_lod_count to rebuild the LOD list")));
                    continue;
                }
                if (LODLevel->Modules.Num() == 0)
                {
                    Checks.Add(MakeCheck(EUnrealMCPParticleCheck::LODStructure, TEXT("warning"), LODTarget(Index, LODIndex),
                        TEXT("LOD has no modules, so nothing beyond the required module is evaluated"),
                        TEXT("add_particle_module on this emitter")));
                }

                for (int32 ModuleIndex = 0; ModuleIndex < LODLevel->Modules.Num(); ++ModuleIndex)
                {
                    const UParticleModule* Module = LODLevel->Modules[ModuleIndex];
                    if (!Module)
                    {
                        continue;
                    }
                    if ((Module->LODValidity & (1 << LODIndex)) == 0)
                    {
                        Checks.Add(MakeCheck(EUnrealMCPParticleCheck::LODStructure, TEXT("warning"),
                            LODTarget(Index, LODIndex, FString::Printf(TEXT(".modules[%d]"), ModuleIndex)),
                            FString::Printf(TEXT("%s is listed in this LOD but its lod_validity (%d) excludes it"),
                                *Module->GetClass()->GetName(), static_cast<int32>(Module->LODValidity)),
                            TEXT("copy_particle_lod or set_particle_lod_count to rebuild consistent LOD validity")));
                    }
                }
            }
        }
    }

    // --- system level: LOD counts and local/world space consistency ---------
    if (RunsCheck(EUnrealMCPParticleCheck::LODStructure))
    {
        TSet<int32> LODCounts;
        for (const UParticleEmitter* Emitter : System->Emitters)
        {
            if (Emitter)
            {
                LODCounts.Add(Emitter->LODLevels.Num());
            }
        }
        if (LODCounts.Num() > 1)
        {
            TArray<FString> Descriptions;
            for (int32 Index = 0; Index < System->Emitters.Num(); ++Index)
            {
                const UParticleEmitter* Emitter = System->Emitters[Index];
                if (Emitter)
                {
                    Descriptions.Add(FString::Printf(TEXT("emitter[%d]=%d"), Index, Emitter->LODLevels.Num()));
                }
            }
            Checks.Add(MakeCheck(EUnrealMCPParticleCheck::LODStructure, TEXT("error"), TEXT("system"),
                FString::Printf(TEXT("Emitters disagree on the LOD count: %s (the engine enforces one count for the system)"),
                    *FString::Join(Descriptions, TEXT(", "))),
                TEXT("set_particle_lod_count on the emitters that differ")));
        }
    }

    if (RunsCheck(EUnrealMCPParticleCheck::EmitterSpace))
    {
        TArray<FString> Descriptions;
        TSet<bool> SpaceValues;
        for (const int32 Index : EmitterIndices)
        {
            const UParticleEmitter* Emitter = System->Emitters.IsValidIndex(Index) ? System->Emitters[Index] : nullptr;
            const UParticleLODLevel* LODLevel = (Emitter && Emitter->LODLevels.Num() > 0) ? Emitter->LODLevels[0] : nullptr;
            if (!LODLevel || !LODLevel->RequiredModule)
            {
                continue;
            }
            const bool bLocalSpace = LODLevel->RequiredModule->bUseLocalSpace;
            SpaceValues.Add(bLocalSpace);
            Descriptions.Add(FString::Printf(TEXT("emitter[%d]=%s"), Index, bLocalSpace ? TEXT("local") : TEXT("world")));
        }

        if (SpaceValues.Num() > 1)
        {
            Checks.Add(MakeCheck(EUnrealMCPParticleCheck::EmitterSpace, TEXT("warning"), TEXT("system"),
                FString::Printf(TEXT("Emitters mix local and world space: %s"), *FString::Join(Descriptions, TEXT(", "))),
                TEXT("set_particle_module_property on each Required module (bUseLocalSpace) to make them agree")));
        }
        else if (Descriptions.Num() > 0)
        {
            Checks.Add(PassCheck(EUnrealMCPParticleCheck::EmitterSpace, TEXT("system"),
                FString::Printf(TEXT("All emitters agree on the space: %s"), *FString::Join(Descriptions, TEXT(", ")))));
        }
    }

    // Nothing here writes: the transaction/save wrappers stay out of a read command by design.
    OutResult.Success = true;
    OutResult.AssetPath = System->GetPathName();
    OutResult.EmitterCount = System->Emitters.Num();
    return true;
}

// ---------------------------------------------------------------------------
// Structure edits: order, naming, LOD switch, duplication
// ---------------------------------------------------------------------------

namespace
{
    /** Module class names of one LOD in evaluation order. */
    void CollectModuleOrder(const UParticleLODLevel* LODLevel, TArray<FString>& OutOrder)
    {
        OutOrder.Reset();
        if (!LODLevel)
        {
            return;
        }
        for (const UParticleModule* Module : LODLevel->Modules)
        {
            OutOrder.Add(Module ? Module->GetClass()->GetName() : FString(TEXT("None")));
        }
    }

    /**
     * Whether the module occupies one of the LOD's single-module slots. Those are placed by the
     * engine's own rules (required is created with the emitter, spawn drives spawning), so their
     * position in the list is not the caller's to choose.
     */
    bool IsSlotModuleOf(const UParticleLODLevel* LODLevel, const UParticleModule* Module)
    {
        if (!LODLevel || !Module)
        {
            return false;
        }
        return Module == LODLevel->RequiredModule.Get() || Module == LODLevel->SpawnModule.Get() ||
               Module == LODLevel->TypeDataModule.Get() || Module == LODLevel->EventGenerator.Get();
    }
}

bool FUnrealMCPParticleOps::MoveModule(const FString& AssetPath, int32 EmitterIndex, const FString& ModuleClassName,
                                       int32 ModuleIndex, int32 LODIndex, int32 ToIndex,
                                       FUnrealMCPParticleOpResult& OutResult)
{
    OutResult = FUnrealMCPParticleOpResult();

    UParticleSystem* System = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!ResolveParticleSystem(AssetPath, System, ErrorCode, ErrorMessage))
    {
        Fail(OutResult, ErrorCode, ErrorMessage);
        return false;
    }

    if (RejectIfEditorOpen(System, OutResult))
    {
        return false;
    }

    TArray<FString> Candidates;
    UParticleEmitter* Emitter = nullptr;
    if (!ResolveEmitter(System, EmitterIndex, Emitter, ErrorCode, ErrorMessage, Candidates))
    {
        Fail(OutResult, ErrorCode, ErrorMessage, Candidates);
        return false;
    }

    UParticleLODLevel* LODLevel = nullptr;
    if (!ResolveLODLevel(Emitter, LODIndex, LODLevel, ErrorCode, ErrorMessage, Candidates))
    {
        Fail(OutResult, ErrorCode, ErrorMessage, Candidates);
        return false;
    }

    UParticleModule* Module = nullptr;
    int32 ResolvedIndex = -1;
    if (!ResolveModule(Emitter, LODIndex, ModuleClassName, ModuleIndex,
                       FString(EUnrealMCPParticleSlot::Modules), Module, ResolvedIndex,
                       ErrorCode, ErrorMessage, Candidates))
    {
        Fail(OutResult, ErrorCode, ErrorMessage, Candidates);
        return false;
    }

    if (IsSlotModuleOf(LODLevel, Module))
    {
        Fail(OutResult, EUnrealMCPParticleError::UnsupportedModuleSlot,
            FString::Printf(TEXT("%s occupies a single-module slot; the engine decides where it sits"),
                *Module->GetClass()->GetName()),
            TArray<FString>{ TEXT("required"), TEXT("spawn"), TEXT("type_data"), TEXT("event_generator") });
        return false;
    }

    const int32 ModuleCount = LODLevel->Modules.Num();
    if (ToIndex < 0 || ToIndex >= ModuleCount)
    {
        TArray<FString> ValidIndices;
        for (int32 Index = 0; Index < ModuleCount; ++Index)
        {
            ValidIndices.Add(FString::FromInt(Index));
        }
        Fail(OutResult, EUnrealMCPParticleError::InvalidParams,
            FString::Printf(TEXT("'to_index' %d is out of range (0..%d)"), ToIndex, ModuleCount - 1),
            ValidIndices);
        return false;
    }

    if (ToIndex != ResolvedIndex)
    {
        System->PreEditChange(nullptr);

        // Every LOD level of the emitter keeps the same evaluation order, so the move is applied to
        // each level that lists this module.
        for (UParticleLODLevel* Level : Emitter->LODLevels)
        {
            if (!Level)
            {
                continue;
            }

            const int32 FromIndex = Level->Modules.IndexOfByPredicate(
                [Module](const TObjectPtr<UParticleModule>& Entry) { return Entry == Module; });
            if (FromIndex == INDEX_NONE)
            {
                continue;
            }

            Level->Modules.RemoveAt(FromIndex);
            const int32 InsertAt = FMath::Clamp(ToIndex, 0, Level->Modules.Num());
            Level->Modules.Insert(Module, InsertAt);
        }

        System->UpdateAllModuleLists();
        RefreshSoloing(System);
        System->PostEditChange();
        System->MarkPackageDirty();
    }

    OutResult.Success = true;
    OutResult.EmitterIndex = EmitterIndex;
    OutResult.LODIndex = LODIndex;
    OutResult.ModuleClass = ClassNameOf(Module);
    OutResult.ModuleIndex = ToIndex;
    OutResult.FromIndex = ResolvedIndex;
    OutResult.ToIndex = ToIndex;
    CollectModuleOrder(LODLevel, OutResult.ModuleOrder);
    PersistSystem(System);
    FillEmitterReadback(System, OutResult);
    return true;
}

bool FUnrealMCPParticleOps::SetEmitterName(const FString& AssetPath, int32 EmitterIndex, const FString& Name,
                                           FUnrealMCPParticleOpResult& OutResult)
{
    OutResult = FUnrealMCPParticleOpResult();

    UParticleSystem* System = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!ResolveParticleSystem(AssetPath, System, ErrorCode, ErrorMessage))
    {
        Fail(OutResult, ErrorCode, ErrorMessage);
        return false;
    }

    if (RejectIfEditorOpen(System, OutResult))
    {
        return false;
    }

    TArray<FString> Candidates;
    UParticleEmitter* Emitter = nullptr;
    if (!ResolveEmitter(System, EmitterIndex, Emitter, ErrorCode, ErrorMessage, Candidates))
    {
        Fail(OutResult, ErrorCode, ErrorMessage, Candidates);
        return false;
    }

    if (Name.IsEmpty())
    {
        Fail(OutResult, EUnrealMCPParticleError::InvalidParams, TEXT("'name' must not be empty"));
        return false;
    }

    System->PreEditChange(nullptr);
    const FString NameBefore = Emitter->GetEmitterName().ToString();
    Emitter->SetEmitterName(FName(*Name));
    Emitter->PostEditChange();
    System->PostEditChange();
    System->MarkPackageDirty();

    OutResult.Success = true;
    OutResult.EmitterIndex = EmitterIndex;
    OutResult.NameBefore = NameBefore;
    OutResult.NameAfter = Emitter->GetEmitterName().ToString();
    PersistSystem(System);
    FillEmitterReadback(System, OutResult);
    return true;
}

bool FUnrealMCPParticleOps::SetLODEnabled(const FString& AssetPath, int32 EmitterIndex, int32 LODIndex, bool bEnabled,
                                          FUnrealMCPParticleOpResult& OutResult)
{
    OutResult = FUnrealMCPParticleOpResult();

    UParticleSystem* System = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!ResolveParticleSystem(AssetPath, System, ErrorCode, ErrorMessage))
    {
        Fail(OutResult, ErrorCode, ErrorMessage);
        return false;
    }

    if (RejectIfEditorOpen(System, OutResult))
    {
        return false;
    }

    TArray<FString> Candidates;
    UParticleEmitter* Emitter = nullptr;
    if (!ResolveEmitter(System, EmitterIndex, Emitter, ErrorCode, ErrorMessage, Candidates))
    {
        Fail(OutResult, ErrorCode, ErrorMessage, Candidates);
        return false;
    }

    UParticleLODLevel* LODLevel = nullptr;
    if (!ResolveLODLevel(Emitter, LODIndex, LODLevel, ErrorCode, ErrorMessage, Candidates))
    {
        Fail(OutResult, ErrorCode, ErrorMessage, Candidates);
        return false;
    }

    if (!bEnabled && LODIndex == 0)
    {
        Fail(OutResult, EUnrealMCPParticleError::InvalidParams,
            TEXT("LOD 0 must stay enabled: an emitter with no active level has nothing to evaluate"));
        return false;
    }

    System->PreEditChange(nullptr);
    LODLevel->bEnabled = bEnabled ? 1 : 0;
    // The solo snapshot the engine restores from has to be refreshed, otherwise the next
    // SetupSoloing (the save path runs it) puts the previous enable state back.
    RefreshSoloing(System);
    System->PostEditChange();
    System->MarkPackageDirty();

    OutResult.Success = true;
    OutResult.EmitterIndex = EmitterIndex;
    OutResult.LODIndex = LODIndex;
    OutResult.LODEnabled = LODLevel->bEnabled != 0;
    OutResult.bLODEnabledSet = true;
    PersistSystem(System);
    FillEmitterReadback(System, OutResult);
    return true;
}

bool FUnrealMCPParticleOps::DuplicateEmitter(const FString& AssetPath, int32 EmitterIndex, const FString& Name,
                                             FUnrealMCPParticleOpResult& OutResult)
{
    OutResult = FUnrealMCPParticleOpResult();

    UParticleSystem* System = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!ResolveParticleSystem(AssetPath, System, ErrorCode, ErrorMessage))
    {
        Fail(OutResult, ErrorCode, ErrorMessage);
        return false;
    }

    if (RejectIfEditorOpen(System, OutResult))
    {
        return false;
    }

    TArray<FString> Candidates;
    UParticleEmitter* Emitter = nullptr;
    if (!ResolveEmitter(System, EmitterIndex, Emitter, ErrorCode, ErrorMessage, Candidates))
    {
        Fail(OutResult, ErrorCode, ErrorMessage, Candidates);
        return false;
    }

    if (Emitter->LODLevels.Num() == 0)
    {
        Fail(OutResult, EUnrealMCPParticleError::ParticleSystemNotReady,
            FString::Printf(TEXT("Emitter %d has no LOD level to copy"), EmitterIndex));
        return false;
    }

    const FString SourceName = Emitter->GetEmitterName().ToString();
    const FString NewName = Name.IsEmpty() ? FString::Printf(TEXT("%s_Copy"), *SourceName) : Name;

    System->PreEditChange(nullptr);

    UParticleEmitter* NewEmitter = NewObject<UParticleEmitter>(System, Emitter->GetClass(), NAME_None, RF_Transactional);
    if (!NewEmitter)
    {
        Fail(OutResult, EUnrealMCPParticleError::WriteFailed,
            FString::Printf(TEXT("Failed to create a %s"), *Emitter->GetClass()->GetName()));
        return false;
    }

    // One duplicate per distinct module, shared by every new LOD level - the engine's own emitters
    // share a module object across their LOD levels, and editing it has to affect all of them.
    TMap<UParticleModule*, UParticleModule*> Duplicates;

    for (int32 LevelIndex = 0; LevelIndex < Emitter->LODLevels.Num(); ++LevelIndex)
    {
        const UParticleLODLevel* SourceLevel = Emitter->LODLevels[LevelIndex];
        if (!SourceLevel)
        {
            continue;
        }

        const int32 CreatedIndex = NewEmitter->CreateLODLevel(LevelIndex, /*bGenerateModuleData=*/false);
        if (CreatedIndex < 0 || !NewEmitter->LODLevels.IsValidIndex(CreatedIndex))
        {
            Fail(OutResult, EUnrealMCPParticleError::WriteFailed,
                FString::Printf(TEXT("Failed to create LOD %d on the copy of emitter %d"), LevelIndex, EmitterIndex));
            return false;
        }

        UParticleLODLevel* NewLevel = NewEmitter->LODLevels[CreatedIndex];
        if (!NewLevel)
        {
            Fail(OutResult, EUnrealMCPParticleError::WriteFailed,
                FString::Printf(TEXT("The copy of emitter %d has an empty LOD %d"), EmitterIndex, LevelIndex));
            return false;
        }

        NewLevel->Level = SourceLevel->Level;
        NewLevel->bEnabled = SourceLevel->bEnabled;

        if (const UParticleModuleRequired* SourceRequired = SourceLevel->RequiredModule)
        {
            if (UParticleModuleRequired* NewRequired =
                    Cast<UParticleModuleRequired>(StaticDuplicateObject(SourceRequired, System)))
            {
                NewLevel->RequiredModule = NewRequired;
            }
        }

        auto DuplicateSlotModule = [System, &Duplicates](UParticleModule* SourceModule) -> UParticleModule*
        {
            if (!SourceModule)
            {
                return nullptr;
            }
            if (UParticleModule* Existing = Duplicates.FindRef(SourceModule))
            {
                return Existing;
            }
            UParticleModule* NewModule = Cast<UParticleModule>(StaticDuplicateObject(SourceModule, System));
            if (NewModule)
            {
                Duplicates.Add(SourceModule, NewModule);
            }
            return NewModule;
        };

        if (UParticleModule* SourceSpawn = SourceLevel->SpawnModule.Get())
        {
            if (UParticleModuleSpawn* NewSpawn = Cast<UParticleModuleSpawn>(DuplicateSlotModule(SourceSpawn)))
            {
                NewLevel->SpawnModule = NewSpawn;
            }
        }
        if (UParticleModule* SourceTypeData = SourceLevel->TypeDataModule.Get())
        {
            if (UParticleModuleTypeDataBase* NewTypeData =
                    Cast<UParticleModuleTypeDataBase>(DuplicateSlotModule(SourceTypeData)))
            {
                NewLevel->TypeDataModule = NewTypeData;
            }
        }
        if (UParticleModule* SourceEventGenerator = SourceLevel->EventGenerator.Get())
        {
            if (UParticleModuleEventGenerator* NewEventGenerator =
                    Cast<UParticleModuleEventGenerator>(DuplicateSlotModule(SourceEventGenerator)))
            {
                NewLevel->EventGenerator = NewEventGenerator;
            }
        }

        NewLevel->Modules.Reset();
        for (UParticleModule* SourceModule : SourceLevel->Modules)
        {
            if (!SourceModule)
            {
                continue;
            }

            if (SourceModule == SourceLevel->RequiredModule.Get())
            {
                NewLevel->Modules.Add(NewLevel->RequiredModule);
                continue;
            }

            UParticleModule*& Duplicate = Duplicates.FindOrAdd(SourceModule);
            if (!Duplicate)
            {
                Duplicate = Cast<UParticleModule>(StaticDuplicateObject(SourceModule, System));
            }
            if (!Duplicate)
            {
                Fail(OutResult, EUnrealMCPParticleError::WriteFailed,
                    FString::Printf(TEXT("Failed to copy module %s"), *SourceModule->GetClass()->GetName()));
                return false;
            }
            NewLevel->Modules.Add(Duplicate);
        }
    }

    NewEmitter->SetEmitterName(FName(*NewName));
    System->Emitters.Add(NewEmitter);

    System->UpdateAllModuleLists();
    RefreshSoloing(System);
    System->PostEditChange();
    System->MarkPackageDirty();

    OutResult.Success = true;
    OutResult.EmitterIndex = System->Emitters.Num() - 1;
    OutResult.NameBefore = SourceName;
    OutResult.NameAfter = NewEmitter->GetEmitterName().ToString();
    for (const UParticleLODLevel* NewLevel : NewEmitter->LODLevels)
    {
        OutResult.ModuleCounts.Add(NewLevel ? NewLevel->Modules.Num() : 0);
    }
    PersistSystem(System);
    FillEmitterReadback(System, OutResult);
    return true;
}

bool FUnrealMCPParticleOps::AddModules(const FString& AssetPath, int32 EmitterIndex, const TArray<FString>& ModuleClassNames,
                                       int32 LODIndex, int32 InsertIndex, const FString& Slot, bool bHideSprite,
                                       FUnrealMCPParticleOpResult& OutResult)
{
    OutResult = FUnrealMCPParticleOpResult();

    if (ModuleClassNames.Num() == 0)
    {
        Fail(OutResult, EUnrealMCPParticleError::InvalidParams, TEXT("'module_classes' must not be empty"));
        return false;
    }

    // Every class is resolved before anything is created: a typo in a batch must not leave half of
    // it behind. (The command layer's transaction would be cancelled too, but a clear error is
    // cheaper than an undo.)
    UParticleSystem* System = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!ResolveParticleSystem(AssetPath, System, ErrorCode, ErrorMessage))
    {
        Fail(OutResult, ErrorCode, ErrorMessage);
        return false;
    }

    TArray<FString> Candidates;
    for (const FString& ModuleClassName : ModuleClassNames)
    {
        UClass* ModuleClass = nullptr;
        if (!ResolveModuleClass(ModuleClassName, ModuleClass, ErrorCode, ErrorMessage, Candidates))
        {
            Fail(OutResult, ErrorCode, ErrorMessage, Candidates);
            return false;
        }
    }

    for (int32 Index = 0; Index < ModuleClassNames.Num(); ++Index)
    {
        // Keep the requested order: an explicit insert index advances with the batch.
        const int32 StepInsertIndex = (InsertIndex < 0) ? -1 : (InsertIndex + Index);
        // hide_sprite describes the final state of the emitter, so it rides on the last step.
        const bool bStepHideSprite = bHideSprite && (Index == ModuleClassNames.Num() - 1);

        FUnrealMCPParticleOpResult StepResult;
        if (!AddModule(AssetPath, EmitterIndex, ModuleClassNames[Index], LODIndex, StepInsertIndex, Slot,
                       bStepHideSprite, StepResult))
        {
            OutResult = StepResult;
            return false;
        }

        FUnrealMCPParticleAddedModule Added;
        Added.ModuleClass = StepResult.ModuleClass;
        Added.ModuleIndex = StepResult.ModuleIndex;
        OutResult.AddedModules.Add(Added);

        OutResult.EmitterIndex = StepResult.EmitterIndex;
        OutResult.LODIndex = StepResult.LODIndex;
        OutResult.SpriteHidden = StepResult.SpriteHidden;
        OutResult.bUseMaxDrawCount = StepResult.bUseMaxDrawCount;
        OutResult.MaxDrawCount = StepResult.MaxDrawCount;
    }

    OutResult.Success = true;
    FillEmitterReadback(System, OutResult);
    return true;
}

bool FUnrealMCPParticleOps::AddModulesFromEmitter(const FString& AssetPath, int32 EmitterIndex, int32 SourceEmitterIndex,
                                                  int32 LODIndex, FUnrealMCPParticleOpResult& OutResult)
{
    OutResult = FUnrealMCPParticleOpResult();

    UParticleSystem* System = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!ResolveParticleSystem(AssetPath, System, ErrorCode, ErrorMessage))
    {
        Fail(OutResult, ErrorCode, ErrorMessage);
        return false;
    }

    TArray<FString> Candidates;
    UParticleEmitter* SourceEmitter = nullptr;
    if (!ResolveEmitter(System, SourceEmitterIndex, SourceEmitter, ErrorCode, ErrorMessage, Candidates))
    {
        Fail(OutResult, ErrorCode, ErrorMessage, Candidates);
        return false;
    }

    UParticleLODLevel* SourceLODLevel = nullptr;
    if (!ResolveLODLevel(SourceEmitter, LODIndex, SourceLODLevel, ErrorCode, ErrorMessage, Candidates))
    {
        Fail(OutResult, ErrorCode, ErrorMessage, Candidates);
        return false;
    }

    // Slot modules belong to the emitter itself (the target already has its own), everything else
    // is a module worth copying - repeated classes included, that is how seeded variants work.
    TArray<FString> ModuleClassNames;
    for (const UParticleModule* Module : SourceLODLevel->Modules)
    {
        if (!Module || IsSlotModuleOf(SourceLODLevel, Module))
        {
            continue;
        }
        ModuleClassNames.Add(Module->GetClass()->GetName());
    }

    if (ModuleClassNames.Num() == 0)
    {
        Fail(OutResult, EUnrealMCPParticleError::InvalidParams,
            FString::Printf(TEXT("Emitter %d has no copyable modules on LOD %d"), SourceEmitterIndex, LODIndex));
        return false;
    }

    return AddModules(AssetPath, EmitterIndex, ModuleClassNames, LODIndex, -1,
                      FString(EUnrealMCPParticleSlot::Modules), /*bHideSprite=*/false, OutResult);
}

