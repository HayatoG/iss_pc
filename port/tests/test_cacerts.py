"""Certificates of the launcher's HTTPS requests (launcher/qt/cacerts.py): the system bundle the
Ubuntu-built Linux package falls back to on SteamOS/Arch/Fedora (#160, #237), and a failed
certificate check told apart from "offline". No test uses the network."""
import ssl
import sys
import unittest
import urllib.error
from types import SimpleNamespace

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
from qt import cacerts, releases, updater  # noqa: E402

NO_DEFAULTS = SimpleNamespace(cafile=None, capath=None)    # Ubuntu's /usr/lib/ssl missing
ARCH = '/etc/ssl/certs/ca-certificates.crt'
FEDORA = '/etc/pki/tls/certs/ca-bundle.crt'


def cert_error():
    return ssl.SSLCertVerificationError(1, '[SSL: CERTIFICATE_VERIFY_FAILED] certificate verify failed: '
                                           'unable to get local issuer certificate (_ssl.c:1006)')


def raising(error):
    def open_(request, timeout=None):
        raise error
    return open_


class CaFileTests(unittest.TestCase):
    def pick(self, platform='linux', env=None, defaults=NO_DEFAULTS, present=(ARCH,)):
        return cacerts.ca_file(platform, env or {}, defaults, exists=lambda path: path in present)

    def test_steamos_and_fedora_get_their_system_bundle(self):
        self.assertEqual(self.pick(present=(ARCH,)), ARCH)
        self.assertEqual(self.pick(present=(FEDORA,)), FEDORA)

    def test_working_defaults_are_kept(self):
        self.assertIsNone(self.pick(defaults=SimpleNamespace(cafile='/usr/lib/ssl/cert.pem', capath=None)))
        self.assertIsNone(self.pick(defaults=SimpleNamespace(cafile=None, capath='/usr/lib/ssl/certs')))

    def test_player_choice_and_other_systems_are_untouched(self):
        self.assertIsNone(self.pick(env={'SSL_CERT_FILE': '/my/ca.pem'}))
        self.assertIsNone(self.pick(env={'SSL_CERT_DIR': '/my/certs'}))
        self.assertIsNone(self.pick(platform='win32'))
        self.assertIsNone(self.pick(platform='darwin'))

    def test_nothing_found_keeps_the_default(self):
        self.assertIsNone(self.pick(present=()))

    def test_context_verifies(self):
        context = cacerts.context()
        self.assertEqual(context.verify_mode, ssl.CERT_REQUIRED)
        self.assertTrue(context.check_hostname)


class CertErrorTests(unittest.TestCase):
    def test_kinds(self):
        self.assertTrue(cacerts.is_cert_error(cert_error()))
        self.assertTrue(cacerts.is_cert_error(urllib.error.URLError(cert_error())))
        self.assertFalse(cacerts.is_cert_error(urllib.error.URLError(OSError(-3, 'Name or service not known'))))
        self.assertFalse(cacerts.is_cert_error(urllib.error.URLError('no route')))

    def test_release_check_says_tls_not_offline(self):
        with self.assertRaises(releases.UpdateError) as caught:
            releases.fetch_latest('v1', opener=raising(urllib.error.URLError(cert_error())))
        self.assertEqual(caught.exception.kind, 'tls')
        self.assertIn('CERTIFICATE_VERIFY_FAILED', caught.exception.detail)
        with self.assertRaises(releases.UpdateError) as caught:
            releases.fetch_latest('v1', opener=raising(urllib.error.URLError(OSError(-3, 'no name'))))
        self.assertEqual(caught.exception.kind, 'offline')

    def test_download_says_tls(self):
        asset = {'name': 'SHA256SUMS', 'url': releases.RELEASE_PREFIX + 'download/v1/SHA256SUMS', 'size': 1}
        with self.assertRaises(releases.UpdateError) as caught:
            updater.fetch_sums(asset, 'v1', raising(urllib.error.URLError(cert_error())))
        self.assertEqual(caught.exception.kind, 'tls')

    def test_message_names_the_detail(self):
        t = lambda key, **fmt: (key, fmt)  # noqa: E731
        self.assertEqual(releases.error_text(releases.UpdateError('tls', 'x'), t), ('upd_err_tls', {'detail': 'x'}))


if __name__ == '__main__':
    unittest.main()
