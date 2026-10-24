"""Free meter is character-specific; Focus and fireball cancels keep their input semantics."""
import collections
import types
import unittest
import test_specialcancels as fixtures
from ttt2char.convert import specialcancels as S, tagsystem as TS, moveset


class FreeSpecials(unittest.TestCase):
    def converter(self):
        c = fixtures.SpecialCancels().converter()
        c.defn.MOVESET.update(buffered_special_cancels=True, free_meter_specials=True)
        c.T.update(move_begin_props=[], move_end_props=[], cancel_extras=[])
        c.extra_index = {}
        return c

    def test_meter_branches_are_only_added_for_free_meter_opt_in(self):
        c = self.converter()
        S.apply(c)
        rows = TS.cancels(c.T, c.T['moves'][0]['cancel'])
        dispatch = [r for r in rows if r['move_id'] == 1]
        self.assertEqual(len(dispatch), 2)  # ordinary and formerly meter-gated branch
        self.assertTrue(all(TS.reqs(c.T, r['req']) == [(307, 3), (308, 1)] for r in dispatch))
        c = self.converter()
        c.defn.MOVESET['free_meter_specials'] = False
        S.apply(c)
        rows = TS.cancels(c.T, c.T['moves'][0]['cancel'])
        self.assertEqual(sum(r['move_id'] == 1 for r in rows), 1)

    def test_focus_capture_accepts_buttons_without_a_motion_sequence(self):
        c = self.converter()
        c.k.tables['cancels'][0].update(direction=0, button=3)
        S.apply(c)
        rows = TS.cancels(c.T, c.T['moves'][0]['cancel'])
        capture = next(r for r in rows if (0x8093, 3) in TS.reqs(c.T, r['req']))
        self.assertEqual((capture['direction'], capture['button'], capture['start_frame']), (0, 3, 1))
        self.assertEqual(c.T['cancel_extras'][capture['extradata']]['value'], 0x2800)

    def test_fireball_cancel_does_not_require_a_hit_or_block(self):
        c = self.converter()
        c.defn.MOVESET['noncontact_special_cancels'] = ('normal',)
        # Remove hit tests to model T7's fireball -> Focus / EX Moon Glide.
        for row in c.k.tables['requirements']:
            if row['condition'] == 49:
                row.update(condition=629, param=500)
        S.apply(c)
        rows = TS.cancels(c.T, c.T['moves'][0]['cancel'])
        dispatch = [r for r in rows if r['move_id'] == 1]
        self.assertTrue(dispatch)
        self.assertTrue(all(TS.reqs(c.T, r['req']) == [(307, 3)] for r in dispatch))
        self.assertFalse(any(a in (46, 47, 308) for r in rows for a, _ in TS.reqs(c.T, r['req'])))

    def test_early_capture_cutoff_is_extended_through_late_dispatch(self):
        c = self.converter()
        c.defn.MOVESET['buffered_special_timing'] = {'extra_frames': 8}
        c.k.tables['cancels'][0]['win_end'] = 7
        S.apply(c)
        rows = TS.cancels(c.T, c.T['moves'][0]['cancel'])
        capture = next(r for r in rows if (0x8093, 3) in TS.reqs(c.T, r['req']))
        self.assertEqual(capture['win_end'], 25)




    def test_secondary_string_restores_four_with_original_timing(self):
        c = self.converter()
        c.defn.MOVESET = {'secondary_string_followups': {'normal': ('dp',)}}
        pairs=[(353,0),(0x80a9,1),(881,0),
               (352,0),(353,1),(881,0),(352,1),(353,1),(0x82ea,0),(881,0),(881,0)]
        c.k.tables['requirements']=[dict(condition=a,param=b) for a,b in pairs]
        rows=c.k.tables['cancels']
        rows[0].update(direction=0,button=0x40000008,req=0,win_start=1,win_end=20)
        rows[1].update(direction=0,button=0,req=3,win_start=20,win_end=20,start_frame=20)
        rows[2].update(direction=0,button=0,req=6,win_start=20,win_end=20,start_frame=20)
        rows[3]['req']=10
        S.apply(c)
        result=TS.cancels(c.T,c.T['moves'][0]['cancel'])[0]
        self.assertEqual((result['button'],result['win_start'],result['win_end'],result['start_frame']),
                         (0x40000008,1,20,20))
        self.assertEqual(result['move_id'],1)
        self.assertEqual(TS.reqs(c.T,result['req']),[])

    def test_jump_inputs_keep_ex_priority_and_stay_airborne(self):
        c = self.converter()
        c.kn = {'Vp_sJUMP_00_': 0, 'dp': 1}
        c.k.move_name = lambda i: ['Vp_sJUMP_00_', 'dp'][i]
        c.defn.MOVESET = {'jump_special_inputs': ('dp',), 'free_meter_specials': True}
        c.k.tables['moves'][0].update(airborne_start=9, airborne_end=43)
        c.T['moves'][0].update(airborne_start=9, airborne_end=45)
        c.k.tables['requirements'] = [dict(condition=a, param=b) for a, b in
                                      [(629, 500), (630, 0), (0x822d, 0), (881, 0)]]
        row = dict(direction=S.SEQ0, button=0, req=0, extradata=0,
                   win_start=9, win_end=41, start_frame=9, move_id=1, option=80)
        c.k.tables['cancels'] = [row, dict(row, req=1),
                                dict(row, direction=0x8000, req=3, move_id=0)]
        S.restore_jump_specials(c)
        rows = TS.cancels(c.T, c.T['moves'][0]['cancel'])
        self.assertEqual(len(rows), 2)  # duplicate rising/falling input coalesced
        self.assertEqual((rows[0]['win_start'], rows[0]['win_end']), (9, 43))
        self.assertEqual(TS.reqs(c.T, rows[0]['req']), [])
        self.assertEqual(rows[0]['direction'], S.SEQ0 + 5)

    def test_converter_removes_meter_gate_and_cost_only_when_requested(self):
        for free in (False, True):
            c = types.SimpleNamespace(defn=types.SimpleNamespace(MOVESET={'free_meter_specials': free}),
                k=types.SimpleNamespace(game='t7', tables={'requirements': [
                    dict(condition=629, param=500), dict(condition=0x81c2, param=0xfffffe0c),
                    dict(condition=881, param=0)]}), cache={}, T={'requirements': []},
                stats=collections.Counter(), map_req=lambda _: None)
            c.k.iter_list = types.MethodType(fixtures.Moveset.iter_list, c.k)
            _, ok = moveset.Converter.conv_reqs(c, 0)
            self.assertEqual(ok, free)
            if free:
                self.assertEqual(c.T['requirements'], [dict(condition=690, param=0)])
                self.assertIsNone(moveset.Converter.map_prop(c, 0x81c2))


if __name__ == '__main__':
    unittest.main()
