#include "Commands/Common/UnrealMCPComponentLibrary.h"

#include "Components/ActorComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

bool UUnrealMCPComponentLibrary::RegisterComponent(UActorComponent* Component)
{
    if (!Component)
    {
        return false;
    }

    UWorld* World = Component->GetWorld();
    if (!World)
    {
        return false;
    }

    Component->RegisterComponentWithWorld(World);
    return Component->IsRegistered();
}

bool UUnrealMCPComponentLibrary::RegisterComponentWithWorld(UActorComponent* Component, UWorld* World)
{
    if (!Component || !World)
    {
        return false;
    }

    Component->RegisterComponentWithWorld(World);
    return Component->IsRegistered();
}

bool UUnrealMCPComponentLibrary::IsComponentRegistered(const UActorComponent* Component)
{
    return Component && Component->IsRegistered();
}

bool UUnrealMCPComponentLibrary::AddInstanceComponent(AActor* Actor, UActorComponent* Component)
{
    if (!Actor || !Component)
    {
        return false;
    }

    // The component must belong to the actor, exactly like the editor's Add Component requires.
    if (Component->GetOwner() != Actor)
    {
        return false;
    }

    Actor->AddOwnedComponent(Component);

    // This is the part the component tree cares about: it sets CreationMethod to Instance, which is
    // what SubobjectDataSubsystem::ShouldAddInstancedActorComponent checks (Native components without an
    // editable property are filtered out - the reason new_object components never showed up).
    Actor->AddInstanceComponent(Component);
    return true;
}
