# CLI Reference

```
devsoak DEVICE UNIT -d -r START,LEN [options]
```

devsoak refuses to run without both `-d` and `-r`, prints the device
geometry and the range, and asks for confirmation unless `-y` is given.
It never writes outside the range (the bounds probes included).

## Options

| option | default | notes |
|---|---|---|
| `-d` | — | destructive; required |
| `-r START,LEN` | — | test range in sectors; required. K/M/G suffixes multiply by 1024 — these are **sector counts, not bytes**. 0x hex accepted |
| `-t DURATION` | `60s` | e.g. `30s`, `20m`, `8h` |
| `-w N` | 4 (max 8) | worker tasks |
| `-q N` | 4 (max 8) | outstanding requests per worker |
| `-S N` | 256 | stripe size in sectors |
| `-A MIN` | 10 | full audit interval in minutes; 0 = start/end only |
| `-M BYTES` | `0x1FE00` | MaxTransfer; set to the driver's real limit |
| `-m ADDR` | — | extra buffer memory region (hex address); may repeat |
| `-s SEED` | from clock, printed | PRNG seed |
| `-e` | off | stop on first error |
| `-y` | off | skip the destructive-run confirmation |
| `-B` | off | big device: include the 4 GB boundary tests |
| `-R` | off | removable media semantics (change-interrupt phase) |
| `-H CMD` | — | shell command run to trigger eject/insert (with `-R`) |
| `-X` | off | include the HD_SCSICMD tests |
| `-W SEC` | 5 | watchdog timeout; see sizing note below |
| `-o con\|ser\|both` | `con` | output sink |
| `-Q FILE` | `devsoak.quirks` in the current directory, then `PROGDIR:` | quirks file |
| `-k ID[,ID]` | — | force these quirk ids on even if not status `confirmed` |
| `-K` | off | ignore the quirks file entirely (driver-under-test mode) |
| `-Z` | off | include the "risky" tier-3 tests (random command numbers, unadvertised 64-bit dialects, TD_FORMAT variants) |
| `-P FILE` | — | crash-breadcrumb file — see [Surviving a crash](Quirks.md#surviving-a-crash) |
| `--resume` | — | after a crash: report the suspect from `-P FILE` (needs the DEVICE/UNIT arguments too, for the suggested quirk block) |
| `-v` | off | verbose |

### Watchdog sizing (`-W`)

`-W` flags any request outstanding longer than N seconds as a hang. The
default (5 s) suits real hardware at low queue depths. The *oldest*
request legitimately waits about `w × q × MaxTransfer / device-speed`
seconds under full load — 16 in-flight 127 KB requests on a ~1 MB/s PIO
device queue past 5 s while perfectly healthy. Size `-W` above that
product, e.g. `-W 30` for `-w 4 -q 4` on slow IDE.

### `-o` output sinks

Everything goes through one output layer:

- `-o con` writes to the CLI.
- `-o ser` emits every character through the ROM debug serial port (exec
  `RawPutChar`) — unbuffered, so a machine that crashes on the next
  command has already emitted the line; CR LF endings for terminal/log
  capture.
- `-o both` does both.

Nothing else may have serial.device open during a serial run — `RawPutChar`
drives the same hardware serial.device would use — and the baud rate is
whatever the ROM/Prefs left it at (typically 9600 on 1.3).

## Exit codes

| code | meaning |
|---|---|
| 0 | clean |
| 5 | warnings only (quirk-downgraded findings, or a run cut short) |
| 10 | data or behaviour error (or watchdog hang) |
| 20 | fatal: bad arguments, cannot open device, allocation failure |

## The RESULT verdict line

Because a guest exit code doesn't reach the host through an emulator's
serial log, the last thing devsoak prints is a machine-greppable verdict:

```
devsoak: RESULT PASS
devsoak: RESULT WARN ... rc=5
devsoak: RESULT FAIL rc=10
```

CI should key on that line rather than the process exit code. See
[Testing under Emulation](Emulator-Testing.md#cismokesh) for how
`ci/smoke.sh` maps it to a shell exit code, including the case where no
RESULT line appears at all (a crash or hang).
