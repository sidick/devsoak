# Understanding a Run

## What a run does

1. **Fill**: writes generation 1 over the whole range. Every sector
   devsoak writes carries a 32-byte header (magic `DSOK`, run seed,
   64-bit LBA, per-sector generation count, writer id, transfer length,
   checksum) and an xorshift32 payload — everything is recomputable from
   (sector, generation), so verification is stateless.
2. **Initial audit**: sequential read-verify of the range.
3. Optional **`-X` HD_SCSICMD phase** (INQUIRY, TEST UNIT READY, READ
   CAPACITY vs TD_GETGEOMETRY, READ(10) vs CMD_READ byte-compare,
   unsupported-opcode autosense) and **`-R` removable phase** (three
   TD_ADDCHANGEINT handlers, an eject/insert cycle, stale-ETD checks,
   TD_REMCHANGEINT delivery-stop proof; the range is refilled after).
4. **Soak**: N worker tasks each keep `q` requests in flight — 45%
   writes, 45% read-verifies, 10% housekeeping — across every command
   dialect the driver was probed to support (CMD, ETD, TD64, NSD64,
   NSD-ETD64), buffer alignment variants (long/word/odd/4 KB-crossing),
   and an AbortIO probe roughly every thousand ops. A stripe-semaphore
   scheme makes overlapping traffic a defined last-writer-wins model. An
   auditor task re-sweeps the range every `-A` minutes, and the invariant
   task runs the edge-case matrix continuously (see below). Main prints a
   status line every 10 s: ops/s, MB/s, in-flight count, per-class
   p50/p99 latency, error count.
5. **Final audit**, verdict, pinned-behaviour summary.

## The invariant matrix

A dedicated task runs named edge-case tests every few seconds for the
whole run, ordered by risk tier (tier 3 only under `-Z`), announcing each
tier-2/3 command on the serial sink before its first issue. It covers
bounds tests (last-sector, past-end, straddle with io_Actual clamping),
zero and unaligned lengths/offsets, over-MaxTransfer, the
NSCMD_DEVICEQUERY listed-vs-implemented cross-check, an undersized-query
overrun check, stale ETD counts, 64-bit high-word garbage, CMD_STOP/START
gating, TD_MOTOR, lifecycle open/close probes, and — with `-B` — the
4 GB straddle and 32-bit boundary-read tests.

Bounds probes at the device end auto-select a 64-bit dialect when the
offset needs one (a 32-bit probe at a >4 GB offset would truncate and
false-fail), and skip when the driver has no 64-bit dialect at all.

## Reading a failure report

A data mismatch prints, in order: the failing sector and its **class** —
each class maps to a driver bug family:

| class | likely cause |
|---|---|
| header corrupt (not devsoak data) | data from outside the run landed here |
| wrong sector (offset bug) | offset arithmetic, ignored high word, off-by-one |
| stale generation (lost write) | lost write, reordering, cache not written back |
| payload mismatch (partial transfer) | partial DMA, MaxTransfer truncation, byte lanes |

Then the sector's decoded header claim (LBA/generation/writer/xfer_len)
versus what was expected, the first 16 differing bytes as
expected/read1/read2 — the buffer is prefilled 0xA5 before the first read
and 0x5A before the diagnostic re-read, so "read1==A5, read2==5A" means
the driver never touched the buffer — and a dump of the op ring buffer
(the last 4096 ops with timestamps, worker ids, commands, offsets and
results); with several workers the interleaving is usually the bug.
Guard bytes (64 × 0xCC around every buffer) catch overruns and name the
offending command.

The affected worker stops; the others continue unless `-e` was given.

## Pinned behaviours

Where known-good drivers legitimately disagree, devsoak does not
hard-fail: it **pins** the first observed behaviour and fails only if the
behaviour *changes* during the run. Pinned values are printed live and
summarised at the end. Observed so far:

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

\* lide.device 40.12 reads stale `io_Actual` as an offset high word for
TD_FORMAT, and advertises ETD_FORMAT without dispatching it — both
diagnosed to source and reported upstream; see
[the Quirks File](Quirks.md) (`lide-format-etd-nocmd`).

Zero-length probes hard-fail only when bytes beyond the claimed
io_Actual are touched. Two drivers can both be "correct" and pin
differently — that's what the [quirks file](Quirks.md) records, and what
[fingerprints](Fingerprinting.md) capture per driver version.

## Run profiles

- **Smoke** — `-t 30s -y`: fill, 30 s of traffic, audit. CI on every
  commit.
- **Soak** — `-t 8h -w 6 -q 8 -A 15 -W 60`: overnight.
- **Bisect** — `-w 1 -q 1 -s SEED`: single request in flight, one PRNG —
  the op sequence is reproducible from the seed.
- **Big** — `-B` with `-r` placed across the 4 GB boundary.
- **Removable** — `-R`, optionally `-H "cmd"` to script eject/insert.
- **SCSI** — `-X` for drivers implementing HD_SCSICMD.

## Worked examples

All examples assume a scratch range — devsoak destroys it. Sector 0 of a
partitioned disk holds the RDB; put `-r` somewhere expendable.

**Characterise an unknown driver in a minute.** The quickest way to learn
what a driver actually does — which dialects it speaks, how it answers
the edge cases — is a short run in driver-under-test mode and a read of
the pin lines:

```
devsoak lide.device 0 -d -r 512,2K -t 60s -K -y
```

The `matrix: pinned ...` lines are the driver's behavioural fingerprint
(zero-length handling, unaligned offsets, error codes for past-end and
bad units, IOF_QUICK, motor semantics...). Add `-Z` to also fingerprint
the risky tier (FORMAT variants, random command numbers, unadvertised
64-bit dialects).

**Overnight soak** — the actual endurance question. Size the watchdog for
the queue depth (see [CLI Reference](CLI-Reference.md#watchdog-sizing-w))
and let the auditor sweep periodically:

```
devsoak copperhf.device 0 -d -r 512,120K -t 8h -w 4 -q 6 -A 15 -W 60 -X -y
```

**Reproduce and bisect a failure.** A soak failure prints the run's seed.
Replay the identical op sequence with one request in flight:

```
devsoak copperhf.device 0 -d -r 512,120K -w 1 -q 1 -s 1535717554 -y
```

`-w 1 -q 1` is strictly sequential and deterministic from the seed, so
the op ring of two runs matches byte for byte — then shrink `-t`/`-r`
until the failure is minutes away instead of hours.

**Hunt a crash in a fragile driver.** Tiered ordering plus breadcrumbs
means a lockup names its own culprit:

```
devsoak old.device 0 -d -r 2K,8K -t 5m -K -Z -P RAM:crumbs -o both -y
```

Every tier-2/3 command is announced on serial and appended to the `-P`
file *before* its first issue. After the reboot:

```
devsoak old.device 0 -P RAM:crumbs --resume
```

prints the last breadcrumb and a ready-to-paste `status suspected` quirks
entry for the command that was in flight. See
[Surviving a crash](Quirks.md#surviving-a-crash) for more.

**Big device / 4 GB boundary.** Place the range across the boundary
(sector 8388608 at 512-byte sectors) so straddling transfers are real
writes:

```
devsoak lide.device 0 -d -r 8388096,1K -t 60s -B -y
```

Catches ignored offset high words (the classic ">4 GB wraps to a low LBA"
corruption) via both the boundary tests and the content model: a wrapped
write lands with the wrong LBA in its sector header.
