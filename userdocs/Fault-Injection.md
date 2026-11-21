# Fault Injection with Copperline

devsoak proves a driver correct on a medium that behaves. Copperline's
[device-level fault injection](https://github.com/CopperlineHQ/Copperline/blob/main/docs/debugger/disk-fault.md)
(`disk.fault.*`, Copperline 1.1 and later) makes the medium misbehave on
purpose: dropped, torn and stale writes, corrupted reads, media errors,
a simulated power cut. Running the two together answers questions a
clean run can't:

- **Does the driver report a medium error?** An injected media error
  must come back to devsoak as a failed request, not as a success that
  the content model later catches as corruption.
- **Would devsoak notice?** Each fault below has a known, expected
  failure report. Seeing it once is a rehearsal for reading the real
  thing, and a check that the range, duration and worker count you soak
  with are enough to catch that class of fault.

The faults are seeded and keyed on the device's request sequence, so a
run with a fixed devsoak seed (`-s`) and a fixed fault seed repeats
exactly.

A fault run is *supposed* to end in `RESULT FAIL`. Don't wire one into
CI with `ci/smoke.sh`'s exit-code mapping unless you invert it.

## Running devsoak under a fault policy

Fault policies are installed over the Copperline Control Protocol, so
the emulator runs headless under `--control` instead of free-running as
it does for `ci/smoke.sh`. Two things follow from that:

- The machine starts paused and only runs while a client has a
  `continue` outstanding. One-shot `copperline-ctl` calls can't keep it
  running; a `--repl` client has to stay attached for the whole run.
- `--run` arms a break at program load, so the first `continue` stops
  there and a second one starts devsoak.

From a directory holding `devsoak`, `devsoak.quirks` and a config from
`test/`:

```
xdftool victim.hdf create size=2Mi + format Victim ffs

copperline --config a1200-lide.toml --factory --noaudio \
    --run ./devsoak \
    --run-args "lide.device 0 -d -r 512,2K -s 12345 -t 30s -w 2 -q 2 -A 0 -W 30 -y -o ser" \
    --control :0 --control-info ccp.json > serial.log 2> emu.err &
until [ -s ccp.json ]; do sleep 1; done

TARGET='{"kind":"ide","unit":0}'
RULES='[{"op":"write","after":{"writes":160},"times":5,"action":"drop"}]'

printf '%s\n' \
    "disk.fault.set {\"target\":$TARGET,\"seed\":1,\"policy\":{\"rules\":$RULES}}" \
    continue \
    continue |
    copperline-ctl --info ccp.json --repl > ctl.log &
CTL=$!

until grep -q 'devsoak: RESULT' serial.log; do sleep 1; done
kill $CTL

copperline-ctl --info ccp.json disk.fault.stats "{\"target\":$TARGET}"
copperline-ctl --info ccp.json shutdown
grep 'devsoak: RESULT' serial.log
```

The REPL client blocks in the second `continue` until it is killed;
that is what keeps the machine running. Once it has gone, the machine is
paused and one-shot calls work again. `disk.fault.stats` then shows what
was actually injected, which is worth checking before reading anything
into a verdict: a `PASS` with `"dropped":0` only means the rule never
fired.

## Choosing the target

The target names the emulated controller, not the Amiga driver:

| devsoak device | config | target |
|---|---|---|
| scsi.device (Gayle IDE) | `a1200-scsi.toml` | `{"kind":"ide","unit":0}` |
| lide.device (Zorro IDE board) | `a1200-lide.toml` | `{"kind":"ide","unit":0}` |
| scsi.device (A2091 and other SCSI) | `a1200-a2091.toml` | `{"kind":"scsi","unit":0}` |
| copperhf.device | a `[copperhf]` config | `{"kind":"copperhf","unit":0}` |
| trackdisk.device | `a1200-floppy.toml` | `{"kind":"floppy","drive":0}` |

Only the `ide` target on lide.device has been exercised for this page.
The others are listed from the Copperline documentation, which also
describes the floppy target's own `lba` numbering and its plain-ADF
restriction.

## Placing the fault

A rule without a gate fires on the first matching request, which is the
boot-time mount or devsoak's fill: the run fails before the soak has
started. Use `after` to let the fill and initial audit through first.

To find the numbers for your own range and config, do one clean run
(`RULES='[]'`) and stop it after the initial audit, or read the totals
at the end and work back. For the command above the fill and initial
audit were over by about 130 writes and 1800 reads, and the whole run
made about 250 writes and 7800 reads, so `"after":{"writes":160}` and
`"after":{"reads":2500}` both land early in the soak.

`lba` narrows a rule to part of the medium. It is an absolute sector
number on the device, the same numbering as devsoak's `-r` and its
failure reports.

## What each fault looks like

Observed on lide.device 40.12 with the command above, each rule gated
into the soak as described and limited with `times`:

| rule | devsoak report |
|---|---|
| `"op":"read","action":"corrupt"` | `DATA MISMATCH ... class payload mismatch (partial transfer)`; the header is intact and names the right sector and generation |
| `"op":"write","action":"drop"`, `times` 5 | `DATA MISMATCH ... class stale generation (lost write)`, from a worker's read-verify and again from the final audit |
| `"op":"write","action":"tear","at":"sector","n":1` | `stale generation (lost write)` at the final audit |
| `"op":"write","action":"stale"` | `stale generation (lost write)` at the final audit |
| `"op":"read","action":"error","code":"media"` | `read FAILED cmd ... err -6 actual <full length>` |
| `"op":"write","action":"error","code":"media"` | `write FAILED cmd ... err -6 actual <full length>` |
| `"action":"cut"` | every request from then on `FAILED ... err -6`, final audit included |

All of them ended in `RESULT FAIL`. See
[Reading a failure report](Understanding-Runs.md#reading-a-failure-report)
for the mismatch classes.

Two things in that table are about the driver rather than the fault:

- lide.device reports an ATA error as `err -6` (`IOERR_UNITBUSY`) and
  leaves `io_Actual` at the full request length. A driver that returned
  success here would show up as a `DATA MISMATCH` with no `FAILED` line
  before it, which is the case this whole exercise exists to catch.
- A `write FAILED` is followed later by a `stale generation` mismatch on
  the same sectors. That is the failed write itself, seen again by the
  audit, not a second fault.

## When a fault goes unnoticed

A single dropped write (`times` 1) passed, twice, with identical request
counts both times and `"dropped":1` in the stats. The same rule with
`times` 5 failed. The likely explanation is that devsoak rewrote those
sectors before anything read them back: a later write over the same
sectors makes a lost write unobservable, to devsoak and to any other
reader. That is a property of the workload, not a detection bug, but it
means one injected fault is not a reliable test. Give the rule a `times`
of several, or a `probability`, and check the stats.

`reorder` and `ignore` hold writes in a simulated cache that reads still
see, so on their own they are invisible to devsoak; they only matter
combined with `cut`. devsoak has no power-loss model (it cannot resume a
run and audit what survived), and those combinations have not been tried
here. `delay` currently adds real latency only on `scsi` targets.
