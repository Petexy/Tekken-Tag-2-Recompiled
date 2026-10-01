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
   and hands to a rendering backend (null today, Vulkan next). Latte shaders
   the game ships will be translated to SPIR-V; tiled surfaces detiled.
3. **snd_core / snd_user → host audio** (SDL3).
4. **vpad / padscore → SDL3 gamepad**.
5. **nsysnet / nlibcurl / nn_\***: offline stubs.

Cemu, decaf-emu and nWiiURecomp are references and test oracles only; nothing
from them ships in the port.

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
   draws, clears, copies, swaps ──► Backend (null; Vulkan in M3)
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
- Audio: each 3 ms AX frame runs the frame callbacks, then the device
  final-mix callbacks with 48 kHz planar buffers (TV 6 channels, GamePad
  4 x 2). The title's CRI middleware mixes in software and drives its
  engine from these callbacks; output is silent until voices are mixed.

## Running the port

`build-port/ttt2 "<path>/TEKKEN TAG 2 Wii U EDITION (EU).wua"` reads the
executable and all assets from the archive in place (a directory with
code/, content/, meta/ also works). Saves go to `$TTT2_SAVE_DIR`, default
`~/.local/share/ttt2/save`. The OS layer is in `runtime/src/os/`; each OS
function is plain C++ registered with `CAFE_EXPORT(module, name, fn)`, and
anything not implemented stops with the function's name and a guest
backtrace. Iterating on the runtime rebuilds and relinks in ~2 s.

Debugging: every guest function is a native function (`sub_XXXXXXXX_orig`),
so gdb breakpoints, watchpoints and backtraces work on guest code directly;
host threads are named `<OSThread address>/<core>`. The export thunks take
`(PPCContext&, uint8_t* base)`, so `$rsi` in any `imp_*` frame is the guest
base address. `TTT2_TRACE_THREADS=1` logs thread creation, priorities,
affinities and names. The null GPU backend prints frames per second and
draws per frame every ten seconds.

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
| M3 | GX2 → Vulkan | Title/logo screens render correctly |
| M4 | Input, audio, filesystem, saves | Menus navigable with sound |
| M5 | Gameplay | Offline match playable start to finish |
| M6 | Hardening | All stages/characters, long runs, performance |
