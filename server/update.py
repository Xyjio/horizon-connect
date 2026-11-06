"""Updates Horizon Connect in place from the newest GitHub release. The plugin starts it hidden when the player
chooses the update row in the server list (plugin/online.c, update_install):

    python update.py --mod "<...\\mods\\Horizon Connect>" --version 1.0.3 [--zip <a release ZIP, for testing>]

1. asks GitHub for the newest release and stops unless it is newer than --version
2. downloads its Horizon-Connect-<version>.zip (only from this project's releases) and checks it
3. writes every changed file next to its target as <name>.update-new, then swaps them in: the old file becomes
   <name>.update-old (files in use, like the running DLL or python.exe, can be renamed but not overwritten). If a
   swap fails, the files swapped so far are put back.
4. HorizonConnect.ini keeps the player's settings; settings that are new in this version are added with their
   defaults. The menu's page files (Media) are left alone: the plugin rewrites them while the game runs.

The .update-old files are listed in plugins\\HorizonConnect\\update-old.txt; the plugin deletes them at the next
start. Exit code: 0 updated, 1 already up to date, 2 failed. Progress goes to stdout and update.log.
"""
import argparse, io, json, os, re, sys, time, urllib.request, zipfile

REPO = 'Xyjio/horizon-connect'
API = f'https://api.github.com/repos/{REPO}/releases/latest'
DOWNLOADS = f'https://github.com/{REPO}/releases/download/'
TOP = 'Horizon Connect/'                     # the mod folder inside the release ZIP
INI = 'plugins/HorizonConnect.ini'
KEEP = ('plugins/horizonconnect/media/',)    # rewritten by the plugin at run time: never replaced
OLD_LIST = 'plugins/HorizonConnect/update-old.txt'
LOG = None


def log(text):
    line = time.strftime('%H:%M:%S ') + text
    print(line, flush=True)
    if LOG:
        try:
            with open(LOG, 'a', encoding='utf-8') as f:
                f.write(line + '\n')
        except OSError:
            pass


def version_tuple(v):
    return tuple(int(x) for x in re.findall(r'\d+', v.split('-')[0])[:4])


def fetch(url, accept=None):
    req = urllib.request.Request(url, headers={'User-Agent': 'HorizonConnect-updater', **({'Accept': accept} if accept else {})})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def newest_release():
    rel = json.loads(fetch(API, 'application/vnd.github+json'))
    version = rel['tag_name'].lstrip('vV')
    assets = [a for a in rel.get('assets', []) if a['name'].lower() == f'horizon-connect-{version}.zip'.lower()]
    if not assets:
        assets = [a for a in rel.get('assets', []) if a['name'].lower().startswith('horizon-connect') and a['name'].lower().endswith('.zip')]
    url = assets[0]['browser_download_url'] if assets else None
    if url and not url.startswith(DOWNLOADS):
        raise RuntimeError(f'unexpected download address {url}')
    return version, url


def check_zip(z, version):
    bad = z.testzip()
    if bad:
        raise RuntimeError(f'the ZIP is damaged ({bad})')
    names = z.namelist()
    for n in names:
        if n.startswith('/') or '\\' in n or '..' in n.split('/') or ':' in n:
            raise RuntimeError(f'unsafe path in the ZIP: {n}')
    for need in (TOP + 'mod.json', TOP + 'plugins/HorizonConnect.dll'):
        if need not in names:
            raise RuntimeError(f'{need} is missing in the ZIP')
    got = json.loads(z.read(TOP + 'mod.json').decode('utf-8-sig')).get('version', '')
    if version and version_tuple(got) != version_tuple(version):
        raise RuntimeError(f'the ZIP is version {got}, not {version}')
    return got


def same(path, data):
    try:
        if os.path.getsize(path) != len(data):
            return False
        with open(path, 'rb') as f:
            return f.read() == data
    except OSError:
        return False


def free_name(path):
    """path.update-old, or .update-old2 ... when an older one can't be deleted (still in use)."""
    for i in range(1, 100):
        p = path + '.update-old' + ('' if i == 1 else str(i))
        if not os.path.exists(p):
            return p
        try:
            os.remove(p)
            return p
        except OSError:
            continue
    raise RuntimeError(f'no free name for the old {path}')


# ---- HorizonConnect.ini: keep the player's values, add new settings ----------------------------------------------

def ini_entries(text):
    """[(section, key, value line, comment lines before it)] in file order."""
    out, section, comments = [], '', []
    for line in text.splitlines():
        s = line.strip()
        if s.startswith('[') and s.endswith(']'):
            section, comments = s[1:-1].strip().lower(), []
        elif s.startswith(';') or s.startswith('#'):
            comments.append(line)
        elif '=' in s:
            out.append((section, s.split('=', 1)[0].strip().lower(), line, comments))
            comments = []
        else:
            comments = []
    return out


def merge_ini(mine, new):
    """The player's ini text with the settings of the new one it lacks (with their comments)."""
    have = {(sec, key) for sec, key, _, _ in ini_entries(mine)}
    missing = [e for e in ini_entries(new) if (e[0], e[1]) not in have]
    if not missing:
        return mine, []
    lines = mine.splitlines()
    for sec, key, line, comments in missing:
        # the end of the section: before the next header, not after its trailing blank lines
        start = next((i for i, l in enumerate(lines) if l.strip().lower() == f'[{sec}]'), None)
        if start is None:
            if lines and lines[-1].strip():
                lines.append('')
            lines += [f'[{sec}]'] + comments + [line]
            continue
        end = next((i for i in range(start + 1, len(lines)) if lines[i].strip().startswith('[')), len(lines))
        while end > start + 1 and not lines[end - 1].strip():
            end -= 1
        lines[end:end] = comments + [line]
    return '\r\n'.join(lines) + '\r\n', [f'[{m[0]}] {m[1]}' for m in missing]


# ---- install -------------------------------------------------------------------------------------------------------

def install(z, mod):
    changes = []                                 # (target, data)
    for n in z.namelist():
        if not n.startswith(TOP) or n.endswith('/'):
            continue
        rel = n[len(TOP):]
        if rel.lower() == INI.lower():
            continue
        target = os.path.join(mod, *rel.split('/'))
        if rel.lower().startswith(KEEP) and os.path.exists(target):
            continue
        data = z.read(n)
        if not same(target, data):
            changes.append((target, data))
    log(f'{len(changes)} files changed')

    staged = []
    try:
        for target, data in changes:
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target + '.update-new', 'wb') as f:
                f.write(data)
            staged.append(target)
    except OSError as e:
        for t in staged:
            try: os.remove(t + '.update-new')
            except OSError: pass
        raise RuntimeError(f'could not write the new files: {e}')

    done, olds = [], []                          # (target, old name or None)
    try:
        for target in staged:
            old = None
            if os.path.exists(target):
                old = free_name(target)
                os.rename(target, old)
                olds.append(old)
            done.append((target, old))
            os.rename(target + '.update-new', target)
    except OSError as e:
        log(f'swap failed ({e}); putting the old files back')
        for target, old in reversed(done):
            try:
                if not os.path.exists(target + '.update-new'):   # the new file is in place: take it out again
                    os.remove(target)
                if old:
                    os.rename(old, target)
            except OSError as e2:
                log(f'could not restore {target}: {e2}')
        for t in staged:
            try: os.remove(t + '.update-new')
            except OSError: pass
        raise RuntimeError('the update was undone')

    if olds:
        with open(os.path.join(mod, *OLD_LIST.split('/')), 'a', encoding='utf-8') as f:
            for o in olds:
                f.write(os.path.relpath(o, mod) + '\n')

    ini = os.path.join(mod, *INI.split('/'))
    try:
        with open(ini, 'rb') as f:
            raw = f.read()
        unicode = raw[:2] == b'\xff\xfe'            # the plugin keeps it as UTF-16 since 1.0.6
        mine = raw.decode('utf-16') if unicode else raw.decode('latin-1')
        merged, added = merge_ini(mine, z.read(TOP + INI).decode('latin-1'))
        if added:
            with open(ini + '.update-new', 'wb') as f:
                f.write(('\ufeff' + merged).encode('utf-16-le') if unicode else merged.encode('latin-1'))
            os.replace(ini + '.update-new', ini)
            log('new settings: ' + ', '.join(added))
    except (OSError, KeyError) as e:
        log(f'settings not merged ({e}); the old ones stay')
    return len(changes)


def main():
    global LOG
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--mod', required=True, help='the "Horizon Connect" mod folder')
    ap.add_argument('--version', required=True, help='the installed version')
    ap.add_argument('--zip', help='install this release ZIP instead of downloading the newest (testing)')
    a = ap.parse_args()
    mod = os.path.abspath(a.mod)
    if not os.path.isfile(os.path.join(mod, 'plugins', 'HorizonConnect.dll')):
        log(f'{mod} is not the Horizon Connect folder')
        return 2
    LOG = os.path.join(mod, 'plugins', 'HorizonConnect', 'update.log')
    log(f'Horizon Connect {a.version} in {mod}')
    try:
        if a.zip:
            z = zipfile.ZipFile(a.zip)
            version = check_zip(z, None)
        else:
            version, url = newest_release()
            if version_tuple(version) <= version_tuple(a.version):
                log(f'up to date (newest release {version})')
                return 1
            if not url:
                raise RuntimeError(f'release {version} has no ZIP')
            log(f'downloading {version}: {url}')
            data = fetch(url)
            log(f'{len(data) // 1024} KB')
            z = zipfile.ZipFile(io.BytesIO(data))
            check_zip(z, version)
        n = install(z, mod)
        log(f'updated to {version} ({n} files); restart the game')
        return 0
    except Exception as e:
        log(f'update failed: {e}')
        return 2


if __name__ == '__main__':
    sys.exit(main())
