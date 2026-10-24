import argparse
import contextlib
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import move_edit as E


class SourceEditing(unittest.TestCase):
    def test_updates_one_option_and_preserves_other_text(self):
        text = "# café\nMOVESET = {\n 'bounds': {'old': True}, # keep comment\n 'own_moves': r'^running3$',\n}\n"
        result = E.replace_option(text, 'bounds', {'old': True, 'new': False})
        self.assertEqual(E.option(result, 'bounds', {}), {'old': True, 'new': False})
        self.assertIn("'own_moves': r'^running3$'", result)
        self.assertIn('# keep comment', result)
        self.assertTrue(result.startswith('# café'))

    def test_adds_option_without_requiring_trailing_comma(self):
        text = "MOVESET = {'name': 'Fahk'}\n"
        result = E.replace_option(text, 'damage', {'move': 23})
        self.assertEqual(E.option(result, 'damage', {}), {'move': 23})
        self.assertEqual(E.option(result, 'name', None), 'Fahk')

    def test_unchanged_value_preserves_formatting(self):
        text = "MOVESET = {'bounds': {\n 'x': True,\n}}\n"
        self.assertEqual(E.replace_option(text, 'bounds', {'x': True}), text)

    def test_nonliteral_option_is_rejected(self):
        with self.assertRaises((ValueError, TypeError)):
            E.option("MOVESET = {'bounds': build_bounds()}\n", 'bounds', {})

    def test_dry_run_then_apply_backs_up_exact_original(self):
        from types import SimpleNamespace
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); repo = root / 'repo'
            file = repo / 'tools/characters/ttt2char/characters/fighter.py'
            file.parent.mkdir(parents=True);original = "# preserve me\nMOVESET = {'own_moves': 'running3'}\n";file.write_text(original)
            engine = repo / 'tools/characters/ttt2char/convert/move_overrides.py';engine.parent.mkdir();engine.write_text('# support')
            args = argparse.Namespace(fighter='fighter',move='kick',command='bound',state='on',apply=False,backups=root/'backups')
            m=SimpleNamespace(tables={'moves':[{'first_active':12}]},move_name=lambda i:'kick')
            with patch.object(E,'definitions',return_value={'fighter':SimpleNamespace()}), patch.object(E,'load',return_value=m),contextlib.redirect_stdout(io.StringIO()):
                E.edit(args,repo)
                self.assertEqual(file.read_text(),original)
                self.assertFalse(args.backups.exists())
                args.apply=True;E.edit(args,repo)
            self.assertEqual(E.option(file.read_text(),'bounds',{}),{'kick':True})
            self.assertEqual(E.option(file.read_text(),'own_moves',None),'running3')
            self.assertEqual(next(args.backups.glob('*/fighter.py')).read_text(),original)

if __name__ == '__main__':
    unittest.main()
