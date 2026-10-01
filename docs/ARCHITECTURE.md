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
per-sample deltas. The frame clock follows the playback device: a frame is
produced when less than 30 ms of output is queued, so the title makes sound
exactly as fast as it is played (falling back to the 3 ms clock if the
device stops consuming, or without a device).

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
DX10 loops become `while` loops with `break`. What the microcode does not
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
- The renderer is synchronous with the command processor: work is
  submitted and waited for before a submission's timestamp retires, an
  end-of-pipe event is written or a frame completes. Everything the guest
  can observe follows the GPU work it depends on, and buffers it reuses
  after a wait are no longer read.
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
- Validated 2026-10-02 with the Khronos layer (core, synchronization,
  object lifetime, thread safety) through boot, menus and a match: no
  errors or hazards. One warning remains by design,
  Undefined-Value-ShaderOutputNotConsumed: some depth-only passes use pixel
  shaders that also write colour, which Vulkan discards when no colour
  attachment is bound; trimming those outputs would multiply shader
  variants.
- Compiled SPIR-V and the Vulkan pipeline cache persist in
  `~/.cache/ttt2` (`$XDG_CACHE_HOME/ttt2`).

## Running the port

`build-port/ttt2 "<path>/TEKKEN TAG 2 Wii U EDITION (EU).wua"` reads the
executable and all assets from the archive in place (a directory with
code/, content/, meta/ also works). Saves go to `$TTT2_SAVE_DIR`, default
`~/.local/share/ttt2/save`. The OS layer is in `runtime/src/os/`; each OS
function is plain C++ registered with `CAFE_EXPORT(module, name, fn)`, and
anything not implemented stops with the function's name and a guest
backtrace. Iterating on the runtime rebuilds and relinks in ~2 s.

The window shows the TV image (F11 toggles fullscreen). Keyboard: arrows
D-pad, X/Z/S/A the A/B/X/Y buttons, Q/W L/R, 1/2 ZL/ZR, Enter +, Backspace
−, H Home, I/J/K/L the left stick; SDL gamepads map by button position.
`TTT2_GPU=null` runs headless without a window. Sound plays on the
default output device; `TTT2_AUDIO=0` disables it.

Unattended runs press buttons with `TTT2_INPUT_SCRIPT`. From a cold start
this reaches an arcade match (solo, Heihachi) at about 72 s; append
presses such as `,72:y,73:x,...` to fight:
`TTT2_INPUT_SCRIPT="8:plus,21:plus,24:plus,31:a,37:a,41:right,42:right,43:right,45:a,49:right,50:right,53:a,57:a"`.
Character select refuses a character already picked for the team.

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
| `TTT2_TRACE_AT=<s>`, `TTT2_TRACE_FRAME=<n>` | log every command of one frame; with `TTT2_CAPTURE`, save all its render targets |
| `TTT2_INPUT_SCRIPT="25:plus,26.5:a"` | press GamePad buttons at given seconds, for unattended runs |
| `TTT2_DUMP_SHADERS=<dir>` | shader binaries, draw registers and generated GLSL |
| `TTT2_DUMP_TEXTURES=<dir>` | every texture loaded from memory, as PNG |
| `TTT2_TRACE_TARGETS=1`, `TTT2_WATCH=<hex address>` | render target creation; writes and loads touching an address |
| `TTT2_CHECK_TILING=1` | compare fast detiling with addrlib per element |
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
