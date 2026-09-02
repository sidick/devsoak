# Testing under Emulation

`test/` holds Copperline configs used for development and CI. Each
config expects a scratch victim image next to the binary in the
Copperline working directory.

## Test configurations

- **`a1200-scsi.toml`** — A1200 + KS3.2, victim HDF on Gayle IDE
  (scsi.device unit 0), serial to host stdout.
- **`a1200-lide.toml`** — A1200 + KS3.2, victim HDF on Copperline's
  built-in LIDE (RIPPLE) Zorro II IDE board → lide.device, the modern
  reference driver with full TD64/NSD support.
- **`a1200-floppy.toml`** — A1200 + KS3.2, scratch ADF in df0
  (trackdisk.device unit 0, genuinely removable), for `-R`; eject/insert
  driven over the Copperline Control Protocol.

Generate the disk-target victim with:

```
xdftool victim.hdf create size=2Mi + format Victim ffs
```

or for the floppy target:

```
xdftool blank.adf create + format Scratch ofs
```

Copperline's synthesized RDB occupies the first cylinder of a bare
hardfile, so place `-r` past it — e.g. `-r 512,2K` on the 2 MB victim, as
all three configs' comments note.

## `ci/smoke.sh`

`ci/smoke.sh` is the CI entry point. It builds a fresh scratch victim,
boots the named config, runs the 30-second smoke profile, and maps
devsoak's final `RESULT` line to a shell exit code:

```
ci/smoke.sh [CONFIG] [DEVICE] [UNIT] [RANGE] [EXTRA-ARGS...]
```

Defaults are `test/a1200-scsi.toml scsi.device 0 512,2K`. It needs
`copperline` and `xdftool` on `PATH`, and a built `./devsoak` (`make`
first). The config's `[serial]` mode must be `"stdout"` so the script can
capture it.

Exit code mapping:

| RESULT line | exit code |
|---|---|
| `RESULT PASS` | 0 |
| `RESULT WARN` | 5 |
| `RESULT FAIL` | 10 |
| no RESULT line at all | 20 |

The guest's own exit code can't cross the emulator boundary, so the
mapping is entirely driven by grepping the serial log for the
machine-greppable verdict — see [CLI Reference](CLI-Reference.md#the-result-verdict-line).
A missing RESULT line means the guest crashed or hung; the serial log
then ends with the last breadcrumb naming the suspect, if `-P`/`-o ser`
were in play (see [Surviving a crash](Quirks.md#surviving-a-crash)).

## Serial capture: stdout vs tcp

`[serial]` in the config controls where the guest's serial output goes.
`mode = "stdout"` writes to the emulator's own stdout — simple, and what
`ci/smoke.sh` expects, but **block-buffered when redirected to a file**,
so a script tailing the file live won't see lines as they're emitted.
`mode = "tcp"` is live: a host script connects and sees output as the
guest produces it, which matters for anything that needs to react to the
run in progress — such as driving removable-media prompts.

## Removable media (`-R`) over the Control Protocol

Testing `-R` needs a genuinely removable unit and something to swap the
disk. Under Copperline, drive that from the host over the Copperline
Control Protocol instead of answering an interactive prompt:

```
copperline-ctl media.floppy.eject  {"unit":0}
copperline-ctl media.floppy.insert {"unit":0,"path":"blank.adf"}
```

as wired up in `test/a1200-floppy.toml`. Only one CCP client may be
attached at a time. Pair a TCP serial sink with the CCP script so the
prompts are visible to the host in real time as devsoak issues them.

## Floppy-specific notes

- trackdisk.device's floppy DMA goes through Paula, which only reaches
  chip RAM — see the `floppy-chip-buffers` quirk in
  [The Quirks File](Quirks.md#worked-example-floppy-chip-buffers).
- `a1200-floppy.toml` sets `[floppy] speed = 0` (turbo), so devsoak fills
  and audits the range at emulator speed rather than real floppy speed;
  note the config also sets `write_protected = false` since the default
  is `true` and devsoak is destructive.
- Use `--no-z` when fingerprinting a floppy target — trackdisk can't
  sustain the tier-3 formats or concurrent access under emulation, but
  its tier 0–2 behaviour still fingerprints cleanly. See
  [Fingerprinting Drivers](Fingerprinting.md#-no-z).

## Pointing at a custom emulator build

`ci/fingerprint.sh` honours a `COPPERLINE` environment variable to
override the emulator binary it invokes, for testing against a local
Copperline build rather than whatever's on `PATH`:

```
COPPERLINE=/path/to/my/copperline ci/fingerprint.sh test/a1200-scsi.toml \
    scsi.device 0 fingerprints/scsi.device-47.4-a1200-ide.txt
```
