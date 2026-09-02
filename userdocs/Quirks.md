# The Quirks File

`devsoak.quirks` is a plain-text database of known driver behaviours,
kept separate from the driver under test (§16 of the design brief). It
exists so that a genuinely known-good driver's legitimate oddities don't
drown out real findings when you're comparing it against something else
— but a crash or hang on the driver *actually under test* is a finding,
not a quirk. That's the reasoning behind `-K`: it ignores the quirks file
entirely, so nothing softens what a driver-under-test run reports.

## File format

Each entry names match conditions and actions:

```
quirk   scsi.device-cbm-no-nsd
match   name=scsi.device version<40
because Commodore scsi.device before V40 predates NSD; TD64 may be
        dispatched through an unchecked jump table.
action  skip TD_READ64 TD_WRITE64 NSCMD_*
action  expect NSCMD_DEVICEQUERY IOERR_NOCMD
status  confirmed
```

`#` comments and blank lines are ignored.

### Match keys

- `name=STR` — case-insensitive exact match on the driver's `ln_Name`.
- `version</<=/>/>=/= N` and `revision</<=/>/>=/= N` — the driver's
  `lib_Version`/`lib_Revision`.
- `id~STR` — case-insensitive substring of `lib_IdString`.
- `kick</<=/>/>=/= N` — the running Kickstart's exec.library
  `lib_Version`.

All conditions across all `match` lines in an entry must hold (AND). Any
other key — a SCSI INQUIRY string, an AttnFlags bit, a specific expansion
board — isn't evaluable by this parser: an entry using one is flagged
unmatchable at load time and printed as a warning, and can then only ever
be applied via `-k`.

### Actions

- `skip CMD...` — skip named commands/tests; a trailing `*` wildcards a
  prefix, e.g. `NSCMD_*`.
- `expect CMD ERROR` — an expected error counts as the correct result.
- `warn TEST` — downgrade a named matrix test to a warning.
- `skiptest TEST` — skip a named matrix test outright.
- `align N` — require N-byte buffer alignment.
- `nochip` — the driver doesn't need chip-RAM buffers.
- `chipbuffers` — force chip-RAM buffers (see the floppy example below).
- `maxinflight N` — cap outstanding requests.
- `maxxfer BYTES` — cap transfer size (decimal or `0x` hex).
- `norandomcmd` — don't probe with random command numbers.
- `tier N` — reclassify a test's risk tier.

## Status policy

Only `status confirmed` entries apply automatically against a reference
driver. `reported` entries (seen in the wild, not reproduced by this
project) and `suspected` entries (a guess, or a first crash devsoak
itself hit) are inert until named with `-k ID` on the command line. `-K`
ignores the file entirely.

Nothing may be promoted to `confirmed` on the strength of a document
alone — only after it has actually been reproduced with devsoak against
real hardware or a real emulator build. To promote an entry, reproduce it
and record the driver version and log in the `because` text.

## Skip reporting

Every skip is reported and counted; nothing silently vanishes from the
run's output because a quirk suppressed it.

## Surviving a crash

With `-P FILE`, every tier-2/3 command is appended to FILE and flushed
*before* its first issue, so after a reboot the last line names the
culprit:

```
devsoak DEVICE UNIT -P FILE --resume
```

reads the file, reports the suspect, and prints a ready-to-paste
`status suspected` quirk entry for it. With `-o ser`, the same
breadcrumbs went over serial too, which in an emulator survives a guest
reset — so a serial capture alone can name the culprit even without a
`-P` file surviving the crash.

## Worked example: `floppy-chip-buffers`

trackdisk.device's floppy DMA goes through Paula, which can only address
chip RAM. devsoak's default `MEMF_PUBLIC` buffers may land in fast RAM,
where a read completes having transferred nothing and verify sees the
untouched buffer. On Kickstart 2.0+ the driver's TD_GETGEOMETRY
`dg_BufMemType` already advertises `MEMF_CHIP` and devsoak honours it
automatically; Kickstart 1.3 trackdisk predates TD_GETGEOMETRY, so this
quirk forces it:

```
quirk   floppy-chip-buffers
match   name=trackdisk.device
because Floppy trackdisk.device DMA goes through Paula, which can only
        address chip RAM. devsoak's default MEMF_PUBLIC buffers may land
        in fast RAM, where a read completes with no data transferred
        (verify sees the untouched buffer). On Kickstart 2.0+ the
        driver's TD_GETGEOMETRY dg_BufMemType already advertises
        MEMF_CHIP and devsoak honours it automatically; Kickstart 1.3
        trackdisk predates TD_GETGEOMETRY, so force it there with
        -k floppy-chip-buffers. Applies to the internal drive and any
        external floppy on trackdisk.device.
action  chipbuffers
status  confirmed
```

It's the one entry in `devsoak.quirks` at `status confirmed` — reproduced
directly rather than carried over from another project's report. Use it
with:

```
devsoak trackdisk.device 0 -d -r 0,220 -k floppy-chip-buffers ...
```

on Kickstart 1.3, or omit `-k` on 2.0+ where the driver advertises its
own buffer memory type.
