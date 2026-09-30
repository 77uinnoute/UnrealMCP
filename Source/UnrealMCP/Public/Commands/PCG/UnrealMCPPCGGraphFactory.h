#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"

#include "UnrealMCPPCGGraphFactory.generated.h"

class UPCGGraph;

/**
 * Minimal factory for UPCGGraph assets.
 *
 * The engine's own PCGGraphFactory sits in PCGEditor/Private/ (not Public/), so no other module can
 * include it and the class cannot be reused from here. This factory only has to satisfy
 * FUnrealMCPCommonUtils::CreateAssetDirect: build the object inside the package it is handed. It
 * deliberately has no options and creates no subobjects.
 */
UCLASS()
class UNREALMCP_API UUnrealMCPPCGGraphFactory : public UFactory
{
    GENERATED_BODY()

public:
    UUnrealMCPPCGGraphFactory();

    virtual UObject* FactoryCreateNew(UClass* Class, UObject* InParent, FName Name, EObjectFlags Flags,
                                      UObject* Context, FFeedbackContext* Warn) override;
};
