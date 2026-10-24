import argparse
import contextlib
import io
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import move_edit as E

class SelectedRebuilds(unittest.TestCase):
    def test_selected_rebuilds(self):
        args = argparse.Namespace(fighters=['eliza', 'akuma', 'eliza'], apply=False,
            packs=Path('/packs'), backups=Path('/backups'), t7=Path('/t7'),
            movesets=Path('/movesets'), game=Path('/game'))
        definitions = {n: SimpleNamespace(NAME=n, CODE=n) for n in ('akuma', 'eliza')}
        with patch.object(E, 'definitions', return_value=definitions), patch.object(E.Path, 'exists', return_value=True), patch.object(E, 'backup') as backup, patch.object(E.subprocess, 'run') as run, contextlib.redirect_stdout(io.StringIO()):
            E.rebuild_selected(args, Path('/repo'))
            backup.assert_not_called()
            run.assert_not_called()
            args.apply = True
            E.rebuild_selected(args, Path('/repo'))
            self.assertEqual([c.args[0][5] for c in run.call_args_list], ['eliza', 'akuma'])
            self.assertEqual(backup.call_count, 2)
            run.reset_mock(); backup.reset_mock()
            args.fighters = ['akuma', 'unknown']
            with self.assertRaisesRegex(ValueError, 'Unknown fighter'):
                E.rebuild_selected(args, Path('/repo'))
            backup.assert_not_called(); run.assert_not_called()
            args.fighters = ['akuma', 'eliza']
            run.side_effect = E.subprocess.CalledProcessError(1, 'importer')
            with self.assertRaises(E.subprocess.CalledProcessError):
                E.rebuild_selected(args, Path('/repo'))
            self.assertEqual(run.call_count, 1)
