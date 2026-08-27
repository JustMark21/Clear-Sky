import copy

import numpy as np
import zmq
import matplotlib.pyplot as plt

WIDTH = 64
HEIGHT = 64
NUM_OVERPASSES = 4
ENDPOINT = "tcp://localhost:5556"

# Matches the simulator's baseline (288.15 K) plus its peak warm-core
# excess (6.0 K), with a little headroom so the warm core never clips.
VMIN_K = 286.0
VMAX_K = 296.0

SST_CMAP = copy.copy(plt.get_cmap("inferno"))
SST_CMAP.set_bad(color="#000000")

ctx = zmq.Context()
sub = ctx.socket(zmq.SUB)
sub.connect(ENDPOINT)
sub.setsockopt(zmq.SUBSCRIBE, b"")

fig, (ax_raw, ax_lvza) = plt.subplots(1, 2, figsize=(10, 5))

blank = np.zeros((HEIGHT, WIDTH), dtype=np.float32)
im_raw = ax_raw.imshow(blank, origin="lower", cmap=SST_CMAP, vmin=VMIN_K, vmax=VMAX_K)
im_lvza = ax_lvza.imshow(blank, origin="lower", cmap=SST_CMAP, vmin=VMIN_K, vmax=VMAX_K)
ax_lvza.set_title("LVZA composite")
fig.colorbar(im_lvza, ax=[ax_raw, ax_lvza], label="SST (K)")
plt.ion()
plt.show()

raw_cycle_index = 0

while True:
    parts = sub.recv_multipart()
    overpasses = [
        np.frombuffer(p, dtype=np.float32).reshape(HEIGHT, WIDTH) for p in parts[:NUM_OVERPASSES]
    ]
    lvza = np.frombuffer(parts[NUM_OVERPASSES], dtype=np.float32).reshape(HEIGHT, WIDTH)

    # Only one raw-overpass panel exists so far -- cycle through the
    # available overpasses one per frame rather than picking a fixed one.
    ax_raw.set_title(f"raw overpass {raw_cycle_index}")
    im_raw.set_data(overpasses[raw_cycle_index])
    raw_cycle_index = (raw_cycle_index + 1) % len(overpasses)

    im_lvza.set_data(lvza)

    fig.canvas.draw_idle()
    fig.canvas.flush_events()
    plt.pause(0.01)
