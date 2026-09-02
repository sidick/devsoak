# devsoak

devsoak is a destructive correctness and soak tester for trackdisk-style
AmigaOS block device drivers. One run answers two questions:

1. Does every command the driver accepts do the right thing — including
   the edge cases (bounds, alignment, zero length, the 64-bit offset high
   word, stale ETD change counts, unsupported commands)?
2. Does it keep doing the right thing for hours while several tasks issue
   overlapping reads, writes and housekeeping commands, with multiple
   requests in flight?

It's aimed at driver authors: the primary target is copperhf.device, but
devsoak works against any trackdisk-compatible device — scsi.device,
lide.device, oktagon.device, uaehf.device, trackdisk.device — so you can
compare a driver under test against known-good references.

## Relationship to devtest

devsoak complements [devtest](https://github.com/cdhooper/amiga_devtest),
it doesn't replace it: devtest probes what a driver accepts and sweeps
whole devices, while devsoak works a fixed sector range so it can hold
per-sector state and prove *content* correctness under sustained
concurrent load. It borrows devtest's conventions — 0xA5 result-field
prefill, 0xA5/0x5A double-read diagnostics, offset-high-word-in-io_Actual
for the 64-bit dialects — so findings from either tool read the same way.

## Supported platforms

Runs on Kickstart 1.3 and later, and on AROS m68k. 68000-safe. No ixemul,
no ReadArgs, no utility.library — see [Installation](Installation.md) for
why that matters for the build.

## Destructive by design

devsoak overwrites the range you give it. It refuses to run without both
`-d` and `-r`, prints the device geometry and the range, and asks for
confirmation unless `-y` is given. It never writes outside the range (the
bounds probes included) — but everything inside it is fair game. Never
point it at a range you care about.

## Quick start

Build it:

```
make
```

Run a one-minute characterisation of a driver you don't know yet, in
driver-under-test mode:

```
devsoak lide.device 0 -d -r 512,2K -t 60s -K -y
```

Or run the smoke profile that CI uses on every commit — 30 seconds of
traffic against a scratch range:

```
ci/smoke.sh test/a1200-scsi.toml scsi.device 0 512,2K
```

See [Understanding a Run](Understanding-Runs.md#run-profiles) for what
each profile trades off, and more [worked examples](Understanding-Runs.md#worked-examples).

## What you get

- [Installation](Installation.md) — building with amiga-gcc, what to copy
  to the Amiga, memory sizing, and a Kickstart 1.3 boot floppy recipe.
- [CLI Reference](CLI-Reference.md) — every option, the exit codes, and
  the `RESULT` verdict line CI keys on.
- [Understanding a Run](Understanding-Runs.md) — what happens phase by
  phase, how to read a failure report, pinned behaviours, and run
  profiles with worked examples.
- [The Quirks File](Quirks.md) — recording known-driver behaviour
  separately from the driver under test.
- [Fingerprinting Drivers](Fingerprinting.md) — capturing a driver's
  behavioural identity so a future run can diff against it.
- [Testing under Emulation](Emulator-Testing.md) — the Copperline configs,
  the CI smoke gate, and removable-media testing without real hardware.
