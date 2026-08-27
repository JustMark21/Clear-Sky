import numpy as np
import zmq
import matplotlib.pyplot as plt

WIDTH = 64
HEIGHT = 64
ENDPOINT = "tcp://localhost:5556"

ctx = zmq.Context()
sub = ctx.socket(zmq.SUB)
sub.connect(ENDPOINT)
sub.setsockopt(zmq.SUBSCRIBE, b"")

fig, ax = plt.subplots()
im = ax.imshow(np.zeros((HEIGHT, WIDTH), dtype=np.float32), origin="lower", vmin=0.0, vmax=1.0)
plt.ion()
plt.show()

while True:
    data = sub.recv()
    grid = np.frombuffer(data, dtype=np.float32).reshape(HEIGHT, WIDTH)
    im.set_data(grid)
    fig.canvas.draw_idle()
    fig.canvas.flush_events()
    plt.pause(0.01)
