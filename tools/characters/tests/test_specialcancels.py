"""Regression checks for Akuma's opt-in special and tag-launch cancels."""
import collections
import os
import sys
import types
import unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from ttt2char.movesets import Moveset
from ttt2char.convert import specialcancels as S, idmaps as M, tagsystem as TS


class SpecialCancels(unittest.TestCase):
    def converter(self):
        c = types.SimpleNamespace()
        c.defn = types.SimpleNamespace(MOVESET={'special_cancels': {'normal': ('dp',)}})
        c.kn = {'normal': 0, 'dp': 1}
        c.newidx = {0: 0, 1: 1}
        req = [{'condition': a, 'param': b} for a, b in
               [(346, 0), (0x80c5, 3), (M.END_REQ['t7'], 0),
                (49, 0), (346, 3), (M.END_REQ['t7'], 0),
                (629, 500), (49, 0), (346, 3), (M.END_REQ['t7'], 0),
                (M.END_REQ['t7'], 0)]]
        def row(direction, rq, target):
            return dict(direction=direction, button=0, req=rq, extradata=0,
                        win_start=1, win_end=17, start_frame=17, move_id=target, option=0)
        c.k = types.SimpleNamespace(tables={'moves': [{'cancel': 0}, {'cancel': 0}],
                                          'requirements': req, 'input_sequences': [{}],
                                          'cancels': [row(S.SEQ0, 0, 0), row(0, 3, 1),
                                                      row(0, 6, 1), row(M.LIST_END, 10, 0)]},
                                    game='t7', resolve_alias=lambda i: i,
                                    move_name=lambda i: ['normal', 'dp'][i])
        c.k.iter_list = types.MethodType(Moveset.iter_list, c.k)
        end = row(M.LIST_END, 0, 0)
        c.extra_index = {0x182: 7}
        c.T = {'moves': [{'cancel': 0}, {'cancel': 0}], 'cancels': [end], 'requirements': []}
        c.stats = collections.Counter()
        def add(table, value):
            c.T[table].append(value)
            return len(c.T[table]) - 1
        c.add = add
        c.conv_seq = lambda i: i + 5
        c.extradata = lambda i: 7
        return c

    def test_contact_gated_inputs_and_ex_branches(self):
        c = self.converter()
        S.apply(c)
        rows = TS.cancels(c.T, c.T['moves'][0]['cancel'])
        self.assertEqual(len(rows), 3)  # two contact gates, no extra EX branch
        self.assertEqual([TS.reqs(c.T, row['req']) for row in rows[:2]],
                         [[(46, 0)], [(47, 0)]])
        for row in rows[:2]:
            self.assertEqual((row['direction'], row['move_id'], row['start_frame']), (S.SEQ0 + 5, 1, 1))
        self.assertEqual(rows[-1]['direction'], M.LIST_END)
        self.assertEqual(c.T['cancels'][0]['direction'], M.LIST_END)  # shared list untouched

    def test_leniency_extends_only_opted_in_cancel_sequences(self):
        c = self.converter()
        c.defn.MOVESET['special_cancel_leniency'] = {'normal': 6}
        c.cache = {}
        c.T['input_sequences'] = [dict(window=13, inputs=0, amount=3)] * 6
        S.apply(c)
        rows = TS.cancels(c.T, c.T['moves'][0]['cancel'])[:2]
        self.assertTrue(all(row['win_end'] == 23 for row in rows))
        self.assertTrue(all(row['start_frame'] == 1 for row in rows))
        self.assertEqual(rows[0]['direction'], rows[1]['direction'])
        self.assertEqual(c.T['input_sequences'][rows[0]['direction'] - S.SEQ0]['window'], 19)
        self.assertEqual(c.T['input_sequences'][5]['window'], 13)
        self.assertEqual([TS.reqs(c.T, row['req']) for row in rows], [[(46, 0)], [(47, 0)]])

    def test_native_tag_exit_precedes_automatic_followups(self):
        c = self.converter()
        c.defn.MOVESET = {'tag_launchers': {'dp': 'launcher'}}
        c.d = c.k
        c.dn = {'launcher': 0}
        c.d.tables['requirements'] = [dict(condition=a, param=b) for a, b in
                                      [(47, 0), (481, 0), (M.END_REQ['t7'], 0)]]
        tag = dict(direction=0, button=0, req=0, extradata=188,
                   win_start=1, win_end=16, start_frame=1, move_id=7, option=0)
        c.d.tables['cancels'] = [tag, dict(tag, direction=M.LIST_END, req=2)]
        c.d.tables['moves'][0]['last_active'] = 16
        c.T['moves'][1]['last_active'] = 13
        c.h2 = types.SimpleNamespace(tables={'moves': [{'cancel': 0, 'last_active': 8}],
                                             'requirements': c.d.tables['requirements'],
                                             'cancels': [dict(tag, win_end=16), dict(tag, direction=M.LIST_END, req=2)]},
                                      game='t7', move_name=lambda i: 'He_shoryu_S')
        c.h2.iter_list = types.MethodType(Moveset.iter_list, c.h2)
        S.apply(c)
        rows = TS.cancels(c.T, c.T['moves'][1]['cancel'])
        self.assertEqual((rows[0]['move_id'], rows[0]['extradata'], rows[0]['win_end']), (7, 188, 21))
        self.assertEqual(rows[-1]['direction'], M.LIST_END)
        self.assertEqual(c.stats['tag launcher cancels added'], 1)

    def test_air_only_tag_launcher_requires_airborne_contact(self):
        c = self.converter()
        c.defn.MOVESET = {'tag_launchers': {'dp': 'launcher'}, 'tag_launcher_air_only': ('dp',)}
        c.d = c.k
        c.dn = {'launcher': 0}
        c.d.tables['requirements'] = [dict(condition=a, param=b) for a, b in
                                      [(47, 0), (481, 0), (M.END_REQ['t7'], 0)]]
        tag = dict(direction=0, button=0, req=0, extradata=188,
                   win_start=1, win_end=16, start_frame=1, move_id=7, option=0)
        c.d.tables['cancels'] = [tag, dict(tag, direction=M.LIST_END, req=2)]
        c.d.tables['moves'][0]['last_active'] = 16
        c.T['moves'][1]['last_active'] = 13
        c.h2 = types.SimpleNamespace(tables={'moves': [{'cancel': 0, 'last_active': 8}],
                                             'requirements': c.d.tables['requirements'],
                                             'cancels': [dict(tag, win_end=16), dict(tag, direction=M.LIST_END, req=2)]},
                                      game='t7', move_name=lambda i: 'He_shoryu_S')
        c.h2.iter_list = types.MethodType(Moveset.iter_list, c.h2)
        S.apply(c)
        rows = TS.cancels(c.T, c.T['moves'][1]['cancel'])
        self.assertEqual((rows[0]['move_id'], rows[0]['extradata'], rows[0]['win_end']), (7, 188, 21))
        self.assertEqual(TS.reqs(c.T, rows[0]['req']), [(47, 0), (481, 0), (466, 0)])
        self.assertEqual(rows[-1]['direction'], M.LIST_END)
        self.assertEqual(c.stats['tag launcher cancels added'], 1)

    def test_missing_input_fails_instead_of_silently_omitting_fix(self):
        c = self.converter()
        c.k.tables['input_sequences'] = []
        with self.assertRaisesRegex(ValueError, 'no special cancels'):
            S.apply(c)


if __name__ == '__main__':
    unittest.main()
