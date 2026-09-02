# devsoak

A destructive correctness and soak tester for trackdisk-style AmigaOS block
device drivers. One run answers two questions:

1. Does every command the driver accepts do the right thing — including the
   edge cases (bounds, alignment, zero length, the 64-bit offset high word,
   stale ETD change counts, unsupported commands)?
2. Does it keep doing the right thing for hours while several tasks issue
   overlapping reads, writes and housekeeping commands, with multiple
   requests in flight?

Primary targets are copperhf.device and the MIRAGE ROM driver, but devsoak
works against any trackdisk-compatible device (scsi.device, lide.device,
uaehf.device, trackdisk.device) so results can be compared against
known-good drivers.

devsoak complements [devtest](https://github.com/cdhooper/amiga_devtest):
devtest probes what a driver accepts and sweeps whole devices; devsoak works
a fixed sector range so it can hold per-sector state and prove *content*
correctness under sustained concurrent load. It borrows devtest's
conventions (0xA5 result-field prefill, 0xA5/0x5A double-read diagnostics,
offset-high-word-in-io_Actual for the 64-bit dialects).

Runs on Kickstart 1.3 and later, and on AROS m68k. 68000-safe. No ixemul,
no ReadArgs, no utility.library.

## Building

Needs bebbo's amiga-gcc. With `m68k-amigaos-gcc` on the PATH, `make` builds
directly; otherwise `make` runs the build inside the
`stefanreinauer/amiga-gcc:gcc-v16.1` docker image. The result is a single
static `devsoak` binary.

The build uses `-mcrt=nix13`, not plain `-noixemul`: the default libnix
variant implements even 32-bit multiply/divide via utility.library
(V36+), which makes the binary refuse to start on Kickstart 1.3
("utility.library failed to load"). `src/soft64.c` additionally provides
pure-software 64-bit helpers so no U64 arithmetic can reintroduce the
dependency. If you change the toolchain flags, re-check with
`m68k-amigaos-nm devsoak | grep -i utility` — it must print nothing.

## Usage

```
devsoak DEVICE UNIT -d -r START,LEN [options]

  -d            destructive (required)
  -r START,LEN  test range in sectors (required); K/M/G suffixes multiply
                by 1024 (sector counts, not bytes); 0x hex accepted
  -t DURATION   e.g. 30s, 20m, 8h (default 60s)
  -w N          worker tasks (default 4, max 8)
  -q N          outstanding requests per worker (default 4, max 8)
  -S N          stripe size in sectors (default 256)
  -A MIN        full audit interval in minutes (default 10, 0 = start/end only)
  -M BYTES      MaxTransfer (default 0x1FE00); set to the driver's real limit
  -m ADDR       extra buffer memory region (hex address), may repeat
  -s SEED       PRNG seed (default: from clock, printed)
  -e            stop on first error
  -y            skip the destructive-run confirmation
  -B            big device: include the 4 GB boundary tests
  -R            removable media semantics (change-interrupt phase)
  -H CMD        shell command run to trigger eject/insert (with -R)
  -X            include the HD_SCSICMD tests
  -W SEC        watchdog timeout (default 5; see sizing note below)
  -o con|ser|both  output sink (default con)
  -Q FILE       quirks file (default: devsoak.quirks in the current
                directory, then PROGDIR:)
  -k ID[,ID]    force these quirk ids on even if not status confirmed
  -K            ignore the quirks file entirely (driver-under-test mode)
  -Z            include the "risky" tier-3 tests (random command numbers,
                unadvertised 64-bit dialects, TD_FORMAT variants)
  -P FILE       crash-breadcrumb file (see "Surviving a crash")
  --resume      after a crash: report the suspect from -P FILE (needs the
                DEVICE/UNIT arguments too, for the suggested quirk block)
  -v            verbose
```

devsoak refuses to run without both `-d` and `-r`, prints the device
geometry and the range, and asks for confirmation unless `-y` is given.
It never writes outside the range (the §8 bounds probes included).

### Exit codes

| code | meaning |
|------|---------|
| 0    | clean |
| 5    | warnings only (quirk-downgraded findings, or a run cut short) |
| 10   | data or behaviour error (or watchdog hang) |
| 20   | fatal: bad arguments, cannot open device, allocation failure |

Because a guest exit code doesn't reach the host through an emulator's
serial log, the last thing devsoak prints is a machine-greppable verdict:
`devsoak: RESULT PASS`, `devsoak: RESULT WARN ... rc=5` or
`devsoak: RESULT FAIL rc=10`. CI should key on that line.

### Watchdog sizing

`-W` flags any request outstanding longer than N seconds as a hang. The
default (5 s) suits real hardware at low queue depths. The *oldest* request
legitimately waits about `w × q × MaxTransfer / device-speed` seconds under
full load — 16 in-flight 127 KB requests on a ~1 MB/s PIO device queue past
5 s while perfectly healthy. Size `-W` above that product (e.g. `-W 30`
for `-w 4 -q 4` on slow IDE).

## Run profiles

- **Smoke** — `-t 30s -y`: fill, 30 s of traffic, audit. CI on every commit.
- **Soak** — `-t 8h -w 6 -q 8 -A 15 -W 60`: overnight.
- **Bisect** — `-w 1 -q 1 -s SEED`: single request in flight, one PRNG —
  the op sequence is reproducible from the seed.
- **Big** — `-B` with `-r` placed across the 4 GB boundary.
- **Removable** — `-R`, optionally `-H "cmd"` to script eject/insert.
- **SCSI** — `-X` for drivers implementing HD_SCSICMD.

Range sizing: 16–64 MB on a machine with ≥ 8 MB fast RAM; the generation
table costs 2 bytes/sector and each worker slot buffers MaxTransfer bytes.
A 1 MB machine runs `-w 2 -q 2 -r ...,8M -M 0x8000`. Placing the range at
the end of the device puts the traffic and the bounds tests in the same
neighbourhood.

## What a run does

1. **Fill**: writes generation 1 over the whole range. Every sector devsoak
   writes carries a 32-byte header (magic `DSOK`, run seed, 64-bit LBA,
   per-sector generation count, writer id, transfer length, checksum) and
   an xorshift32 payload — everything is recomputable from (sector,
   generation), so verification is stateless.
2. **Initial audit**: sequential read-verify of the range.
3. Optional **-X HD_SCSICMD phase** (INQUIRY, TEST UNIT READY,
   READ CAPACITY vs TD_GETGEOMETRY, READ(10) vs CMD_READ byte-compare,
   unsupported-opcode autosense) and **-R removable phase** (three
   TD_ADDCHANGEINT handlers, an eject/insert cycle, stale-ETD checks,
   TD_REMCHANGEINT delivery-stop proof; the range is refilled after).
4. **Soak**: N worker tasks each keep q requests in flight — 45 % writes,
   45 % read-verifies, 10 % housekeeping — across every command dialect
   the driver was probed to support (CMD, ETD, TD64, NSD64, NSD-ETD64),
   buffer alignment variants (long/word/odd/4 KB-crossing), and an AbortIO
   probe roughly every thousand ops. A stripe-semaphore scheme makes
   overlapping traffic a defined last-writer-wins model. An auditor task
   re-sweeps the range every `-A` minutes, and the invariant task runs the
   edge-case matrix continuously (see below). Main prints a status line
   every 10 s: ops/s, MB/s, in-flight count, per-class p50/p99 latency,
   error count.
5. **Final audit**, verdict, pinned-behaviour summary.

## The invariant matrix

A dedicated task runs named edge-case tests every few seconds for the whole
run, ordered by risk tier (tier 3 only under `-Z`), announcing each tier-2/3
command on the serial sink before its first issue. Bounds tests
(last-sector, past-end, straddle with io_Actual clamping), zero and
unaligned lengths/offsets, over-MaxTransfer, the NSCMD_DEVICEQUERY
listed-vs-implemented cross-check, an undersized-query overrun check, stale
ETD counts, 64-bit high-word garbage, CMD_STOP/START gating, TD_MOTOR,
lifecycle open/close probes, and with `-B` the 4 GB straddle and 32-bit
boundary-read tests.

### Pinned behaviours

Where known-good drivers legitimately disagree, devsoak does not hard-fail:
it **pins** the first observed behaviour and fails only if the behaviour
*changes* during the run. Pinned values are printed live and summarised at
the end. Observed so far:

| behaviour | scsi.device 47.4 | lide.device 40.12 | trackdisk.device 47.14 |
|---|---|---|---|
| zero-length read | success, actual 0 | IOERR_BADLENGTH | success, reads a whole sector (actual 512)! |
| non-sector-multiple length (read) | IOERR_BADLENGTH | serviced | — |
| unaligned io_Offset (read) | serviced | serviced | — |
| TD_MOTOR io_Actual | always 0 (no prev state) | always 0 | previous state |
| ETD_ dialect | IOERR_NOCMD | works | works (native) |
| TD_RAWREAD/RAWWRITE | IOERR_NOCMD | IOERR_NOCMD | native |
| plain TD_FORMAT (whole track) | works | IOERR_BADADDRESS* | works |
| bad-unit OpenDevice error | 50 | TDERR_BadUnitNum (32) | TDERR_BadUnitNum |
| IOF_QUICK | never honoured | never honoured | never honoured |

Zero-length probes therefore hard-fail only when bytes beyond the claimed
io_Actual are touched. Bounds probes at the device end auto-select a
64-bit dialect when the offset needs one (a 32-bit probe at a >4 GB
offset would truncate and false-fail — thanks to LIV2 for flagging the
same hazard independently), and skip when the driver has no 64-bit
dialect at all.

Driver-caching notes learned the hard way: trackdisk.device serves
repeat reads of the same track from its RAM track buffer without
touching the hardware (so an ejected disk keeps "reading" — the -R
poll alternates tracks to defeat this), and TD_REMCHANGEINT must be
issued on the same still-pending IORequest that carried
TD_ADDCHANGEINT, per the V47 autodoc — any other request wedges the
driver.

\* lide.device 40.12 reads stale `io_Actual` as an offset high word for
TD_FORMAT, and advertises ETD_FORMAT without dispatching it — both
diagnosed to source and reported upstream; see `devsoak.quirks`
(`lide-format-etd-nocmd`).

## Reading a failure report

A data mismatch prints, in order: the failing sector and its **class** —
each class maps to a driver bug family:

| class | likely cause |
|---|---|
| header corrupt (not devsoak data) | data from outside the run landed here |
| wrong sector (offset bug) | offset arithmetic, ignored high word, off-by-one |
| stale generation (lost write) | lost write, reordering, cache not written back |
| payload mismatch (partial transfer) | partial DMA, MaxTransfer truncation, byte lanes |

then the sector's decoded header claim (LBA/generation/writer/xfer_len)
versus what was expected, the first 16 differing bytes as
expected/read1/read2 (the buffer is prefilled 0xA5 before the first read
and 0x5A before the diagnostic re-read, so "read1==A5, read2==5A"
means the driver never touched the buffer), and a dump of the op ring
buffer (the last 4096 ops with timestamps, worker ids, commands, offsets
and results) — with several workers the interleaving is usually the bug.
Guard bytes (64 × 0xCC around every buffer) catch overruns and name the
offending command.

The affected worker stops; the others continue unless `-e` was given.

## Output sinks

Everything goes through one output layer. `-o con` writes to the CLI.
`-o ser` emits every character through the ROM debug serial port
(exec RawPutChar) — unbuffered, so a machine that crashes on the next
command has already emitted the line; CR LF endings for terminal/log
capture. `-o both` does both. Nothing else may have serial.device open
during a serial run (RawPutChar drives the same hardware), and the baud
rate is whatever the ROM/Prefs left it at (typically 9600 on 1.3).
Emulators (Copperline, WinUAE) can log the serial port to a host file —
that's the primary CI logging path.

## The quirks file

`devsoak.quirks` is a plain-text database of known driver behaviours,
separate from the driver under test (§16 of the design brief). Each entry
names match conditions (device name, version/revision comparisons,
IdString substring, Kickstart version) and actions:

```
quirk   scsi.device-cbm-no-nsd
match   name=scsi.device version<40
because Commodore scsi.device before V40 predates NSD; TD64 may be
        dispatched through an unchecked jump table.
action  skip TD_READ64 TD_WRITE64 NSCMD_*
action  expect NSCMD_DEVICEQUERY IOERR_NOCMD
status  confirmed
```

Actions: `skip CMD…` (trailing-`*` prefix wildcards), `expect CMD ERROR`
(an expected error counts as the correct result), `warn TEST` /
`skiptest TEST` (downgrade or skip a named matrix test), `align N`,
`nochip`, `maxinflight N`, `maxxfer BYTES`, `norandomcmd`, `tier N`.

Only `status confirmed` entries apply automatically; `reported` and
`suspected` entries print a note and need `-k ID` to force. `-K` ignores
the file entirely — a crash or hang on the driver *under test* is a
finding, not a quirk. Every skip is reported and counted; nothing
silently vanishes. To promote an entry to `confirmed`, reproduce it with
devsoak on the actual hardware and record the driver version and log in
the `because` text.

## Surviving a crash

With `-P FILE`, every tier-2/3 command is appended to FILE and flushed
*before* its first issue, so after a reboot the last line names the
culprit. `devsoak DEVICE UNIT -P FILE --resume` reads the file, reports
the suspect and prints a ready-to-paste `status suspected` quirk entry
for it. (With `-o ser`, the same breadcrumbs went over serial, which in
an emulator survives a guest reset.)

## Test configurations

`test/` holds Copperline configs used for development and CI:
`a1200-scsi.toml` (Gayle IDE → scsi.device 47), `a1200-lide.toml`
(LIDE/RIPPLE board → lide.device, full TD64/NSD), `a1200-floppy.toml`
(df0 → trackdisk.device, for `-R`; eject/insert driven over the
Copperline Control Protocol). Each expects a scratch `victim.hdf`
(`xdftool victim.hdf create size=2Mi + format Victim ffs`) or
`blank.adf` next to the binary. Note that Copperline's synthesized RDB
occupies the first cylinder of a bare hardfile, so place `-r` past it
(e.g. `-r 512,2K` on the 2 MB victim).

`ci/smoke.sh` is the CI entry point: it builds a fresh scratch victim,
boots the named config, runs the 30-second profile and maps devsoak's
final `RESULT` line to its exit code (PASS 0 / WARN 5 / FAIL 10 / no
verdict at all 20 — the last meaning the guest crashed or hung, with
the §16.4 breadcrumb naming the suspect at the end of the serial log).

For Kickstart 1.3 there is no `--run` staging (it needs 2.0+ shell
built-ins): build a minimal bootable OFS floppy instead — devsoak, the
quirks file, and a one-line `s/startup-sequence` invoking it — e.g.
`xdftool boot.adf create + format Boot ofs + boot install + write
devsoak + write devsoak.quirks + makedir s + write ss.txt
s/startup-sequence`.

After devsoak is clean, layer real clients on top: mount FFS/PFS3/SFS
partitions on the driver and run filesystem-level stress (e.g.
FileSystemStressTest from Aminet) while devsoak works a separate range of
the same unit — filesystems pick different command dialects and catch
"works with everything except PFS3" bugs.
