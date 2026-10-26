# SPDX-License-Identifier: GPL-2.0-or-later
"""Download and install of a release, no Qt; the Updates screen runs it off the UI thread and only
after the player confirms each step. Packaged builds only (a git checkout updates with git).

  1. pick_assets: the package of this platform among the assets of the latest release (the same
     API answer releases.fetch_latest already got), plus SHA256SUMS when the release has it;
  2. download: HTTPS only (redirects too), into a temporary file next to the install, the size
     checked against the API's and the SHA-256 against SHA256SUMS when it exists;
  3. install: extracted into a NEW sibling folder (<package>-<tag>), never over the running one; the
     player's data folder is copied in when it lives inside the old folder (portable mode); the old
     folder stays as it is, so the player can go back by opening it;
  4. launch: starts the new launcher, detached; the caller then closes this one.
Errors are releases.UpdateError with a kind the screen turns into a friendly message (upd_err_<kind>).
"""
import hashlib
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import socket
import subprocess
import tarfile
import urllib.error
import urllib.request
import zipfile

from . import cacerts
from .releases import RELEASE_PREFIX, TIMEOUT, UpdateError, user_agent

# packaging/{linux,windows}/package.sh: dist/$PORT_SLUG-<platform>-x86_64.<ext>, one top folder
# with the same name inside.
ASSET_NAMES = {'linux': 'infamous-linux-x86_64.tar.gz', 'windows': 'infamous-windows-x86_64.zip'}
SUMS_NAME = 'SHA256SUMS'
DOWNLOAD_PREFIX = RELEASE_PREFIX + 'download/'
CHUNK = 256 * 1024
SUMS_LIMIT = 64 * 1024
# Not carried into the new folder: the shader cache is large and the game rebuilds it.
DATA_SKIP = ('cache',)


def platform_key(nt=None):
    nt = (os.name == 'nt') if nt is None else nt
    return 'windows' if nt else 'linux'


def package_folder(asset_name):
    """The top folder inside the archive: the asset name without .tar.gz / .zip."""
    return re.sub(r'(\.tar\.gz|\.tgz|\.zip)$', '', asset_name)


def pick_assets(release, nt=None):
    """(package asset, SHA256SUMS asset or None) of `release` (releases.fetch_latest's dict) for
    this platform. Raises UpdateError('noasset') when the release has no package for it."""
    assets = {a.get('name'): a for a in (release or {}).get('assets') or [] if isinstance(a, dict)}
    package = assets.get(ASSET_NAMES[platform_key(nt)])
    if not package or not _trusted(package.get('url')) or not package.get('size'):
        raise UpdateError('noasset', ASSET_NAMES[platform_key(nt)])
    sums = assets.get(SUMS_NAME)
    return package, (sums if sums and _trusted(sums.get('url')) else None)


def _trusted(url):
    return isinstance(url, str) and url.startswith(DOWNLOAD_PREFIX)


# --------------------------------------------------------------------------------------- network
class _HttpsOnlyRedirects(urllib.request.HTTPRedirectHandler):
    """GitHub answers a download with a redirect to its file host: followed only to HTTPS."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not str(newurl).lower().startswith('https://'):
            raise UpdateError('insecure', str(newurl))
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def https_opener():
    return urllib.request.build_opener(_HttpsOnlyRedirects, cacerts.https_handler()).open


def _open(url, version, opener, timeout):
    if not _trusted(url):
        raise UpdateError('insecure', str(url))
    request = urllib.request.Request(url, headers={'User-Agent': user_agent(version),
                                                   'Accept': 'application/octet-stream'})
    try:
        response = (opener or https_opener())(request, timeout=timeout)
    except UpdateError:
        raise
    except urllib.error.HTTPError as error:
        if error.code in (403, 429):
            raise UpdateError('limit', str(error.code)) from error
        raise UpdateError('http', str(error.code)) from error
    except (socket.timeout, TimeoutError) as error:
        raise UpdateError('timeout') from error
    except urllib.error.URLError as error:
        if isinstance(error.reason, UpdateError):
            raise error.reason from error
        if isinstance(error.reason, (socket.timeout, TimeoutError)):
            raise UpdateError('timeout') from error
        if cacerts.is_cert_error(error):
            raise UpdateError('tls', str(error.reason)) from error
        raise UpdateError('offline', str(error.reason)) from error
    except OSError as error:
        if cacerts.is_cert_error(error):
            raise UpdateError('tls', str(error)) from error
        raise UpdateError('offline', str(error)) from error
    final = getattr(response, 'geturl', lambda: url)() or url
    if not str(final).lower().startswith('https://'):
        response.close()
        raise UpdateError('insecure', str(final))
    return response


def parse_sums(text):
    """{file name: sha256 hex} of a sha256sum file ('<hex>  <name>' or '<hex> *<name>')."""
    out = {}
    for line in text.splitlines():
        m = re.match(r'^([0-9a-fA-F]{64})\s+\*?(.+?)\s*$', line)
        if m:
            out[m.group(2)] = m.group(1).lower()
    return out


def fetch_sums(asset, version='', opener=None, timeout=TIMEOUT):
    """The SHA256SUMS of the release as {name: hex}."""
    try:
        with _open(asset['url'], version, opener, timeout) as response:
            raw = response.read(SUMS_LIMIT + 1)
    except (socket.timeout, TimeoutError) as error:
        raise UpdateError('timeout') from error
    except OSError as error:
        raise UpdateError('offline', str(error)) from error
    if len(raw) > SUMS_LIMIT:
        raise UpdateError('bad')
    try:
        return parse_sums(raw.decode('utf-8'))
    except UnicodeDecodeError as error:
        raise UpdateError('bad') from error


def download(asset, dest, version='', opener=None, progress=None, cancelled=None, timeout=30):
    """Downloads `asset` ({'name','url','size'}) to `dest` through dest + '.part'; returns the
    SHA-256 (hex). progress(done, total) is called per chunk; cancelled() -> True stops it
    (UpdateError('cancel')). The size must be exactly the API's (UpdateError('size'))."""
    dest = Path(dest)
    part = dest.with_name(dest.name + '.part')
    total = int(asset['size'])
    digest, done = hashlib.sha256(), 0
    try:
        with _open(asset['url'], version, opener, timeout) as response, open(part, 'wb') as out:
            while True:
                if cancelled and cancelled():
                    raise UpdateError('cancel')
                try:
                    chunk = response.read(CHUNK)
                except (socket.timeout, TimeoutError) as error:
                    raise UpdateError('timeout') from error
                except OSError as error:
                    raise UpdateError('offline', str(error)) from error
                if not chunk:
                    break
                done += len(chunk)
                if done > total:
                    raise UpdateError('size', f'> {total}')
                digest.update(chunk)
                out.write(chunk)
                if progress:
                    progress(done, total)
        if done != total:
            raise UpdateError('size', f'{done} != {total}')
        os.replace(part, dest)
    except UpdateError:
        part.unlink(missing_ok=True)
        raise
    except OSError as error:      # the network ones became UpdateError above: this is the disk
        part.unlink(missing_ok=True)
        raise UpdateError('write', str(error)) from error
    return digest.hexdigest()


def verify(name, digest, sums, asset_digest=''):
    """True when SHA256SUMS (or, without it, the API's SHA-256 of the asset) confirms the file.
    Raises UpdateError('unverified') when the release publishes neither, and UpdateError('checksum')
    on a mismatch or a list without the file. Both come from the same release answer: integrity,
    not a signature."""
    if sums is None:
        if not re.fullmatch(r'sha256:[0-9a-fA-F]{64}', asset_digest):
            raise UpdateError('unverified', name)
        expected = asset_digest[7:].lower()
    else:
        expected = sums.get(name)
    if expected != digest.lower():
        raise UpdateError('checksum', name)
    return True


# --------------------------------------------------------------------------------------- install
def target_folder(port_dir, asset_name, tag):
    """A new sibling of the running install: <parent>/<package>-<tag>, -2, -3 ... when taken."""
    port_dir = Path(port_dir)
    base = f"{package_folder(asset_name)}-{re.sub(r'[^0-9A-Za-z._-]', '_', tag or 'new')}"
    target, n = port_dir.parent / base, 2
    while target.exists() or same_path(target, port_dir):
        target, n = port_dir.parent / f'{base}-{n}', n + 1
    return target


def same_path(a, b):
    try:
        return Path(a).resolve() == Path(b).resolve()
    except OSError:
        return str(a) == str(b)


def data_to_copy(port_dir, data_dir):
    """The data folder when it lives inside the running install (portable mode, <port>/user): the
    new folder needs its own copy. None when it is outside (ISS_USER_DIR or the pointer file of
    userdata.py): the new version finds it by itself."""
    try:
        Path(data_dir).resolve().relative_to(Path(port_dir).resolve())
    except (ValueError, OSError):
        return None
    return Path(data_dir) if Path(data_dir).is_dir() else None


def free_bytes(path):
    return shutil.disk_usage(path).free


def needed_bytes(size, data=None):
    """Room to ask for before downloading: the archive, about twice it extracted, and the data copy."""
    extra = 0
    if data:
        for p in Path(data).rglob('*'):
            try:
                if p.is_file() and not _skipped(Path(data), p):
                    extra += p.stat().st_size
            except OSError:
                pass
    return int(size) * 3 + extra + 50_000_000


def _skipped(root, path):
    return path.relative_to(root).parts[0] in DATA_SKIP


def _safe_name(name):
    """Archive member names that stay inside the folder: relative, no '..', no drive letter."""
    name = name.replace('\\', '/')
    parts = PurePosixPath(name).parts
    return bool(parts) and not name.startswith('/') and '..' not in parts and not re.match(r'^[A-Za-z]:', name)


def _check_tar(archive):
    for member in archive.getmembers():
        if not _safe_name(member.name):
            raise UpdateError('archive', member.name)
        if member.isdev():
            raise UpdateError('archive', member.name)
        if member.issym() or member.islnk():
            link = member.linkname if member.islnk() else str(PurePosixPath(member.name).parent / member.linkname)
            if member.linkname.startswith('/') or not _safe_name(os.path.normpath(link).replace('\\', '/')):
                raise UpdateError('archive', member.name)


def extract(archive_path, target, top):
    """Extracts the archive into `target` (which must not exist) through a hidden sibling, then
    renames: a failed extraction leaves no half folder that looks like an install. `top` is the
    archive's top folder (package_folder); an archive without it is taken as is."""
    archive_path, target = Path(archive_path), Path(target)
    if target.exists():
        raise UpdateError('write', str(target))
    tmp = target.with_name(f'.{target.name}.extracting')
    shutil.rmtree(tmp, ignore_errors=True)
    try:
        tmp.mkdir(parents=True)
        if zipfile.is_zipfile(archive_path):
            with zipfile.ZipFile(archive_path) as archive:
                for name in archive.namelist():
                    if not _safe_name(name):
                        raise UpdateError('archive', name)
                archive.extractall(tmp)
        else:
            try:
                archive = tarfile.open(archive_path, 'r:*')
            except tarfile.TarError as error:
                raise UpdateError('archive', str(error)) from error
            with archive:
                _check_tar(archive)
                if hasattr(tarfile, 'data_filter'):
                    archive.extractall(tmp, filter='data')
                else:
                    archive.extractall(tmp)  # noqa: S202 (every member checked by _check_tar)
        source = tmp / top if (tmp / top).is_dir() else tmp
        os.replace(source, target)
    except UpdateError:
        raise
    except (OSError, tarfile.TarError, zipfile.BadZipFile) as error:
        raise UpdateError('archive' if not isinstance(error, OSError) else 'write', str(error)) from error
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return target


def copy_data(src, dst):
    """Copies the player's data folder into the new install (the shader cache stays behind). Files
    the package itself brings are replaced by the player's; the old folder is not touched."""
    src, dst = Path(src), Path(dst)

    def ignore(folder, names):
        return [n for n in names if Path(folder) == src and n in DATA_SKIP]
    try:
        shutil.copytree(src, dst, ignore=ignore, dirs_exist_ok=True)
    except (OSError, shutil.Error) as error:
        raise UpdateError('write', str(error)) from error


def install(archive_path, port_dir, asset_name, tag, exe_name, data_dir=None):
    """Extracts the verified archive into a new sibling folder and copies the data folder in when
    it lived inside the old one (data_to_copy). Returns the new folder. The new folder must have
    the launcher (`exe_name`), or it is removed and UpdateError('archive') is raised."""
    target = target_folder(port_dir, asset_name, tag)
    extract(archive_path, target, package_folder(asset_name))
    if not (target / exe_name).is_file():
        shutil.rmtree(target, ignore_errors=True)
        raise UpdateError('archive', exe_name)
    copy = data_to_copy(port_dir, data_dir) if data_dir else None
    if copy:
        copy_data(copy, target / Path(copy).resolve().relative_to(Path(port_dir).resolve()))
    return target


def launch(folder, exe_name, env=None, options=None, popen=subprocess.Popen):
    """Starts the launcher of the new folder, detached from this one. A frozen (PyInstaller)
    launcher started from another frozen one must not reuse the parent's unpacked state:
    PYINSTALLER_RESET_ENVIRONMENT makes it start from scratch."""
    env = dict(os.environ if env is None else env)
    env['PYINSTALLER_RESET_ENVIRONMENT'] = '1'
    env.pop('_MEIPASS2', None)
    folder = Path(folder)
    return popen([str(folder / exe_name)], cwd=str(folder), env=env, stdin=subprocess.DEVNULL,
                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, **(options or {}))
