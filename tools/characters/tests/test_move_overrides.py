import collections
import os
import sys
import types
import unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from ttt2char.convert import move_overrides as O, tagsystem as T, idmaps as M

class BufferedString(unittest.TestCase):
    def test_direct_followup_keeps_counterhit_gate_and_timing(self):
        c=types.SimpleNamespace(kn={'kick':0},newidx={0:0},stats=collections.Counter())
        c.map_req=lambda i:{352:313,0x80a6:32918}.get(i)
        c.T={'moves':[{'cancel':0}], 'cancels':[], 'requirements':[]}
        def req(*items):
            n=len(c.T['requirements'])
            c.T['requirements'] += [dict(condition=a,param=b) for a,b in items]+[dict(condition=T.TT_END,param=0)]
            return n
        def row(button,rq,target,ws=1,we=23,sf=15):
            return dict(direction=0,button=button,req=rq,move_id=target,win_start=ws,win_end=we,start_frame=sf,extradata=14,option=0)
        c.T['cancels']=[row(0x40000002,req((313,0),(32918,1)),0x8001),
                        row(0,req((313,1),(128,0)),2),row(0,req((313,1)),1),
                        dict(row(0,req((0,0)),0x8001),direction=M.LIST_END)]
        original=[dict(r) for r in c.T['cancels']]
        def add(table,value):
            c.T[table].append(value)
            return len(c.T[table])-1
        c.add=add
        O.direct_buffered_string(c,'kick')
        rows=T.cancels(c.T,c.T['moves'][0]['cancel'])
        self.assertEqual(len(rows),3)
        self.assertEqual([r['move_id'] for r in rows[:2]],[2,1])
        self.assertEqual([T.reqs(c.T,r['req']) for r in rows[:2]], [[(128,0)],[]])
        for r in rows[:2]:
            self.assertEqual((r['button'],r['win_start'],r['win_end'],r['start_frame']),(0x40000002,1,23,15))
        self.assertEqual(c.T['cancels'][:4],original)
        self.assertEqual(rows[-1]['direction'],M.LIST_END)

class NativeStance(unittest.TestCase):
    def test_miguel_reference_preserves_entry_timing(self):
        from ttt2char.movesets import Moveset
        c=types.SimpleNamespace(kn={'f4':0,'set':1},newidx={0:0,1:1},
            defn=types.SimpleNamespace(MOVESET={'stance_transitions':{'f4':'set'},
                'stance_reference':(36,'native_entry','native_state')}),extra_index={},stats=collections.Counter())
        row=dict(direction=64,button=0,req=0,move_id=1,extradata=0,
                 win_start=20,win_end=20,start_frame=20,option=0)
        end=dict(row,direction=M.LIST_END,move_id=0x8001)
        c.T={'moves':[{'cancel':0},{'bitfield':0x120}], 'cancels':[row,end],
             'requirements':[dict(condition=T.TT_END,param=0)],'cancel_extras':[]}
        native=Moveset();native.game='ttt2'
        native.tables={'moves':[{'cancel':0},{'bitfield':0}],
            'cancels':[dict(row),dict(end)],'cancel_extras':[dict(value=0x401)]}
        native.move_name=lambda i:['native_entry','native_state'][i]
        c.stance_reference=native
        def add(table,value):
            c.T[table].append(value)
            return len(c.T[table])-1
        c.add=add
        c.map_req = lambda i: None
        O.apply(c)
        result=T.cancels(c.T,c.T['moves'][0]['cancel'])[0]
        self.assertEqual((result['win_start'],result['win_end'],result['start_frame']),(20,20,20))
        self.assertEqual(c.T['cancel_extras'][result['extradata']]['value'],0x401)
        self.assertEqual(c.T['moves'][1]['bitfield'],0x100) # only native stance bit changed
        self.assertEqual(c.T['cancels'][0],row)

class AirReaction(unittest.TestCase):
    def test_air_template_preserves_damage_ground_and_shared_lists(self):
        c=types.SimpleNamespace(kn={'b1':0},newidx={0:0},dn={'native':0},stats=collections.Counter(),
            defn=types.SimpleNamespace(MOVESET={'air_reactions':{'b1':'native'}}))
        requirements=[dict(condition=466,param=0),dict(condition=T.TT_END,param=0)]
        c.T={'moves':[{'hit_condition':0}], 'hit_conditions':[
            dict(req=0,damage=20,reactions=0),dict(req=1,damage=20,reactions=1)],
            'requirements':requirements, 'reactions':[
                dict(default=22,vertical_pushback=0),dict(default=322,vertical_pushback=0)]}
        c.d=types.SimpleNamespace(tables={'moves':[{'hit_condition':0}],
            'hit_conditions':[dict(req=0,damage=30,reactions=0),dict(req=1,damage=30,reactions=0)],
            'requirements':requirements,'reactions':[dict(default=32,vertical_pushback=120)]})
        original=[dict(h) for h in c.T['hit_conditions']]
        def add(table,value):
            c.T[table].append(value)
            return len(c.T[table])-1
        c.add=add
        c.map_req = lambda i: None
        O.apply(c)
        hits=T.hits(c.T,c.T['moves'][0]['hit_condition'])
        self.assertEqual(c.T['reactions'][hits[0]['reactions']],dict(default=32,vertical_pushback=120))
        self.assertEqual(hits[0]['damage'],20)
        self.assertEqual(hits[1],original[1])
        self.assertEqual(c.T['hit_conditions'][:2],original)
        self.assertEqual(c.T['reactions'][1]['vertical_pushback'],0)


class BufferedStanceCandidate(unittest.TestCase):
    def test_buffers_forward_without_advancing_transition_or_attack(self):
        row=dict(direction=64,button=0,req=0,move_id=1,extradata=0,
                 win_start=20,win_end=20,start_frame=20,option=0)
        end=dict(row,direction=M.LIST_END,move_id=0x8001)
        c=types.SimpleNamespace(kn={'f4':0,'set':1},newidx={0:0,1:1},extra_index={0x80:0},stats=collections.Counter())
        c.T={'moves':[dict(cancel=0,first_active=15,last_active=16),
            dict(hitlevel=0,bitfield=0x20,first_active=0,last_active=0,airborne_start=0,airborne_end=0)],
            'cancels':[row,end],'cancel_extras':[dict(value=0x80)]}
        O.buffered_stance_transition(c,'f4','set')
        r=T.cancels(c.T,c.T['moves'][0]['cancel'])[0]
        self.assertEqual((r['win_start'],r['win_end'],r['start_frame']),(1,20,20))
        self.assertEqual(c.T['moves'][0]['first_active'],15)
        self.assertEqual(c.T['moves'][1]['hitlevel'],0x200000)
        self.assertEqual(c.T['moves'][1]['airborne_start'],0)
        self.assertEqual(c.T['moves'][1]['bitfield'],0x20)
        self.assertEqual(c.T['cancels'][0],row)

class SafeBufferNormalization(unittest.TestCase):
    def fixture(self, complex_capture=False, missing_dispatch=False):
        c=types.SimpleNamespace(kn={'kick':0},newidx={0:0},stats=collections.Counter())
        c.map_req=lambda i:{352:313,0x80a6:32918}.get(i)
        c.T={'moves':[{'cancel':0}], 'cancels':[], 'requirements':[]}
        def req(*items):
            n=len(c.T['requirements']);c.T['requirements'] += [dict(condition=a,param=b) for a,b in items]+[dict(condition=T.TT_END,param=0)];return n
        def row(bt,rq,mid,dr=0):return dict(direction=dr,button=bt,req=rq,move_id=mid,win_start=1,win_end=20,start_frame=18,extradata=0,option=0)
        extra=((32771,7),) if complex_capture else ()
        c.T['cancels']=[row(0x40000002,req((313,0),(32918,1),*extra),0x8001),
            row(0,req((313,2 if missing_dispatch else 1),(128,0)),1),row(0,req(),0x8001,M.LIST_END)]
        def add(t,v):c.T[t].append(v);return len(c.T[t])-1
        c.add=add
        return c
    def test_simple_capture_is_made_direct_and_gate_preserved(self):
        c=self.fixture();O.normalize_buffered_strings(c)
        rows=T.cancels(c.T,c.T['moves'][0]['cancel'])
        self.assertEqual(rows[0]['move_id'],1)
        self.assertEqual(rows[0]['button'],0x40000002)
        self.assertEqual(T.reqs(c.T,rows[0]['req']),[(128,0)])
    def test_complex_capture_is_not_changed(self):
        c=self.fixture(complex_capture=True);O.normalize_buffered_strings(c)
        self.assertEqual(c.T['moves'][0]['cancel'],0)
    def test_unmatched_capture_is_not_changed(self):
        c=self.fixture(missing_dispatch=True);O.normalize_buffered_strings(c)
        self.assertEqual(c.T['moves'][0]['cancel'],0)


class BufferPriority(unittest.TestCase):
    def test_forced_rows_keep_priority_over_input(self):
        c=SafeBufferNormalization().fixture()
        forced=dict(c.T['cancels'][1],move_id=99,req=6)
        c.T['cancels'].insert(0,forced)
        O.normalize_buffered_strings(c)
        rows=T.cancels(c.T,c.T['moves'][0]['cancel'])
        self.assertEqual(rows[0],forced)
        self.assertEqual(rows[1]['move_id'],1)


class DamageOverride(unittest.TestCase):
    def test_damage_copies_shared_list_and_preserves_reactions(self):
        c=types.SimpleNamespace(kn={'kick':0},newidx={0:0},stats=collections.Counter(),
            defn=types.SimpleNamespace(MOVESET={'damage':{'kick':28}}),map_req=lambda i:None)
        c.T={'moves':[{'hit_condition':0},{'hit_condition':0}],
             'requirements':[dict(condition=466,param=0),dict(condition=T.TT_END,param=0)],
             'hit_conditions':[dict(req=0,damage=23,reactions=4),dict(req=1,damage=23,reactions=5)]}
        O.apply(c)
        hits=T.hits(c.T,c.T['moves'][0]['hit_condition'])
        self.assertEqual([h['damage'] for h in hits],[28,28])
        self.assertEqual([h['reactions'] for h in hits],[4,5])
        self.assertEqual(c.T['moves'][1]['hit_condition'],0)
        self.assertEqual(c.T['hit_conditions'][0]['damage'],23)

    def test_invalid_damage_is_rejected(self):
        c=types.SimpleNamespace(defn=types.SimpleNamespace(MOVESET={'damage':{'kick':-1}}))
        with self.assertRaises(ValueError):O.apply(c)

if __name__=='__main__':unittest.main()
