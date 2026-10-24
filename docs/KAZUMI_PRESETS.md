# Kazumi preset outfits

The current runtime supports all 17 imported fighters; see [Imported preset outfits](IMPORTED_PRESET_OUTFITS.md).

Kazumi uses the same complete-body selection path as Fahkumram, with the additional tiger skeleton checks described below.

## Preset order

| Menu entry | Source suffix | Body code |
| --- | --- | --- |
| 1 | DefaultCos1P | kza |
| 2 | DefaultCos2P | kzb |
| 3 | 1p_c2 | kzc |
| 4 | dancer | kzd |
| 5 | dancer_silver | kze |
| 6 | dougi | kzf |
| 7 | lycoris | kzg |
| 8 | lycoris_c1 | kzh |
| 9 | taisyo_roman | kzi |

The sets come from `/Game/Character/Item/Sets/KZM/CS_KZM_<suffix>` in the player's T7 installation. Each has a matching preview asset. These labels describe the source; the native menu uses pictures.

## Conversion and reproduction

See [Imported preset outfits](IMPORTED_PRESET_OUTFITS.md) for current runtime and importer build commands. Import `kazumi` using the same process as [Fahkumram](FAHKUMRAM_PRESETS.md), preserving existing pack slot assignments and shared resources in a private output directory.

Every body includes the tiger through the normal model converter. The preset builder compares tiger bone indices, names, parents and bind transforms with the base skeleton. Each MOP contains the tiger's no-spring entries plus any supported hanging-chain entries. Presets use the current 14-entry chooser limit and 32-bit registry namespace; `kza` through `kzi` remain stable across roster ordering changes.

## Validation

Test all nine presets, both sides/team slots, repeated outfit changes, mirror matches, tiger attacks and intro/win poses, fly stance during Tag Assault, and returning to the main menu. With `TTT2_TRACE_ROSTER=1`, selected `kz*` body files should load with Kazumi's base player ID and motion row. Numeric identities depend on the installed pack slots.
