# SPDX-License-Identifier: GPL-2.0-or-later
"""The certificates the launcher's HTTPS requests trust (releases.py, updater.py), no Qt.

The Linux package is frozen on Ubuntu and carries Ubuntu's OpenSSL, which looks for the system's
certificates in /usr/lib/ssl. SteamOS, Arch and Fedora keep them elsewhere, so every request failed
the certificate check and the Updates screen said "Could not reach GitHub" (#160, #237). When the
OpenSSL default has no certificates, the first bundle of CA_FILES that exists is used instead.
SSL_CERT_FILE / SSL_CERT_DIR set by the player always win, and Windows (system store) is untouched."""
import os
import ssl
import sys
import urllib.request

# The system bundle of each family: Debian/Ubuntu/Arch/SteamOS, Fedora/RHEL, openSUSE, Alpine/Void.
CA_FILES = (
    '/etc/ssl/certs/ca-certificates.crt',
    '/etc/pki/tls/certs/ca-bundle.crt',
    '/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem',
    '/etc/ssl/ca-bundle.pem',
    '/etc/ssl/cert.pem',
)

_context = None


def ca_file(platform=None, env=None, defaults=None, exists=os.path.isfile):
    """The CA bundle to load instead of OpenSSL's default, or None to keep the default: not Linux,
    the player set SSL_CERT_FILE/SSL_CERT_DIR, the default bundle or folder exists, or none of
    CA_FILES does."""
    platform = sys.platform if platform is None else platform
    env = os.environ if env is None else env
    if not platform.startswith('linux') or env.get('SSL_CERT_FILE') or env.get('SSL_CERT_DIR'):
        return None
    defaults = ssl.get_default_verify_paths() if defaults is None else defaults
    if defaults.cafile or defaults.capath:    # None when the file or folder does not exist
        return None
    return next((path for path in CA_FILES if exists(path)), None)


def context():
    """The SSL context of every request (built once): OpenSSL's defaults plus, when they have no
    certificates, the system bundle ca_file() found."""
    global _context
    if _context is None:
        _context = ssl.create_default_context(cafile=ca_file())
    return _context


def https_handler():
    return urllib.request.HTTPSHandler(context=context())


def is_cert_error(error):
    """True for a failed certificate check, as urllib raises it (the URLError's reason) or bare."""
    reason = getattr(error, 'reason', error)
    return isinstance(reason, ssl.SSLCertVerificationError) or \
        (isinstance(reason, ssl.SSLError) and 'CERTIFICATE_VERIFY_FAILED' in str(reason))
