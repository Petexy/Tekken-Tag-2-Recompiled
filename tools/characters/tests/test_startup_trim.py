import unittest,types,collections,os,sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import numpy as np
from ttt2char.convert.move_overrides import trim_startup
from ttt2char.ttt2 import animation as A
from ttt2char.convert import tagsystem as TS
class StartupTrim(unittest.TestCase):
 def test_pose_and_event_timeline_shift_together_without_shared_edits(self):
  values=np.zeros((30,1,3),np.float32);values[:,0,0]=np.arange(30)
  anim=A.encode_raw([1],values)
  move=dict(anim=0x4000,first_active=20,last_active=21,anim_len=30,airborne_start=0,airborne_end=0,ground_fall=0,cancel=0,extraprops=0)
  row=dict(direction=0x8000,button=0,req=0,extradata=0,win_start=29,win_end=32767,start_frame=29,move_id=0,option=0)
  c=types.SimpleNamespace(kn={'move':0},newidx={0:0},pack0_count=0,new_anims=[anim],stats=collections.Counter(),
    T={'moves':[move],'cancels':[row],'extra_move_properties':[dict(frame=20,id=1,value=0),dict(frame=0x800f,id=2,value=0),dict(frame=0,id=0,value=0)]})
  trim_startup(c,'move',4)
  dec=A.decode(c.new_anims[1]);np.testing.assert_allclose(dec['values'],values[4:])
  self.assertEqual((move['first_active'],move['last_active'],move['anim_len']),(16,17,26))
  self.assertEqual(c.T['cancels'][0]['win_start'],29)
  changed=TS.cancels(c.T,move['cancel'])[0]
  self.assertEqual((changed['win_start'],changed['win_end']),(25,32767))
  props=c.T['extra_move_properties'][move['extraprops']:]
  self.assertEqual([p['frame'] for p in props],[16,0x800b,0])
if __name__=='__main__':unittest.main()
