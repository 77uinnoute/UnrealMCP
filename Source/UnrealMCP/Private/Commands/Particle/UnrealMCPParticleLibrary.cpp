#include "Commands/Particle/UnrealMCPParticleLibrary.h"
#include "Commands/Particle/UnrealMCPParticleOps.h"
#include "Commands/Blueprint/UnrealMCPBlueprintGraphOps.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleEmitter.h"
#include "Particles/ParticleLODLevel.h"
#include "Particles/ParticleModule.h"

namespace
{
    /** Fill a list-result error from the kernel's error triple. */
    FUnrealMCPParticleListResult MakeListFailure(const FString& ErrorCode, const FString& ErrorMessage,
                                                const TArray<FString>& Candidates)
    {
        FUnrealMCPParticleListResult Result;
        Result.Success = false;
        Result.ErrorCode = ErrorCode;
        Result.ErrorMessage = ErrorMessage;
        Result.Candidates = Candidates;
        return Result;
    }

    FUnrealMCPParticleListResult MakeListFailureFrom(const FUnrealMCPParticleOpResult& Source)
    {
        return MakeListFailure(Source.ErrorCode, Source.ErrorMessage, Source.Candidates);
    }

    /** Everything the reads need before they can answer: a valid particle system. */
    bool ResolveSystemAsset(UObject* Asset, UParticleSystem*& OutSystem, FString& OutErrorCode,
                            FString& OutErrorMessage)
    {
        if (!Asset)
        {
            OutErrorCode = EUnrealMCPParticleError::InvalidParams;
            OutErrorMessage = TEXT("Invalid asset");
            return false;
        }

        OutSystem = Cast<UParticleSystem>(Asset);
        if (!OutSystem)
        {
            OutErrorCode = EUnrealMCPParticleError::AssetNotParticleSystem;
            OutErrorMessage = FString::Printf(TEXT("Asset '%s' is a %s, not a UParticleSystem"),
                *Asset->GetName(), *Asset->GetClass()->GetName());
            return false;
        }
        return true;
    }

    FUnrealMCPParticleListResult MakeEmitterListResult(UParticleSystem* System, int32 EmitterIndex,
                                                       int32 LODIndex, bool bFilterToLOD)
    {
        FUnrealMCPParticleListResult Result;

        FString ErrorCode;
        FString ErrorMessage;
        TArray<FString> Candidates;

        UParticleEmitter* Emitter = nullptr;
        if (!FUnrealMCPParticleOps::ResolveEmitter(System, EmitterIndex, Emitter, ErrorCode, ErrorMessage, Candidates))
        {
            return MakeListFailure(ErrorCode, ErrorMessage, Candidates);
        }

        if (bFilterToLOD)
        {
            UParticleLODLevel* LODLevel = nullptr;
            if (!FUnrealMCPParticleOps::ResolveLODLevel(Emitter, LODIndex, LODLevel, ErrorCode, ErrorMessage, Candidates))
            {
                return MakeListFailure(ErrorCode, ErrorMessage, Candidates);
            }
        }

        FUnrealMCPParticleEmitterInfo EmitterInfo;
        FUnrealMCPParticleOps::MakeEmitterInfo(System, EmitterIndex, EmitterInfo);
        if (bFilterToLOD && EmitterInfo.Lods.IsValidIndex(LODIndex))
        {
            TArray<FUnrealMCPParticleLODInfo> SingleLOD;
            SingleLOD.Add(EmitterInfo.Lods[LODIndex]);
            EmitterInfo.Lods = MoveTemp(SingleLOD);
        }

        Result.Success = true;
        Result.AssetPath = System->GetPathName();
        Result.EmitterCount = 1;
        Result.Emitters.Add(MoveTemp(EmitterInfo));
        return Result;
    }
}

FUnrealMCPParticleListResult UUnrealMCPParticleLibrary::ListParticleEmitters(UObject* Asset)
{
    FUnrealMCPParticleListResult Result;

    UParticleSystem* System = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!ResolveSystemAsset(Asset, System, ErrorCode, ErrorMessage))
    {
        return MakeListFailure(ErrorCode, ErrorMessage, TArray<FString>());
    }

    TArray<FUnrealMCPParticleEmitterInfo> Emitters;
    TArray<FString> Candidates;
    if (!FUnrealMCPParticleOps::ListEmitters(System, Emitters, ErrorCode, ErrorMessage, Candidates))
    {
        return MakeListFailure(ErrorCode, ErrorMessage, Candidates);
    }

    Result.Success = true;
    Result.AssetPath = System->GetPathName();
    Result.EmitterCount = Emitters.Num();
    Result.Emitters = MoveTemp(Emitters);
    return Result;
}

FUnrealMCPParticleListResult UUnrealMCPParticleLibrary::ListParticleModules(UObject* Asset, int32 EmitterIndex,
                                                                           int32 LODIndex)
{
    UParticleSystem* System = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!ResolveSystemAsset(Asset, System, ErrorCode, ErrorMessage))
    {
        return MakeListFailure(ErrorCode, ErrorMessage, TArray<FString>());
    }

    return MakeEmitterListResult(System, EmitterIndex, LODIndex, /*bFilterToLOD=*/true);
}

FUnrealMCPParticleListResult UUnrealMCPParticleLibrary::ValidateParticleSystem(UObject* Asset, int32 EmitterIndex,
                                                                               const TArray<FString>& CheckIds)
{
    FUnrealMCPParticleListResult Result;

    UParticleSystem* System = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!ResolveSystemAsset(Asset, System, ErrorCode, ErrorMessage))
    {
        Result.Success = false;
        Result.ErrorCode = ErrorCode;
        Result.ErrorMessage = ErrorMessage;
        return Result;
    }

    FUnrealMCPParticleOps::ValidateParticleSystem(System->GetPathName(), EmitterIndex, CheckIds, Result);
    return Result;
}

FUnrealMCPParticleOpResult UUnrealMCPParticleLibrary::AddParticleModules(const FString& AssetPath, int32 EmitterIndex,
                                                                        const TArray<FString>& ModuleClasses,
                                                                        int32 LODIndex, int32 InsertIndex,
                                                                        const FString& Slot, bool bHideSprite)
{
    FUnrealMCPParticleOpResult Result;
    FUnrealMCPParticleOps::AddModules(AssetPath, EmitterIndex, ModuleClasses, LODIndex, InsertIndex, Slot,
                                      bHideSprite, Result);
    return Result;
}

FUnrealMCPParticleOpResult UUnrealMCPParticleLibrary::AddParticleModulesFromEmitter(const FString& AssetPath,
                                                                                   int32 EmitterIndex,
                                                                                   int32 SourceEmitterIndex,
                                                                                   int32 LODIndex)
{
    FUnrealMCPParticleOpResult Result;
    FUnrealMCPParticleOps::AddModulesFromEmitter(AssetPath, EmitterIndex, SourceEmitterIndex, LODIndex, Result);
    return Result;
}

FUnrealMCPParticleOpResult UUnrealMCPParticleLibrary::MoveParticleModule(const FString& AssetPath, int32 EmitterIndex,
                                                                        const FString& ModuleClass, int32 ModuleIndex,
                                                                        int32 LODIndex, int32 ToIndex)
{
    FUnrealMCPParticleOpResult Result;
    FUnrealMCPParticleOps::MoveModule(AssetPath, EmitterIndex, ModuleClass, ModuleIndex, LODIndex, ToIndex, Result);
    return Result;
}

FUnrealMCPParticleOpResult UUnrealMCPParticleLibrary::SetParticleEmitterName(const FString& AssetPath, int32 EmitterIndex,
                                                                            const FString& Name)
{
    FUnrealMCPParticleOpResult Result;
    FUnrealMCPParticleOps::SetEmitterName(AssetPath, EmitterIndex, Name, Result);
    return Result;
}

FUnrealMCPParticleOpResult UUnrealMCPParticleLibrary::SetParticleLODEnabled(const FString& AssetPath, int32 EmitterIndex,
                                                                           int32 LODIndex, bool bEnabled)
{
    FUnrealMCPParticleOpResult Result;
    FUnrealMCPParticleOps::SetLODEnabled(AssetPath, EmitterIndex, LODIndex, bEnabled, Result);
    return Result;
}

FUnrealMCPParticleOpResult UUnrealMCPParticleLibrary::DuplicateParticleEmitter(const FString& AssetPath,
                                                                              int32 EmitterIndex, const FString& Name)
{
    FUnrealMCPParticleOpResult Result;
    FUnrealMCPParticleOps::DuplicateEmitter(AssetPath, EmitterIndex, Name, Result);
    return Result;
}

FUnrealMCPParticleOpResult UUnrealMCPParticleLibrary::GetParticleModule(UObject* Asset, int32 EmitterIndex,
                                                                       const FString& ModuleClass, int32 ModuleIndex,
                                                                       int32 LODIndex)
{
    FUnrealMCPParticleOpResult Result;

    UParticleSystem* System = nullptr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!ResolveSystemAsset(Asset, System, ErrorCode, ErrorMessage))
    {
        FUnrealMCPParticleOps::Fail(Result, ErrorCode, ErrorMessage);
        return Result;
    }

    TArray<FString> Candidates;
    UParticleEmitter* Emitter = nullptr;
    if (!FUnrealMCPParticleOps::ResolveEmitter(System, EmitterIndex, Emitter, ErrorCode, ErrorMessage, Candidates))
    {
        FUnrealMCPParticleOps::Fail(Result, ErrorCode, ErrorMessage, Candidates);
        return Result;
    }

    UParticleModule* Module = nullptr;
    int32 ResolvedModuleIndex = -1;
    if (!FUnrealMCPParticleOps::ResolveModule(Emitter, LODIndex, ModuleClass, ModuleIndex,
                                              FString(), Module, ResolvedModuleIndex,
                                              ErrorCode, ErrorMessage, Candidates))
    {
        FUnrealMCPParticleOps::Fail(Result, ErrorCode, ErrorMessage, Candidates);
        return Result;
    }

    FUnrealMCPParticleOps::MakeModuleInfo(Module, ResolvedModuleIndex,
        FUnrealMCPParticleOps::DetermineModuleSlot(Emitter, LODIndex, Module), Result.Module);
    Result.Success = true;
    Result.AssetPath = System->GetPathName();
    Result.EmitterIndex = EmitterIndex;
    Result.LODIndex = LODIndex;
    Result.ModuleClass = Result.Module.ModuleClass;
    Result.ModuleIndex = ResolvedModuleIndex;
    return Result;
}

FUnrealMCPParticleOpResult UUnrealMCPParticleLibrary::CreateParticleSystem(const FString& Name, const FString& Folder,
                                                                          const FString& EmitterClass, int32 LODCount)
{
    FUnrealMCPParticleOpResult Result;
    FUnrealMCPParticleOps::CreateParticleSystem(Name, Folder, EmitterClass, LODCount, Result);
    return Result;
}

FUnrealMCPParticleOpResult UUnrealMCPParticleLibrary::AddParticleEmitter(const FString& AssetPath,
                                                                        const FString& EmitterClass, int32 InsertIndex,
                                                                        int32 LODCount)
{
    FUnrealMCPParticleOpResult Result;
    FUnrealMCPParticleOps::AddEmitter(AssetPath, EmitterClass, InsertIndex, LODCount, Result);
    return Result;
}

FUnrealMCPParticleOpResult UUnrealMCPParticleLibrary::RemoveParticleEmitter(const FString& AssetPath, int32 EmitterIndex)
{
    FUnrealMCPParticleOpResult Result;
    FUnrealMCPParticleOps::RemoveEmitter(AssetPath, EmitterIndex, Result);
    return Result;
}

FUnrealMCPParticleOpResult UUnrealMCPParticleLibrary::SetParticleLODCount(const FString& AssetPath, int32 EmitterIndex,
                                                                         int32 LODCount)
{
    FUnrealMCPParticleOpResult Result;
    FUnrealMCPParticleOps::SetLODCount(AssetPath, EmitterIndex, LODCount, Result);
    return Result;
}

FUnrealMCPParticleOpResult UUnrealMCPParticleLibrary::CopyParticleLOD(const FString& AssetPath, int32 EmitterIndex,
                                                                     int32 SourceLODIndex, int32 InsertIndex)
{
    FUnrealMCPParticleOpResult Result;
    FUnrealMCPParticleOps::CopyLOD(AssetPath, EmitterIndex, SourceLODIndex, InsertIndex, Result);
    return Result;
}

FUnrealMCPParticleOpResult UUnrealMCPParticleLibrary::SetParticleLODDistance(const FString& AssetPath, int32 EmitterIndex,
                                                                           int32 LODIndex, float Distance)
{
    FUnrealMCPParticleOpResult Result;
    FUnrealMCPParticleOps::SetLODDistance(AssetPath, EmitterIndex, LODIndex, Distance, Result);
    return Result;
}

FUnrealMCPParticleOpResult UUnrealMCPParticleLibrary::AddParticleModule(const FString& AssetPath, int32 EmitterIndex,
                                                                       const FString& ModuleClass, int32 LODIndex,
                                                                       int32 InsertIndex, const FString& Slot,
                                                                       bool bHideSprite)
{
    FUnrealMCPParticleOpResult Result;
    FUnrealMCPParticleOps::AddModule(AssetPath, EmitterIndex, ModuleClass, LODIndex, InsertIndex, Slot,
                                     bHideSprite, Result);
    return Result;
}

FUnrealMCPParticleOpResult UUnrealMCPParticleLibrary::SetParticleBursts(const FString& AssetPath, int32 EmitterIndex,
                                                                       int32 LODIndex,
                                                                       const TArray<FUnrealMCPParticleBurst>& Bursts)
{
    FUnrealMCPParticleOpResult Result;
    FUnrealMCPParticleOps::SetParticleBursts(AssetPath, EmitterIndex, LODIndex, Bursts, Result);
    return Result;
}

FUnrealMCPParticleOpResult UUnrealMCPParticleLibrary::RemoveParticleModule(const FString& AssetPath, int32 EmitterIndex,
                                                                          const FString& ModuleClass, int32 ModuleIndex,
                                                                          int32 LODIndex)
{
    FUnrealMCPParticleOpResult Result;
    FUnrealMCPParticleOps::RemoveModule(AssetPath, EmitterIndex, ModuleClass, ModuleIndex, LODIndex, Result);
    return Result;
}

FUnrealMCPParticleOpResult UUnrealMCPParticleLibrary::SetParticleModuleProperty(const FString& AssetPath,
                                                                               int32 EmitterIndex,
                                                                               const FString& ModuleClass,
                                                                               int32 ModuleIndex, int32 LODIndex,
                                                                               const FString& PropertyName,
                                                                               const FString& Value,
                                                                               const FString& ValueKind)
{
    FUnrealMCPParticleOpResult Result;

    // Same value dispatch the graph library and the MCP commands use, so a kind that works
    // in one entry point works in the other.
    TSharedPtr<FJsonValue> ValueJson;
    FString ValueErrorMessage;
    if (!FUnrealMCPBlueprintGraphOps::MakeJsonValueFromString(Value, ValueKind, ValueJson, ValueErrorMessage))
    {
        FUnrealMCPParticleOps::Fail(Result, EUnrealMCPParticleError::InvalidValue,
            FString::Printf(TEXT("Property '%s': %s"), *PropertyName, *ValueErrorMessage));
        return Result;
    }

    FUnrealMCPParticleOps::SetModuleProperty(AssetPath, EmitterIndex, ModuleClass, ModuleIndex, LODIndex,
        PropertyName, ValueJson, Result);
    return Result;
}

FUnrealMCPParticleOpResult UUnrealMCPParticleLibrary::SetParticleDistribution(const FString& AssetPath,
                                                                            int32 EmitterIndex,
                                                                            const FString& ModuleClass,
                                                                            int32 ModuleIndex, int32 LODIndex,
                                                                            const FString& PropertyName,
                                                                            const FString& Kind,
                                                                            const TArray<float>& Values,
                                                                            const TArray<FUnrealMCPParticleDistributionKey>& Keys)
{
    FUnrealMCPParticleOpResult Result;
    FUnrealMCPParticleOps::SetDistribution(AssetPath, EmitterIndex, ModuleClass, ModuleIndex, LODIndex,
        PropertyName, Kind, Values, Keys, Result);
    return Result;
}
