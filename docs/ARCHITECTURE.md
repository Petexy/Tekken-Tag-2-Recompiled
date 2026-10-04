# Standalone port architecture

Goal: a native Linux x86-64 Tekken Tag Tournament 2 (Wii U, EU v16) that runs
**without CPU emulation, without Cemu**, and whose code can be freely modified
afterwards. Decided 2026-10-01.

```
Tekken.rpx ──► cafe-recomp ──► generated/*.cpp  (one C++ function per guest function)
                                      │
                 runtime/ (native Cafe OS: coreinit, gx2→Vulkan, snd, vpad, fs)
                                      │
                 game/   (TTT2 overrides, names, mods) ──► ttt2 (ELF x86-64)
```

## Why our own recompiler

nWiiURecomp lifts *basic blocks* that return to a dispatch loop with an
instruction budget and an interpreter fallback: an accelerated emulator, built
for hosting inside Cemu. A port wants *function-level* output (the
XenonRecomp / N64Recomp model): `bl` is a C++ call, `blr` a return, every guest
function a named, overridable C++ function. nWiiURecomp stays useful as an
independent reference interpreter for differential tests.

## Facts the design rests on

Measured on the v16 RPX with `build/recomp/cafe-census`
(`analysis/instruction-census.*`):

| Fact | Consequence |
| --- | --- |
| `.symtab` holds 205,140 sized `STT_FUNC` symbols that partition `.text` exactly (0 overlaps, 5 gaps); names are stripped | Function boundaries are given, not guessed |
| 29,769,084 instructions, **0 undecodable words** in any function | No data islands inside code |
| 190 instruction variants, standard Espresso set incl. paired singles; no `sc` | All OS access is through the 508 imports |
| Game writes GQR5 (`mtspr 901`) | Quantized `psq_l/psq_st` need real GQR state |
| 470,080 `bctrl`, 8,775 `bctr`; 106,292 relocations take function addresses | Indirect calls go through a guest-address → host-function table |
| Switches are `lis/addi; mtctr; bctr` into an in-function table of `b`s (222 functions) | `bctr` with known in-function targets becomes `switch` + `goto` |
| ~1,000 branches enter the save/restore-register helpers at `0x0913D06C..0x0913D3F4` mid-sequence | Branch targets inside another function become alternate entry functions |
| Entry calls an undefined weak symbol (address 0) behind a guard | Null calls trap with a diagnostic |
| Only 79,507 functions / 6.0M instructions (20.3%) are reachable from the entry point and address-taken functions | Compile the reachable set optimised; the rest cheaply, logging if ever called |

Every stored code address in an RPL carries a relocation, which is what makes
the reachability bound sound.

## Generated code conventions

- Guest state: `PPCContext` (r0–r31, f0–f31 as paired doubles, CR as eight
  fields, LR, CTR, XER, FPSCR, GQR0–7, reservation).
- Guest memory: one 4 GiB host reservation; guest address `a` is `base + a`.
  Data stays big-endian; loads/stores byte-swap.
- Every guest load and store is a volatile access (`ppc_ops.h`): guest memory
  is shared between guest threads, so the compiler must perform each access
  in program order. With plain loads clang compiled a guest spin-wait
  (`lwz; cmpwi; bne` to itself) into a single read and the game's worker
  handshake broke. `build/tests/memory_model_test` guards this.
- Each function: `void sub_XXXXXXXX(PPCContext& ctx, uint8_t* base)`, emitted
  as a weak alias of `sub_XXXXXXXX_orig`, so user code can replace any function
  by defining it, and still call the original.
- Imports: calls to stub addresses become calls to
  `cafe_import_<module>_<name>`. The runtime defines the implemented ones;
  generated weak defaults stop with "unimplemented import" for the rest.
- A names file maps addresses to readable names as they are identified.

## Runtime (native Cafe OS)

Implemented in host C++, no guest-code interpretation:

1. **coreinit**: heaps, threads (guest thread = host thread), mutex /
   condition / semaphore / event / alarm / message queue, time, OSDynLoad,
   filesystem mapped onto the extracted content directory, save directory
   under `~/.local/share/ttt2/`.
2. **gx2 → GPU** (see below): GX2 implemented natively, writing PM4 command
   buffers that a command processor executes against a Latte register file
   and hands to a rendering backend: Vulkan, or null for headless runs.
   Latte shaders the game ships are translated to GLSL and compiled to
   SPIR-V at run time; tiled surfaces are detiled.
3. **snd_core / snd_user → host audio**: AX voices mixed natively and
   played through SDL3 (see the audio section below).
4. **vpad / padscore → SDL3 gamepad**.
5. **nsysnet / nlibcurl / nn_\***: offline stubs.

Cemu, decaf-emu and nWiiURecomp are references and test oracles only; nothing
from them ships in the port.

Guest threads are host threads; every kernel object is examined and changed
under one kernel lock. A blocked thread sleeps on a *wait channel* keyed by
the object it waits for (a mutex, condition, semaphore, event, message
queue, thread queue or thread, by guest address; or the interrupt lock,
display, GPU, alarm schedule), and waking an object notifies only its
channel. With one shared condition variable every mutex unlock and every
OSRestoreInterrupts woke all ~40 of the title's blocked threads: the title's
allocator locks a mutex per allocation, and the boot-time loading screen
took ~17 s of herd wake-ups for 0.9 s of work (now ~1 s; CPU use fell from
~5 cores to a quarter of one).

System libraries the title loads with OSDynLoad are implemented the same
way: `swkbd` (software keyboard) and `erreula` (error viewer) have no UI
yet, so a request for text is confirmed with the title's own initial text
and an error is printed and acknowledged. An OSDynLoad export the runtime
lacks still gets an address; calling it stops with its name.

### GX2 and the GPU

```
GX2 (runtime/src/gx2/) ──PM4──► command buffers in guest memory
                                     │ GX2Flush / display lists
                                     ▼
command processor thread (runtime/src/gpu/) ── Latte register file
   draws, clears, copies, swaps ──► Backend (Vulkan; null when headless)
   timestamps, GPU-written memory, swap/flip counts ──► guest
```

- GX2 writes real PM4 packets, big-endian, into the command buffer pool
  GX2Init receives, used as a ring, or into display lists. Context states
  work as on hardware: register writes are shadowed into the
  GX2ContextState while it is current and loaded back with LOAD_* packets.
  Operations the console performs with GX2-internal shaders (clears,
  surface copies, resolves, scan-buffer copies, swaps) are runtime-defined
  packets (opcodes 0x01-0x08) carrying the GX2 structures the title passed.
- Addresses in command buffers are guest virtual addresses; the title never
  translates addresses itself.
- Surface layouts (size, alignment, pitch, mip offsets, tile mode) come from
  AMD's address library in its R600 variant (MIT, pinned in
  `dependencies.lock.json`, `third_party/addrlib`), with the console's
  parameters, because the title allocates from them and ships pre-tiled
  textures laid out by them.
- Fetch shaders are generated as real Latte microcode (little-endian, the
  GPU's order), so the shader translator reads vertex layouts from one
  format whether GX2 or the title produced them.
- The display flips frames at 59.94 Hz, honouring the swap interval;
  GX2GetSwapStatus reports what the title paces its loop on.
- GX2CopySurface with an unaligned linear (LINEAR_SPECIAL) surface on
  either side is a synchronous CPU copy, as on the console: the title
  copies each texture out of one staging buffer and reuses the buffer at
  once. Other copies go to the GPU in order.
- GX2Invalidate's CPU flag (and DMA engine transfers) tell the renderer the
  CPU wrote memory (`gpu::cpu_wrote`); SURFACE_SYNC packets are GPU cache
  operations only. The title flushes the whole texture cache several times
  a frame, which says nothing about what changed.
- A texture over a render target of a different format with the same
  element size is copied from it, reinterpreted: the title compresses
  textures on the GPU by rendering each 4x4 block as one texel (BC1 over
  R16G16B16A16, BC3 over R32G32B32A32_UINT) and samples the memory as BC1
  or BC3, every mip level from its own target.
- The title never uses multisampled render targets and never issues a
  stream-out "draw opaque" (it does not import GX2DrawStreamOut); neither
  is implemented.

### Audio

Every 3 ms AX frame (`runtime/src/os/ax.cpp`) mixes each playing voice,
runs the title's frame callbacks, passes each device's output through the
device final-mix callbacks (48 kHz planar 32-bit buffers in 16-bit range:
TV 6 channels, GamePad 4 x 2) and plays the TV's channels 0-1 as stereo
(`runtime/src/host/audio.cpp`, SDL3). A voice reads PCM16, PCM8 or
DSP-ADPCM from guest memory at its 16.16 rate ratio (relative to AX's
32 kHz renderer), linearly interpolated, scaled by its volume envelope and
mixed into each device channel by its device mix; volumes ramp by their
per-sample deltas. Frames are steady 3 ms ticks, as on the console: the
title's sound engine fills its voices from its own threads, and frames run
in bursts at the device's demand read audio it has not written yet (that
silenced half of the output after the intro). With a device the tick is
nudged by at most 0.5% to keep ~2048 frames queued, following the device's
clock (in practice by a few hundredths of a percent); if the queue runs dry
(start-up, a stall) silence refills it at once.

The title's CRI ADX2 middleware decodes and mixes in software and outputs
5.1 at 44.1 kHz through six looping PCM16 voices of 2,880 samples; their
device mixes fold them to TV stereo, and their volume envelopes come from
the MIX library (`snd_user.cpp`: input levels in 0.1 dB, ramped over one
frame). No voice sends to the aux (effect) buses, so AXFX effects are
never run; filters are accepted and not applied.

### Latte shaders

`runtime/src/latte/` decodes Latte microcode (control flow, ALU groups,
texture and vertex fetches, subroutines) and translates it to GLSL
(`translate.cpp`). Each invocation runs as one thread of the console GPU:
GPRs are integer `ivec4`s that float instructions bit-cast, an ALU group
reads every operand before it writes, PV/PS carry the previous group's
results, and DX9-style `MUL`/`MULADD`/`DOT4` treat 0 × anything as 0. The
per-thread active mask and push/pop stack are variables, so every clause
is guarded and the jumps that only skip inactive threads are dropped;
DX10 loops become `while` loops with `break`. Relative register addressing
(`R[base + AR]`, used by loops over register arrays such as the main
menu's depth-of-field blur) goes through `rel_load`/`rel_store`, which
switch over the program's registers (`SQ_PGM_RESOURCES.NUM_GPRS`) so the
register file is only ever indexed by constants: indexing it directly
made the driver keep all 128 registers in scratch memory, and that one
blur took 16.5 ms a pass at 1440p (now 0.07 ms; every shader the title
uses through a match compiles with no scratch memory, checked with
`RADV_DEBUG=shaderstats`). What the microcode does not
decide (vertex semantics, the VS→PS parameter linkage, texture types,
render target number types, alpha test, point sprites, stream-out) comes
from the register file (`environment.cpp`) and is part of the shader key.
The fetch shader is inlined at `CALL_FS`.

Shaders read guest memory directly through buffer device addresses
(`shader_abi.h`): vertex buffers (decoded and endian-swapped in the
shader), uniform blocks (raw little-endian, as the title stores them) and
buffer fetches; stream-out (`MEM_STREAM`) writes guest memory the same
way. `cafe-shader translate <dump dir>` translates and compiles every
draw recorded with `TTT2_DUMP_SHADERS`.

### Vulkan renderer

`runtime/src/gpu/vulkan/`: Vulkan 1.3 with dynamic rendering, push
descriptors and extended dynamic state; one pipeline per shader pair,
attachment formats and blend state.

- Guest memory (MEM2, MEM1) is imported with VK_EXT_external_memory_host,
  so the GPU reads the title's buffers in place.
- Submission is asynchronous: command buffers (8 in turn) and upload
  memory (8 chunks of a 256 MiB host buffer) are tracked on a timeline
  semaphore and reused once the GPU has finished with them. The command
  processor never waits; a retirement thread performs what the guest can
  observe (retired timestamps, end-of-pipe memory writes, finished
  frames) in command order once the GPU work before it has finished, so
  buffers the guest reuses after a wait are no longer read.
- Render targets and depth buffers are GPU images keyed by address,
  format and size (the title aliases memory between differently sized
  targets). A texture is an image per resource: copied from a render
  target at its address if the GPU wrote that last, otherwise detiled from
  guest memory and reloaded when the CPU writes it and its hash changes.
  Depth buffers sampled as textures are copied through a buffer.
- Detiling (`gpu/tiling.cpp`) uses addrlib once per 8×8 micro tile: in
  thin single-sample modes every micro tile has the same internal layout
  (verified against per-element addrlib, `TTT2_CHECK_TILING=1`).
- The TV scan buffer is blitted, letterboxed, to an SDL3 window
  (`runtime/src/host/window.cpp`, on the process's main thread).
- Upscaling: targets the size of the screen and its halvings (heights 720,
  368, 192, 96) get images `TTT2_SCALE` times larger (default: enough to
  cover the display, 2 on 1080p and 1440p, 3 on 4K). Shadow maps, the
  GPU's texture-compression targets and the GamePad screen keep the
  title's size. Passes into upscaled targets scale the viewport, scissor
  and point size; pixel shaders see `gl_FragCoord` in the title's pixels;
  textures copied from upscaled targets stay upscaled, and shaders scale
  texel coordinates, size queries and texel offsets for them
  (`DrawConstants` carries the scale and a per-stage mask). Copies
  between images of different scale are filtered blits.
- Validated 2026-10-02 with the Khronos layer (core, synchronization,
  object lifetime, thread safety) through boot, menus and a match: no
  errors or hazards. One warning remains by design,
  Undefined-Value-ShaderOutputNotConsumed: some depth-only passes use pixel
  shaders that also write colour, which Vulkan discards when no colour
  attachment is bound; trimming those outputs would multiply shader
  variants.
- Frame interpolation (`gpu/vulkan/interpolate.cpp`, default on displays
  of 100 Hz or more, `TTT2_FPS=60/120/180/240`): the title's logic,
  physics and hitboxes stay at 59.94 steps a second; between two of its
  frames the renderer shows K-1 more. The command processor logs each
  frame (register writes and operations with their arguments, nothing the
  guest can observe) and replays it after the swap; every draw's constant
  data (uniform blocks, buffer resources: camera, transforms, the
  characters' bone matrices, fetched by the vertex shaders) is copied into
  a GPU-visible record that the real draw reads too, and a replay blends
  it with the previous frame's matching draw, value by value where both
  are ordinary floats. Draws match by key (shaders, targets, vertex count
  and first texture): first those whose data did not change, then each
  with the most similar draw near its place among those with its key, so
  sprites sharing a key keep their partners when others come and go (a
  blinking cursor once paired every later menu sprite with its
  neighbour). Only depth-tested (3D) draws blend: menus and the HUD place
  parts of a widget with vertex data the CPU writes each frame, which a
  replay cannot blend, so they are shown as the frame has them
  (`TTT2_INTERP_2D=1` blends them too). Draws whose data mostly jumps (a
  different object, a cut) and frames where fewer than 80% of draws match
  are shown as they are. Replays skip anything that writes guest memory (stream-out writes,
  CPU-side surface copies); render targets a frame reads before writing
  them start replays from their start-of-frame content, and the next
  frame sees the real frame's. With unblended replays
  (`TTT2_INTERP_TEST=1`) every extra frame equals its real frame pixel for
  pixel (checked through menus, character select and fights). Particles
  whose geometry the CPU rebuilds each frame and the HUD move at 60 Hz.
  A frame whose swap comes while the GPU has yet to finish the previous
  frame's images gets no extra frames (the previous image stays up
  through their part of its slot): the title learns a frame is done only
  after its replays, so on a shared or overloaded GPU they would slow the
  game itself (measured: a fight on a desktop compositor taking ~80% of
  the GPU went from 46-51 to 55-60 title frames a second; the statistics
  line counts the extra frames skipped).
- Presentation (`gpu/vulkan/present.cpp`) runs on its own thread and on a
  compute queue (AMD's asynchronous compute, `TTT2_PRESENT_BLIT=1` for
  the rendering queue): a present there waits only for its own frame, not
  for rendering queued after it. A compute shader scales the frame,
  letterboxed, into the swapchain. The mode follows the display the
  window is on, chosen again when the window moves to another display or
  its mode changes. On a display refreshing at a multiple of 60 Hz of at
  least 120 Hz with present waits (`VK_KHR_present_wait2`), the thread
  presents at every refresh in FIFO order, a few presents ahead of the
  screen (about 8 ms: 3 at 240 Hz, 2 at 120 Hz; KWin latches a commit
  about two refreshes before showing it, and tells late that it did), and
  counts refreshes one per present reaching the screen, plus, by time
  since the last present it waited for, any refresh the screen showed
  twice (once the next present confirms it: a single late wake-up is not
  one), part refreshes carried over (a display slower than reported by
  other than a whole factor) and, when it fell behind or the compositor
  stalled, the time that passed. Every 60th of a second of them is the title's vertical blank
  (`gpu::host_vsync`; the 59.94 Hz timer stands in when they stop for two
  frames, e.g. for a hidden window, or at once when presentation stops
  following the display, and the blanks it gives count against the
  host's when they resume), and each frame's images are due at fixed
  refreshes after the vblank its frame started from, the extra frames
  first and the real one last. So the title runs at the display's own
  rate (59.99 Hz on a 239.97 Hz display) and every image stays on screen
  for the same number of refreshes: measured in fights at 2x on a 240 Hz
  KWin desktop, 99% of images shown for exactly two refreshes (before,
  frames timed by the clock alone came 6 to 11 ms apart). The time
  between presents it waited for checks the reported rate: a display
  refreshing more than 2% faster (in two windows of 240 presents in a
  row) chooses the mode again with the measured rate; a slower one, or a
  compositor skipping refreshes, is already counted by time
  (`TTT2_DISPLAY_HZ` fakes a reported rate to test this). Presents that fail or do not reach the screen show the newest
  finished image at once, so the queue keeps moving; when the renderer
  replaces the presentation images (the TV buffer changed size) the
  thread lets go of them first. The lead from vblank to the first image
  is what frames needed over the last ten seconds, all but the slowest
  1%, raised at once when frames keep coming late (7-9 refreshes,
  ~30-37 ms, in fights). On other displays, including 60 Hz ones where
  presenting every refresh would only add its queue's latency
  (`TTT2_PRESENT_TIMED=1` forces it), frames get display slots one title
  frame apart (re-anchored when the title falls behind) at a lead after
  their swap and are presented at those times (mailbox). With
  interpolation the real image is shown half a frame later than
  without.
- Compiled SPIR-V and the Vulkan pipeline cache persist in
  `~/.cache/ttt2` (`$XDG_CACHE_HOME/ttt2`).

## Running the port

`build-port/ttt2 "<path>/TEKKEN TAG 2 Wii U EDITION (EU).wua"` reads the
executable and all assets from the archive in place (a directory with
code/, content/, meta/ also works). Reading through the archive costs
nothing measurable in play (`TTT2_TRACE_FS=1`): a fight reads ~0.5 MB of
music per 10 s (0.3 ms), loading a stage ~95 MB (0.5 s from a hard disk,
mostly seeks). Saves go to `$TTT2_SAVE_DIR`, default
`~/.local/share/ttt2/save`. The OS layer is in `runtime/src/os/`; each OS
function is plain C++ registered with `CAFE_EXPORT(module, name, fn)`, and
anything not implemented stops with the function's name and a guest
backtrace. Iterating on the runtime rebuilds and relinks in ~2 s.

The window shows the TV image (F11 toggles fullscreen, `TTT2_FULLSCREEN=1`
starts in it). Keyboard: arrows
D-pad, X/Z/S/A the A/B/X/Y buttons, Q/W L/R, 1/2 ZL/ZR, Enter +, Backspace
−, H Home, I/J/K/L the left stick; SDL gamepads map by button position.
`TTT2_GPU=null` runs headless without a window. Sound plays on the
default output device; `TTT2_AUDIO=0` disables it and
`TTT2_AUDIO_VOLUME=<percent>` scales it.

Unattended runs press buttons with `TTT2_INPUT_SCRIPT`. From a cold start
this reaches an arcade match (solo, Heihachi) at about 72 s; append
presses such as `,72:y,73:x,...` to fight:
`TTT2_INPUT_SCRIPT="8:plus,21:plus,24:plus,31:a,37:a,41:right,42:right,43:right,45:a,49:right,50:right,53:a,57:a"`.
Character select refuses a character already picked for the team.

A guest memory fault prints the guest registers and backtrace, says when
the address lies just below the stack pointer (a stack overflow), and
gives the host code offset, which `addr2line -f -e build-port/ttt2
<offset>` turns into the guest function. The main thread gets at least a
2 MB stack whatever the RPX asks for (TTT2's says 64 KB, but its menus
nest two functions with 501 KB frames).

Debugging: every guest function is a native function (`sub_XXXXXXXX_orig`),
so gdb breakpoints, watchpoints and backtraces work on guest code directly;
host threads are named `<OSThread address>/<core>`. The export thunks take
`(PPCContext&, uint8_t* base)`, so `$rsi` in any `imp_*` frame is the guest
base address. `TTT2_TRACE_THREADS=1` logs thread creation, priorities,
affinities and names. The GPU backends print frames per second and draws
per frame every ten seconds. Renderer debugging:

| Variable | Effect |
| --- | --- |
| `TTT2_CAPTURE=<dir>` | TV/GamePad images as PNG every `TTT2_CAPTURE_INTERVAL` seconds (default 5) |
| `TTT2_CAPTURE_SEQUENCE=<n>` | with `TTT2_CAPTURE`, also every image shown for the next n frames after each capture, in order (`seq_<capture>_<frame>_<n>.png`, the real image last) |
| `TTT2_TRACE_AT=<s>`, `TTT2_TRACE_FRAME=<n>` | log every command of one frame; with `TTT2_CAPTURE`, save all its render targets |
| `TTT2_INPUT_SCRIPT="25:plus,26.5:a"` | press GamePad buttons at given seconds, for unattended runs |
| `TTT2_DUMP_SHADERS=<dir>` | shader binaries, draw registers and generated GLSL |
| `TTT2_DUMP_TEXTURES=<dir>` | every texture loaded from memory, as PNG |
| `TTT2_TRACE_TARGETS=1`, `TTT2_WATCH=<hex address>` | render target creation; writes and loads touching an address |
| `TTT2_CHECK_TILING=1` | compare fast detiling with addrlib per element |
| `TTT2_SCALE=1..4` | render scale of screen-sized targets (default from the display) |
| `TTT2_FPS=60/120/180/240` | frames shown per second (default 120 on displays of 100 Hz and more) |
| `TTT2_INTERP_TEST=1` | replays without blending: extra frames must equal real ones |
| `TTT2_INTERP_2D=1` | blend flat (not depth-tested) draws too |
| `TTT2_PRESENT_TIMED=1` | present at chosen times instead of at every refresh |
| `TTT2_PRESENT_QUEUE=1..4` | presents kept ahead of the screen when presenting every refresh (default about 8 ms of refreshes) |
| `TTT2_DISPLAY_HZ=<hz>` | report this refresh rate for the window's display (tests the rate check) |
| `TTT2_FULLSCREEN=1` | start fullscreen |
| `TTT2_TRACE_INTERP=1`, `TTT2_TRACE_PACING=1` | draw matching, carried targets; frame readiness, swap times after the vblank |
| `TTT2_PROFILE_AT=<s>` | GPU time of every operation of one frame |
| `TTT2_VK_VALIDATION=1` | Khronos validation layer, if installed (synchronization checks: `khronos_validation.validate_sync = true` in a file named by `VK_LAYER_SETTINGS_PATH`) |
| `TTT2_AUDIO_DUMP=<file.wav>` | everything played, as 48 kHz stereo WAV (also without a device) |
| `TTT2_TRACE_AX=1` | voice set-up, device mixes, output level |
| `TTT2_TRACE_FS=1` | file and save opens, reads and writes, with times |

## Verification

- Per-instruction differential tests (`build/tests/semantics_test`, done):
  `cafe-semgen` samples up to 48 real encodings of every non-control-flow
  instruction variant in the game (6,753 encodings); each runs on 300 random
  machine states (edge integers, NaNs, infinities, denormals, random GQR
  formats) and must match Dolphin's Broadway interpreter bit for bit in every
  register and in memory. Dolphin is built from `third_party/ref` against a
  shim (`tests/oracle/`) and is a test-only dependency. Result 2026-10-01: 0
  mismatches. One oracle correction: Dolphin's `addme`/`subfme` carry is
  wrong when CA=1 (architecture and Cemu agree on CA=1); the test documents it.
  Branches, `lwarx`/`stwcx.`, `dcbz` and traps are not covered by it yet.
- Reference checkouts (sparse, read-only, never shipped), recreate with
  `git clone --filter=blob:none --sparse` at these revisions:
  Dolphin `771fb154059c5812d6a715a23226249cc42877a2` into
  `third_party/ref/dolphin` (Source/Core/Core/PowerPC/Interpreter,
  Gekko.h, PowerPC.h, ConditionRegister.*, Common/{BitField,BitUtils,
  CommonTypes,FloatUtils,Inline,Swap}); Cemu (MPL-2.0)
  `0a2b6ff7db61871b0dd028ac47dc72eda94351a1` into `third_party/ref/cemu`
  (src/Cafe/HW/Espresso/Interpreter, src/Cafe/HW/Latte/{ISA,Core,LatteAddrLib},
  src/Cafe/OS/libs/{coreinit,gx2,snd_core,dmae,swkbd,erreula,...});
  decaf-emu (GPL-3.0) `e6c528a20a41c34e0f9eb91dd3da40f119db2dee` into
  `third_party/ref/decaf` (src/libdecaf/src/cafe/libraries/{gx2,swkbd,erreula},
  src/libgpu/latte, src/libgpu/src). Where the two disagree, Cemu's
  behaviour was preferred because it runs this title.
- `build/tests/memory_model_test`: guest polling loops, compiled as the
  generator emits them with the port's flags, must wait for and then see
  another thread's store (fails against plain loads; passes).
- Per-function differential tests on the real image.
- Later: function-entry/exit traces from the game running in Cemu (test oracle
  only) compared with the port.
- Acceptance is behavioural: boots, menus, an offline match, saves, and long
  runs without divergence. A window opening is not acceptance.

## Milestones

| | Milestone | Exit criterion |
| --- | --- | --- |
| M0 | Loader, decoder, census | Done 2026-10-01 |
| M1 | Code generator for the full ISA | Done 2026-10-01: all 205,200 entries compile (`--all`, 14 min); semantics and control-flow tests pass with 0 mismatches |
| M2 | Runtime core, boot | Done 2026-10-01: the game runs its main loop at a steady 59.9 frames/s (vsync-paced) on the null GPU backend, every static import implemented |
| M3 | GX2 → Vulkan | Done 2026-10-01: intro movie, logos, title screen, attract-mode fights, main menu and character select render correctly at ~58 frames/s |
| M4 | Input, audio, filesystem, saves | Done 2026-10-02: menus and character select navigate, sound plays (checked from WAV dumps: content, levels, no buffer underruns), saves load and persist (the battle record survives a restart) |
| M5 | Gameplay | Reached with scripted input 2026-10-02: an arcade match from character select through both rounds, K.O., continue and game over, back to the menu with the record updated. To confirm by hand with a keyboard or gamepad |
| M6 | Hardening | All stages/characters, long runs, performance |
