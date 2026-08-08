# arm-selfmod-lab

A minimal, Dockerized ARMv7-A bare-metal test harness for prototyping fixes
to a real-hardware bug found while rebuilding the Mercusys MR80X v5 APPSBL
from source (see the sibling `appsbl` project's `CLEAN_ROOM_STATUS.md` for
the full investigation).

## Background

The APPSBL's HTTP recovery upload can be abused to overlay a from-source
rebuild of the APPSBL onto the copy currently executing in RAM, then boot a
kernel from RAM instead of writing to NAND. The exploit's upload buffer
necessarily grows to cover the address range the *currently-executing*
firmware occupies, which means at some point during the transfer, the
vendor's own (unmodified) `memcpy()` call - the one copying each incoming
TCP segment into the upload buffer - starts writing over its own and its
caller's currently-executing code.

Real hardware testing confirms a crash exactly at that boundary (`prefetch
abort`, `pc` garbage, `lr` pinned to the return address of that `memcpy()`
call), reproducibly, regardless of whether the bytes being written there are
byte-identical to what was already present. That rules out a simple
"keep the source identical" fix and points to a genuine ARM self-modifying
code hazard: writing to memory the CPU has already fetched/is mid-executing
can corrupt in-flight execution independent of the value written, absent
explicit cache/pipeline barriers (`DSB`+`ISB`+I-cache invalidate) - which the
vendor's own, already-flashed code has no way to include, since we can't
patch code before it runs once.

This repo is a cheap, fast-iterating sandbox for the two things that
actually need validating before spending another real-hardware cycle
(each of which costs ~2-5 minutes: 100MB+ HTTP upload, physical crash,
reset, reconnect):

1. **Whether QEMU can even reproduce the hazard.** (Spoiler in
   `experiments/01-qemu-self-mod-limitation`: it mostly can't, and that's
   important to know before trusting any QEMU result about this bug.)
2. **The control-flow mechanics of a fix that sidesteps the hazard
   entirely**, by redirecting execution to our own, barrier-protected code
   *before* the transfer reaches the dangerous address range - via a
   stack-smash-style return-address overwrite, since the crash dump shows
   the live stack sits inside our writable range and is reached slightly
   *before* `.text` (see `experiments/02-stack-redirect-poc`).

## Why not just run the real firmware in QEMU?

There's no upstream QEMU machine model for the IPQ5018 (NAND layout, RTL8367
switch, clocking, etc.) - building one would be its own project. Everything
here runs a small, purpose-built bare-metal program that reproduces just the
mechanism in isolation, not the real firmware.

## Requirements

Docker only. No package is installed on the host - `docker build` pulls a
throwaway Ubuntu base and installs `qemu-system-arm` + `gcc-arm-none-eabi`
inside the image.

## Usage

```sh
make build   # builds the toolchain/qemu image (once)
make run-01  # QEMU self-modifying-code limitation demo
make run-02  # stack-redirect proof of concept
```

Each experiment prints PASS/FAIL plus an explanation to stdout via ARM
semihosting, then exits QEMU.
