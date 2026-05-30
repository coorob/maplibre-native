#!/usr/bin/env bash
set -euo pipefail

SIMULATOR_ID="${SIMULATOR_ID:-E9CF466F-71D5-41A3-B780-9665538FA552}"
BUNDLE_ID="${BUNDLE_ID:-app.klattra.dev}"
RUN_NAME="${1:-safe_direct}"
RELIEF_MODE="${2:-safe}"
RUN_MODE="${3:-playback}"
OUT_DIR="${OUT_DIR:-/tmp/klattra_evidence_runs}/${RUN_NAME}"
TRIGGER_FILE="${TRIGGER_FILE:-/tmp/klattra_capture_now}"

usage() {
    cat >&2 <<EOF
usage:
  $0 [run_name] [safe|off] [playback|manual]
  $0 trigger

examples:
  $0 safe_direct safe playback
  $0 off_direct off playback
  $0 manual_bad_frame safe manual
  $0 trigger

manual mode launches the app with trigger-based terrain-drape dumps enabled.
When the artifact is visible, run "$0 trigger" from another shell.
EOF
}

if [[ "${RUN_NAME}" == "trigger" ]]; then
    mkdir -p "$(dirname "${TRIGGER_FILE}")"
    touch "${TRIGGER_FILE}"
    echo "triggered ${TRIGGER_FILE}"
    exit 0
fi

if [[ "${RELIEF_MODE}" != "safe" && "${RELIEF_MODE}" != "off" ]]; then
    usage
    exit 2
fi

if [[ "${RUN_MODE}" != "playback" && "${RUN_MODE}" != "manual" ]]; then
    usage
    exit 2
fi

rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}"

xcrun simctl terminate "${SIMULATOR_ID}" "${BUNDLE_ID}" >/dev/null 2>&1 || true

xcrun simctl spawn "${SIMULATOR_ID}" log stream \
    --style compact \
    --level debug \
    --predicate 'process == "App"' > "${OUT_DIR}/oslog.txt" 2>&1 &
LOG_PID=$!

cleanup() {
    kill "${LOG_PID}" >/dev/null 2>&1 || true
    wait "${LOG_PID}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

sleep 1

launch_env=(
    SIMCTL_CHILD_KLATTRA_LOG_TERRAIN_FINAL=1
)

if [[ "${RELIEF_MODE}" == "off" ]]; then
    launch_env+=(
        SIMCTL_CHILD_KLATTRA_ALLOW_TERRAIN_LIGHT_OVERRIDE=1
        SIMCTL_CHILD_KLATTRA_TERRAIN_SHADER_LIGHT=0
    )
fi

if [[ "${RUN_MODE}" == "playback" ]]; then
    launch_env+=(
        SIMCTL_CHILD_KLATTRA_FORCE_CAMERA=1
        SIMCTL_CHILD_KLATTRA_CAMERA_LAT=68.020616
        SIMCTL_CHILD_KLATTRA_CAMERA_LON=18.694648
        SIMCTL_CHILD_KLATTRA_CAMERA_DISTANCE=18150
        SIMCTL_CHILD_KLATTRA_CAMERA_PITCH=55
        SIMCTL_CHILD_KLATTRA_CAMERA_HEADING=215
        SIMCTL_CHILD_KLATTRA_CAMERA_PLAYBACK=1
        SIMCTL_CHILD_KLATTRA_CAMERA_PLAYBACK_LOOPS=2
        SIMCTL_CHILD_KLATTRA_CAMERA_PLAYBACK_STEP_SEC=1.6
        SIMCTL_CHILD_KLATTRA_CAMERA_PLAYBACK_PAUSE_SEC=0.45
        SIMCTL_CHILD_KLATTRA_CAMERA_PLAYBACK_DELAY_SEC=8.0
        SIMCTL_CHILD_KLATTRA_LOG_DRAPE_TRACE=1
    )
else
    rm -f "${TRIGGER_FILE}"
    mkdir -p "${OUT_DIR}/screens" "${OUT_DIR}/render_targets"
    launch_env+=(
        SIMCTL_CHILD_KLATTRA_CAPTURE_TRIGGER_FILE="${TRIGGER_FILE}"
        SIMCTL_CHILD_KLATTRA_DUMP_TRIGGER_FILE="${TRIGGER_FILE}"
        SIMCTL_CHILD_KLATTRA_DUMP_TRIGGER_WINDOW_MS="${DUMP_TRIGGER_WINDOW_MS:-4500}"
        SIMCTL_CHILD_KLATTRA_DUMP_DIR="${OUT_DIR}/render_targets"
        SIMCTL_CHILD_KLATTRA_DUMP_RENDER_TARGETS=terrain-drape
        SIMCTL_CHILD_KLATTRA_DUMP_RENDER_TARGETS_REPEAT=1
        SIMCTL_CHILD_KLATTRA_DUMP_RENDER_TARGET_MAX="${DUMP_MAX_TARGETS:-200}"
        SIMCTL_CHILD_KLATTRA_LOG_TARGET_PIXELS=terrain-drape
        SIMCTL_CHILD_KLATTRA_LOG_TARGET_PIXELS_REPEAT=1
        SIMCTL_CHILD_KLATTRA_LOG_TERRAIN_FINAL_REPEAT=1
        SIMCTL_CHILD_KLATTRA_LOG_DRAPE_TRACE=1
    )
fi

env "${launch_env[@]}" \
    xcrun simctl launch --terminate-running-process "${SIMULATOR_ID}" "${BUNDLE_ID}" \
    > "${OUT_DIR}/launch.txt" 2>&1

if [[ "${RUN_MODE}" == "manual" ]]; then
    echo "manual capture is armed"
    echo "  output:  ${OUT_DIR}"
    echo "  trigger: ${TRIGGER_FILE}"
    echo "run this when the artifact is visible:"
    echo "  TRIGGER_FILE=${TRIGGER_FILE} $0 trigger"

    capture_index=0
    last_mtime=""
    while true; do
        if [[ -e "${TRIGGER_FILE}" ]]; then
            current_mtime="$(stat -f '%m' "${TRIGGER_FILE}")"
            if [[ "${current_mtime}" != "${last_mtime}" ]]; then
                last_mtime="${current_mtime}"
                capture_index=$((capture_index + 1))
                burst_dir="${OUT_DIR}/screens/capture_${capture_index}"
                mkdir -p "${burst_dir}"
                echo "capture ${capture_index}: triggered at $(date -u '+%Y-%m-%dT%H:%M:%SZ')"

                previous=0
                for t in 0 1 2 4 7 11; do
                    sleep $((t - previous))
                    xcrun simctl io "${SIMULATOR_ID}" screenshot "${burst_dir}/screen_t${t}.png" >/dev/null
                    echo "capture ${capture_index} t=${t} saved"
                    previous="${t}"
                done
            fi
        fi
        sleep 0.25
    done
fi

previous=0
for t in 2 5 8 10 12 14 16 19 22 25 28 32 36 40 45 50; do
    sleep $((t - previous))
    xcrun simctl io "${SIMULATOR_ID}" screenshot "${OUT_DIR}/screen_t${t}.png" >/dev/null
    echo "${RUN_NAME} t=${t} captured"
    previous="${t}"
done

sleep 2
