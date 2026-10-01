# Tekken Tag Tournament 2 Wii U → x86-64

This workspace starts a **static recompilation** project for the user's European
Wii U dump. The target is a native x86-64 program; generated C/C++ is acceptable.
**There is no working native Tekken executable yet.**

## Verified input

| Property | Value |
| --- | --- |
| Product | WUP-P-AKNP, European Wii U edition |
| Title ID | `000500001010f800` |
| Title version | 16 (`0x0010`) |
| Executable | `local/wua/000500001010f800_v16/code/Tekken.rpx` |
| RPX SHA-256 | `fc0270465d384386f67804e48716e30bfbe85a5a1004e3a6bfe3ac75f9de5019` |
| CPU | 32-bit big-endian PowerPC / Espresso |
| Entry point | `0x08966b7c` |
| RPX size | 62,966,080 bytes |
| Executable section size | 119,076,920 bytes (includes `.syscall`) |
| Relocations | 1,803,220 |
| Named static import symbols | 508 across 16 modules |

The archive contains one title/version directory and 125 files (15,921,522,945
uncompressed bytes). Only 11 `code`/`meta` files, approximately 68 MiB, have been
extracted. Game content remains in the original WUA. The original archive was
opened read-only. Extracted files are hashed in
[`analysis/input-manifest.json`](analysis/input-manifest.json); the entire WUA
has **not** been hashed or checked against its archive-wide integrity digest.

## Direction

**Standalone native port: no Cemu, no runtime CPU emulation, freely modifiable
output** (decided 2026-10-01). The plan, the measured facts it rests on, and the
milestones are in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md). In short: our
own function-level PowerPC → C++ recompiler (`recomp/`) plus a native Cafe OS
runtime. nWiiURecomp is kept only as a reference and a test oracle.

## Current results

- **The game runs natively into its main loop**, reading `Tekken.rpx` and
  assets straight from the `.wua`: a steady 59.9 frames/s (vsync-paced),
  10-15 draws per frame, with no window yet (the GPU backend renders
  nothing). Every function the game imports statically is implemented.
  `cafe-recomp` turns the reachable 79,564 functions (6.03M instructions)
  into 257 C++ files in ~2 s; clang builds them with the runtime into a
  132 MB x86-64 `ttt2` in ~4 minutes.
- The native Cafe OS layer (`runtime/src/os/`) covers threads and
  synchronisation, heaps, filesystem and saves, system services, ProcUI,
  input, the AX voice model and final-mix stage (no sound output yet), DMA,
  the software keyboard and error viewer (no UI yet) and an offline network.
- GX2 (`runtime/src/gx2/`) is implemented natively and writes real PM4
  command buffers; a command processor (`runtime/src/gpu/`) executes them,
  with surface layouts from AMD's address library. See
  [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md#gx2-and-the-gpu).
- Guest memory accesses are volatile, so guest threads that poll memory see
  each other's stores (`build/tests/memory_model_test`).
- Every non-control-flow instruction variant the game uses matches Dolphin's
  hardware-verified interpreter bit for bit (6,753 encodings x 300 random
  states, 0 mismatches; `build/tests/semantics_test`).
- `cafe-census` facts (205,140 functions from the symbol table, 0 undecodable
  words, 20.3% reachable) are in
  [`analysis/instruction-census.txt`](analysis/instruction-census.txt).
- Not done: rendering (Vulkan backend, Latte shader translation), sound
  output, input from host devices, gameplay.

Earlier evaluation of existing recompilers is in
[`analysis/recompiler-assessment.md`](analysis/recompiler-assessment.md).

## Reproduce the workspace

Run these commands from this directory. Required for extraction: Git, CMake,
a C++20 compiler, Python 3, and zstd development files. The nWiiU tools also
require OpenSSL 3, zlib, shaderc, pkg-config, and Ninja for the commands below.
Those dependencies were available on the current machine. Nothing is installed
system-wide by this workflow.

```bash
python3 tools/setup_deps.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target wua_extract cafe-census -j 4
build/recomp/cafe-census local/wua/000500001010f800_v16/code/Tekken.rpx \
  analysis/instruction-census.json > analysis/instruction-census.txt

build/wua_extract list \
  '/path/to/TEKKEN TAG 2 Wii U EDITION (EU).wua' \
  > analysis/archive-files.tsv
```

For a fresh workspace only (already completed here):

```bash
build/wua_extract extract-analysis \
  '/path/to/TEKKEN TAG 2 Wii U EDITION (EU).wua' \
  local/wua
```

The extractor refuses to overwrite files. For a full extraction later, use
`extract-all` with a **new empty output directory**, such as `local/full-wua`;
it will need approximately 15 GiB. Do not use `local/wua`, which already contains
the extracted analysis files.

```bash
python3 tools/rpx_inspect.py \
  local/wua/000500001010f800_v16/code/Tekken.rpx \
  --output analysis/tekken-rpx.json

cmake -S third_party/nWiiURecomp -B build/nwiiu -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/nwiiu --target \
  nwiiu-analyze nwiiu-recompile nwiiu-run \
  native_test project_generator_test recompile_cli_test -j 4
ctest --test-dir build/nwiiu \
  -R '^(native_test|project_generator_test|recompile_cli_test)$' \
  --output-on-failure

python3 tools/probe_recompiler.py
```

The probe currently exits nonzero because generation is incomplete. Its log and
summary are written to `analysis/latest-recompile.*`. It has a 120-second time
limit, configurable with `--timeout`, and does not run guest code.

The full analyzer manifest can be regenerated with:

```bash
mkdir -p local/analysis
build/nwiiu/nWiiUAnalyzer/nwiiu-analyze \
  --config configs/ttt2-eu-v16.toml \
  local/wua/000500001010f800_v16/code/Tekken.rpx \
  local/analysis/nwiiu-manifest.json
```

An analyzer exit code of 3 means unresolved control-flow records remain. The
manifest is approximately 397 MiB; the condensed summary is in `analysis/`.

Pinned upstream revisions are in `dependencies.lock.json`. Project fixes live
under `patches/nWiiURecomp/` because `third_party/` is ignored. `setup_deps.py`
applies those patches and refuses to reset existing checkouts to a different
revision. GhidraRPXLoader source was inspected as a format reference; Ghidra has
not been installed or run.

## Build and run the port

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target cafe-recomp -j 8
build/recomp/cafe-recomp local/wua/000500001010f800_v16/code/Tekken.rpx generated/ttt2
CC=clang CXX=clang++ cmake -S . -B build-port -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DTTT2_GENERATED_DIR=$PWD/generated/ttt2
cmake --build build-port --target ttt2 -j 8
build-port/ttt2 "/path/to/TEKKEN TAG 2 Wii U EDITION (EU).wua"
```

The semantics test needs the Dolphin reference checkout in
`third_party/ref/dolphin` and `fmt`:
`cmake --build build --target semantics_test && build/tests/semantics_test`.
`build/tests/memory_model_test` needs nothing extra.

## Next engineering milestones

See the milestone table in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).
Next: M3, a Vulkan backend for the command processor (render targets,
textures with detiling, Latte shader to SPIR-V translation) and a window.

## Local data and checks

`local/`, `build/`, `third_party/`, and `generated/` are excluded by `.gitignore`.
Keep the game archive, extracted executables/assets, and generated game code in
those local directories. No game data has been uploaded or published.

Extraction was checked using synthetic archives for exact output, empty and
multi-block files, selective extraction, overwrite rejection, and symlink
rejection. The RPX inspector rejects truncated files, wrong architecture, and
incorrect compressed section lengths. Runtime correctness remains unverified.
