"""Editor-side implementation of the MCP tool `preview_material_expression`.

Three steps, each one bridge command from tools/material_tools.py, which prints the result
back as `MCP_PREVIEW_JSON:<json>`:

    build(request)  -> bridge build_material_preview (C++): clone the target expression and
                       its upstream chain into a NEW throw-away material, unlit, wired into
                       EmissiveColor, synchronously recompiled. Returns the state below.
    render(job)     -> draw that material into an LDR render target, read the pixels, encode
                       the PNG. Repeatable: the caller polls it until the image settles.
    cleanup(state)  -> delete the throw-away material.

Why the building step lives in C++: draw_material_to_render_target evaluates a whole MATERIAL,
and a material that was DUPLICATED in the session keeps rendering its source's compiled shader
(no python-side call refreshes it: RecompileMaterial, save, reload, rename, usage flags, a
material instance parent were all tried). Only C++ reaches ForceRecompileForRendering /
FMaterialUpdateContext, and only a material built from scratch renders its own graph.

Diagnostic only: no lighting, no camera, no scene - it never replaces looking at the viewport.

The PNG is encoded here instead of with export_render_target: the FCanvas draw runs with a
colour-only blend state, so the render target's alpha is 0 everywhere and export_render_target
writes an RGBA PNG that every viewer shows as fully transparent. Encoding it here also writes
the linear target values as sRGB (which is how the image is displayed) and lets the samples be
reported in their real 0..1 linear units.
"""

import json
import os
import struct
import zlib

import unreal

PREVIEW_COMMAND = "build_material_preview"
TEMP_FOLDER = "/Game/MCP/_Preview"
SAVED_SUBDIR = "MCPMaterialPreview"
CHANNELS = ("rgba", "r", "g", "b", "a")
DEFAULT_SIZE = 256
MIN_SIZE = 8
MAX_SIZE = 512


class PreviewError(Exception):
    """Structured failure: an error_code plus arbitrary context for the caller."""

    def __init__(self, code, message, **extra):
        super(PreviewError, self).__init__(message)
        self.code = code
        self.message = message
        self.extra = extra

    def to_result(self):
        out = {"success": False, "error_code": self.code, "message": self.message}
        out.update(self.extra)
        return out


# --------------------------------------------------------------------------------------
# bridge loopback
# --------------------------------------------------------------------------------------

def _bridge(command, **params):
    raw = unreal.UnrealMCPPythonAPI.execute_mcp_command(command, json.dumps(params))
    try:
        return json.loads(raw)
    except Exception:
        raise PreviewError("bridge_bad_response",
                           "%s did not return JSON" % command, raw=str(raw)[:400])


def _bridge_result(command, **params):
    """Run a bridge command and return its inner result, or raise PreviewError."""
    response = _bridge(command, **params)
    inner = response.get("result")
    if response.get("status") != "success":
        detail = dict(inner) if isinstance(inner, dict) else {}
        code = detail.pop("error", None) or "bridge_command_failed"
        detail.pop("success", None)
        # The handler's own payload (candidates, available_outputs, ...) is what makes the error
        # actionable, so surface it on the error itself instead of nesting it away.
        message = detail.pop("message", None) or response.get("error") or code
        detail["bridge_command"] = command
        raise PreviewError(code, message, **detail)
    if not isinstance(inner, dict):
        raise PreviewError("bridge_bad_result", "%s returned no result object" % command)
    return inner


# --------------------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------------------

def _editor_world():
    subsystem = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    return subsystem.get_editor_world()


def _rendering_library():
    for name in ("RenderingLibrary", "KismetRenderingLibrary"):
        library = getattr(unreal, name, None)
        if library is not None and hasattr(library, "draw_material_to_render_target"):
            return library
    raise PreviewError("rendering_library_missing",
                       "unreal.RenderingLibrary.draw_material_to_render_target is not exposed")


def _sanitize(text, limit=32):
    cleaned = "".join(ch if (ch.isalnum() or ch == "_") else "_" for ch in (text or ""))
    return cleaned[:limit] or "expr"


def _delete_asset(asset_path):
    """Delete an asset without a save prompt (preview materials never hit the disk)."""
    if not unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        return
    asset = unreal.EditorAssetLibrary.load_asset(asset_path)
    if asset is not None:
        unreal.EditorAssetLibrary.delete_loaded_asset(asset)


def _linear_to_srgb_byte(value):
    linear = min(1.0, max(0.0, value))
    if linear <= 0.0031308:
        encoded = linear * 12.92
    else:
        encoded = 1.055 * (linear ** (1.0 / 2.4)) - 0.055
    return int(round(encoded * 255.0))


def _write_png(path, pixels, size):
    """Write an 8-bit RGB PNG from linear 0..255 samples (see the module docstring)."""
    raw = bytearray()
    for y in range(size):
        raw.append(0)  # filter type 0 (None) for every scanline
        base = y * size
        for x in range(size):
            color = pixels[base + x]
            raw.append(_linear_to_srgb_byte(color.r / 255.0))
            raw.append(_linear_to_srgb_byte(color.g / 255.0))
            raw.append(_linear_to_srgb_byte(color.b / 255.0))

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    header = struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)
    with open(path, "wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n")
        handle.write(chunk(b"IHDR", header))
        handle.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        handle.write(chunk(b"IEND", b""))


def _sample_rt(pixels, size):
    """A 3x3 numeric signature of the target, in the material's own 0..1 linear units."""
    samples = []
    for fy in (0.25, 0.5, 0.75):
        for fx in (0.25, 0.5, 0.75):
            x = min(size - 1, max(0, int(fx * size)))
            y = min(size - 1, max(0, int(fy * size)))
            color = pixels[y * size + x]
            samples.append([color.r / 255.0, color.g / 255.0, color.b / 255.0])
    mean = [sum(s[i] for s in samples) / len(samples) for i in range(3)]
    return {"grid": 3, "unit": "linear_0_1", "center": samples[4], "mean": mean}


# --------------------------------------------------------------------------------------
# steps
# --------------------------------------------------------------------------------------

def build(request):
    """Ask the C++ side for a preview material of one expression (returns state + info)."""
    asset_path = str(request.get("asset_path") or "").strip()
    if not asset_path:
        raise PreviewError("missing_asset_path", "asset_path is required")
    if not unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        raise PreviewError("material_not_found", "no asset at %s" % asset_path)
    expression_name = request.get("expression_name")
    expression_desc = request.get("expression_desc")
    if not expression_name and not expression_desc:
        raise PreviewError("missing_locator", "expression_name or expression_desc is required")

    channel = str(request.get("channel") or "rgba").lower()
    if channel not in CHANNELS:
        raise PreviewError("invalid_channel", "channel must be one of %s" % (", ".join(CHANNELS),))
    output_index = max(0, int(request.get("output_index") or 0))
    size = int(request.get("size") or DEFAULT_SIZE)
    if size < MIN_SIZE or size > MAX_SIZE:
        raise PreviewError("invalid_size", "size must be between %d and %d" % (MIN_SIZE, MAX_SIZE))

    asset_name = asset_path.rstrip("/").split("/")[-1].split(".")[0]
    temp_name = "PM_%s__%s" % (asset_name, _sanitize(expression_desc or expression_name))
    temp_asset_path = "%s/%s" % (TEMP_FOLDER, temp_name)

    # The command refuses to write into an existing asset; stale previews of the same node are ours.
    _delete_asset(temp_asset_path)

    params = {
        "asset_path": asset_path,
        "output_index": output_index,
        "channel": channel,
        "temp_name": temp_name,
        "temp_folder": TEMP_FOLDER,
    }
    if expression_name:
        params["expression_name"] = expression_name
    if expression_desc:
        params["expression_desc"] = expression_desc

    built = _bridge_result(PREVIEW_COMMAND, **params)

    png_name = "%s__%s__o%d%s.png" % (asset_name, _sanitize(expression_desc or expression_name),
                                     output_index, "" if channel == "rgba" else "__" + channel)
    state = {
        "temp_asset_path": built.get("temp_asset_path", temp_asset_path),
        "size": size,
        "png_name": png_name,
        "channel": channel,
        "output_index": output_index,
    }
    info = {
        "source_asset_path": built.get("source_asset_path"),
        "temp_asset_path": state["temp_asset_path"],
        "expression": {
            "name": built.get("expression_name"),
            "desc": built.get("expression_desc"),
            "type": built.get("expression_type"),
        },
        "output_index": output_index,
        "channel": channel,
        "shading_model": built.get("shading_model"),
        "copied_nodes": built.get("copied_nodes"),
        "channel_mask_created": built.get("channel_mask_created"),
        "available_outputs": built.get("outputs"),
    }
    return {"success": True, "state": state, "info": info}


def render(job):
    """Draw the preview material and (optionally) encode the PNG."""
    state = job.get("state") or {}
    write_png = bool(job.get("write_png"))
    size = int(state.get("size") or DEFAULT_SIZE)
    material = unreal.EditorAssetLibrary.load_asset(state.get("temp_asset_path") or "")
    if material is None:
        raise PreviewError("preview_material_missing",
                           "preview material is gone: %s" % state.get("temp_asset_path"))

    library = _rendering_library()
    world = _editor_world()
    rt = library.create_render_target2d(world, size, size,
                                        unreal.TextureRenderTargetFormat.RTF_RGBA8,
                                        unreal.LinearColor(0.0, 0.0, 0.0, 1.0))
    if rt is None:
        raise PreviewError("rt_create_failed", "could not create the %dx%d LDR render target" % (size, size))
    try:
        library.draw_material_to_render_target(world, rt, material)
        pixels = library.read_render_target_raw(world, rt, True)
        result = {"success": True, "samples": _sample_rt(pixels, size)}
        if write_png:
            out_dir = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir())
            out_dir = os.path.join(out_dir.rstrip("/\\"), SAVED_SUBDIR)
            os.makedirs(out_dir, exist_ok=True)
            png_path = os.path.join(out_dir, state.get("png_name") or "preview.png")
            _write_png(png_path, pixels, size)
            if not os.path.isfile(png_path):
                raise PreviewError("export_failed", "the render target was not written to disk", path=png_path)
            result["png_path"] = png_path
            result["width"] = size
            result["height"] = size
            result["rt_format"] = "RTF_RGBA8"
            result["encoding"] = "srgb8_from_linear"
        return result
    finally:
        library.release_render_target2d(rt)


def cleanup(state):
    """Delete the preview material (and report whether it is gone)."""
    temp_asset_path = (state or {}).get("temp_asset_path")
    if temp_asset_path:
        _delete_asset(temp_asset_path)
    return {"success": True,
            "temp_asset_path": temp_asset_path,
            "deleted": bool(temp_asset_path) and not unreal.EditorAssetLibrary.does_asset_exist(temp_asset_path)}


# --------------------------------------------------------------------------------------
# bridge entry point
# --------------------------------------------------------------------------------------

def run_step(step_name, payload):
    """Dispatch one step for the bridge command: PreviewError never escapes as a traceback."""
    handlers = {"build": build, "render": render, "cleanup": cleanup}
    handler = handlers.get(step_name)
    if handler is None:
        return PreviewError("unknown_step", "unknown preview step: %s" % step_name).to_result()
    try:
        result = handler(payload)
    except PreviewError as error:
        return error.to_result()
    result["success"] = True
    return result
