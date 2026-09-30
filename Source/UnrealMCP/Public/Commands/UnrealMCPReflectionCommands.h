#pragma once

#include "CoreMinimal.h"
#include "Json.h"
#include "Core/MCPCommandRegistry.h"

/**
 * Handler class for the property probe and the generic property writer.
 *
 * The probe answers "what is this property, what can it accept, and why can it not be written" from the
 * FProperty itself. Python cannot do this: get_editor_property only exposes editable properties, so the
 * full type picture (including the ones nothing can write) exists only on this side.
 *
 * The writer exists for the targets python cannot reach at all - a sub-object of an asset whose class has
 * no python bindings (a cloth config object is the case that forced it): it resolves the object path
 * through reflection instead of through python's type mapping, and it can write several properties with a
 * single save. The probe uses the same resolver, so both commands address any sub-object path.
 *
 * Both also accept a property path ("LodData[0].PhysicalMeshData.WeightMaps[1].Values"), which is the only
 * way to reach baked per-vertex data such as a cloth weight mask: those leaves are not properties of the
 * object, and python cannot see the clothing asset's LOD data at all.
 *
 * Path resolution is shared with the clothing commands (see Reflection/MCPObjectPathResolver.h), so a path
 * means the same thing everywhere.
 */
class UNREALMCP_API FUnrealMCPReflectionCommands
{
public:
    FUnrealMCPReflectionCommands();

    // Register this domain's commands with the process-wide command registry.
    void RegisterCommands(FMCPCommandRegistry& Registry);

private:
    TSharedPtr<FJsonObject> HandleReflectProbe(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetObjectProperty(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListEnumValues(const TSharedPtr<FJsonObject>& Params);
};
