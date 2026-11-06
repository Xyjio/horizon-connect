"""Builds the release package dist/Horizon-Connect-<version>.zip from this repository.

    python tools/package.py [--dll build/HorizonConnect.dll] [--python <embed zip or folder>] [--playitd <exe>]

What goes into it:
    GUIDE.txt                                           the player's guide (package/GUIDE.txt)
    Horizon Connect/mod.json                            the Extra Options manifest (package/mod.json)
    Horizon Connect/plugins/HorizonConnect.dll          build it first with tools/build.ps1 or tools/build.sh
    Horizon Connect/plugins/HorizonConnect.ini          default settings (package/HorizonConnect.ini)
    Horizon Connect/plugins/HorizonConnect/server/      server/*.py and pefile (installed from PyPI)
    Horizon Connect/plugins/HorizonConnect/python/      the Windows embeddable Python (python.org)
    Horizon Connect/plugins/HorizonConnect/playit/      playitd.exe (playit.gg agent)
    Horizon Connect/plugins/HorizonConnect/Media/...    100 page files for the in-game menu (see plugin/menu.c)
    Horizon Connect/plugins/HorizonConnect/source/      the plugin's source code and the licences (GPL-3.0)

Python and playitd are downloaded into downloads/ the first time unless --python / --playitd point at local
copies.
"""
import argparse, json, os, shutil, subprocess, sys, tempfile, urllib.request, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PYTHON_URL = 'https://www.python.org/ftp/python/3.13.9/python-3.13.9-embed-amd64.zip'
PLAYITD_URL = 'https://github.com/playit-cloud/playit-agent/releases/download/v1.2.0/playitd-windows-x86_64-signed.exe'
POOL = 100   # must match PAGE_POOL in plugin/menu.c


def download(url, name):
    os.makedirs(os.path.join(ROOT, 'downloads'), exist_ok=True)
    path = os.path.join(ROOT, 'downloads', name)
    if not os.path.exists(path):
        print(f'downloading {url}')
        try:
            urllib.request.urlretrieve(url, path)
        except Exception as e:
            sys.exit(f'download failed ({e}); download it yourself and pass its path (see --help)')
    return path


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dll', default=os.path.join(ROOT, 'build', 'HorizonConnect.dll'))
    ap.add_argument('--python', help='the Python embeddable package (zip) or an unpacked copy of it')
    ap.add_argument('--playitd', help='playitd.exe for Windows (x86-64)')
    a = ap.parse_args()
    if not os.path.exists(a.dll):
        sys.exit(f'{a.dll} is missing: build it first (tools/build.ps1 or tools/build.sh)')
    version = json.load(open(os.path.join(ROOT, 'package', 'mod.json'), encoding='utf-8'))['version']

    stage = tempfile.mkdtemp(prefix='horizonconnect-')
    mod = os.path.join(stage, 'Horizon Connect')
    online = os.path.join(mod, 'plugins', 'HorizonConnect')
    copy(os.path.join(ROOT, 'package', 'GUIDE.txt'), os.path.join(stage, 'GUIDE.txt'))
    copy(os.path.join(ROOT, 'package', 'mod.json'), os.path.join(mod, 'mod.json'))
    copy(a.dll, os.path.join(mod, 'plugins', 'HorizonConnect.dll'))
    copy(os.path.join(ROOT, 'package', 'HorizonConnect.ini'), os.path.join(mod, 'plugins', 'HorizonConnect.ini'))

    # the server and pefile (it reads the game's executable)
    for f in ('fh2srv.py', 'nerd.py', 'relay.py', 'run_server.py', 'update.py', 'xls.py'):
        copy(os.path.join(ROOT, 'server', f), os.path.join(online, 'server', f))
    pip_target = tempfile.mkdtemp(prefix='pefile-')
    subprocess.check_call([sys.executable, '-m', 'pip', 'install', '--quiet', '--no-deps', '--target', pip_target, 'pefile'])
    copy(os.path.join(pip_target, 'pefile.py'), os.path.join(online, 'server', 'pefile.py'))
    shutil.copytree(os.path.join(pip_target, 'ordlookup'), os.path.join(online, 'server', 'ordlookup'),
                    ignore=shutil.ignore_patterns('__pycache__'))
    copy(os.path.join(ROOT, 'docs', 'LICENSE-pefile.txt'), os.path.join(online, 'server', 'pefile-LICENSE.txt'))

    # Python: the embeddable package; its ._pth lets the server's folder be found ("." = the script's folder)
    py = a.python or download(PYTHON_URL, os.path.basename(PYTHON_URL))
    pydir = os.path.join(online, 'python')
    if os.path.isdir(py):
        shutil.copytree(py, pydir)
    else:
        zipfile.ZipFile(py).extractall(pydir)
    pth = [f for f in os.listdir(pydir) if f.endswith('._pth')]
    if pth:
        with open(os.path.join(pydir, pth[0]), 'w', encoding='ascii') as f:
            f.write(pth[0].replace('._pth', '.zip') + '\n.\n')

    # playit.gg agent
    copy(a.playitd or download(PLAYITD_URL, 'playitd.exe'), os.path.join(online, 'playit', 'playitd.exe'))
    copy(os.path.join(ROOT, 'docs', 'LICENSE-playit.txt'), os.path.join(online, 'playit', 'LICENSE-playit.txt'))

    # the menu's page files: they must exist before the game starts (plugin/menu.c rewrites them as pages open)
    for i in range(1, POOL + 1):
        copy(os.path.join(ROOT, 'package', 'pool-page.xaml'), os.path.join(online, 'Media', 'UI', 'Scenes', 'Anthem', f'HorizonConnect_{i}.xaml'))

    # source and licences (GPL-3.0: the source goes with the DLL)
    for f in sorted(os.listdir(os.path.join(ROOT, 'plugin'))):
        copy(os.path.join(ROOT, 'plugin', f), os.path.join(online, 'source', f))
    copy(os.path.join(ROOT, 'LICENSE'), os.path.join(online, 'LICENSE-GPL3.txt'))
    copy(os.path.join(ROOT, 'docs', 'LICENSE-FH2Net-MIT.txt'), os.path.join(online, 'LICENSE-FH2Net-MIT.txt'))
    copy(os.path.join(ROOT, 'NOTICE.md'), os.path.join(online, 'NOTICE.md'))

    os.makedirs(os.path.join(ROOT, 'dist'), exist_ok=True)
    out = os.path.join(ROOT, 'dist', f'Horizon-Connect-{version}.zip')
    if os.path.exists(out):
        os.remove(out)
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for base, dirs, files in os.walk(stage):
            dirs[:] = [d for d in dirs if d != '__pycache__']
            for f in files:
                p = os.path.join(base, f)
                z.write(p, os.path.relpath(p, stage))
    shutil.rmtree(stage, ignore_errors=True)
    print(f'{out}: {os.path.getsize(out) // 1024} KB')


if __name__ == '__main__':
    main()
