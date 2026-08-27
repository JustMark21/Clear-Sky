#!/usr/bin/env bash
# run_demo.sh -- one-command orchestration for the L3S-LEO hybrid pipeline.
#
# Spawns, in order:
#   1. the C++ engine (./build/l3s_engine --source live), binding its
#      outbound telemetry PUB (tcp://*:5556) and inbound ingestion PULL
#      (tcp://*:5557) sockets;
#   2. python/ingestion_worker.py, which connects to the engine's PULL
#      socket and feeds it real (or, on any failure, synthetic) scenes;
#   3. python/subscriber.py in the FOREGROUND, so the dashboard window
#      opens immediately and the script blocks until it's closed.
#
# Ctrl+C, or closing the dashboard window, cleanly stops every process
# this script started -- see cleanup() below.
#
# Usage: ./run_demo.sh [--region NAME|LAT0,LAT1,LON0,LON1] [--width N]
#                       [--height N] [--overpasses N] [--fps N]
#                       [--interval SEC] [--offline]
set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
cd "$SCRIPT_DIR" || exit 1

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
WIDTH=480
HEIGHT=320
OVERPASSES=4
FPS=4
INTERVAL=20
REGION="agulhas"
OFFLINE=0
PERSISTENT_STORM_PROBABILITY=0.0

ENGINE_BIN="${L3S_ENGINE_BIN:-./build/l3s_engine}"
VENV_PY="${L3S_VENV_PY:-.venv/bin/python}"

# Region-preset sizing: presets store a CENTER point only; the actual
# bounding box is computed at runtime from --width/--height so it is
# proportional to whatever aspect ratio is actually requested (not just
# whichever resolution the preset happened to be tuned for last). See
# compute_bbox() below. ~0.01 deg/pixel is jplMURSST41's native grid
# resolution (verified against the live ERDDAP endpoint); MARGIN adds
# headroom against grid-alignment rounding; MAX_SPAN_DEG caps the AOI so a
# very large --width/--height doesn't turn into a multi-million-row CSV
# fetch (4 overpasses' worth, sequentially) that stalls the demo.
MUR_DEG_PER_PIXEL=0.01
BBOX_MARGIN=1.15
MAX_SPAN_DEG=12.0

PUB_PORT=5556
INGEST_PORT=5557
PUB_ENDPOINT="tcp://*:${PUB_PORT}"
PUB_HOST_ENDPOINT="tcp://localhost:${PUB_PORT}"
INGEST_ENDPOINT="tcp://*:${INGEST_PORT}"
INGEST_HOST_ENDPOINT="tcp://localhost:${INGEST_PORT}"
PORT_WAIT_TIMEOUT_S=10

# ---------------------------------------------------------------------------
# Usage / argument parsing
# ---------------------------------------------------------------------------
usage() {
    cat <<'EOF'
run_demo.sh -- one-command launcher for the L3S-LEO hybrid pipeline
  (C++ engine + live ingestion worker + dashboard, wired together)

Usage: ./run_demo.sh [options]

  --region NAME|LAT0,LAT1,LON0,LON1
        Preset region, or a custom bounding box. Presets name a CENTER
        point only -- the actual box is computed from --width/--height at
        launch time, proportional to whatever aspect ratio you request, so
        it always fills the grid instead of being tuned for one specific
        resolution and squeezed/clipped at any other:
          agulhas     (default) Agulhas Current, South Africa -- paper Fig. 8/9
          florida     Straits of Florida / Gulf Stream inflow
          gulfstream  Gulf Stream off Cape Hatteras
          monterey    Monterey Bay, CA -- paper Fig. 6/7
        Custom example: --region 20.0,26.0,-158.0,-150.0
        (custom boxes are used exactly as given, NOT auto-scaled -- if the
        span is too small for your --width/--height, run_demo.sh warns)

  --width N        grid width, px  (default: 480)
  --height N       grid height, px (default: 320)
  --overpasses N   overpasses/scene -- the dashboard's raw-pass block is a
                   fixed 2x2 layout, so leave this at 4 unless you also
                   intend to see blank/hidden panel slots (default: 4)
  --fps N          engine's target publish rate (default: 4)
  --interval SEC   ingestion worker's seconds between scenes (default: 20)
  --offline        force the worker to --source synthetic (zero network
                   calls) -- use this to rehearse with no live dependency
  --persistent-storm-probability P
                   probability in [0,1] that a given storm is "persistent"
                   -- locked to the same position across all overpasses in
                   a scene instead of jittering independently per pass, so
                   it can mask the IDENTICAL pixels in every raw overpass
                   simultaneously. Forwarded as-is to ingestion_worker.py
                   (default: 0.0 -- disabled, matches prior behavior)
  -h, --help       show this help and exit

Ctrl+C, or closing the dashboard window, cleanly stops every process this
script started (engine + worker) -- no orphaned processes or held ports.
EOF
}

need_value() {
    # $1 = flag name, $2 = the next token (or unset)
    if [[ -z "${2:-}" || "${2:-}" == --* ]]; then
        echo "run_demo.sh: ${1} requires a value" >&2
        exit 1
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --region)      need_value "$1" "${2:-}"; REGION="$2"; shift 2 ;;
        --width)       need_value "$1" "${2:-}"; WIDTH="$2"; shift 2 ;;
        --height)      need_value "$1" "${2:-}"; HEIGHT="$2"; shift 2 ;;
        --overpasses)  need_value "$1" "${2:-}"; OVERPASSES="$2"; shift 2 ;;
        --fps)         need_value "$1" "${2:-}"; FPS="$2"; shift 2 ;;
        --interval)    need_value "$1" "${2:-}"; INTERVAL="$2"; shift 2 ;;
        --offline)     OFFLINE=1; shift ;;
        --persistent-storm-probability)
                       need_value "$1" "${2:-}"; PERSISTENT_STORM_PROBABILITY="$2"; shift 2 ;;
        -h|--help)     usage; exit 0 ;;
        *)
            echo "run_demo.sh: unrecognized argument: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

# ---------------------------------------------------------------------------
# Region presets -> lat0 lat1 lon0 lon1
#
# Presets below are CENTER POINTS ONLY. The box is computed here, at
# launch time, from the actual --width/--height, so it is proportional to
# whatever aspect ratio is requested (4:3, 3:2, 16:9, 16:10, ...) instead
# of being sized for one specific resolution and under-filled/clipped at
# any other -- which is what happened when these were fixed boxes.
# ---------------------------------------------------------------------------
compute_bbox() {
    # $1=lat_center $2=lon_center $3=width_px $4=height_px -> "lat0 lat1 lon0 lon1"
    awk -v latc="$1" -v lonc="$2" -v w="$3" -v h="$4" \
        -v degpx="$MUR_DEG_PER_PIXEL" -v margin="$BBOX_MARGIN" -v maxspan="$MAX_SPAN_DEG" '
        BEGIN {
            lon_span = w * degpx * margin
            lat_span = h * degpx * margin
            if (lon_span > maxspan) lon_span = maxspan
            if (lat_span > maxspan) lat_span = maxspan
            printf "%.3f %.3f %.3f %.3f\n", latc - lat_span/2, latc + lat_span/2, \
                                             lonc - lon_span/2, lonc + lon_span/2
        }'
}

resolve_region() {
    local name="$1" width="$2" height="$3"
    local lat_c lon_c
    case "$name" in
        agulhas)    lat_c=-36.0; lon_c=21.0 ;;
        florida)    lat_c=26.0;  lon_c=-80.0 ;;
        gulfstream) lat_c=35.0;  lon_c=-73.0 ;;
        monterey)   lat_c=36.5;  lon_c=-122.0 ;;
        *)
            # Custom "LAT0,LAT1,LON0,LON1" -- used exactly as given (the
            # user supplied explicit numbers on purpose, so this is not
            # auto-scaled the way presets are); checked for under-fill
            # below instead.
            if [[ "$name" =~ ^-?[0-9]+([.][0-9]+)?,-?[0-9]+([.][0-9]+)?,-?[0-9]+([.][0-9]+)?,-?[0-9]+([.][0-9]+)?$ ]]; then
                echo "${name//,/ }"
                return 0
            else
                return 1
            fi
            ;;
    esac
    compute_bbox "$lat_c" "$lon_c" "$width" "$height"
}

if ! REGION_VALUES="$(resolve_region "$REGION" "$WIDTH" "$HEIGHT")"; then
    echo "run_demo.sh: unrecognized --region '$REGION'" >&2
    echo "  presets: agulhas, florida, gulfstream, monterey" >&2
    echo "  or a custom bbox: --region LAT0,LAT1,LON0,LON1" >&2
    exit 1
fi
read -r LAT0 LAT1 LON0 LON1 <<< "$REGION_VALUES"

# Sanity check the RESOLVED box against the RESOLVED grid, regardless of
# whether it came from a preset (should always pass, by construction) or
# a custom bbox (might not) -- catches the exact under-fill/clipping bug
# this whole mechanism exists to prevent, instead of letting it show up
# silently as a squeezed map in the dashboard.
NEED_LAT_DEG=$(awk -v h="$HEIGHT" -v dp="$MUR_DEG_PER_PIXEL" 'BEGIN{print h*dp}')
NEED_LON_DEG=$(awk -v w="$WIDTH"  -v dp="$MUR_DEG_PER_PIXEL" 'BEGIN{print w*dp}')
if awk -v a="$LAT0" -v b="$LAT1" -v need="$NEED_LAT_DEG" 'BEGIN{d=b-a; if(d<0)d=-d; exit !(d<need)}'; then
    echo "[run_demo] WARNING: region lat span may under-fill a ${HEIGHT}px-tall grid" \
         "(need >= ${NEED_LAT_DEG} deg at MUR's ~${MUR_DEG_PER_PIXEL} deg/px) -- the map may show blank padding" >&2
fi
if awk -v a="$LON0" -v b="$LON1" -v need="$NEED_LON_DEG" 'BEGIN{d=b-a; if(d<0)d=-d; exit !(d<need)}'; then
    echo "[run_demo] WARNING: region lon span may under-fill a ${WIDTH}px-wide grid" \
         "(need >= ${NEED_LON_DEG} deg at MUR's ~${MUR_DEG_PER_PIXEL} deg/px) -- the map may show blank padding" >&2
fi

# ---------------------------------------------------------------------------
# Pre-flight checks -- fail fast with an actionable message, before
# spawning anything, rather than partway through startup.
# ---------------------------------------------------------------------------
if [[ ! -x "$ENGINE_BIN" ]]; then
    echo "run_demo.sh: engine binary not found or not executable: $ENGINE_BIN" >&2
    echo "  build it first:" >&2
    echo "    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j\$(nproc)" >&2
    exit 1
fi
if [[ ! -x "$VENV_PY" ]]; then
    echo "run_demo.sh: python venv not found: $VENV_PY" >&2
    echo "  create it first:" >&2
    echo "    python3 -m venv .venv" >&2
    echo "    .venv/bin/pip install -r python/requirements.txt -r python/requirements-ingestion.txt" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Cleanup / signal trapping -- armed BEFORE anything is spawned, so even a
# failure partway through startup still cleans up whatever did start.
# ---------------------------------------------------------------------------
ENGINE_PID=""
WORKER_PID=""
CLEANED_UP=0

cleanup() {
    # Idempotent: EXIT always fires after an INT/TERM-triggered cleanup
    # too (standard bash behavior), so guard against running this twice.
    if [[ "$CLEANED_UP" -eq 1 ]]; then
        return
    fi
    CLEANED_UP=1

    echo
    echo "[run_demo] shutting down..."

    local pid
    for pid in "$WORKER_PID" "$ENGINE_PID"; do
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null
        fi
    done

    # Both processes handle SIGTERM/SIGINT for a clean shutdown (the
    # engine closes its ZMQ sockets via RAII, the worker via its own
    # try/finally); give them a brief grace period, then escalate to
    # SIGKILL for anything still alive so no process -- and no bound
    # ZeroMQ port -- is ever left behind.
    local waited=0
    while (( waited < 20 )); do
        local any_alive=0
        for pid in "$WORKER_PID" "$ENGINE_PID"; do
            if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
                any_alive=1
            fi
        done
        (( any_alive == 0 )) && break
        sleep 0.25
        waited=$((waited + 1))
    done
    for pid in "$WORKER_PID" "$ENGINE_PID"; do
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            echo "[run_demo] pid $pid still alive after grace period, sending SIGKILL"
            kill -KILL "$pid" 2>/dev/null
        fi
    done

    wait "$WORKER_PID" "$ENGINE_PID" 2>/dev/null
    echo "[run_demo] all processes stopped, ports released."
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------------------
# Port-bound check -- confirms the engine's ZMQ sockets are actually
# listening before the worker (or we) try to talk to them, instead of a
# blind sleep. Prefers `ss` (present on this system); falls back to a
# plain bash /dev/tcp probe if `ss` isn't available elsewhere.
# ---------------------------------------------------------------------------
port_is_listening() {
    local port="$1"
    if command -v ss >/dev/null 2>&1; then
        ss -ltn 2>/dev/null | awk 'NR>1{print $4}' | grep -qE "[:.]${port}\$" && return 0
        return 1
    fi
    ( exec 3<>"/dev/tcp/127.0.0.1/${port}" ) 2>/dev/null && { exec 3>&- 3<&-; return 0; }
    return 1
}

wait_for_port() {
    local port="$1" label="$2" timeout_s="$3"
    local elapsed=0
    printf '[run_demo] waiting for %s to bind port %s' "$label" "$port"
    while (( elapsed < timeout_s )); do
        if ! kill -0 "$ENGINE_PID" 2>/dev/null; then
            echo
            echo "[run_demo] ERROR: engine (pid $ENGINE_PID) exited before binding its sockets -- see its output above" >&2
            exit 1
        fi
        if port_is_listening "$port"; then
            echo " -- bound (~${elapsed}s)"
            return 0
        fi
        printf '.'
        sleep 0.5
        elapsed=$((elapsed + 1))
    done
    echo
    echo "[run_demo] WARNING: $label port $port not confirmed bound after ${timeout_s}s -- proceeding anyway" >&2
    return 1
}

# ---------------------------------------------------------------------------
# 1. Spawn the C++ engine
# ---------------------------------------------------------------------------
echo "[run_demo] region=$REGION  bbox lat[$LAT0, $LAT1] lon[$LON0, $LON1]  grid ${WIDTH}x${HEIGHT}  overpasses=$OVERPASSES"
echo "[run_demo] starting engine:"
echo "  $ENGINE_BIN --width $WIDTH --height $HEIGHT --overpasses $OVERPASSES --fps $FPS \\"
echo "      --endpoint $PUB_ENDPOINT --source live --ingest-endpoint $INGEST_ENDPOINT"
"$ENGINE_BIN" \
    --width "$WIDTH" --height "$HEIGHT" --overpasses "$OVERPASSES" --fps "$FPS" \
    --endpoint "$PUB_ENDPOINT" \
    --source live --ingest-endpoint "$INGEST_ENDPOINT" &
ENGINE_PID=$!
echo "[run_demo] engine pid=$ENGINE_PID"

# 2. Wait for both engine sockets to actually be bound before the worker
#    starts pushing scenes at it.
wait_for_port "$PUB_PORT" "engine PUB" "$PORT_WAIT_TIMEOUT_S" || true
wait_for_port "$INGEST_PORT" "engine PULL" "$PORT_WAIT_TIMEOUT_S" || true

# ---------------------------------------------------------------------------
# 3. Spawn the Python ingestion worker
# ---------------------------------------------------------------------------
SOURCE_MODE="auto"
if [[ "$OFFLINE" -eq 1 ]]; then
    SOURCE_MODE="synthetic"
fi

echo "[run_demo] starting ingestion worker (source=$SOURCE_MODE, persistent-storm-probability=$PERSISTENT_STORM_PROBABILITY):"
echo "  $VENV_PY python/ingestion_worker.py --engine-endpoint $INGEST_HOST_ENDPOINT \\"
echo "      --width $WIDTH --height $HEIGHT --overpasses $OVERPASSES --interval $INTERVAL \\"
echo "      --source $SOURCE_MODE --lat0 $LAT0 --lat1 $LAT1 --lon0 $LON0 --lon1 $LON1 \\"
echo "      --persistent-storm-probability $PERSISTENT_STORM_PROBABILITY"
"$VENV_PY" python/ingestion_worker.py \
    --engine-endpoint "$INGEST_HOST_ENDPOINT" \
    --width "$WIDTH" --height "$HEIGHT" --overpasses "$OVERPASSES" \
    --interval "$INTERVAL" \
    --source "$SOURCE_MODE" \
    --lat0 "$LAT0" --lat1 "$LAT1" --lon0 "$LON0" --lon1 "$LON1" \
    --persistent-storm-probability "$PERSISTENT_STORM_PROBABILITY" &
WORKER_PID=$!
echo "[run_demo] ingestion worker pid=$WORKER_PID"

sleep 1
if ! kill -0 "$WORKER_PID" 2>/dev/null; then
    echo "[run_demo] ERROR: ingestion worker exited immediately -- check python/requirements*.txt are installed" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# 4. Launch the dashboard in the FOREGROUND. Closing its window (or
#    Ctrl+C) returns control here, the script reaches its end, and the
#    EXIT trap tears everything else down.
# ---------------------------------------------------------------------------
echo "[run_demo] launching dashboard -- close its window, or press Ctrl+C here, to stop everything"
echo
"$VENV_PY" python/subscriber.py --endpoint "$PUB_HOST_ENDPOINT"

echo "[run_demo] dashboard closed."
