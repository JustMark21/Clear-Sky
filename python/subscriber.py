#!/usr/bin/env python3
"""
L3S-LEO Infrastructure Control Room
====================================

Real-time telemetry dashboard for the C++ l3s_engine backend. Subscribes
to its ZeroMQ PUB socket, decodes the binary frame protocol (see
include/l3s/Telemetry.hpp for the authoritative layout, wire format v2 /
magic "L3S3"), and renders a dark, high-contrast, maps-first command-
center view:

  TOP block (dominant), a 2-row x 4-column grid:
    - cols 0-1, both rows: a 2x2 block of the individual RAW overpasses
      (up to 4 -- see NOTE below), each fragmented/cloud-gapped on its own.
    - col 2, spanning both rows: the legacy LVZA composite (jagged/stitched).
    - col 3, spanning both rows: the fused L3S SST (smooth, SciML-ready).
  Every panel shares one locked vmin/vmax (set by the engine over the
  union of every grid in the frame), so the fragmented -> jagged ->
  smooth progression is visually honest.

  BOTTOM row (compact — engine metrics, ~20% of figure height):
    - Left:  system throughput (MB/s)
    - Right: processing latency (ms)

NOTE on panel count: the 2x2 raw block has exactly 4 fixed slots by
design (per the requested strict layout). The engine's --overpasses count
is a runtime CLI choice and could in principle differ from 4; this script
displays min(num_overpasses, 4) slots and leaves any unused slot visibly
blank rather than crashing or silently stretching the layout, logging a
one-time console warning if the mismatch would hide data (num_overpasses
> 4).

This script does NOT use matplotlib.animation.FuncAnimation -- it never
did; the loop below (zmq poll -> recv_multipart -> draw_idle ->
flush_events) already serves that role without pulling in the Animation
machinery, and canvas redraws are still only ever forced once a whole
frame (every panel + both strip charts + the status line) has been fully
updated.

Usage:
    python3 subscriber.py [--endpoint tcp://localhost:5556]
"""
from __future__ import annotations

import argparse
import struct
import sys
import time
from collections import deque
from dataclasses import dataclass, field

import numpy as np
import zmq
import matplotlib
import matplotlib.pyplot as plt
from matplotlib import gridspec
from matplotlib.colors import LinearSegmentedColormap

# ---------------------------------------------------------------------------
# Wire protocol (must match include/l3s/Telemetry.hpp exactly)
# ---------------------------------------------------------------------------
HEADER_FMT = "<6I4d2f"  # magic,width,height,frame,iters,numOverpasses | ts,exec,thpt,mem | min,max
HEADER_SIZE = struct.calcsize(HEADER_FMT)  # 64 bytes
MAGIC = 0x3353334C  # "L3S3" -- wire format v2 (variable-length raw overpass list)

MAX_RAW_PANELS = 4  # fixed 2x2 block, per the requested strict layout


@dataclass
class Frame:
    width: int
    height: int
    frame_index: int
    debias_iterations: int
    num_overpasses: int
    timestamp: float
    exec_time_ms: float
    throughput_mb_s: float
    mem_footprint_mb: float
    sst_min_k: float
    sst_max_k: float
    overpasses: list[np.ndarray] = field(default_factory=list)  # one raw grid per overpass, NaN gaps
    lvza: np.ndarray = None       # legacy lowest-VZA composite
    clean: np.ndarray = None      # final fused L3S SST


def decode_frame(parts: list[bytes]) -> Frame | None:
    """`parts` is the multipart message with the topic frame already
    stripped: [header, raw_0, ..., raw_{N-1}, lvza, clean]."""
    if len(parts) < 3:  # header + lvza + clean, minimum (N could be 0)
        return None
    header_bytes = parts[0]
    if len(header_bytes) != HEADER_SIZE:
        return None
    (magic, width, height, frame_index, debias_iterations, num_overpasses,
     ts, exec_ms, thpt, mem_mb, sst_min, sst_max) = struct.unpack(HEADER_FMT, header_bytes)
    if magic != MAGIC:
        return None

    expected = 1 + num_overpasses + 2  # header + N raw + lvza + clean
    if len(parts) != expected:
        return None

    shape = (height, width)
    raw_parts = parts[1:1 + num_overpasses]
    lvza_bytes = parts[1 + num_overpasses]
    clean_bytes = parts[2 + num_overpasses]

    overpasses = [np.frombuffer(b, dtype="<f4").reshape(shape) for b in raw_parts]
    lvza = np.frombuffer(lvza_bytes, dtype="<f4").reshape(shape)
    clean = np.frombuffer(clean_bytes, dtype="<f4").reshape(shape)
    return Frame(width, height, frame_index, debias_iterations, num_overpasses, ts, exec_ms, thpt,
                 mem_mb, sst_min, sst_max, overpasses, lvza, clean)


# ---------------------------------------------------------------------------
# Control-room visual theme
# ---------------------------------------------------------------------------
BG = "#050b14"
PANEL_BG = "#0a1220"
GRID_LINE = "#16283f"
FG = "#d8e6f5"
MUTED = "#5c7a99"
ACCENT_CYAN = "#38f2c8"
ACCENT_AMBER = "#ffb454"
ACCENT_RED = "#ff5d6c"
ACCENT_VIOLET = "#a68bff"

# Scientific SST colormap: deep violet (cold) -> cyan -> green -> amber -> red (warm),
# echoing the paper's own SST [K] colorbars (Figs. 2-9) while staying legible on black.
SST_CMAP = LinearSegmentedColormap.from_list(
    "l3s_sst",
    ["#1a0b3d", "#2a2e8f", "#1f7ec2", "#19c3a6", "#7fd957", "#f4e04d", "#f2932b", "#e6483c"],
)

# NaN / no-data (cloud-masked) pixels must be unmistakably a *void*, not just
# another dark shade the eye can mistake for cold water. The coldest SST
# stop above (#1a0b3d) is itself a very dark violet -- a "dark slate" bad
# color sits only ~33/441 RGB-units from it, i.e. visually adjacent. Pure
# black is the maximally-distinct choice (~67/441 from the coldest stop,
# and categorically unlike anything else in the ramp), so a cloud gap can
# never be read as an extreme-cold retrieval.
SST_CMAP.set_bad(color="#000000")


def style_map_axes(ax, title, fontsize=10.5):
    ax.set_facecolor(PANEL_BG)
    ax.set_title(title, color=FG, fontsize=fontsize, fontweight="bold", pad=6, loc="left")
    ax.tick_params(colors=MUTED, labelsize=7)
    for spine in ax.spines.values():
        spine.set_color(GRID_LINE)


def style_chart_axes(ax, title):
    ax.set_facecolor(PANEL_BG)
    ax.set_title(title, color=MUTED, fontsize=8.5, fontweight="bold", pad=4, loc="left")
    ax.tick_params(colors=MUTED, labelsize=7)
    for spine in ax.spines.values():
        spine.set_color(GRID_LINE)
    ax.grid(True, color=GRID_LINE, linewidth=0.5, alpha=0.6)


def make_figure():
    matplotlib.rcParams["font.family"] = "monospace"
    fig = plt.figure(figsize=(17, 9.5), facecolor=BG)
    fig.canvas.manager.set_window_title("L3S-LEO :: Infrastructure Control Room")

    # Outer: maps block on top (~78%), metrics compressed to a slim strip
    # (~20%) below, thin spacer row between them.
    outer = gridspec.GridSpec(
        3, 1, figure=fig,
        height_ratios=[3.9, 0.12, 1.0],
        hspace=0.05,
        left=0.04, right=0.98, top=0.90, bottom=0.06,
    )

    # Maps block: strict 2-row x 4-column mapping --
    #   cols 0-1 (both rows): 2x2 block of raw overpasses
    #   col 2 (rowspan 2):    legacy LVZA composite
    #   col 3 (rowspan 2):    fused L3S SST
    maps_gs = gridspec.GridSpecFromSubplotSpec(
        2, 4, subplot_spec=outer[0], wspace=0.14, hspace=0.22,
        width_ratios=[1, 1, 1.35, 1.35],
    )
    ax_raw = [
        fig.add_subplot(maps_gs[0, 0]),
        fig.add_subplot(maps_gs[0, 1]),
        fig.add_subplot(maps_gs[1, 0]),
        fig.add_subplot(maps_gs[1, 1]),
    ]
    ax_lvza = fig.add_subplot(maps_gs[:, 2])
    ax_clean = fig.add_subplot(maps_gs[:, 3])

    metrics_gs = gridspec.GridSpecFromSubplotSpec(1, 2, subplot_spec=outer[2], wspace=0.10)
    ax_thpt = fig.add_subplot(metrics_gs[0, 0])
    ax_lat = fig.add_subplot(metrics_gs[0, 1])

    for i, ax in enumerate(ax_raw):
        style_map_axes(ax, f"RAW PASS {i + 1}  //  Sensor Overpass", fontsize=9)
    style_map_axes(ax_lvza, "LEGACY FUSION  //  LVZA Composite")
    style_map_axes(ax_clean, "FUSED L3S SST  //  SciML-Ready Tensor")

    style_chart_axes(ax_thpt, "SYSTEM THROUGHPUT  [MB/s]")
    style_chart_axes(ax_lat, "PROCESSING LATENCY  [ms]")

    fig.suptitle("ACSPO L3S-LEO  //  SCIML DATA PREP ENGINE",
                 color=ACCENT_CYAN, fontsize=15, fontweight="bold", x=0.04, ha="left", y=0.975)
    fig.text(0.04, 0.945,
              "4 raw overpasses -> legacy composite -> fused tensor  ·  locked color scale  ·  live telemetry over ZeroMQ",
              color=MUTED, fontsize=9)

    return fig, ax_raw, ax_lvza, ax_clean, ax_thpt, ax_lat


class HudLine:
    """A compact scrolling strip-chart trace with a bright current-value readout."""

    def __init__(self, ax, color, unit, maxlen=180):
        self.ax = ax
        self.color = color
        self.unit = unit
        self.xs = deque(maxlen=maxlen)
        self.ys = deque(maxlen=maxlen)
        (self.line,) = ax.plot([], [], color=color, linewidth=1.4, solid_capstyle="round")
        self.fill = None
        self.label = ax.text(
            0.985, 0.82, "", transform=ax.transAxes, ha="right", va="top",
            color=color, fontsize=12, fontweight="bold", family="monospace",
        )

    def push(self, x, y):
        self.xs.append(x)
        self.ys.append(y)

    def redraw(self):
        if not self.xs:
            return
        xs = list(self.xs)
        ys = list(self.ys)
        self.line.set_data(xs, ys)
        if self.fill is not None:
            self.fill.remove()
        self.fill = self.ax.fill_between(xs, ys, 0, color=self.color, alpha=0.12)
        lo, hi = min(xs), max(xs)
        self.ax.set_xlim(lo, hi if hi > lo else lo + 1)
        ymax = max(ys) * 1.25 if max(ys) > 0 else 1.0
        self.ax.set_ylim(0, ymax)
        self.label.set_text(f"{ys[-1]:,.1f} {self.unit}")


def main():
    parser = argparse.ArgumentParser(description="L3S-LEO telemetry dashboard")
    parser.add_argument("--endpoint", default="tcp://localhost:5556",
                         help="ZeroMQ PUB endpoint of l3s_engine (default: tcp://localhost:5556)")
    args = parser.parse_args()

    ctx = zmq.Context.instance()
    sub = ctx.socket(zmq.SUB)
    sub.setsockopt(zmq.SUBSCRIBE, b"L3S")
    sub.setsockopt(zmq.RCVHWM, 4)
    sub.connect(args.endpoint)
    print(f"[dashboard] subscribed to {args.endpoint}, waiting for l3s_engine frames...")

    plt.ion()
    fig, ax_raw, ax_lvza, ax_clean, ax_thpt, ax_lat = make_figure()
    all_map_axes = ax_raw + [ax_lvza, ax_clean]

    thpt_line = HudLine(ax_thpt, ACCENT_CYAN, "MB/s")
    lat_line = HudLine(ax_lat, ACCENT_AMBER, "ms")

    images: dict[str, object] = {}   # panel key -> AxesImage, created lazily per key
    cbar = None
    warned_overflow = False          # log the >4-overpasses warning at most once
    status_text = fig.text(0.04, 0.015, "", color=MUTED, fontsize=8.5, family="monospace")

    t_start = time.time()
    fps_ema = None
    last_wall = time.time()

    try:
        while plt.fignum_exists(fig.number):
            # Non-blocking poll so the window stays responsive even if the
            # backend stalls; redraw is only forced on a full frame boundary.
            events = sub.poll(timeout=50)
            if not events:
                fig.canvas.flush_events()
                continue

            parts = sub.recv_multipart()
            if len(parts) < 4:  # topic + header + lvza + clean, minimum
                continue
            frame = decode_frame(parts[1:])  # strip the topic frame
            if frame is None:
                continue

            if frame.num_overpasses > MAX_RAW_PANELS and not warned_overflow:
                print(f"[dashboard] WARNING: engine sent {frame.num_overpasses} raw overpasses, "
                      f"but this layout has a fixed {MAX_RAW_PANELS}-panel 2x2 block -- "
                      f"only the first {MAX_RAW_PANELS} are shown.")
                warned_overflow = True

            now = time.time()
            dt = max(1e-6, now - last_wall)
            last_wall = now
            inst_fps = 1.0 / dt
            fps_ema = inst_fps if fps_ema is None else (0.85 * fps_ema + 0.15 * inst_fps)

            elapsed = now - t_start
            thpt_line.push(elapsed, frame.throughput_mb_s)
            lat_line.push(elapsed, frame.exec_time_ms)

            # Locked, shared scale across EVERY panel (set by the engine
            # over the union of all raw + LVZA + clean grids) -- the
            # fragmented -> jagged -> smooth progression must not be an
            # artifact of per-panel auto-scaling.
            vmin, vmax = float(frame.sst_min_k), float(frame.sst_max_k)
            if not np.isfinite(vmin) or not np.isfinite(vmax) or vmin == vmax:
                vmin, vmax = 285.0, 300.0

            extent = (0, frame.width, 0, frame.height)

            # Build this frame's panel data: 4 raw slots (None if the
            # engine sent fewer this frame) + lvza + clean.
            panel_data: list[tuple[object, str, np.ndarray | None]] = []
            for i in range(MAX_RAW_PANELS):
                data = frame.overpasses[i] if i < len(frame.overpasses) else None
                panel_data.append((ax_raw[i], f"raw_{i}", data))
            panel_data.append((ax_lvza, "lvza", frame.lvza))
            panel_data.append((ax_clean, "clean", frame.clean))

            for ax, key, data in panel_data:
                if data is None:
                    # No overpass in this slot this frame -- leave the
                    # panel visibly blank rather than showing stale data
                    # or crashing.
                    if key in images:
                        images[key].remove()
                        del images[key]
                    ax.set_facecolor(PANEL_BG)
                    continue
                masked = np.ma.masked_invalid(data)
                if key not in images:
                    im = ax.imshow(masked, cmap=SST_CMAP, vmin=vmin, vmax=vmax,
                                    origin="lower", extent=extent, aspect="auto",
                                    interpolation="nearest")
                    images[key] = im
                else:
                    images[key].set_data(masked)
                    images[key].set_clim(vmin, vmax)

            if cbar is None and "clean" in images:
                cbar = fig.colorbar(images["clean"], ax=all_map_axes,
                                     orientation="horizontal", shrink=0.35,
                                     pad=0.03, aspect=40, location="top")
                cbar.set_label("SST [K]  (locked scale, all panels)", color=FG, fontsize=8.5)
                cbar.ax.xaxis.set_tick_params(color=MUTED, labelcolor=MUTED, labelsize=7.5)
                cbar.outline.set_edgecolor(GRID_LINE)

            thpt_line.redraw()
            lat_line.redraw()

            status_text.set_text(
                f"frame {frame.frame_index:>6}  |  grid {frame.width}x{frame.height}  |  "
                f"{frame.num_overpasses} raw pass(es)  |  debias iters {frame.debias_iterations}  |  "
                f"RSS {frame.mem_footprint_mb:6.1f} MB  |  dashboard fps {fps_ema:5.1f}  |  "
                f"engine latency {frame.exec_time_ms:6.2f} ms  |  locked SST scale [{vmin:.1f}, {vmax:.1f}] K"
            )

            # Force the redraw only once the whole frame (every heatmap +
            # both strip charts + the status line) has been fully updated,
            # so the canvas never shows a half-updated frame and the loop
            # stays smooth and non-blocking.
            fig.canvas.draw_idle()
            fig.canvas.flush_events()

    except KeyboardInterrupt:
        pass
    finally:
        sub.close(0)
        ctx.term()
        print("\n[dashboard] disconnected.")


if __name__ == "__main__":
    sys.exit(main())
