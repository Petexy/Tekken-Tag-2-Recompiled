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
2. **gx2 → Vulkan**: GX2 API implemented natively; Latte shaders the game
   ships are translated to SPIR-V (ahead of time where possible); tiled
   surfaces detiled.
3. **snd_core / snd_user → host audio** (SDL3).
4. **vpad / padscore → SDL3 gamepad**.
5. **nsysnet / nlibcurl / nn_\***: offline stubs.

Cemu, decaf-emu and nWiiURecomp are references and test oracles only; nothing
from them ships in the port.

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
  CommonTypes,FloatUtils,Inline,Swap}); Cemu
  `0a2b6ff7db61871b0dd028ac47dc72eda94351a1` into `third_party/ref/cemu`
  (src/Cafe/HW/Espresso/Interpreter, src/Cafe/OS/libs/coreinit).
- Per-function differential tests on the real image.
- Later: function-entry/exit traces from the game running in Cemu (test oracle
  only) compared with the port.
- Acceptance is behavioural: boots, menus, an offline match, saves, and long
  runs without divergence. A window opening is not acceptance.

## Milestones

| | Milestone | Exit criterion |
| --- | --- | --- |
| M0 | Loader, decoder, census | Done 2026-10-01 |
| M1 | Code generator for the full ISA | All 205k functions compile; instruction tests pass. **In progress:** reachable set builds and runs; semantics test passes; control-flow tests and a `--all` build remain |
| M2 | Runtime core, boot | Entry point runs to the game's main loop with stubbed GPU/audio |
| M3 | GX2 → Vulkan | Title/logo screens render correctly |
| M4 | Input, audio, filesystem, saves | Menus navigable with sound |
| M5 | Gameplay | Offline match playable start to finish |
| M6 | Hardening | All stages/characters, long runs, performance |
