#include "Commands/PCG/UnrealMCPPCGGraphFactory.h"

#include "PCGGraph.h"

UUnrealMCPPCGGraphFactory::UUnrealMCPPCGGraphFactory()
{
    SupportedClass = UPCGGraph::StaticClass();
    bCreateNew = true;
    bEditAfterNew = false;
}

UObject* UUnrealMCPPCGGraphFactory::FactoryCreateNew(UClass* Class, UObject* InParent, FName Name,
                                                     EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn)
{
    return NewObject<UPCGGraph>(InParent, Class ? Class : UPCGGraph::StaticClass(), Name, Flags);
}
