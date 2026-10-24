# SPDX-License-Identifier: GPL-2.0-or-later
"""Release check, no Qt: version names, comparison, the notes in the player's language and the one
HTTPS GET the Updates screen makes. Downloading and installing a release is updater.py, and only
when the player asks for it."""
import json
import re
import socket
import time
import urllib.error
import urllib.request

from . import cacerts

API_URL = 'https://api.github.com/repos/HayatoG/iss_pc/releases/latest'
RELEASES_URL = 'https://github.com/HayatoG/iss_pc/releases/latest'
RELEASE_PREFIX = 'https://github.com/HayatoG/iss_pc/releases/'
TIMEOUT = 10


class UpdateError(Exception):
    """kind: offline | tls | timeout | limit | http | bad | none (the key suffix of the friendly message).
    reset: for 'limit', when GitHub takes requests again (Unix time), None when it did not say."""

    def __init__(self, kind, detail='', reset=None):
        super().__init__(f'{kind}: {detail}')
        self.kind, self.detail, self.reset = kind, detail, reset


def limit_reset(code, headers, now=None):
    """For a 403/429 answer of the API: None when it is not a rate limit, else when requests are
    taken again (Unix time; 0 when GitHub did not say). GitHub's limit for requests without login
    is 60 an hour per IP address, shared by every program of the network: X-RateLimit-Remaining 0
    and X-RateLimit-Reset, or Retry-After (seconds) for its secondary limit."""
    headers = headers or {}
    now = time.time() if now is None else now
    retry = str(headers.get('Retry-After') or '').strip()
    if retry.isdigit():
        return int(now) + int(retry)
    if str(headers.get('X-RateLimit-Remaining') or '').strip() == '0':
        reset = str(headers.get('X-RateLimit-Reset') or '').strip()
        return int(reset) if reset.isdigit() else 0
    return 0 if code == 429 else None


def error_text(error, t):
    """The Updates screen's message for an UpdateError; t(key, **fmt) is the launcher's translation.
    A rate limit with a known reset says when to try again (local time)."""
    if error.kind == 'limit' and error.reset:
        return t('upd_err_limit_until', time=time.strftime('%H:%M', time.localtime(error.reset)))
    return t('upd_err_' + error.kind, detail=error.detail)


# ------------------------------------------------------------------------------------ versions
_VERSION_RE = re.compile(r'^v?(\d{4})\.(\d{1,2})\.(\d{1,2})(?:-([0-9a-f]{4,40}))?(\+dirty)?$', re.IGNORECASE)
_COMMIT_RE = re.compile(r'^([0-9a-f]{7,40})(\+dirty)?$', re.IGNORECASE)


def parse_version(text):
    """((year, month, day) or None, commit or '') of 'v2026.10.15-abc1234', 'v2026.10.15' or a bare
    commit hash (the "Build:" line of a log). None for anything else."""
    text = (text or '').strip()
    m = _VERSION_RE.match(text)
    if m:
        return (int(m.group(1)), int(m.group(2)), int(m.group(3))), (m.group(4) or '').lower()
    m = _COMMIT_RE.match(text)
    if m:
        return None, m.group(1).lower()
    return 'bad'


def _same_commit(a, b):
    n = min(len(a), len(b))
    return n >= 7 and a[:n] == b[:n]


def compare(current, latest):
    """How the latest release relates to the one the player runs:
    'newer'     the latest is newer (an update exists)
    'different' the latest is another build we cannot order (e.g. only the commit of a log is known)
    'same'      the player has the latest
    'older'     the player runs something newer than the latest release (a test build)
    'unknown'   the version of this install is not known"""
    cur, new = parse_version(current), parse_version(latest)
    if cur == 'bad' or new == 'bad' or not current or not latest:
        return 'unknown'
    (cur_date, cur_commit), (new_date, new_commit) = cur, new
    if cur_date and new_date:
        if new_date != cur_date:
            return 'newer' if new_date > cur_date else 'older'
        if cur_commit and new_commit and not _same_commit(cur_commit, new_commit):
            return 'newer'    # two builds on the same day: the published one is the one to have
        return 'same'
    if cur_commit and new_commit:
        return 'same' if _same_commit(cur_commit, new_commit) else 'different'
    return 'unknown'


def update_available(current, latest):
    return compare(current, latest) in ('newer', 'different')


# ----------------------------------------------------------------------------------------- notes
_SECTION_TITLES = {'pt': ('novidades',), 'en': ("what's new", 'whats new', 'what’s new')}


def extract_notes(body, portuguese):
    """The release notes in the interface language. The body the CI writes has a "## Novidades"
    (pt-BR) and a "## What's new" (en-US) section; <!-- notes:pt-BR --> ... <!-- /notes --> blocks
    (the PR format of release_notes.sh) are understood too. Other text is kept only when neither
    exists, so a hand-written release still shows something."""
    body = (body or '').replace('\r\n', '\n').replace('\r', '\n')
    lang = 'pt-BR' if portuguese else 'en-US'
    blocks = re.findall(r'<!--\s*notes:' + lang + r'\s*-->(.*?)<!--\s*/notes\s*-->', body, re.S | re.I)
    if blocks:
        return _clean('\n'.join(blocks))
    wanted = _SECTION_TITLES['pt' if portuguese else 'en']
    lines, on, found = [], False, False
    for line in body.split('\n'):
        heading = re.match(r'^#{1,3}\s*(.+?)\s*#*$', line)
        if heading:
            title = heading.group(1).strip().lower()
            if title in wanted:
                on, found = True, True
                continue
            if on:
                break
            continue
        if on:
            lines.append(line)
    if found:
        return _clean('\n'.join(lines))
    if re.search(r'^#{1,3}\s*(novidades|what.s new)\s*$', body, re.M | re.I):
        return ''
    return _clean(re.sub(r'<!--.*?-->', '', body, flags=re.S))


def _clean(text):
    text = re.sub(r'<!--.*?-->', '', text, flags=re.S)
    out = []
    for line in text.split('\n'):
        line = line.rstrip()
        line = re.sub(r'^\s*[-*]\s+', '• ', line)
        line = re.sub(r'\*\*(.+?)\*\*', r'\1', line)
        line = re.sub(r'`([^`]+)`', r'\1', line)
        out.append(line)
    return re.sub(r'\n{3,}', '\n\n', '\n'.join(out)).strip()


# --------------------------------------------------------------------------------------- network
def user_agent(version=''):
    return f'iss_pc-launcher/{version or "unknown"} (+https://github.com/HayatoG/iss_pc)'


def fetch_latest(version='', opener=None, url=API_URL, timeout=TIMEOUT, etag=''):
    """The one request the launcher makes: GET the latest release. Sends no data of the player,
    only the User-Agent that names the port. Returns {'tag','name','url','body','published','assets',
    'etag'} (assets: [{'name','url','size'}] of the files whose download URL is HTTPS on our own
    releases). With the etag of the release seen before it asks only whether that changed: None
    when it did not (a 304, which GitHub does not count against its limit).
    Raises UpdateError with a kind the screen turns into a friendly message."""
    headers = {'User-Agent': user_agent(version), 'Accept': 'application/vnd.github+json'}
    if etag:
        headers['If-None-Match'] = etag
    request = urllib.request.Request(url, headers=headers)
    opener = opener or urllib.request.build_opener(cacerts.https_handler()).open
    try:
        with opener(request, timeout=timeout) as response:
            raw = response.read(2 * 1024 * 1024)
            answer_etag = str((getattr(response, 'headers', None) or {}).get('ETag') or '')
    except urllib.error.HTTPError as error:
        if error.code == 304 and etag:
            return None
        if error.code == 404:
            raise UpdateError('none') from error
        if error.code in (403, 429):
            reset = limit_reset(error.code, error.headers)
            if reset is not None:
                raise UpdateError('limit', str(error.code), reset=reset or None) from error
        raise UpdateError('http', str(error.code)) from error
    except (socket.timeout, TimeoutError) as error:
        raise UpdateError('timeout') from error
    except urllib.error.URLError as error:
        if isinstance(error.reason, (socket.timeout, TimeoutError)):
            raise UpdateError('timeout') from error
        if cacerts.is_cert_error(error):
            raise UpdateError('tls', str(error.reason)) from error
        raise UpdateError('offline', str(error.reason)) from error
    except OSError as error:
        if cacerts.is_cert_error(error):
            raise UpdateError('tls', str(error)) from error
        raise UpdateError('offline', str(error)) from error
    try:
        data = json.loads(raw.decode('utf-8'))
        tag = str(data['tag_name'])
    except (ValueError, KeyError, TypeError, UnicodeDecodeError) as error:
        raise UpdateError('bad') from error
    page = str(data.get('html_url') or '')
    assets = []
    for asset in data.get('assets') or []:
        if not isinstance(asset, dict):
            continue
        link = str(asset.get('browser_download_url') or '')
        if link.startswith(RELEASE_PREFIX + 'download/') and isinstance(asset.get('size'), int):
            assets.append({'name': str(asset.get('name') or ''), 'url': link, 'size': asset['size'],
                           'digest': str(asset.get('digest') or '')})
    return {'tag': tag, 'name': str(data.get('name') or tag), 'body': str(data.get('body') or ''),
            'url': page if page.startswith(RELEASE_PREFIX) else RELEASES_URL,    # only our own releases page opens
            'published': str(data.get('published_at') or '')[:10], 'assets': assets, 'etag': answer_etag}
