"""Preset discovery must preserve gameplay and include the full worn model."""
import os
import sys
import unittest
import types
from unittest.mock import patch
import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

from ttt2char.characters import fahkumram, kazumi, CHARACTERS, definition
from ttt2char.convert import outfits
from ttt2char.ttt2 import nud, nut
from ttt2char import pack


def item(path):
    return {'MeshType': 'SkeletalMeshAnim', 'SourceSkeletalMesh': path}


class OutfitTests(unittest.TestCase):
    def test_all_roster_aliases_are_unique_and_fit_native_chooser(self):
        codes = [code for name in CHARACTERS for code in outfits.codes(definition(name))]
        self.assertEqual(len(codes), 137)
        self.assertEqual(len(codes), len(set(codes)))
        self.assertTrue(all(len(code) == 3 and code.islower() and code.isalpha() for code in codes))
        self.assertTrue(all(len(definition(name).PRESETS) <= 14 for name in CHARACTERS))

    def test_katarina_face_with_omitted_mesh_type_is_kept(self):
        from ttt2char.characters import katarina
        face = dict(SourceSkeletalMesh=katarina.TEKKEN7['parts'][0][1])
        with patch.object(outfits.model, 'costume_items', return_value=[face]):
            variant = outfits.definition(None, katarina, '/Game/Test')
        self.assertIn(katarina.TEKKEN7['parts'][0], variant.TEKKEN7['parts'])
        self.assertEqual(variant.MESHES['fac'], katarina.MESHES['fac'])

    def test_explicit_static_mesh_is_not_treated_as_skeletal(self):
        parts = [dict(item('/Game/Meshes/Face/face'), MeshType='StaticMeshSocket'),
                 item('/Game/Meshes/Upper/body')]
        with patch.object(outfits.model, 'costume_items', return_value=parts):
            variant = outfits.definition(None, fahkumram, '/Game/Test')
        self.assertEqual([tag for tag, _ in variant.TEKKEN7['parts']], ['bdu'])

    def test_alias_prefixes_do_not_depend_on_roster_order(self):
        expected = {'kazumi': 'kza', 'akuma': 'oea', 'katarina': 'oha',
                    'kunimitsu2': 'ona', 'noctis': 'oqa'}
        for name, first in expected.items():
            self.assertEqual(outfits.codes(definition(name))[0], first)
        self.assertEqual(set(outfits.PREFIXES), {definition(name).CODE for name in CHARACTERS})

    def test_wearable_blueprint_keeps_skeletal_tail_and_glasses_offsets(self):
        tail = dict(ItemBlueprint="BlueprintGeneratedClass'/Game/Tail.Tail_C'")
        glasses = dict(ItemBlueprint="BlueprintGeneratedClass'/Game/Glasses.Glasses_C'",
                       ItemOffsets={'Translation': [0, 5.1, .8]})
        with patch.object(outfits.model, 'load', side_effect=lambda paks, path: path), \
             patch.object(outfits.model, 'export_properties', side_effect=lambda path: [
                 (None, {'SkeletalMesh': "SkeletalMesh'/Game/TailMesh.TailMesh'"})] if path == '/Game/Tail' else [
                 (None, {'StaticMesh': "StaticMesh'/Game/GlassesMesh.GlassesMesh'"})]):
            expanded = outfits.blueprint_items(None, [tail, glasses])
        self.assertEqual(expanded[2]['MeshType'], 'SkeletalMeshAnim')
        self.assertEqual(expanded[3]['MeshType'], 'StaticMeshSocket')
        self.assertEqual(expanded[3]['ItemOffsets'], glasses['ItemOffsets'])
        self.assertNotIn('MeshType', tail)

    def test_missing_chain_rules_do_not_leak_into_other_hairstyles(self):
        base = types.SimpleNamespace(CODE='abc', MESHES={'har': ('abc_har', 'head', 0)},
            TEKKEN7={'merge_order': ['har']}, CHAINS=[('har', 'old_tail_root', [(0,-1,0)])],
            HIDE=[('gla', 'held_mask')], COVERED=[('har', 'old_mask')])
        mesh = types.SimpleNamespace(skeleton={'names': ['Head', 'short_hair']}, materials=[])
        with patch.object(outfits.model, 'costume_items', return_value=[item('/Game/Meshes/Hair/short')]), \
             patch.object(outfits.model, 'load'), patch.object(outfits.model.t7mesh, 'SkeletalMesh', return_value=mesh):
            variant = outfits.definition(None, base, '/Game/ShortHair')
        self.assertEqual((variant.CHAINS, variant.HIDE, variant.COVERED), ([], [], []))
        self.assertEqual(len(base.CHAINS), 1)

    def test_nine_kazumi_aliases_do_not_overlap_fahkumram(self):
        self.assertEqual(len(outfits.codes(kazumi)), 9)
        self.assertFalse(set(outfits.codes(kazumi)) & set(outfits.codes(fahkumram)))

    def test_full_body_costume_keeps_tiger_and_uses_body_parent(self):
        with patch.object(outfits.model, 'costume_items', return_value=[
                item('/Game/Meshes/kzm/Full_Body/dougi'), item('/Game/Meshes/kzm/Face/face')]):
            variant = outfits.definition(None, kazumi, '/Game/Dougi')
        self.assertIs(variant.TIGER, kazumi.TIGER)
        self.assertEqual(variant.MESHES['bdf'][1], 'KOSHI_jnt')

    def test_tiger_animation_layout_and_bind_transforms_must_match(self):
        def skeleton():
            return dict(names=['root', 'cha:OPT600_tiger', 'cha:OPT601_tail'], parent=[-1, 0, 1],
                        opt=[600, 601], t=np.zeros((3, 3)), r=np.zeros((3, 3)), s=np.ones((3, 3)))
        base, preset = skeleton(), skeleton()
        outfits.check_tiger_skeleton(preset, base)
        preset['parent'][2] = 0
        with self.assertRaisesRegex(ValueError, 'bone indices'):
            outfits.check_tiger_skeleton(preset, base)
        preset = skeleton()
        preset['t'][2, 1] = 1
        with self.assertRaisesRegex(ValueError, 'bind transforms'):
            outfits.check_tiger_skeleton(preset, base)
        with self.assertRaisesRegex(ValueError, 'base skeleton'):
            outfits.check_tiger_skeleton(preset, None)

    def test_full_model_and_gameplay_preserved(self):
        parts = [item('/Game/Meshes/nsc/Face/face'),
                 item('/Game/Meshes/nsc/Upper/champion'),
                 item('/Game/Meshes/nsc/Head/crown')]
        with patch.object(outfits.model, 'costume_items', return_value=parts) as read:
            variant = outfits.definition(None, fahkumram, '/Game/Champion')
        read.assert_called_once_with(None, '/Game/Champion', follow=True)
        self.assertEqual([t for t, _ in variant.TEKKEN7['parts']], ['fac', 'bdu', 'hed'])
        self.assertEqual(variant.TEKKEN7['merge_order'], ['fac', 'bdu', 'hed'])
        self.assertIs(variant.MOVESET, fahkumram.MOVESET)
        self.assertEqual(getattr(variant, 'WEAPONS', ()), getattr(fahkumram, 'WEAPONS', ()))
        self.assertNotIn('costume_2p', variant.TEKKEN7)
        self.assertNotIn('hed', fahkumram.MESHES)
        self.assertEqual(fahkumram.TEKKEN7['costume'].split('/')[-1], 'CS_NSC_DefaultCos1P')

    def test_duplicate_dependency_mesh_not_added_twice(self):
        with patch.object(outfits.model, 'costume_items', return_value=[
                item('/Game/Meshes/nsc/Hair/short'), item('/game/meshes/nsc/hair/SHORT')]):
            variant = outfits.definition(None, fahkumram, '/Game/Test')
        self.assertEqual(len(variant.TEKKEN7['parts']), 1)

    def test_unknown_mesh_fails_instead_of_silently_dropping_it(self):
        with patch.object(outfits.model, 'costume_items', return_value=[item('/Game/Unknown/model')]):
            with self.assertRaisesRegex(ValueError, 'unsupported preset mesh'):
                outfits.definition(None, fahkumram, '/Game/Test')

    def test_relocated_model_and_texture_container_agree(self):
        # Include a serialized GIDX extension: changing only the decoded gidx field
        # used to leave that extension (and the actual on-disk ID) unchanged.
        texture = nut.make_texture(np.full((4, 4, 4), 255, np.uint8), 123, mips=1)
        textures = nut.write([texture])
        verts = np.zeros(3, nud.vertex_dtype(0x13, 0x12))
        verts['pw'] = 1
        material = dict(flags=0, src=0, dst=0, alpha_test=0, alpha_func=0, ref=0, cull=0,
                        textures=[dict(id=123, wrap_s=1, wrap_t=1, min=1, mag=1, mip=1),
                                  dict(id=456, wrap_s=1, wrap_t=1, min=1, mag=1, mip=1)],
                        props=[('NU_colorSamplerUV', [1.0, 1.0, 0.0, 0.0])])
        mesh = dict(version=0x0201, sphere=(0, 0, 0, 1), bone_first=0, bone_last=1, meshes=[
            dict(name='test', sphere=(0, 0, 0, 1), center=(0, 0, 0), sort_bias=0, zero=0, boneflag=4,
                 singlebind=-1, polys=[dict(vtype=0x13, uvtype=0x12, pflag=0, tail=bytes(12),
                     indices=np.array([0, 1, 2], np.uint16), verts=verts, materials=[material])])])
        model, bank = outfits.relocate(nud.write(mesh), textures, 1000)
        self.assertEqual(nut.read(bank)['textures'][0]['gidx'], 1000)
        refs = nud.read(model)['meshes'][0]['polys'][0]['materials'][0]['textures']
        self.assertEqual([t['id'] for t in refs], [1000, 456])  # shared template texture stays shared
        _, other = outfits.relocate(nud.write(mesh), textures, 2000)
        self.assertNotEqual(nut.read(bank)['textures'][0]['gidx'], nut.read(other)['textures'][0]['gidx'])

    def test_pack_metadata_is_opt_in(self):
        self.assertNotIn('outfits =', pack.character_ini(fahkumram, 10))
        self.assertIn('outfits = fka fkb', pack.character_ini(fahkumram, 10, outfits=('fka', 'fkb')))


if __name__ == '__main__':
    unittest.main()
