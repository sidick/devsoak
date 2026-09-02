#!/bin/sh
# Produce a canonical, diffable behavioural fingerprint of a trackdisk-
# style driver: a 60 s devsoak classification run (full matrix incl. the
# risky tier), reduced to identity + dialects + sorted pin list.
#
# Usage:
#   ci/fingerprint.sh CONFIG DEVICE UNIT OUTFILE [extra devsoak args...]
#   ci/fingerprint.sh --from-log SERIAL.LOG OUTFILE
#
# e.g.
#   ci/fingerprint.sh test/a1200-scsi.toml scsi.device 0 \
#       fingerprints/scsi.device-47.4-a1200-ide.txt
#   ci/fingerprint.sh cfg/a2091.toml scsi.device 0 \
#       fingerprints/scsi.device-37.64-a2091.txt -k a2091-37-unaligned-length-hang
#
# Extra args go to devsoak after the defaults, so a later -r/-k/-K wins:
# -k ID forces a quirk the run needs to survive (recorded in the
# output), -r 0,1K shrinks the range for a floppy, -K for a pure
# driver-under-test run. COPPERLINE overrides the emulator binary.
# --from-log reduces an existing serial capture instead of running (for
# targets --run staging cannot reach, e.g. Kickstart 1.3 boot floppies).
#
# The fingerprint is only written when the run reached a RESULT verdict;
# a FAIL verdict still fingerprints (the failure is part of it).

set -u

REPO=$(cd "$(dirname "$0")/.." && pwd)

extract() {
    _log=$1; _out=$2
    {
        echo "# devsoak fingerprint (60s classification, -Z)"
        grep 'devsoak: driver '  "$_log" | head -1 | sed 's/devsoak: //'
        grep 'devsoak: idstring' "$_log" | head -1 | sed 's/devsoak: //'
        if grep -q 'devsoak: geometry: sector' "$_log"; then
            grep 'devsoak: geometry: sector' "$_log" | head -1 \
                | sed 's/devsoak: geometry: /geometry: /;s/, total sectors.*//'
        else
            echo "geometry: unavailable"
        fi
        grep 'NSCMD_DEVICEQUERY:' "$_log" | head -1 \
            | sed 's/.*NSCMD_DEVICEQUERY: /nsd: /'
        grep -q 'NSCMD_DEVICEQUERY unsupported' "$_log" && echo "nsd: none"
        printf 'dialects:'
        grep 'dialect enabled:' "$_log" | sed 's/.*enabled: / /' | tr -d '\n'
        echo ""
        grep 'quirks: applied' "$_log" \
            | sed 's/devsoak: quirks: applied /quirk-applied: /'
        grep 'SKIP dialect' "$_log" | sed 's/devsoak: /note: /'
        grep 'devsoak: matrix: pinned' "$_log" \
            | sed 's/devsoak: matrix: pinned /pin /' | sort -u
        grep -E 'devsoak: matrix: FAIL|devsoak: matrix: WARN' "$_log" \
            | sed 's/devsoak: matrix: /finding: /' | sort -u
        grep 'devsoak: matrix: [0-9]' "$_log" | tail -1 | sed 's/devsoak: //'
        grep 'devsoak: RESULT' "$_log" | tail -1 | sed 's/devsoak: //'
    } > "$_out"
    echo "fingerprint: wrote $_out"
}

if [ "$1" = "--from-log" ]; then
    OUT=$3
    case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac
    extract "$2" "$OUT"
    exit 0
fi

# --no-z drops the risky tier and single-threads: floppy trackdisk under
# Copperline can't sustain the -Z formats or concurrent access, but its
# tier 0-2 behaviour still fingerprints cleanly.
ZARG="-Z"; WQ="-w 2 -q 2"
if [ "$1" = "--no-z" ]; then ZARG=""; WQ="-w 1 -q 1"; shift; fi

CONFIG=$1; DEVICE=$2; UNIT=$3; OUT=$4; shift 4
case "$CONFIG" in /*) ;; *) CONFIG="$PWD/$CONFIG" ;; esac
case "$OUT"    in /*) ;; *) OUT="$PWD/$OUT" ;; esac
EMU=${COPPERLINE:-copperline}

WORK=$(mktemp -d)
LOG="$WORK/serial.log"

[ -x "$REPO/devsoak" ] || { echo "fingerprint: build devsoak first (make)"; exit 20; }
cp "$REPO/devsoak" "$REPO/devsoak.quirks" "$WORK/"

# scratch victims: HDF for disk targets, ADF for floppy targets
( cd "$WORK" && xdftool victim.hdf create size=2Mi + format Victim ffs ) \
    >/dev/null || { echo "fingerprint: xdftool failed"; exit 20; }
( cd "$WORK" && xdftool victim.adf create + format Victim ofs ) >/dev/null 2>&1

echo "fingerprint: $DEVICE unit $UNIT via $CONFIG $*"
( cd "$WORK" && timeout 900 "$EMU" --config "$CONFIG" --factory \
    --run ./devsoak \
    --run-args "$DEVICE $UNIT -d -r 512,2K -t 60s $WQ -A 0 -W 90 $ZARG -y -o ser $*" \
    --noaudio --screenshot-after 260 fp.png ) > "$LOG" 2>"$WORK/emu.err"

if ! grep -q 'devsoak: RESULT' "$LOG"; then
    echo "fingerprint: no RESULT line — run crashed or hung; log: $LOG"
    tail -5 "$LOG"
    exit 20
fi

extract "$LOG" "$OUT"
sed -n '2,6p' "$OUT"
