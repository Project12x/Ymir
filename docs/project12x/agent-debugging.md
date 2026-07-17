# Project12x agent-debugging fork

This branch adds a narrow, machine-oriented debugger transport to Ymir without changing emulator timing or rendering
paths. `ymir-headless` owns the emulated Saturn and speaks newline-delimited JSON-RPC 2.0 on standard input/output.
Human-readable diagnostics go to standard error. A controller may therefore be implemented and licensed separately as
an ordinary process client. In development builds, the worker preserves its original stdout descriptor for JSON-RPC
and redirects Ymir core development logs to stderr before constructing the Saturn instance.

## Provenance and reuse record

| Source | Pinned revision | License | Files inspected | Reuse mode |
| --- | --- | --- | --- | --- |
| [StrikerX3/Ymir](https://github.com/StrikerX3/Ymir) | `244d5c841e0cb9b0eb1402b39a7742f7f973b1c2` | GPL-3.0 | `LICENSE`, `CONTRIBUTING.md`, `apps/ymir-headless/src/*`, `apps/ymir-sdl3/src/app/app.cpp`, `apps/ymir-sdl3/src/app/services/screenshot_service.cpp`, `libs/ymir-core/docs/mainpage.hpp`, `libs/ymir-core/include/ymir/media/cd_device/cd_device_host.hpp`, `libs/ymir-core/src/ymir/media/cd_device/cd_device_host.cpp`, `libs/ymir-dbg-commons/include/**/*`, `libs/ymir-core/include/ymir/debug/debug_break.hpp`, `libs/ymir-core/include/ymir/sys/saturn.hpp`, `libs/ymir-core/include/ymir/hw/sh2/sh2.hpp`, `libs/ymir-core/src/ymir/sys/saturn.cpp`, and the SDL ROM/disc/BRAM loading call sites | Fork plus close-port inside the GPL-covered tree; the framebuffer callback/copy pattern is adapted for headless snapshots |
| [nlohmann/json](https://github.com/nlohmann/json) | `3.12.0#2` from vcpkg `86dc619bd8d9697405ae5c944b474117ea9457ce` | MIT | Existing `JsonRpcAdapter` integration, package declaration, and installed copyright file | Existing dependency; no vendored copy added |
| [xxHash](https://github.com/Cyan4973/xxHash) | `e573d4d2aaeaba0f3e5a0a9a54144a1f2b4b56e7` | BSD-2-Clause | `LICENSE`, `xxhash.h`, `xxh3.h`, Ymir's existing ISO filesystem hash call sites | Existing vendored dependency; direct `XXH3_128bits` and canonical digest APIs, no implementation copied |
| [stb](https://github.com/nothings/stb) | `f75e8d1cad7d90d72ef7a4661f1b994ef78b4e31` from vcpkg `86dc619bd8d9697405ae5c944b474117ea9457ce` | MIT selected from `MIT OR CC-PDDC` | `LICENSE`, `stb_image_write.h`, Ymir's existing static instantiation and PNG screenshot call sites | Existing dependency; direct in-memory PNG API with a headless-local static instantiation |
| [cppcodec](https://github.com/tplgy/cppcodec) | `8019b8b580f8573c33c50372baec7039dfe5a8ce` | MIT | `LICENSE`, `base64_rfc4648.hpp`, `detail/base64.hpp`, `detail/stream_codec.hpp` | Pattern-only research; no dependency or code copied. The output-only encoder is implemented locally against RFC 4648 to avoid a general codec dependency |
| [yaul-org/libyaul-gdbstub](https://github.com/yaul-org/libyaul-gdbstub) | `84cfb4e9911dfe635a8c89e465f983043d46c876` | MIT | `LICENSE`, `gdbstub.c`, `gdbstub.h`, `gdbstub-internal.h`, `sh2.c` | Pattern-only research for a later hardware-side GDB bridge; no code copied into this milestone |
| [Microsoft Debug Adapter Protocol](https://github.com/microsoft/debug-adapter-protocol) | `e34479c39ed4973210115872c8e118c097a50d4a` | MIT for code/schema | `License-code.txt`, `debugAdapterProtocol.json` (`ContinueRequest`, `PauseRequest`, `StoppedEvent`, `ContinuedEvent`) | Pattern-only protocol research: acknowledge execution control and report a distinct stopped event after actual suspension; no schema or code copied |
| [Microsoft VS Code Mock Debug](https://github.com/microsoft/vscode-mock-debug) | `2f0f960f60c3f5e6f96582ca88f9a42eef331d41` | MIT | `LICENSE.txt`, `src/mockDebug.ts`, `src/mockRuntime.ts` | Pattern-only research for separating runtime stop callbacks from protocol event emission; no code copied |

Ymir and all modifications in this repository remain GPL-3.0. Do not copy Ymir implementation or internal headers into
the SM64 Saturn port or a permissively licensed automation client. Keep those projects independent and communicate only
through the documented process protocol. If libyaul-gdbstub code is later copied or closely ported into the Saturn
program, preserve its MIT copyright and permission notice.

## Implemented protocol slice

The worker starts paused and emits `instance.ready`. Protocol version 0.3.0 implements:

- `debug.version`
- `instance.status`
- `instance.shutdown`
- `exec.reset`
- `exec.continue`
- `exec.pause`
- `exec.run_for`
- `exec.stepi`
- `regs.read`
- `mem.peek`
- `video.frame_hash`
- `video.capture`

`mem.peek` uses the SH-2 probe's side-effect-free accessor, bypasses the emulated cache, accepts integer or decimal/hex
string addresses, and limits each response to 64 KiB. Slave SH-2 commands return `target_disabled` until the emulated
SMPC has enabled that processor; `--no-slave` also hides it from remote access but does not modify Saturn hardware
behavior.

`exec.continue` transfers Saturn ownership to one execution thread. Core inspection commands remain rejected with
`invalid_state` until `exec.pause` has waited for `RunFrame()` to return and the instance is actually quiescent. The
pause response is written before an `instance.stopped` notification with reason `pause`, the master SH-2 PC, and a
monotonic stop sequence. This follows the response/event ordering pattern studied in the MIT-licensed DAP schema while
retaining the project's smaller JSON-RPC protocol.

`exec.run_for` is the deterministic automation path: it runs 1 to 3,600 complete frames synchronously, returns to the
paused state, and emits `instance.stopped` with reason `frame_limit`. It is deliberately bounded; clients should use
continuous execution plus pause when they need an open-ended run. Ymir's SDL emulator-thread loop and host-CD worker
were the implementation references inside the GPL fork. The service uses the same single-owner principle but does not
copy code from the permissive protocol references.

The headless service uses Ymir's software renderer with its worker threads disabled so `RunFrame()` completion is also a
deterministic framebuffer boundary. The render callback copies into a preallocated maximum-size staging buffer. Both
video methods require the instance to be paused and operate on the latest completed frame; they return `no_frame` before
the first completed frame and after `exec.reset` until another frame finishes.

`video.frame_hash` returns `sequence`, `width`, `height`, `pixel_format` (`rgba8888`), `hash_algorithm` (`xxh3-128`),
and a 32-character canonical hexadecimal `hash`. The hash covers only tightly packed, top-to-bottom RGBA8888 bytes, so
clients must compare the dimensions and pixel format too. `video.capture` returns the same identity fields plus an
in-memory PNG as padded RFC 4648 base64 in `data`, with `mime_type`, `encoding`, and the decoded `byte_count`. It never
accepts or writes a host path. Poll with the hash method and request the larger PNG payload only when needed.

Breakpoints, disassembly, and trace streaming are not advertised yet. Breakpoints will also need spontaneous stop
delivery while protocol input is idle; the current notification queue guarantees ordering for request-driven pause and
bounded execution only.

Example session (one JSON object per line):

```json
{"jsonrpc":"2.0","method":"debug.version","id":1}
{"jsonrpc":"2.0","method":"regs.read","params":{"target":"sh2.master"},"id":2}
{"jsonrpc":"2.0","method":"mem.peek","params":{"address":"0x06004000","count":32},"id":3}
{"jsonrpc":"2.0","method":"exec.stepi","params":{"target":"sh2.master"},"id":4}
{"jsonrpc":"2.0","method":"exec.run_for","params":{"frames":60},"id":5}
{"jsonrpc":"2.0","method":"video.frame_hash","id":6}
{"jsonrpc":"2.0","method":"video.capture","id":7}
{"jsonrpc":"2.0","method":"exec.continue","id":8}
{"jsonrpc":"2.0","method":"exec.pause","id":9}
{"jsonrpc":"2.0","method":"instance.shutdown","id":10}
```

JSON-RPC parse/shape errors use the standard codes. Debug-domain failures use server error `-32000` and include a
stable `error.data.debug_code` string such as `invalid_state` or `target_disabled`.

## Verification record

On 2026-07-17, MSVC Debug and Release builds of `ymir-headless-tests` passed all 57 test cases and 294 assertions. The
suite covers bounded frame execution, continuously running-state rejection of core inspection, pause quiescence,
monotonic stopped events, frame conversion/hash/PNG/base64 helpers, reset invalidation, shutdown during continuous
execution, and an actual `ymir-headless` subprocess. The process test parses every stdout line as JSON, verifies
response-before-event order for both `frame_limit` and `pause`, and validates hash and PNG responses; Ymir diagnostics
remain isolated on stderr.

## Contribution note

Ymir's `CONTRIBUTING.md` requires disclosure of AI assistance and manually written commit and pull-request descriptions.
This Project12x fork work was produced with Codex assistance. Before offering anything upstream, the repository owner
must manually review and understand every change, constrain the submission to an independently reviewable slice, and
write the disclosure and rationale in their own words.
