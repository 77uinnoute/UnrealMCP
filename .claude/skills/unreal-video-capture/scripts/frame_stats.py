"""Objective per-frame statistics: is the effect bright, blown out, or colour-shifted?

Runs on the HOST python:
    python frame_stats.py [dir] [frame ...]

Why this exists: "looks washed out / looks too white" is not reproducible, these
numbers are.  A pixel counts as BRIGHT when max(RGB) > 0.55 and as NEAR_WHITE when
min(RGB) > 0.80.  Columns to watch:

    bright    - how many pixels carry the effect (0 = empty phase, the gap between loops)
    mean      - mean RGB of the bright area: the colour the effect actually reads as
    B-R       - hue lean; a cyan/blue effect must be clearly positive, ~0 means grey
    near_white- share of the bright area with no colour left (blown out)
    px>0.9    - share of the whole frame above 0.9 = clipping pressure
    frame_mean- whole-frame mean, handy for "is this frame an empty gap"

Typical readings from a healthy cyan effect: B-R = +50..+110, near_white < 0.15.
Everything at (150,170,190) with near_white ~ 0.4 means the render is being
over-exposed into white - check the scene exposure before touching the material.
"""
import os
import sys

import numpy as np
from PIL import Image

D = sys.argv[1] if len(sys.argv) > 1 else r"E:\out\seq"
FRAMES = [int(a) for a in sys.argv[2:]] or [0, 3, 7, 10, 14, 17, 20, 24, 31]

for f in FRAMES:
    path = os.path.join(D, "f%04d.png" % f)
    if not os.path.exists(path):
        continue
    a = np.asarray(Image.open(path).convert("RGB")).astype(np.float32) / 255.0
    m = a.max(axis=2) > 0.55
    if m.sum() < 50:
        print("f%04d  (nothing bright)   frame_mean=%.4f" % (f, a.mean()))
        continue
    c = a[m].mean(axis=0) * 255.0
    print("f%04d  bright=%6d  mean=%.0f,%.0f,%.0f  B-R=%+5.0f  near_white=%.2f  px>0.9=%.3f  frame_mean=%.4f"
          % (f, m.sum(), c[0], c[1], c[2], c[2] - c[0],
             (a[m].min(axis=1) > 0.80).mean(), (a.max(axis=2) > 0.9).mean(), a.mean()))
