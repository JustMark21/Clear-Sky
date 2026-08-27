import copy
import struct
from dataclasses import dataclass

import numpy as np
import zmq
import matplotlib.pyplot as plt
from matplotlib.gridspec import GridSpec

ENDPOINT = "tcp://localhost:5556"

# Only used to size the initial blank imshow canvases before the first
# real frame arrives -- must match SimulatorConfig's defaults so the
# image extent doesn't have to be re-scaled once real data lands.
WIDTH = 64
HEIGHT = 64

FRAME_MAGIC = 0x3353334C  # "L3S3"
FRAME_VERSION = 1
# 6x uint32 + 3x double, little-endian -- matches Telemetry.hpp's
# FrameHeader byte-for-byte on an x86_64 host.
HEADER_STRUCT = struct.Struct("<6I3d")

# Matches the simulator's baseline (288.15 K) plus its peak warm-core
# excess (6.0 K), with a little headroom so the warm core never clips.
VMIN_K = 286.0
VMAX_K = 296.0

SST_CMAP = copy.copy(plt.get_cmap("inferno"))
SST_CMAP.set_bad(color="#000000")


@dataclass
class DecodedFrame:
    width: int
    height: int
    num_overpasses: int
    frame_index: int
    timestamp_s: float
    exec_ms: float
    throughput_mb_s: float
    overpasses: list
    lvza: np.ndarray
    fused: np.ndarray


def decode_frame(parts):
    """Validates and decodes one multipart frame. Returns None (and logs
    why) for anything malformed, instead of ever reinterpreting bytes
    that don't match what the header claims."""
    if len(parts) < 4:
        print(f"[subscriber] dropped frame: only {len(parts)} parts, expected at least 4")
        return None

    header_bytes = parts[0]
    if len(header_bytes) != HEADER_STRUCT.size:
        print(f"[subscriber] dropped frame: header is {len(header_bytes)} bytes, expected {HEADER_STRUCT.size}")
        return None

    magic, version, width, height, num_overpasses, frame_index, timestamp_s, exec_ms, throughput_mb_s = (
        HEADER_STRUCT.unpack(header_bytes)
    )
    if magic != FRAME_MAGIC:
        print(f"[subscriber] dropped frame: bad magic 0x{magic:08X}")
        return None
    if version != FRAME_VERSION:
        print(f"[subscriber] dropped frame: unsupported version {version}")
        return None

    expected_parts = 1 + num_overpasses + 2  # header + raw overpasses + lvza + fused
    if len(parts) != expected_parts:
        print(f"[subscriber] dropped frame: expected {expected_parts} parts, got {len(parts)}")
        return None

    expected_field_bytes = width * height * 4
    fields = []
    for i, raw in enumerate(parts[1:]):
        if len(raw) != expected_field_bytes:
            print(f"[subscriber] dropped frame: field {i} is {len(raw)} bytes, expected {expected_field_bytes}")
            return None
        fields.append(np.frombuffer(raw, dtype=np.float32).reshape(height, width))

    overpasses = fields[:num_overpasses]
    lvza = fields[num_overpasses]
    fused = fields[num_overpasses + 1]

    return DecodedFrame(
        width, height, num_overpasses, frame_index, timestamp_s, exec_ms, throughput_mb_s, overpasses, lvza, fused
    )


def main():
    ctx = zmq.Context()
    sub = ctx.socket(zmq.SUB)
    sub.connect(ENDPOINT)
    sub.setsockopt(zmq.SUBSCRIBE, b"")

    fig = plt.figure(figsize=(14, 8))
    gs = GridSpec(3, 4, figure=fig, height_ratios=[1, 1, 0.35], hspace=0.35, wspace=0.3)

    ax_raw = [fig.add_subplot(gs[i // 2, i % 2]) for i in range(4)]
    ax_lvza = fig.add_subplot(gs[0:2, 2])
    ax_fused = fig.add_subplot(gs[0:2, 3])
    ax_hud = fig.add_subplot(gs[2, :])
    ax_hud.axis("off")

    blank = np.zeros((HEIGHT, WIDTH), dtype=np.float32)
    im_raw = [ax.imshow(blank, origin="lower", cmap=SST_CMAP, vmin=VMIN_K, vmax=VMAX_K) for ax in ax_raw]
    im_lvza = ax_lvza.imshow(blank, origin="lower", cmap=SST_CMAP, vmin=VMIN_K, vmax=VMAX_K)
    im_fused = ax_fused.imshow(blank, origin="lower", cmap=SST_CMAP, vmin=VMIN_K, vmax=VMAX_K)

    for i, ax in enumerate(ax_raw):
        ax.set_title(f"raw overpass {i}")
    ax_lvza.set_title("LVZA composite (legacy)")
    ax_fused.set_title("Eq. (1) weighted composite")
    fig.colorbar(im_fused, ax=[*ax_raw, ax_lvza, ax_fused], label="SST (K)", fraction=0.02)

    hud_text = ax_hud.text(0.0, 0.5, "", family="monospace", fontsize=11, va="center")

    plt.ion()
    plt.show()

    while True:
        parts = sub.recv_multipart()
        frame = decode_frame(parts)
        if frame is None:
            continue

        for i in range(frame.num_overpasses):
            im_raw[i].set_data(frame.overpasses[i])
        im_lvza.set_data(frame.lvza)
        im_fused.set_data(frame.fused)

        hud_text.set_text(
            f"frame {frame.frame_index:6d}   "
            f"grid {frame.width}x{frame.height}   "
            f"exec {frame.exec_ms:7.2f} ms   "
            f"throughput {frame.throughput_mb_s:7.2f} MB/s"
        )

        fig.canvas.draw_idle()
        fig.canvas.flush_events()
        plt.pause(0.01)


if __name__ == "__main__":
    main()
