#!/bin/sh
# devsoak CI smoke run (brief §4/§11): boot a Copperline image, run the
# 30-second profile against a driver, capture the serial log on the host,
# and fail the build unless devsoak's final verdict line says PASS.
#
# Usage:
#   ci/smoke.sh [CONFIG] [DEVICE] [UNIT] [RANGE] [EXTRA-ARGS...]
# Defaults: test/a1200-scsi.toml scsi.device 0 512,2K
#
# Requirements on the CI host: copperline and xdftool on PATH, and a
# built ./devsoak binary (make). The config's serial sink must be
# "stdout" ([serial] mode = "stdout") so this script can capture it.
# The guest's exit code cannot cross the emulator boundary, so the
# machine-greppable "devsoak: RESULT ..." line is the verdict:
#   PASS -> 0, WARN -> 5, FAIL -> 10, no verdict line at all -> 20
# (a crash/hang before the verdict leaves no RESULT line; the serial log
# then ends with the §16.4 "about to send" breadcrumb naming the
# culprit).

set -u

REPO=$(cd "$(dirname "$0")/.." && pwd)
CONFIG=${1:-"$REPO/test/a1200-scsi.toml"}
case "$CONFIG" in /*) ;; *) CONFIG="$PWD/$CONFIG" ;; esac
DEVICE=${2:-scsi.device}
UNIT=${3:-0}
RANGE=${4:-512,2K}
[ $# -ge 4 ] && shift 4 || true
[ $# -ge 1 ] && [ "$1" = "$CONFIG" ] && shift || true

WORK=${SMOKE_WORKDIR:-$(mktemp -d)}
LOG="$WORK/serial.log"

[ -x "$REPO/devsoak" ] || { echo "smoke: build devsoak first (make)"; exit 20; }
cp "$REPO/devsoak" "$REPO/devsoak.quirks" "$WORK/"

# fresh scratch victim each run: deterministic starting state
( cd "$WORK" && xdftool victim.hdf create size=2Mi + format Victim ffs ) \
    >/dev/null || { echo "smoke: xdftool failed"; exit 20; }

echo "smoke: $DEVICE unit $UNIT range $RANGE via $CONFIG"
( cd "$WORK" && timeout 900 "${COPPERLINE:-copperline}" --config "$CONFIG" --factory \
    --run ./devsoak \
    --run-args "$DEVICE $UNIT -d -r $RANGE -t 30s -w 2 -q 2 -A 0 -W 30 -y -o ser $*" \
    --noaudio --screenshot-after 200 smoke.png ) > "$LOG" 2>"$WORK/emu.err"

VERDICT=$(grep 'devsoak: RESULT' "$LOG" | tail -1)
echo "smoke: serial log: $LOG"
echo "smoke: ${VERDICT:-no RESULT line (guest crashed or hung?)}"

case "$VERDICT" in
    *"RESULT PASS"*) exit 0 ;;
    *"RESULT WARN"*) exit 5 ;;
    *"RESULT FAIL"*) grep -E 'FAIL|MISMATCH|WATCHDOG|GUARD' "$LOG" | head -20
                     exit 10 ;;
    *)               echo "smoke: log tail:"; tail -5 "$LOG"; exit 20 ;;
esac
