#pragma once

#include "CoreMinimal.h"

class UNiagaraSystem;
struct FNiagaraEmitterHandle;

/** One emitter handle of a system, as reported by list_niagara_emitters / add_niagara_emitter. */
struct FUnrealMCPNiagaraEmitterInfo
{
    FString Id;
    FString Name;
    bool bEnabled = true;
    /** Object path of the emitter subobject that the handle points at. */
    FString EmitterPath;
    /** Number of enabled simulation stages on the emitter (GPU stage count in practice). */
    int32 SimulationStageCount = 0;
    /** Names of the emitter's simulation stages, in stack order. */
    TArray<FString> SimulationStageNames;
    /** Sim target of the emitter: "CPUSim" / "GPUComputeSim" / "Unknown". */
    FString SimTarget;
    // Simulator properties that decide whether a Grid2D / RenderTarget based effect can run at all.
    bool bLocalSpace = false;
    bool bDeterminism = false;
    int32 RandomSeed = 0;
    bool bInterpolatedSpawning = false;
    int32 PreAllocationCount = 0;
    /** Renderer classes on the emitter, in stack order (Sprite / Mesh / Ribbon / ...). */
    TArray<FString> RendererClasses;
};

/** Emitter simulator properties the caller asked to change (only the set ones are applied). */
struct FUnrealMCPNiagaraEmitterEdits
{
    bool bSimTarget = false;
    FString SimTarget;
    bool bLocalSpace = false;
    bool LocalSpace = false;
    bool bDeterminism = false;
    bool Determinism = false;
    bool bRandomSeed = false;
    int32 RandomSeed = 0;
    bool bInterpolatedSpawning = false;
    bool InterpolatedSpawning = false;
    bool bPreAllocationCount = false;
    int32 PreAllocationCount = 0;
};

/** One user parameter of a system: name (with the "User." namespace), Niagara type, current value. */
struct FUnrealMCPNiagaraParameterInfo
{
    FString Name;
    FString Type;
    FString Value;
};

/** One stack module (UNiagaraNodeFunctionCall) of an emitter stage. */
struct FUnrealMCPNiagaraModuleInfo
{
    FString Guid;
    FString Name;
    /** SystemSpawn / SystemUpdate / EmitterSpawn / EmitterUpdate / ParticleSpawn / ParticleUpdate. */
    FString Stage;
    bool bEnabled = true;
    /** Object path of the module script the node calls (empty for a scratch-pad module). */
    FString ScriptPath;
    /** Node position in the graph, so a caller can tell stack order apart from graph layout. */
    int32 PosX = 0;
    int32 PosY = 0;
    /**
     * Position within the stage's evaluation order, read by walking the stage's parameter-map chain
     * back from its output node. -1 when the node is not on a stage chain (e.g. a nested call).
     */
    int32 Order = -1;
};

/** One compile diagnostic, read back from the script's own last-compile events. */
struct FUnrealMCPNiagaraCompileMessage
{
    /** "error" / "warning" / "log" / "display". */
    FString Severity;
    /** Script the event came from, so a per-script failure is attributable. */
    FString ScriptPath;
    FString Message;
    FString NodeGuid;
};

/** One node of a scratch-pad graph. */
struct FUnrealMCPNiagaraNodeInfo
{
    FString Guid;
    /** CustomHlsl / Op / MapGet / MapSet / If / Input / Output / FunctionCall / <class name>. */
    FString Type;
    FString Name;
    int32 PosX = 0;
    int32 PosY = 0;
};

/** One pin of a scratch-pad node. */
struct FUnrealMCPNiagaraPinInfo
{
    FString Name;
    FString Direction;
    FString Type;
    /** Owning node, so a whole-graph pin dump stays attributable. */
    FString NodeGuid;
    bool bConnected = false;
};

/** One pin the caller asked set_niagara_scratch_pad_pins to declare. */
struct FUnrealMCPNiagaraPinSpec
{
    FString Name;
    /** "input" / "output" (input also accepts "Input"). */
    FString Direction;
    /** float / int / bool / vec2 / vec3 / vec4 / position / color / ArrayFloat / Grid2D / ... */
    FString Type;
};

/** One attribute of a Grid2DCollection, as the engine's own FindAttributes reports it. */
struct FUnrealMCPNiagaraGridAttributeInfo
{
    FString Name;
    FString Type;
    /** First slice the attribute occupies in the grid texture - the tile an RT copy writes it to. */
    int32 FirstSlice = 0;
    /** Floats per cell: 1 for a float attribute, 3 for a vec3, ... */
    int32 Channels = 1;
};

/** One value read back from a system's last compile, per script. */
struct FUnrealMCPNiagaraScriptStatus
{
    FString ScriptPath;
    FString Usage;
    FString Status;
    int32 ErrorCount = 0;
    int32 WarningCount = 0;
};

/**
 * Result of one Niagara operation.
 *
 * One flat struct for the whole domain, the same shape the Cascade particle ops use: the command
 * layer is the only place that knows about JSON, so everything here stays typed and the JSON
 * shaping happens in one place.
 */
struct FUnrealMCPNiagaraResult
{
    bool bSuccess = false;
    FString ErrorCode;
    FString ErrorMessage;
    /** Legal values, for an error about a name that did not resolve. */
    TArray<FString> Candidates;

    FString AssetPath;
    FString EmitterId;
    FString EmitterName;
    FString ParamName;
    FString ModuleGuid;
    FString ModuleName;
    FString NodeGuid;
    FString ScriptPath;
    FString Value;
    FString ValueType;
    FString HlslCode;
    FString CompileStatus;
    FString ActorName;
    FString ActorPath;

    bool bEnabled = true;
    bool bConnected = false;
    bool bPersisted = false;
    /** Name of the rapid iteration parameter a module input write landed in (the readback source). */
    FString RapidIterationParameter;
    /** Node count of the scratch graph after a structural edit, so the caller sees the new state. */
    int32 RemainingNodes = 0;
    /** Grid2D attribute table: attributes found, the collection's slice count, total channels. */
    int32 GridAttributeCount = 0;
    int32 GridSlotCount = 0;
    int32 GridChannelCount = 0;
    /**
     * The grid DI's editor preview settings, read back from the asset (not from a running
     * instance's Vars). Kept in dedicated fields because the domain's generic FString Value /
     * bool bConnected are multi-purpose and cannot carry this meaning on their own.
     */
    bool bPreviewGridEnabled = false;
    FString PreviewAttribute;

    TArray<FUnrealMCPNiagaraEmitterInfo> Emitters;
    TArray<FUnrealMCPNiagaraParameterInfo> Parameters;
    TArray<FUnrealMCPNiagaraGridAttributeInfo> GridAttributes;
    /** Warnings the engine's FindAttributes raised while building the attribute table. */
    TArray<FString> GridAttributeWarnings;
    TArray<FUnrealMCPNiagaraModuleInfo> Modules;
    TArray<FUnrealMCPNiagaraCompileMessage> CompileMessages;
    TArray<FUnrealMCPNiagaraScriptStatus> ScriptStatuses;
    TArray<FUnrealMCPNiagaraNodeInfo> Nodes;
    TArray<FUnrealMCPNiagaraPinInfo> Pins;
    /**
     * Conversion nodes the graph schema inserted on its own while connecting. A connect can succeed
     * and still leave the graph uncompilable when the schema bridges two types it cannot translate
     * (a Position/LWC endpoint is the known case), so the caller is told about them.
     */
    TArray<FString> InsertedConversionNodes;
};

/**
 * Niagara asset / stack / scratch-pad operations.
 *
 * Cross-module note (this is why the code looks the way it does): NiagaraEditor only exports part
 * of FNiagaraStackGraphUtilities. AddScriptModuleToStack, SetModuleIsEnabled,
 * GetOrCreateStackFunctionInputOverridePin, SetLinkedValueHandleForFunctionInput,
 * UEdGraphSchema_Niagara::TypeDefinitionToPinType and UNiagaraGraph::FindEquivalentOutputNode are
 * exported; RemoveModuleFromStack, ResetGraphForOutput, FindOutputNode(s), the MapGet/MapSet
 * headers, UNiagaraNodeCustomHlsl::SetCustomHlsl and UNiagaraNodeWithDynamicPins::AddParameterPin
 * are NOT. Everything that needs one of the latter goes through a documented workaround
 * (class lookup by path, UEdGraph reflection, pin creation through the exported schema).
 */
class UNREALMCP_API FUnrealMCPNiagaraOps
{
public:
    // ------------------------------------------------------------------
    // Helpers
    // ------------------------------------------------------------------
    static void Fail(FUnrealMCPNiagaraResult& Out, const FString& ErrorCode, const FString& ErrorMessage,
                     const TArray<FString>& Candidates = TArray<FString>());

    /** Load a UNiagaraSystem by asset path (errors: invalid_params / asset_not_found / not_a_niagara_system). */
    static bool ResolveSystem(const FString& AssetPath, UNiagaraSystem*& OutSystem, FUnrealMCPNiagaraResult& Out);

    /** Resolve an emitter handle by handle id (GUID text) or by emitter name (errors: emitter_not_found). */
    static bool ResolveEmitter(UNiagaraSystem* System, const FString& IdOrName,
                               FNiagaraEmitterHandle*& OutHandle, FUnrealMCPNiagaraResult& Out);

    // ------------------------------------------------------------------
    // Asset lifecycle
    // ------------------------------------------------------------------
    static bool CreateSystem(const FString& Name, const FString& Folder, FUnrealMCPNiagaraResult& Out);
    static bool DuplicateSystem(const FString& SourcePath, const FString& DestinationPath, FUnrealMCPNiagaraResult& Out);

    // ------------------------------------------------------------------
    // Emitters
    // ------------------------------------------------------------------
    static bool ListEmitters(const FString& AssetPath, FUnrealMCPNiagaraResult& Out);
    static bool AddEmitter(const FString& AssetPath, const FString& EmitterName, const FString& SourceEmitterPath,
                           bool bAddDefaultModules, FUnrealMCPNiagaraResult& Out);
    static bool RemoveEmitter(const FString& AssetPath, const FString& EmitterIdOrName, FUnrealMCPNiagaraResult& Out);
    /**
     * Simulator properties of an emitter (sim target, local space, determinism, ...). Sim target is
     * the one that decides whether a Grid2D / RenderTarget based effect can run at all.
     */
    static bool SetEmitterProperties(const FString& AssetPath, const FString& EmitterIdOrName,
                                     const FUnrealMCPNiagaraEmitterEdits& Edits, FUnrealMCPNiagaraResult& Out);
    /** Bind a material to one of the emitter's renderers (the reflection path, no hard-coded names). */
    static bool SetRendererMaterial(const FString& AssetPath, const FString& EmitterIdOrName,
                                    const FString& MaterialPath, int32 RendererIndex, FUnrealMCPNiagaraResult& Out);
    /**
     * Add a renderer to an emitter ("Sprite" / "Mesh" / "Ribbon" / "Light" / "Decal" / "Component"),
     * optionally binding a material to it.
     *
     * The renderer array (FVersionedNiagaraEmitterData::RendererProperties) is private, but
     * UNiagaraEmitter::AddRenderer is NIAGARA_API - so the object is created here and handed to the
     * emitter's own add. A renderer that is never added is never drawn: this is what makes a
     * simulation-visible system (a GPU grid solver, a PBF particle pool) actually show up.
     */
    static bool AddRenderer(const FString& AssetPath, const FString& EmitterIdOrName,
                            const FString& RendererType, const FString& MaterialPath,
                            FUnrealMCPNiagaraResult& Out);
    /** Remove one renderer by index (the index is what list_niagara_emitters reports the order as). */
    static bool RemoveRenderer(const FString& AssetPath, const FString& EmitterIdOrName,
                               int32 RendererIndex, FUnrealMCPNiagaraResult& Out);
    /**
     * Add a simulation stage (GPU iteration) to an emitter.
     *
     * The editor's own path needs FNiagaraStackGraphUtilities::ResetGraphForOutput, which is not
     * exported, and UNiagaraEmitter::AddSimulationStage, which is not either - so the stage object and
     * its script are created here, the script's usage id (the stage's merge id) is what the compiler
     * reads back, the private SimulationStages array is appended to by reflection, and the stage's
     * output node (usage ParticleSimulationStageScript + that id) is built by hand.
     */
    static bool AddSimulationStage(const FString& AssetPath, const FString& EmitterIdOrName,
                                   const FString& StageName, const FString& IterationSource, bool bEnabled,
                                   FUnrealMCPNiagaraResult& Out);
    /**
     * Configure an existing simulation stage.
     *
     * Two of its properties are out of the generic writer's reach, which is what this command is for:
     * `bEnabled` is a bitfield (the reflector reports it read-only; the exported
     * UNiagaraSimulationStageBase::SetEnabled is used here), and NumIterations keeps its number in a
     * raw byte blob (written through the exported SetDefaultValueEditorOnly). The iteration source and
     * the bound DataInterface parameter are semantic here: the parameter is resolved to a real
     * DataInterface user parameter instead of a hand-built variable.
     */
    static bool SetSimulationStage(const FString& AssetPath, const FString& EmitterIdOrName,
                                   const FString& StageRef, const FString& StageName, bool bSetEnabled,
                                   bool bEnabled, const FString& IterationSource, bool bSetNumIterations,
                                   int32 NumIterations, const FString& DataInterfaceParameter,
                                   FUnrealMCPNiagaraResult& Out);

    // ------------------------------------------------------------------
    // Compile
    // ------------------------------------------------------------------
    static bool CompileSystem(const FString& AssetPath, bool bWait, FUnrealMCPNiagaraResult& Out);

    // ------------------------------------------------------------------
    // User parameters
    // ------------------------------------------------------------------
    static bool GetParameters(const FString& AssetPath, FUnrealMCPNiagaraResult& Out);
    static bool SetParameter(const FString& AssetPath, const FString& ParamName, const FString& ParamType,
                             const FString& Value, bool bAddIfMissing, FUnrealMCPNiagaraResult& Out);
    /**
     * Create or configure a DataInterface user parameter (a Grid2D collection, a render target, an
     * array...), which is what a GPU simulation stage reads and writes.
     *
     * With no property map this is a read: it reports the DataInterface's editable properties so a
     * caller does not have to guess names. Property values go through reflection by name.
     */
    static bool SetDataInterfaceProperties(const FString& AssetPath, const FString& ParamName,
                                           const FString& DataInterfaceType,
                                           const TArray<TPair<FString, FString>>& Properties,
                                           FUnrealMCPNiagaraResult& Out);
    /**
     * The attribute table of a Grid2DCollection user parameter: name, type, first slice and channel
     * count per attribute, plus the collection's own slot count and the warnings the engine raised.
     *
     * A Grid2D an RT receives is written as one slice per attribute, in the order the engine's
     * FindAttributes discovers them (the emitters' GPU compute scripts, in generated-function order) -
     * neither the parameter list nor the scratch-pad pins carry that order, so this is the only
     * authoritative index <-> name map. It is also where the editor's repeated
     * "Failed to map PreviewAttribute <name> to a grid index" warning comes from.
     */
    static bool ListGridAttributes(const FString& AssetPath, const FString& ParamName,
                                   FUnrealMCPNiagaraResult& Out);

    // ------------------------------------------------------------------
    // Stack modules
    // ------------------------------------------------------------------
    static bool ListModules(const FString& AssetPath, const FString& EmitterName, const FString& Stage,
                            FUnrealMCPNiagaraResult& Out);
    static bool AddModule(const FString& AssetPath, const FString& EmitterName, const FString& Stage,
                          const FString& ModuleAssetPath, int32 InsertIndex, FUnrealMCPNiagaraResult& Out);
    static bool SetModuleEnabled(const FString& AssetPath, const FString& EmitterName, const FString& ModuleGuidOrName,
                                 bool bEnabled, bool bSystemLevel, FUnrealMCPNiagaraResult& Out);
    /**
     * Write one input of a stack module (the literal value, stored as the module's rapid iteration
     * parameter). Goes through the editor's own FNiagaraStackFunctionInputBinder, so the graph gets
     * the same override nodes the stack UI would create.
     */
    static bool SetModuleInput(const FString& AssetPath, const FString& EmitterName, const FString& ModuleRef,
                               const FString& InputName, const FString& Value, const FString& ParamType,
                               FUnrealMCPNiagaraResult& Out);
    /**
     * Remove a module from the stack, with the parameter-map override nodes the stack put around it.
     *
     * FNiagaraStackGraphUtilities::RemoveModuleFromStack and its helpers (GetStackNodeGroups,
     * DisconnectStackNodeGroup) are not exported, so the group is identified here by walking the
     * parameter-map chain backwards: only an override node whose pins all name this very module is
     * taken with it. Every removed node is reported, and a compile follows.
     */
    static bool RemoveModule(const FString& AssetPath, const FString& EmitterName, const FString& ModuleRef,
                             bool bSystemLevel, FUnrealMCPNiagaraResult& Out);

    // ------------------------------------------------------------------
    // Scratch-pad graphs
    // ------------------------------------------------------------------
    static bool CreateScratchPadModule(const FString& AssetPath, const FString& EmitterName, const FString& Stage,
                                       const FString& ModuleName, FUnrealMCPNiagaraResult& Out);
    static bool ListScratchPadNodes(const FString& AssetPath, const FString& EmitterName, const FString& ModuleRef,
                                    FUnrealMCPNiagaraResult& Out);
    static bool SetCustomHlsl(const FString& AssetPath, const FString& EmitterName, const FString& ModuleRef,
                              const FString& NodeGuid, const FString& Code, FUnrealMCPNiagaraResult& Out);
    static bool SetScratchPadPins(const FString& AssetPath, const FString& EmitterName, const FString& ModuleRef,
                                  const FString& NodeGuid, const TArray<FUnrealMCPNiagaraPinSpec>& Pins,
                                  FUnrealMCPNiagaraResult& Out);
    static bool RemoveScratchPadNode(const FString& AssetPath, const FString& EmitterName, const FString& ModuleRef,
                                     const FString& NodeGuid, FUnrealMCPNiagaraResult& Out);
    static bool RemoveScratchPadPin(const FString& AssetPath, const FString& EmitterName, const FString& ModuleRef,
                                    const FString& NodeGuid, const FString& PinName, const FString& Direction,
                                    FUnrealMCPNiagaraResult& Out);
    static bool ConnectScratchPadPins(const FString& AssetPath, const FString& EmitterName, const FString& ModuleRef,
                                      const FString& FromNode, const FString& FromPin,
                                      const FString& ToNode, const FString& ToPin, FUnrealMCPNiagaraResult& Out);

    // ------------------------------------------------------------------
    // Level actor + component parameters
    // ------------------------------------------------------------------
    static bool SpawnNiagaraActor(const FString& SystemPath, const FString& ActorName, const FVector& Location,
                                  const FRotator& Rotation, bool bAutoActivate, FUnrealMCPNiagaraResult& Out);
    static bool SetComponentParameter(const FString& ActorName, const FString& ParamName, const FString& ParamType,
                                      const FString& Value, FUnrealMCPNiagaraResult& Out);
};
