"""Download and install of a release (launcher/qt/updater.py) and the Updates screen's flow around
it (qt/updates.py): asset choice, HTTPS only, size and SHA-256 checks, extraction into a new sibling
folder, data copy, restart. No test uses the network: every download goes through a fake opener."""
import hashlib
import io
import json
import os
import sys
import tarfile
import tempfile
import time
import unittest
import urllib.error
import zipfile
from pathlib import Path
from unittest import mock

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
from qt import releases, updater  # noqa: E402

try:
    from qt.backend import Backend
    HAVE_QT = True
except ImportError:
    HAVE_QT = False

DL = releases.RELEASE_PREFIX + 'download/v2026.10.15-61e8942/'
EXE = 'inFAMOUS Second Son'


class FakeResponse(io.BytesIO):
    def __init__(self, data, url='https://objects.githubusercontent.com/x', headers=None):
        super().__init__(data)
        self.url = url
        self.headers = headers or {}

    def geturl(self):
        return self.url

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False


def fake_opener(files, final_url=None):
    """urlopen stand-in serving {url: bytes}; records the requests."""
    calls = []

    def open_(request, timeout=None):
        calls.append(request)
        return FakeResponse(files[request.full_url], final_url or 'https://objects.githubusercontent.com/x')
    open_.calls = calls
    return open_


def tar_package(entries, top='infamous-linux-x86_64'):
    """A .tar.gz like packaging/linux/package.sh makes: {relative path: bytes}."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w:gz') as tar:
        for name, data in entries.items():
            info = tarfile.TarInfo(f'{top}/{name}' if top else name)
            info.size, info.mode = len(data), 0o755
            tar.addfile(info, io.BytesIO(data))
    return buf.getvalue()


def release_with(package, name='infamous-linux-x86_64.tar.gz', sums=True):
    assets = [{'name': name, 'url': DL + name, 'size': len(package)},
              {'name': 'infamous-windows-x86_64.zip', 'url': DL + 'infamous-windows-x86_64.zip', 'size': 10}]
    files = {DL + name: package}
    if sums:
        text = f'{hashlib.sha256(package).hexdigest()}  {name}\n{"0" * 64}  infamous-windows-x86_64.zip\n'.encode()
        assets.append({'name': 'SHA256SUMS', 'url': DL + 'SHA256SUMS', 'size': len(text)})
        files[DL + 'SHA256SUMS'] = text
    return {'tag': 'v2026.10.15-61e8942', 'assets': assets}, files


class FetchAssetsTests(unittest.TestCase):
    def test_fetch_latest_keeps_only_our_https_assets(self):
        data = {'tag_name': 'v1', 'assets': [
            {'name': 'infamous-linux-x86_64.tar.gz', 'browser_download_url': DL + 'infamous-linux-x86_64.tar.gz', 'size': 5},
            {'name': 'evil.tar.gz', 'browser_download_url': 'https://evil.example/evil.tar.gz', 'size': 5},
            {'name': 'plain', 'browser_download_url': 'http://github.com/HayatoG/iss_pc/releases/download/v1/plain', 'size': 5}]}
        got = releases.fetch_latest('', opener=lambda request, timeout=None: FakeResponse(json.dumps(data).encode()))
        self.assertEqual([a['name'] for a in got['assets']], ['infamous-linux-x86_64.tar.gz'])
        self.assertEqual(got['assets'][0]['size'], 5)


class FetchLatestTests(unittest.TestCase):
    """The one API request: the ETag of the release seen before (a 304 costs nothing against
    GitHub's 60 an hour per network) and which 403s are a rate limit, with its reset time."""

    @staticmethod
    def http_error(code, headers):
        return urllib.error.HTTPError(releases.API_URL, code, 'x', headers, io.BytesIO(b'{}'))

    @staticmethod
    def raising(error, calls):
        def open_(request, timeout=None):
            calls.append(request)
            raise error
        return open_

    def test_etag_is_kept_and_sent_back(self):
        calls = []

        def open_(request, timeout=None):
            calls.append(request)
            return FakeResponse(json.dumps({'tag_name': 'v2026.10.19-d3995fe'}).encode(), headers={'ETag': 'W/"abc"'})
        self.assertEqual(releases.fetch_latest('v1', opener=open_)['etag'], 'W/"abc"')
        self.assertIsNone(calls[0].get_header('If-none-match'))
        calls.clear()
        unchanged = releases.fetch_latest('v1', opener=self.raising(self.http_error(304, {}), calls), etag='W/"abc"')
        self.assertIsNone(unchanged)
        self.assertEqual(calls[0].get_header('If-none-match'), 'W/"abc"')

    def test_rate_limit_carries_the_reset_time(self):
        error = self.http_error(403, {'X-RateLimit-Remaining': '0', 'X-RateLimit-Reset': '1792559606'})
        with self.assertRaises(releases.UpdateError) as caught:
            releases.fetch_latest(opener=self.raising(error, []))
        self.assertEqual((caught.exception.kind, caught.exception.reset), ('limit', 1792559606))
        self.assertEqual(releases.limit_reset(403, {'Retry-After': '30'}, now=1000), 1030)   # secondary limit
        self.assertEqual(releases.limit_reset(429, {}), 0)                                   # limit, no time given

    def test_other_403_is_an_http_error(self):
        with self.assertRaises(releases.UpdateError) as caught:
            releases.fetch_latest(opener=self.raising(self.http_error(403, {'X-RateLimit-Remaining': '42'}), []))
        self.assertEqual((caught.exception.kind, caught.exception.detail), ('http', '403'))

    def test_messages(self):
        t = lambda key, **fmt: (key, fmt)  # noqa: E731
        reset = 1792559606
        self.assertEqual(releases.error_text(releases.UpdateError('limit', '403', reset=reset), t),
                         ('upd_err_limit_until', {'time': time.strftime('%H:%M', time.localtime(reset))}))
        self.assertEqual(releases.error_text(releases.UpdateError('limit', '429'), t), ('upd_err_limit', {'detail': '429'}))
        strings = json.loads((ROOT / 'launcher' / 'qt' / 'i18n' / 'strings.json').read_text(encoding='utf-8'))
        for key, field in (('upd_err_limit_until', '{time}'), ('upd_cached', '{checked}')):
            self.assertTrue(all(field in strings[key][lang] for lang in ('en', 'pt')), key)


class PickTests(unittest.TestCase):
    def test_platform_asset_and_sums(self):
        release, _ = release_with(b'x')
        package, sums = updater.pick_assets(release, nt=False)
        self.assertEqual(package['name'], 'infamous-linux-x86_64.tar.gz')
        self.assertEqual(sums['name'], 'SHA256SUMS')
        package, _ = updater.pick_assets(release, nt=True)
        self.assertEqual(package['name'], 'infamous-windows-x86_64.zip')

    def test_no_package_or_foreign_url(self):
        with self.assertRaises(releases.UpdateError) as ctx:
            updater.pick_assets({'assets': []}, nt=False)
        self.assertEqual(ctx.exception.kind, 'noasset')
        release = {'assets': [{'name': 'infamous-linux-x86_64.tar.gz', 'url': 'https://evil.example/a', 'size': 3}]}
        with self.assertRaises(releases.UpdateError):
            updater.pick_assets(release, nt=False)

    def test_without_sums_only_the_package(self):
        release, _ = release_with(b'x', sums=False)
        self.assertIsNone(updater.pick_assets(release, nt=False)[1])

    def test_parse_sums(self):
        sums = updater.parse_sums(f'{"A" * 64}  one.zip\n{"b" * 64} *two.tar.gz\nnot a line\n')
        self.assertEqual(sums, {'one.zip': 'a' * 64, 'two.tar.gz': 'b' * 64})


class DownloadTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_download_checks_size_and_sha(self):
        data = b'package' * 1000
        release, files = release_with(data)
        package, sums_asset = updater.pick_assets(release, nt=False)
        opener = fake_opener(files)
        seen = []
        sums = updater.fetch_sums(sums_asset, 'v1', opener)
        digest = updater.download(package, self.root / 'pkg', 'v1', opener, progress=lambda d, t: seen.append((d, t)))
        self.assertTrue(updater.verify(package['name'], digest, sums))
        self.assertEqual((self.root / 'pkg').read_bytes(), data)
        self.assertEqual(seen[-1], (len(data), len(data)))
        self.assertIn('iss_pc-launcher/v1', opener.calls[-1].get_header('User-agent'))
        self.assertFalse((self.root / 'pkg.part').exists())

    def test_wrong_size_is_refused_and_removed(self):
        release, files = release_with(b'abc')
        package, _ = updater.pick_assets(release, nt=False)
        for data in (b'ab', b'abcd'):
            files[package['url']] = data
            with self.assertRaises(releases.UpdateError) as ctx:
                updater.download(package, self.root / 'pkg', opener=fake_opener(files))
            self.assertEqual(ctx.exception.kind, 'size')
            self.assertFalse((self.root / 'pkg').exists())
            self.assertFalse((self.root / 'pkg.part').exists())

    def test_checksum_mismatch_and_missing_name(self):
        with self.assertRaises(releases.UpdateError) as ctx:
            updater.verify('a.zip', 'f' * 64, {'a.zip': '0' * 64})
        self.assertEqual(ctx.exception.kind, 'checksum')
        with self.assertRaises(releases.UpdateError):
            updater.verify('a.zip', 'f' * 64, {'b.zip': 'f' * 64})
        with self.assertRaises(releases.UpdateError):   # no SHA256SUMS and no API digest: refused
            updater.verify('a.zip', 'f' * 64, None)
        self.assertTrue(updater.verify('a.zip', 'f' * 64, None, 'sha256:' + 'f' * 64))

    def test_https_only(self):
        release, files = release_with(b'abc')
        package, _ = updater.pick_assets(release, nt=False)
        with self.assertRaises(releases.UpdateError) as ctx:
            updater.download(dict(package, url=package['url'].replace('https:', 'http:')), self.root / 'pkg',
                             opener=fake_opener(files))
        self.assertEqual(ctx.exception.kind, 'insecure')
        with self.assertRaises(releases.UpdateError) as ctx:   # the redirect ended on plain HTTP
            updater.download(package, self.root / 'pkg', opener=fake_opener(files, final_url='http://x/y'))
        self.assertEqual(ctx.exception.kind, 'insecure')
        handler = updater._HttpsOnlyRedirects()
        with self.assertRaises(releases.UpdateError):
            handler.redirect_request(None, None, 302, 'Found', {}, 'http://objects.example/file')

    def test_cancel(self):
        release, files = release_with(b'x' * (updater.CHUNK * 3))
        package, _ = updater.pick_assets(release, nt=False)
        with self.assertRaises(releases.UpdateError) as ctx:
            updater.download(package, self.root / 'pkg', opener=fake_opener(files), cancelled=lambda: True)
        self.assertEqual(ctx.exception.kind, 'cancel')
        self.assertFalse((self.root / 'pkg.part').exists())


class InstallTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.port = self.root / 'infamous-linux-x86_64'
        (self.port / 'user' / 'savedata').mkdir(parents=True)
        (self.port / EXE).write_bytes(b'old')
        (self.port / 'user' / 'savedata' / 'slot.bin').write_bytes(b'save')
        (self.port / 'user' / 'tlg.ini').write_text('vsync=1\n')
        (self.port / 'user' / 'cache').mkdir()
        (self.port / 'user' / 'cache' / 'big.bin').write_bytes(b'cache')

    def archive(self, data, name='pkg.tar.gz'):
        path = self.root / name
        path.write_bytes(data)
        return path

    def test_new_sibling_folder_with_data_copy_and_old_one_untouched(self):
        archive = self.archive(tar_package({EXE: b'new', 'out/tlg-probe': b'probe'}))
        folder = updater.install(archive, self.port, 'infamous-linux-x86_64.tar.gz', 'v2026.10.15-61e8942', EXE,
                                 self.port / 'user')
        self.assertEqual(folder, self.root / 'infamous-linux-x86_64-v2026.10.15-61e8942')
        self.assertEqual((folder / EXE).read_bytes(), b'new')
        self.assertTrue(os.access(folder / EXE, os.X_OK) or os.name == 'nt')
        self.assertEqual((folder / 'user' / 'savedata' / 'slot.bin').read_bytes(), b'save')
        self.assertTrue((folder / 'user' / 'tlg.ini').is_file())
        self.assertFalse((folder / 'user' / 'cache').exists())      # the shader cache stays behind
        self.assertEqual((self.port / EXE).read_bytes(), b'old')     # the running install is not touched
        self.assertTrue((self.port / 'user' / 'cache' / 'big.bin').is_file())
        self.assertEqual([p.name for p in self.root.iterdir() if p.name.startswith('.')], [])

    def test_data_outside_the_install_is_not_copied(self):
        outside = self.root / 'data'
        outside.mkdir()
        self.assertIsNone(updater.data_to_copy(self.port, outside))
        archive = self.archive(tar_package({EXE: b'new'}))
        folder = updater.install(archive, self.port, 'infamous-linux-x86_64.tar.gz', 'v2', EXE, outside)
        self.assertFalse((folder / 'user').exists())

    def test_taken_folder_gets_a_suffix(self):
        (self.root / 'infamous-linux-x86_64-v2').mkdir()
        archive = self.archive(tar_package({EXE: b'new'}))
        folder = updater.install(archive, self.port, 'infamous-linux-x86_64.tar.gz', 'v2', EXE)
        self.assertEqual(folder.name, 'infamous-linux-x86_64-v2-2')

    def test_unsafe_members_are_refused(self):
        for entries, top in (({'../escape': b'x'}, ''), ({'/abs': b'x'}, '')):
            archive = self.archive(tar_package(entries, top=top))
            with self.assertRaises(releases.UpdateError) as ctx:
                updater.install(archive, self.port, 'infamous-linux-x86_64.tar.gz', 'v3', EXE)
            self.assertEqual(ctx.exception.kind, 'archive')
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode='w:gz') as tar:
            link = tarfile.TarInfo('infamous-linux-x86_64/lib')
            link.type, link.linkname = tarfile.SYMTYPE, '../../etc'
            tar.addfile(link)
        with self.assertRaises(releases.UpdateError):
            updater.install(self.archive(buf.getvalue()), self.port, 'infamous-linux-x86_64.tar.gz', 'v3', EXE)
        self.assertFalse((self.root / 'escape').exists())
        self.assertEqual(sorted(p.name for p in self.root.iterdir()), ['infamous-linux-x86_64', 'pkg.tar.gz'])

    def test_package_without_the_launcher_is_removed(self):
        archive = self.archive(tar_package({'README': b'x'}))
        with self.assertRaises(releases.UpdateError) as ctx:
            updater.install(archive, self.port, 'infamous-linux-x86_64.tar.gz', 'v4', EXE)
        self.assertEqual(ctx.exception.kind, 'archive')
        self.assertFalse((self.root / 'infamous-linux-x86_64-v4').exists())

    def test_windows_zip(self):
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, 'w') as z:
            z.writestr('infamous-windows-x86_64/' + EXE + '.exe', b'new')
        folder = updater.install(self.archive(buf.getvalue(), 'pkg.zip'), self.port, 'infamous-windows-x86_64.zip',
                                 'v5', EXE + '.exe')
        self.assertEqual(folder.name, 'infamous-windows-x86_64-v5')
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, 'w') as z:
            z.writestr('..\\evil.exe', b'x')
        with self.assertRaises(releases.UpdateError):
            updater.install(self.archive(buf.getvalue(), 'bad.zip'), self.port, 'infamous-windows-x86_64.zip',
                            'v6', EXE + '.exe')

    def test_launch_resets_the_pyinstaller_environment_and_detaches(self):
        popen = mock.Mock()
        updater.launch(self.port, EXE, env={'_MEIPASS2': '/tmp/x', 'HOME': '/h'}, options={'start_new_session': True},
                       popen=popen)
        args, kwargs = popen.call_args
        self.assertEqual(args[0], [str(self.port / EXE)])
        self.assertEqual(kwargs['cwd'], str(self.port))
        self.assertEqual(kwargs['env']['PYINSTALLER_RESET_ENVIRONMENT'], '1')
        self.assertNotIn('_MEIPASS2', kwargs['env'])
        self.assertTrue(kwargs['start_new_session'])


class StringsTests(unittest.TestCase):
    def test_every_error_kind_has_a_message(self):
        strings = json.loads((ROOT / 'launcher' / 'qt' / 'i18n' / 'strings.json').read_text(encoding='utf-8'))
        for kind in ('noasset', 'insecure', 'size', 'checksum', 'space', 'write', 'archive', 'cancel', 'launch',
                     'offline', 'tls', 'timeout', 'limit', 'http', 'bad'):
            self.assertIn('upd_err_' + kind, strings)
        text = (ROOT / 'launcher' / 'qt' / 'updates.py').read_text(encoding='utf-8')
        for key in set(__import__('re').findall(r"'(upd_\w+)'", text)) - {'upd_err_'}:
            self.assertIn(key, strings, key)


@unittest.skipUnless(HAVE_QT, 'PySide6 not installed')
class ScreenFlowTests(unittest.TestCase):
    """The Updates screen: nothing happens without the player's yes, step by step."""

    def setUp(self):
        from qt import logic
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.port = self.root / 'infamous-linux-x86_64'
        (self.port / 'user').mkdir(parents=True)
        (self.port / 'user' / 'tlg.ini').write_text('x')
        package = tar_package({EXE: b'new'})
        self.release, self.files = release_with(package)
        self.release.update({'url': releases.RELEASES_URL, 'body': '', 'published': '2026-10-15'})
        self.frozen, self.launched, self.quit = True, [], []
        patcher = mock.patch.object(updater, 'platform_key', lambda nt=None: 'linux')
        patcher.start()
        self.addCleanup(patcher.stop)
        probes = {'gpu': lambda: ('GPU', '1.3'), 'pad': lambda: None, 'problems': lambda: [], 'cache': lambda: 0,
                  'save': lambda: None, 'news': lambda: [], 'version': lambda: 'v1', 'save_fixes': lambda s, st: None,
                  'saves': {'running': lambda: False, 'scan': lambda: [], 'open': lambda p: None},
                  'updates': {'version': lambda: 'v2026.10.13-3f2a9c1', 'fetch': lambda v: self.release,
                              'open_url': lambda url: None, 'save': lambda s: None, 'frozen': lambda: self.frozen,
                              'port_dir': lambda: self.port, 'data_dir': lambda: self.port / 'user', 'exe': lambda: EXE,
                              'opener': fake_opener(self.files), 'launch': self.launched.append,
                              'quit': lambda: self.quit.append(True)}}
        settings = dict(logic.tlg_run.DEFAULT_SETTINGS, game_dir='/g', ui_language='en_US')
        with mock.patch.dict(os.environ, {'ISS_USER_DIR': str(self.port / 'user')}):
            self.backend = Backend(settings=settings, probes=probes)
        self.updates = self.backend.updates
        from PySide6.QtCore import QCoreApplication
        self.app = QCoreApplication.instance() or QCoreApplication([])

    def pump(self, until, limit=5000):
        import time
        end = time.time() + limit / 1000
        while not until() and time.time() < end:
            self.app.processEvents()
            time.sleep(0.01)
        self.assertTrue(until(), 'timed out')

    def test_source_checkout_shows_a_hint_and_no_install(self):
        self.frozen = False
        self.updates.check()
        self.pump(lambda: not self.updates.checking)
        self.assertFalse(self.updates.canInstall)
        self.assertIn('git pull', self.updates.installHint)
        self.updates.install()
        self.assertFalse(self.updates.dialog)

    def test_a_failed_check_says_the_release_shown_is_an_older_one(self):
        self.updates.check()
        self.pump(lambda: not self.updates.checking)
        seen = self.updates.release['checked']

        def limited(version):
            raise releases.UpdateError('limit', '403', reset=1792559606)
        self.updates.probes['fetch'] = limited
        self.updates.check()
        self.pump(lambda: not self.updates.checking)
        self.assertEqual(self.updates.latest, 'v2026.10.15-61e8942')    # the one seen before stays on screen
        self.assertIn(time.strftime('%H:%M', time.localtime(1792559606)), self.updates.statusText)
        self.assertIn(seen, self.updates.statusText)                    # ... and the text says when it was seen
        self.assertEqual(self.updates.tone, 'bad')

    def test_unchanged_release_keeps_the_one_seen(self):
        self.updates.check()
        self.pump(lambda: not self.updates.checking)
        self.updates.release['checked'] = '2026-10-01'
        self.updates.probes['fetch'] = lambda version: None             # a 304: still the latest
        self.updates.check()
        self.pump(lambda: not self.updates.checking)
        self.assertEqual(self.updates.latest, 'v2026.10.15-61e8942')
        self.assertEqual(self.updates.release['checked'], time.strftime('%Y-%m-%d'))
        self.assertNotIn('2026-10-01', self.updates.statusText)

    def test_download_install_restart_each_after_a_yes(self):
        self.updates.check()
        self.pump(lambda: not self.updates.checking)
        self.assertTrue(self.updates.canInstall)
        self.updates.install()
        self.assertTrue(self.updates.dialog['open'])
        self.assertEqual(len(self.updates.probes['opener'].calls), 0)   # nothing downloaded before the yes
        self.updates.answer('yes')
        self.pump(lambda: self.updates.dialog.get('open'))
        self.assertIn('SHA-256', self.updates.dialog['body'])
        self.assertFalse((self.root / 'infamous-linux-x86_64-v2026.10.15-61e8942').exists())
        self.updates.answer('yes')
        self.pump(lambda: self.updates.canRestart)
        folder = self.root / 'infamous-linux-x86_64-v2026.10.15-61e8942'
        self.assertTrue((folder / EXE).is_file())
        self.assertTrue((folder / 'user' / 'tlg.ini').is_file())
        self.assertEqual(sorted(p.name for p in self.root.iterdir()), ['infamous-linux-x86_64', folder.name])
        self.updates.answer('yes')
        self.assertEqual(self.launched, [folder])
        self.assertEqual(self.quit, [True])

    def test_bad_checksum_stops_before_install(self):
        self.files[releases.RELEASE_PREFIX + 'download/v2026.10.15-61e8942/SHA256SUMS'] = \
            f'{"0" * 64}  infamous-linux-x86_64.tar.gz\n'.encode()
        self.updates.check()
        self.pump(lambda: not self.updates.checking)
        self.updates.install()
        self.updates.answer('yes')
        self.pump(lambda: not self.updates.busy)
        self.assertIn('checksum', self.updates.statusText)
        self.assertFalse(self.updates.dialog)
        self.assertEqual(sorted(p.name for p in self.root.iterdir()), ['infamous-linux-x86_64'])


if __name__ == '__main__':
    unittest.main()
