"""Starts fh2srv.py and copies everything it prints (relay events included) into logs\\server-<date>.log,
so a problem can be looked at after the window is closed. Arguments are passed to fh2srv.py unchanged."""
import datetime, glob, os, runpy, sys, time

here = os.path.dirname(os.path.abspath(__file__))
os.makedirs(os.path.join(here, 'logs'), exist_ok=True)


def tidy():
    """Old records don't pile up: the newest 30 server logs are kept, a capture list over 20 MB is moved to
    *.old.jsonl (the one before goes), and request bodies (captures/*.bin) older than 7 days go."""
    try:
        logs = sorted(glob.glob(os.path.join(here, 'logs', 'server-*.log')), key=os.path.getmtime)
        for f in logs[:-30]:
            os.remove(f)
        caps = os.path.join(here, 'captures')
        for name in ('requests.jsonl', 'udp.jsonl'):
            f = os.path.join(caps, name)
            if os.path.isfile(f) and os.path.getsize(f) > 20 * 1024 * 1024:
                os.replace(f, f[:-len('.jsonl')] + '.old.jsonl')
        week = time.time() - 7 * 86400
        for f in glob.glob(os.path.join(caps, '*.bin')):
            if os.path.getmtime(f) < week:
                os.remove(f)
    except OSError:
        pass


tidy()
log = open(os.path.join(here, 'logs', datetime.datetime.now().strftime('server-%Y-%m-%d_%H-%M-%S.log')),
           'a', encoding='utf-8', buffering=1)


class Tee:
    def __init__(self, stream):
        self.stream = stream

    def write(self, s):
        try:
            self.stream.write(s)
        except UnicodeEncodeError:
            self.stream.write(s.encode(self.stream.encoding or 'ascii', 'replace').decode(self.stream.encoding or 'ascii'))
        log.write(s)
        return len(s)

    def flush(self):
        self.stream.flush()
        log.flush()


sys.stdout, sys.stderr = Tee(sys.stdout), Tee(sys.stderr)
sys.argv = [os.path.join(here, 'fh2srv.py')] + sys.argv[1:]
runpy.run_path(sys.argv[0], run_name='__main__')
