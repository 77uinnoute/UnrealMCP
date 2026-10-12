"""Wait for an MRQ render to finish by watching the frame count.

Runs on the HOST python (not inside the editor):
    python wait_mrq_frames.py [dir] [need_frames]

MRQ renders from another process, and the editor-side progress text is not a
usable signal - the first run compiles shaders and sits at 0.0 for a long time.
The only reliable "done" is "the output directory stopped growing".
"""
import os
import sys
import time

D = sys.argv[1] if len(sys.argv) > 1 else r"E:\out\seq"
NEED = int(sys.argv[2]) if len(sys.argv) > 2 else 100
STEP = 6            # poll interval, seconds
IDLE_LIMIT = 8      # stop after ~48 s without a new frame

last, idle = -1, 0
for i in range(120):                                   # ~12 min ceiling
    count = len(os.listdir(D)) if os.path.isdir(D) else 0
    print("%4ds  frames=%d" % (i * STEP, count), flush=True)
    if count >= NEED:
        break
    if count == last and count > 0:
        idle += 1
        if idle >= IDLE_LIMIT:
            print("stalled at", count)
            break
    else:
        idle = 0
    last = count
    time.sleep(STEP)

print("FINAL", len(os.listdir(D)) if os.path.isdir(D) else 0)
