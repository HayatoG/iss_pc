# SPDX-License-Identifier: GPL-2.0-or-later
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import time
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'launcher'))
from PySide6.QtCore import QCoreApplication, QObject
from qt.logic import Translator
from qt.updates import Updates
from qt import releases
import update_install


class Backend(QObject):
    def __init__(self, root):
        super().__init__()
        self.settings = {'ui_language': 'en_US', 'game_dir': str(root / 'game')}
        self.t_ = Translator(self.settings)
        self._play_state = 'idle'
        self._jobs = None
        self.quit_called = False

    def _quit_app(self):
        self.quit_called = True


class ControllerTests(unittest.TestCase):
    app = None

    @classmethod
    def setUpClass(cls):
        cls.app = QCoreApplication.instance() or QCoreApplication([])

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='iss-qt-update-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.port = self.root / 'installed'
        self.port.mkdir()
        (self.port / 'launcher.exe').write_text('old')
        self.payload = self.root / 'package'
        self.payload.mkdir()
        (self.payload / 'launcher.exe').write_text('new')
        self.tag = 'v2026.10.19-abcd123'
        update_install.write_manifest(self.payload, self.tag)
        data = io.BytesIO()
        with zipfile.ZipFile(data, 'w') as z:
            for p in self.payload.iterdir():
                z.write(p, p.name)
        self.zip = data.getvalue()
        self.backend = Backend(self.port)
        self.handoffs = []
        self.running = False
        self.release = {'tag': self.tag, 'url': releases.RELEASES_URL, 'published': '', 'body': '',
                        'assets': [{'name': 'infamous-windows-x86_64.zip', 'size': len(self.zip),
                                    'url': releases.RELEASE_PREFIX + 'download/v1/test.zip',
                                    'digest': 'sha256:' + hashlib.sha256(self.zip).hexdigest()}]}
        self.u = Updates(self.backend, {'frozen': lambda: True, 'nt': lambda: True,
                    'port_dir': lambda: self.port, 'data_dir': lambda: self.port / 'user',
                    'exe': lambda: 'launcher.exe', 'running': lambda: self.running,
                    'version': lambda: 'v2026.10.18-abcd124', 'save': lambda _: None,
                    'backup': lambda: None, 'handoff': lambda *a: self.handoffs.append(a),
                    'opener': lambda *a, **k: io.BytesIO(self.zip),
                    # Install rechecks GitHub first (2026-11-23): these stand in for the two requests.
                    'fetch': lambda _version: dict(self.release), 'fetch_beta': self.no_beta})
        self.u.release = self.release

    @staticmethod
    def no_beta(_version):
        raise releases.UpdateError('none')

    def pump(self, condition):
        end = time.monotonic() + 5
        while not condition() and time.monotonic() < end:
            self.app.processEvents()
            time.sleep(.005)
        self.assertTrue(condition(), self.u.statusText)

    def test_install_rechecks_then_downloads_prepares_and_hands_off_with_no_other_dialog(self):
        # 2026-11-23: the click on Install is the confirmation; the launcher rechecks the release,
        # then downloads, verifies, prepares and hands off without asking again.
        self.assertTrue(self.u.canInstall)
        self.u.install()
        self.assertFalse(self.u.dialog)
        self.pump(lambda: self.backend.quit_called or bool(self.u._error))
        self.assertEqual(self.u._error, '')
        self.assertEqual(len(self.handoffs), 1)
        self.assertEqual((self.port / 'launcher.exe').read_text(), 'old')
        self.assertEqual(update_install._read(self.handoffs[0][0])['target'], str(self.port))
        self.assertFalse(self.u.dialog)

    def test_a_failed_recheck_installs_nothing(self):
        def offline(_version):
            raise releases.UpdateError('offline')
        self.u.probes['fetch'] = offline
        self.u.install()
        self.pump(lambda: bool(self.u._error))
        self.assertEqual(self.handoffs, [])
        self.assertFalse(self.backend.quit_called)

    def test_busy_and_source_checkout_cannot_install(self):
        self.running = True
        self.assertFalse(self.u.canInstall)
        self.u.install()
        self.assertFalse(self.u.dialog)
        self.running = False
        self.u.probes['frozen'] = lambda: False
        self.assertFalse(self.u.canInstall)

    def test_game_started_after_the_click_blocks_the_update(self):
        self.u.install()
        self.running = True
        self.pump(lambda: not self.u.checking)
        for _ in range(50):
            self.app.processEvents()
            time.sleep(.005)
        self.assertEqual(self.handoffs, [])
        self.assertFalse(self.backend.quit_called)
        self.assertEqual((self.port / 'launcher.exe').read_text(), 'old')

    def test_wrong_digest_and_missing_protocol_cannot_touch_install(self):
        for digest in ('sha256:' + '0' * 64, ''):
            self.u.release['assets'][0]['digest'] = digest
            self.u._error = ''
            self.u.install()
            self.pump(lambda: bool(self.u._error))
            self.assertEqual(self.handoffs, [])
        self.assertEqual((self.port / 'launcher.exe').read_text(), 'old')


if __name__ == '__main__':
    unittest.main()
