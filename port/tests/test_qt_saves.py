"""The Qt "Saves and data" controller (launcher/qt/saves.py): folder change with the copy question,
delete/import confirmations, language-following texts, error texts. Skipped without PySide6."""
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
from qt import logic  # noqa: E402
import savemanager  # noqa: E402
import userdata  # noqa: E402
from test_savemanager import put_slot, body_of  # noqa: E402

try:
    from qt.backend import Backend
    HAVE_QT = True
except ImportError:
    HAVE_QT = False

STRINGS = json.loads((ROOT / 'launcher' / 'qt' / 'i18n' / 'strings.json').read_text(encoding='utf-8'))


class ErrorKeyTests(unittest.TestCase):
    def test_every_error_key_raised_by_the_core_has_a_text(self):
        import re
        keys = set()
        for name in ('userdata.py', 'savemanager.py'):
            keys |= set(re.findall(r"(?:SaveError|DataError)\('(\w+)'", (ROOT / 'launcher' / name).read_text()))
        self.assertGreater(len(keys), 10)
        for key in keys:
            self.assertIn('err_' + key, STRINGS, key)


@unittest.skipUnless(HAVE_QT, 'PySide6 not installed')
class SavesControllerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        patcher = mock.patch.dict(os.environ, {'XDG_CONFIG_HOME': str(self.root / 'cfg')})
        patcher.start()
        self.addCleanup(patcher.stop)
        for name in ('ISS_USER_DIR', 'TLG_USER_DIR'):
            os.environ.pop(name, None)
        port = mock.patch.object(logic.tlg_run, 'PORT_DIR', self.root / 'port')
        port.start()
        self.addCleanup(port.stop)
        probes = {'gpu': lambda: ('GPU', '1.3'), 'pad': lambda: None, 'problems': lambda: [], 'cache': lambda: 0,
                  'save': lambda: None, 'news': lambda: [], 'version': lambda: 'v1', 'save_fixes': lambda s, st: None,
                  'saves': {'running': lambda: False, 'scan': lambda: [], 'open': lambda p: None}}
        settings = dict(logic.tlg_run.DEFAULT_SETTINGS, game_dir='/g', ui_language='en_US')
        self.backend = Backend(settings=settings, probes=probes)
        self.saves = self.backend.saves
        self.default = self.root / 'port' / 'user'

    def test_default_folder_is_portable_and_pointer_changes_it(self):
        self.assertEqual(str(self.default), self.saves.userDir)
        self.assertEqual('default', self.saves.source)
        self.saves.chooseDir(str(self.root / 'docs'))            # empty old folder: no question
        self.assertEqual(str(self.root / 'docs'), self.saves.userDir)
        self.assertEqual('pointer', self.saves.source)
        self.saves.resetDir()
        self.assertEqual(str(self.default), self.saves.userDir)

    @mock.patch.dict(os.environ, {'ISS_ADVANCED': '1'})   # fix_states read tlg.ini: advanced mode only
    def test_changing_folder_asks_to_copy_and_copies_never_moves(self):
        put_slot(self.default, 'infamousssUser0', 'T', b'data')
        (self.default / 'tlg.ini').write_text('bp01=0\n')
        self.saves.reload()
        self.saves.chooseDir(str(self.root / 'docs'))
        self.assertTrue(self.saves.dialog['open'])
        self.assertEqual('copy', self.saves.dialog['buttons'][0]['id'])
        self.assertEqual(str(self.default), self.saves.userDir)   # nothing changed yet
        self.saves.answer('copy')
        self.assertEqual(str(self.root / 'docs'), self.saves.userDir)
        self.assertEqual(b'data', body_of(self.root / 'docs', 'infamousssUser0'))
        self.assertEqual(b'data', body_of(self.default, 'infamousssUser0'))   # still in the old place
        self.assertEqual(1, len(self.saves.slots))
        self.assertEqual('good', self.saves.messageTone)
        self.assertEqual(0, self.backend.fix_states.get('BP01') is None)       # tlg.ini of the new folder was read

    def test_cancel_and_do_not_copy(self):
        put_slot(self.default, 'infamousssUser0')
        self.saves.chooseDir(str(self.root / 'docs'))
        self.saves.answer('cancel')
        self.assertEqual(str(self.default), self.saves.userDir)
        self.saves.chooseDir(str(self.root / 'docs'))
        self.saves.answer('nocopy')
        self.assertEqual(str(self.root / 'docs'), self.saves.userDir)
        self.assertEqual([], self.saves.slots)
        self.assertTrue(userdata.has_saves(self.default))

    def test_environment_variable_locks_the_folder(self):
        with mock.patch.dict(os.environ, {'ISS_USER_DIR': str(self.root / 'forced')}):
            self.assertFalse(self.saves.canChange)
            self.saves.chooseDir(str(self.root / 'docs'))
            self.assertEqual(str(self.root / 'forced'), self.saves.userDir)
            self.assertEqual('warn', self.saves.messageTone)

    def test_unwritable_target_is_an_error_not_a_change(self):
        blocker = self.root / 'file'
        blocker.write_text('x')
        self.saves.chooseDir(str(blocker / 'sub'))
        self.assertEqual('bad', self.saves.messageTone)
        self.assertEqual(str(self.default), self.saves.userDir)

    def test_delete_asks_then_backs_up_and_deletes(self):
        put_slot(self.default, 'infamousssUser0', 'T', b'data')
        self.saves.reload()
        self.saves.askDelete('1/infamousssUser0')
        self.assertTrue(self.saves.dialog['open'])
        self.assertEqual(1, len(self.saves.slots))
        self.saves.answer('ok')
        self.assertEqual([], self.saves.slots)
        self.assertEqual(1, len(self.saves.backups))
        self.assertIn(self.saves.backups[0]['name'], self.saves.message)

    def test_actions_are_refused_while_the_game_runs(self):
        put_slot(self.default, 'infamousssUser0')
        self.saves.probes['running'] = lambda: True
        self.saves.reload()
        self.assertTrue(self.saves.running)
        self.saves.askDelete('1/infamousssUser0')
        self.saves.answer('ok')
        self.assertEqual('bad', self.saves.messageTone)
        self.assertEqual(1, len(self.saves.slots))

    def test_import_zip_conflict_dialog_offers_new_only_and_replace(self):
        put_slot(self.default, 'infamousssUser0', 'mine', b'mine')
        src = self.root / 'src'
        put_slot(src, 'infamousssUser0', 'theirs', b'theirs')
        put_slot(src, 'infamousssUser1', 'new', b'new')
        savemanager.export_zip(src, self.root / 'in.zip')
        self.saves.importZip(str(self.root / 'in.zip'))
        self.assertEqual(['new', 'all', 'cancel'], [b['id'] for b in self.saves.dialog['buttons']])
        self.saves.answer('new')
        self.assertEqual(b'mine', body_of(self.default, 'infamousssUser0'))
        self.assertEqual(b'new', body_of(self.default, 'infamousssUser1'))

    def test_bad_zip_shows_an_error(self):
        bad = self.root / 'bad.zip'
        bad.write_bytes(b'nope')
        self.saves.importZip(str(bad))
        self.assertEqual('bad', self.saves.messageTone)
        self.assertFalse(self.saves.dialog)

    def test_home_hint_only_when_this_folder_is_empty_and_another_has_saves(self):
        self.saves._found = [{'path': str(self.root / 'old' / 'user'), 'saves': 2, 'newest': 1_700_000_000.0}]
        self.assertIn('old', self.saves.homeHint)
        self.assertEqual(1, len(self.saves.found))
        put_slot(self.default, 'infamousssUser0')
        self.saves.reload()
        self.assertEqual('', self.saves.homeHint)

    def test_texts_follow_the_language(self):
        put_slot(self.default, 'infamousssAuto0')
        self.saves.reload()
        self.assertEqual('Auto', self.saves.slots[0]['kindText'])
        self.backend.setLang('pt_BR')
        self.assertEqual('Automático', self.saves.slots[0]['kindText'])

    def test_backup_now_and_keep(self):
        put_slot(self.default, 'infamousssUser0')
        self.saves.backupNow()
        self.assertEqual(1, len(self.saves.backups))
        self.saves.setKeep(5)
        self.assertEqual(5, self.saves.keep)


if __name__ == '__main__':
    unittest.main()
