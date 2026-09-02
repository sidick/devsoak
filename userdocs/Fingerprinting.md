# Fingerprinting Drivers

A fingerprint is a canonical, diffable behavioural fingerprint of a
driver: its identity, its command dialects, and its sorted pin list from
a classification run. Pins are *behaviour* — two correct drivers can
legitimately differ, which is exactly what [the quirks
file](Quirks.md) exists to record. What a fingerprint buys you is a
baseline: a changed pin on a later run against the same driver version is
a behavioural regression, something the plain pass/fail verdict alone
would miss.

## Using `ci/fingerprint.sh`

Normal use runs a 60 s classification (full matrix, including the risky
tier) under Copperline and reduces the serial log to identity + dialects
+ sorted pins:

```
ci/fingerprint.sh test/a1200-scsi.toml scsi.device 0 \
    fingerprints/scsi.device-47.4-a1200-ide.txt
```

Extra arguments after the four positional ones go to devsoak after its
defaults, so a later `-r`/`-k`/`-K` wins:

```
ci/fingerprint.sh cfg/a2091.toml scsi.device 0 \
    fingerprints/scsi.device-37.64-a2091.txt -k a2091-37-unaligned-length-hang
```

`-k ID` forces a quirk the run needs to survive (and it's recorded in the
output); `-r 0,1K` shrinks the range for a floppy; `-K` runs pure
driver-under-test. `COPPERLINE` overrides the emulator binary used.

A fingerprint is only written when the run reached a RESULT verdict; a
FAIL verdict still fingerprints — the failure is part of the identity.

### `--from-log`

Reduces an existing serial capture instead of running the emulator — for
targets the `--run` staging can't reach, e.g. Kickstart 1.3 boot floppies:

```
ci/fingerprint.sh --from-log serial.log fingerprints/trackdisk.device-34.1-ks13.txt
```

### `--no-z`

Drops the risky tier and single-threads (`-w 1 -q 1` instead of
`-w 2 -q 2`, no `-Z`), for floppy trackdisk which can't sustain the
tier-3 formats or concurrent access under emulation — its tier 0–2
behaviour still fingerprints cleanly:

```
ci/fingerprint.sh --no-z test/a1200-floppy.toml trackdisk.device 0 \
    fingerprints/trackdisk.device-47.14-ks32.txt -r 0,220
```

## The catalogue

`fingerprints/` holds one file per (driver, version, controller).
`fingerprints/INDEX.md` tabulates them all — driver, version, PASS/FAIL
result, and identity string — and shows drift across OS versions. The
worked example there is scsi.device's pins moving across Kickstart
releases:

| pin | V37/a2091 | V45/a3000 | V47/ide | V47/a3000 |
|---|---|---|---|---|
| past-end-err | — | 20 | 3 | 20 |
| unaligned-len-err | — | -4 | -4 | -4 |
| drivetype-err | -1 | -3 | -3 | -3 |
| bad-unit-err | 50 | 50 | 50 | 50 |
| flush-err | 0 | 0 | 0 | 0 |

(`—` means the test wasn't reached — V37's scsi.device predates the
dialects the test needs.) V37's generic `-1` errors give way to V45/V47's
specific codes, and TD_GETGEOMETRY/NSD arrive at V36/V40 — visible here
as pins that simply don't exist on the oldest driver.

## When to commit a fingerprint

Commit a fingerprint when a driver's behaviour is expected to be stable,
and let a future run diff against it. A changed pin is a behavioural
regression the pass/fail verdict alone would miss — the whole point of
capturing dialects and pins as data rather than only a PASS/FAIL line.
