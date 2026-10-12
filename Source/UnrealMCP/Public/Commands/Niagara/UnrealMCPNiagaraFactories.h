#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"

#include "UnrealMCPNiagaraFactories.generated.h"

/**
 * Minimal factory for UNiagaraSystem assets.
 *
 * UNiagaraSystemFactoryNew lives in NiagaraEditor/Public but the class itself is not exported, so
 * its FactoryCreateNew cannot be called from another module; only its static InitializeSystem is
 * (NIAGARAEDITOR_API). This factory exists to satisfy FUnrealMCPCommonUtils::CreateAssetDirect:
 * build the system inside the package it is handed, then run the engine's own initialisation so the
 * system spawn/update graphs get their output nodes and the required SystemUpdate module - a system
 * built without that cannot compile.
 */
UCLASS()
class UNREALMCP_API UUnrealMCPNiagaraSystemFactory : public UFactory
{
    GENERATED_BODY()

public:
    UUnrealMCPNiagaraSystemFactory();

    virtual UObject* FactoryCreateNew(UClass* Class, UObject* InParent, FName Name, EObjectFlags Flags,
                                      UObject* Context, FFeedbackContext* Warn) override;
};
