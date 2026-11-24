# SPDX-License-Identifier: GPL-2.0-or-later
"""NVIDIA DLSS for renderer v2: where the two DLLs are, whether they are usable, and downloading NVIDIA's.

The project ships no NVIDIA binary. Two components are needed and the launchers show both:
  * nvngx_dlss.dll, NVIDIA's own. The player can download it from NVIDIA's public repository
    (github.com/NVIDIA/DLSS) with the launcher's button, after confirming the origin, the size and
    that the file is NVIDIA's, under NVIDIA's licence (the player decides). Or copy it from a game.
  * tlg_dlss.dll, the bridge (gpu/dlss_bridge, MIT, built with NVIDIA's static NGX library). It is
    an optional separate add-on, not part of the base package; packaging/windows/build_dlss.sh
    builds it locally.
The renderer searches a folder list (search_folders): the folder set in tlg.ini (dlss_dll_dir), then
<user data>/dlss, then the executable's folder, and never fails the start when something is missing.

The download goes to a temporary file in the destination folder, is checked (size, PE header,
Authenticode signature of "NVIDIA Corporation", readable version) and only then renamed over
nvngx_dlss.dll. A failed or cancelled download leaves nothing behind. The file is never executed.

Texts (TEXT, English and Portuguese) are shared by both launchers like native_resolution.TEXT.
"""
import json
import os
import re
import ssl
import struct
import subprocess
import threading
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

DLL_NAME = 'nvngx_dlss.dll'
BRIDGE_NAME = 'tlg_dlss.dll'
BRIDGE_EXPORT = 'TlgDlssGetApi'
REPO_URL = 'https://github.com/NVIDIA/DLSS'
SOURCE_URL = 'https://github.com/NVIDIA/DLSS/raw/main/lib/Windows_x86_64/rel/nvngx_dlss.dll'
SIGNER = 'NVIDIA Corporation'
NOMINAL_MB = 56
# What a real nvngx_dlss.dll weighs (56 MB in 310.9.16). Outside this range the download is refused.
MIN_SIZE = 10 * 1024 * 1024
MAX_SIZE = 300 * 1024 * 1024
TEMP_NAME = 'nvngx_dlss.download.dll'    # ends in .dll: Authenticode checks by extension
CHUNK = 64 * 1024
SIGNATURE_TIMEOUT = 120

WINDOWS = os.name == 'nt'
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


class DlssError(Exception):
    """A failed step with a key for the player's message (TEXT['dlss_err_<key>']) and a detail."""

    def __init__(self, key, detail=''):
        super().__init__(f'{key}: {detail}' if detail else key)
        self.key, self.detail = key, str(detail)


# ------------------------------------------------------------------------------------ where
def search_folders(user_dir, configured='', exe_dir=None):
    """The folders the renderer searches, best first (iss_temporal_dlss.cpp SearchFolders)."""
    folders = []
    if configured:
        folders.append(Path(configured))
    if user_dir:
        folders.append(Path(user_dir) / 'dlss')
    if exe_dir:
        folders.append(Path(exe_dir))
    return folders


def find(name, folders):
    for folder in folders:
        candidate = Path(folder) / name
        if candidate.is_file():
            return candidate
    return None


def default_folder(user_dir):
    return Path(user_dir) / 'dlss'


# ------------------------------------------------------------------------------------ the files
def file_version(path):
    """'310.9.16.0' from the file's version resource (Windows); None when unreadable or not Windows."""
    if not WINDOWS:
        return None
    try:
        import ctypes
        from ctypes import wintypes
        version = ctypes.WinDLL('version', use_last_error=True)
        size = version.GetFileVersionInfoSizeW(str(path), None)
        if not size:
            return None
        data = ctypes.create_string_buffer(size)
        if not version.GetFileVersionInfoW(str(path), 0, size, data):
            return None
        block, length = ctypes.c_void_p(), wintypes.UINT()
        if not version.VerQueryValueW(data, '\\', ctypes.byref(block), ctypes.byref(length)) or length.value < 52:
            return None
        fixed = ctypes.string_at(block.value, 52)
        ms, ls = struct.unpack_from('<II', fixed, 8)
        return f'{ms >> 16}.{ms & 0xffff}.{ls >> 16}.{ls & 0xffff}'
    except (OSError, AttributeError, ValueError):
        return None


def signature(path):
    """(valid, subject): Authenticode of the file, through PowerShell (Windows). valid is True only for a
    valid signature whose signer is NVIDIA Corporation."""
    if not WINDOWS:
        return False, 'the signature can only be checked on Windows'
    script = ("$s = Get-AuthenticodeSignature -LiteralPath $env:TLG_SIGN_PATH; "
              "@{Status = $s.Status.ToString(); Subject = if ($s.SignerCertificate) { $s.SignerCertificate.Subject } "
              "else { '' }} | ConvertTo-Json -Compress")
    try:
        out = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command', script],
                             env={**os.environ, 'TLG_SIGN_PATH': str(path)},
                             capture_output=True, text=True, timeout=SIGNATURE_TIMEOUT,
                             stdin=subprocess.DEVNULL, creationflags=NO_WINDOW).stdout
        info = json.loads(out.strip().splitlines()[-1])
    except (OSError, subprocess.SubprocessError, ValueError, IndexError):
        return False, 'the signature could not be read'
    subject = info.get('Subject') or ''
    valid = info.get('Status') == 'Valid' and re.search(r'(^|,\s*)CN=' + re.escape(SIGNER) + r'(,|$)', subject)
    return bool(valid), subject


def pe_exports(path):
    """The names a PE file exports (pure Python). Empty for anything that is not a 64-bit PE with exports."""
    try:
        with open(path, 'rb') as f:
            head = f.read(0x1000)
            if head[:2] != b'MZ':
                return []
            pe = struct.unpack_from('<I', head, 0x3c)[0]
            f.seek(pe)
            nt = f.read(0x108)
            if nt[:4] != b'PE\0\0':
                return []
            sections_count = struct.unpack_from('<H', nt, 6)[0]
            optional_size = struct.unpack_from('<H', nt, 20)[0]
            magic = struct.unpack_from('<H', nt, 24)[0]
            if magic != 0x20b:
                return []
            export_rva, export_size = struct.unpack_from('<II', nt, 24 + 112)
            if not export_rva or not export_size:
                return []
            f.seek(pe + 24 + optional_size)
            sections = [struct.unpack('<8sIIII', f.read(40)[:24]) for _ in range(sections_count)]

            def offset(rva):
                for _, vsize, vaddr, rsize, raw in sections:
                    if vaddr <= rva < vaddr + max(vsize, rsize):
                        return raw + rva - vaddr
                return None
            start = offset(export_rva)
            if start is None:
                return []
            f.seek(start)
            table = f.read(40)
            count, names_rva = struct.unpack_from('<II', table, 24)[0], struct.unpack_from('<I', table, 32)[0]
            f.seek(offset(names_rva))
            name_rvas = struct.unpack(f'<{count}I', f.read(4 * count))
            names = []
            for rva in name_rvas:
                f.seek(offset(rva))
                raw = b''
                while True:
                    byte = f.read(1)
                    if not byte or byte == b'\0':
                        break
                    raw += byte
                names.append(raw.decode('ascii', 'replace'))
            return names
    except (OSError, struct.error, TypeError, ValueError):
        return []


class Component:
    """The state of one DLL: 'missing', 'invalid' or 'ok', where it is, its version and a message key."""

    def __init__(self, state, path=None, version=None, detail=''):
        self.state, self.path, self.version, self.detail = state, path, version, detail

    @property
    def ok(self):
        return self.state == 'ok'

    def __repr__(self):
        return f'Component({self.state!r}, {str(self.path)!r}, {self.version!r}, {self.detail!r})'


def inspect_nvidia_dll(path, check_signature=True, signature_check=None):
    """nvngx_dlss.dll: present, a PE file, signed by NVIDIA, with a readable version."""
    if path is None or not Path(path).is_file():
        return Component('missing')
    size = Path(path).stat().st_size
    try:
        with open(path, 'rb') as f:
            if f.read(2) != b'MZ':
                return Component('invalid', path, detail='format')
    except OSError:
        return Component('invalid', path, detail='read')
    if not MIN_SIZE <= size <= MAX_SIZE:
        return Component('invalid', path, detail='size')
    if check_signature:
        valid, subject = (signature_check or signature)(path)
        if not valid:
            return Component('invalid', path, detail='signature')
    return Component('ok', path, file_version(path))


def inspect_bridge(path):
    """tlg_dlss.dll: present and exporting TlgDlssGetApi."""
    if path is None or not Path(path).is_file():
        return Component('missing')
    if BRIDGE_EXPORT not in pe_exports(path):
        return Component('invalid', path, detail='bridge')
    return Component('ok', path)


def status(user_dir, configured='', exe_dir=None, check_signature=True):
    """{'dll': Component, 'bridge': Component, 'folders': [...]} for the folders the renderer searches."""
    folders = search_folders(user_dir, configured, exe_dir)
    return {
        'dll': inspect_nvidia_dll(find(DLL_NAME, folders), check_signature),
        'bridge': inspect_bridge(find(BRIDGE_NAME, folders)),
        'folders': folders,
    }


def usable(state):
    return state['dll'].ok and state['bridge'].ok


def dll_line(t, component):
    """One line for the DLL: 'DLSS 310.9.16 (…path…)', or what is wrong."""
    if component.ok:
        version = '.'.join(component.version.split('.')[:3]) if component.version else '?'
        return t('dlss_dll_ok', version=version, path=str(component.path))
    if component.state == 'missing':
        return t('dlss_dll_missing')
    return t('dlss_dll_invalid', why=t('dlss_why_' + (component.detail or 'format')))


def bridge_line(t, component):
    if component.ok:
        return t('dlss_bridge_ok', path=str(component.path))
    if component.state == 'missing':
        return t('dlss_bridge_missing')
    return t('dlss_bridge_invalid')


# ------------------------------------------------------------------------------------ download
def _check_url(url):
    parts = urllib.parse.urlsplit(url)
    local = parts.hostname in ('127.0.0.1', 'localhost', '::1')
    if parts.scheme != 'https' and not (parts.scheme == 'http' and local):
        raise DlssError('url', url)
    return parts


def download(target_dir, url=SOURCE_URL, progress=None, cancel=None, *, signature_check=None,
             min_size=MIN_SIZE, max_size=MAX_SIZE, timeout=30, urlopen=None):
    """Downloads NVIDIA's nvngx_dlss.dll to target_dir (created). progress(done, total) is called as it
    comes (total 0 when unknown); cancel is a threading.Event checked on every chunk. The file is written
    as a temporary name, checked, and only then renamed to nvngx_dlss.dll. Returns the Component of the
    new file. Raises DlssError(key, detail) and leaves no partial file. HTTPS with certificate
    verification (plain http only to the local machine, for the tests). Nothing is executed."""
    _check_url(url)
    cancel = cancel or threading.Event()
    target_dir = Path(target_dir)
    temp = target_dir / TEMP_NAME
    final = target_dir / DLL_NAME
    try:
        target_dir.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        raise DlssError('write', error)
    try:
        request = urllib.request.Request(url, headers={'User-Agent': 'infamous-pc-launcher'})
        opener = urlopen or (lambda r: urllib.request.urlopen(r, timeout=timeout, context=ssl.create_default_context()))
        with opener(request) as response:
            if getattr(response, 'status', 200) != 200:
                raise DlssError('network', f'HTTP {response.status}')
            total = int(response.headers.get('Content-Length') or 0)
            if total and not min_size <= total <= max_size:
                raise DlssError('size', total)
            done = 0
            with open(temp, 'wb') as out:
                while True:
                    if cancel.is_set():
                        raise DlssError('cancelled')
                    chunk = response.read(CHUNK)
                    if not chunk:
                        break
                    done += len(chunk)
                    if done > max_size:
                        raise DlssError('size', done)
                    out.write(chunk)
                    if progress:
                        progress(done, total)
            if total and done != total:
                raise DlssError('network', f'{done} of {total} bytes')
        found = inspect_nvidia_dll_for_download(temp, signature_check, min_size, max_size)
        try:
            os.replace(temp, final)
        except OSError as error:
            raise DlssError('write', error)
        return Component('ok', final, found.version)
    except DlssError:
        _remove(temp)
        raise
    except (urllib.error.URLError, OSError, ssl.SSLError, ValueError) as error:
        _remove(temp)
        raise DlssError('network', getattr(error, 'reason', error))


# The bridge, when the project publishes it: an asset named tlg_dlss.dll on one of the newest releases
# (pre-releases included) of the port's own repository. Nothing else is accepted.
RELEASES_API = 'https://api.github.com/repos/HayatoG/iss_pc/releases?per_page=15'
RELEASE_DOWNLOADS = 'https://github.com/HayatoG/iss_pc/releases/download/'
BRIDGE_TEMP = 'tlg_dlss.download.dll'
BRIDGE_MAX = 32 * 1024 * 1024


def bridge_asset(releases_url=RELEASES_API, timeout=15, urlopen=None):
    """(url, size) of the newest tlg_dlss.dll published on the port's releases; DlssError('unpublished')
    when none has it."""
    _check_url(releases_url)
    request = urllib.request.Request(releases_url, headers={'User-Agent': 'infamous-pc-launcher',
                                                            'Accept': 'application/vnd.github+json'})
    opener = urlopen or (lambda r: urllib.request.urlopen(r, timeout=timeout, context=ssl.create_default_context()))
    try:
        with opener(request) as response:
            rows = json.loads(response.read(4 * 1024 * 1024).decode('utf-8'))
    except (urllib.error.URLError, OSError, ssl.SSLError, ValueError) as error:
        raise DlssError('network', getattr(error, 'reason', error))
    for release in rows if isinstance(rows, list) else []:
        for asset in (release or {}).get('assets') or []:
            url = str(asset.get('browser_download_url') or '')
            if asset.get('name') == BRIDGE_NAME and url.startswith(RELEASE_DOWNLOADS) \
                    and isinstance(asset.get('size'), int) and 0 < asset['size'] <= BRIDGE_MAX:
                return url, asset['size']
    raise DlssError('unpublished')


def download_bridge(target_dir, progress=None, cancel=None, *, releases_url=RELEASES_API, timeout=30, urlopen=None):
    """Downloads the port's tlg_dlss.dll into target_dir: a temporary name, checked (a 64-bit DLL that
    exports TlgDlssGetApi, the exact size of the release), then renamed. Returns its Component."""
    url, size = bridge_asset(releases_url, urlopen=urlopen)
    _check_url(url)
    cancel = cancel or threading.Event()
    target_dir = Path(target_dir)
    temp, final = target_dir / BRIDGE_TEMP, target_dir / BRIDGE_NAME
    try:
        target_dir.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        raise DlssError('write', error)
    try:
        request = urllib.request.Request(url, headers={'User-Agent': 'infamous-pc-launcher'})
        opener = urlopen or (lambda r: urllib.request.urlopen(r, timeout=timeout, context=ssl.create_default_context()))
        done = 0
        with opener(request) as response, open(temp, 'wb') as out:
            while True:
                if cancel.is_set():
                    raise DlssError('cancelled')
                chunk = response.read(CHUNK)
                if not chunk:
                    break
                done += len(chunk)
                if done > size:
                    raise DlssError('size', done)
                out.write(chunk)
                if progress:
                    progress(done, size)
        if done != size:
            raise DlssError('network', f'{done} of {size} bytes')
        if not inspect_bridge(temp).ok:
            raise DlssError('bridge_invalid')
        try:
            os.replace(temp, final)
        except OSError as error:
            raise DlssError('write', error)
        return Component('ok', final)
    except DlssError:
        _remove(temp)
        raise
    except (urllib.error.URLError, OSError, ssl.SSLError, ValueError) as error:
        _remove(temp)
        raise DlssError('network', getattr(error, 'reason', error))


def inspect_nvidia_dll_for_download(path, signature_check, min_size, max_size):
    """The checks before a downloaded file replaces the DLL (the size limits are the caller's)."""
    size = Path(path).stat().st_size
    if not min_size <= size <= max_size:
        raise DlssError('size', size)
    with open(path, 'rb') as f:
        if f.read(2) != b'MZ':
            raise DlssError('format')
    valid, subject = (signature_check or signature)(path)
    if not valid:
        raise DlssError('signature', subject)
    return Component('ok', path, file_version(path))


def _remove(path):
    try:
        Path(path).unlink()
    except OSError:
        pass


# ------------------------------------------------------------------------------------ texts
TEXT = {
    'dlss_section': ('NVIDIA DLSS (renderer v2)', 'NVIDIA DLSS (renderer v2)'),
    'dlss_what': (
        'DLSS needs two files that the project does not distribute: NVIDIA\'s nvngx_dlss.dll (a GeForce RTX card and '
        'a recent driver are needed) and the bridge tlg_dlss.dll, an optional add-on. Without them the game uses '
        'FSR 3.1.',
        'O DLSS precisa de dois arquivos que o projeto não distribui: o nvngx_dlss.dll da NVIDIA (precisa de uma '
        'placa GeForce RTX e de um driver recente) e a ponte tlg_dlss.dll, um complemento opcional. Sem eles o jogo '
        'usa o FSR 3.1.'),
    'dlss_checking': ('Checking the NVIDIA DLL…', 'Conferindo a DLL da NVIDIA…'),
    'dlss_dll_ok': ('NVIDIA DLL: DLSS {version} ({path})', 'DLL da NVIDIA: DLSS {version} ({path})'),
    'dlss_dll_missing': ('NVIDIA DLL: not found', 'DLL da NVIDIA: não encontrada'),
    'dlss_dll_invalid': ('NVIDIA DLL: refused ({why})', 'DLL da NVIDIA: recusada ({why})'),
    'dlss_why_format': ('not a Windows DLL', 'não é uma DLL do Windows'),
    'dlss_why_size': ('unexpected size', 'tamanho inesperado'),
    'dlss_why_signature': ('not signed by NVIDIA', 'não assinada pela NVIDIA'),
    'dlss_why_read': ('cannot be read', 'não pode ser lida'),
    'dlss_why_bridge': ('not the bridge', 'não é a ponte'),
    'dlss_bridge_ok': ('Bridge tlg_dlss.dll: ready ({path})', 'Ponte tlg_dlss.dll: pronta ({path})'),
    'dlss_bridge_missing': ('Bridge tlg_dlss.dll: not found (the optional add-on; build it with '
                            'packaging/windows/build_dlss.sh)',
                            'Ponte tlg_dlss.dll: não encontrada (o complemento opcional; construa com '
                            'packaging/windows/build_dlss.sh)'),
    'dlss_bridge_invalid': ('Bridge tlg_dlss.dll: not a valid bridge', 'Ponte tlg_dlss.dll: não é uma ponte válida'),
    'dlss_ready': ('DLSS can be used: pick it in "Temporal upscaler" (needs a GeForce RTX card).',
                   'O DLSS pode ser usado: escolha-o em "Upscaler temporal" (precisa de uma placa GeForce RTX).'),
    'dlss_download': ('Download DLSS from NVIDIA', 'Baixar o DLSS da NVIDIA'),
    'dlss_download_again': ('Download again', 'Baixar de novo'),
    'dlss_cancel': ('Cancel', 'Cancelar'),
    'dlss_confirm': (
        'This downloads nvngx_dlss.dll, about {mb} MB, from NVIDIA\'s official repository:\n{url}\n'
        'The file belongs to NVIDIA and is under NVIDIA\'s licence ({repo}); the project does not distribute it. '
        'The launcher checks that it is signed by NVIDIA Corporation before keeping it. Download?',
        'Isto baixa o nvngx_dlss.dll, cerca de {mb} MB, do repositório oficial da NVIDIA:\n{url}\n'
        'O arquivo é da NVIDIA e está sob a licença dela ({repo}); o projeto não o distribui. O launcher confere que '
        'ele é assinado pela NVIDIA Corporation antes de guardá-lo. Baixar?'),
    'dlss_confirm_yes': ('Download', 'Baixar'),
    'dlss_progress': ('Downloading… {done} of {total} MB', 'Baixando… {done} de {total} MB'),
    'dlss_progress_unknown': ('Downloading… {done} MB', 'Baixando… {done} MB'),
    'dlss_done': ('Done: DLSS {version} is in {path}.', 'Pronto: o DLSS {version} está em {path}.'),
    'dlss_auto_working': ('DLSS selected: downloading what it needs (NVIDIA\'s nvngx_dlss.dll from github.com/NVIDIA/DLSS, '
                          'under NVIDIA\'s licence, and the port\'s bridge)…',
                          'DLSS escolhido: baixando o que ele precisa (o nvngx_dlss.dll da NVIDIA, de github.com/NVIDIA/DLSS, '
                          'sob a licença da NVIDIA, e a ponte do port)…'),
    'dlss_auto_ready': ('DLSS is ready: both files are in place. It applies on the next start.',
                        'O DLSS está pronto: os dois arquivos estão no lugar. Vale na próxima abertura.'),
    'dlss_auto_partial': ('DLSS is not ready yet: {detail} Until then the game uses FSR 3.1.',
                          'O DLSS ainda não está pronto: {detail} Até lá o jogo usa o FSR 3.1.'),
    'dlss_err_unpublished': ('The bridge tlg_dlss.dll is not published on the port\'s GitHub releases yet.',
                             'A ponte tlg_dlss.dll ainda não foi publicada nos lançamentos do port no GitHub.'),
    'dlss_err_bridge_invalid': ('The downloaded bridge is not a valid tlg_dlss.dll; it was discarded.',
                                'A ponte baixada não é um tlg_dlss.dll válido; foi descartada.'),
    'dlss_err_url': ('The address is not allowed (HTTPS only).', 'O endereço não é permitido (só HTTPS).'),
    'dlss_err_network': ('The download failed: {detail}. Check the connection and try again; nothing was kept.',
                         'O download falhou: {detail}. Verifique a conexão e tente de novo; nada foi guardado.'),
    'dlss_err_cancelled': ('Download cancelled; nothing was kept.', 'Download cancelado; nada foi guardado.'),
    'dlss_err_size': ('The file has an unexpected size ({detail} bytes); it was discarded.',
                      'O arquivo tem um tamanho inesperado ({detail} bytes); foi descartado.'),
    'dlss_err_format': ('The file is not a Windows DLL; it was discarded.',
                        'O arquivo não é uma DLL do Windows; foi descartado.'),
    'dlss_err_signature': ('The file is not signed by NVIDIA Corporation ({detail}); it was discarded.',
                           'O arquivo não é assinado pela NVIDIA Corporation ({detail}); foi descartado.'),
    'dlss_err_write': ('The file could not be saved: {detail}', 'Não foi possível salvar o arquivo: {detail}'),
}
