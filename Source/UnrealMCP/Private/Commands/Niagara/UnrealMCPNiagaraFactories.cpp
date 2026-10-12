#include "Commands/Niagara/UnrealMCPNiagaraFactories.h"

#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"

UUnrealMCPNiagaraSystemFactory::UUnrealMCPNiagaraSystemFactory()
{
    SupportedClass = UNiagaraSystem::StaticClass();
    bCreateNew = true;
    bEditAfterNew = false;
}

UObject* UUnrealMCPNiagaraSystemFactory::FactoryCreateNew(UClass* Class, UObject* InParent, FName Name,
                                                          EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn)
{
    UNiagaraSystem* System = NewObject<UNiagaraSystem>(InParent, Class ? Class : UNiagaraSystem::StaticClass(),
                                                      Name, Flags | RF_Transactional);
    if (!System)
    {
        return nullptr;
    }

    // The engine factory's own path (NiagaraSystemFactoryNew.cpp:137-139): without this the system
    // has no system spawn/update graph output nodes and no SystemUpdate module.
    UNiagaraSystemFactoryNew::InitializeSystem(System, /*bCreateDefaultNodes=*/true);
    return System;
}
