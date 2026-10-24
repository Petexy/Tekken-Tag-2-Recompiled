"""Shared-attack reachability and imported-owner checks, using synthetic movesets."""
import collections
import os
import sys
import unittest
import types

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from ttt2char.convert import idmaps as M, moveset, specialcancels as S, tagsystem as TS
from ttt2char.movesets import Moveset


def source(names, reqs=()):
    k = Moveset()
    k.game, k.character_id = 't7', 44
    k.namebuf = b''
    offsets = []
    for name in names:
        offsets.append(len(k.namebuf))
        k.namebuf += name.encode() + b'\0'
    k.aliases = [0] * 56
    k.tables = dict(moves=[dict(name=o) for o in offsets], requirements=[
        dict(condition=a, param=b) for a, b in reqs] + [dict(condition=881, param=0)],
        input_sequences=[], cancels=[])
    return k


def converter(k):
    c = moveset.Converter.__new__(moveset.Converter)
    c.k, c.k_id = k, k.character_id
    c.newid, c.stats, c.cache = 80, collections.Counter(), {}
    c.newidx, c.dn, c.delta_move, c.react_remap = {}, {}, {}, {}
    c.cls, c.queue, c.alloc_ok = ['T'] * len(k.tables['moves']), [], 0
    c.T = dict(moves=[], requirements=[])
    c.conv_move = lambda i: dict(name=k.tables['moves'][i]['name'])
    c.is_state = lambda i: False
    c.map_req = lambda n: {0: 0, 217: 200, 218: 201, 219: 202, 223: 206, 881: 690}.get(n)
    c.extradata = lambda i: i
    return c


class SharedAttacks(unittest.TestCase):
    def test_crouch_jump_and_throw_targets_are_imported_from_retained_inputs(self):
        names = ['Co_syalp00', 'Co_syarp00', 'Co_syalk00', 'Co_syalkF', 'Co_t_slp00', 'Co_t_srk00',
                 'Co_jplk00', 'Co_jprk00', 'Co_tp_Lxxxxn']
        for name in names:
            with self.subTest(move=name):
                k = source([name]); c = converter(k)
                self.assertIsNone(c.target(0))
                target = c.target(0, input_target=True)
                self.assertEqual(target, 0)
                self.assertEqual(c.queue, [0])
                self.assertEqual(c.target(0, input_target=True), target)
                self.assertEqual(len(c.T['moves']), 1)

    def test_native_counterpart_has_priority(self):
        c = converter(source(['Co_syalp00']))
        c.dn['Co_syalp00'] = 42
        self.assertEqual(c.target(0, input_target=True), 42)
        self.assertEqual(c.queue, [])

    def test_unsupported_and_item_moves_are_not_enabled_by_input_conversion(self):
        for name in ['Co_IFishGun', 'Wi_IFire', 'Mt_IWeapon', 'Story_foo', 'b7_foo', 'Foo_RageArt',
                     'Foo_Balcony', 'FACE_foo', 'vra_foo', 'Foo_EX', 'Foo_EX2']:
            with self.subTest(move=name):
                c = converter(source([name]))
                self.assertIsNone(c.target(0, input_target=True))
                self.assertEqual(c.queue, [])

    def test_cancel_list_retains_input_window_and_target_zero(self):
        k = source(['Co_syalp00'])
        row = dict(direction=0, button=0x40000001, req=0, extradata=0, win_start=3, win_end=17,
                   start_frame=5, move_id=0, option=0)
        k.tables['cancels'] = [row, dict(row, direction=M.LIST_END)]
        c = converter(k)
        result = c.conv_cancel_list(0)
        self.assertEqual(result[0]['move_id'], 0)
        self.assertEqual((result[0]['win_start'], result[0]['win_end'], result[0]['start_frame']), (3, 17, 5))
        self.assertEqual(len(result), 2)


class OwnerCharacterChecks(unittest.TestCase):
    def test_owner_checks_fold_for_every_import_slot_including_native_family_selectors(self):
        for row in [58, 64, 61, 62, 65] + list(range(69, 84)) + [84, 127, 191]:
            with self.subTest(row=row):
                c = converter(source(['Mz_yokoRP'], [(217, 44)])); c.newid = row
                index, ok = c.conv_reqs(0)
                self.assertTrue(ok)
                self.assertEqual(c.T['requirements'][index:], [dict(condition=690, param=0)])

    def test_other_owner_and_negation_have_opposite_outcomes(self):
        for kind, param, expected in [(217, 25, False), (218, 44, False), (218, 25, True)]:
            with self.subTest(kind=kind, param=param):
                c = converter(source(['x'], [(kind, param)]))
                self.assertEqual(c.conv_reqs(0)[1], expected)

    def test_opponent_check_is_not_folded_true(self):
        c = converter(source(['x'], [(219, 44)]))
        index, ok = c.conv_reqs(0)
        self.assertTrue(ok)
        self.assertEqual(c.T['requirements'][index], dict(condition=202, param=80))

    def test_additional_condition_still_gates_the_move(self):
        c = converter(source(['x'], [(217, 44), (157, 0)]))
        self.assertFalse(c.conv_reqs(0)[1])


class NormalStrings(unittest.TestCase):
    def converter(self, reqs, direction=0):
        k = source(['jab', 'followup'], reqs)
        row = dict(direction=direction, button=0x40000001, req=0, extradata=0, win_start=7,
                   win_end=12, start_frame=3, move_id=1, option=0)
        end = dict(row, direction=M.LIST_END, button=0)
        k.tables['cancels'] = [row, end]
        k.tables['moves'][0]['cancel'] = 0
        k.tables['moves'][1]['cancel'] = 1
        c = converter(k)
        c.kn, c.newidx = {'jab': 0, 'followup': 1}, {0: 0, 1: 1}
        c.defn = types.SimpleNamespace(MOVESET={})
        c.extra_index = {}
        c.T.update(moves=[dict(cancel=0), dict(cancel=0)], cancels=[end], cancel_extras=[])
        return c

    def test_empty_buffer_normal_followup_keeps_original_window(self):
        c = self.converter([(346, 0)])
        S.restore_unbuffered_normal_strings(c)
        rows = TS.cancels(c.T, c.T['moves'][0]['cancel'])
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0]['move_id'], 1)
        self.assertEqual((rows[0]['win_start'], rows[0]['win_end'], rows[0]['start_frame']), (7, 12, 3))
        self.assertEqual(TS.reqs(c.T, rows[0]['req']), [])

    def test_contact_followup_requires_hit_or_block(self):
        c = self.converter([(49, 0), (346, 0)])
        S.restore_unbuffered_normal_strings(c)
        rows = TS.cancels(c.T, c.T['moves'][0]['cancel'])
        self.assertEqual([TS.reqs(c.T, row['req']) for row in rows[:-1]], [[(46, 0)], [(47, 0)]])

    def test_meter_nonempty_buffer_and_motion_captures_stay_excluded(self):
        for reqs, direction in [([(346, 0), (629, 500)], 0), ([(346, 3)], 0),
                                ([(346, 0), (0x80c5, 3)], S.SEQ0), ([(346, 0)], S.SEQ0)]:
            with self.subTest(reqs=reqs, direction=direction):
                c = self.converter(reqs, direction)
                S.restore_unbuffered_normal_strings(c)
                self.assertEqual(c.T['moves'][0]['cancel'], 0)
                self.assertEqual(len(c.T['cancels']), 1)

    def test_explicit_buffered_special_source_keeps_its_priority_rules(self):
        c = self.converter([(346, 0)])
        c.defn.MOVESET = dict(buffered_special_cancels=True, special_cancels={'jab': ('followup',)})
        S.restore_unbuffered_normal_strings(c)
        self.assertEqual(len(c.T['cancels']), 1)

    def hit_latched_converter(self):
        c = self.converter([(353, 1)])
        capture_req = len(c.k.tables['requirements'])
        c.k.tables['requirements'] += [dict(condition=a, param=b) for a, b in
                                       [(43, 0), (0x80a9, 1), (881, 0)]]
        dispatch = dict(c.k.tables['cancels'][0], win_start=16, win_end=16, start_frame=16)
        capture = dict(dispatch, req=capture_req, button=0, win_start=15, win_end=15, start_frame=15)
        c.k.tables['cancels'] = [capture, dispatch, c.k.tables['cancels'][1]]
        c.k.tables['moves'][1]['cancel'] = 2
        c.T['move_begin_props'] = []
        S.restore_hit_latched_normal_strings(c)
        return c

    def test_just_frame_normal_followup_latches_hit_and_keeps_one_frame_window(self):
        c = self.hit_latched_converter()
        rows = TS.cancels(c.T, c.T['moves'][0]['cancel'])
        self.assertEqual((rows[1]['win_start'], rows[1]['win_end'], rows[1]['start_frame']), (16, 16, 16))
        self.assertEqual(c.T['cancel_extras'][rows[0]['extradata']]['value'], 0x2800)
        for hit, pressed, expected in [(True, 16, True), (False, 16, False), (True, 17, False)]:
            with self.subTest(hit=hit, pressed=pressed):
                state, fired = 0, False
                for frame in (15, 16, 17):
                    for row in rows[:-1]:
                        if not row['win_start'] <= frame <= row['win_end']:
                            continue
                        if row['button'] and frame != pressed:
                            continue
                        req = TS.reqs(c.T, row['req'])
                        if (47, 0) in req and not (hit and frame == 15):
                            continue
                        if (S.CONTACT_VAR, 1) in req and state != 1:
                            continue
                        if (S.CONTACT_SET, 1) in req:
                            state = 1
                        elif row['button']:
                            fired = True
                self.assertEqual(fired, expected)

    def test_just_frame_normal_clears_previous_contact_on_entry(self):
        c = self.hit_latched_converter()
        entry = c.T['move_begin_props'][c.T['moves'][0]['begin_props']]
        self.assertEqual((entry['id'], entry['value']), (S.CONTACT_SET, 0))
        self.assertEqual(TS.reqs(c.T, entry['req']), [])


@unittest.skipUnless(os.environ.get('TTT2_TEST_GAME') and os.environ.get('TTT2_TEST_MOVESETS'),
                     'set TTT2_TEST_GAME and TTT2_TEST_MOVESETS for the local 17-fighter regression')
class SavedRosterCoverage(unittest.TestCase):
    def test_all_imported_fighters(self):
        from ttt2char import pack
        from ttt2char.characters import CHARACTERS, definition
        from ttt2char.tekken7 import tkmvst
        from ttt2char.ttt2 import motbin
        from ttt2char.ttt2.archive import Archives
        ar = Archives(os.environ['TTT2_TEST_GAME'])
        self.addCleanup(lambda: [stream.close() for stream in ar._files.values()])
        saved = tkmvst.saved_movesets(os.environ['TTT2_TEST_MOVESETS'])
        h7 = tkmvst.read(saved[8])
        native = [motbin.read(ar.entry('data006', i)) for i in range(58)]
        usage, states = M.ttt2_usage(native), TS.TagStates(native)
        packs, common = {}, ar.entry('data004', 0)
        expected = {'kazumi': ['Co_jplk00', 'Co_jplk00B', 'Co_t_srk00'],
         'chloe': ['Co_Dwn_rk00'],
         'eliza': ['Co_t_srk00'],
         'claudio': ['Co_Dwn_rk00', 'Co_jplp00F', 'Co_syalkF', 'Co_syarp00'],
         'akuma': ['Co_t_srk00'],
         'gigas': ['Co_Dwn_rk00', 'Co_syalk00', 'Co_syalkF', 'Co_syalp00', 'Co_syarp00', 'Co_t_slp00'],
         'josie': ['Co_Dwn_rk00',
                   'Co_btp_Lxxxxn',
                   'Co_btp_Rxxxxn',
                   'Co_jplk00',
                   'Co_jplk00B',
                   'Co_jprk00',
                   'Co_jprk00B',
                   'Co_sjplk11F',
                   'Co_sjplk12F',
                   'Co_sjplk13F',
                   'Co_syalk00',
                   'Co_syalp00',
                   'Co_t_slp00',
                   'Co_tp_Lxxxxn',
                   'Co_tp_Rxxxxn'],
         'katarina': ['Co_t_srk00'],
         'master_raven': ['Co_Dwn_rk00',
                          'Co_btp_Lxxxxn',
                          'Co_btp_Rxxxxn',
                          'Co_runlk00',
                          'Co_sjplk11F',
                          'Co_sjplk12F',
                          'Co_sjplk13F',
                          'Co_syalp00',
                          'Co_t_slp00',
                          'Co_tp_Lxxxxn',
                          'Co_tp_Rxxxxn'],
         'shaheen': ['Co_Dwn_rk00',
                     'Co_jplp00F',
                     'Co_syalk00',
                     'Co_syalkF',
                     'Co_syalp00',
                     'Co_syarp00',
                     'Co_t_slp00'],
         'fahkumram': ['Co_jplk00',
                       'Co_jplk00B',
                       'Co_jprk00',
                       'Co_jprk00B',
                       'Co_syalk00',
                       'Co_syalkF',
                       'Co_syalp00',
                       'Co_t_slp00'],
         'leroy': [],
         'lidia': ['Co_btp_Lxxxxn',
                   'Co_btp_Rxxxxn',
                   'Co_jplk00B',
                   'Co_jprk00B',
                   'Co_syalk00',
                   'Co_syalp00',
                   'Co_t_slp00',
                   'Co_t_srk00',
                   'Co_tp_Lxxxxn',
                   'Co_tp_Rxxxxn'],
         'kunimitsu2': ['Co_Dwn_rk00',
                        'Co_btp_Lxxxxn',
                        'Co_btp_Rxxxxn',
                        'Co_syalp00',
                        'Co_t_slp00',
                        'Co_tp_Lxxxxn',
                        'Co_tp_Rxxxxn',
                        'Thrmv_B2_09'],
         'negan': ['Co_Dwn_rk00',
                   'Co_jplk00',
                   'Co_jplk00B',
                   'Co_jplp00F',
                   'Co_jprk00',
                   'Co_jprk00B',
                   'Co_syalp00',
                   'Co_syarp00',
                   'Co_t_slp00'],
         'geese': [],
         'noctis': []}
        side_attacks = {'gigas': 'Cz_yoko_rp', 'fahkumram': 'Nc_yokork', 'leroy': 'Nb_SideRK00',
                        'lidia': 'Nd_yokorp', 'kunimitsu2': 'Kt_SideRP', 'noctis': 'Mz_yokoRP'}
        strings = {'eliza': ['Vp_lp00_A', 'Vp_lp00_B', 'Vp_lplp', 'Vp_lprp'],
                   'geese': ['My_lplp', 'My_lprp00', 'My_rp03F'], 'akuma': ['Mx_66rplp2']}
        for slot, name in enumerate(CHARACTERS):
            with self.subTest(fighter=name):
                d = definition(name)
                k = tkmvst.read(saved[d.TEKKEN7_ID])
                evidence = collections.defaultdict(M.Evidence)
                for who, row in d.ALIGN:
                    M.align(k if who == 'self' else h7, native[row], evidence)
                req, prop = M.build_maps(evidence)
                maps = req, prop, M.reaction_remap(evidence), M.hitbox_remap(evidence)
                if d.DONOR_ROW not in packs:
                    packs[d.DONOR_ROW] = ar.entry('data005', d.DONOR_ROW)
                c = moveset.Converter(k, h7, native[d.DONOR_ROW], packs[d.DONOR_ROW], native[13], common,
                                      maps, usage, pack.place(slot)['row'], d)
                c.tag_states, c.win_tones = states, {}
                if d.MOVESET.get('electric_tag_exit'):
                    c.electric_pack = packs.get(13) or ar.entry('data005', 13)
                c.air_reaction_references = {spec[0]: native[spec[0]]
                    for spec in d.MOVESET.get('air_reactions', {}).values() if isinstance(spec, tuple)}
                if 'air_reaction_reference' in d.MOVESET:
                    c.air_reaction_reference = native[d.MOVESET['air_reaction_reference']]
                if d.MOVESET.get('stance_reference'):
                    c.stance_reference = native[d.MOVESET['stance_reference'][0]]
                c.run()
                for target in expected[name]:
                    self.assertIsNotNone(c.target(c.kn[target]), name + ': ' + target)
                input_rows = list(c.T['cancels']) + list(c.T['group_cancels'])
                for target in strings.get(name, []):
                    mid = c.newidx[c.kn[target]]
                    self.assertTrue(any(r['button'] and r['move_id'] == mid for r in input_rows), target)
                if name in side_attacks:
                    target = side_attacks[name]
                    mid = c.newidx[c.kn[target]]
                    rows = TS.cancels(c.T, c.T['moves'][c.dn['sSTEP_01L']]['cancel'])
                    side = [r for r in rows if r['move_id'] == mid and r['button'] == 0x40000002
                            and r['direction'] == 0x20]
                    # SS4 fighters use button 4 rather than button 2.
                    if name in ('fahkumram', 'leroy'):
                        side = [r for r in rows if r['move_id'] == mid and r['button'] == 0x40000008
                                and r['direction'] == 0x20]
                    self.assertTrue(side, target)
                    for row in side:
                        self.assertNotIn((200, c.newid), TS.reqs(c.T, row['req']))


if __name__ == '__main__':
    unittest.main()
