"""Engine-version compatibility for the PYTHON side.

The twin of `Source/UnrealMCP/Private/Compat/UnrealMCPVersionCompat.h`: the plugin supports UE 5.5 - 5.8
from one tree, so every `unreal` python API that differs between those releases is funnelled through this
module instead of being guarded with `hasattr` at each call site. A new engine version should be a change
HERE, not a hunt through tool wrappers and fixture scripts.

Each entry states what changed, from which engine version, the replacement, and what a caller gets on an
older engine. Callers use the helper and never probe the API themselves.

Measured on 5.5.4 (this project's engine):
  * `unreal.Package` exposes NEITHER `set_dirty_flag` NOR `is_dirty` (both are 5.6+), but `modify()` does
    mark the package dirty - see set_package_dirty below for why only one half has a fallback;
  * `EditorLoadingAndSavingUtils.get_dirty_map_packages() / get_dirty_content_packages()` work on 5.5+,
    so dirtiness can always be READ even when it cannot be cleared.

Import safety: the MCP server runs in its own process without the `unreal` module, so importing this file
there must not fail. Every helper that touches the engine calls _require_unreal() first.
"""

import re

try:
    import unreal
except ImportError:  # the MCP server process: no engine, no `unreal` module
    unreal = None


# -------------------------------------------------------------------------------------------
# Capability registry: one row per known difference, so the set is greppable
# -------------------------------------------------------------------------------------------
# Keys are `Class.member`. `since` is the engine version that introduced the member (the version a
# caller may rely on it unconditionally). `fallback` says what happens on an older engine.
CAPABILITIES = {
    "Package.set_dirty_flag": {
        "since": (5, 6),
        "fallback": "set_package_dirty(..., True) uses Package.modify(); clearing a flag is impossible "
                    "on 5.5 (see set_package_dirty)",
    },
    "Package.is_dirty": {
        "since": (5, 6),
        "fallback": "is_package_dirty() falls back to EditorLoadingAndSavingUtils.get_dirty_* membership",
    },
}


def _require_unreal():
    if unreal is None:
        raise RuntimeError(
            "unreal_mcp_compat only works inside the editor: this process has no `unreal` module "
            "(the MCP server is a plain python process - run the helper through execute_python_*)"
        )


# -------------------------------------------------------------------------------------------
# Engine version
# -------------------------------------------------------------------------------------------
_ENGINE_VERSION = None


def engine_version():
    """(major, minor) of the running editor, e.g. (5, 5). Cached: it cannot change while we run."""
    _require_unreal()
    global _ENGINE_VERSION
    if _ENGINE_VERSION is None:
        raw = str(unreal.SystemLibrary.get_engine_version())  # '5.5.4-40574608+++UE5+Release-5.5'
        match = re.match(r"(\d+)\.(\d+)", raw)
        _ENGINE_VERSION = (int(match.group(1)), int(match.group(2))) if match else (0, 0)
    return _ENGINE_VERSION


def engine_version_string():
    """The full version string the engine reports, for replies and logs."""
    _require_unreal()
    return str(unreal.SystemLibrary.get_engine_version())


def is_at_least(major, minor):
    """`engine_version() >= (major, minor)`, for the rare case where the helper below does not exist yet."""
    return engine_version() >= (major, minor)


def class_available(name):
    """Is a reflected class (e.g. 'MoviePipelineQueue') exposed to python here?

    A plugin that is disabled by default (`MovieRenderPipeline` is one) contributes no classes, which is
    easy to misread as "this engine cannot do it" - ask this before concluding that.
    """
    _require_unreal()
    return hasattr(unreal, name)


def exposed(prefix):
    """Every reflected name starting with `prefix`, sorted - the probe form of class_available()."""
    _require_unreal()
    return sorted(name for name in dir(unreal) if name.startswith(prefix))


def capability_status():
    """{key: 'native' | 'fallback' | 'unsupported'} for every row in CAPABILITIES, on this engine."""
    _require_unreal()
    status = {}
    for key in CAPABILITIES:
        owner_name, member = key.split(".", 1)
        owner = getattr(unreal, owner_name, None)
        if owner is None:
            status[key] = "unsupported"  # the class itself is not exposed here
        elif hasattr(owner, member):
            status[key] = "native"
        else:
            status[key] = "fallback"  # the class is there, the member is not (the 5.5 case)
    return status


# -------------------------------------------------------------------------------------------
# Package dirtiness
# -------------------------------------------------------------------------------------------
def package_of(obj):
    """The UPackage holding `obj` (an object, or a package itself). None when `obj` has no package.

    get_package() exists on both supported engines, so this needs no version branch.
    """
    _require_unreal()
    if obj is None:
        return None
    if isinstance(obj, unreal.Package):
        return obj
    if hasattr(obj, "get_package"):
        return obj.get_package()
    return None


def dirty_packages():
    """(map packages, content packages) that are unsaved. Read-only, and stable across versions."""
    _require_unreal()
    return (
        list(unreal.EditorLoadingAndSavingUtils.get_dirty_map_packages()),
        list(unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages()),
    )


def is_package_dirty(obj):
    """Is the package holding `obj` unsaved? None when `obj` has no package."""
    _require_unreal()
    package = package_of(obj)
    if package is None:
        return None

    if hasattr(package, "is_dirty"):  # 5.6+: the direct answer
        return package.is_dirty()

    maps, content = dirty_packages()  # 5.5: the package lists are the only source
    return any(package == entry for entry in maps + content)


def set_package_dirty(obj, dirty=True):
    """Mark (or clear) the unsaved flag of the package holding `obj`. True when it was applied.

    What changed: 5.6 exposed `UPackage.set_dirty_flag`. On 5.5 there is NO setter in the python API and
    no equivalent for the clearing direction - so this function is honest about being one-sided:

      * 5.6+  : `set_dirty_flag(dirty)` does both directions.
      * 5.5   : `modify()` marks the package dirty (measured: `pkg.modify()` puts it straight into
                `get_dirty_content_packages()`), so MARKING works. CLEARING does not - nothing in the 5.5
                python API takes a flag back off a package. Returning False here is the answer, not an
                exception: callers that wanted a clean package have to restart the editor without saving.

    Note the bridge's own editor-world writes mark the level dirty on the C++ side (UPackage::
    SetDirtyFlag is reachable there), so this helper is for editor-side scripts of our own - probes,
    fixtures, verification - not for making an MCP command persist.
    """
    _require_unreal()
    package = package_of(obj)
    if package is None:
        return False

    if hasattr(package, "set_dirty_flag"):  # 5.6+
        package.set_dirty_flag(dirty)
        return True

    if dirty and hasattr(package, "modify"):  # 5.5 fallback, marking only
        package.modify()
        return True

    return False
