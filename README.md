# Tekken Tag Tournament 2 for Linux

A native Linux port of **Tekken Tag Tournament 2 Wii U Edition**, made by
static recompilation: the game's PowerPC code is translated to C++ ahead of
time and runs on a reimplementation of the Wii U's system libraries, with a
Vulkan renderer. There is no emulator and no CPU emulation at run time.

> [!IMPORTANT]
> **No game files are included.** This repository holds only original tools
> and runtime code. You need your own copy of the game ([see below](#the-game))
> to build and play. The C++ generated from the game, and any program built
> from it, contain the game's code: keep them to yourself.
>
> This is an unofficial fan project, not affiliated with or endorsed by
> Bandai Namco Entertainment or Nintendo. TEKKEN, Tekken Tag Tournament and
> Wii U are trademarks of their respective owners.

## Features

- **The whole game, natively**: intro, menus, character select, arcade mode
  and versus matches, with sound and saves. Arcade mode has been played
  through by hand.
- **120 frames per second**: between each two frames the game renders, the
  port shows one more, drawn from the same frame with the camera, objects
  and character skeletons blended halfway. The game's logic, physics and
  hitboxes stay at 60 Hz. When other programs keep the GPU busy, frames go
  out without their extra frame rather than slowing the game down.
- **Higher resolutions**: 2x (2560x1440) or more of the original 720p, chosen
  from your display.
- **Even frame pacing** on 120 Hz and 240 Hz displays: the game's clock
  follows the display's refreshes, so every image stays on screen equally
  long.
- **Installs like any other program**: `ttt2 --install` copies the game into
  a folder you choose and adds it to the application menu.
- **Made to be modified**: every function of the game is a C++ function that
  can be replaced (`PPC_FUNC(sub_<address>) { ... }`).

## Status

Playable from boot through arcade mode. Not done yet: the Wii U GamePad's
screen and speaker, online play, the software keyboard and error viewer
screens, and audio effects the game does not seem to use. Only the European
version, update 16, is supported (other versions have different code).
Design notes, measurements and the milestone list are in
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## The game

You need **TEKKEN TAG TOURNAMENT 2 Wii U EDITION**, European release, with
update v16 installed, as a `.wua` archive (Cemu can create one from an
installed title), or as a folder with its `code/`, `content/` and `meta/`.

| | |
| --- | --- |
| Product code | WUP-P-AKNP |
| Title ID | `000500001010F800` |
| Version | 16 |
| `code/Tekken.rpx` SHA-256 | `fc0270465d384386f67804e48716e30bfbe85a5a1004e3a6bfe3ac75f9de5019` |

The build and the installer check the executable's hash and refuse any other
version.

## Requirements

- Linux on x86-64 with AVX2 and FMA (x86-64-v3: Intel Haswell, AMD Excavator
  or newer).
- A Vulkan 1.3 GPU and driver with `VK_EXT_external_memory_host` and push
  descriptors (current AMD, Intel and NVIDIA drivers).
- To build: Git, CMake 3.20+, Ninja, Clang (or GCC 15+), Python 3, and the
  development files of SDL3, shaderc, Vulkan (headers and loader), OpenSSL 3,
  zstd and zlib.

On Arch Linux:

```bash
sudo pacman -S --needed base-devel git cmake ninja clang python zstd zlib openssl vulkan-headers vulkan-icd-loader sdl3 shaderc
```

Other distributions have the same libraries under similar names (SDL3 needs a
recent release).

## Building

```bash
git clone <this repository> ttt2 && cd ttt2
python3 tools/setup_deps.py                      # ZArchive and AMD's addrlib, at pinned revisions

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build --target wua_extract cafe-recomp

# The game's executable and metadata (about 70 MB) into local/wua
build/wua_extract extract-analysis "/path/to/TEKKEN TAG 2 Wii U EDITION (EU).wua" local/wua
# Its code as C++ in generated/ttt2
build/recomp/cafe-recomp local/wua/000500001010f800_v16/code/Tekken.rpx generated/ttt2

cmake -B build -DTTT2_GENERATED_DIR=$PWD/generated/ttt2
cmake --build build --target ttt2
```

Recompiling takes a few seconds; compiling the generated code takes a few
minutes. `local/`, `generated/`, `third_party/` and `build*/` are ignored by
Git, so nothing derived from the game ends up in a commit.

## Playing

Run the game straight from its archive:

```bash
build/ttt2 "/path/to/TEKKEN TAG 2 Wii U EDITION (EU).wua"
```

Or install it, choosing the archive and the install folder in file dialogs
(or giving them as `--install GAME FOLDER`). This copies the game (16 GB)
and the program into that folder and adds **Tekken Tag Tournament 2** to the
application menu; the `.wua` is then no longer needed:

```bash
build/ttt2 --install
build/ttt2 --update      # after a rebuild: copies the new program into the install
```

Saves go to `~/.local/share/ttt2/save`. F11 toggles fullscreen.

| Wii U GamePad | Keyboard |
| --- | --- |
| D-pad | Arrow keys |
| A / B / X / Y | X / Z / S / A |
| L / R | Q / W |
| ZL / ZR | 1 / 2 |
| + / − | Enter / Backspace |
| HOME | H |
| Left stick | I / J / K / L |

Gamepads work through SDL, by button position.

| Environment variable | Effect |
| --- | --- |
| `TTT2_SCALE=1..4` | render scale (default from the display) |
| `TTT2_FPS=60/120/180/240` | frames shown per second (default 120 on displays of 100 Hz and more) |
| `TTT2_FULLSCREEN=1` | start fullscreen |
| `TTT2_AUDIO=0`, `TTT2_AUDIO_VOLUME=<percent>` | no sound; volume |
| `TTT2_SAVE_DIR=<folder>` | where saves go |

Debugging variables are listed in
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md#running-the-port).

## How it works

- `recomp/`: `cafe-recomp` loads the RPX, decodes every function of the
  game (79,564 reachable of 205,200) and writes each as a C++ function; guest
  calls are C++ calls, indirect calls go through an address table.
- `runtime/src/os/`: the Cafe OS libraries the game imports (threads,
  synchronisation, memory, file system, saves, input, sound mixing), written
  natively.
- `runtime/src/gx2/` and `runtime/src/gpu/`: GX2 writes real GPU command
  buffers, which a command processor executes; the Vulkan renderer
  (`runtime/src/gpu/vulkan/`) translates the GPU's shaders to SPIR-V
  (`runtime/src/latte/`), interpolates frames and paces presentation.
- `tests/`: a memory-model test, and differential tests that run every
  instruction form the game uses against Dolphin's PowerPC interpreter (see
  [Tests](#tests)).

## Tests

```bash
ctest --test-dir build
```

runs `memory_model_test`. The differential tests (`semantics_test`,
`control_flow_test`) need a Dolphin checkout in `third_party/ref/dolphin`
(the revision and sparse paths are in
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)), `fmt`, and the game's
`Tekken.rpx` in `local/wua`. They build Dolphin's interpreter (GPL-2.0 or
later) into the test programs; nothing of Dolphin is stored in this
repository.

## License

This repository's code is under the [MIT License](LICENSE). It does not cover
the game, or anything generated from it.

Fetched at build time, under their own licenses:
[ZArchive](https://github.com/Exzap/ZArchive) (MIT-0) and AMD's address
library ([decaf-emu/addrlib](https://github.com/decaf-emu/addrlib), MIT, which
is built into the `ttt2` program). The port also links SDL3 (zlib),
shaderc (Apache-2.0), the Vulkan loader (Apache-2.0), OpenSSL (Apache-2.0),
zstd (BSD) and zlib.

## Acknowledgements

- [Cemu](https://github.com/cemu-project/Cemu) and
  [decaf-emu](https://github.com/decaf-emu/decaf-emu), whose reverse
  engineering of the Wii U's system libraries and GPU documented much of the
  behaviour reimplemented here (structure layouts, register values,
  formats). No code was taken from them.
- [Dolphin](https://github.com/dolphin-emu/dolphin), whose hardware-tested
  PowerPC interpreter is the reference the instruction tests compare
  against, and whose hardware research measured the processor's estimate
  tables.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and
  [N64Recomp](https://github.com/N64Recomp/N64Recomp), for showing how far
  function-level static recompilation can go.
