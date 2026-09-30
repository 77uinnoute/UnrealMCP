#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UnrealMCPParticleLibrary.generated.h"

class UParticleSystem;

/**
 * One editable UPROPERTY of a particle module, as seen by python.
 */
USTRUCT(BlueprintType)
struct FUnrealMCPParticlePropertyInfo
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString Name;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString Type;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString Value;

    /** Unit of the value when the engine does not make it obvious (may be empty). */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString Units;

    /** How to read the value (may be empty). */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString Hint;
};

/**
 * One module added by a batch add: its class and the index it landed on in the requested LOD.
 */
USTRUCT(BlueprintType)
struct FUnrealMCPParticleAddedModule
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString ModuleClass;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    int32 ModuleIndex = -1;
};

/**
 * One entry of a spawn module's BurstList: Count particles emitted at Time. CountLow is the
 * lower bound of the random count (-1 means "use Count").
 */
USTRUCT(BlueprintType)
struct FUnrealMCPParticleBurst
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    int32 Count = 1;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    int32 CountLow = -1;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    float Time = 0.0f;
};

/**
 * One key of a distribution curve. Value holds the flattened curve components:
 * 1 entry for a constant curve, 2 for a uniform curve (min, max), 3 for a vector
 * constant curve (x, y, z) and 6 for a vector uniform curve (min xyz, max xyz).
 */
USTRUCT(BlueprintType)
struct FUnrealMCPParticleDistributionKey
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    float Time = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    TArray<float> Value;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString Interp;
};

/**
 * The authored distribution behind a FRawDistributionFloat / FRawDistributionVector property:
 * its kind, its class and the values it currently holds.
 */
USTRUCT(BlueprintType)
struct FUnrealMCPParticleDistributionInfo
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString PropertyName;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString Kind;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString DistributionClass;

    /** kind == "constant": the constant (3 entries for a vector distribution). */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    TArray<float> Constants;

    /** kind == "uniform": low/high (3 + 3 entries for a vector distribution). */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    TArray<float> MinMax;

    /** kind == "*_curve": the authored keyframes. */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    TArray<FUnrealMCPParticleDistributionKey> Keys;

    /** Value of the raw distribution at time 0 (read back from the baked lookup table). */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    TArray<float> SampledValue;

    /** Unit of the value when the engine does not make it obvious (may be empty). */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString Units;

    /** How to read the value (may be empty). */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString Hint;
};

/**
 * One module instance. Modules are shared across the LOD levels of an emitter:
 * LODValidity is the bitmask of the LOD indices that use this instance.
 */
USTRUCT(BlueprintType)
struct FUnrealMCPParticleModuleInfo
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    int32 ModuleIndex = -1;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString ModuleClass;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString ModuleName;

    /**
     * Subobject path of the module instance ("<package.asset>:<subobject name>"). A struct or
     * array property that the writer cannot express can still be set from python with
     * unreal.load_object(None, object_path) + set_editor_property (C++ property names only).
     */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString ObjectPath;

    /** "modules" (list slot), "required", "type_data", "spawn" or "event_generator". */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString Slot;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    int32 LODValidity = 0;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    TArray<int32> ActiveLods;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    bool Editable = true;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    TArray<FUnrealMCPParticlePropertyInfo> Properties;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    TArray<FUnrealMCPParticleDistributionInfo> Distributions;

    /** The live module object (python can drive it further through its own tools). */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    UObject* Module = nullptr;
};

/**
 * One LOD level of an emitter: the module list plus the derived cache arrays the
 * runtime actually evaluates (they must stay in sync with Modules).
 */
USTRUCT(BlueprintType)
struct FUnrealMCPParticleLODInfo
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    int32 LODIndex = -1;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    int32 Level = 0;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    int32 ModuleCount = 0;

    /** UParticleLODLevel::bEnabled: a disabled level is kept but not evaluated. */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    bool Enabled = true;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString RequiredModuleClass;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString TypeDataModuleClass;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString SpawnModuleClass;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString EventGeneratorClass;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    TArray<FString> SpawningModuleClasses;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    TArray<FString> SpawnModuleClasses;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    TArray<FString> UpdateModuleClasses;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    TArray<FUnrealMCPParticleModuleInfo> Modules;
};

/**
 * One emitter of a particle system.
 */
USTRUCT(BlueprintType)
struct FUnrealMCPParticleEmitterInfo
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    int32 EmitterIndex = -1;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString EmitterClass;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString EmitterName;

    /** Subobject path of the emitter ("<package.asset>:<subobject name>"). */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString ObjectPath;

    /** The live emitter object (python can drive it further through its own tools). */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    UObject* Emitter = nullptr;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    int32 LODCount = 0;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    TArray<FUnrealMCPParticleLODInfo> Lods;
};

/**
 * One result line of validate_particle_system: what was inspected, what was found and what to do
 * about it. A check that found nothing worth reporting still shows up with Severity "info".
 */
USTRUCT(BlueprintType)
struct FUnrealMCPParticleCheck
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString Id;

    /** "error" | "warning" | "info". */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString Severity;

    /** Where the finding is: "emitter[1].lod[0].TypeDataMesh". */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString Target;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString Message;

    /** The next action, as a command or parameter, so the finding is actionable. */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString FixHint;
};

/**
 * Result of a read: the whole emitter tree, or a slice of it.
 */
USTRUCT(BlueprintType)
struct FUnrealMCPParticleListResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    bool Success = false;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString ErrorCode;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString ErrorMessage;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    TArray<FString> Candidates;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString AssetPath;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    int32 EmitterCount = 0;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    TArray<FUnrealMCPParticleEmitterInfo> Emitters;

    /** validate_particle_system only: the findings, one entry per check that ran. */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    TArray<FUnrealMCPParticleCheck> Checks;
};

/**
 * Result of a write: what was done, what the value was before and after, and the
 * (possibly changed) emitter tree for the caller to assert on.
 */
USTRUCT(BlueprintType)
struct FUnrealMCPParticleOpResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    bool Success = false;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString ErrorCode;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString ErrorMessage;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    TArray<FString> Candidates;

    /**
     * Writable fields of the struct whose field name could not be resolved (empty otherwise),
     * so a rejected struct write is self-correctable.
     */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    TArray<FString> AvailableFields;

    /** Position of the container element that was refused (-1 when the write was not element-wise). */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    int32 FailedIndex = -1;

    /** True when a refused write left the target value untouched (the atomicity promise). */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    bool bUnchanged = false;

    /** Set when PropertyName was resolved through an alias: the name the caller passed. */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString ResolvedFrom;

    /** Unit of the written property (may be empty). */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString Units;

    /** How to read the written property (may be empty). */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString Hint;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString AssetPath;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    int32 EmitterIndex = -1;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    int32 LODIndex = -1;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString ModuleClass;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    int32 ModuleIndex = -1;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString PropertyName;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString PropertyType;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString ValueBefore;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString ValueAfter;

    /** Slots that had to be cleared because the removed module occupied them. */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    TArray<FString> ClearedSlots;

    /** Number of entries after a BurstList write. */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    int32 BurstCount = -1;

    /** True when hide_sprite applied: the light emitter draws no sprite of its own. */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    bool SpriteHidden = false;

    /** Readback of Required.bUseMaxDrawCount after a hide_sprite add. */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    bool bUseMaxDrawCount = false;

    /** Readback of Required.MaxDrawCount after a hide_sprite add (-1 when not read back). */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    int32 MaxDrawCount = -1;

    /** Module position before / after a move (-1 when the command was not a move). */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    int32 FromIndex = -1;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    int32 ToIndex = -1;

    /** Module class names in evaluation order after a move. */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    TArray<FString> ModuleOrder;

    /** Modules added by one batch add, in insertion order. */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    TArray<FUnrealMCPParticleAddedModule> AddedModules;

    /** Emitter name before / after a rename or a duplicate (empty when not applicable). */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString NameBefore;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FString NameAfter;

    /** LOD enable state after set_particle_lod_enabled (BEnabledSet says whether it was read). */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    bool LODEnabled = false;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    bool bLODEnabledSet = false;

    /** Modules per LOD of a duplicated emitter. */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    TArray<int32> ModuleCounts;

    /** Emitters closed because the command was called with auto_close=true. */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    TArray<FString> ClosedEditors;

    /**
     * Emitters that had to be resized to keep every emitter's LOD count equal (the engine
     * enforces that invariant when it loads a particle system).
     */
    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    TArray<int32> AdjustedEmitters;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    FUnrealMCPParticleModuleInfo Module;

    UPROPERTY(BlueprintReadOnly, Category = "UnrealMCP|Particle")
    TArray<FUnrealMCPParticleEmitterInfo> Emitters;
};

/**
 * Python reflection surface for Cascade (UParticleSystem) editing.
 *
 * Exists for the same reason the blueprint graph library does: the data the tools
 * need is not reflected. UParticleSystem::Emitters, UParticleEmitter::LODLevels and
 * UParticleLODLevel::Modules are UPROPERTY(instanced) without EditAnywhere, so editor
 * python cannot read them; and there is no exported add/remove helper for emitters or
 * modules anywhere in the engine. This library returns typed USTRUCTs and wraps the
 * same kernel (FUnrealMCPParticleOps) the MCP commands use, so both entry points agree
 * on state and error codes by construction.
 *
 * All functions must be called on the GameThread.
 */
UCLASS()
class UNREALMCP_API UUnrealMCPParticleLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    // --- reads ---------------------------------------------------------------

    /** Every emitter of the particle system, with each LOD's module list. */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Particle")
    static FUnrealMCPParticleListResult ListParticleEmitters(UObject* Asset);

    /** One emitter's LOD, with its module list and derived cache arrays. */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Particle")
    static FUnrealMCPParticleListResult ListParticleModules(UObject* Asset, int32 EmitterIndex, int32 LODIndex);

    /** One module: its properties and its distributions. */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Particle")
    static FUnrealMCPParticleOpResult GetParticleModule(UObject* Asset, int32 EmitterIndex, const FString& ModuleClass,
                                                       int32 ModuleIndex, int32 LODIndex);

    /** Structural health pass; EmitterIndex < 0 covers every emitter, empty CheckIds every check. */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Particle")
    static FUnrealMCPParticleListResult ValidateParticleSystem(UObject* Asset, int32 EmitterIndex,
                                                               const TArray<FString>& CheckIds);

    // --- writes --------------------------------------------------------------

    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Particle")
    static FUnrealMCPParticleOpResult CreateParticleSystem(const FString& Name, const FString& Folder,
                                                          const FString& EmitterClass, int32 LODCount);

    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Particle")
    static FUnrealMCPParticleOpResult AddParticleEmitter(const FString& AssetPath, const FString& EmitterClass,
                                                        int32 InsertIndex, int32 LODCount);

    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Particle")
    static FUnrealMCPParticleOpResult RemoveParticleEmitter(const FString& AssetPath, int32 EmitterIndex);

    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Particle")
    static FUnrealMCPParticleOpResult SetParticleLODCount(const FString& AssetPath, int32 EmitterIndex, int32 LODCount);

    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Particle")
    static FUnrealMCPParticleOpResult CopyParticleLOD(const FString& AssetPath, int32 EmitterIndex,
                                                     int32 SourceLODIndex, int32 InsertIndex);

    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Particle")
    static FUnrealMCPParticleOpResult SetParticleLODDistance(const FString& AssetPath, int32 EmitterIndex,
                                                            int32 LODIndex, float Distance);

    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Particle")
    static FUnrealMCPParticleOpResult AddParticleModule(const FString& AssetPath, int32 EmitterIndex,
                                                       const FString& ModuleClass, int32 LODIndex,
                                                       int32 InsertIndex, const FString& Slot, bool bHideSprite);

    /**
     * Replace the spawn module's BurstList. An empty array clears it; HideSprite sets the required
     * module so a light emitter draws no sprite of its own.
     */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Particle")
    static FUnrealMCPParticleOpResult SetParticleBursts(const FString& AssetPath, int32 EmitterIndex, int32 LODIndex,
                                                       const TArray<FUnrealMCPParticleBurst>& Bursts);

    /** Add several modules in one call, in the order given (all-or-nothing). */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Particle")
    static FUnrealMCPParticleOpResult AddParticleModules(const FString& AssetPath, int32 EmitterIndex,
                                                        const TArray<FString>& ModuleClasses, int32 LODIndex,
                                                        int32 InsertIndex, const FString& Slot, bool bHideSprite);

    /** Add the module set of another emitter of the same system. */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Particle")
    static FUnrealMCPParticleOpResult AddParticleModulesFromEmitter(const FString& AssetPath, int32 EmitterIndex,
                                                                   int32 SourceEmitterIndex, int32 LODIndex);

    /** Move a module inside its LOD's evaluation order. */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Particle")
    static FUnrealMCPParticleOpResult MoveParticleModule(const FString& AssetPath, int32 EmitterIndex,
                                                        const FString& ModuleClass, int32 ModuleIndex,
                                                        int32 LODIndex, int32 ToIndex);

    /** Rename an emitter so several emitters can be told apart. */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Particle")
    static FUnrealMCPParticleOpResult SetParticleEmitterName(const FString& AssetPath, int32 EmitterIndex,
                                                            const FString& Name);

    /** Enable or disable one LOD level (LOD 0 has to stay enabled). */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Particle")
    static FUnrealMCPParticleOpResult SetParticleLODEnabled(const FString& AssetPath, int32 EmitterIndex,
                                                           int32 LODIndex, bool bEnabled);

    /** Copy an emitter (LOD levels and modules, values included) to a new emitter of the system. */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Particle")
    static FUnrealMCPParticleOpResult DuplicateParticleEmitter(const FString& AssetPath, int32 EmitterIndex,
                                                              const FString& Name);

    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Particle")
    static FUnrealMCPParticleOpResult RemoveParticleModule(const FString& AssetPath, int32 EmitterIndex,
                                                          const FString& ModuleClass, int32 ModuleIndex,
                                                          int32 LODIndex);

    /**
     * Write one ordinary UPROPERTY of a module. Value is parsed according to ValueKind
     * ("auto" / "number" / "bool" / "string" / "name" / "text" / "object" / "class" / ...),
     * the same set the graph library accepts.
     */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Particle")
    static FUnrealMCPParticleOpResult SetParticleModuleProperty(const FString& AssetPath, int32 EmitterIndex,
                                                               const FString& ModuleClass, int32 ModuleIndex,
                                                               int32 LODIndex, const FString& PropertyName,
                                                               const FString& Value, const FString& ValueKind);

    /**
     * Write a FRawDistributionFloat / FRawDistributionVector property.
     * Kind is one of "constant" / "uniform" / "constant_curve" / "uniform_curve";
     * Values holds the constants or the min/max pair, Keys the curve keyframes.
     */
    UFUNCTION(BlueprintCallable, Category = "UnrealMCP|Particle")
    static FUnrealMCPParticleOpResult SetParticleDistribution(const FString& AssetPath, int32 EmitterIndex,
                                                             const FString& ModuleClass, int32 ModuleIndex,
                                                             int32 LODIndex, const FString& PropertyName,
                                                             const FString& Kind, const TArray<float>& Values,
                                                             const TArray<FUnrealMCPParticleDistributionKey>& Keys);
};
