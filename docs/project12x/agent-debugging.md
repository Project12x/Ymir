# Project12x agent-debugging fork

This branch adds a narrow, machine-oriented debugger transport to Ymir without changing emulator timing or rendering
paths. `ymir-headless` owns the emulated Saturn and speaks newline-delimited JSON-RPC 2.0 on standard input/output.
Human-readable diagnostics go to standard error. A controller may therefore be implemented and licensed separately as
an ordinary process client. In development builds, the worker preserves its original stdout descriptor for JSON-RPC
and redirects Ymir core development logs to stderr before constructing the Saturn instance.

## Provenance and reuse record

| Source | Pinned revision | License | Files inspected | Reuse mode |
| --- | --- | --- | --- | --- |
| [StrikerX3/Ymir](https://github.com/StrikerX3/Ymir) | `244d5c841e0cb9b0eb1402b39a7742f7f973b1c2` | GPL-3.0 | `LICENSE`, `CONTRIBUTING.md`, `apps/ymir-headless/src/*`, `apps/ymir-dbg/src/main.cpp`, `libs/ymir-dbg-commons/include/**/*`, `libs/ymir-core/include/ymir/debug/debug_break.hpp`, `libs/ymir-core/include/ymir/sys/saturn.hpp`, `libs/ymir-core/include/ymir/hw/sh2/sh2.hpp`, `libs/ymir-core/src/ymir/sys/saturn.cpp`, and the SDL ROM/disc/BRAM loading call sites | Fork plus close-port inside the GPL-covered tree |
| [nlohmann/json](https://github.com/nlohmann/json) | `3.12.0#2` from vcpkg `86dc619bd8d9697405ae5c944b474117ea9457ce` | MIT | Existing `JsonRpcAdapter` integration, package declaration, and installed copyright file | Existing dependency; no vendored copy added |
| [yaul-org/libyaul-gdbstub](https://github.com/yaul-org/libyaul-gdbstub) | `84cfb4e9911dfe635a8c89e465f983043d46c876` | MIT | `LICENSE`, `gdbstub.c`, `gdbstub.h`, `gdbstub-internal.h`, `sh2.c` | Pattern-only research for a later hardware-side GDB bridge; no code copied into this milestone |

Ymir and all modifications in this repository remain GPL-3.0. Do not copy Ymir implementation or internal headers into
the SM64 Saturn port or a permissively licensed automation client. Keep those projects independent and communicate only
through the documented process protocol. If libyaul-gdbstub code is later copied or closely ported into the Saturn
program, preserve its MIT copyright and permission notice.

## Implemented protocol slice

The worker starts paused and emits `instance.ready`. It currently implements:

- `debug.version`
- `instance.status`
- `instance.shutdown`
- `exec.reset`
- `exec.stepi`
- `regs.read`
- `mem.peek`

`mem.peek` uses the SH-2 probe's side-effect-free accessor, bypasses the emulated cache, accepts integer or decimal/hex
string addresses, and limits each response to 64 KiB. Slave SH-2 commands return `target_disabled` until the emulated
SMPC has enabled that processor; `--no-slave` also hides it from remote access but does not modify Saturn hardware
behavior.

Continuous execution, asynchronous pause, breakpoints, disassembly, screenshots, frame hashes, and trace streaming are
not advertised yet. They need a serialized execution controller and explicit stop-event semantics; pretending that a
frame step is `exec.continue` would make automation unreliable.

Example session (one JSON object per line):

```json
{"jsonrpc":"2.0","method":"debug.version","id":1}
{"jsonrpc":"2.0","method":"regs.read","params":{"target":"sh2.master"},"id":2}
{"jsonrpc":"2.0","method":"mem.peek","params":{"address":"0x06004000","count":32},"id":3}
{"jsonrpc":"2.0","method":"exec.stepi","params":{"target":"sh2.master"},"id":4}
{"jsonrpc":"2.0","method":"instance.shutdown","id":5}
```

JSON-RPC parse/shape errors use the standard codes. Debug-domain failures use server error `-32000` and include a
stable `error.data.debug_code` string such as `invalid_state` or `target_disabled`.

## Contribution note

Ymir's `CONTRIBUTING.md` requires disclosure of AI assistance and manually written commit and pull-request descriptions.
This Project12x fork work was produced with Codex assistance. Before offering anything upstream, the repository owner
must manually review and understand every change, constrain the submission to an independently reviewable slice, and
write the disclosure and rationale in their own words.
