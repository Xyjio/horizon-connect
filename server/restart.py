"""Starts the game again once it has closed. The plugin runs it (plugin/online.c, game_restart) right before it
closes the game, e.g. after an update:

    python restart.py --pid <the game's process id> -- <azimuth.exe> --skip-launcher --game=<title>

It waits up to 10 minutes for the process to end, then starts the command, detached and in its own folder.
"""
import argparse, ctypes, os, subprocess, sys, time

SYNCHRONIZE = 0x00100000
DETACHED_PROCESS, CREATE_NEW_PROCESS_GROUP = 0x00000008, 0x00000200


def wait_for_exit(pid, timeout_s):
    k32 = ctypes.windll.kernel32
    h = k32.OpenProcess(SYNCHRONIZE, False, pid)
    if not h:
        return True                            # already gone
    try:
        return k32.WaitForSingleObject(h, int(timeout_s * 1000)) == 0
    finally:
        k32.CloseHandle(h)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--pid', type=int, required=True)
    ap.add_argument('command', nargs=argparse.REMAINDER)
    a = ap.parse_args()
    cmd = a.command[1:] if a.command[:1] == ['--'] else a.command
    if not cmd:
        return 2
    if not wait_for_exit(a.pid, 600):
        return 1                               # still running: never start a second copy
    time.sleep(2)                              # let Azimuth let go of its files
    subprocess.Popen(cmd, cwd=os.path.dirname(cmd[0]) or None, close_fds=True,
                     creationflags=DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP)
    return 0


if __name__ == '__main__':
    sys.exit(main())
