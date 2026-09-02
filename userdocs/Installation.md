# Installation

## Building

devsoak needs [bebbo's amiga-gcc](https://github.com/bebbo/amiga-gcc).
With `m68k-amigaos-gcc` on your `PATH`, `make` builds directly. Otherwise
`make` runs the build inside the `stefanreinauer/amiga-gcc:gcc-v16.1`
Docker image, so you don't need the cross-toolchain installed natively:

```
make
```

Either way the result is a single static `devsoak` binary — nothing else
to link or install on the Amiga side.

### Why `-mcrt=nix13`

The build uses `-mcrt=nix13`, not plain `-noixemul`. The default libnix
variant implements even 32-bit multiply/divide via utility.library
(V36+), which makes the binary refuse to start on Kickstart 1.3
("utility.library failed to load"). `src/soft64.c` additionally provides
pure-software 64-bit helpers so no 64-bit arithmetic can reintroduce the
dependency.

If you change the toolchain flags, re-check that utility.library hasn't
crept back in:

```
m68k-amigaos-nm devsoak | grep -i utility
```

It must print nothing.

## What to copy to the Amiga

Copy `devsoak` and `devsoak.quirks` to the target machine side by side —
by default devsoak looks for the quirks file in the current directory,
then `PROGDIR:`, so keep them together unless you point `-Q` elsewhere.

## Memory requirements

Range sizing: 16–64 MB on a machine with at least 8 MB of fast RAM. The
generation table costs 2 bytes per sector in the range, and each worker
slot buffers `MaxTransfer` bytes, so worker count, queue depth and
`-M` all trade directly against memory. On a 1 MB machine, cut all three
down:

```
devsoak DEVICE UNIT -d -r ...,8M -M 0x8000 -w 2 -q 2
```

Placing the range at the end of the device puts the traffic and the
bounds tests in the same neighbourhood.

## Kickstart 1.3 boot floppy

On Kickstart 1.3 there is no `--run` staging under Copperline (that needs
a 2.0+ shell), and there are no shell built-ins on the guest either, so
build a minimal bootable OFS floppy: devsoak, the quirks file, and a
one-line `s/startup-sequence` that invokes it.

```
xdftool boot.adf create + format Boot ofs + boot install + write devsoak \
    + write devsoak.quirks + makedir s + write ss.txt s/startup-sequence
```

Then run devsoak from that floppy, keeping the resource footprint small
and output on serial (no CLI window to read from):

```
devsoak lide.device 0 -d -r 512,2K -t 30s -w 2 -q 2 -W 60 -y -o ser
```

See [Testing under Emulation](Emulator-Testing.md) for the Copperline
side of running a 1.3 boot floppy in CI.
