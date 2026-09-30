#pragma once

#include "CoreMinimal.h"
#include "Commands/Particle/UnrealMCPParticleLibrary.h"
#include "Json.h"

class UParticleSystem;
class UParticleEmitter;
class UParticleLODLevel;
class UParticleModule;

/**
 * Structured error codes shared by the MCP commands and the python reflection library.
 * A single set of codes is what keeps the two entry points interchangeable.
 */
namespace EUnrealMCPParticleError
{
    inline const TCHAR* AssetNotFound              = TEXT("asset_not_found");
    inline const TCHAR* AssetNotParticleSystem     = TEXT("asset_not_particle_system");
    inline const TCHAR* ParticleSystemNotReady     = TEXT("particle_system_not_ready");
    inline const TCHAR* AssetExists                = TEXT("asset_exists");
    inline const TCHAR* CreateFailed               = TEXT("create_failed");
    inline const TCHAR* EmitterIndexOutOfRange     = TEXT("emitter_index_out_of_range");
    inline const TCHAR* LODIndexOutOfRange         = TEXT("lod_index_out_of_range");
    inline const TCHAR* ModuleNotFound             = TEXT("module_not_found");
    inline const TCHAR* AmbiguousModule            = TEXT("ambiguous_module");
    inline const TCHAR* ModuleClassNotFound        = TEXT("module_class_not_found");
    inline const TCHAR* EmitterClassNotFound       = TEXT("emitter_class_not_found");
    inline const TCHAR* UnsupportedModuleSlot      = TEXT("unsupported_module_slot");
    inline const TCHAR* UnsupportedModuleProperty  = TEXT("unsupported_module_property");
    inline const TCHAR* NotADistributionProperty   = TEXT("not_a_distribution_property");
    inline const TCHAR* UnsupportedDistributionKind= TEXT("unsupported_distribution_kind");
    inline const TCHAR* ParticleEditorOpen       = TEXT("particle_editor_open");
    inline const TCHAR* InvalidParams              = TEXT("invalid_params");
    inline const TCHAR* InvalidValue               = TEXT("invalid_value");
    inline const TCHAR* TypeMismatch               = TEXT("type_mismatch");
    inline const TCHAR* UnknownProperty            = TEXT("unknown_property");
    inline const TCHAR* WriteFailed                = TEXT("write_failed");
    inline const TCHAR* UnknownField               = TEXT("unknown_field");
    inline const TCHAR* InvalidDetail              = TEXT("invalid_detail");
    inline const TCHAR* UnknownResponseField       = TEXT("unknown_response_field");
    inline const TCHAR* UnknownCheck               = TEXT("unknown_check");
}

/** Distribution kinds accepted by SetDistribution. */
namespace EUnrealMCPParticleDistributionKind
{
    inline const TCHAR* Constant       = TEXT("constant");
    inline const TCHAR* Uniform        = TEXT("uniform");
    inline const TCHAR* ConstantCurve  = TEXT("constant_curve");
    inline const TCHAR* UniformCurve   = TEXT("uniform_curve");
}

/** Slots a module can be added to. "modules" is the evaluated module list. */
namespace EUnrealMCPParticleSlot
{
    inline const TCHAR* Modules        = TEXT("modules");
    inline const TCHAR* Required       = TEXT("required");
    inline const TCHAR* TypeData       = TEXT("type_data");
    inline const TCHAR* Spawn          = TEXT("spawn");
    inline const TCHAR* EventGenerator = TEXT("event_generator");
}

/**
 * Check ids of validate_particle_system. Every id can be requested on its own; a typo comes back
 * as unknown_check with the full list as candidates.
 */
namespace EUnrealMCPParticleCheck
{
    inline const TCHAR* SpriteMaterial    = TEXT("sprite_material");
    inline const TCHAR* MeshMaterial      = TEXT("mesh_material");
    inline const TCHAR* LightSprite       = TEXT("light_sprite");
    inline const TCHAR* EmitterSpace      = TEXT("emitter_space");
    inline const TCHAR* LODStructure      = TEXT("lod_structure");
    inline const TCHAR* InheritedModules  = TEXT("inherited_modules");
    inline const TCHAR* MaterialUsage     = TEXT("material_usage");
    inline const TCHAR* AdditiveOpacity   = TEXT("additive_opacity");
}

/**
 * The single implementation of Cascade (UParticleSystem) reading and writing.
 *
 * Both entry points are thin shells over this class: the MCP command handlers parse JSON
 * and serialize the result structs, the python reflection library converts arguments and
 * hands the structs straight back. Neither owns any particle logic, so "MCP can do it,
 * python can't" and behavioural drift between the two are structurally impossible.
 *
 * The engine exports the building blocks (CreateLODLevel, SetToSensibleDefaults,
 * UpdateAllModuleLists, ...) but not the combinations: adding an emitter or a module is
 * implemented inside the unexported Cascade editor, so this kernel reimplements that
 * sequence (see Docs/MCP_Particle_Editing_Notes for the verified engine references).
 *
 * All functions must be called on the GameThread.
 */
class UNREALMCP_API FUnrealMCPParticleOps
{
public:
    // --- asset / element resolution -----------------------------------------

    /** Load the asset and check it is a UParticleSystem. */
    static bool ResolveParticleSystem(const FString& AssetPath, UParticleSystem*& OutSystem,
                                      FString& OutErrorCode, FString& OutErrorMessage);

    /** Resolve one emitter by index; candidates list the valid indices. */
    static bool ResolveEmitter(UParticleSystem* System, int32 EmitterIndex, UParticleEmitter*& OutEmitter,
                               FString& OutErrorCode, FString& OutErrorMessage, TArray<FString>& OutCandidates);

    /** Resolve one LOD level by index; candidates list the valid indices. */
    static bool ResolveLODLevel(UParticleEmitter* Emitter, int32 LODIndex, UParticleLODLevel*& OutLODLevel,
                                FString& OutErrorCode, FString& OutErrorMessage, TArray<FString>& OutCandidates);

    /** Resolve a module class by short name, with or without the ParticleModule prefix, or by path. */
    static bool ResolveModuleClass(const FString& ModuleClassName, UClass*& OutClass,
                                   FString& OutErrorCode, FString& OutErrorMessage, TArray<FString>& OutCandidates);

    /** Resolve an emitter class by short name (Sprite / Mesh / ...) or by path. */
    static bool ResolveEmitterClass(const FString& EmitterClassName, UClass*& OutClass,
                                    FString& OutErrorCode, FString& OutErrorMessage, TArray<FString>& OutCandidates);

    /**
     * Resolve one module inside an emitter.
     * ModuleIndex < 0 means "not given": a class that matches several instances then reports
     * ambiguous_module with every instance's index and LOD validity as candidates.
     * A Slot (from EUnrealMCPParticleSlot) restricts the search to that slot's field.
     */
    static bool ResolveModule(UParticleEmitter* Emitter, int32 LODIndex, const FString& ModuleClassName,
                              int32 ModuleIndex, const FString& Slot, UParticleModule*& OutModule,
                              int32& OutModuleIndex, FString& OutErrorCode, FString& OutErrorMessage,
                              TArray<FString>& OutCandidates);

    // --- reads ---------------------------------------------------------------

    /** Fill the full emitter tree. */
    static bool ListEmitters(UParticleSystem* System, TArray<FUnrealMCPParticleEmitterInfo>& OutEmitters,
                             FString& OutErrorCode, FString& OutErrorMessage, TArray<FString>& OutCandidates);

    /** Fill a single emitter's tree (used by list_particle_modules and by write responses). */
    static void MakeEmitterInfo(UParticleSystem* System, int32 EmitterIndex, FUnrealMCPParticleEmitterInfo& OutInfo);

    /** Fill one LOD level of an emitter, including its module list and cache arrays. */
    static void MakeLODInfo(UParticleSystem* System, UParticleEmitter* Emitter, int32 LODIndex,
                            FUnrealMCPParticleLODInfo& OutInfo);

    /** Fill one module instance (properties + distributions). */
    static void MakeModuleInfo(UParticleModule* Module, int32 ModuleIndex, const FString& Slot,
                               FUnrealMCPParticleModuleInfo& OutInfo);

    /** Distribution info of one FRawDistribution* property, or false when it is not one. */
    static bool MakeDistributionInfo(UParticleModule* Module, FProperty* Property,
                                     FUnrealMCPParticleDistributionInfo& OutInfo);

    /** The slot a resolved module actually occupies ("modules" for the module list). */
    static FString DetermineModuleSlot(UParticleEmitter* Emitter, int32 LODIndex, UParticleModule* Module);

    /** All distribution kinds this kernel accepts, for error candidates. */
    static TArray<FString> SupportedDistributionKinds();

    /** All module slots this kernel accepts, for error candidates. */
    static TArray<FString> SupportedModuleSlots();

    /**
     * One pass over the asset for the structural mistakes that never raise an error in the editor
     * (placeholder materials, a light emitter that still draws its sprite, leftover inherited
     * modules, LOD inconsistencies, material usage / compile errors, additive opacity wiring).
     * EmitterIndex < 0 checks every emitter; CheckIds empty runs every check. Read only.
     */
    static bool ValidateParticleSystem(const FString& AssetPath, int32 EmitterIndex,
                                       const TArray<FString>& CheckIds,
                                       FUnrealMCPParticleListResult& OutResult);

    /** All check ids, for error candidates. */
    static TArray<FString> SupportedChecks();

    // --- writes --------------------------------------------------------------

    static bool CreateParticleSystem(const FString& Name, const FString& Folder, const FString& EmitterClassName,
                                     int32 LODCount, FUnrealMCPParticleOpResult& OutResult);

    static bool AddEmitter(const FString& AssetPath, const FString& EmitterClassName, int32 InsertIndex,
                           int32 LODCount, FUnrealMCPParticleOpResult& OutResult);

    static bool RemoveEmitter(const FString& AssetPath, int32 EmitterIndex, FUnrealMCPParticleOpResult& OutResult);

    static bool SetLODCount(const FString& AssetPath, int32 EmitterIndex, int32 LODCount,
                            FUnrealMCPParticleOpResult& OutResult);

    static bool CopyLOD(const FString& AssetPath, int32 EmitterIndex, int32 SourceLODIndex, int32 InsertIndex,
                        FUnrealMCPParticleOpResult& OutResult);

    static bool SetLODDistance(const FString& AssetPath, int32 EmitterIndex, int32 LODIndex, float Distance,
                               FUnrealMCPParticleOpResult& OutResult);

    static bool AddModule(const FString& AssetPath, int32 EmitterIndex, const FString& ModuleClassName,
                          int32 LODIndex, int32 InsertIndex, const FString& Slot, bool bHideSprite,
                          FUnrealMCPParticleOpResult& OutResult);

    /**
     * Replace the spawn module's BurstList (pulsed / one-shot emission). An empty list clears it.
     * Values are written through the shared property writer, so count_low / time are validated
     * against FParticleBurst before anything is replaced.
     */
    static bool SetParticleBursts(const FString& AssetPath, int32 EmitterIndex, int32 LODIndex,
                                  const TArray<FUnrealMCPParticleBurst>& Bursts,
                                  FUnrealMCPParticleOpResult& OutResult);

    /**
     * Move a module inside its LOD's evaluation order (the order the engine evaluates modules in).
     * Slot modules (required / spawn / type data / event generator) cannot be moved.
     */
    static bool MoveModule(const FString& AssetPath, int32 EmitterIndex, const FString& ModuleClassName,
                           int32 ModuleIndex, int32 LODIndex, int32 ToIndex,
                           FUnrealMCPParticleOpResult& OutResult);

    /** Rename an emitter (UParticleEmitter::SetEmitterName), so several emitters can be told apart. */
    static bool SetEmitterName(const FString& AssetPath, int32 EmitterIndex, const FString& Name,
                               FUnrealMCPParticleOpResult& OutResult);

    /** Enable or disable one LOD level (LOD 0 must stay enabled). */
    static bool SetLODEnabled(const FString& AssetPath, int32 EmitterIndex, int32 LODIndex, bool bEnabled,
                              FUnrealMCPParticleOpResult& OutResult);

    /**
     * Copy an emitter (its LOD levels and modules, values included) into a new emitter appended to
     * the system. Modules are duplicated once and shared across the new LOD levels, the way the
     * engine's own emitters do it.
     */
    static bool DuplicateEmitter(const FString& AssetPath, int32 EmitterIndex, const FString& Name,
                                 FUnrealMCPParticleOpResult& OutResult);

    /**
     * Add several modules in one call. Every class is validated before anything is created, so an
     * unusable entry fails the whole batch without a trace.
     */
    static bool AddModules(const FString& AssetPath, int32 EmitterIndex, const TArray<FString>& ModuleClassNames,
                           int32 LODIndex, int32 InsertIndex, const FString& Slot, bool bHideSprite,
                           FUnrealMCPParticleOpResult& OutResult);

    /** Add the module set of another emitter of the same system (slot modules are not re-added). */
    static bool AddModulesFromEmitter(const FString& AssetPath, int32 EmitterIndex, int32 SourceEmitterIndex,
                                      int32 LODIndex, FUnrealMCPParticleOpResult& OutResult);

    static bool RemoveModule(const FString& AssetPath, int32 EmitterIndex, const FString& ModuleClassName,
                             int32 ModuleIndex, int32 LODIndex, FUnrealMCPParticleOpResult& OutResult);

    static bool SetModuleProperty(const FString& AssetPath, int32 EmitterIndex, const FString& ModuleClassName,
                                  int32 ModuleIndex, int32 LODIndex, const FString& PropertyName,
                                  const TSharedPtr<FJsonValue>& Value, FUnrealMCPParticleOpResult& OutResult);

    static bool SetDistribution(const FString& AssetPath, int32 EmitterIndex, const FString& ModuleClassName,
                                int32 ModuleIndex, int32 LODIndex, const FString& PropertyName, const FString& Kind,
                                const TArray<float>& Values, const TArray<FUnrealMCPParticleDistributionKey>& Keys,
                                FUnrealMCPParticleOpResult& OutResult);

    // --- shared response helpers ---------------------------------------------

    /** Fill AssetPath / Emitters on a success result so the caller can assert on real state. */
    static void FillEmitterReadback(UParticleSystem* System, FUnrealMCPParticleOpResult& OutResult);

    /** Fill the error fields of a result. */
    static void Fail(FUnrealMCPParticleOpResult& OutResult, const FString& ErrorCode, const FString& ErrorMessage,
                     const TArray<FString>& Candidates = TArray<FString>(),
                     const TArray<FString>& AvailableFields = TArray<FString>());
};
