#pragma once

/**
 * Engine-version compatibility shims.
 *
 * The plugin targets UE 5.5 - 5.8 from one source tree, so every engine API that moved between
 * those releases is funnelled through this file instead of being guarded at each call site: a new
 * engine version should be a change HERE, not a hunt through the command files.
 *
 * Each block states what changed, in which version, and what the replacement is. The version used
 * as the boundary is the one the deprecation was ANNOUNCED in, because Epic's pattern is to
 * deprecate in 5.x (still callable, warning only) and remove in 5.x+1 - guarding on the announced
 * version is what keeps a build against 5.x warning-free and a build against 5.x+1 correct.
 *
 * Version checks use UE_VERSION_OLDER_THAN from Misc/EngineVersionComparison.h.
 */

#include "CoreMinimal.h"
#include "Misc/EngineVersionComparison.h"

// 5.7 stopped delivering FStringOutputDevice through transitive includes. Every ImportText error
// sink (FProperty::ImportText_Direct, UScriptStruct::ImportText) takes an FOutputDevice*, so the
// header has to be included explicitly - without it the declaration fails and each use of the sink
// cascades into "undeclared identifier" plus the const-initialisation errors that follow.
#include "Misc/StringOutputDevice.h"

// -------------------------------------------------------------------------------------------
// Object lookup: ANY_PACKAGE
// -------------------------------------------------------------------------------------------
// 5.1 deprecated ANY_PACKAGE, 5.7 removed it. The replacement for "search every package" is an
// explicit nullptr outer; the old macro is kept for the versions that still have it so 5.5/5.6
// keep the exact code path they were developed and tested on.
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	#define UNREALMCP_ANY_PACKAGE ANY_PACKAGE
#else
	#define UNREALMCP_ANY_PACKAGE nullptr
#endif

// -------------------------------------------------------------------------------------------
// Material: material resource at a feature level
// -------------------------------------------------------------------------------------------
// 5.7 deprecated GetMaterialResource(ERHIFeatureLevel::Type) in favour of the EShaderPlatform
// overload. UMaterial itself only overrides the EShaderPlatform one, which hides the base class's
// feature-level overload: the old call no longer resolves (C2665/C2663), it is not just a warning.
// GetFeatureLevelShaderPlatform maps a feature level to this machine's platform.
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	#define UNREALMCP_MATERIAL_RESOURCE_FOR_FEATURE_LEVEL(MaterialPtr, FeatureLevel) \
		((MaterialPtr)->GetMaterialResource(FeatureLevel))
#else
	#include "RHIGlobals.h"
	#define UNREALMCP_MATERIAL_RESOURCE_FOR_FEATURE_LEVEL(MaterialPtr, FeatureLevel) \
		((MaterialPtr)->GetMaterialResource(GetFeatureLevelShaderPlatform(FeatureLevel)))
#endif

// -------------------------------------------------------------------------------------------
// PCG: component cleanup
// -------------------------------------------------------------------------------------------
// 5.6 dropped the bSave parameter ("Use version with no bSave parameter"), so cleaning up a
// component can no longer both remove and save the generated components: on 5.6+ the generated
// components are always discarded, and the caller's request to keep them is ignored.
#if UE_VERSION_OLDER_THAN(5, 6, 0)
	#define UNREALMCP_PCG_CLEANUP(ComponentPtr, bRemoveComponents, bSaveGeneratedComponents) \
		((ComponentPtr)->Cleanup(bRemoveComponents, bSaveGeneratedComponents))
#else
	#define UNREALMCP_PCG_CLEANUP(ComponentPtr, bRemoveComponents, bSaveGeneratedComponents) \
		do { (void)(bSaveGeneratedComponents); (ComponentPtr)->Cleanup(bRemoveComponents); } while (false)
#endif

// -------------------------------------------------------------------------------------------
// MovieScene: property track name, binding reads, binding removal
// -------------------------------------------------------------------------------------------
// 5.6 deprecated UMovieScenePropertyTrack::UniqueTrackName in favour of GetTrackName(): the engine
// now derives the track name from the property binding that SetPropertyNameAndPath installs, so
// writing the field is both deprecated and redundant.
#if UE_VERSION_OLDER_THAN(5, 6, 0)
	#define UNREALMCP_SET_PROPERTY_TRACK_UNIQUE_NAME(TrackPtr, InTrackName) \
		((TrackPtr)->UniqueTrackName = (InTrackName))
#else
	#define UNREALMCP_SET_PROPERTY_TRACK_UNIQUE_NAME(TrackPtr, InTrackName) \
		do { (void)(TrackPtr); (void)(InTrackName); } while (false)
#endif

// Reading the bindings needs no version branch: casting to const selects the const accessor, which
// exists in every supported version and is the only one 5.7 still allows (the non-const overload is
// deprecated there).
#define UNREALMCP_SCENE_BINDINGS(ScenePtr) (static_cast<const UMovieScene*>(ScenePtr)->GetBindings())

// 5.7 deprecated the non-const GetBindings(), which is the only way to drop a binding entry by hand
// (UMovieScene::RemoveBinding is protected). From 5.7 on, removal goes through RemovePossessable,
// which calls RemoveBinding internally and also fires the binding-removed event.
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	#define UNREALMCP_REMOVE_BINDING_FOR_GUID(ScenePtr, BindingGuid) \
		((ScenePtr)->GetBindings().RemoveAll([&BindingGuid](const FMovieSceneBinding& Binding) \
		{ return Binding.GetObjectGuid() == BindingGuid; }))
#else
	#define UNREALMCP_REMOVE_BINDING_FOR_GUID(ScenePtr, BindingGuid) \
		do { (void)(ScenePtr); (void)(BindingGuid); } while (false)
#endif
