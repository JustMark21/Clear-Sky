import numpy as np
import zmq
import matplotlib.pyplot as plt

WIDTH = 64
HEIGHT = 64
ENDPOINT = "tcp://localhost:5556"

# Matches the simulator's baseline (288.15 K) plus its peak warm-core
# excess (6.0 K), with a little headroom so the warm core never clips.
VMIN_K = 286.0
VMAX_K = 296.0

ctx = zmq.Context()
sub = ctx.socket(zmq.SUB)
sub.connect(ENDPOINT)
sub.setsockopt(zmq.SUBSCRIBE, b"")

fig, ax = plt.subplots()
im = ax.imshow(
    np.zeros((HEIGHT, WIDTH), dtype=np.float32),
    origin="lower",
    cmap="inferno",
    vmin=VMIN_K,
    vmax=VMAX_K,
)
fig.colorbar(im, ax=ax, label="SST (K)")
plt.ion()
plt.show()

while True:
    data = sub.recv()
    field = np.frombuffer(data, dtype=np.float32).reshape(HEIGHT, WIDTH)
    im.set_data(field)
    fig.canvas.draw_idle()
    fig.canvas.flush_events()
    plt.pause(0.01)
