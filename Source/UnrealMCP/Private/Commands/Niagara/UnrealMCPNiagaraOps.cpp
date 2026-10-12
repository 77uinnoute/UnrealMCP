#include "Commands/Niagara/UnrealMCPNiagaraOps.h"

#include "Commands/Common/UnrealMCPCommonUtils.h"
#include "Commands/Niagara/UnrealMCPNiagaraFactories.h"

// Niagara runtime
#include "NiagaraActor.h"
#include "NiagaraCommon.h"
#include "NiagaraComponent.h"
#include "NiagaraDataInterface.h"
#include "NiagaraDataInterfaceGrid2DCollection.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraParameterStore.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraScriptSource.h"
#include "NiagaraScriptSourceBase.h"
#include "NiagaraScratchPadContainer.h"
#include "NiagaraShared.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "NiagaraUserRedirectionParameterStore.h"

// Niagara editor. Only part of this surface is NIAGARAEDITOR_API - see UnrealMCPNiagaraOps.h.
#include "EdGraphSchema_Niagara.h"
#include "NiagaraEditorSettings.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeWithDynamicPins.h"
#include "NiagaraParameterMapHistory.h"
#include "NiagaraScriptSource.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraStackFunctionInputBinder.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

// Editor / engine
#include "Algo/Reverse.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace
{
    const TCHAR* const StageSystemSpawn = TEXT("SystemSpawn");
    const TCHAR* const StageSystemUpdate = TEXT("SystemUpdate");
    const TCHAR* const StageEmitterSpawn = TEXT("EmitterSpawn");
    const TCHAR* const StageEmitterUpdate = TEXT("EmitterUpdate");
    const TCHAR* const StageParticleSpawn = TEXT("ParticleSpawn");
    const TCHAR* const StageParticleUpdate = TEXT("ParticleUpdate");

    TArray<FString> StageCandidates()
    {
        return TArray<FString>{ StageSystemSpawn, StageSystemUpdate, StageEmitterSpawn, StageEmitterUpdate,
                                StageParticleSpawn, StageParticleUpdate };
    }

    TArray<FString> ParameterTypeCandidates()
    {
        return TArray<FString>{ TEXT("float"), TEXT("int"), TEXT("bool"), TEXT("vec2"), TEXT("vec3"),
                                TEXT("vec4"), TEXT("color"), TEXT("position"), TEXT("quat") };
    }

    /** Single implementation of "what an emitter handle is", used by every readback. */
    void FillEmitterInfo(const FNiagaraEmitterHandle& Handle, FUnrealMCPNiagaraEmitterInfo& OutInfo);

    /**
     * Is this pin the parameter-map connection that holds a stage's node chain together?
     * UNiagaraNode::IsParameterMapPin does this but is a non-static member, so a node typed as the
     * base UEdGraphNode (the chain walk) cannot call it; the schema gives the same answer.
     */
    bool IsParameterMapConnection(const UEdGraphPin* Pin)
    {
        return Pin != nullptr
            && UEdGraphSchema_Niagara::PinToTypeDefinition(Pin) == FNiagaraTypeDefinition::GetParameterMapDef();
    }

    /** Name after the last namespace separator ("User.Grid" -> "Grid"). */
    FString NameTail(const FString& Name)
    {
        int32 DotIndex = INDEX_NONE;
        return Name.FindLastChar(TEXT('.'), DotIndex) ? Name.RightChop(DotIndex + 1) : Name;
    }

    /** Text of a reflected value, for a readback that is comparable to what was sent. */
    FString JsonValueToText(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid())
        {
            return FString();
        }
        switch (Value->Type)
        {
        case EJson::String: return Value->AsString();
        case EJson::Number: return FString::SanitizeFloat(Value->AsNumber());
        case EJson::Boolean: return Value->AsBool() ? TEXT("true") : TEXT("false");
        case EJson::Null: return TEXT("null");
        default:
        {
            FString Text;
            const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
            FJsonSerializer::Serialize(Value.ToSharedRef(), TEXT(""), Writer);
            return Text;
        }
        }
    }

    /**
     * UClass of a UNiagaraDataInterface subclass by name: a full class path, an exact class name, or a
     * unique substring ("Grid2DCollection"). Ambiguous or unknown names come back with candidates.
     */
    UClass* ResolveDataInterfaceClass(const FString& TypeName, TArray<FString>& OutCandidates)
    {
        if (TypeName.StartsWith(TEXT("/")))
        {
            if (UClass* Direct = FindObject<UClass>(nullptr, *TypeName))
            {
                return Direct->IsChildOf(UNiagaraDataInterface::StaticClass()) ? Direct : nullptr;
            }
            return nullptr;
        }

        UClass* Base = UNiagaraDataInterface::StaticClass();

        // The canonical form first: "Grid2DCollection" is really
        // /Script/Niagara.NiagaraDataInterfaceGrid2DCollection.
        const FString CanonicalName = FString::Printf(TEXT("/Script/Niagara.NiagaraDataInterface%s"), *TypeName);
        if (UClass* Canonical = FindObject<UClass>(nullptr, *CanonicalName))
        {
            if (Canonical->IsChildOf(Base))
            {
                return Canonical;
            }
        }

        UClass* Exact = nullptr;
        UClass* Fuzzy = nullptr;
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Candidate = *It;
            if (!Candidate->IsChildOf(Base) || Candidate->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
            {
                continue;
            }
            const FString Name = Candidate->GetName();
            if (Name.Equals(TypeName, ESearchCase::IgnoreCase))
            {
                Exact = Candidate;
                break;
            }
            if (Name.Contains(TypeName, ESearchCase::IgnoreCase))
            {
                OutCandidates.AddUnique(Name);
                // Several classes can contain the same fragment (a collection and its reader/debug
                // variants); the shortest name is the one the fragment actually names.
                if (!Fuzzy || Name.Len() < Fuzzy->GetName().Len())
                {
                    Fuzzy = Candidate;
                }
            }
        }
        if (Exact)
        {
            return Exact;
        }
        if (OutCandidates.Num() == 1)
        {
            return Fuzzy;
        }
        // An unambiguous fragment still wins if every other match is a longer variation of it.
        if (Fuzzy && OutCandidates.Num() > 1)
        {
            const FString Shortest = Fuzzy->GetName();
            const bool bAllOthersLonger = OutCandidates.ContainsByPredicate([&](const FString& Name)
            {
                return Name != Shortest && Name.Len() <= Shortest.Len();
            }) == false;
            if (bAllOthersLonger)
            {
                return Fuzzy;
            }
        }
        return nullptr;
    }

    FString NormalizeTypeName(const FString& InName)
    {
        FString Name = InName.TrimStartAndEnd().ToLower();
        if (Name == TEXT("vector") || Name == TEXT("vector3") || Name == TEXT("float3")) { return TEXT("vec3"); }
        if (Name == TEXT("vector2") || Name == TEXT("float2")) { return TEXT("vec2"); }
        if (Name == TEXT("vector4") || Name == TEXT("float4")) { return TEXT("vec4"); }
        if (Name == TEXT("int32")) { return TEXT("int"); }
        if (Name == TEXT("linearcolor")) { return TEXT("color"); }
        return Name;
    }

    /** Niagara type for a caller-supplied type name. Unknown names fall through to false. */
    bool ResolveParamType(const FString& TypeName, FNiagaraTypeDefinition& OutType)
    {
        const FString Name = NormalizeTypeName(TypeName);
        if (Name == TEXT("float")) { OutType = FNiagaraTypeDefinition::GetFloatDef(); return true; }
        if (Name == TEXT("int")) { OutType = FNiagaraTypeDefinition::GetIntDef(); return true; }
        if (Name == TEXT("bool")) { OutType = FNiagaraTypeDefinition::GetBoolDef(); return true; }
        if (Name == TEXT("vec2")) { OutType = FNiagaraTypeDefinition::GetVec2Def(); return true; }
        if (Name == TEXT("vec3")) { OutType = FNiagaraTypeDefinition::GetVec3Def(); return true; }
        if (Name == TEXT("vec4")) { OutType = FNiagaraTypeDefinition::GetVec4Def(); return true; }
        if (Name == TEXT("color")) { OutType = FNiagaraTypeDefinition::GetColorDef(); return true; }
        if (Name == TEXT("position")) { OutType = FNiagaraTypeDefinition::GetPositionDef(); return true; }
        if (Name == TEXT("quat")) { OutType = FNiagaraTypeDefinition::GetQuatDef(); return true; }
        return false;
    }

    /**
     * Any UObject class by short name ("TextureRenderTarget2D", "Texture2D", "StaticMesh") or path
     * ("/Script/Engine.TextureRenderTarget2D").
     *
     * A Niagara user parameter can hold an object, not just a number: a Grid2DCollection writes the
     * grid it computes into the RenderTarget bound through `RenderTargetUserParameter`, and that
     * binding is a user parameter of type UTextureRenderTarget - the only way a material can read a
     * grid.
     */
    UClass* ResolveObjectClass(const FString& TypeName)
    {
        const FString Name = TypeName.TrimStartAndEnd();
        if (Name.IsEmpty())
        {
            return nullptr;
        }
        if (Name.StartsWith(TEXT("/")))
        {
            return FindObject<UClass>(nullptr, *Name);
        }
        for (const TCHAR* Root : { TEXT("/Script/Engine."), TEXT("/Script/Niagara.") })
        {
            if (UClass* Found = FindObject<UClass>(nullptr, *FString::Printf(TEXT("%s%s"), Root, *Name)))
            {
                return Found;
            }
        }
        const FString Wanted = NameTail(Name);
        for (TObjectIterator<UClass> It; It; ++It)
        {
            if (It->GetName().Equals(Wanted, ESearchCase::IgnoreCase) && !It->HasAnyClassFlags(CLASS_Abstract))
            {
                return *It;
            }
        }
        return nullptr;
    }

    /**
     * Pin/parameter type for the scratch-pad and parameter commands: the scalar/vector types, a data
     * interface class ("Grid2DCollection"), or any UObject class ("TextureRenderTarget2D").
     */
    bool ResolvePinType(const FString& TypeName, FNiagaraTypeDefinition& OutType)
    {
        if (ResolveParamType(TypeName, OutType))
        {
            return true;
        }
        TArray<FString> ClassCandidates;
        if (UClass* DataInterfaceClass = ResolveDataInterfaceClass(TypeName.TrimStartAndEnd(), ClassCandidates))
        {
            OutType = FNiagaraTypeDefinition(DataInterfaceClass);
            return true;
        }
        if (UClass* ObjectClass = ResolveObjectClass(TypeName))
        {
            OutType = FNiagaraTypeDefinition(ObjectClass);
            return true;
        }
        return false;
    }

    /**
     * Type name of a parameter, plus the data-interface flag: a DI parameter's "value" is an object,
     * not a number, so it needs a different writer and a different readback.
     */
    FString TypeNameOf(const FNiagaraTypeDefinition& Type)
    {
        if (Type == FNiagaraTypeDefinition::GetFloatDef()) { return TEXT("float"); }
        if (Type == FNiagaraTypeDefinition::GetIntDef()) { return TEXT("int"); }
        if (Type == FNiagaraTypeDefinition::GetBoolDef()) { return TEXT("bool"); }
        if (Type == FNiagaraTypeDefinition::GetVec2Def()) { return TEXT("vec2"); }
        if (Type == FNiagaraTypeDefinition::GetVec3Def()) { return TEXT("vec3"); }
        if (Type == FNiagaraTypeDefinition::GetVec4Def()) { return TEXT("vec4"); }
        if (Type == FNiagaraTypeDefinition::GetColorDef()) { return TEXT("color"); }
        if (Type == FNiagaraTypeDefinition::GetPositionDef()) { return TEXT("position"); }
        if (Type == FNiagaraTypeDefinition::GetQuatDef()) { return TEXT("quat"); }
        if (Type.GetClass() != nullptr)
        {
            // Data interface (Grid2DCollection, ArrayFloat3, ...) or a UObject type.
            return Type.GetClass()->GetName();
        }
        return Type.GetName();
    }

    /**
     * Numbers out of a value string. Accepts "1,2,3", "(1, 2, 3)", "[1 2 3]" and the engine's own
     * export form with member labels ("(X=1.0,Y=2.0,Z=3.0)", "(R=1,G=0,B=0,A=1)") - the shape a
     * previous readback hands back, so a value can be fed straight back in.
     */
    bool ParseNumbers(const FString& InText, TArray<double>& OutNumbers)
    {
        FString Text = InText.TrimStartAndEnd();
        Text.ReplaceInline(TEXT("("), TEXT(" "));
        Text.ReplaceInline(TEXT(")"), TEXT(" "));
        Text.ReplaceInline(TEXT("["), TEXT(" "));
        Text.ReplaceInline(TEXT("]"), TEXT(" "));
        Text.ReplaceInline(TEXT("{"), TEXT(" "));
        Text.ReplaceInline(TEXT("}"), TEXT(" "));
        Text.ReplaceInline(TEXT(","), TEXT(" "));

        TArray<FString> Tokens;
        Text.ParseIntoArrayWS(Tokens);
        for (FString& Token : Tokens)
        {
            // Strip "X=" / "R=" style labels.
            int32 EqualsIndex = INDEX_NONE;
            if (Token.FindChar(TEXT('='), EqualsIndex))
            {
                Token = Token.RightChop(EqualsIndex + 1);
            }
            if (Token.IsEmpty())
            {
                continue;
            }
            OutNumbers.Add(FCString::Atod(*Token));
        }
        return OutNumbers.Num() > 0;
    }

    FString FormatDouble(double Value)
    {
        return FString::Printf(TEXT("%g"), Value);
    }

    // ------------------------------------------------------------------
    // Stage <-> usage
    // ------------------------------------------------------------------

    bool StageToUsage(const FString& Stage, ENiagaraScriptUsage& OutUsage)
    {
        const FString S = Stage.TrimStartAndEnd();
        if (S.Equals(StageSystemSpawn, ESearchCase::IgnoreCase)) { OutUsage = ENiagaraScriptUsage::SystemSpawnScript; return true; }
        if (S.Equals(StageSystemUpdate, ESearchCase::IgnoreCase)) { OutUsage = ENiagaraScriptUsage::SystemUpdateScript; return true; }
        if (S.Equals(StageEmitterSpawn, ESearchCase::IgnoreCase)) { OutUsage = ENiagaraScriptUsage::EmitterSpawnScript; return true; }
        if (S.Equals(StageEmitterUpdate, ESearchCase::IgnoreCase)) { OutUsage = ENiagaraScriptUsage::EmitterUpdateScript; return true; }
        if (S.Equals(StageParticleSpawn, ESearchCase::IgnoreCase)) { OutUsage = ENiagaraScriptUsage::ParticleSpawnScript; return true; }
        if (S.Equals(StageParticleUpdate, ESearchCase::IgnoreCase)) { OutUsage = ENiagaraScriptUsage::ParticleUpdateScript; return true; }
        return false;
    }

    FString UsageToStage(ENiagaraScriptUsage Usage)
    {
        switch (Usage)
        {
        case ENiagaraScriptUsage::Function: return TEXT("Function");
        case ENiagaraScriptUsage::Module: return TEXT("Module");
        case ENiagaraScriptUsage::DynamicInput: return TEXT("DynamicInput");
        case ENiagaraScriptUsage::ParticleSpawnScript: return StageParticleSpawn;
        case ENiagaraScriptUsage::ParticleSpawnScriptInterpolated: return TEXT("ParticleSpawnInterpolated");
        case ENiagaraScriptUsage::ParticleUpdateScript: return StageParticleUpdate;
        case ENiagaraScriptUsage::ParticleEventScript: return TEXT("ParticleEvent");
        case ENiagaraScriptUsage::ParticleSimulationStageScript: return TEXT("ParticleSimulationStage");
        case ENiagaraScriptUsage::ParticleGPUComputeScript: return TEXT("ParticleGPUCompute");
        case ENiagaraScriptUsage::EmitterSpawnScript: return StageEmitterSpawn;
        case ENiagaraScriptUsage::EmitterUpdateScript: return StageEmitterUpdate;
        case ENiagaraScriptUsage::SystemSpawnScript: return StageSystemSpawn;
        case ENiagaraScriptUsage::SystemUpdateScript: return StageSystemUpdate;
        default: return FString::Printf(TEXT("Usage%d"), static_cast<int32>(Usage));
        }
    }

    bool IsSystemStage(ENiagaraScriptUsage Usage)
    {
        return Usage == ENiagaraScriptUsage::SystemSpawnScript || Usage == ENiagaraScriptUsage::SystemUpdateScript;
    }

    UNiagaraScript* GetStageScript(FVersionedNiagaraEmitterData* EmitterData, ENiagaraScriptUsage Usage)
    {
        if (!EmitterData)
        {
            return nullptr;
        }
        switch (Usage)
        {
        case ENiagaraScriptUsage::ParticleSpawnScript: return EmitterData->SpawnScriptProps.Script;
        case ENiagaraScriptUsage::ParticleUpdateScript: return EmitterData->UpdateScriptProps.Script;
        case ENiagaraScriptUsage::EmitterSpawnScript: return EmitterData->EmitterSpawnScriptProps.Script;
        case ENiagaraScriptUsage::EmitterUpdateScript: return EmitterData->EmitterUpdateScriptProps.Script;
        default: return nullptr;
        }
    }

    /** The emitter's single stage graph; every stage of one emitter shares it. */
    UNiagaraGraph* GetEmitterGraph(FVersionedNiagaraEmitterData* EmitterData)
    {
        if (!EmitterData)
        {
            return nullptr;
        }
        if (UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(EmitterData->GraphSource))
        {
            return Source->NodeGraph;
        }
        return nullptr;
    }

    UNiagaraGraph* GetSystemGraph(UNiagaraSystem* System)
    {
        if (!System || !System->GetSystemUpdateScript())
        {
            return nullptr;
        }
        if (UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(System->GetSystemUpdateScript()->GetLatestSource()))
        {
            return Source->NodeGraph;
        }
        return nullptr;
    }

    UNiagaraScript* GetSystemStageScript(UNiagaraSystem* System, ENiagaraScriptUsage Usage)
    {
        if (!System)
        {
            return nullptr;
        }
        return Usage == ENiagaraScriptUsage::SystemSpawnScript ? System->GetSystemSpawnScript()
                                                               : System->GetSystemUpdateScript();
    }

    /**
     * Output node of one stage.
     *
     * FindEquivalentOutputNode is the exported lookup; the fallback walks the graph because
     * UNiagaraGraph::FindOutputNode(s) is not exported across modules.
     */
    UNiagaraNodeOutput* FindStageOutputNode(UNiagaraGraph* Graph, ENiagaraScriptUsage Usage, UNiagaraScript* StageScript)
    {
        if (!Graph)
        {
            return nullptr;
        }
        if (UNiagaraNodeOutput* Node = Graph->FindEquivalentOutputNode(Usage, StageScript ? StageScript->GetUsageId() : FGuid()))
        {
            return Node;
        }
        TArray<UNiagaraNodeOutput*> AllOutputs;
        Graph->GetNodesOfClass<UNiagaraNodeOutput>(AllOutputs);
        for (UNiagaraNodeOutput* Candidate : AllOutputs)
        {
            if (Candidate && Candidate->GetUsage() == Usage)
            {
                return Candidate;
            }
        }
        return nullptr;
    }

    /**
     * A simulation stage by name ("SWE Sweep") or by index ("0") on the emitter.
     *
     * A simulation stage is not one of the six fixed stages: it has its own script (the stage identity
     * the compiler matches on is that script's usage id) and shares the emitter's graph. The index is
     * the emitter's SimulationStages order, which is also the order the stages run in.
     */
    UNiagaraSimulationStageBase* FindSimulationStage(FVersionedNiagaraEmitterData* EmitterData,
                                                     const FString& StageRef)
    {
        if (!EmitterData)
        {
            return nullptr;
        }
        const FString S = StageRef.TrimStartAndEnd();
        if (S.IsEmpty())
        {
            return nullptr;
        }
        const TArray<UNiagaraSimulationStageBase*>& Stages = EmitterData->GetSimulationStages();
        if (S.IsNumeric())
        {
            const int32 Index = FCString::Atoi(*S);
            return Stages.IsValidIndex(Index) ? Stages[Index] : nullptr;
        }
        for (UNiagaraSimulationStageBase* Stage : Stages)
        {
            if (Stage && Stage->SimulationStageName.ToString().Equals(S, ESearchCase::IgnoreCase))
            {
                return Stage;
            }
        }
        return nullptr;
    }

    void AppendSimulationStageCandidates(FVersionedNiagaraEmitterData* EmitterData, TArray<FString>& Candidates)
    {
        if (!EmitterData)
        {
            return;
        }
        const TArray<UNiagaraSimulationStageBase*>& Stages = EmitterData->GetSimulationStages();
        for (int32 Index = 0; Index < Stages.Num(); ++Index)
        {
            if (Stages[Index])
            {
                Candidates.Add(FString::Printf(TEXT("%d: %s"), Index,
                                               *Stages[Index]->SimulationStageName.ToString()));
            }
        }
    }

    /** A stage reference resolved to everything the module commands need. */
    struct FNiagaraStageTarget
    {
        ENiagaraScriptUsage Usage = ENiagaraScriptUsage::ParticleUpdateScript;
        UNiagaraScript* StageScript = nullptr;
        UNiagaraSimulationStageBase* SimulationStage = nullptr;
        FString Label;
    };

    /** Resolve a stage name/index on one emitter: one of the four emitter stages, or a simulation stage. */
    bool ResolveEmitterStageTarget(FVersionedNiagaraEmitterData* EmitterData, const FString& StageRef,
                                   FNiagaraStageTarget& OutTarget)
    {
        if (!EmitterData)
        {
            return false;
        }
        ENiagaraScriptUsage Usage;
        if (StageToUsage(StageRef, Usage) && !IsSystemStage(Usage))
        {
            OutTarget.Usage = Usage;
            OutTarget.StageScript = GetStageScript(EmitterData, Usage);
            OutTarget.Label = UsageToStage(Usage);
            return true;
        }
        if (UNiagaraSimulationStageBase* Stage = FindSimulationStage(EmitterData, StageRef))
        {
            OutTarget.Usage = ENiagaraScriptUsage::ParticleSimulationStageScript;
            OutTarget.StageScript = Stage->Script;
            OutTarget.SimulationStage = Stage;
            OutTarget.Label = Stage->SimulationStageName.ToString();
            return true;
        }
        return false;
    }

    /**
     * The stage a module actually reports. A module on a simulation stage is reported by that stage's
     * own name - "ParticleSimulationStage" is only the script usage behind it.
     */
    FString StageLabelForOutputNode(FVersionedNiagaraEmitterData* EmitterData, UNiagaraNodeOutput* OutputNode)
    {
        if (!OutputNode)
        {
            return TEXT("unknown");
        }
        if (OutputNode->GetUsage() == ENiagaraScriptUsage::ParticleSimulationStageScript && EmitterData)
        {
            const TArray<UNiagaraSimulationStageBase*>& Stages = EmitterData->GetSimulationStages();
            for (UNiagaraSimulationStageBase* Stage : Stages)
            {
                if (Stage && Stage->Script && Stage->Script->GetUsageId() == OutputNode->GetUsageId())
                {
                    return Stage->SimulationStageName.ToString();
                }
            }
        }
        return UsageToStage(OutputNode->GetUsage());
    }

    /** The script of the stage an output node belongs to (simulation stage included). */
    UNiagaraScript* StageScriptForOutputNode(FVersionedNiagaraEmitterData* EmitterData, UNiagaraSystem* System,
                                             UNiagaraNodeOutput* OutputNode)
    {
        if (!OutputNode)
        {
            return nullptr;
        }
        const ENiagaraScriptUsage Usage = OutputNode->GetUsage();
        if (Usage == ENiagaraScriptUsage::ParticleSimulationStageScript)
        {
            if (!EmitterData)
            {
                return nullptr;
            }
            const TArray<UNiagaraSimulationStageBase*>& Stages = EmitterData->GetSimulationStages();
            for (UNiagaraSimulationStageBase* Stage : Stages)
            {
                if (Stage && Stage->Script && Stage->Script->GetUsageId() == OutputNode->GetUsageId())
                {
                    return Stage->Script;
                }
            }
            return nullptr;
        }
        return EmitterData ? GetStageScript(EmitterData, Usage) : GetSystemStageScript(System, Usage);
    }

    /** The stage a module node belongs to, read from the output node it eventually reaches. */
    UNiagaraNodeOutput* FindOutputNodeForFunctionCall(UNiagaraNodeFunctionCall* FunctionCall)
    {
        if (!FunctionCall)
        {
            return nullptr;
        }
        TSet<UEdGraphNode*> Visited;
        TArray<UEdGraphNode*> Pending;
        for (UEdGraphPin* Pin : FunctionCall->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Output)
            {
                continue;
            }
            for (UEdGraphPin* Linked : Pin->LinkedTo)
            {
                if (Linked && Linked->GetOwningNode())
                {
                    Pending.Add(Linked->GetOwningNode());
                }
            }
        }

        while (Pending.Num() > 0)
        {
            UEdGraphNode* Current = Pending.Pop();
            if (!Current || Visited.Contains(Current))
            {
                continue;
            }
            Visited.Add(Current);
            if (UNiagaraNodeOutput* OutputNode = Cast<UNiagaraNodeOutput>(Current))
            {
                return OutputNode;
            }
            for (UEdGraphPin* Pin : Current->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Output)
                {
                    continue;
                }
                for (UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    if (Linked && Linked->GetOwningNode())
                    {
                        Pending.Add(Linked->GetOwningNode());
                    }
                }
            }
        }
        return nullptr;
    }

    /**
     * Stack order of one stage: walk backwards from the output node along the parameter-map chain.
     * Returned front-to-back (first module first). Walking backwards is the only way that gives the
     * evaluation order without the (unexported) FNiagaraStackGraphUtilities::GetStackNodeGroups.
     */
    void CollectStageModules(UNiagaraNodeOutput* OutputNode, TArray<UNiagaraNodeFunctionCall*>& OutModules)
    {
        OutModules.Reset();
        if (!OutputNode)
        {
            return;
        }

        TSet<const UEdGraphNode*> Visited;
        const UEdGraphNode* Current = OutputNode;
        while (Current && !Visited.Contains(Current))
        {
            Visited.Add(Current);

            const UEdGraphPin* MapInput = nullptr;
            for (UEdGraphPin* Pin : Current->Pins)
            {
                // UNiagaraNode::IsParameterMapPin is declared on the Niagara node, not on the base
                // UEdGraphNode this walk holds, so the parameter-map test goes through the schema.
                if (Pin && Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() > 0
                    && UEdGraphSchema_Niagara::PinToTypeDefinition(Pin) == FNiagaraTypeDefinition::GetParameterMapDef())
                {
                    MapInput = Pin;
                    break;
                }
            }
            if (!MapInput)
            {
                break;
            }

            const UEdGraphNode* Previous = MapInput->LinkedTo[0] ? MapInput->LinkedTo[0]->GetOwningNode() : nullptr;
            if (!Previous)
            {
                break;
            }
            if (const UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Previous))
            {
                OutModules.Add(const_cast<UNiagaraNodeFunctionCall*>(Call));
            }
            Current = Previous;
        }

        Algo::Reverse(OutModules);
    }

    // ------------------------------------------------------------------
    // Scratch-pad graph access
    // ------------------------------------------------------------------

    UNiagaraNodeFunctionCall* FindModuleNodeInGraph(UNiagaraGraph* Graph, const FString& ModuleRef)
    {
        if (!Graph)
        {
            return nullptr;
        }
        TArray<UNiagaraNodeFunctionCall*> Calls;
        Graph->GetNodesOfClass<UNiagaraNodeFunctionCall>(Calls);
        for (UNiagaraNodeFunctionCall* Call : Calls)
        {
            if (!Call)
            {
                continue;
            }
            if (Call->NodeGuid.ToString().Equals(ModuleRef, ESearchCase::IgnoreCase)
                || Call->GetFunctionName().Equals(ModuleRef, ESearchCase::IgnoreCase)
                || (Call->FunctionScript && Call->FunctionScript->GetName().Equals(ModuleRef, ESearchCase::IgnoreCase)))
            {
                return Call;
            }
        }
        return nullptr;
    }

    UNiagaraNodeFunctionCall* FindModuleNode(FVersionedNiagaraEmitterData* EmitterData, const FString& ModuleRef)
    {
        return FindModuleNodeInGraph(GetEmitterGraph(EmitterData), ModuleRef);
    }

    UNiagaraGraph* GetScratchGraph(UNiagaraNodeFunctionCall* Call)
    {
        if (!Call || !Call->FunctionScript)
        {
            return nullptr;
        }
        if (UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Call->FunctionScript->GetLatestSource()))
        {
            return Source->NodeGraph;
        }
        return nullptr;
    }

    UNiagaraScratchPadContainer* GetOrCreateScratchPadContainer(FNiagaraEmitterHandle* Handle,
                                                                FVersionedNiagaraEmitterData* EmitterData)
    {
        if (!Handle || !EmitterData)
        {
            return nullptr;
        }
        if (EmitterData->ScratchPads)
        {
            return EmitterData->ScratchPads;
        }
        UNiagaraEmitter* Emitter = Handle->GetInstance().Emitter;
        if (!Emitter)
        {
            return nullptr;
        }
        UNiagaraScratchPadContainer* Container = NewObject<UNiagaraScratchPadContainer>(Emitter, NAME_None, RF_Transactional);
        EmitterData->ScratchPads = Container;
        return Container;
    }

    UEdGraphNode* FindNodeByGuid(UNiagaraGraph* Graph, const FString& NodeGuid)
    {
        if (!Graph)
        {
            return nullptr;
        }
        FGuid Guid;
        if (!FGuid::Parse(NodeGuid, Guid))
        {
            return nullptr;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node->NodeGuid == Guid)
            {
                return Node;
            }
        }
        return nullptr;
    }

    UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction,
                               bool bConstrainDirection)
    {
        if (!Node)
        {
            return nullptr;
        }
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->bHidden)
            {
                continue;
            }
            if (bConstrainDirection && Pin->Direction != Direction)
            {
                continue;
            }
            if (Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
            {
                return Pin;
            }
        }
        return nullptr;
    }

    /** Node classes that live in NiagaraEditor/Private, reached by path (their headers are not public). */
    UClass* GetNiagaraEditorNodeClass(const TCHAR* ShortName)
    {
        const FString FullPath = FString::Printf(TEXT("/Script/NiagaraEditor.%s"), ShortName);
        return FindObject<UClass>(nullptr, *FullPath);
    }

    UClass* GetMapGetClass()
    {
        static UClass* Cached = GetNiagaraEditorNodeClass(TEXT("NiagaraNodeParameterMapGet"));
        return Cached;
    }

    UClass* GetMapSetClass()
    {
        static UClass* Cached = GetNiagaraEditorNodeClass(TEXT("NiagaraNodeParameterMapSet"));
        return Cached;
    }

    bool IsMapGetNode(const UEdGraphNode* Node)
    {
        UClass* Class = GetMapGetClass();
        return Node && Class && Node->IsA(Class);
    }

    bool IsMapSetNode(const UEdGraphNode* Node)
    {
        UClass* Class = GetMapSetClass();
        return Node && Class && Node->IsA(Class);
    }

    bool IsAddPin(const UEdGraphPin* Pin)
    {
        return Pin && Pin->PinType.PinCategory == UEdGraphSchema_Niagara::PinCategoryMisc
            && Pin->PinType.PinSubCategory == FName(TEXT("DynamicAddPin"));
    }

    FString NodeTypeLabel(UEdGraphNode* Node)
    {
        if (!Node)
        {
            return FString();
        }
        if (Node->IsA<UNiagaraNodeCustomHlsl>()) { return TEXT("CustomHlsl"); }
        if (IsMapGetNode(Node)) { return TEXT("MapGet"); }
        if (IsMapSetNode(Node)) { return TEXT("MapSet"); }
        if (Node->IsA<UNiagaraNodeInput>()) { return TEXT("Input"); }
        if (Node->IsA<UNiagaraNodeOutput>()) { return TEXT("Output"); }
        if (Node->IsA<UNiagaraNodeFunctionCall>()) { return TEXT("FunctionCall"); }
        return Node->GetClass()->GetName();
    }

    /** Mirror of FGraphNodeCreator<T>::Finalize() for a node class whose header is not public. */
    UEdGraphNode* SpawnNodeByClass(UNiagaraGraph* Graph, UClass* NodeClass, int32 PosX, int32 PosY)
    {
        if (!Graph || !NodeClass)
        {
            return nullptr;
        }
        UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, NodeClass, NAME_None, RF_Transactional);
        if (!Node)
        {
            return nullptr;
        }
        Graph->AddNode(Node, /*bFromUI=*/false, /*bSelectNewNode=*/false);
        Node->CreateNewGuid();
        Node->PostPlacedNewNode();
        Node->AllocateDefaultPins();
        Node->NodePosX = PosX;
        Node->NodePosY = PosY;
        return Node;
    }

    // UNiagaraNodeCustomHlsl::Get/SetCustomHlsl and RebuildSignatureFromPins are protected and are
    // not exported from NiagaraEditor, so the HLSL text and the node signature are reached through
    // the UPROPERTY and the public pin set instead.

    FProperty* FindCustomHlslProperty(UEdGraphNode* Node)
    {
        return Node ? Node->GetClass()->FindPropertyByName(TEXT("CustomHlsl")) : nullptr;
    }

    void SetCustomHlslReflected(UEdGraphNode* HlslNode, const FString& Code)
    {
        if (!HlslNode)
        {
            return;
        }
        if (FStrProperty* Property = CastField<FStrProperty>(FindCustomHlslProperty(HlslNode)))
        {
            HlslNode->Modify();
            Property->SetPropertyValue_InContainer(HlslNode, Code);
            FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
            HlslNode->PostEditChangeProperty(Event);
        }
    }

    FString GetCustomHlslReflected(UEdGraphNode* HlslNode)
    {
        if (FStrProperty* Property = CastField<FStrProperty>(FindCustomHlslProperty(HlslNode)))
        {
            return Property->GetPropertyValue_InContainer(HlslNode);
        }
        return FString();
    }

    /**
     * Rebuild a Custom HLSL node's signature from its current pins.
     *
     * The engine does this in the protected UNiagaraNodeCustomHlsl::RebuildSignatureFromPins. The
     * obvious alternative (PostEditChangeProperty on a pin change) runs RefreshFromExternalChanges ->
     * ReallocatePins(false), which rebuilds the pins from the *stale* signature and drops the pin
     * that was just created.
     */
    void RebuildCustomHlslSignatureFromPins(UEdGraphNode* HlslNode)
    {
        UNiagaraNodeFunctionCall* FunctionCall = Cast<UNiagaraNodeFunctionCall>(HlslNode);
        if (!FunctionCall)
        {
            return;
        }
        const UEdGraphSchema_Niagara* Schema = Cast<UEdGraphSchema_Niagara>(HlslNode->GetSchema());
        if (!Schema)
        {
            return;
        }

        HlslNode->Modify();
        FNiagaraFunctionSignature& Signature = FunctionCall->Signature;
        Signature.Inputs.Empty();
        Signature.Outputs.Empty();

        for (UEdGraphPin* Pin : HlslNode->Pins)
        {
            if (!Pin || IsAddPin(Pin))
            {
                continue;
            }
            if (Pin->Direction == EGPD_Input)
            {
                Signature.Inputs.Add(Schema->PinToNiagaraVariable(Pin, /*bNeedsValue=*/true));
            }
            else
            {
                Signature.Outputs.Add(Schema->PinToNiagaraVariable(Pin, /*bNeedsValue=*/false));
            }
        }
    }

    // ------------------------------------------------------------------
    // Compile status
    // ------------------------------------------------------------------

    FString CompileStatusToString(ENiagaraScriptCompileStatus Status)
    {
        switch (Status)
        {
        case ENiagaraScriptCompileStatus::NCS_UpToDate: return TEXT("up_to_date");
        case ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings: return TEXT("up_to_date_with_warnings");
        case ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings: return TEXT("compute_up_to_date_with_warnings");
        case ENiagaraScriptCompileStatus::NCS_Error: return TEXT("error");
        case ENiagaraScriptCompileStatus::NCS_Dirty: return TEXT("dirty");
        case ENiagaraScriptCompileStatus::NCS_BeingCreated: return TEXT("being_created");
        default: return TEXT("unknown");
        }
    }

    FString CompileEventSeverityToString(FNiagaraCompileEventSeverity Severity)
    {
        switch (Severity)
        {
        case FNiagaraCompileEventSeverity::Error: return TEXT("error");
        case FNiagaraCompileEventSeverity::Warning: return TEXT("warning");
        case FNiagaraCompileEventSeverity::Log: return TEXT("log");
        default: return TEXT("display");
        }
    }

    /** Every script of a system: the two system scripts, each emitter's scripts and its scratch pads. */
    void CollectSystemScripts(UNiagaraSystem* System, TArray<UNiagaraScript*>& OutScripts)
    {
        OutScripts.Reset();
        if (!System)
        {
            return;
        }
        if (UNiagaraScript* Spawn = System->GetSystemSpawnScript()) { OutScripts.Add(Spawn); }
        if (UNiagaraScript* Update = System->GetSystemUpdateScript()) { OutScripts.Add(Update); }

        for (FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
        {
            FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
            if (!EmitterData)
            {
                continue;
            }
            TArray<UNiagaraScript*> EmitterScripts;
            EmitterData->GetScripts(EmitterScripts, /*bCompilableOnly=*/true, /*bEnabledOnly=*/false);
            OutScripts.Append(EmitterScripts);
#if WITH_EDITORONLY_DATA
            if (EmitterData->ScratchPads)
            {
                for (UNiagaraScript* Scratch : EmitterData->ScratchPads->Scripts)
                {
                    if (Scratch)
                    {
                        OutScripts.Add(Scratch);
                    }
                }
            }
#endif
        }

        OutScripts.RemoveAll([](UNiagaraScript* Script) { return Script == nullptr; });
    }

    void CollectCompileEvents(UNiagaraSystem* System, FUnrealMCPNiagaraResult& Out)
    {
        TArray<UNiagaraScript*> Scripts;
        CollectSystemScripts(System, Scripts);

        int32 ErrorCount = 0;
        int32 WarningCount = 0;
        for (UNiagaraScript* Script : Scripts)
        {
            const ENiagaraScriptCompileStatus Status = Script->GetLastCompileStatus();

            FUnrealMCPNiagaraScriptStatus ScriptStatus;
            ScriptStatus.ScriptPath = Script->GetPathName();
            ScriptStatus.Usage = UsageToStage(Script->GetUsage());
            ScriptStatus.Status = CompileStatusToString(Status);
            int32 ScriptErrors = 0;
            int32 ScriptWarnings = 0;

#if WITH_EDITORONLY_DATA
            const FNiagaraVMExecutableData& ExecutableData = Script->GetVMExecutableData();
            if (!ExecutableData.ErrorMsg.IsEmpty())
            {
                FUnrealMCPNiagaraCompileMessage Message;
                Message.Severity = TEXT("error");
                Message.ScriptPath = Script->GetPathName();
                Message.Message = ExecutableData.ErrorMsg;
                Out.CompileMessages.Add(Message);
                ++ScriptErrors;
            }
            for (const FNiagaraCompileEvent& Event : ExecutableData.LastCompileEvents)
            {
                const FString Severity = CompileEventSeverityToString(Event.Severity);
                if (Severity == TEXT("log") || Severity == TEXT("display"))
                {
                    continue;
                }
                FUnrealMCPNiagaraCompileMessage Message;
                Message.Severity = Severity;
                Message.ScriptPath = Script->GetPathName();
                Message.Message = Event.Message.IsEmpty() ? Event.ShortDescription : Event.Message;
                Message.NodeGuid = Event.NodeGuid.ToString();
                Out.CompileMessages.Add(Message);
                if (Severity == TEXT("error")) { ++ScriptErrors; } else { ++ScriptWarnings; }
            }
#endif
            ScriptStatus.ErrorCount = ScriptErrors;
            ScriptStatus.WarningCount = ScriptWarnings;
            ErrorCount += ScriptErrors;
            WarningCount += ScriptWarnings;
            Out.ScriptStatuses.Add(ScriptStatus);
        }

        Out.CompileStatus = ErrorCount > 0 ? TEXT("error") : TEXT("success");
    }

    // ------------------------------------------------------------------
    // Parameter value read helpers
    // ------------------------------------------------------------------

    bool ReadParameterValue(const FNiagaraParameterStore& Store, const FNiagaraVariable& Variable, FString& OutValue)
    {
        const FNiagaraTypeDefinition& Type = Variable.GetType();

        if (Type == FNiagaraTypeDefinition::GetPositionDef())
        {
            if (const FVector* Position = Store.GetPositionParameterValue(Variable.GetName()))
            {
                OutValue = FString::Printf(TEXT("(X=%s,Y=%s,Z=%s)"), *FormatDouble(Position->X),
                    *FormatDouble(Position->Y), *FormatDouble(Position->Z));
                return true;
            }
            return false;
        }
        if (Type.GetClass() != nullptr)
        {
            // A data interface's stored "offset" indexes the store's DataInterfaces array, every
            // other object parameter's indexes UObjects. Asking for the wrong one returns whatever
            // object happens to live at that index in the other array, so the branch has to follow
            // the type: a TextureRenderTarget parameter otherwise reads back as the DataInterface
            // that sits at the same offset (measured: WaterRT reported the Grid2DCollection).
            if (Type.IsDataInterface())
            {
                if (UNiagaraDataInterface* DataInterface = Store.GetDataInterface(Variable))
                {
                    OutValue = DataInterface->GetPathName();
                    return true;
                }
            }
            else if (Type.IsUObject())
            {
                if (UObject* Object = Store.GetUObject(Variable))
                {
                    OutValue = Object->GetPathName();
                    return true;
                }
            }
            OutValue = TEXT("null");
            return true;
        }

        const uint8* Data = Store.GetParameterData(Variable);
        if (!Data)
        {
            return false;
        }

        if (Type == FNiagaraTypeDefinition::GetFloatDef())
        {
            OutValue = FormatDouble(*reinterpret_cast<const float*>(Data));
            return true;
        }
        if (Type == FNiagaraTypeDefinition::GetIntDef())
        {
            OutValue = FString::FromInt(*reinterpret_cast<const int32*>(Data));
            return true;
        }
        if (Type == FNiagaraTypeDefinition::GetBoolDef())
        {
            OutValue = (*reinterpret_cast<const FNiagaraBool*>(Data)) ? TEXT("true") : TEXT("false");
            return true;
        }
        if (Type == FNiagaraTypeDefinition::GetVec2Def())
        {
            const FVector2f& Value = *reinterpret_cast<const FVector2f*>(Data);
            OutValue = FString::Printf(TEXT("(X=%s,Y=%s)"), *FormatDouble(Value.X), *FormatDouble(Value.Y));
            return true;
        }
        if (Type == FNiagaraTypeDefinition::GetVec3Def())
        {
            const FVector3f& Value = *reinterpret_cast<const FVector3f*>(Data);
            OutValue = FString::Printf(TEXT("(X=%s,Y=%s,Z=%s)"), *FormatDouble(Value.X),
                *FormatDouble(Value.Y), *FormatDouble(Value.Z));
            return true;
        }
        if (Type == FNiagaraTypeDefinition::GetVec4Def())
        {
            const FVector4f& Value = *reinterpret_cast<const FVector4f*>(Data);
            OutValue = FString::Printf(TEXT("(X=%s,Y=%s,Z=%s,W=%s)"), *FormatDouble(Value.X),
                *FormatDouble(Value.Y), *FormatDouble(Value.Z), *FormatDouble(Value.W));
            return true;
        }
        if (Type == FNiagaraTypeDefinition::GetColorDef())
        {
            const FLinearColor& Value = *reinterpret_cast<const FLinearColor*>(Data);
            OutValue = FString::Printf(TEXT("(R=%s,G=%s,B=%s,A=%s)"), *FormatDouble(Value.R),
                *FormatDouble(Value.G), *FormatDouble(Value.B), *FormatDouble(Value.A));
            return true;
        }
        if (Type == FNiagaraTypeDefinition::GetQuatDef())
        {
            const FQuat4f& Value = *reinterpret_cast<const FQuat4f*>(Data);
            OutValue = FString::Printf(TEXT("(X=%s,Y=%s,Z=%s,W=%s)"), *FormatDouble(Value.X),
                *FormatDouble(Value.Y), *FormatDouble(Value.Z), *FormatDouble(Value.W));
            return true;
        }
        return false;
    }

    /** Write one scalar value into a parameter store; the store grows the parameter when asked to. */
    bool WriteParameterValue(FNiagaraParameterStore& Store, const FNiagaraVariable& Variable, const FString& InValue,
                             bool bAddIfMissing)
    {
        const FNiagaraTypeDefinition& Type = Variable.GetType();
        TArray<double> Numbers;
        ParseNumbers(InValue, Numbers);

        if (Type == FNiagaraTypeDefinition::GetPositionDef())
        {
            if (Numbers.Num() < 3)
            {
                return false;
            }
            return Store.SetPositionParameterValue(FVector(Numbers[0], Numbers[1], Numbers[2]), Variable.GetName(), bAddIfMissing);
        }
        if (Type == FNiagaraTypeDefinition::GetFloatDef())
        {
            if (Numbers.Num() < 1) { return false; }
            return Store.SetParameterValue(static_cast<float>(Numbers[0]), Variable, bAddIfMissing);
        }
        if (Type == FNiagaraTypeDefinition::GetIntDef())
        {
            if (Numbers.Num() < 1) { return false; }
            return Store.SetParameterValue(static_cast<int32>(Numbers[0]), Variable, bAddIfMissing);
        }
        if (Type == FNiagaraTypeDefinition::GetBoolDef())
        {
            const FString Trimmed = InValue.TrimStartAndEnd().ToLower();
            const bool bValue = Trimmed == TEXT("true") || Trimmed == TEXT("1")
                || (Numbers.Num() > 0 && Numbers[0] != 0.0);
            return Store.SetParameterValue(FNiagaraBool(bValue), Variable, bAddIfMissing);
        }
        if (Type == FNiagaraTypeDefinition::GetVec2Def())
        {
            if (Numbers.Num() < 2) { return false; }
            return Store.SetParameterValue(FVector2f(Numbers[0], Numbers[1]), Variable, bAddIfMissing);
        }
        if (Type == FNiagaraTypeDefinition::GetVec3Def())
        {
            if (Numbers.Num() < 3) { return false; }
            return Store.SetParameterValue(FVector3f(Numbers[0], Numbers[1], Numbers[2]), Variable, bAddIfMissing);
        }
        if (Type == FNiagaraTypeDefinition::GetVec4Def())
        {
            if (Numbers.Num() < 4) { return false; }
            return Store.SetParameterValue(FVector4f(Numbers[0], Numbers[1], Numbers[2], Numbers[3]), Variable, bAddIfMissing);
        }
        if (Type == FNiagaraTypeDefinition::GetColorDef())
        {
            if (Numbers.Num() < 3) { return false; }
            const float Alpha = Numbers.Num() > 3 ? static_cast<float>(Numbers[3]) : 1.0f;
            return Store.SetParameterValue(FLinearColor(static_cast<float>(Numbers[0]), static_cast<float>(Numbers[1]),
                static_cast<float>(Numbers[2]), Alpha), Variable, bAddIfMissing);
        }
        if (Type == FNiagaraTypeDefinition::GetQuatDef())
        {
            if (Numbers.Num() < 4) { return false; }
            return Store.SetParameterValue(FQuat4f(Numbers[0], Numbers[1], Numbers[2], Numbers[3]), Variable, bAddIfMissing);
        }
        return false;
    }

    /** "Gravity" -> "User.Gravity"; already namespaced names are left alone. */
    FString NormalizeUserParameterName(const FString& InName)    {
        FString Name = InName.TrimStartAndEnd();
        if (Name.Contains(TEXT(".")))
        {
            return Name;
        }
        return FString::Printf(TEXT("User.%s"), *Name);
    }

    /**
     * Drop the system graph's UNiagaraNodeEmitter entries whose emitter handle is gone.
     *
     * The editor does this inside FNiagaraStackGraphUtilities::RebuildEmitterNodes, which is not
     * exported. Only the removal half is replicated here: the add half is covered because every
     * add goes through the exported FNiagaraEditorUtilities::AddEmitterToSystem, which calls the
     * real rebuild itself.
     */
    void PruneStaleEmitterNodes(UNiagaraSystem& System)
    {
        UNiagaraScript* SystemScript = System.GetSystemSpawnScript();
        UNiagaraScriptSource* Source = SystemScript ? Cast<UNiagaraScriptSource>(SystemScript->GetLatestSource()) : nullptr;
        UNiagaraGraph* Graph = Source ? Source->NodeGraph : nullptr;
        if (!Graph)
        {
            return;
        }

        UClass* EmitterNodeClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeEmitter"));
        if (!EmitterNodeClass)
        {
            return;
        }

        TSet<FGuid> LiveHandleIds;
        for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
        {
            LiveHandleIds.Add(Handle.GetId());
        }

        FStructProperty* HandleIdProperty =
            CastField<FStructProperty>(EmitterNodeClass->FindPropertyByName(TEXT("EmitterHandleId")));
        if (!HandleIdProperty || HandleIdProperty->Struct != TBaseStructure<FGuid>::Get())
        {
            return;
        }

        TArray<UEdGraphNode*> Nodes;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            Nodes.Add(Node);
        }
        Graph->Modify();
        for (UEdGraphNode* Node : Nodes)
        {
            if (!Node || !Node->IsA(EmitterNodeClass))
            {
                continue;
            }

            const FGuid HandleId = *HandleIdProperty->ContainerPtrToValuePtr<FGuid>(Node);
            if (LiveHandleIds.Contains(HandleId))
            {
                continue;
            }

            // Same relink the engine does: the node sits in the parameter-map chain, so its
            // neighbours have to be joined after it goes.
            UEdGraphPin* InPin = nullptr;
            UEdGraphPin* OutPin = nullptr;
            if (UNiagaraNode* NiagaraNode = Cast<UNiagaraNode>(Node))
            {
                InPin = NiagaraNode->GetInputPin(0);
                OutPin = NiagaraNode->GetOutputPin(0);
            }
            UEdGraphPin* InLinked = InPin && InPin->LinkedTo.Num() == 1 ? InPin->LinkedTo[0] : nullptr;
            UEdGraphPin* OutLinked = OutPin && OutPin->LinkedTo.Num() == 1 ? OutPin->LinkedTo[0] : nullptr;

            Node->Modify();
            Node->DestroyNode();
            if (InLinked && OutLinked)
            {
                InLinked->MakeLinkTo(OutLinked);
            }
        }

        Graph->NotifyGraphChanged();
    }
}

// =====================================================================
// Helpers
// =====================================================================

void FUnrealMCPNiagaraOps::Fail(FUnrealMCPNiagaraResult& Out, const FString& ErrorCode, const FString& ErrorMessage,
                                const TArray<FString>& Candidates)
{
    Out.bSuccess = false;
    Out.ErrorCode = ErrorCode;
    Out.ErrorMessage = ErrorMessage;
    Out.Candidates = Candidates;
}

bool FUnrealMCPNiagaraOps::ResolveSystem(const FString& AssetPath, UNiagaraSystem*& OutSystem,
                                         FUnrealMCPNiagaraResult& Out)
{
    OutSystem = nullptr;
    if (AssetPath.IsEmpty())
    {
        Fail(Out, TEXT("invalid_params"), TEXT("Missing 'asset_path'"));
        return false;
    }

    UObject* Asset = FUnrealMCPCommonUtils::FindAsset(AssetPath);
    if (!Asset)
    {
        // A typed load last: FindAsset only looks at loaded objects and the registry, and a system
        // that was just created by this plugin is resident but may not be in the registry yet.
        Asset = StaticLoadObject(UNiagaraSystem::StaticClass(), nullptr, *AssetPath);
    }

    UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset);
    if (!System)
    {
        if (!Asset)
        {
            const FString PIEHint = FUnrealMCPCommonUtils::ClassifyAssetLoadFailure(AssetPath);
            Fail(Out, PIEHint.IsEmpty() ? TEXT("asset_not_found") : PIEHint,
                 FString::Printf(TEXT("No asset at '%s'"), *AssetPath));
            return false;
        }
        Fail(Out, TEXT("not_a_niagara_system"),
             FString::Printf(TEXT("'%s' is a %s, not a Niagara system"), *AssetPath, *Asset->GetClass()->GetName()));
        return false;
    }

    OutSystem = System;
    Out.AssetPath = AssetPath;
    return true;
}

bool FUnrealMCPNiagaraOps::ResolveEmitter(UNiagaraSystem* System, const FString& IdOrName,
                                          FNiagaraEmitterHandle*& OutHandle, FUnrealMCPNiagaraResult& Out)
{
    OutHandle = nullptr;
    if (!System)
    {
        Fail(Out, TEXT("invalid_params"), TEXT("No system"));
        return false;
    }
    if (IdOrName.IsEmpty())
    {
        Fail(Out, TEXT("invalid_params"), TEXT("Missing 'emitter_name' (or 'emitter_id')"));
        return false;
    }

    TArray<FString> Candidates;
    for (FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
    {
        const FString EmitterName = Handle.GetName().ToString();
        if (Handle.GetId().ToString().Equals(IdOrName, ESearchCase::IgnoreCase)
            || EmitterName.Equals(IdOrName, ESearchCase::IgnoreCase))
        {
            OutHandle = &Handle;
            Out.EmitterId = Handle.GetId().ToString();
            Out.EmitterName = EmitterName;
            return true;
        }
        Candidates.Add(EmitterName);
    }

    Fail(Out, TEXT("emitter_not_found"),
         FString::Printf(TEXT("No emitter '%s' in '%s'"), *IdOrName, *System->GetName()), Candidates);
    return false;
}

// =====================================================================
// Asset lifecycle
// =====================================================================

bool FUnrealMCPNiagaraOps::CreateSystem(const FString& Name, const FString& Folder, FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    if (Name.IsEmpty())
    {
        Fail(Out, TEXT("invalid_params"), TEXT("Missing 'name'"));
        return false;
    }

    FString PackagePath = Folder.IsEmpty() ? FString(TEXT("/Game/VFX")) : Folder;
    PackagePath.RemoveFromEnd(TEXT("/"));
    if (!PackagePath.StartsWith(TEXT("/")))
    {
        PackagePath = TEXT("/") + PackagePath;
    }
    if (!FPackageName::IsValidLongPackageName(PackagePath))
    {
        Fail(Out, TEXT("invalid_params"), FString::Printf(TEXT("Invalid folder '%s'"), *Folder));
        return false;
    }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
    if (UObject* Existing = FUnrealMCPCommonUtils::FindAsset(FullPath))
    {
        if (!IsValid(Existing) || Existing->IsUnreachable())
        {
            Existing->MarkAsGarbage();
            CollectGarbage(RF_NoFlags);
        }
        else
        {
            Fail(Out, TEXT("asset_exists"),
                 FString::Printf(TEXT("Asset already exists: %s (%s); delete it or pass another name"),
                     *FullPath, *Existing->GetClass()->GetName()),
                 TArray<FString>{ Existing->GetClass()->GetName() });
            return false;
        }
    }

    UFactory* Factory = NewObject<UUnrealMCPNiagaraSystemFactory>(GetTransientPackage());
    UObject* NewAsset = FUnrealMCPCommonUtils::CreateAssetDirect(Name, PackagePath, UNiagaraSystem::StaticClass(), Factory);
    UNiagaraSystem* System = Cast<UNiagaraSystem>(NewAsset);
    if (!System)
    {
        Fail(Out, TEXT("create_failed"), FString::Printf(TEXT("Failed to create a Niagara system at '%s'"), *FullPath));
        return false;
    }

    Out.bSuccess = true;
    // The package path, not the object path: that is the form every other Niagara command accepts
    // as asset_path, so the result of this call can be fed straight back in.
    Out.AssetPath = System->GetOutermost()->GetName();
    return true;
}

bool FUnrealMCPNiagaraOps::DuplicateSystem(const FString& SourcePath, const FString& DestinationPath,
                                           FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* Source = nullptr;
    if (!ResolveSystem(SourcePath, Source, Out))
    {
        return false;
    }

    FString Destination = DestinationPath.TrimStartAndEnd();
    if (Destination.IsEmpty())
    {
        Destination = Source->GetOutermost()->GetName() + TEXT("_Copy");
    }
    if (!Destination.StartsWith(TEXT("/")))
    {
        Destination = TEXT("/") + Destination;
    }

    FString DestinationPackage = Destination;
    FString DestinationName;
    if (!DestinationPackage.Split(TEXT("/"), &DestinationPackage, &DestinationName, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
    {
        Fail(Out, TEXT("invalid_params"), FString::Printf(TEXT("Malformed destination path '%s'"), *DestinationPath));
        return false;
    }
    if (!FPackageName::IsValidLongPackageName(DestinationPackage))
    {
        Fail(Out, TEXT("invalid_params"), FString::Printf(TEXT("Invalid destination folder '%s'"), *DestinationPackage));
        return false;
    }

    if (UObject* Existing = FUnrealMCPCommonUtils::FindAsset(Destination))
    {
        Fail(Out, TEXT("asset_exists"), FString::Printf(TEXT("Asset already exists: %s"), *Destination));
        return false;
    }

    FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
    UObject* Duplicate = AssetToolsModule.Get().DuplicateAsset(DestinationName, DestinationPackage, Source);
    UNiagaraSystem* NewSystem = Cast<UNiagaraSystem>(Duplicate);
    if (!NewSystem)
    {
        Fail(Out, TEXT("duplicate_failed"),
             FString::Printf(TEXT("Duplicating '%s' to '%s' failed"), *SourcePath, *Destination));
        return false;
    }

    NewSystem->RequestCompile(false);
    NewSystem->WaitForCompilationComplete();

    Out.bSuccess = true;
    Out.AssetPath = NewSystem->GetOutermost()->GetName();
    return true;
}

// =====================================================================
// Emitters
// =====================================================================

bool FUnrealMCPNiagaraOps::ListEmitters(const FString& AssetPath, FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* System = nullptr;
    if (!ResolveSystem(AssetPath, System, Out))
    {
        return false;
    }

    for (FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
    {
        FUnrealMCPNiagaraEmitterInfo Info;
        FillEmitterInfo(Handle, Info);
        Out.Emitters.Add(Info);
    }

    Out.bSuccess = true;
    return true;
}

bool FUnrealMCPNiagaraOps::AddEmitter(const FString& AssetPath, const FString& EmitterName,
                                      const FString& SourceEmitterPath, bool bAddDefaultModules,
                                      FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* System = nullptr;
    if (!ResolveSystem(AssetPath, System, Out))
    {
        return false;
    }

    UNiagaraEmitter* SourceEmitter = nullptr;
    if (!SourceEmitterPath.IsEmpty())
    {
        SourceEmitter = Cast<UNiagaraEmitter>(FUnrealMCPCommonUtils::FindAsset(SourceEmitterPath));
        if (!SourceEmitter)
        {
            SourceEmitter = LoadObject<UNiagaraEmitter>(nullptr, *SourceEmitterPath);
        }
        if (!SourceEmitter)
        {
            Fail(Out, TEXT("emitter_asset_not_found"),
                 FString::Printf(TEXT("No emitter asset at '%s'"), *SourceEmitterPath));
            return false;
        }
    }

    // Always create a fresh emitter for the system: AddEmitterHandle copies the source into the
    // system package (NiagaraSystem.cpp:2774), so a loaded emitter asset is only ever a template.
    UNiagaraEmitter* NewEmitter = SourceEmitter;
    if (!NewEmitter)
    {
        NewEmitter = NewObject<UNiagaraEmitter>(GetTransientPackage());
        UNiagaraEmitterFactoryNew::InitializeEmitter(NewEmitter, bAddDefaultModules);
        NewEmitter->bIsInheritable = false;
        NewEmitter->SetFlags(RF_Transactional);
    }

    const FName UniqueName = FNiagaraEditorUtilities::GetUniqueObjectName(
        GetTransientPackage(), UNiagaraEmitter::StaticClass(),
        EmitterName.IsEmpty() ? FString(TEXT("Emitter")) : EmitterName);
    NewEmitter->SetUniqueEmitterName(UniqueName.ToString());

    const FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP add_niagara_emitter")));
    const FGuid HandleId = FNiagaraEditorUtilities::AddEmitterToSystem(*System, *NewEmitter, FGuid());

    // Read the handle back instead of echoing the request: the name may have been uniquified.
    FNiagaraEmitterHandle* Handle = nullptr;
    FUnrealMCPNiagaraResult ResolveResult;
    if (!ResolveEmitter(System, HandleId.ToString(), Handle, ResolveResult))
    {
        Fail(Out, TEXT("add_failed"), TEXT("Emitter was added but its handle could not be read back"));
        return false;
    }

    System->MarkPackageDirty();
    System->RequestCompile(false);
    System->WaitForCompilationComplete();

    Out.bSuccess = true;
    Out.EmitterId = HandleId.ToString();
    Out.EmitterName = Handle->GetName().ToString();
    Out.ModuleGuid = Handle->GetIdName().ToString();
    return true;
}

bool FUnrealMCPNiagaraOps::RemoveEmitter(const FString& AssetPath, const FString& EmitterIdOrName,
                                         FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* System = nullptr;
    if (!ResolveSystem(AssetPath, System, Out))
    {
        return false;
    }

    FNiagaraEmitterHandle* Handle = nullptr;
    if (!ResolveEmitter(System, EmitterIdOrName, Handle, Out))
    {
        return false;
    }

    const FString RemovedName = Handle->GetName().ToString();
    const FGuid RemovedId = Handle->GetId();

    const FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP remove_niagara_emitter")));
    System->Modify();

    // FNiagaraEditorUtilities::RemoveEmittersFromSystemByEmitterHandleId is not exported in 5.5
    // (NiagaraEditorUtilities.h:278 carries no API macro), so this is its two relevant halves:
    // the exported handle removal, then the emitter-node cleanup it does through RebuildEmitterNodes.
    System->RemoveEmitterHandlesById(TSet<FGuid>{ RemovedId });
    PruneStaleEmitterNodes(*System);

    // Instances keep a pointer to the emitter array they came from; they must go before the next
    // compile reads the modified handle list.
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (World)
    {
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            TArray<UNiagaraComponent*> Components;
            It->GetComponents<UNiagaraComponent>(Components);
            for (UNiagaraComponent* Component : Components)
            {
                if (Component && Component->GetAsset() == System)
                {
                    Component->DestroyInstance();
                }
            }
        }
    }

    System->MarkPackageDirty();
    System->RequestCompile(false);
    System->WaitForCompilationComplete();

    Out.bSuccess = true;
    Out.EmitterId = RemovedId.ToString();
    Out.EmitterName = RemovedName;
    return true;
}

namespace
{
    void FillEmitterInfo(const FNiagaraEmitterHandle& Handle, FUnrealMCPNiagaraEmitterInfo& OutInfo)
    {
        OutInfo.Id = Handle.GetId().ToString();
        OutInfo.Name = Handle.GetName().ToString();
        OutInfo.bEnabled = Handle.GetIsEnabled();

        const FVersionedNiagaraEmitter Instance = Handle.GetInstance();
        if (Instance.Emitter)
        {
            OutInfo.EmitterPath = Instance.Emitter->GetPathName();
        }

        FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
        if (!EmitterData)
        {
            return;
        }
        OutInfo.SimulationStageCount = EmitterData->GetSimulationStages().Num();
        for (const UNiagaraSimulationStageBase* Stage : EmitterData->GetSimulationStages())
        {
            if (Stage)
            {
                OutInfo.SimulationStageNames.Add(Stage->SimulationStageName.ToString());
            }
        }
        OutInfo.SimTarget = StaticEnum<ENiagaraSimTarget>()->GetNameStringByValue(
            static_cast<int64>(EmitterData->SimTarget));
        OutInfo.bLocalSpace = EmitterData->bLocalSpace;
        OutInfo.bDeterminism = EmitterData->bDeterminism;
        OutInfo.RandomSeed = EmitterData->RandomSeed;
        OutInfo.bInterpolatedSpawning = EmitterData->bInterpolatedSpawning != 0;
        OutInfo.PreAllocationCount = EmitterData->PreAllocationCount;
        // RendererProperties is private; GetRenderers() is the public accessor (NiagaraEmitter.h:377).
        for (const UNiagaraRendererProperties* Renderer : EmitterData->GetRenderers())
        {
            if (Renderer)
            {
                OutInfo.RendererClasses.Add(Renderer->GetClass()->GetName());
            }
        }
    }
}

bool FUnrealMCPNiagaraOps::SetEmitterProperties(const FString& AssetPath, const FString& EmitterIdOrName,
                                                const FUnrealMCPNiagaraEmitterEdits& Edits,
                                                FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* System = nullptr;
    if (!ResolveSystem(AssetPath, System, Out))
    {
        return false;
    }
    FNiagaraEmitterHandle* Handle = nullptr;
    if (!ResolveEmitter(System, EmitterIdOrName, Handle, Out))
    {
        return false;
    }

    FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();
    if (!EmitterData)
    {
        Fail(Out, TEXT("emitter_data_missing"), TEXT("The emitter handle has no emitter data"));
        return false;
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP set_niagara_emitter_properties")));
    System->Modify();
    if (UNiagaraEmitter* Emitter = Handle->GetInstance().Emitter)
    {
        Emitter->Modify();
    }

    if (Edits.bSimTarget)
    {
        const UEnum* SimTargetEnum = StaticEnum<ENiagaraSimTarget>();
        const int64 Value = SimTargetEnum ? SimTargetEnum->GetValueByNameString(Edits.SimTarget) : INDEX_NONE;
        if (Value == INDEX_NONE)
        {
            TArray<FString> Candidates;
            if (SimTargetEnum)
            {
                for (int32 Index = 0; Index < SimTargetEnum->NumEnums() - 1; ++Index)
                {
                    Candidates.Add(SimTargetEnum->GetNameStringByIndex(Index));
                }
            }
            Fail(Out, TEXT("unknown_sim_target"),
                 FString::Printf(TEXT("Unknown sim_target '%s'"), *Edits.SimTarget), Candidates);
            return false;
        }
        EmitterData->SimTarget = static_cast<ENiagaraSimTarget>(Value);
    }
    if (Edits.bLocalSpace)
    {
        EmitterData->bLocalSpace = Edits.LocalSpace;
    }
    if (Edits.bDeterminism)
    {
        EmitterData->bDeterminism = Edits.Determinism;
    }
    if (Edits.bRandomSeed)
    {
        EmitterData->RandomSeed = Edits.RandomSeed;
    }
    if (Edits.bInterpolatedSpawning)
    {
        EmitterData->bInterpolatedSpawning = Edits.InterpolatedSpawning ? 1 : 0;
    }
    if (Edits.bPreAllocationCount)
    {
        EmitterData->PreAllocationCount = Edits.PreAllocationCount;
    }

    System->MarkPackageDirty();
    System->RequestCompile(false);
    System->WaitForCompilationComplete();

    // Read the properties back off the emitter, so the response is the state, not the request.
    FUnrealMCPNiagaraEmitterInfo Info;
    FillEmitterInfo(*Handle, Info);
    Out.Emitters.Add(Info);
    Out.bSuccess = true;
    Out.EmitterId = Info.Id;
    Out.EmitterName = Info.Name;
    return true;
}

bool FUnrealMCPNiagaraOps::AddRenderer(const FString& AssetPath, const FString& EmitterIdOrName,
                                       const FString& RendererType, const FString& MaterialPath,
                                       FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* System = nullptr;
    if (!ResolveSystem(AssetPath, System, Out))
    {
        return false;
    }
    FNiagaraEmitterHandle* Handle = nullptr;
    if (!ResolveEmitter(System, EmitterIdOrName, Handle, Out))
    {
        return false;
    }
    const FVersionedNiagaraEmitter VersionedEmitter = Handle->GetInstance();
    UNiagaraEmitter* Emitter = VersionedEmitter.Emitter;
    if (!Emitter)
    {
        Fail(Out, TEXT("emitter_data_missing"), TEXT("The emitter handle has no emitter"));
        return false;
    }

    // "Sprite" / "NiagaraSpriteRendererProperties" / a full class name all land on the same class.
    const FString Wanted = RendererType.TrimStartAndEnd();
    UClass* RendererClass = nullptr;
    TArray<FString> RendererCandidates;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Candidate = *It;
        if (!Candidate->IsChildOf(UNiagaraRendererProperties::StaticClass())
            || Candidate->HasAnyClassFlags(CLASS_Abstract))
        {
            continue;
        }
        const FString ClassName = Candidate->GetName();
        RendererCandidates.Add(ClassName);
        if (ClassName.Equals(Wanted, ESearchCase::IgnoreCase)
            || ClassName.Equals(FString::Printf(TEXT("Niagara%sRendererProperties"), *Wanted), ESearchCase::IgnoreCase)
            || ClassName.Equals(FString::Printf(TEXT("%sRendererProperties"), *Wanted), ESearchCase::IgnoreCase))
        {
            RendererClass = Candidate;
            break;
        }
    }
    if (!RendererClass)
    {
        Fail(Out, TEXT("unknown_renderer_type"),
             FString::Printf(TEXT("'%s' is not a renderer class"), *RendererType), RendererCandidates);
        return false;
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP add_niagara_renderer")));
    System->Modify();
    Emitter->Modify();

    UNiagaraRendererProperties* Renderer = NewObject<UNiagaraRendererProperties>(
        Emitter, RendererClass, NAME_None, RF_Transactional);
    if (!Renderer)
    {
        Fail(Out, TEXT("create_failed"),
             FString::Printf(TEXT("Could not create a %s"), *RendererClass->GetName()));
        return false;
    }
    Renderer->Modify();

    if (!MaterialPath.IsEmpty())
    {
        UMaterialInterface* Material = Cast<UMaterialInterface>(FUnrealMCPCommonUtils::FindAsset(MaterialPath));
        if (!Material)
        {
            Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
        }
        if (!Material)
        {
            Fail(Out, TEXT("load_failed"), FString::Printf(TEXT("No material at '%s'"), *MaterialPath));
            return false;
        }

        FObjectProperty* MaterialProperty = nullptr;
        for (TFieldIterator<FObjectProperty> It(Renderer->GetClass()); It; ++It)
        {
            FObjectProperty* Candidate = *It;
            if (Candidate->PropertyClass
                && Candidate->PropertyClass->IsChildOf(UMaterialInterface::StaticClass())
                && Candidate->GetName().Contains(TEXT("Material"))
                && Candidate->HasAllPropertyFlags(CPF_Edit))
            {
                MaterialProperty = Candidate;
                break;
            }
        }
        if (!MaterialProperty)
        {
            Fail(Out, TEXT("material_property_not_found"),
                 FString::Printf(TEXT("Renderer '%s' has no editable material property"),
                     *Renderer->GetClass()->GetName()));
            return false;
        }
        MaterialProperty->SetObjectPropertyValue_InContainer(Renderer, Material);
        Out.Value = Material->GetPathName();
    }

    Emitter->AddRenderer(Renderer, VersionedEmitter.Version);
#if WITH_EDITOR
    // Fills in the default bindings (position / color / size) the renderer needs to draw at all.
    Renderer->PostEditChange();
#endif

    System->MarkPackageDirty();
    System->RequestCompile(false);
    System->WaitForCompilationComplete();

    FVersionedNiagaraEmitterData* AfterData = Handle->GetEmitterData();
    const TArray<UNiagaraRendererProperties*>& Renderers = AfterData ? AfterData->GetRenderers()
                                                                    : TArray<UNiagaraRendererProperties*>();
    for (UNiagaraRendererProperties* Existing : Renderers)
    {
        Out.Candidates.Add(Existing ? Existing->GetClass()->GetName() : TEXT("null"));
    }

    Out.bSuccess = true;
    Out.EmitterName = Handle->GetUniqueInstanceName();
    Out.ModuleName = Renderer->GetClass()->GetName();
    Out.ParamName = FString::Printf(TEXT("%d"), Renderers.Num() - 1);
    return true;
}

bool FUnrealMCPNiagaraOps::RemoveRenderer(const FString& AssetPath, const FString& EmitterIdOrName,
                                          int32 RendererIndex, FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* System = nullptr;
    if (!ResolveSystem(AssetPath, System, Out))
    {
        return false;
    }
    FNiagaraEmitterHandle* Handle = nullptr;
    if (!ResolveEmitter(System, EmitterIdOrName, Handle, Out))
    {
        return false;
    }
    UNiagaraEmitter* Emitter = Handle->GetInstance().Emitter;
    FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();
    if (!Emitter || !EmitterData)
    {
        Fail(Out, TEXT("emitter_data_missing"), TEXT("The emitter handle has no emitter"));
        return false;
    }

    const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
    if (!Renderers.IsValidIndex(RendererIndex))
    {
        TArray<FString> Candidates;
        for (int32 Index = 0; Index < Renderers.Num(); ++Index)
        {
            const UNiagaraRendererProperties* Renderer = Renderers[Index];
            Candidates.Add(FString::Printf(TEXT("%d: %s"), Index,
                Renderer ? *Renderer->GetClass()->GetName() : TEXT("null")));
        }
        Fail(Out, TEXT("renderer_not_found"),
             FString::Printf(TEXT("Emitter '%s' has no renderer %d"), *Handle->GetName().ToString(), RendererIndex),
             Candidates);
        return false;
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP remove_niagara_renderer")));
    System->Modify();
    Emitter->Modify();

    UNiagaraRendererProperties* Renderer = Renderers[RendererIndex];
    Out.ModuleName = Renderer ? Renderer->GetClass()->GetName() : FString();
    Emitter->RemoveRenderer(Renderer, Handle->GetInstance().Version);

    System->MarkPackageDirty();
    System->RequestCompile(false);
    System->WaitForCompilationComplete();

    FVersionedNiagaraEmitterData* AfterData = Handle->GetEmitterData();
    const TArray<UNiagaraRendererProperties*>& Remaining = AfterData ? AfterData->GetRenderers()
                                                                     : TArray<UNiagaraRendererProperties*>();
    for (UNiagaraRendererProperties* Existing : Remaining)
    {
        Out.Candidates.Add(Existing ? Existing->GetClass()->GetName() : TEXT("null"));
    }
    Out.bSuccess = true;
    Out.EmitterName = Handle->GetUniqueInstanceName();
    return true;
}

bool FUnrealMCPNiagaraOps::SetRendererMaterial(const FString& AssetPath, const FString& EmitterIdOrName,                                               const FString& MaterialPath, int32 RendererIndex,
                                               FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* System = nullptr;
    if (!ResolveSystem(AssetPath, System, Out))
    {
        return false;
    }
    FNiagaraEmitterHandle* Handle = nullptr;
    if (!ResolveEmitter(System, EmitterIdOrName, Handle, Out))
    {
        return false;
    }
    FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();
    if (!EmitterData)
    {
        Fail(Out, TEXT("emitter_data_missing"), TEXT("The emitter handle has no emitter data"));
        return false;
    }
    const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
    if (!Renderers.IsValidIndex(RendererIndex))
    {
        TArray<FString> Candidates;
        for (int32 Index = 0; Index < Renderers.Num(); ++Index)
        {
            const UNiagaraRendererProperties* Renderer = Renderers[Index];
            Candidates.Add(FString::Printf(TEXT("%d: %s"), Index,
                Renderer ? *Renderer->GetClass()->GetName() : TEXT("null")));
        }
        Fail(Out, TEXT("renderer_not_found"),
             FString::Printf(TEXT("Emitter '%s' has no renderer %d"), *Handle->GetName().ToString(), RendererIndex),
             Candidates);
        return false;
    }

    UNiagaraRendererProperties* Renderer = Renderers[RendererIndex];

    // The material property is named differently per renderer type (Sprite/Mesh/Ribbon/Decal), so it
    // is found by shape - "an object property that takes a material" - instead of by a name table.
    FObjectProperty* MaterialProperty = nullptr;
    for (TFieldIterator<FObjectProperty> It(Renderer->GetClass()); It; ++It)
    {
        FObjectProperty* Candidate = *It;
        if (Candidate->PropertyClass
            && Candidate->PropertyClass->IsChildOf(UMaterialInterface::StaticClass())
            && Candidate->GetName().Contains(TEXT("Material"))
            && Candidate->HasAllPropertyFlags(CPF_Edit))
        {
            MaterialProperty = Candidate;
            break;
        }
    }
    if (!MaterialProperty)
    {
        Fail(Out, TEXT("material_property_not_found"),
             FString::Printf(TEXT("Renderer '%s' has no editable material property"),
                 *Renderer->GetClass()->GetName()));
        return false;
    }

    UMaterialInterface* Material = nullptr;
    if (!MaterialPath.IsEmpty())
    {
        Material = Cast<UMaterialInterface>(FUnrealMCPCommonUtils::FindAsset(MaterialPath));
        if (!Material)
        {
            Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
        }
        if (!Material)
        {
            Fail(Out, TEXT("material_not_found"), FString::Printf(TEXT("No material at '%s'"), *MaterialPath));
            return false;
        }
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP set_niagara_renderer_material")));
    System->Modify();
    Renderer->Modify();
    MaterialProperty->SetObjectPropertyValue_InContainer(Renderer, Material);
    FPropertyChangedEvent Event(MaterialProperty, EPropertyChangeType::ValueSet);
    Renderer->PostEditChangeProperty(Event);

    System->MarkPackageDirty();

    // Read back what the renderer holds now (the property, not the requested path).
    FString Readback = TEXT("null");
    UObject* Assigned = MaterialProperty->GetObjectPropertyValue_InContainer(Renderer);
    if (Assigned)
    {
        Readback = Assigned->GetPathName();
    }

    Out.bSuccess = true;
    Out.EmitterId = Handle->GetId().ToString();
    Out.EmitterName = Handle->GetName().ToString();
    Out.AssetPath = AssetPath;
    Out.ScriptPath = Readback;
    Out.ParamName = MaterialProperty->GetName();
    Out.Value = Readback;
    Out.Modules.Reset();
    {
        FUnrealMCPNiagaraEmitterInfo Info;
        FillEmitterInfo(*Handle, Info);
        Out.Emitters.Add(Info);
    }
    return true;
}

bool FUnrealMCPNiagaraOps::AddSimulationStage(const FString& AssetPath, const FString& EmitterIdOrName,
                                              const FString& StageName, const FString& IterationSource,
                                              bool bEnabled, FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* System = nullptr;
    if (!ResolveSystem(AssetPath, System, Out))
    {
        return false;
    }
    FNiagaraEmitterHandle* Handle = nullptr;
    if (!ResolveEmitter(System, EmitterIdOrName, Handle, Out))
    {
        return false;
    }

    FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();
    UNiagaraEmitter* Emitter = Handle->GetInstance().Emitter;
    if (!EmitterData || !Emitter)
    {
        Fail(Out, TEXT("emitter_data_missing"), TEXT("The emitter handle has no emitter data"));
        return false;
    }

    UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
    UNiagaraGraph* Graph = Source ? Source->NodeGraph : nullptr;
    if (!Graph)
    {
        Fail(Out, TEXT("graph_unavailable"), TEXT("The emitter has no graph source to hang the stage on"));
        return false;
    }

    ENiagaraIterationSource Iteration = ENiagaraIterationSource::Particles;
    if (!IterationSource.IsEmpty())
    {
        const UEnum* Enum = StaticEnum<ENiagaraIterationSource>();
        const int64 Value = Enum ? Enum->GetValueByNameString(IterationSource) : INDEX_NONE;
        if (Value == INDEX_NONE)
        {
            TArray<FString> Candidates;
            for (int32 Index = 0; Enum && Index < Enum->NumEnums() - 1; ++Index)
            {
                Candidates.Add(Enum->GetNameStringByIndex(Index));
            }
            Fail(Out, TEXT("unknown_iteration_source"),
                 FString::Printf(TEXT("Unknown iteration_source '%s'"), *IterationSource), Candidates);
            return false;
        }
        Iteration = static_cast<ENiagaraIterationSource>(Value);
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP add_niagara_simulation_stage")));
    System->Modify();
    Emitter->Modify();
    Graph->Modify();

    UNiagaraSimulationStageGeneric* Stage = NewObject<UNiagaraSimulationStageGeneric>(
        Emitter, NAME_None, RF_Transactional);
    if (!Stage)
    {
        Fail(Out, TEXT("create_failed"), TEXT("Could not create the simulation stage"));
        return false;
    }
    Stage->Modify();
    Stage->SimulationStageName = StageName.IsEmpty() ? FName(TEXT("Simulation Stage")) : FName(*StageName);
    Stage->bEnabled = bEnabled ? 1 : 0;
    Stage->IterationSource = Iteration;
    Stage->OuterEmitterVersion = Handle->GetInstance().Version;

    // The compiler reads the stage's identity back off its script (StageGuid = Script->GetUsageId()).
    UNiagaraScript* StageScript = NewObject<UNiagaraScript>(Stage, FName(TEXT("SimulationStageScript")),
                                                            RF_Transactional);
    StageScript->SetUsage(ENiagaraScriptUsage::ParticleSimulationStageScript);
    StageScript->SetUsageId(Stage->GetMergeId());
    StageScript->SetLatestSource(EmitterData->GraphSource);
    Stage->Script = StageScript;

    // UNiagaraEmitter::AddSimulationStage is not exported and the array is private, so the stage goes
    // in by reflection. The editor's own delegates are not broadcast - the compile below is what
    // proves the stage is wired, and the stack UI picks it up when the asset editor is (re)opened.
    FArrayProperty* StagesProperty = CastField<FArrayProperty>(
        FVersionedNiagaraEmitterData::StaticStruct()->FindPropertyByName(TEXT("SimulationStages")));
    FObjectProperty* StagesInner = StagesProperty ? CastField<FObjectProperty>(StagesProperty->Inner) : nullptr;
    if (!StagesProperty || !StagesInner)
    {
        Fail(Out, TEXT("simulation_stages_unavailable"),
             TEXT("The emitter's SimulationStages array is not reachable through reflection"));
        return false;
    }
    FScriptArrayHelper StagesHelper(StagesProperty, StagesProperty->ContainerPtrToValuePtr<void>(EmitterData));
    StagesInner->SetObjectPropertyValue(StagesHelper.GetRawPtr(StagesHelper.AddValue()), Stage);

    // The stage's graph output. FNiagaraStackGraphUtilities::ResetGraphForOutput is not exported, so
    // the two nodes it would create are created here (SetUsage / SetUsageId are inline).
    const FGuid UsageId = StageScript->GetUsageId();
    if (!Graph->FindEquivalentOutputNode(ENiagaraScriptUsage::ParticleSimulationStageScript, UsageId))
    {
        FGraphNodeCreator<UNiagaraNodeOutput> OutputCreator(*Graph);
        UNiagaraNodeOutput* OutputNode = OutputCreator.CreateNode();
        OutputNode->SetUsage(ENiagaraScriptUsage::ParticleSimulationStageScript);
        OutputNode->SetUsageId(UsageId);
        OutputNode->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Out")));
        OutputCreator.Finalize();

        FGraphNodeCreator<UNiagaraNodeInput> InputCreator(*Graph);
        UNiagaraNodeInput* InputNode = InputCreator.CreateNode();
        InputNode->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("InputMap"));
        InputNode->Usage = ENiagaraInputNodeUsage::Parameter;
        InputCreator.Finalize();

        if (UEdGraphPin* OutputIn = OutputNode->GetInputPin(0))
        {
            if (UEdGraphPin* InputOut = InputNode->GetOutputPin(0))
            {
                InputOut->MakeLinkTo(OutputIn);
            }
        }
    }

    Graph->NotifyGraphChanged();
    System->MarkPackageDirty();
    System->RequestCompile(false);
    System->WaitForCompilationComplete();

    FUnrealMCPNiagaraEmitterInfo Info;
    FillEmitterInfo(*Handle, Info);
    Out.Emitters.Add(Info);
    Out.AssetPath = AssetPath;
    Out.EmitterId = Info.Id;
    Out.EmitterName = Info.Name;
    Out.ScriptPath = StageScript->GetPathName();
    Out.ParamName = Stage->SimulationStageName.ToString();
    Out.ValueType = StaticEnum<ENiagaraIterationSource>()->GetNameStringByValue(
        static_cast<int64>(Stage->IterationSource));
    Out.bEnabled = Stage->bEnabled != 0;
    Out.bSuccess = true;
    return true;
}

bool FUnrealMCPNiagaraOps::SetSimulationStage(const FString& AssetPath, const FString& EmitterIdOrName,
                                              const FString& StageRef, const FString& StageName,
                                              bool bSetEnabled, bool bEnabled, const FString& IterationSource,
                                              bool bSetNumIterations, int32 NumIterations,
                                              const FString& DataInterfaceParameter,
                                              FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* System = nullptr;
    if (!ResolveSystem(AssetPath, System, Out))
    {
        return false;
    }
    FNiagaraEmitterHandle* Handle = nullptr;
    if (!ResolveEmitter(System, EmitterIdOrName, Handle, Out))
    {
        return false;
    }
    FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();
    if (!EmitterData)
    {
        Fail(Out, TEXT("emitter_data_missing"), TEXT("The emitter handle has no emitter data"));
        return false;
    }

    const TArray<UNiagaraSimulationStageBase*>& Stages = EmitterData->GetSimulationStages();
    UNiagaraSimulationStageBase* Stage = nullptr;
    TArray<FString> Candidates;
    const int32 StageIndex = FCString::IsNumeric(*StageRef) ? FCString::Atoi(*StageRef) : INDEX_NONE;
    for (int32 Index = 0; Index < Stages.Num(); ++Index)
    {
        UNiagaraSimulationStageBase* Candidate = Stages[Index];
        if (!Candidate)
        {
            continue;
        }
        const FString Name = Candidate->SimulationStageName.ToString();
        Candidates.Add(FString::Printf(TEXT("%d: %s"), Index, *Name));
        if (Index == StageIndex || Name.Equals(StageRef, ESearchCase::IgnoreCase))
        {
            Stage = Candidate;
            break;
        }
    }
    if (!Stage)
    {
        Fail(Out, TEXT("stage_not_found"),
             FString::Printf(TEXT("Emitter '%s' has no simulation stage '%s'"), *Handle->GetName().ToString(),
                 *StageRef),
             Candidates);
        return false;
    }

    UNiagaraSimulationStageGeneric* GenericStage = Cast<UNiagaraSimulationStageGeneric>(Stage);

    const FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP set_niagara_simulation_stage")));
    System->Modify();
    Stage->Modify();

    if (!StageName.IsEmpty())
    {
        Stage->SimulationStageName = FName(*StageName);
    }

    if (bSetEnabled)
    {
        // A bitfield: not writable through the generic reflector, but the engine exports the setter.
        Stage->SetEnabled(bEnabled);
    }

    ENiagaraIterationSource Iteration = ENiagaraIterationSource::Particles;
    if (!IterationSource.IsEmpty())
    {
        const UEnum* Enum = StaticEnum<ENiagaraIterationSource>();
        const int64 Value = Enum ? Enum->GetValueByNameString(IterationSource) : INDEX_NONE;
        if (Value == INDEX_NONE || !GenericStage)
        {
            TArray<FString> IterationCandidates;
            for (int32 Index = 0; Enum && Index < Enum->NumEnums() - 1; ++Index)
            {
                IterationCandidates.Add(Enum->GetNameStringByIndex(Index));
            }
            Fail(Out, TEXT("unknown_iteration_source"),
                 FString::Printf(TEXT("Unknown iteration_source '%s'"), *IterationSource), IterationCandidates);
            return false;
        }
        GenericStage->IterationSource = static_cast<ENiagaraIterationSource>(Value);
    }

    if (bSetNumIterations)
    {
        if (!GenericStage)
        {
            Fail(Out, TEXT("unsupported_stage_type"),
                 FString::Printf(TEXT("Stage '%s' is a %s, which has no iteration count"),
                     *Stage->SimulationStageName.ToString(), *Stage->GetClass()->GetName()));
            return false;
        }
        // The value lives in a byte blob whose width the property itself reports; writing it through
        // the exported SetDefaultValueEditorOnly avoids guessing the struct's setter.
        const int32 ExpectedSize = static_cast<int32>(sizeof(int32));
        const int32 CurrentSize = GenericStage->NumIterations.GetDefaultValueArray().Num();
        if (CurrentSize != 0 && CurrentSize != ExpectedSize)
        {
            Fail(Out, TEXT("unsupported_iteration_value_size"),
                 FString::Printf(TEXT("NumIterations holds a %d-byte value; this command writes int32"),
                     CurrentSize));
            return false;
        }
        const int32 Value = NumIterations;
        GenericStage->NumIterations.SetDefaultValueEditorOnly(reinterpret_cast<const uint8*>(&Value));
    }

    if (!DataInterfaceParameter.IsEmpty())
    {
        if (!GenericStage)
        {
            Fail(Out, TEXT("unsupported_stage_type"),
                 FString::Printf(TEXT("Stage '%s' is a %s, which has no DataInterface binding"),
                     *Stage->SimulationStageName.ToString(), *Stage->GetClass()->GetName()));
            return false;
        }

        // Semantic on purpose: the caller names a parameter, and it has to be a DataInterface one.
        const FString FullName = NormalizeUserParameterName(DataInterfaceParameter);
        const FString WantedTail = NameTail(FullName);
        FNiagaraVariable Variable;
        TArray<FNiagaraVariable> UserParameters;
        System->GetExposedParameters().GetUserParameters(UserParameters);
        for (const FNiagaraVariable& Candidate : UserParameters)
        {
            const FString CandidateName = Candidate.GetName().ToString();
            if (CandidateName.Equals(FullName, ESearchCase::IgnoreCase)
                || NameTail(CandidateName).Equals(WantedTail, ESearchCase::IgnoreCase))
            {
                // The redirection store hands user parameters back under their short key (parameters added
                // straight to the store), but the compiler matches a simulation stage's iteration source
                // against the parameter map name, which is always namespaced. Writing the short name here
                // compiles to "Variable <short> missing in graphs referenced during compile".
                Variable = FNiagaraVariable(Candidate.GetType(),
                                            FName(*NormalizeUserParameterName(CandidateName)));
                break;
            }
        }
        if (Variable.GetName().IsNone())
        {
            TArray<FString> ParameterCandidates;
            for (const FNiagaraVariable& Candidate : UserParameters)
            {
                ParameterCandidates.Add(Candidate.GetName().ToString());
            }
            Fail(Out, TEXT("parameter_not_found"),
                 FString::Printf(TEXT("'%s' is not a user parameter; create it with "
                                     "set_niagara_data_interface_properties"), *FullName),
                 ParameterCandidates);
            return false;
        }
        if (Variable.GetType().GetClass() == nullptr
            || !Variable.GetType().GetClass()->IsChildOf(UNiagaraDataInterface::StaticClass()))
        {
            Fail(Out, TEXT("not_a_data_interface_parameter"),
                 FString::Printf(TEXT("'%s' is a %s parameter, not a DataInterface one"), *FullName,
                     *TypeNameOf(Variable.GetType())));
            return false;
        }

        GenericStage->DataInterface.BoundVariable = Variable;
        // A bound DataInterface is only used when the stage iterates over one.
        if (IterationSource.IsEmpty())
        {
            GenericStage->IterationSource = ENiagaraIterationSource::DataInterface;
        }
    }

    System->MarkPackageDirty();
    System->RequestCompile(false);
    System->WaitForCompilationComplete();

    // Read the stage back off the emitter, so the response is the state rather than the request.
    FUnrealMCPNiagaraEmitterInfo Info;
    FillEmitterInfo(*Handle, Info);
    Out.Emitters.Add(Info);
    Out.AssetPath = AssetPath;
    Out.EmitterId = Info.Id;
    Out.EmitterName = Info.Name;
    Out.ParamName = Stage->SimulationStageName.ToString();
    Out.bEnabled = Stage->bEnabled != 0;
    if (GenericStage)
    {
        Out.ValueType = StaticEnum<ENiagaraIterationSource>()->GetNameStringByValue(
            static_cast<int64>(GenericStage->IterationSource));
        Out.Value = FString::FromInt(NumIterations);
        Out.ScriptPath = GenericStage->DataInterface.BoundVariable.GetName().ToString();
    }
    Out.bSuccess = true;
    return true;
}

// =====================================================================
// Compile
// =====================================================================
bool FUnrealMCPNiagaraOps::CompileSystem(const FString& AssetPath, bool bWait, FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* System = nullptr;
    if (!ResolveSystem(AssetPath, System, Out))
    {
        return false;
    }

    System->RequestCompile(/*bForce=*/false);
    if (bWait)
    {
        // Synchronous on purpose: every other command in this domain reports a result that only
        // exists after the compile, so the caller must not have to poll.
        System->WaitForCompilationComplete();
    }
    else
    {
        System->PollForCompilationComplete();
    }

    CollectCompileEvents(System, Out);
    Out.bSuccess = true;
    return true;
}

// =====================================================================
// User parameters
// =====================================================================

bool FUnrealMCPNiagaraOps::GetParameters(const FString& AssetPath, FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* System = nullptr;
    if (!ResolveSystem(AssetPath, System, Out))
    {
        return false;
    }

    FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
    TArray<FNiagaraVariable> UserParameters;
    Store.GetUserParameters(UserParameters);

    for (const FNiagaraVariable& Variable : UserParameters)
    {
        FUnrealMCPNiagaraParameterInfo Info;
        Info.Name = Variable.GetName().ToString();
        Info.Type = TypeNameOf(Variable.GetType());
        if (!ReadParameterValue(Store, Variable, Info.Value))
        {
            Info.Value = TEXT("<unreadable>");
        }
        Out.Parameters.Add(Info);
    }

    Out.bSuccess = true;
    return true;
}

bool FUnrealMCPNiagaraOps::SetParameter(const FString& AssetPath, const FString& ParamName, const FString& ParamType,
                                        const FString& Value, bool bAddIfMissing, FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* System = nullptr;
    if (!ResolveSystem(AssetPath, System, Out))
    {
        return false;
    }
    if (ParamName.IsEmpty())
    {
        Fail(Out, TEXT("invalid_params"), TEXT("Missing 'param_name'"));
        return false;
    }

    const FString FullName = NormalizeUserParameterName(ParamName);
    FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();

    // Existing parameter wins over the requested type: writing a float into a vec3 slot would
    // silently corrupt the layout, so a mismatch is an error with the real type in it.
    // GetUserParameters reports the redirection store's SHORT keys ("WaterRT"), so an exact compare
    // against the normalized name ("User.WaterRT") never matches - every write would look like a new
    // parameter, and a type mismatch would silently add a second variable of the same name instead of
    // being refused. Compare the name tail, like the other parameter commands do.
    const FString WantedTail = NameTail(FullName);
    FNiagaraTypeDefinition Type;
    FNiagaraVariable Existing;
    TArray<FNiagaraVariable> UserParameters;
    Store.GetUserParameters(UserParameters);
    for (const FNiagaraVariable& Variable : UserParameters)
    {
        const FString CandidateName = Variable.GetName().ToString();
        if (CandidateName.Equals(FullName, ESearchCase::IgnoreCase)
            || NameTail(CandidateName).Equals(WantedTail, ESearchCase::IgnoreCase))
        {
            Existing = Variable;
            Type = Variable.GetType();
            break;
        }
    }

    if (Existing.GetName().IsNone() && !bAddIfMissing)
    {
        TArray<FString> Candidates;
        for (const FNiagaraVariable& Variable : UserParameters)
        {
            Candidates.Add(Variable.GetName().ToString());
        }
        Fail(Out, TEXT("parameter_not_found"),
             FString::Printf(TEXT("'%s' is not a user parameter of '%s' (pass add_if_missing=true to create it)"),
                 *FullName, *System->GetName()),
             Candidates);
        return false;
    }

    if (!ParamType.IsEmpty())
    {
        FNiagaraTypeDefinition RequestedType;
        if (!ResolvePinType(ParamType, RequestedType))
        {
            Fail(Out, TEXT("unknown_parameter_type"),
                 FString::Printf(TEXT("Unknown param_type '%s'"), *ParamType), ParameterTypeCandidates());
            return false;
        }
        if (Existing.GetName().IsNone())
        {
            Type = RequestedType;
        }
        else if (Type != RequestedType)
        {
            Fail(Out, TEXT("parameter_type_mismatch"),
                 FString::Printf(TEXT("'%s' is a %s parameter, not %s"), *FullName, *TypeNameOf(Type), *ParamType),
                 TArray<FString>{ TypeNameOf(Type) });
            return false;
        }
    }

    if (Type == FNiagaraTypeDefinition::GetWildcardDef() || Type.GetName().IsEmpty())
    {
        Fail(Out, TEXT("unknown_parameter_type"),
             FString::Printf(TEXT("Could not determine the type of '%s'; pass param_type"), *FullName),
             ParameterTypeCandidates());
        return false;
    }
    if (Type.GetClass() != nullptr)
    {
        UClass* ParameterClass = Type.GetClass();
        if (ParameterClass->IsChildOf(UNiagaraDataInterface::StaticClass()))
        {
            Fail(Out, TEXT("unsupported_parameter_type"),
                 FString::Printf(TEXT("'%s' is a %s (data interface) parameter; a data interface is an object, "
                                     "not text - configure it with set_niagara_data_interface_properties"),
                     *FullName, *TypeNameOf(Type)),
                 ParameterTypeCandidates());
            return false;
        }

        // An object-valued user parameter: a RenderTarget / Texture / Mesh that a data interface or a
        // renderer reads. The value is an asset path; "" or "None" clears it.
        UObject* Object = nullptr;
        if (!Value.IsEmpty() && !Value.Equals(TEXT("None"), ESearchCase::IgnoreCase))
        {
            Object = LoadObject<UObject>(nullptr, *Value);
            if (!Object)
            {
                Object = FUnrealMCPCommonUtils::FindAsset(Value);
            }
            if (!Object)
            {
                Fail(Out, TEXT("load_failed"), FString::Printf(TEXT("No asset at '%s'"), *Value));
                return false;
            }
            if (!Object->IsA(ParameterClass))
            {
                Fail(Out, TEXT("type_mismatch"),
                     FString::Printf(TEXT("'%s' is a %s parameter, and '%s' is a %s"), *FullName,
                         *TypeNameOf(Type), *Value, *Object->GetClass()->GetName()),
                     TArray<FString>{ TypeNameOf(Type), TEXT("asset path string") });
                return false;
            }
        }

        const FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP set_niagara_parameter")));
        System->Modify();

        FNiagaraVariable ObjectVariable(Type, FName(*FullName));
        Store.AddParameter(ObjectVariable, /*bInitialize=*/true, /*bTriggerRebind=*/true);
        Store.SetUObject(Object, ObjectVariable);
        System->MarkPackageDirty();

        const UObject* Stored = Store.GetUObject(ObjectVariable);
        Out.bSuccess = true;
        Out.ParamName = FullName;
        Out.ValueType = TypeNameOf(Type);
        Out.Value = Stored ? Stored->GetPathName() : FString(TEXT("None"));
        return true;
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP set_niagara_parameter")));
    System->Modify();

    FNiagaraVariable Variable(Type, FName(*FullName));
    if (!WriteParameterValue(Store, Variable, Value, /*bAddIfMissing=*/true))
    {
        Fail(Out, TEXT("write_failed"),
             FString::Printf(TEXT("Could not write '%s' (%s) from '%s'"), *FullName, *TypeNameOf(Type), *Value));
        return false;
    }

    // Read back from the store, not from the request.
    FString Readback;
    if (!ReadParameterValue(Store, Variable, Readback))
    {
        Fail(Out, TEXT("readback_failed"),
             FString::Printf(TEXT("'%s' was written but cannot be read back"), *FullName));
        return false;
    }

    System->MarkPackageDirty();

    Out.bSuccess = true;
    Out.Value = Readback;
    Out.ValueType = TypeNameOf(Type);
    Out.ParamName = FullName;
    return true;
}

bool FUnrealMCPNiagaraOps::SetDataInterfaceProperties(const FString& AssetPath, const FString& ParamName,
                                                      const FString& DataInterfaceType,
                                                      const TArray<TPair<FString, FString>>& Properties,
                                                      FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* System = nullptr;
    if (!ResolveSystem(AssetPath, System, Out))
    {
        return false;
    }
    if (ParamName.IsEmpty())
    {
        Fail(Out, TEXT("invalid_params"), TEXT("Missing 'param_name'"));
        return false;
    }

    const FString FullName = NormalizeUserParameterName(ParamName);
    FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();

    // The user redirection store keeps BOTH forms: parameters created through the stack are listed
    // with their "User." prefix, ones added straight to the store come back without it. Matching on
    // the name after the last dot accepts either, and still separates two parameters that only differ
    // by namespace.
    const FString WantedTail = NameTail(FullName);

    // Find the parameter; a DataInterface parameter's type is the DataInterface class itself.
    FNiagaraVariable Variable;
    TArray<FNiagaraVariable> UserParameters;
    Store.GetUserParameters(UserParameters);
    for (const FNiagaraVariable& Candidate : UserParameters)
    {
        const FString CandidateName = Candidate.GetName().ToString();
        if (CandidateName.Equals(FullName, ESearchCase::IgnoreCase)
            || NameTail(CandidateName).Equals(WantedTail, ESearchCase::IgnoreCase))
        {
            Variable = Candidate;
            break;
        }
    }

    UNiagaraDataInterface* DataInterface = nullptr;
    if (Variable.GetName().IsNone())
    {
        if (DataInterfaceType.IsEmpty())
        {
            TArray<FString> Candidates;
            for (const FNiagaraVariable& Candidate : UserParameters)
            {
                Candidates.Add(Candidate.GetName().ToString());
            }
            Fail(Out, TEXT("parameter_not_found"),
                 FString::Printf(TEXT("'%s' is not a user parameter of '%s'; pass data_interface_type to create it "
                                     "(e.g. \"Grid2DCollection\")"), *FullName, *System->GetName()),
                 Candidates);
            return false;
        }

        TArray<FString> ClassCandidates;
        UClass* DataInterfaceClass = ResolveDataInterfaceClass(DataInterfaceType, ClassCandidates);
        if (!DataInterfaceClass)
        {
            Fail(Out, TEXT("unknown_data_interface_type"),
                 FString::Printf(TEXT("No UNiagaraDataInterface class matches '%s'"), *DataInterfaceType),
                 ClassCandidates);
            return false;
        }

        const FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP create_niagara_data_interface")));
        System->Modify();

        DataInterface = NewObject<UNiagaraDataInterface>(System, DataInterfaceClass, NAME_None, RF_Transactional);
        if (!DataInterface)
        {
            Fail(Out, TEXT("create_failed"),
                 FString::Printf(TEXT("Could not create a %s"), *DataInterfaceClass->GetName()));
            return false;
        }

        Variable = FNiagaraVariable(FNiagaraTypeDefinition(DataInterfaceClass), FName(*FullName));
        Store.AddParameter(Variable, /*bInitialize=*/true, /*bTriggerRebind=*/true);
        Store.SetDataInterface(DataInterface, Variable);
        System->MarkPackageDirty();
    }
    else
    {
        DataInterface = Store.GetDataInterface(Variable);
        if (!DataInterface)
        {
            Fail(Out, TEXT("not_a_data_interface_parameter"),
                 FString::Printf(TEXT("'%s' is a %s parameter, not a DataInterface parameter"), *FullName,
                     *TypeNameOf(Variable.GetType())));
            return false;
        }
    }

    // Which properties can be written: everything editable that is not the DataInterface's own
    // bookkeeping. Reported on request (no property map) and as candidates when a name misses.
    TArray<FString> EditableProperties;
    for (TFieldIterator<FProperty> It(DataInterface->GetClass()); It; ++It)
    {
        FProperty* Property = *It;
        if (Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_EditConst)
            && !Property->HasAnyPropertyFlags(CPF_Transient))
        {
            EditableProperties.Add(FString::Printf(TEXT("%s (%s)"), *Property->GetName(),
                                                   *Property->GetCPPType()));
        }
    }

    if (Properties.Num() == 0)
    {
        Out.bSuccess = true;
        Out.AssetPath = AssetPath;
        Out.ParamName = FullName;
        Out.ValueType = DataInterface->GetClass()->GetName();
        Out.ScriptPath = DataInterface->GetPathName();
        Out.Candidates = EditableProperties;
        return true;
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP set_niagara_data_interface_properties")));
    System->Modify();
    DataInterface->Modify();

    for (const TPair<FString, FString>& Pair : Properties)
    {
        FProperty* Property = DataInterface->GetClass()->FindPropertyByName(FName(*Pair.Key));
        if (!Property)
        {
            // Case-insensitive fallback: a caller often has the display name rather than the code name.
            for (TFieldIterator<FProperty> It(DataInterface->GetClass()); It; ++It)
            {
                if ((*It)->GetName().Equals(Pair.Key, ESearchCase::IgnoreCase))
                {
                    Property = *It;
                    break;
                }
            }
        }
        if (!Property)
        {
            Fail(Out, TEXT("data_interface_property_not_found"),
                 FString::Printf(TEXT("'%s' has no property '%s'"), *DataInterface->GetClass()->GetName(),
                     *Pair.Key),
                 EditableProperties);
            return false;
        }

        void* ValuePtr = Property->ContainerPtrToValuePtr<void>(DataInterface);
        bool bWritten = false;

        // Order matters: FBoolProperty and FByteProperty are both FNumericProperty subclasses, so the
        // generic numeric branch would swallow them (a bool would be written as the integer 0).
        if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
        {
            const FString Trimmed = Pair.Value.TrimStartAndEnd().ToLower();
            BoolProperty->SetPropertyValue(ValuePtr, Trimmed == TEXT("true") || Trimmed == TEXT("1"));
            bWritten = true;
        }
        else if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
        {
            // An enum: accept the entry name or its number.
            const UEnum* Enum = ByteProperty->Enum;
            const int64 Named = Enum ? Enum->GetValueByNameString(Pair.Value) : INDEX_NONE;
            ByteProperty->SetIntPropertyValue(ValuePtr, Named != INDEX_NONE ? Named : FCString::Atoi64(*Pair.Value));
            bWritten = true;
        }
        else if (FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
        {
            if (Numeric->IsFloatingPoint())
            {
                Numeric->SetFloatingPointPropertyValue(ValuePtr, FCString::Atod(*Pair.Value));
                bWritten = true;
            }
            else if (Numeric->IsInteger())
            {
                Numeric->SetIntPropertyValue(ValuePtr, static_cast<int64>(FCString::Atoi64(*Pair.Value)));
                bWritten = true;
            }
        }
        else if (FStrProperty* StringProperty = CastField<FStrProperty>(Property))
        {
            StringProperty->SetPropertyValue(ValuePtr, Pair.Value);
            bWritten = true;
        }

        if (!bWritten)
        {
            Fail(Out, TEXT("unsupported_property_type"),
                 FString::Printf(TEXT("Property '%s' is a %s, which this command cannot write"),
                     *Property->GetName(), *Property->GetCPPType()),
                 EditableProperties);
            return false;
        }
    }

    DataInterface->PostEditChange();

    // Read every written property back off the object, not off the request.
    for (const TPair<FString, FString>& Pair : Properties)
    {
        if (FProperty* Property = DataInterface->GetClass()->FindPropertyByName(FName(*Pair.Key)))
        {
            FUnrealMCPNiagaraParameterInfo Info;
            Info.Name = Property->GetName();
            Info.Type = Property->GetCPPType();
            const TSharedPtr<FJsonValue> JsonValue = FUnrealMCPCommonUtils::PropertyValueToJson(
                Property, Property->ContainerPtrToValuePtr<void>(DataInterface));
            Info.Value = JsonValueToText(JsonValue);
            Out.Parameters.Add(Info);
        }
    }

    System->MarkPackageDirty();
    System->RequestCompile(false);
    System->WaitForCompilationComplete();

    Out.bSuccess = true;
    Out.AssetPath = AssetPath;
    Out.ParamName = FullName;
    Out.ValueType = DataInterface->GetClass()->GetName();
    Out.ScriptPath = DataInterface->GetPathName();
    Out.Candidates = EditableProperties;
    return true;
}

// =====================================================================
// Grid attributes
// =====================================================================

bool FUnrealMCPNiagaraOps::ListGridAttributes(const FString& AssetPath, const FString& ParamName,
                                              FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* System = nullptr;
    if (!ResolveSystem(AssetPath, System, Out))
    {
        return false;
    }
    if (ParamName.IsEmpty())
    {
        Fail(Out, TEXT("invalid_params"), TEXT("Missing 'param_name'"));
        return false;
    }

    const FString FullName = NormalizeUserParameterName(ParamName);
    const FString WantedTail = NameTail(FullName);
    FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();

    FNiagaraVariable Variable;
    TArray<FNiagaraVariable> UserParameters;
    Store.GetUserParameters(UserParameters);
    for (const FNiagaraVariable& Candidate : UserParameters)
    {
        const FString CandidateName = Candidate.GetName().ToString();
        if (CandidateName.Equals(FullName, ESearchCase::IgnoreCase)
            || NameTail(CandidateName).Equals(WantedTail, ESearchCase::IgnoreCase))
        {
            Variable = Candidate;
            break;
        }
    }
    if (Variable.GetName().IsNone())
    {
        TArray<FString> Candidates;
        for (const FNiagaraVariable& Candidate : UserParameters)
        {
            Candidates.Add(Candidate.GetName().ToString());
        }
        Fail(Out, TEXT("parameter_not_found"),
             FString::Printf(TEXT("'%s' is not a user parameter of '%s'"), *FullName, *System->GetName()),
             Candidates);
        return false;
    }

    // Only the grid collections carry an attribute table; everything else has nothing to report.
    UNiagaraDataInterfaceGrid2DCollection* Grid =
        Cast<UNiagaraDataInterfaceGrid2DCollection>(Store.GetDataInterface(Variable));
    if (!Grid)
    {
        Fail(Out, TEXT("not_a_grid_collection"),
             FString::Printf(TEXT("'%s' is a %s; only a Grid2DCollection has an attribute table"),
                 *FullName, *TypeNameOf(Variable.GetType())),
             TArray<FString>{ TEXT("Grid2DCollection") });
        return false;
    }

    // The engine's own collection: it walks the emitters' GPU compute scripts and takes every
    // generated function that carries an `Attribute=` specifier. The result is the only
    // authoritative index <-> name map for the tiles a RenderTarget receives, and its warnings are
    // what the editor prints as "Failed to map PreviewAttribute ... to a grid index".
    TArray<FNiagaraVariableBase> Variables;
    TArray<uint32> Offsets;
    int32 NumAttribChannelsFound = 0;
    TArray<FText> Warnings;
    Grid->FindAttributes(Variables, Offsets, NumAttribChannelsFound, &Warnings);

    for (int32 Index = 0; Index < Variables.Num(); ++Index)
    {
        FUnrealMCPNiagaraGridAttributeInfo Info;
        Info.Name = Variables[Index].GetName().ToString();
        Info.Type = TypeNameOf(Variables[Index].GetType());
        Info.FirstSlice = Offsets.IsValidIndex(Index) ? static_cast<int32>(Offsets[Index]) : INDEX_NONE;
        Info.Channels = Variables[Index].GetSizeInBytes() / static_cast<int32>(sizeof(float));
        Out.GridAttributes.Add(Info);
    }
    for (const FText& Warning : Warnings)
    {
        Out.GridAttributeWarnings.Add(Warning.ToString());
    }

    Out.AssetPath = AssetPath;
    Out.ParamName = FullName;
    Out.ValueType = Grid->GetClass()->GetName();
    // The DI's editor preview settings, straight off the asset: these are what a RenderTarget
    // tile copy is keyed to, and they say nothing about a running instance's Vars mapping.
    Out.PreviewAttribute = Grid->PreviewAttribute.ToString();
    Out.GridAttributeCount = Out.GridAttributes.Num();
    Out.GridSlotCount = Grid->NumAttributes;
    Out.GridChannelCount = NumAttribChannelsFound;
    Out.bPreviewGridEnabled = Grid->bPreviewGrid != 0;
    Out.bSuccess = true;
    return true;
}

// =====================================================================
// Stack modules
// =====================================================================

bool FUnrealMCPNiagaraOps::ListModules(const FString& AssetPath, const FString& EmitterName, const FString& Stage,
                                       FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* System = nullptr;
    if (!ResolveSystem(AssetPath, System, Out))
    {
        return false;
    }

    UNiagaraGraph* Graph = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    if (!EmitterName.IsEmpty())
    {
        FNiagaraEmitterHandle* Handle = nullptr;
        if (!ResolveEmitter(System, EmitterName, Handle, Out))
        {
            return false;
        }
        EmitterData = Handle->GetEmitterData();
        Graph = GetEmitterGraph(EmitterData);
    }
    else
    {
        Graph = GetSystemGraph(System);
    }

    if (!Graph)
    {
        Fail(Out, TEXT("graph_unavailable"), TEXT("The requested graph is not available"));
        return false;
    }

    // The requested stage is resolved to the label the modules actually report: one of the six fixed
    // stages, or a simulation stage under its own name ("SWE Sweep") / index ("0").
    const FString RequestedStage = Stage.TrimStartAndEnd();
    FString RequestedLabel = RequestedStage;
    if (!RequestedStage.IsEmpty())
    {
        ENiagaraScriptUsage RequestedUsage;
        FNiagaraStageTarget Target;
        if (StageToUsage(RequestedStage, RequestedUsage))
        {
            RequestedLabel = UsageToStage(RequestedUsage);
        }
        else if (ResolveEmitterStageTarget(EmitterData, RequestedStage, Target))
        {
            RequestedLabel = Target.Label;
        }
        else
        {
            TArray<FString> Candidates = StageCandidates();
            AppendSimulationStageCandidates(EmitterData, Candidates);
            Fail(Out, TEXT("unknown_stage"), FString::Printf(TEXT("Unknown stage '%s'"), *Stage), Candidates);
            return false;
        }
    }

    TArray<UNiagaraNodeFunctionCall*> Calls;
    Graph->GetNodesOfClass<UNiagaraNodeFunctionCall>(Calls);

    // Evaluation order per stage: walk each output node's parameter-map chain once and remember each
    // module's position, so "which module runs first" is answerable without trusting X/Y layout.
    TMap<const UEdGraphNode*, int32> ModuleOrder;
    TArray<UNiagaraNodeOutput*> Outputs;
    Graph->GetNodesOfClass<UNiagaraNodeOutput>(Outputs);
    for (UNiagaraNodeOutput* OutputNode : Outputs)
    {
        TArray<UNiagaraNodeFunctionCall*> Ordered;
        CollectStageModules(OutputNode, Ordered);
        for (int32 Index = 0; Index < Ordered.Num(); ++Index)
        {
            ModuleOrder.Add(Ordered[Index], Index);
        }
    }

    for (UNiagaraNodeFunctionCall* Call : Calls)
    {
        if (!Call)
        {
            continue;
        }

        UNiagaraNodeOutput* OutputNode = FindOutputNodeForFunctionCall(Call);
        const FString CallStage = StageLabelForOutputNode(EmitterData, OutputNode);

        if (!RequestedLabel.IsEmpty() && !CallStage.Equals(RequestedLabel, ESearchCase::IgnoreCase))
        {
            continue;
        }

        FUnrealMCPNiagaraModuleInfo Info;
        Info.Guid = Call->NodeGuid.ToString();
        Info.Name = Call->GetFunctionName();
        Info.Stage = CallStage;
        Info.bEnabled = Call->IsNodeEnabled();
        Info.ScriptPath = Call->FunctionScript ? Call->FunctionScript->GetPathName() : FString();
        Info.PosX = Call->NodePosX;
        Info.PosY = Call->NodePosY;
        if (const int32* Order = ModuleOrder.Find(Call))
        {
            Info.Order = *Order;
        }
        Out.Modules.Add(Info);
    }

    Out.bSuccess = true;
    return true;
}

bool FUnrealMCPNiagaraOps::AddModule(const FString& AssetPath, const FString& EmitterName, const FString& Stage,
                                     const FString& ModuleAssetPath, int32 InsertIndex, FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* System = nullptr;
    if (!ResolveSystem(AssetPath, System, Out))
    {
        return false;
    }
    if (ModuleAssetPath.IsEmpty())
    {
        Fail(Out, TEXT("invalid_params"), TEXT("Missing 'module_asset_path'"));
        return false;
    }

    // The stage is either one of the six fixed stages or a simulation stage of this emitter, given by
    // its name ("SWE Sweep") or its index ("0") - a simulation stage is only known once the emitter is,
    // so the second case is resolved further down.
    ENiagaraScriptUsage Usage = ENiagaraScriptUsage::ParticleUpdateScript;
    const bool bFixedStage = StageToUsage(Stage, Usage);
    FNiagaraStageTarget Target;
    if (!bFixedStage && EmitterName.IsEmpty())
    {
        Fail(Out, TEXT("unknown_stage"),
             FString::Printf(TEXT("Unknown stage '%s'"), *Stage), StageCandidates());
        return false;
    }

    UNiagaraScript* ModuleScript = Cast<UNiagaraScript>(FUnrealMCPCommonUtils::FindAsset(ModuleAssetPath));
    if (!ModuleScript)
    {
        ModuleScript = LoadObject<UNiagaraScript>(nullptr, *ModuleAssetPath);
    }
    if (!ModuleScript)
    {
        Fail(Out, TEXT("module_script_not_found"),
             FString::Printf(TEXT("No module script at '%s'"), *ModuleAssetPath));
        return false;
    }
    if (ModuleScript->GetUsage() != ENiagaraScriptUsage::Module)
    {
        Fail(Out, TEXT("not_a_module_script"),
             FString::Printf(TEXT("'%s' is a %s script, not a Module script"), *ModuleAssetPath,
                 *UsageToStage(ModuleScript->GetUsage())));
        return false;
    }

    UNiagaraGraph* Graph = nullptr;
    UNiagaraScript* StageScript = nullptr;
    if (bFixedStage && IsSystemStage(Usage))
    {
        Graph = GetSystemGraph(System);
        StageScript = GetSystemStageScript(System, Usage);
    }
    else
    {
        FNiagaraEmitterHandle* Handle = nullptr;
        if (!ResolveEmitter(System, EmitterName, Handle, Out))
        {
            return false;
        }
        FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();
        Graph = GetEmitterGraph(EmitterData);
        if (bFixedStage)
        {
            StageScript = GetStageScript(EmitterData, Usage);
        }
        else if (ResolveEmitterStageTarget(EmitterData, Stage, Target))
        {
            Usage = Target.Usage;
            StageScript = Target.StageScript;
        }
        else
        {
            TArray<FString> Candidates = StageCandidates();
            AppendSimulationStageCandidates(EmitterData, Candidates);
            Fail(Out, TEXT("unknown_stage"),
                 FString::Printf(TEXT("Unknown stage '%s'"), *Stage), Candidates);
            return false;
        }
    }

    UNiagaraNodeOutput* OutputNode = FindStageOutputNode(Graph, Usage, StageScript);
    if (!OutputNode)
    {
        Fail(Out, TEXT("stage_output_not_found"),
             FString::Printf(TEXT("No output node for stage '%s'"), *Stage));
        return false;
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP add_niagara_module")));
    System->Modify();
    Graph->Modify();

    // The exported overload: everything else in FNiagaraStackGraphUtilities that would build the
    // parameter-map wiring around the new node is not exported across modules.
    UNiagaraNodeFunctionCall* NewNode = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
        ModuleScript, *OutputNode, InsertIndex, FString(), FGuid());
    if (!NewNode)
    {
        Fail(Out, TEXT("add_failed"),
             FString::Printf(TEXT("Adding '%s' to stage '%s' failed"), *ModuleAssetPath, *Stage));
        return false;
    }

    Graph->NotifyGraphChanged();
    System->MarkPackageDirty();
    System->RequestCompile(false);
    System->WaitForCompilationComplete();

    Out.bSuccess = true;
    Out.ModuleGuid = NewNode->NodeGuid.ToString();
    Out.ModuleName = NewNode->GetFunctionName();
    Out.ScriptPath = ModuleScript->GetPathName();
    return true;
}

bool FUnrealMCPNiagaraOps::SetModuleEnabled(const FString& AssetPath, const FString& EmitterName,
                                            const FString& ModuleGuidOrName, bool bEnabled, bool bSystemLevel,
                                            FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* System = nullptr;
    if (!ResolveSystem(AssetPath, System, Out))
    {
        return false;
    }
    if (ModuleGuidOrName.IsEmpty())
    {
        Fail(Out, TEXT("invalid_params"), TEXT("Missing 'module_guid' (or 'module_name')"));
        return false;
    }

    UNiagaraGraph* Graph = nullptr;
    if (bSystemLevel)
    {
        Graph = GetSystemGraph(System);
    }
    else
    {
        FNiagaraEmitterHandle* Handle = nullptr;
        if (!ResolveEmitter(System, EmitterName, Handle, Out))
        {
            return false;
        }
        Graph = GetEmitterGraph(Handle->GetEmitterData());
    }
    if (!Graph)
    {
        Fail(Out, TEXT("graph_unavailable"), TEXT("The requested graph is not available"));
        return false;
    }

    FGuid ModuleGuid;
    UNiagaraNodeFunctionCall* ModuleNode = nullptr;
    TArray<UNiagaraNodeFunctionCall*> Calls;
    Graph->GetNodesOfClass<UNiagaraNodeFunctionCall>(Calls);
    TArray<FString> Candidates;
    for (UNiagaraNodeFunctionCall* Call : Calls)
    {
        if (!Call)
        {
            continue;
        }
        const FString Name = Call->GetFunctionName();
        if (Call->NodeGuid.ToString().Equals(ModuleGuidOrName, ESearchCase::IgnoreCase)
            || Name.Equals(ModuleGuidOrName, ESearchCase::IgnoreCase))
        {
            ModuleNode = Call;
            ModuleGuid = Call->NodeGuid;
            break;
        }
        Candidates.Add(FString::Printf(TEXT("%s (%s)"), *Name, *Call->NodeGuid.ToString()));
    }

    if (!ModuleNode)
    {
        Fail(Out, TEXT("module_not_found"),
             FString::Printf(TEXT("No module '%s' in the requested graph"), *ModuleGuidOrName), Candidates);
        return false;
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP set_niagara_module_enabled")));
    System->Modify();
    FNiagaraStackGraphUtilities::SetModuleIsEnabled(*ModuleNode, bEnabled);

    System->MarkPackageDirty();
    System->RequestCompile(false);
    System->WaitForCompilationComplete();

    Out.bSuccess = true;
    Out.ModuleGuid = ModuleGuid.ToString();
    Out.ModuleName = ModuleNode->GetFunctionName();
    Out.bEnabled = ModuleNode->IsNodeEnabled();
    return true;
}

bool FUnrealMCPNiagaraOps::RemoveModule(const FString& AssetPath, const FString& EmitterName,
                                        const FString& ModuleRef, bool bSystemLevel,
                                        FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* System = nullptr;
    if (!ResolveSystem(AssetPath, System, Out))
    {
        return false;
    }

    UNiagaraGraph* Graph = nullptr;
    if (bSystemLevel)
    {
        Graph = GetSystemGraph(System);
    }
    else
    {
        FNiagaraEmitterHandle* Handle = nullptr;
        if (!ResolveEmitter(System, EmitterName, Handle, Out))
        {
            return false;
        }
        Graph = GetEmitterGraph(Handle->GetEmitterData());
    }
    if (!Graph)
    {
        Fail(Out, TEXT("graph_unavailable"), TEXT("The requested graph is not available"));
        return false;
    }

    UNiagaraNodeFunctionCall* Call = FindModuleNodeInGraph(Graph, ModuleRef);
    if (!Call)
    {
        Fail(Out, TEXT("module_not_found"),
             FString::Printf(TEXT("No module '%s' in the requested graph"), *ModuleRef));
        return false;
    }

    // Chain around the module: <upstream group> -> [override MapSets] -> module -> <downstream group>.
    UEdGraphPin* MapInPin = nullptr;
    UEdGraphPin* MapOutPin = nullptr;
    for (UEdGraphPin* Pin : Call->Pins)
    {
        if (!IsParameterMapConnection(Pin))
        {
            continue;
        }
        if (Pin->Direction == EGPD_Input && !MapInPin)
        {
            MapInPin = Pin;
        }
        else if (Pin->Direction == EGPD_Output && !MapOutPin)
        {
            MapOutPin = Pin;
        }
    }
    if (!MapInPin || MapInPin->LinkedTo.Num() != 1 || !MapOutPin || MapOutPin->LinkedTo.Num() != 1)
    {
        Fail(Out, TEXT("module_not_connected"),
             FString::Printf(TEXT("Module '%s' is not wired into its stage's parameter-map chain"),
                 *Call->GetFunctionName()));
        return false;
    }

    // Where the chain has to be closed after the removal: a pin on a node that survives.
    UEdGraphPin* UpstreamOutPin = MapInPin->LinkedTo[0];
    UEdGraphPin* DownstreamInPin = MapOutPin->LinkedTo[0];

    // Override nodes created for this module have pins that name the module; only those are taken.
    // (Guessing further back risks eating the previous module's overrides.)
    const FString CallName = Call->GetFunctionName();
    TArray<UEdGraphNode*> ToRemove;
    ToRemove.Add(Call);
    TArray<FString> RemovedSummary;

    UEdGraphNode* Cursor = UpstreamOutPin ? UpstreamOutPin->GetOwningNode() : nullptr;
    while (Cursor && IsMapSetNode(Cursor))
    {
        bool bBelongsToModule = false;
        bool bSawNamedPin = false;
        for (UEdGraphPin* Pin : Cursor->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Input || IsParameterMapConnection(Pin) || IsAddPin(Pin))
            {
                continue;
            }
            bSawNamedPin = true;
            if (!Pin->PinName.ToString().Contains(CallName))
            {
                bBelongsToModule = false;
                bSawNamedPin = false;
                break;
            }
            bBelongsToModule = true;
        }
        if (!bBelongsToModule || !bSawNamedPin)
        {
            break;
        }

        ToRemove.Add(Cursor);

        UEdGraphPin* CursorMapIn = nullptr;
        for (UEdGraphPin* Pin : Cursor->Pins)
        {
            if (IsParameterMapConnection(Pin) && Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() == 1)
            {
                CursorMapIn = Pin;
                break;
            }
        }
        if (!CursorMapIn)
        {
            UpstreamOutPin = nullptr;
            break;
        }
        UpstreamOutPin = CursorMapIn->LinkedTo[0];
        Cursor = UpstreamOutPin ? UpstreamOutPin->GetOwningNode() : nullptr;
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP remove_niagara_module")));
    System->Modify();
    Graph->Modify();

    for (UEdGraphNode* Node : ToRemove)
    {
        Node->Modify();
        RemovedSummary.Add(FString::Printf(TEXT("%s (%s)"), *NodeTypeLabel(Node), *Node->NodeGuid.ToString()));
        Node->BreakAllNodeLinks();
        Graph->RemoveNode(Node);
    }

    // Close the chain: the upstream group now feeds whatever consumed this module's output.
    if (UpstreamOutPin && DownstreamInPin)
    {
        UpstreamOutPin->MakeLinkTo(DownstreamInPin);
    }

    Graph->NotifyGraphChanged();
    System->MarkPackageDirty();
    System->RequestCompile(false);
    System->WaitForCompilationComplete();

    // Report the stage as it is now, so "did it work" is answered by the graph and not by the request.
    FUnrealMCPNiagaraResult Readback;
    ListModules(AssetPath, bSystemLevel ? FString() : EmitterName, FString(), Readback);

    Out.bSuccess = true;
    Out.ModuleGuid = Call->NodeGuid.ToString();
    Out.ModuleName = CallName;
    Out.EmitterName = bSystemLevel ? FString() : EmitterName;
    Out.Modules = Readback.Modules;
    Out.CompileMessages = Readback.CompileMessages;
    Out.Candidates = RemovedSummary;
    return true;
}

bool FUnrealMCPNiagaraOps::SetModuleInput(const FString& AssetPath, const FString& EmitterName,
                                          const FString& ModuleRef, const FString& InputName,
                                          const FString& Value, const FString& ParamType,
                                          FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* System = nullptr;
    if (!ResolveSystem(AssetPath, System, Out))
    {
        return false;
    }
    if (ModuleRef.IsEmpty() || InputName.IsEmpty())
    {
        Fail(Out, TEXT("invalid_params"), TEXT("Both 'module' and 'input_name' are required"));
        return false;
    }

    UNiagaraGraph* Graph = nullptr;
    FVersionedNiagaraEmitter VersionedEmitter;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    FString OwningEmitterUniqueName;
    TArray<UNiagaraScript*> DependentScripts;

    if (EmitterName.IsEmpty())
    {
        Graph = GetSystemGraph(System);
    }
    else
    {
        FNiagaraEmitterHandle* Handle = nullptr;
        if (!ResolveEmitter(System, EmitterName, Handle, Out))
        {
            return false;
        }
        EmitterData = Handle->GetEmitterData();
        Graph = GetEmitterGraph(EmitterData);
        VersionedEmitter = Handle->GetInstance();
        OwningEmitterUniqueName = Handle->GetUniqueInstanceName();
        if (EmitterData)
        {
            EmitterData->GetScripts(DependentScripts, /*bCompilableOnly=*/true, /*bEnabledOnly=*/false);
        }
    }
    if (!Graph)
    {
        Fail(Out, TEXT("graph_unavailable"), TEXT("The requested graph is not available"));
        return false;
    }

    UNiagaraNodeFunctionCall* Call = FindModuleNodeInGraph(Graph, ModuleRef);
    if (!Call)
    {
        TArray<FString> Candidates;
        TArray<UNiagaraNodeFunctionCall*> Calls;
        Graph->GetNodesOfClass<UNiagaraNodeFunctionCall>(Calls);
        for (UNiagaraNodeFunctionCall* Candidate : Calls)
        {
            if (Candidate)
            {
                Candidates.Add(FString::Printf(TEXT("%s (%s)"), *Candidate->GetFunctionName(),
                                               *Candidate->NodeGuid.ToString()));
            }
        }
        Fail(Out, TEXT("module_not_found"),
             FString::Printf(TEXT("No module '%s' in the requested graph"), *ModuleRef), Candidates);
        return false;
    }

    UNiagaraNodeOutput* OutputNode = FindOutputNodeForFunctionCall(Call);
    const ENiagaraScriptUsage Usage = OutputNode ? OutputNode->GetUsage()
                                                : ENiagaraScriptUsage::ParticleUpdateScript;
    UNiagaraScript* StageScript = StageScriptForOutputNode(EmitterData, System, OutputNode);
    if (!StageScript)
    {
        Fail(Out, TEXT("stage_script_not_found"),
             FString::Printf(TEXT("Module '%s' sits on stage '%s', which has no script to bind against"),
                 *Call->GetFunctionName(), *StageLabelForOutputNode(EmitterData, OutputNode)));
        return false;
    }

    // The input is named by the module script's own script variables (the source the stack itself
    // reads). UNiagaraNodeFunctionCall::Signature.Inputs is empty until the node refreshes, so it is
    // only a fallback - the first version of this command matched against it and found nothing.
    TArray<FNiagaraVariable> InputVariables;
    if (UNiagaraGraph* CalledGraph = Call->GetCalledGraph())
    {
        for (const TPair<FNiagaraVariable, TObjectPtr<UNiagaraScriptVariable>>& Pair : CalledGraph->GetAllMetaData())
        {
            InputVariables.Add(Pair.Key);
        }
    }
    for (const FNiagaraVariable& Input : Call->Signature.Inputs)
    {
        InputVariables.AddUnique(Input);
    }

    const FNiagaraVariable* InputVariable = nullptr;
    TArray<FString> InputCandidates;
    for (const FNiagaraVariable& Input : InputVariables)
    {
        const FString Full = Input.GetName().ToString();
        FString Tail = Full;
        int32 DotIndex = INDEX_NONE;
        if (Full.FindLastChar(TEXT('.'), DotIndex))
        {
            Tail = Full.RightChop(DotIndex + 1);
        }

        // Exact name, "Module.<name>", or a module-namespaced name whose tail matches.
        const bool bNameMatches = Full.Equals(InputName, ESearchCase::IgnoreCase)
            || Full.Equals(FString::Printf(TEXT("Module.%s"), *InputName), ESearchCase::IgnoreCase)
            || (Full.StartsWith(TEXT("Module."), ESearchCase::IgnoreCase)
                && Tail.Equals(InputName, ESearchCase::IgnoreCase));
        if (bNameMatches && !InputVariable)
        {
            InputVariable = &Input;
        }
        if (Full.StartsWith(TEXT("Module."), ESearchCase::IgnoreCase))
        {
            InputCandidates.Add(Full);
        }
    }
    if (!InputVariable)
    {
        Fail(Out, TEXT("module_input_not_found"),
             FString::Printf(TEXT("Module '%s' has no input '%s'"), *Call->GetFunctionName(), *InputName),
             InputCandidates);
        return false;
    }

    FNiagaraTypeDefinition InputType = InputVariable->GetType();
    if (!ParamType.IsEmpty())
    {
        FNiagaraTypeDefinition RequestedType;
        if (!ResolveParamType(ParamType, RequestedType))
        {
            Fail(Out, TEXT("unknown_parameter_type"),
                 FString::Printf(TEXT("Unknown param_type '%s'"), *ParamType), ParameterTypeCandidates());
            return false;
        }
        if (RequestedType != InputType)
        {
            Fail(Out, TEXT("parameter_type_mismatch"),
                 FString::Printf(TEXT("Input '%s' is a %s input, not %s"), *InputName,
                     *TypeNameOf(InputType), *ParamType),
                 TArray<FString>{ TypeNameOf(InputType) });
            return false;
        }
    }
    if (InputType.GetClass() != nullptr)
    {
        Fail(Out, TEXT("unsupported_parameter_type"),
             FString::Printf(TEXT("Input '%s' is a %s (data interface / object) input; this command writes "
                                 "scalar and vector values"), *InputName, *TypeNameOf(InputType)),
             ParameterTypeCandidates());
        return false;
    }

    TArray<double> Numbers;
    const bool bNumbersParsed = ParseNumbers(Value, Numbers);
    // An enum input also accepts its member name ("Infinite"), so the numeric parse is not the gate.
    if (!bNumbersParsed && !InputType.IsEnum())
    {
        Fail(Out, TEXT("invalid_params"), FString::Printf(TEXT("Could not read a value out of '%s'"), *Value));
        return false;
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP set_niagara_module_input")));
    System->Modify();
    Graph->Modify();

    // The editor's own writer: it creates the override node / rapid iteration parameter exactly the
    // way the stack UI does (FNiagaraStackFunctionInputBinder::SetValue is just SetData).
    FNiagaraStackFunctionInputBinder Binder;
    FText BindError;
    const FCompileConstantResolver ConstantResolver =
        EmitterData ? FCompileConstantResolver(VersionedEmitter, Usage)
                    : FCompileConstantResolver(System, Usage);

    if (!Binder.TryBind(StageScript, DependentScripts, ConstantResolver, OwningEmitterUniqueName, Call,
                        FName(*InputName), TOptional<FNiagaraTypeDefinition>(InputType),
                        /*bInIsRequired=*/true, BindError))
    {
        Fail(Out, TEXT("module_input_bind_failed"),
             FString::Printf(TEXT("Could not bind input '%s' of module '%s': %s"), *InputName,
                 *Call->GetFunctionName(),
                 BindError.IsEmpty() ? TEXT("binder refused the input") : *BindError.ToString()));
        return false;
    }

    // The value is dispatched on the SIGNATURE's type, not on Binder.GetInputType(): that getter is
    // only declared in the header (no NIAGARAEDITOR_API), so calling it would not link. SetValue<T>
    // is an inline template that lands on the exported SetData, and my param_type check above keeps
    // the two types in agreement.
    bool bWritten = false;
    if (InputType.IsEnum())
    {
        // Niagara stores an enum input as the enum's integer value, so the write is an int32 - but the
        // caller may name the member instead of the number ("Infinite"), which is what the stack UI
        // shows and what a human reads off the panel.
        UEnum* Enum = InputType.GetEnum();
        int32 EnumValue = INDEX_NONE;
        if (bNumbersParsed && Numbers.Num() >= 1)
        {
            EnumValue = static_cast<int32>(Numbers[0]);
        }
        else if (Enum)
        {
            const FString Requested = Value.TrimStartAndEnd();
            FString ShortName = Requested;
            if (ShortName.Contains(TEXT("::")))
            {
                ShortName.Split(TEXT("::"), nullptr, &ShortName);
            }
            EnumValue = static_cast<int32>(Enum->GetValueByNameString(ShortName));
            if (EnumValue == INDEX_NONE && ShortName != Requested)
            {
                EnumValue = static_cast<int32>(Enum->GetValueByNameString(Requested));
            }
            if (EnumValue == INDEX_NONE)
            {
                TArray<FString> EnumCandidates;
                const int32 NumEnums = Enum->NumEnums();
                for (int32 Index = 0; Index < NumEnums; ++Index)
                {
                    const FString Name = Enum->GetNameStringByIndex(Index);
                    if (!Name.EndsWith(TEXT("_MAX")))
                    {
                        EnumCandidates.Add(Name);
                    }
                }
                Fail(Out, TEXT("unknown_enum_member"),
                     FString::Printf(TEXT("'%s' is not a member of %s"), *Requested, *Enum->GetName()),
                     EnumCandidates);
                return false;
            }
        }
        if (EnumValue == INDEX_NONE)
        {
            Fail(Out, TEXT("invalid_params"),
                 FString::Printf(TEXT("Input '%s' is an enum input; give a member name or a number"), *InputName));
            return false;
        }
        Binder.SetValue<int32>(EnumValue);
        bWritten = true;
    }
    else if (InputType == FNiagaraTypeDefinition::GetFloatDef() && Numbers.Num() >= 1)
    {
        Binder.SetValue<float>(static_cast<float>(Numbers[0]));
        bWritten = true;
    }
    else if (InputType == FNiagaraTypeDefinition::GetIntDef() && Numbers.Num() >= 1)
    {
        Binder.SetValue<int32>(static_cast<int32>(Numbers[0]));
        bWritten = true;
    }
    else if (InputType == FNiagaraTypeDefinition::GetBoolDef())
    {
        const FString Trimmed = Value.TrimStartAndEnd().ToLower();
        const bool bValue = Trimmed == TEXT("true") || Trimmed == TEXT("1")
            || (Numbers.Num() > 0 && Numbers[0] != 0.0);
        Binder.SetValue<FNiagaraBool>(FNiagaraBool(bValue));
        bWritten = true;
    }
    else if (InputType == FNiagaraTypeDefinition::GetVec2Def() && Numbers.Num() >= 2)
    {
        Binder.SetValue<FVector2f>(FVector2f(Numbers[0], Numbers[1]));
        bWritten = true;
    }
    else if (InputType == FNiagaraTypeDefinition::GetVec3Def() && Numbers.Num() >= 3)
    {
        Binder.SetValue<FVector3f>(FVector3f(Numbers[0], Numbers[1], Numbers[2]));
        bWritten = true;
    }
    else if (InputType == FNiagaraTypeDefinition::GetVec4Def() && Numbers.Num() >= 4)
    {
        Binder.SetValue<FVector4f>(FVector4f(Numbers[0], Numbers[1], Numbers[2], Numbers[3]));
        bWritten = true;
    }
    else if (InputType == FNiagaraTypeDefinition::GetColorDef() && Numbers.Num() >= 3)
    {
        const float Alpha = Numbers.Num() > 3 ? static_cast<float>(Numbers[3]) : 1.0f;
        Binder.SetValue<FLinearColor>(FLinearColor(static_cast<float>(Numbers[0]), static_cast<float>(Numbers[1]),
            static_cast<float>(Numbers[2]), Alpha));
        bWritten = true;
    }
    else if (InputType == FNiagaraTypeDefinition::GetQuatDef() && Numbers.Num() >= 4)
    {
        Binder.SetValue<FQuat4f>(FQuat4f(Numbers[0], Numbers[1], Numbers[2], Numbers[3]));
        bWritten = true;
    }

    if (!bWritten)
    {
        Fail(Out, TEXT("write_failed"),
             FString::Printf(TEXT("Cannot write '%s' into the %s input '%s'"), *Value,
                 *TypeNameOf(InputType), *InputName),
             ParameterTypeCandidates());
        return false;
    }

    // Readback: FNiagaraStackFunctionInputBinder::GetData is not exported, but the value it wrote is
    // the module's rapid iteration parameter, which lives in the script's own store.
    FString Readback;
    TArray<FNiagaraVariable> RapidIterationVariables;
    StageScript->RapidIterationParameters.GetParameters(RapidIterationVariables);
    for (const FNiagaraVariable& Variable : RapidIterationVariables)
    {
        const FString Full = Variable.GetName().ToString();
        int32 DotIndex = INDEX_NONE;
        if (!Full.FindLastChar(TEXT('.'), DotIndex)
            || !Full.RightChop(DotIndex + 1).Equals(InputName, ESearchCase::IgnoreCase))
        {
            continue;
        }
        if (!ReadParameterValue(StageScript->RapidIterationParameters, Variable, Readback))
        {
            continue;
        }
        Out.RapidIterationParameter = Full;
        break;
    }

    Graph->NotifyGraphChanged();
    System->MarkPackageDirty();
    System->RequestCompile(false);
    System->WaitForCompilationComplete();

    Out.bSuccess = true;
    Out.ModuleGuid = Call->NodeGuid.ToString();
    Out.ModuleName = Call->GetFunctionName();
    Out.ParamName = InputName;
    Out.ValueType = TypeNameOf(InputType);
    Out.Value = Readback.IsEmpty() ? Value : Readback;
    Out.EmitterName = OwningEmitterUniqueName;
    return true;
}

// =====================================================================
// Scratch-pad modules
// =====================================================================

bool FUnrealMCPNiagaraOps::CreateScratchPadModule(const FString& AssetPath, const FString& EmitterName,
                                                  const FString& Stage, const FString& ModuleName,
                                                  FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* System = nullptr;
    if (!ResolveSystem(AssetPath, System, Out))
    {
        return false;
    }

    FNiagaraEmitterHandle* Handle = nullptr;
    if (!ResolveEmitter(System, EmitterName, Handle, Out))
    {
        return false;
    }

    FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();

    // An emitter stage: one of the four fixed ones, or a simulation stage by name / index ("SWE Sweep",
    // "0") - a scratch module on a GPU simulation stage is what a grid solver (SWE, PBF) is written in.
    FNiagaraStageTarget Target;
    if (!ResolveEmitterStageTarget(EmitterData, Stage, Target))
    {
        TArray<FString> Candidates{ StageEmitterSpawn, StageEmitterUpdate, StageParticleSpawn, StageParticleUpdate };
        AppendSimulationStageCandidates(EmitterData, Candidates);
        Fail(Out, TEXT("unknown_stage"),
             FString::Printf(TEXT("'%s' is not an emitter stage"), *Stage), Candidates);
        return false;
    }
    const ENiagaraScriptUsage Usage = Target.Usage;

    UNiagaraScratchPadContainer* Container = GetOrCreateScratchPadContainer(Handle, EmitterData);
    if (!Container)
    {
        Fail(Out, TEXT("scratch_pad_unavailable"), TEXT("Could not reach the emitter's scratch-pad container"));
        return false;
    }

    UNiagaraGraph* EmitterGraph = GetEmitterGraph(EmitterData);
    UNiagaraScript* StageScript = Target.StageScript;
    UNiagaraNodeOutput* OutputNode = FindStageOutputNode(EmitterGraph, Usage, StageScript);
    if (!OutputNode)
    {
        Fail(Out, TEXT("stage_output_not_found"), FString::Printf(TEXT("No output node for stage '%s'"), *Stage));
        return false;
    }

    // The engine's own template for a Module script. UNiagaraScriptFactoryNew::InitializeScript is
    // not exported across modules, and starting from an empty script means building the whole
    // Map In -> Map Get -> Map Set -> Map Out skeleton by hand.
    UNiagaraScript* DefaultModule = Cast<UNiagaraScript>(GetDefault<UNiagaraEditorSettings>()->DefaultModuleScript.TryLoad());
    if (!DefaultModule
        || Cast<UNiagaraScriptSource>(DefaultModule->GetLatestSource()) == nullptr
        || Cast<UNiagaraScriptSource>(DefaultModule->GetLatestSource())->NodeGraph == nullptr)
    {
        Fail(Out, TEXT("scratch_pad_template_missing"),
             TEXT("UNiagaraEditorSettings::DefaultModuleScript is missing or invalid; cannot create a scratch module"));
        return false;
    }

    Container->SetFlags(RF_Transactional);
    Container->Modify();

    const FName UniqueName = FNiagaraEditorUtilities::GetUniqueObjectName(
        Container, UNiagaraScript::StaticClass(),
        ModuleName.IsEmpty() ? FString(TEXT("ScratchModule")) : ModuleName);

    UNiagaraScript* NewScript = Cast<UNiagaraScript>(StaticDuplicateObject(DefaultModule, Container, UniqueName));
    if (!NewScript)
    {
        Fail(Out, TEXT("create_failed"), TEXT("Failed to duplicate the default module script"));
        return false;
    }
    NewScript->ClearFlags(RF_Public | RF_Standalone);
    NewScript->SetFlags(RF_Transactional);
    Container->Scripts.Add(NewScript);

    if (FVersionedNiagaraScriptData* ScriptData = NewScript->GetLatestScriptData())
    {
        // The bitmask is what allows the script to sit on this stage's stack at all.
        ScriptData->ModuleUsageBitmask |= (1 << static_cast<int32>(Usage));
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP create_niagara_scratch_pad_module")));
    System->Modify();
    EmitterGraph->Modify();

    UNiagaraNodeFunctionCall* ModuleNode = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
        NewScript, *OutputNode, INDEX_NONE, NewScript->GetName(), FGuid());
    if (!ModuleNode)
    {
        Fail(Out, TEXT("add_failed"),
             FString::Printf(TEXT("Adding the scratch module to stage '%s' failed"), *Stage));
        return false;
    }

    System->MarkPackageDirty();
    System->RequestCompile(false);
    System->WaitForCompilationComplete();

    Out.bSuccess = true;
    Out.ModuleGuid = ModuleNode->NodeGuid.ToString();
    Out.ModuleName = ModuleNode->GetFunctionName();
    Out.ScriptPath = NewScript->GetPathName();
    return true;
}

bool FUnrealMCPNiagaraOps::ListScratchPadNodes(const FString& AssetPath, const FString& EmitterName,
                                               const FString& ModuleRef, FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* System = nullptr;
    if (!ResolveSystem(AssetPath, System, Out))
    {
        return false;
    }
    FNiagaraEmitterHandle* Handle = nullptr;
    if (!ResolveEmitter(System, EmitterName, Handle, Out))
    {
        return false;
    }

    UNiagaraNodeFunctionCall* Call = FindModuleNode(Handle->GetEmitterData(), ModuleRef);
    UNiagaraGraph* Graph = GetScratchGraph(Call);
    if (!Graph)
    {
        Fail(Out, TEXT("scratch_pad_not_found"),
             FString::Printf(TEXT("No scratch-pad module '%s' on emitter '%s'"), *ModuleRef, *EmitterName));
        return false;
    }

    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node)
        {
            continue;
        }
        FUnrealMCPNiagaraNodeInfo Info;
        Info.Guid = Node->NodeGuid.ToString();
        Info.Type = NodeTypeLabel(Node);
        Info.Name = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
        Info.PosX = Node->NodePosX;
        Info.PosY = Node->NodePosY;
        Out.Nodes.Add(Info);
    }

    // Pins of every node, so a caller can wire without guessing names.
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node)
        {
            continue;
        }
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || IsAddPin(Pin))
            {
                continue;
            }
            FUnrealMCPNiagaraPinInfo Info;
            Info.Name = Pin->PinName.ToString();
            Info.Direction = Pin->Direction == EGPD_Output ? TEXT("output") : TEXT("input");
            Info.Type = TypeNameOf(UEdGraphSchema_Niagara::PinToTypeDefinition(Pin));
            Info.NodeGuid = Node->NodeGuid.ToString();
            Info.bConnected = Pin->LinkedTo.Num() > 0;
            Out.Pins.Add(Info);
        }
    }

    Out.bSuccess = true;
    Out.ModuleGuid = Call->NodeGuid.ToString();
    Out.ModuleName = Call->GetFunctionName();
    return true;
}

bool FUnrealMCPNiagaraOps::SetCustomHlsl(const FString& AssetPath, const FString& EmitterName,
                                         const FString& ModuleRef, const FString& NodeGuid, const FString& Code,
                                         FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* System = nullptr;
    if (!ResolveSystem(AssetPath, System, Out))
    {
        return false;
    }
    FNiagaraEmitterHandle* Handle = nullptr;
    if (!ResolveEmitter(System, EmitterName, Handle, Out))
    {
        return false;
    }

    UNiagaraNodeFunctionCall* Call = FindModuleNode(Handle->GetEmitterData(), ModuleRef);
    UNiagaraGraph* Graph = GetScratchGraph(Call);
    if (!Graph)
    {
        Fail(Out, TEXT("scratch_pad_not_found"),
             FString::Printf(TEXT("No scratch-pad module '%s' on emitter '%s'"), *ModuleRef, *EmitterName));
        return false;
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP set_niagara_custom_hlsl")));
    System->Modify();
    Graph->Modify();

    UEdGraphNode* HlslNode = nullptr;
    if (NodeGuid.IsEmpty())
    {
        HlslNode = NewObject<UNiagaraNodeCustomHlsl>(Graph, NAME_None, RF_Transactional);
        if (HlslNode)
        {
            Graph->AddNode(HlslNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
            HlslNode->CreateNewGuid();
            HlslNode->PostPlacedNewNode();
            HlslNode->AllocateDefaultPins();
            HlslNode->NodePosX = 300;
            HlslNode->NodePosY = 0;
        }
    }
    else
    {
        HlslNode = FindNodeByGuid(Graph, NodeGuid);
        if (HlslNode && !HlslNode->IsA<UNiagaraNodeCustomHlsl>())
        {
            Fail(Out, TEXT("not_a_custom_hlsl_node"),
                 FString::Printf(TEXT("Node '%s' is a %s"), *NodeGuid, *NodeTypeLabel(HlslNode)));
            return false;
        }
    }

    if (!HlslNode)
    {
        Fail(Out, TEXT("node_not_found"),
             NodeGuid.IsEmpty() ? TEXT("Could not create a Custom HLSL node") :
                                  FString::Printf(TEXT("No node '%s' in the scratch graph"), *NodeGuid));
        return false;
    }

    SetCustomHlslReflected(HlslNode, Code);
    Graph->NotifyGraphChanged();
    System->MarkPackageDirty();

    // Read the code back out of the node: the structural readback of an HLSL write.
    const FString Readback = GetCustomHlslReflected(HlslNode);
    if (Readback != Code)
    {
        Fail(Out, TEXT("readback_mismatch"), TEXT("The HLSL code did not survive the write"));
        return false;
    }

    Out.bSuccess = true;
    Out.NodeGuid = HlslNode->NodeGuid.ToString();
    Out.HlslCode = Readback;
    Out.ModuleGuid = Call->NodeGuid.ToString();
    Out.ModuleName = Call->GetFunctionName();
    return true;
}

bool FUnrealMCPNiagaraOps::SetScratchPadPins(const FString& AssetPath, const FString& EmitterName,
                                             const FString& ModuleRef, const FString& NodeGuid,
                                             const TArray<FUnrealMCPNiagaraPinSpec>& Pins,
                                             FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* System = nullptr;
    if (!ResolveSystem(AssetPath, System, Out))
    {
        return false;
    }
    FNiagaraEmitterHandle* Handle = nullptr;
    if (!ResolveEmitter(System, EmitterName, Handle, Out))
    {
        return false;
    }

    UNiagaraNodeFunctionCall* Call = FindModuleNode(Handle->GetEmitterData(), ModuleRef);
    UNiagaraGraph* Graph = GetScratchGraph(Call);
    if (!Graph)
    {
        Fail(Out, TEXT("scratch_pad_not_found"),
             FString::Printf(TEXT("No scratch-pad module '%s' on emitter '%s'"), *ModuleRef, *EmitterName));
        return false;
    }

    // No node: the pins are the module's own inputs / outputs, i.e. written into the scratch graph's
    // Map Get ("Module.<name>" reads) and Map Set ("<namespace>.<name>" writes).
    UEdGraphNode* TargetNode = NodeGuid.IsEmpty() ? nullptr : FindNodeByGuid(Graph, NodeGuid);
    if (!NodeGuid.IsEmpty() && !TargetNode)
    {
        Fail(Out, TEXT("node_not_found"), FString::Printf(TEXT("No node '%s' in the scratch graph"), *NodeGuid));
        return false;
    }

    FNiagaraTypeDefinition UnusedTypeCheck;
    for (const FUnrealMCPNiagaraPinSpec& Spec : Pins)
    {
        if (!ResolvePinType(Spec.Type, UnusedTypeCheck))
        {
            TArray<FString> TypeCandidates = ParameterTypeCandidates();
            TypeCandidates.Add(TEXT("Grid2DCollection / Grid3DCollection / ArrayFloat / ArrayVector / RenderTarget2D"));
            Fail(Out, TEXT("unknown_pin_type"),
                 FString::Printf(TEXT("Unknown pin type '%s' for pin '%s'"), *Spec.Type, *Spec.Name),
                 TypeCandidates);
            return false;
        }

        // A Position pin on a Custom HLSL node compiles to a VM error and takes the whole system down
        // with it:
        //   NiagaraEmitterInstanceShader.usf(0): error: Component selction_result of variable
        //   selction_result has no valid offset. Possibly uninitialized data being used.
        // (measured on ParticleUpdate/ParticleSpawn: a Position input, a Position output, and a vec3
        // output fed into a Position Map Set pin all fail; float/vec3/int/bool never do.)
        // Refusing here keeps a broken pin out of the graph instead of surfacing that error later.
        const bool bTargetIsCustomHlsl = TargetNode && TargetNode->IsA<UNiagaraNodeCustomHlsl>();
        if (bTargetIsCustomHlsl && UnusedTypeCheck == FNiagaraTypeDefinition::GetPositionDef())
        {
            Fail(Out, TEXT("unsupported_hlsl_pin_type"),
                 FString::Printf(TEXT("Pin '%s': a Position (LWC) pin cannot be translated inside a Custom "
                                     "HLSL node - the VM compile fails with 'Component selction_result ... "
                                     "has no valid offset'. Use vec3 and write an attribute "
                                     "(Particles.WaveHeight / Particles.Velocity) instead; moving "
                                     "Particles.Position needs a simulation stage"),
                     *Spec.Name),
                 TArray<FString>{ TEXT("vec3"), TEXT("float"), TEXT("int"), TEXT("bool") });
            return false;
        }
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP set_niagara_scratch_pad_pins")));
    System->Modify();
    Graph->Modify();

    for (const FUnrealMCPNiagaraPinSpec& Spec : Pins)
    {
        FNiagaraTypeDefinition PinType;
        ResolvePinType(Spec.Type, PinType);
        const FEdGraphPinType GraphPinType = UEdGraphSchema_Niagara::TypeDefinitionToPinType(PinType);
        const bool bOutput = Spec.Direction.Equals(TEXT("output"), ESearchCase::IgnoreCase)
            || Spec.Direction.Equals(TEXT("out"), ESearchCase::IgnoreCase);

        UEdGraphPin* NewPin = nullptr;
        UEdGraphNode* PinOwner = TargetNode;

        if (TargetNode == nullptr)
        {
            // Module-level: a read input creates an output pin on the Map Get, a write output creates
            // an input pin on the Map Set. Both nodes exist in the default module template.
            UClass* DesiredClass = bOutput ? GetMapSetClass() : GetMapGetClass();
            if (!DesiredClass)
            {
                Fail(Out, TEXT("scratch_pad_node_class_missing"),
                     TEXT("NiagaraNodeParameterMapGet/Set classes are not loaded"));
                return false;
            }
            TArray<UEdGraphNode*> SameClass;
            Graph->GetNodesOfClass<UEdGraphNode>(SameClass);
            for (UEdGraphNode* Node : SameClass)
            {
                if (Node && Node->IsA(DesiredClass))
                {
                    PinOwner = Node;
                    break;
                }
            }
            if (!PinOwner)
            {
                PinOwner = SpawnNodeByClass(Graph, DesiredClass, bOutput ? 500 : 100, 0);
            }
            if (!PinOwner)
            {
                Fail(Out, TEXT("scratch_pad_pin_failed"),
                     TEXT("Could not find or create the Map Get/Map Set node"));
                return false;
            }
        }

        // Parameter-map pin names ARE the parameter names (already namespaced); a Custom HLSL pin
        // name is the variable name its code refers to.
        FString PinName = Spec.Name;
        const bool bMapGet = IsMapGetNode(PinOwner);
        const bool bMapSet = IsMapSetNode(PinOwner);
        if (bMapGet && !PinName.Contains(TEXT(".")))
        {
            PinName = FString::Printf(TEXT("Module.%s"), *PinName);
        }
        else if (bMapSet && !PinName.Contains(TEXT(".")))
        {
            PinName = FString::Printf(TEXT("Particles.%s"), *PinName);
        }

        // Pin direction depends on the carrier, and the two conventions are opposite:
        //  * Map Get  : a module INPUT is an output pin (the node hands the value to the graph)
        //  * Map Set  : a module OUTPUT is an input pin (the node receives the value to write)
        //  * typed nodes (Custom HLSL, Op, ...): in stays in, out stays out - the HLSL body reads its
        //    input pins and writes its output pins.
        EEdGraphPinDirection CreateDirection = bOutput ? EGPD_Output : EGPD_Input;
        if (bMapGet)
        {
            CreateDirection = EGPD_Output;
        }
        else if (bMapSet)
        {
            CreateDirection = EGPD_Input;
        }

        // Idempotent: a second call must reuse the pin. Two same-named pins on a Map Get/Set change
        // the module signature and make the system compile invalid.
        if (UEdGraphPin* Existing = FindPinByName(PinOwner, PinName, CreateDirection, /*bConstrainDirection=*/false))
        {
            FUnrealMCPNiagaraPinInfo Info;
            Info.Name = Existing->PinName.ToString();
            Info.Direction = Existing->Direction == EGPD_Output ? TEXT("output") : TEXT("input");
            Info.Type = TypeNameOf(UEdGraphSchema_Niagara::PinToTypeDefinition(Existing));
            Info.NodeGuid = PinOwner->NodeGuid.ToString();
            Info.bConnected = Existing->LinkedTo.Num() > 0;
            Out.Pins.Add(Info);
            continue;
        }

        PinOwner->Modify();
        NewPin = PinOwner->CreatePin(CreateDirection, GraphPinType, FName(*PinName));
        if (!NewPin)
        {
            Fail(Out, TEXT("scratch_pad_pin_failed"),
                 FString::Printf(TEXT("Could not create pin '%s' on %s"), *PinName, *NodeTypeLabel(PinOwner)));
            return false;
        }

        // The "+" placeholder pins on a dynamic-pin node must not survive into the signature.
        for (int32 PinIndex = PinOwner->Pins.Num() - 1; PinIndex >= 0; --PinIndex)
        {
            if (IsAddPin(PinOwner->Pins[PinIndex]))
            {
                PinOwner->RemovePin(PinOwner->Pins[PinIndex]);
            }
        }

        if (PinOwner->IsA<UNiagaraNodeCustomHlsl>())
        {
            RebuildCustomHlslSignatureFromPins(PinOwner);
        }

        FUnrealMCPNiagaraPinInfo Info;
        Info.Name = NewPin->PinName.ToString();
        Info.Direction = NewPin->Direction == EGPD_Output ? TEXT("output") : TEXT("input");
        Info.Type = TypeNameOf(UEdGraphSchema_Niagara::PinToTypeDefinition(NewPin));
        Info.NodeGuid = PinOwner->NodeGuid.ToString();
        Info.bConnected = false;
        Out.Pins.Add(Info);
    }

    Graph->NotifyGraphChanged();
    System->MarkPackageDirty();

    Out.bSuccess = true;
    Out.NodeGuid = TargetNode ? TargetNode->NodeGuid.ToString() : FString();
    Out.ModuleGuid = Call->NodeGuid.ToString();
    Out.ModuleName = Call->GetFunctionName();
    return true;
}

bool FUnrealMCPNiagaraOps::RemoveScratchPadNode(const FString& AssetPath, const FString& EmitterName,
                                                const FString& ModuleRef, const FString& NodeGuid,
                                                FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* System = nullptr;
    if (!ResolveSystem(AssetPath, System, Out))
    {
        return false;
    }
    FNiagaraEmitterHandle* Handle = nullptr;
    if (!ResolveEmitter(System, EmitterName, Handle, Out))
    {
        return false;
    }

    UNiagaraNodeFunctionCall* Call = FindModuleNode(Handle->GetEmitterData(), ModuleRef);
    UNiagaraGraph* Graph = GetScratchGraph(Call);
    if (!Graph)
    {
        Fail(Out, TEXT("scratch_pad_not_found"),
             FString::Printf(TEXT("No scratch-pad module '%s' on emitter '%s'"), *ModuleRef, *EmitterName));
        return false;
    }

    UEdGraphNode* Node = FindNodeByGuid(Graph, NodeGuid);
    if (!Node)
    {
        TArray<FString> Candidates;
        for (UEdGraphNode* Candidate : Graph->Nodes)
        {
            if (Candidate)
            {
                Candidates.Add(FString::Printf(TEXT("%s (%s)"), *NodeTypeLabel(Candidate),
                                               *Candidate->NodeGuid.ToString()));
            }
        }
        Fail(Out, TEXT("node_not_found"),
             FString::Printf(TEXT("No node '%s' in the scratch graph"), *NodeGuid), Candidates);
        return false;
    }

    // The module graph only compiles with its input node (the parameter map in) and its output node
    // (the map out). Both are structural: editing them by hand leaves a module nothing can use.
    if (Node->IsA<UNiagaraNodeInput>() || Node->IsA<UNiagaraNodeOutput>())
    {
        Fail(Out, TEXT("structural_node"),
             FString::Printf(TEXT("'%s' is the module's %s node and cannot be removed"),
                 *NodeGuid, Node->IsA<UNiagaraNodeInput>() ? TEXT("input") : TEXT("output")));
        return false;
    }

    const FString RemovedType = NodeTypeLabel(Node);

    const FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP remove_niagara_scratch_pad_node")));
    System->Modify();
    Graph->Modify();
    Node->Modify();

    // DestroyNode breaks the node's pin links, which is what the caller wants after a wrong wiring.
    Node->DestroyNode();
    Graph->NotifyGraphChanged();
    System->MarkPackageDirty();

    // Read the graph back: the caller needs the node list as it is now, not as it was asked to be.
    FUnrealMCPNiagaraResult Readback;
    ListScratchPadNodes(AssetPath, EmitterName, ModuleRef, Readback);

    Out.bSuccess = true;
    Out.NodeGuid = NodeGuid;
    Out.Value = RemovedType;
    Out.ModuleGuid = Call->NodeGuid.ToString();
    Out.ModuleName = Call->GetFunctionName();
    Out.Nodes = Readback.Nodes;
    Out.Pins = Readback.Pins;
    Out.RemainingNodes = Readback.Nodes.Num();
    return true;
}

bool FUnrealMCPNiagaraOps::RemoveScratchPadPin(const FString& AssetPath, const FString& EmitterName,
                                               const FString& ModuleRef, const FString& NodeGuid,
                                               const FString& PinName, const FString& Direction,
                                               FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* System = nullptr;
    if (!ResolveSystem(AssetPath, System, Out))
    {
        return false;
    }
    FNiagaraEmitterHandle* Handle = nullptr;
    if (!ResolveEmitter(System, EmitterName, Handle, Out))
    {
        return false;
    }

    UNiagaraNodeFunctionCall* Call = FindModuleNode(Handle->GetEmitterData(), ModuleRef);
    UNiagaraGraph* Graph = GetScratchGraph(Call);
    if (!Graph)
    {
        Fail(Out, TEXT("scratch_pad_not_found"),
             FString::Printf(TEXT("No scratch-pad module '%s' on emitter '%s'"), *ModuleRef, *EmitterName));
        return false;
    }

    UEdGraphNode* Node = FindNodeByGuid(Graph, NodeGuid);
    if (!Node)
    {
        Fail(Out, TEXT("node_not_found"),
             FString::Printf(TEXT("No node '%s' in the scratch graph"), *NodeGuid));
        return false;
    }

    // A parameter-map pin is the chain itself; removing it disconnects the stack.
    UEdGraphPin* Pin = nullptr;
    TArray<FString> Candidates;
    const bool bWantsInput = Direction.Equals(TEXT("input"), ESearchCase::IgnoreCase)
        || Direction.Equals(TEXT("in"), ESearchCase::IgnoreCase);
    const bool bWantsOutput = Direction.Equals(TEXT("output"), ESearchCase::IgnoreCase)
        || Direction.Equals(TEXT("out"), ESearchCase::IgnoreCase);
    for (UEdGraphPin* Candidate : Node->Pins)
    {
        if (!Candidate || IsAddPin(Candidate))
        {
            continue;
        }
        const bool bInput = Candidate->Direction == EGPD_Input;
        if ((bWantsInput && !bInput) || (bWantsOutput && bInput))
        {
            continue;
        }
        if (Candidate->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
        {
            Pin = Candidate;
            break;
        }
        Candidates.Add(FString::Printf(TEXT("%s:%s"), bInput ? TEXT("in") : TEXT("out"),
                                       *Candidate->PinName.ToString()));
    }

    if (!Pin)
    {
        Fail(Out, TEXT("pin_not_found"),
             FString::Printf(TEXT("No pin '%s' on node '%s'"), *PinName, *NodeGuid), Candidates);
        return false;
    }
    if (UEdGraphSchema_Niagara::PinToTypeDefinition(Pin) == FNiagaraTypeDefinition::GetParameterMapDef())
    {
        Fail(Out, TEXT("structural_pin"),
             FString::Printf(TEXT("'%s' is the parameter-map pin that holds the module chain together"), *PinName));
        return false;
    }

    const FString RemovedPinName = Pin->PinName.ToString();

    const FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP remove_niagara_scratch_pad_pin")));
    System->Modify();
    Graph->Modify();
    Node->Modify();

    Pin->BreakAllPinLinks();
    Node->RemovePin(Pin);

    // A Custom HLSL node's signature is compiled state: a removed pin has to leave it, or the old
    // pin keeps being compiled (which is how an unusable Position pin breaks the whole VM compile).
    if (Node->IsA<UNiagaraNodeCustomHlsl>())
    {
        RebuildCustomHlslSignatureFromPins(Node);
    }

    Graph->NotifyGraphChanged();
    System->MarkPackageDirty();

    FUnrealMCPNiagaraResult Readback;
    ListScratchPadNodes(AssetPath, EmitterName, ModuleRef, Readback);

    Out.bSuccess = true;
    Out.NodeGuid = NodeGuid;
    Out.Value = RemovedPinName;
    Out.ModuleGuid = Call->NodeGuid.ToString();
    Out.ModuleName = Call->GetFunctionName();
    Out.Nodes = Readback.Nodes;
    Out.Pins = Readback.Pins;
    Out.RemainingNodes = Readback.Nodes.Num();
    return true;
}

bool FUnrealMCPNiagaraOps::ConnectScratchPadPins(const FString& AssetPath, const FString& EmitterName,
                                                 const FString& ModuleRef, const FString& FromNode,
                                                 const FString& FromPin, const FString& ToNode,
                                                 const FString& ToPin, FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* System = nullptr;
    if (!ResolveSystem(AssetPath, System, Out))
    {
        return false;
    }
    FNiagaraEmitterHandle* Handle = nullptr;
    if (!ResolveEmitter(System, EmitterName, Handle, Out))
    {
        return false;
    }

    UNiagaraNodeFunctionCall* Call = FindModuleNode(Handle->GetEmitterData(), ModuleRef);
    UNiagaraGraph* Graph = GetScratchGraph(Call);
    if (!Graph)
    {
        Fail(Out, TEXT("scratch_pad_not_found"),
             FString::Printf(TEXT("No scratch-pad module '%s' on emitter '%s'"), *ModuleRef, *EmitterName));
        return false;
    }

    UEdGraphNode* FromNodeObj = FindNodeByGuid(Graph, FromNode);
    UEdGraphNode* ToNodeObj = FindNodeByGuid(Graph, ToNode);
    if (!FromNodeObj || !ToNodeObj)
    {
        Fail(Out, TEXT("node_not_found"),
             FString::Printf(TEXT("Could not resolve '%s' -> '%s' in the scratch graph"),
                 *FromNode, *ToNode));
        return false;
    }

    UEdGraphPin* SourcePin = FindPinByName(FromNodeObj, FromPin, EGPD_Output, /*bConstrainDirection=*/true);
    UEdGraphPin* TargetPin = FindPinByName(ToNodeObj, ToPin, EGPD_Input, /*bConstrainDirection=*/true);
    if (!SourcePin || !TargetPin)
    {
        TArray<FString> Candidates;
        for (UEdGraphPin* Pin : FromNodeObj->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Output)
            {
                Candidates.Add(FString::Printf(TEXT("%s:out:%s"), *FromNode, *Pin->PinName.ToString()));
            }
        }
        for (UEdGraphPin* Pin : ToNodeObj->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Input)
            {
                Candidates.Add(FString::Printf(TEXT("%s:in:%s"), *ToNode, *Pin->PinName.ToString()));
            }
        }
        Fail(Out, TEXT("pin_not_found"),
             FString::Printf(TEXT("Could not resolve '%s' (out) -> '%s' (in)"), *FromPin, *ToPin), Candidates);
        return false;
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP connect_niagara_scratch_pad_pins")));
    Graph->Modify();
    const UEdGraphSchema* Schema = Graph->GetSchema();
    if (!Schema)
    {
        Fail(Out, TEXT("schema_missing"), TEXT("The scratch graph has no schema"));
        return false;
    }

    // The schema may insert a conversion node of its own. That is how a vec3 gets wired into a
    // Position-typed parameter: the connect reports success, and the inserted NiagaraNodeConvert is
    // what later dies in the VM compile ("Component ... of variable selction_result has no valid
    // offset"). Snapshot the node set so the insertion can be named instead of discovered later.
    TSet<const UEdGraphNode*> NodesBefore;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        NodesBefore.Add(Node);
    }

    const bool bConnected = Schema->TryCreateConnection(SourcePin, TargetPin);
    if (!bConnected)
    {
        // A Position (LWC) endpoint is the usual reason a hand-built connection is refused, and the
        // engine's own message for it is a bare false: name that case instead of repeating "check types".
        const bool bLwcInvolved =
            UEdGraphSchema_Niagara::PinToTypeDefinition(SourcePin) == FNiagaraTypeDefinition::GetPositionDef()
            || UEdGraphSchema_Niagara::PinToTypeDefinition(TargetPin) == FNiagaraTypeDefinition::GetPositionDef();
        if (bLwcInvolved)
        {
            Fail(Out, TEXT("connect_failed"),
                 FString::Printf(TEXT("The schema refused '%s.%s' -> '%s.%s': a Position (LWC) endpoint only "
                                     "accepts another Position, so a vec3 (a Custom HLSL output) cannot be fed "
                                     "into Particles.Position"), *FromNode, *FromPin, *ToNode, *ToPin),
                 TArray<FString>{ TEXT("position"), TEXT("vec3") });
            return false;
        }
        Fail(Out, TEXT("connect_failed"),
             FString::Printf(TEXT("The schema refused the connection '%s' -> '%s'; check the pin types"),
                 *FromPin, *ToPin));
        return false;
    }

    Graph->NotifyGraphChanged();
    System->MarkPackageDirty();

    UClass* ConvertClass = GetNiagaraEditorNodeClass(TEXT("NiagaraNodeConvert"));
    if (ConvertClass)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && !NodesBefore.Contains(Node) && Node->IsA(ConvertClass))
            {
                Out.InsertedConversionNodes.Add(Node->NodeGuid.ToString());
            }
        }
    }

    Out.bSuccess = true;
    Out.bConnected = SourcePin->LinkedTo.Contains(TargetPin) && TargetPin->LinkedTo.Contains(SourcePin);
    return true;
}

// =====================================================================
// Level actor + component parameters
// =====================================================================

bool FUnrealMCPNiagaraOps::SpawnNiagaraActor(const FString& SystemPath, const FString& ActorName,
                                             const FVector& Location, const FRotator& Rotation, bool bAutoActivate,
                                             FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    UNiagaraSystem* System = nullptr;
    if (!ResolveSystem(SystemPath, System, Out))
    {
        return false;
    }
    if (ActorName.IsEmpty())
    {
        Fail(Out, TEXT("invalid_params"), TEXT("Missing 'actor_name'"));
        return false;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Fail(Out, TEXT("no_editor_world"), TEXT("There is no editor world to spawn into"));
        return false;
    }

    // Same guard as spawn_actor: a name the engine treats as taken must answer, not assert.
    if (TSharedPtr<FJsonObject> NameTaken = FUnrealMCPCommonUtils::MakeNameTakenResponseIfTaken(World, ActorName))
    {
        Fail(Out, TEXT("name_taken"), FString::Printf(TEXT("An actor named '%s' already exists"), *ActorName));
        return false;
    }

    FActorSpawnParameters SpawnParameters;
    SpawnParameters.Name = FName(*ActorName);
    SpawnParameters.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Required_ErrorAndReturnNull;
    SpawnParameters.ObjectFlags |= RF_Transactional;

    const FTransform Transform(Rotation, Location);
    ANiagaraActor* Actor = World->SpawnActor<ANiagaraActor>(ANiagaraActor::StaticClass(), Transform, SpawnParameters);
    if (!Actor)
    {
        Fail(Out, TEXT("spawn_failed"), FString::Printf(TEXT("Spawning a Niagara actor named '%s' failed"), *ActorName));
        return false;
    }

    UNiagaraComponent* Component = Actor->GetNiagaraComponent();
    if (!Component)
    {
        Fail(Out, TEXT("spawn_failed"), TEXT("The spawned actor has no Niagara component"));
        return false;
    }

    Component->SetAsset(System);
    Component->SetAutoActivate(bAutoActivate);
    if (bAutoActivate)
    {
        Component->Activate(/*bReset=*/true);
    }

    World->MarkPackageDirty();

    Out.bSuccess = true;
    // GetName(), not GetActorNameOrLabel(): the label is the class's default ("NiagaraActor") until
    // someone renames it in the outliner, which is not what a caller who passed actor_name wants back.
    Out.ActorName = Actor->GetName();
    Out.ActorPath = Actor->GetPathName();
    Out.AssetPath = System->GetPathName();
    Out.Value = Component->GetPathName();
    return true;
}

bool FUnrealMCPNiagaraOps::SetComponentParameter(const FString& ActorName, const FString& ParamName,
                                                 const FString& ParamType, const FString& Value,
                                                 FUnrealMCPNiagaraResult& Out)
{
    Out = FUnrealMCPNiagaraResult();

    if (ActorName.IsEmpty() || ParamName.IsEmpty())
    {
        Fail(Out, TEXT("invalid_params"), TEXT("Both 'actor_name' and 'param_name' are required"));
        return false;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Fail(Out, TEXT("no_editor_world"), TEXT("There is no editor world"));
        return false;
    }

    // Prefer the play world while PIE runs: that is the world whose behaviour the caller is looking at.
    if (FUnrealMCPCommonUtils::IsPlaySessionRunning() && GEditor->PlayWorld)
    {
        World = GEditor->PlayWorld;
    }

    ANiagaraActor* Actor = nullptr;
    for (TActorIterator<ANiagaraActor> It(World); It; ++It)
    {
        if (It->GetActorNameOrLabel().Equals(ActorName, ESearchCase::IgnoreCase)
            || It->GetName().Equals(ActorName, ESearchCase::IgnoreCase))
        {
            Actor = *It;
            break;
        }
    }
    if (!Actor)
    {
        Fail(Out, TEXT("actor_not_found"), FString::Printf(TEXT("No Niagara actor named '%s' in the editor world"), *ActorName));
        return false;
    }

    UNiagaraComponent* Component = Actor->GetNiagaraComponent();
    if (!Component)
    {
        Fail(Out, TEXT("component_not_found"), TEXT("The actor has no Niagara component"));
        return false;
    }

    const FString FullName = NormalizeUserParameterName(ParamName);
    FNiagaraUserRedirectionParameterStore& Store = Component->GetOverrideParameters();

    FNiagaraTypeDefinition Type;
    if (!ResolveParamType(ParamType, Type))
    {
        Fail(Out, TEXT("unknown_parameter_type"),
             FString::Printf(TEXT("Unknown param_type '%s'"), *ParamType), ParameterTypeCandidates());
        return false;
    }
    if (Type == FNiagaraTypeDefinition::GetPositionDef())
    {
        Fail(Out, TEXT("unsupported_parameter_type"),
             TEXT("Position overrides are not supported on a component; use vec3"), ParameterTypeCandidates());
        return false;
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("UnrealMCP set_niagara_component_parameter")));
    Component->Modify();

    const FNiagaraVariable Variable(Type, FName(*FullName));
    if (!WriteParameterValue(Store, Variable, Value, /*bAddIfMissing=*/true))
    {
        Fail(Out, TEXT("write_failed"),
             FString::Printf(TEXT("Could not write '%s' from '%s'"), *FullName, *Value));
        return false;
    }

    FString Readback;
    if (!ReadParameterValue(Store, Variable, Readback))
    {
        Fail(Out, TEXT("readback_failed"), FString::Printf(TEXT("'%s' was written but cannot be read back"), *FullName));
        return false;
    }

    World->MarkPackageDirty();

    Out.bSuccess = true;
    Out.ActorName = Actor->GetName();
    Out.ActorPath = Actor->GetPathName();
    Out.Value = Readback;
    Out.ValueType = TypeNameOf(Type);
    Out.ParamName = FullName;
    return true;
}
