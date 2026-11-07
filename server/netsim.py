"""A bad internet line for testing, between the relay and the players who come through the tunnel.

Used only when a file `netsim.json` lies next to relay.py (it is not part of a release), e.g.

    {"delay_ms": 60, "jitter_ms": 30, "loss": 0.03}

Every packet to and from such a player (any address but 127.0.0.1, which is the host's own console) is held back
delay_ms plus up to jitter_ms (each way, so the ping is about twice that) and lost with the chance `loss` (each
way). With jitter, packets can overtake each other, as on the internet. The host's own console is not touched."""
import heapq, itertools, json, os, queue, random, socket, threading, time


def load(folder):
    path = os.path.join(folder, 'netsim.json')
    if not os.path.isfile(path):
        return None
    with open(path, encoding='utf-8-sig') as f:
        cfg = json.load(f)
    return {'delay': max(0.0, float(cfg.get('delay_ms', 60))) / 1000, 'jitter': max(0.0, float(cfg.get('jitter_ms', 30))) / 1000,
            'loss': min(max(float(cfg.get('loss', 0.0)), 0.0), 0.5)}


class SimSocket:
    """sendto / recvfrom like the socket it wraps, through the bad line for the tunnel's players."""

    def __init__(self, sock, cfg):
        self.sock, self.cfg = sock, cfg
        self.order = itertools.count()
        self.out, self.inc = [], []                  # heaps of (due, n, data, addr)
        self.cv = threading.Condition()
        self.ready = queue.Queue()                   # packets that have arrived: (data, addr)
        self.sent = self.lost = 0
        threading.Thread(target=self.reader, daemon=True).start()
        threading.Thread(target=self.pump, daemon=True).start()

    @staticmethod
    def remote(addr):
        host = addr[0]
        return host not in ('127.0.0.1', '::ffff:127.0.0.1', '::1')

    def hold(self, heap, data, addr):
        if random.random() < self.cfg['loss']:
            self.lost += 1
            return
        due = time.monotonic() + self.cfg['delay'] + random.random() * self.cfg['jitter']
        with self.cv:
            heapq.heappush(heap, (due, next(self.order), data, addr))
            self.cv.notify()

    def sendto(self, data, addr):
        if not self.remote(addr):
            return self.sock.sendto(data, addr)
        self.sent += 1
        self.hold(self.out, data, addr)
        return len(data)

    def reader(self):
        while True:
            try:
                data, addr = self.sock.recvfrom(65536)
            except ConnectionResetError:
                continue
            except OSError:
                time.sleep(0.01)
                continue
            if self.remote(addr):
                self.hold(self.inc, data, addr)
            else:
                self.ready.put((data, addr))

    def pump(self):
        while True:
            with self.cv:
                now = time.monotonic()
                due = [h[0][0] for h in (self.out, self.inc) if h]
                if not due or min(due) > now:
                    self.cv.wait(timeout=(min(due) - now) if due else 0.05)
                    continue
                go_out = [heapq.heappop(self.out) for _ in range(len(self.out)) if self.out and self.out[0][0] <= now]
                go_in = [heapq.heappop(self.inc) for _ in range(len(self.inc)) if self.inc and self.inc[0][0] <= now]
            for _, _, data, addr in go_out:
                try:
                    self.sock.sendto(data, addr)
                except OSError:
                    pass
            for _, _, data, addr in go_in:
                self.ready.put((data, addr))

    def recvfrom(self, size):
        return self.ready.get()
