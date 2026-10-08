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
//
// The declaration moved with it: up to 5.6 FStringOutputDevice is declared in
// Containers/UnrealString.h, and only from 5.7 on does Misc/StringOutputDevice.h have it. Including
// the new header unconditionally is a hard error on 5.5/5.6 (C1083, "cannot open include file"), and
// it is a FATAL one - every translation unit that includes this file dies with it, not just the ones
// that use the import sinks.
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	#include "Containers/UnrealString.h"
#else
	#include "Misc/StringOutputDevice.h"
#endif

// -------------------------------------------------------------------------------------------
// Viewport input: FInputKeyEventArgs construction
// -------------------------------------------------------------------------------------------
// 5.6 added the event-timestamp constructor
// (FViewport*, FInputDeviceId, FKey, EInputEvent, uint64); 5.5 only has the four-argument forms.
// This is not a "slightly different defaults" difference: a five-argument call does not compile on
// 5.5, and the failure does not stay local - with the construction ill-formed, overload resolution
// on UGameViewportClient::InputKey falls back to another overload that returns void, so the caller's
// `const bool bHandled = ...` reports "cannot convert from void to bool" as well.
#if UE_VERSION_OLDER_THAN(5, 6, 0)
	#define UNREALMCP_INPUT_KEY_EVENT_ARGS(ViewportPtr, DeviceId, Key, Event) \
		FInputKeyEventArgs((ViewportPtr), (DeviceId), (Key), (Event))
#else
	#define UNREALMCP_INPUT_KEY_EVENT_ARGS(ViewportPtr, DeviceId, Key, Event) \
		FInputKeyEventArgs((ViewportPtr), (DeviceId), (Key), (Event), /*EventTimestamp=*/uint64(0))
#endif

// -------------------------------------------------------------------------------------------
// Object lookup: ANY_PACKAGE
// -------------------------------------------------------------------------------------------
// 5.1 deprecated ANY_PACKAGE, 5.7 removed it. There is deliberately NO shim for it: mapping it to a
// null outer - which is what a shim is tempted to do - does not mean "search every package", it means
// "only match an object whose outer is null", i.e. a TOP-LEVEL package
// (CoreUObject/Private/UObject/UObjectHash.cpp:1118, the `&& (!Object->GetOuter())` guard). Every
// class lookup written that way silently stopped resolving on 5.7.
//
// The replacement is FUnrealMCPCommonUtils::ResolveUClass, which does the short-name search with
// FindFirstObject: that one hashes on the object name alone (UObjectHash.cpp:1226-1270), so it really
// does search every package, on every supported engine version.

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
