"""Dump a Grid2D RenderTarget as per-tile statistics (Niagara grid debugging).

Why this exists: after a Grid2DCollection is bound to a RenderTarget the engine rewrites the asset into
"one tile per attribute" (row-major, `NumCells` per tile). Neither `read_render_target_pixel` nor
`read_render_target_raw` is trustworthy on float formats (an R16F RT read back as a constant
`(255,0,0,255)` no matter what it held), so the only reliable read is `export_render_target` + decoding
the PNG here. The PNG comes out **16-bit** (colortype 6) - decoding it as 8-bit produces all-zero tiles.

Note: `export_render_target` only writes a usable PNG for single/8-bit formats. An `RTF_RGBA16F` target
produces a ~2 KB junk file (no IHDR), so keep grid RenderTargets on the format the DataInterface
expects (the project's default grid format, `RTF_R16F` on this machine) - see the findings doc.

Usage: set RT_PATH below, run with execute_python_file. Prints one line per tile plus the RT size and
format; `tile = first_slice - slot_count` maps a tile back to an attribute name (see
`list_niagara_grid_attributes` in the skill body).
"""

import struct
import zlib

import unreal

RT_PATH = "/Game/VFX/RT_WaterGrid"
OUT_DIR = "e:/ue_proj/asset_test_UE55 (2)/asset_test_UE55/Saved/"
TILE_WIDTH = 256          # cells per axis (GridResolution)
SAMPLE_STEP = 8           # pixel stride for the statistics


def read_png(path):
    data = open(path, "rb").read()
    pos, idat, header = 8, b"", None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            header = struct.unpack(">IIBBBBB", chunk)
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break
    if header is None:
        raise RuntimeError("no IHDR in %s - export_render_target did not write a PNG "
                           "(float RTs such as RTF_RGBA16F do this)" % path)
    width, height, bitdepth, colortype = header[0], header[1], header[2], header[3]
    raw = zlib.decompress(idat)
    channels = {0: 1, 2: 3, 4: 2, 6: 4}[colortype]
    bpp = channels * (bitdepth // 8)
    stride = width * bpp
    out = bytearray()
    prev = bytearray(stride)
    p = 0
    for _ in range(height):
        ftype = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if ftype == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ftype == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        out += line
        prev = line
    return width, height, bitdepth // 8, channels, bytes(out)


editor = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
world = editor.get_game_world() or editor.get_editor_world()
rt = unreal.load_asset(RT_PATH)
name = RT_PATH.rsplit("/", 1)[-1] + ".png"
unreal.RenderingLibrary.export_render_target(world, rt, OUT_DIR, name)
print("rt:", rt.size_x, rt.size_y, rt.get_editor_property("render_target_format"))

width, height, sample_bytes, channels, pixels = read_png(OUT_DIR + name)
print("decoded:", width, height, "sample_bytes", sample_bytes, "channels", channels)
row_bytes = width * channels * sample_bytes
tiles = width // TILE_WIDTH
print("tiles:", tiles)

for tile in range(tiles):
    x0 = tile * TILE_WIDTH
    values = []
    for y in range(0, height, SAMPLE_STEP):
        for x in range(0, min(TILE_WIDTH, width - x0), SAMPLE_STEP):
            off = y * row_bytes + ((x0 + x) * channels) * sample_bytes
            values.append(struct.unpack_from(">H", pixels, off)[0] if sample_bytes == 2 else pixels[off])
    print("tile %2d  min %5d  max %5d  mean %8.1f  nonzero %4d/%d" % (
        tile, min(values), max(values), sum(values) / len(values),
        sum(1 for v in values if v > 256), len(values)))
