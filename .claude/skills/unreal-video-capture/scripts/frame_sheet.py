"""Montage MRQ frames into one labelled contact sheet.

Runs on the HOST python:
    python frame_sheet.py [dir] [out.png] [n_frames] [cols] [cell_w] [cell_h]

n_frames > 0 picks that many frames evenly spread over the whole sequence;
n_frames = 0 falls back to PICKS below (one effect cycle, 51 frames at 30 fps).

Keep the cells big (>= 640 px).  Downscaling a bright thin effect into a 320 px
thumbnail averages it with the black background and it turns into a dim grey
tube - you will judge the look from an artefact of the montage, not the render.
MRQ frames are already display-referred: view the sheet as it is, do not boost it.
"""
import os
import sys

from PIL import Image, ImageDraw

D = sys.argv[1] if len(sys.argv) > 1 else r"E:\out\seq"
OUT = sys.argv[2] if len(sys.argv) > 2 else r"E:\out\sheet.png"
N = int(sys.argv[3]) if len(sys.argv) > 3 else 0
COLS = int(sys.argv[4]) if len(sys.argv) > 4 else 4
CW = int(sys.argv[5]) if len(sys.argv) > 5 else 640
CH = int(sys.argv[6]) if len(sys.argv) > 6 else 360

PICKS = [0, 3, 7, 10, 14, 17, 20, 24, 27, 31, 34, 37, 41, 44, 48]

files = sorted(f for f in os.listdir(D) if f.endswith(".png"))
if N:
    picks = [int(files[min(i * len(files) // N, len(files) - 1)][1:5]) for i in range(N)]
else:
    picks = PICKS

rows = (len(picks) + COLS - 1) // COLS
sheet = Image.new("RGB", (CW * COLS, CH * rows), (0, 0, 0))
draw = ImageDraw.Draw(sheet)
for i, frame in enumerate(picks):
    path = os.path.join(D, "f%04d.png" % frame)
    if not os.path.exists(path):
        continue
    tile = Image.open(path).convert("RGB").resize((CW - 4, CH - 4), Image.LANCZOS)
    x, y = (i % COLS) * CW + 2, (i // COLS) * CH + 2
    sheet.paste(tile, (x, y))
    draw.text((x + 4, y + 4), "f%04d" % frame, fill=(255, 255, 0))

sheet.save(OUT)
print("sheet ->", OUT, sheet.size, "frames:", len(files))
