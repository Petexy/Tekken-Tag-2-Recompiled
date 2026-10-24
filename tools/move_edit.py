#!/usr/bin/env python3
"""Inspect and edit this project's imported-fighter overrides. Edits are dry-run by default."""
import argparse
import ast
import datetime
import difflib
import importlib
import json
import os
from pathlib import Path
import pprint
import shutil
import subprocess
import sys

HOME = Path.home()


def moveset_node(text):
    for node in ast.parse(text).body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'MOVESET' for t in node.targets):
            if not isinstance(node.value, ast.Dict):
                raise ValueError('MOVESET must be a dictionary literal')
            return node.value
    raise ValueError('No MOVESET dictionary found')


def offset(text, line, column):
    # Python AST columns are UTF-8 byte offsets, not character offsets.
    lines = text.splitlines(keepends=True)
    return sum(len(x) for x in lines[:line - 1]) + len(lines[line - 1].encode()[:column].decode())


def option(text, key, default):
    for k, v in zip(moveset_node(text).keys, moveset_node(text).values):
        if k is not None and ast.literal_eval(k) == key:
            return ast.literal_eval(v)
    return default


def replace_option(text, key, value):
    """Replace one option's literal; preserve all other source text and comments."""
    node = moveset_node(text)
    rendered = pprint.pformat(value, width=100, sort_dicts=False)
    for k, v in zip(node.keys, node.values):
        if k is not None and ast.literal_eval(k) == key:
            if ast.literal_eval(v) == value:
                return text
            a, b = offset(text, v.lineno, v.col_offset), offset(text, v.end_lineno, v.end_col_offset)
            result = text[:a] + rendered + text[b:]
            ast.parse(result)
            return result
    # Insert immediately after {, so no assumptions about the final trailing comma.
    at = offset(text, node.lineno, node.col_offset) + 1
    result = text[:at] + '\n    ' + repr(key) + ': ' + rendered + ',' + text[at:]
    ast.parse(result)
    return result


def backup(path, root):
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    dest = root / stamp / path.name
    dest.parent.mkdir(parents=True, exist_ok=False)
    if path.is_dir():
        shutil.copytree(path, dest)
    else:
        shutil.copy2(path, dest)
    print('Backup:', dest)
    return dest


def atomic_text(path, text):
    import tempfile
    fd, name = tempfile.mkstemp(prefix=path.name + '.', dir=path.parent)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as f:
            f.write(text)
        os.chmod(name, path.stat().st_mode & 0o777)
        os.replace(name, path)
        if path.suffix == '.py':
            # Same-second, same-size edits must not reuse stale definition bytecode.
            from importlib.util import cache_from_source
            Path(cache_from_source(str(path))).unlink(missing_ok=True)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def setup(args):
    repo = args.repo.expanduser().resolve()
    if not (repo / 'tools/characters/ttt2char').is_dir():
        raise ValueError('Repository not found: ' + str(repo))
    sys.path.insert(0, str(repo / 'tools/characters'))
    return repo


def definitions():
    from ttt2char.characters import CHARACTERS, definition
    return {name: definition(name) for name in CHARACTERS}


def native(args, fighter, explicit=None):
    from ttt2char.ttt2.archive import Archives
    from ttt2char.ttt2 import motbin
    game = args.game / 'game' if (args.game / 'game').is_dir() else args.game
    ar = Archives(str(game))
    if explicit is not None:
        if not 0 <= explicit < 58:
            raise ValueError('Native row must be between 0 and 57')
        return motbin.read(ar.entry('data006', explicit)), explicit
    normalize = lambda s: ''.join(c for c in s.lower() if c.isalnum())
    matches = []
    for row in range(58):
        m = motbin.read(ar.entry('data006', row))
        if normalize(m.name(m.name_offsets[0])) == normalize(fighter):
            matches.append((m, row))
    if len(matches) != 1:
        raise ValueError('Native fighter not uniquely found; use --row. Try fighters --native.')
    return matches[0]


def load(args):
    if args.source == 'native':
        return native(args, args.fighter, args.row)[0]
    from ttt2char.ttt2 import motbin
    from ttt2char.tekken7 import tkmvst
    ds = definitions()
    if args.fighter not in ds:
        raise ValueError('Use an imported fighter id from fighters')
    d = ds[args.fighter]
    if args.source == 'installed':
        return motbin.read((args.packs / d.CODE / (d.CODE + '.moveset')).read_bytes())
    from ttt2char.importer import find_movesets
    own, _ = find_movesets(str(args.movesets), d)
    return tkmvst.read(own)


def move_index(m, name):
    names = {m.move_name(i): i for i in range(len(m.tables['moves']))}
    if name not in names:
        import difflib
        hints = difflib.get_close_matches(name, names, n=5)
        raise ValueError('Exact move name not found: ' + name + ('; similar: ' + ', '.join(hints) if hints else ''))
    return names[name]


def details(m, name):
    from ttt2char.convert import idmaps as M
    i = move_index(m, name)
    v = m.tables['moves'][i]
    result = {'index': i, 'name': name, 'animation': m.anim_name(i),
              'fields': {k: x.hex() if isinstance(x, bytes) else x for k, x in v.items()},
              'hits': [], 'cancels': []}
    from ttt2char.movesets import REACTION_MOVE_FIELDS
    for req, damage, ri in M.hitconds(m, v['hit_condition']):
        rx = m.tables['reactions'][ri]
        reaction = {k: M.target(m, x) if k in REACTION_MOVE_FIELDS else x for k, x in rx.items()}
        push = {}
        for field in rx:
            if field.startswith('pb_') and rx[field] is not None:
                p = dict(m.tables['pushbacks'][rx[field]])
                ex = p['extradata']
                p['extras'] = m.tables['pushback_extras'][ex:ex + p['loops']] if ex is not None else []
                push[field] = p
        result['hits'].append({'requirements': req, 'damage': damage, 'reaction': reaction, 'pushbacks': push})
    for row in M.cancels(m, v['cancel']):
        entry = dict(row)
        entry['requirements'] = M.reqs(m, row['req'])
        entry['target'] = 'GROUP' if row['direction'] == M.GROUP else M.target(m, row['move_id'])
        entry['extra_value'] = m.tables['cancel_extras'][row['extradata']]['value'] if row['extradata'] is not None else None
        result['cancels'].append(entry)
    return result


def edit(args, repo):
    ds = definitions()
    if args.fighter not in ds:
        raise ValueError('Unknown imported fighter')
    d = ds[args.fighter]
    path = repo / 'tools/characters/ttt2char/characters' / (args.fighter + '.py')
    text = path.read_text()
    # Source moveset confirms exact names before changing configuration.
    args.source = 't7'
    m = load(args)
    idx = move_index(m, args.move)
    if args.command == 'bound' and args.state == 'on' and m.tables['moves'][idx]['first_active'] < 2:
        raise ValueError('This move has no supported active-frame attack; inspect its damaging follow-up first')
    engine_path = repo / 'tools/characters/ttt2char/convert/move_overrides.py'
    if not engine_path.exists():
        raise ValueError('This toolkit requires the local move_overrides converter additions; see the guide')
    if args.command == 'bound':
        key, val = 'bounds', args.state == 'on'
    elif args.command == 'damage':
        if not 0 <= args.amount <= 255:
            raise ValueError('Damage must be between 0 and 255')
        engine = (repo / 'tools/characters/ttt2char/convert/move_overrides.py').read_text()
        if "get('damage'" not in engine:
            raise ValueError('This checkout needs the damage override support supplied with this toolkit')
        key, val = 'damage', args.amount
    elif args.command == 'air-reaction':
        from ttt2char.convert import idmaps as M
        ref, row = native(args, args.from_fighter, args.from_row)
        rv = ref.tables['moves'][move_index(ref, args.from_move)]
        air = [h for h in M.hitconds(ref, rv['hit_condition']) if (466, 0) in h[0]]
        if len(air) != 1:
            raise ValueError('Reference move needs exactly one airborne reaction')
        target = m.tables['moves'][move_index(m, args.move)]
        if not any((614, 0) in h[0] for h in M.hitconds(m, target['hit_condition'])):
            raise ValueError('Target has no explicit T7 airborne branch; this command does not create one')
        donor, _ = native(args, '', d.DONOR_ROW)
        from ttt2char.movesets import REACTION_MOVE_FIELDS
        donor_names = {donor.move_name(i) for i in range(len(donor.tables['moves']))}
        rx = ref.tables['reactions'][air[0][2]]
        missing = {M.target(ref, rx[k]) for k in REACTION_MOVE_FIELDS} - donor_names
        if missing:
            raise ValueError('Reference uses reaction moves absent from this donor: ' + ', '.join(sorted(missing)))
        key, val = 'air_reactions', (row, args.from_move)
    elif args.command == 'tag':
        if d.DONOR_ROW != 38:
            raise ValueError('Electric-style tag exit currently supports Lars-based imports only')
        key = 'tag_launchers'
        mapping = option(text, key, {})
        if args.state == 'on':
            mapping[args.move] = 'La_Stormgear'
            text = replace_option(text, 'electric_tag_exit', True)
            text = replace_option(text, 'tag_exit_name', option(text, 'tag_exit_name', d.NAME.replace(' ', '_') + '_TagExit'))
        else:
            mapping.pop(args.move, None)
        air = list(option(text, 'tag_launcher_air_only', ()))
        air = [x for x in air if x != args.move]
        if args.state == 'on' and args.air_only:
            air.append(args.move)
        text = replace_option(text, 'tag_launcher_air_only', tuple(air))
        new = replace_option(text, key, mapping)
    if args.command != 'tag':
        mapping = option(text, key, {})
        mapping[args.move] = val
        new = replace_option(text, key, mapping)
    original = path.read_text()
    print(''.join(difflib.unified_diff(original.splitlines(True), new.splitlines(True), fromfile=str(path), tofile=str(path))), end='')
    if new == original:
        print('Already configured; nothing changed.')
    elif args.apply:
        backup(path, args.backups)
        atomic_text(path, new)
        print('Saved source override. Rebuild this fighter, then restart the game.')
    else:
        print('DRY RUN. Add --apply to save; no pack is rebuilt automatically.')


def rebuild_selected(args, repo):
    fighters = list(dict.fromkeys(args.fighters))
    available = definitions()
    unknown = [name for name in fighters if name not in available]
    if unknown:
        raise ValueError('Unknown fighter(s): ' + ', '.join(unknown) +
                         '. Use the fighters command to list supported names.')
    for fighter in fighters:
        d = available[fighter]
        if not args.apply:
            print('DRY RUN: rebuild', d.NAME, 'from saved T7 assets; backup and replace', args.packs / d.CODE)
            continue
        print('Rebuilding', fighter, flush=True)
        target = args.packs / d.CODE
        if target.exists():
            backup(target, args.backups)
        # Fresh interpreter per fighter; stop immediately if conversion fails.
        code = 'import sys;sys.path.insert(0,sys.argv[1]);from ttt2char import importer;importer.run(sys.argv[2],sys.argv[3],sys.argv[4],sys.argv[5],out=sys.argv[6],make_shared=False)'
        subprocess.run([sys.executable, '-u', '-c', code, str(repo / 'tools/characters'),
                        fighter, str(args.t7), str(args.movesets), str(args.game), str(args.packs)], check=True)
    if not args.apply:
        print('Close the game, then repeat with --apply. Existing overrides for each fighter are included.')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo', type=Path, default=HOME / 'Tekken-Tag-2-Ultimate-Edition')
    p.add_argument('--packs', type=Path, default=HOME / '.local/share/ttt2/characters')
    p.add_argument('--movesets', type=Path, default=HOME / '.local/share/ttt2/tekken7/movesets')
    p.add_argument('--game', type=Path, default=HOME / 'Games/TekkenTag2')
    p.add_argument('--t7', type=Path, default=HOME / '.local/share/Steam/steamapps/common/TEKKEN 7')
    p.add_argument('--backups', type=Path, default=HOME / '.local/state/ttt2-move-tools/backups')
    sub = p.add_subparsers(dest='command', required=True)
    q = sub.add_parser('fighters'); q.add_argument('--native', action='store_true')
    for cmd in ('find', 'show', 'find-input'):
        q = sub.add_parser(cmd); q.add_argument('fighter')
        q.add_argument('query' if cmd == 'find' else 'move', nargs='?' if cmd == 'find-input' else None, default=None)
        q.add_argument('--source', choices=('t7', 'installed', 'native'), default='t7')
        q.add_argument('--row', type=int)
        if cmd == 'find':
            q.add_argument('--regex', action='store_true'); q.add_argument('--limit', type=int, default=40)
        if cmd == 'find-input':
            q.add_argument('--direction', required=True, choices=('n','db','d','df','b','f','ub','u','uf'))
            q.add_argument('--buttons', required=True, help='1, 2, 3, 4 or combinations such as 1+2')
    for cmd in ('bound', 'damage', 'air-reaction', 'tag'):
        q = sub.add_parser(cmd);q.add_argument('fighter');q.add_argument('move');q.add_argument('--apply', action='store_true')
        if cmd in ('bound', 'tag'): q.add_argument('state', choices=('on','off'))
        if cmd == 'damage':q.add_argument('amount', type=int)
        if cmd == 'air-reaction':
            q.add_argument('--from-fighter', required=True);q.add_argument('--from-move', required=True);q.add_argument('--from-row', type=int)
        if cmd == 'tag':q.add_argument('--air-only', action='store_true')
    q = sub.add_parser('config');q.add_argument('fighter')
    q = sub.add_parser('rebuild');q.add_argument('fighters', nargs='+', help='one or more names from the fighters command');q.add_argument('--apply', action='store_true')
    a = p.parse_args()
    errors = (ValueError, KeyError, OSError, subprocess.CalledProcessError)
    try:
        repo = setup(a)
        from ttt2char import InputError
        errors += (InputError,)
        if a.command == 'fighters':
            if a.native:
                for row in range(58):
                    m, _ = native(a, '', row);print(row, m.name(m.name_offsets[0]))
            else:
                for name, d in definitions().items():print(name, d.CODE, d.NAME)
        elif a.command == 'config':
            print(pprint.pformat(definitions()[a.fighter].MOVESET, width=100, sort_dicts=False))
        elif a.command in ('bound','damage','air-reaction','tag'):
            edit(a, repo)
        elif a.command == 'rebuild':
            rebuild_selected(a, repo)
        else:
            from ttt2char.convert import idmaps as M
            m = load(a)
            if a.command == 'show':print(json.dumps(details(m,a.move),indent=2))
            elif a.command == 'find':
                import re
                hits = []
                for i in range(len(m.tables['moves'])):
                    n, anim = m.move_name(i), m.anim_name(i)
                    match = re.search(a.query, n+' '+anim, re.I) if a.regex else a.query.lower() in (n+' '+anim).lower()
                    if match:hits.append((i,n,anim))
                for hit in hits[:a.limit]:print(*hit,sep='\t')
                print(len(hits),'matches;',min(len(hits),a.limit),'shown')
            else:
                directions=dict(n=32,db=2,d=4,df=8,b=16,f=64,ub=128,u=256,uf=512)
                buttons=0
                for b in a.buttons.split('+'):
                    if b not in ('1','2','3','4'):raise ValueError('Buttons must be 1..4, joined by +')
                    buttons |= 1 << (int(b)-1)
                found=set()
                for table in ('cancels','group_cancels'):
                    for r in m.tables[table]:
                        if r['direction']==directions[a.direction] and r['button']&0xffff==buttons:
                            value=(M.target(m,r['move_id']),tuple(M.reqs(m,r['req'])))
                            if value not in found:print(value[0], 'requirements:', value[1]);found.add(value)
                print('Context matters: standing/FC/stance inputs can share buttons. Inspect the move and its follow-ups.')
    except errors as e:
        p.exit(1, 'Error: '+str(e)+'\n')

if __name__ == '__main__':
    main()
