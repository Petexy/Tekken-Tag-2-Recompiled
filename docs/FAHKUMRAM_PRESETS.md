# Fahkumram preset outfits

See [Imported preset outfits](IMPORTED_PRESET_OUTFITS.md) for current build commands, runtime invariants, texture allocation and validation.

Fahkumram's seven presets are declared in `tools/characters/ttt2char/characters/fahkumram.py`, in native chooser order. Their stable body aliases are `fka` through `fkg`. Meshes retain the base `nsc_*` naming convention; aliases select complete bodies, not additional fighters.

## Reproduce

Build the matching runtime, then generate a complete pack from your own installed games:

```bash
cmake --build build --target ttt2
python3 tools/characters/import_character.py fahkumram \
  --game /path/to/TTT2/game \
  --tekken7 "/path/to/TEKKEN 7" \
  --out /path/to/private/character-packs
```

Start with a private copy of existing character packs to preserve slot assignments and shared resources. Full import regenerates gameplay from your source configuration. Use matching runtime and importer versions and keep the generated pack together.

## Loading and regression checks

`sub_044BE1EC` records the selected body per player slot and invalidates a completed model cache when the outfit changes. `sub_0209506C` normalizes a private outfit lookup to the base fighter ID. `sub_02091898` supplies the selected body's strings while retaining the base ID and motion row.

The recipe hook must discard a remembered body when its incoming fighter differs from the body's owner. Returning to the main menu can reuse a slot without another loader call; applying the old body there caused a black screen.

Enable `TTT2_TRACE_ROSTER=1` and verify the chosen `fk*` XML/BON/NUD/MOP/NUT files are read while gameplay identity stays on Fahkumram. Test every preset, repeated outfit changes, both sides/team slots, mirror matches, and return to the main menu before another match. Player IDs and motion rows depend on installed pack slots; do not hardcode development log values.
