# Small move edits for Tekken Tag 2 Ultimate Edition

This guide covers the move override helpers in the character importer. The helper changes **source overrides**, then you rebuild the affected imported fighter. It does not edit Tekken 7's files or patch the running game's memory.

This checkout includes the required converter additions. The new toolkit itself does not change any move settings or installed character packs. Damage editing adds an opt-in converter feature; no damage changes are enabled by installing it.

## 1. Open a terminal and choose the toolkit

On this computer the main script is installed in the repository:

```bash
cd "$HOME/Tekken-Tag-2-Ultimate-Edition"
python tools/move_edit.py --help
python tools/move_edit.py fighters
```

Examples below assume that directory. Use `python3` instead of `python` if needed. The importer also needs the Python dependencies already used to generate your characters, including NumPy.

Global path options go **before the command**:

```bash
python tools/move_edit.py --repo /path/to/repo --game /path/to/TekkenTag2 fighters
```

Defaults: repository `~/Tekken-Tag-2-Ultimate-Edition`, installed packs `~/.local/share/ttt2/characters`, saved T7 movesets `~/.local/share/ttt2/tekken7/movesets`, Wii U files under `~/Games/TekkenTag2`, and T7 under Steam's default Linux library. Change these with `--repo`, `--packs`, `--movesets`, `--game`, and `--t7` when necessary.

## 2. Find the internal move name

The game buttons and the internal move names are different. Find the exact internal name before editing. Names are case-sensitive for edits; search is case-insensitive and checks animation names too.

```bash
python tools/move_edit.py find fahkumram 66rk
python tools/move_edit.py find fahkumram 'Nc_.*rp' --regex --limit 100
python tools/move_edit.py find fahkumram 66rk --source installed
```

Sources:

| Source | What you are inspecting |
|---|---|
| `t7` (default) | Original exported T7 moveset; use its exact move names for edits |
| `installed` | Generated TTT2 pack currently on disk, including your edits and donor moves |
| `native` | Original TTT2 fighter, useful as a reaction reference |

You can also trace a button input:

```bash
python tools/move_edit.py find-input Asuka --source native --direction db --buttons 4
python tools/move_edit.py show Asuka jun_mc02 --source native
```

The second command lists the follow-ups; button 3 leads to `Jn_mc03s`, Asuka's d/b+4,3 finisher. The names look like Jun because the native moveset shares that naming history.

Directions: `db d df b n f ub u uf`. Buttons: `1`, `2`, `3`, `4`, or combinations such as `1+2`. `find-input` searches exact single-direction/button rows. It is **not a full notation parser**: FC, stance, motion inputs, inherited groups, and buffered strings need inspection of their context and follow-ups. It cannot directly interpret a complete `FC d/f+3,2` string.

## 3. Inspect a move before changing it

```bash
python tools/move_edit.py show fahkumram Nc_66rklkrp --source installed
python tools/move_edit.py config fahkumram
```

`show` prints JSON containing active frames, hit conditions, damage, reaction names, pushback data, and cancel targets. Redirect it to a file for easier reading:

```bash
python tools/move_edit.py show fahkumram Nc_66rklkrp --source installed > "$HOME/fahk-hellsweep-elbow.json"
```

For hit requirements, native TTT2 `466` is the airborne branch and `467` is the Bound eligibility branch. T7 uses `614` for airborne. An empty final requirement list is the fallback branch. A cancel with direction `32779` is a group reference, not an ordinary directional input.

The reaction name alone does not determine distance. Pushback duration, displacement, extra values, reaction animation movement, and impact direction all contribute. Raw values are not a reliable distance in metres. Copying a native airborne reaction copies its full reaction and pushback data, resolving move names into the import's donor moveset.

## 4. Preview an edit, then save it

All editing commands default to a **dry run**. They print a source diff. Add `--apply` to save that edit. Saving does not rebuild anything automatically.

### Make a move Bound

```bash
python tools/move_edit.py bound fahkumram Nc_3rplp on
python tools/move_edit.py bound fahkumram Nc_3rplp on --apply
```

Remove Bound:

```bash
python tools/move_edit.py bound noctis Mz_t_srk03F off --apply
```

Bound uses the donor's native Bound reaction and eligibility check. The converter normally adds the Tag Assault twin and exit for a supported active bound attack. This is different from a launcher tag cancel. **One Bound per juggle** still applies. Some attack/projectile layouts need special handling; this helper rejects attacks without a supported active-frame start rather than pretending they are ready for Tag Assault.

### Copy an airborne reaction

This is the latest configuration for Fahk's elbow at the end of his hellsweep:

```bash
python tools/move_edit.py air-reaction fahkumram Nc_66rklkrp --from-fighter Asuka --from-move Jn_mc03s
python tools/move_edit.py air-reaction fahkumram Nc_66rklkrp --from-fighter Asuka --from-move Jn_mc03s --apply
```

Only the existing airborne hit branch changes. Grounded reactions, damage, and attack timing stay as configured. The command requires an explicit airborne branch on the target and exactly one on the native reference. It rejects references whose reaction moves do not exist in the import's donor, because those require additional conversion work.

List native reference fighters or override a row explicitly:

```bash
python tools/move_edit.py fighters --native
python tools/move_edit.py show Asuka Jn_mc03s --source native --row 23
```

Known reference rows: Asuka **23**, Kazuya **14**, Jinpachi **40**. The edit command accepts `--from-row` if the fighter name is ambiguous.

### Change damage

```bash
python tools/move_edit.py damage fahkumram Nc_66rklkrp 28
python tools/move_edit.py damage fahkumram Nc_66rklkrp 28 --apply
```

This example would change Fahk's elbow from 23 to 28; **it has not been applied**. The override changes damage on all hit-condition branches of that exact move, including conditional hits. It does not change the whole string or create EX-specific damage options. Valid values are integers from 0 through 255. Shared hit lists are copied so other moves are not changed accidentally.

### Add a launcher tag cancel

```bash
python tools/move_edit.py tag fahkumram Nc_66rklkrp on --air-only
python tools/move_edit.py tag fahkumram Nc_66rklkrp on --air-only --apply
```

Hold Tag for the hit to bring **your partner** in. `--air-only` adds the native airborne requirement. Omitting it permits the native hit/tag condition without that extra restriction. This is a launcher tag cancel, not Bound/Tag Assault; it does not grant an extra Bound.

```bash
python tools/move_edit.py tag fahkumram Nc_66rklkrp off --apply
```

The current electric-style exit helper is restricted to **Lars-based imported fighters**; it uses native tag requirements and an electric exit animation. Other donor skeletons/exits need additional work. This changes only the specified move's tag rule, although enabling the electric exit can also affect other explicitly configured tag launchers for that fighter. Review `config` first.

## 5. Rebuild only the fighter you edited

Close the game first. A source edit is not visible until regeneration, and an already-running game can retain its old pack.

```bash
python tools/move_edit.py rebuild fahkumram
python tools/move_edit.py rebuild fahkumram --apply
```

The first command previews the operation. The second backs up the existing pack and runs the importer from your saved T7 assets. It includes **all existing customizations** for that fighter. It does not start T7, re-export movesets, rebuild the native game executable, or regenerate the entire roster. The installed pack folder is written by the importer; if conversion fails, read the error before restarting and use the saved pack if needed.

To log a rebuild and monitor it in another terminal:

```bash
set -o pipefail
python -u tools/move_edit.py rebuild fahkumram --apply 2>&1 | tee "$HOME/fahk-rebuild.log"
```

```bash
tail -f "$HOME/fahk-rebuild.log"
```

After successful conversion, inspect the installed result, restart the game, and test grounded, airborne, wall, and tag behavior. File checks do not prove that the animation or gameplay feels right.

To rebuild a selected list, pass multiple fighter names (or a Bash array):

```bash
fighters=(akuma eliza fahkumram noctis kunimitsu2)
python3 tools/move_edit.py --repo "$PWD" --game "$HOME/Tekken Tag Tournament 2/game" rebuild "${fighters[@]}"
python3 tools/move_edit.py --repo "$PWD" --game "$HOME/Tekken Tag Tournament 2/game" rebuild "${fighters[@]}" --apply
```

Use your actual installed game path. `fighters` lists the supported names.
All names are validated before any backup or rebuild. Duplicate names run once,
in the supplied order. Rebuilds run sequentially, back up each existing pack,
and stop on the first failure; earlier successful rebuilds remain installed.
The single-fighter form still works.

## 6. Backups and undo

Every applied source edit prints its backup location under:

```text
~/.local/state/ttt2-move-tools/backups/TIMESTAMP/fahkumram.py
```

Rebuilds back up the old character folder in the same backup root, for example `TIMESTAMP/nsc`. Dry runs make no backup and write nothing. Existing unrelated source text and options are preserved; formatting within the option being changed may be reformatted. A replacement file is written atomically.

To undo a value, set the old value and rebuild again. To remove Bound, use `off`; to remove a tag cancel, use `off`. For damage/reaction override removal, restore the source backup or remove that move's entry from the corresponding `MOVESET` dictionary. Restoring a whole source file also restores all other settings from that moment, so check later changes before copying it over.

To restore a whole pack, keep the newer pack separately, then copy the saved character directory back to the original location. This avoids merging old and new pack files. These are templates—replace the backup timestamp and suffix with your actual paths:

```bash
mv "$HOME/.local/share/ttt2/characters/nsc" "$HOME/.local/share/ttt2/characters/nsc-before-restore"
cp -a "$HOME/.local/state/ttt2-move-tools/backups/TIMESTAMP/nsc" "$HOME/.local/share/ttt2/characters/nsc"
```

Restart after restoring. A later rebuild will use the source settings again, so restore source too if you want the rollback to persist.

## 7. Your useful move-name reference

| Fighter / move | Internal name |
|---|---|
| Fahk's elbow (hellsweep finisher): FC d/f+3,2; ff+4~3,2; d/f+1,4,3,2 | `Nc_66rklkrp` |
| Fahk b+1 | `Nc_lp00B` |
| Fahk d+4 (generic name, own animation `ncss_2rk`) | `Co_t_srk00` |
| Fahk running 3 (own animation `ncss_mach`) | `Co_runlk00` |
| Fahk f+3, first hit | `Nc_6lk` |
| Fahk d/f+2,1 | `Nc_3rplp` |
| Fahk d/f+1,4~1 / ff+4~1 shared follow-up | `Nc_66rklp` |
| Noctis d/f+4,2,2 finisher | `Mz_t_srk02F` |
| Noctis d/f+4,2,4 finisher | `Mz_t_srk03F` |
| Kuni f+4 | `Kt_rk00F` |
| Kuni SET entry | `Kt_shoryu02` |
| Asuka d/b+4,3 finisher (native row 23) | `Jn_mc03s` |
| Jinpachi u+4 (native row 40) | `J8_jprk00` |

Editing a shared internal move affects all routes that reach it. That is why changing Fahk's elbow changes all those hellsweep routes.

The converter also restores native TTT2 grounded back-roll routes when Tekken 7-only recovery targets would otherwise be dropped. This preserves the original orientation requirements for face-up/face-down recovery instead of forcing every position through the same roll.

The player confirmed Fahk's f+3 string and Kuni's f+4~F SET transition during Tag Assault after the continuation fixes. Keep the buffer normalization and Kuni stance overrides when merging upstream. Avoid blanket cancel-window/flag changes as a general editing method; those can change unrelated behavior.

## 8. Sharing changes

Commit the source overrides and converter changes, not generated character packs or game assets. Other players must regenerate their own packs from their own game files. After pulling updates, inspect the configuration and rebuild only the affected imports.

The optional shell wrappers in `tools/` run the same CLI:

```bash
bash tools/find_move.sh fahkumram 66rk
bash tools/inspect_move.sh fahkumram Nc_66rklkrp --source installed
bash tools/make_bound.sh fahkumram Nc_3rplp
bash tools/rebuild_fighter.sh fahkumram --apply
```

Use the main Python script when overriding global paths. The helper's source-edit tests are in `tools/characters/tests/test_move_edit_tool.py`.
