"""Starts fh2srv.py and copies everything it prints (relay events included) into logs\\server-<date>.log,
so a problem can be looked at after the window is closed. Arguments are passed to fh2srv.py unchanged."""
import datetime, os, runpy, sys

here = os.path.dirname(os.path.abspath(__file__))
os.makedirs(os.path.join(here, 'logs'), exist_ok=True)
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
