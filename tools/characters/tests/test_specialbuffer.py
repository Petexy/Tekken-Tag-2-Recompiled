import unittest
import test_specialcancels as fixtures
from ttt2char.convert import specialcancels as S, tagsystem as TS

class BufferedSpecials(unittest.TestCase):
 def converter(self):
  c=fixtures.SpecialCancels().converter();c.defn.MOVESET['buffered_special_cancels']=True
  c.T.update(move_begin_props=[],move_end_props=[],cancel_extras=[]);c.extra_index={}
  c.k.tables['cancels'][1]['win_start']=17
  S.apply(c)
  return c
 def replay(self, motion_frame, contact_frame):
  c=self.converter();rows=TS.cancels(c.T,c.T['moves'][0]['cancel']);state={307:0,308:0};fired=[]
  for frame in range(1,19):
   for row in rows:
    if row['direction']==0x8000 or not row['win_start']<=frame<=row['win_end']:continue
    if row['direction']>=S.SEQ0 and frame!=motion_frame:continue
    actions=[];ok=True
    for pid,value in TS.reqs(c.T,row['req']):
     if pid in [307,308]:ok &= state[pid]==value
     elif pid in [46,47]:ok &= frame==contact_frame and pid==47
     elif pid in [0x8093,0x80a5]:actions.append((307 if pid==0x8093 else 308,value))
    if not ok:continue
    for pid,value in actions:state[pid]=value
    if c.T['cancel_extras'][row['extradata']]['value']==0x182:fired.append(frame)
  return fired
 def test_leniency_clones_motion_and_extends_capture_dispatch_and_contact(self):
  c=fixtures.SpecialCancels().converter()
  c.defn.MOVESET.update(buffered_special_cancels=True,
    buffered_special_timing={'extra_frames':8,'source_extra_frames':{'normal':12},'motion_window':24})
  c.T.update(move_begin_props=[],move_end_props=[],cancel_extras=[],
    input_sequences=[dict(window=13,inputs=0,amount=3) for _ in range(6)])
  c.cache={};c.extra_index={};c.k.tables['cancels'][1]['win_start']=17
  S.apply(c)
  rows=TS.cancels(c.T,c.T['moves'][0]['cancel'])
  self.assertTrue(all(row['win_end']==29 for row in rows[:-1]))
  self.assertEqual(rows[3]['win_start'],17)
  capture=rows[2];sequence=capture['direction']-S.SEQ0
  self.assertEqual(c.T['input_sequences'][sequence]['window'],24)
  self.assertEqual(c.T['input_sequences'][5]['window'],13)
  self.assertEqual(TS.reqs(c.T,rows[3]['req']),[(307,3),(308,1)])
 def test_normal_followup_keeps_early_buffer_and_string_timing(self):
  c=fixtures.SpecialCancels().converter()
  c.defn.MOVESET={'normal_string_followups':{'normal':('dp',)}}
  rq=len(c.k.tables['requirements'])
  c.k.tables['requirements'] += [dict(condition=346,param=0),dict(condition=881,param=0)]
  c.k.tables['cancels'][0].update(direction=0,button=0x40000001,req=rq,win_start=1,win_end=10)
  c.k.tables['cancels'][1].update(direction=0,button=0x40000001,req=rq,win_start=11,win_end=11,start_frame=11)
  S.restore_normal_strings(c)
  rows=TS.cancels(c.T,c.T['moves'][0]['cancel'])
  self.assertEqual((rows[0]['win_start'],rows[0]['win_end'],rows[0]['start_frame']),(1,11,11))
  self.assertEqual(TS.reqs(c.T,rows[0]['req']),[(307,0)])
  self.assertEqual(rows[0]['move_id'],1)
 def test_motion_before_contact_survives_until_dispatch(self):
  self.assertEqual(self.replay(8,15),[17])
 def test_contact_before_motion_survives_until_dispatch(self):
  self.assertEqual(self.replay(16,15),[17])
 def test_whiff_does_not_cancel(self):
  self.assertEqual(self.replay(8,None),[])
 def test_entry_resets_both_words_and_no_end_reset(self):
  c=self.converter();m=c.T['moves'][0];self.assertNotIn('end_props',m)
  props=c.T['move_begin_props'][m['begin_props']:]
  self.assertEqual([(p['id'],p['value']) for p in props],[(0x8093,0),(0x80a5,0),(690,0)])
 def test_input_capture_is_immediate_and_shared_rows_untouched(self):
  c=self.converter();rows=TS.cancels(c.T,c.T['moves'][0]['cancel'])
  self.assertEqual(len(rows),5)
  self.assertEqual(rows[2]['start_frame'],1)
  self.assertEqual(TS.reqs(c.T,rows[2]['req']),[(307,0),(0x8093,3)])
  self.assertEqual(c.T['cancels'][0]['direction'],0x8000)
if __name__=='__main__':unittest.main()
