"""Online traffic: the same civilian cars for every player in the session.

Ported from FH2MP (GPL-3.0, https://github.com/Charelene/FH2MP, native/internal/traffic: manager.go, the port of
the game's own Traffic::TrafficManager, and traffic.go, the wire and the join sequence).

How the game does it: every console runs the traffic around its own car, but asks the server for each car it wants
to add (RequestTraffic). The server grants car ids; the console spawns the car and reports it (TrafficSpawned) and
from then on sends its state (FullSync / RouteProgress / FullRoute / Stationary). The server hands each car to the
other consoles near it (AddTraffic with the latest state), passes the owner's updates on to them, and moves the
ownership to the console that is closest (or that hit it), so a car is driven by one console and seen by all.

Messages: console -> server 71 (ServerTrafficMessage: kind, a, s14, u5, b, flag, seq) and 16/17/73/74 (a car's state);
server -> console 72 (ClientTrafficMessage) and 18/19/75/76 (the state, as the console's own update minus a few fields).
"""

import math
import os
import sqlite3
import struct
import time

# console -> server (TrafficServerMessageType)
(S_REQUEST_TRAFFIC, S_WITHDRAW_REQUEST, S_REQUEST_DESPAWN, S_TRAFFIC_SPAWNED, S_TRAFFIC_REMOVED,
 S_CAN_TAKE_OWNERSHIP, S_REQUEST_OWNERSHIP, S_CAR_MODEL_LOADED, S_CAR_MODEL_UNLOADED) = range(9)
S_NAMES = ['RequestTraffic', 'WithdrawTrafficRequest', 'RequestDespawn', 'TrafficSpawned', 'TrafficRemoved',
           'CanTakeOwnership', 'RequestOwnership', 'CarModelLoaded', 'CarModelUnloaded']
# server -> console (TrafficClientMessageType)
(C_TRAFFIC_LIMIT, C_SPAWN_GRANTED, C_SPAWN_CANCELED, C_WITHDRAW_ACCEPTED, C_ADD_TRAFFIC, C_REMOVE_TRAFFIC,
 C_TAKE_OWNERSHIP, C_DROP_OWNERSHIP, C_OWNERSHIP_DENIED, C_LOAD_CAR_MODEL, C_REMOVE_CAR_MODEL, C_UNLOAD_CAR_MODEL,
 C_ALL_MODELS_SENT, C_INITIAL_JOIN_COMPLETE) = range(14)

MSG_SERVER_FULL_SYNC, MSG_SERVER_ROUTE_PROGRESS, MSG_SERVER_MESSAGE = 16, 17, 71
MSG_SERVER_FULL_ROUTE, MSG_SERVER_STATIONARY = 73, 74
MSG_CLIENT_MESSAGE = 72
UPDATE_LAYOUT = {
    MSG_SERVER_FULL_SYNC: [8, 32, 32, 32, 32] + [16] * 9 + [32, 1, 1],
    MSG_SERVER_ROUTE_PROGRESS: [8, 32, 32, 32, 32, 32, 32],
    MSG_SERVER_FULL_ROUTE: [8] + [16] * 8 + [2, 32, 32, 32, 32, 32, 32],
    MSG_SERVER_STATIONARY: [8, 32, 32, 32, 16, 16, 16, 32, 1],
}
RELAY_LAYOUT = {
    MSG_SERVER_FULL_SYNC: UPDATE_LAYOUT[MSG_SERVER_FULL_SYNC],
    MSG_SERVER_ROUTE_PROGRESS: UPDATE_LAYOUT[MSG_SERVER_ROUTE_PROGRESS][:4],
    MSG_SERVER_FULL_ROUTE: UPDATE_LAYOUT[MSG_SERVER_FULL_ROUTE][:-3],
    MSG_SERVER_STATIONARY: UPDATE_LAYOUT[MSG_SERVER_STATIONARY],
}
RELAY_ID = {MSG_SERVER_FULL_SYNC: 18, MSG_SERVER_ROUTE_PROGRESS: 19, MSG_SERVER_FULL_ROUTE: 75, MSG_SERVER_STATIONARY: 76}
UPDATE_NAMES = {16: 'FullSync', 17: 'RouteProgress', 73: 'FullRoute', 74: 'Stationary'}

# the manager's tunables (TrafficManager)
MAX_CARS, HARD_CAP, BASE_CAPACITY = 216, 18, 10
EVICT_MARGIN, RECENT_MS, RECENT_FACTOR = 100.0, 10000.0, 0.01
ROUND_BUDGET, CHANGE_COST, DEFAULT_RADIUS = 10, 5, 250
OWNER_BONUS, CHANGE_GAP_MS = 20, 2000.0
TRAFFIC_LIMIT = HARD_CAP
# the world's timers
# NO_REPORT_S: FH2MP waits 2 s for a joining console's CarModelLoaded reports; over a slow link (simulated 70 ms +
# 50 ms, 5 % loss, 2026-11-23) they came 2.05 s after a rejoin, InitialJoinComplete had gone out with no models
# known and the player got only some of the cars around. 5 s.
GRANT_TIMEOUT_S, REPORT_SETTLE_S, NO_REPORT_S, JOIN_WAIT_S = 15.0, 0.5, 5.0, 15.0
SUMMARY_S, TICK_S, MAX_MESSAGES_PER_S = 30.0, 0.2, 400
RADIUS_KINDS = {0, 1, 2, 4, 5, 6}


def load_models(game_exe):
    r"""The traffic car models (TrafficCars in the game's database, Mount\media\stripped\gamedbRC.slt)."""
    mount = os.path.dirname(game_exe)
    for sub in (('media', 'stripped'), ('mediapc', 'stripped'), ('media',), ('mediapc',)):
        p = os.path.join(mount, *sub, 'gamedbRC.slt')
        if os.path.isfile(p):
            break
    else:
        print('traffic: the game database (gamedbRC.slt) was not found; no online traffic', flush=True)
        return []
    try:
        c = sqlite3.connect(p)             # only read
        try:
            return sorted({int(r[0]) for r in c.execute('SELECT CarId FROM TrafficCars')})
        finally:
            c.close()
    except sqlite3.Error as e:
        print(f'traffic: the game database can\'t be read ({e}); no online traffic', flush=True)
        return []


def f32(v):
    return struct.unpack('<f', struct.pack('<I', v & 0xFFFFFFFF))[0]


def signed14(v):
    return v - (1 << 14) if v & (1 << 13) else v


def dist2(a, b):
    return (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2


def update_position(mtype, fields):
    if mtype == MSG_SERVER_FULL_SYNC:
        words = fields[2:5]
    elif mtype == MSG_SERVER_STATIONARY:
        words = fields[1:4]
    else:
        words = fields[-3:]
    return tuple(f32(w) for w in words)


# ---- the manager (TrafficManager): who holds which car, who owns it -------------------------------------------------

class Player:
    def __init__(self, key):
        self.key, self.universe, self.pos, self.radius = key, -1, None, DEFAULT_RADIUS
        self.models = {}                     # model -> cars of it this console holds
        self.held, self.can_take, self.removing, self.granted, self.canceled = set(), set(), set(), set(), set()

    def capacity(self):
        return BASE_CAPACITY + sum(1 for c in self.models.values() if c > 0) - len(self.held)

    def settled(self):
        return all(v in self.can_take for v in self.held)


class Car:
    def __init__(self, vid, model, universe, info, owner, changed):
        self.vid, self.model, self.universe, self.info, self.owner = vid, model, universe, info, owner
        self.pos, self.holders, self.changed = None, [], changed


class CarData:
    """A car's AddTraffic values and the last state of each kind (what a new holder needs)."""
    def __init__(self, struct5):
        self.struct, self.full_sync, self.full_route, self.stationary, self.order = struct5, None, None, None, {}

    def latest(self):
        s = [(self.order.get(m, 0), m, f) for m, f in ((MSG_SERVER_FULL_SYNC, self.full_sync),
             (MSG_SERVER_FULL_ROUTE, self.full_route), (MSG_SERVER_STATIONARY, self.stationary)) if f]
        s.sort(key=lambda e: e[0])
        moved = max((n for n, m, f in s if m != MSG_SERVER_STATIONARY), default=-1)
        return [(m, f) for n, m, f in s if not (m == MSG_SERVER_STATIONARY and n < moved)]


class Manager:
    def __init__(self, out):
        self.out = out
        self.players, self.cars, self.reserved, self.requests = {}, {}, {}, {}
        self.placing = {}
        self.refusal, self.resent = '', False

    def join(self, k):
        return self.players.setdefault(k, Player(k))

    def leave(self, k):
        p = self.players.get(k)
        if p:
            self.release(p, True)
            del self.players[k]

    def reset(self, k):
        p = self.players.get(k)
        if p:
            self.release(p, True)
            p.removing = set()

    def set_position(self, k, pos):
        if k in self.players:
            self.players[k].pos = pos

    def set_radius(self, k, r):
        if r > 0 and k in self.players:
            self.players[k].radius = r

    def set_universe(self, k, u):
        p = self.players.get(k)
        if p and p.universe != u:
            if p.universe != -1:
                self.release(p, False)
            p.universe = u

    def model_loaded(self, k, model):
        p = self.players.get(k)
        if p:
            p.models.setdefault(model, 0)

    def model_unloaded(self, k, model):
        p = self.players.get(k)
        if p:
            p.models.pop(model, None)

    def request(self, k, count):
        p = self.players.get(k)
        if not p or p.granted or p.universe == -1:
            return
        p.canceled = set()
        self.requests.setdefault(p.universe, []).append([k, count])

    def withdraw(self, k):
        p = self.players.get(k)
        if not p:
            return
        for v in p.granted:
            self.reserved.pop(v, None)
            p.canceled.add(v)
        p.granted = set()
        self.unqueue(k)
        self.out.withdrawn(k)

    def withdraw_one(self, k, vid):
        p = self.players.get(k)
        if p:
            p.granted.discard(vid)
            p.canceled.add(vid)
            self.reserved.pop(vid, None)

    def spawned(self, k, vid, model, info, now):
        p = self.players.get(k)
        if not p:
            return False
        if vid in p.canceled:
            p.canceled.discard(vid)
            self.refusal = 'the grant was cancelled before the spawn arrived'
            return False
        if vid not in p.granted:
            self.refusal = 'no grant is outstanding for that id' + (' (a car with that id exists already)' if vid in self.cars else '')
            return False
        p.granted.discard(vid)
        self.reserved.pop(vid, None)
        car = Car(vid, model, p.universe, info, k, now)
        self.cars[vid] = car
        self.hold(p, car)
        p.can_take.add(vid)
        if len(p.held) > HARD_CAP:
            far = self.farthest(p)
            if far is not None:
                self.drop_car(p, self.cars[far[0]], False, now)
        self.placing[vid] = k
        return True

    def removed(self, k, vid):
        p = self.players.get(k)
        if p:
            p.removing.discard(vid)

    def despawn(self, k, vid, now):
        p, car = self.players.get(k), self.cars.get(vid)
        if p and car and vid in p.held:
            self.drop_car(p, car, False, now)

    def can_take(self, k, vid):
        p = self.players.get(k)
        if p and vid in self.cars and vid in p.held:
            p.can_take.add(vid)

    def request_ownership(self, k, vid, now):
        p, car = self.players.get(k), self.cars.get(vid)
        self.resent = False
        if car and car.owner == k:
            self.refusal, self.resent = 'the player owns it already, TakeOwnership sent again', True
            self.out.take(k, vid)
            return False
        if not car or not p or vid not in p.held:
            self.refusal = 'no such car' if not car else 'the player does not hold it'
            self.out.denied(k, vid)
            return False
        if now - car.changed <= CHANGE_GAP_MS:
            self.refusal = f'its owner changed {now - car.changed:.0f} ms ago, minimum {CHANGE_GAP_MS:.0f}'
            self.out.denied(k, vid)
            return False
        self.hand_over(car, k, now)
        return True

    def car_position(self, k, vid, pos):
        car = self.cars.get(vid)
        if car and car.owner == k and pos != (0.0, 0.0, 0.0) and all(math.isfinite(x) for x in pos):
            car.pos = pos

    def tick(self, now):
        for u in sorted({p.universe for p in self.players.values() if p.universe != -1}):
            players = [p for p in self.players.values() if p.universe == u]
            cars = [c for c in self.cars.values() if c.universe == u]
            self.update_holders(players, cars, now)
            self.rebalance(players, cars, now)
            self.grant(u)

    def update_holders(self, players, cars, now):
        for p in players:
            if p.pos is None:
                continue
            reach = float(p.radius) ** 2
            for car in cars:
                vid = car.vid
                if car.pos is None or vid in p.held or vid in p.removing or vid not in self.cars:
                    continue
                if car.model not in p.models:
                    continue
                d = dist2(car.pos, p.pos)
                if d >= reach:
                    continue
                if p.capacity() < 1 and p.models[car.model] != 0:
                    far = self.farthest(p)
                    if far is None or far[1] - EVICT_MARGIN <= d:
                        continue
                    self.drop_car(p, self.cars[far[0]], False, now)
                self.hold(p, car)
                self.out.add(p.key, car)

    def rebalance(self, players, cars, now):
        by_key = {p.key: p for p in players}
        scores, owner_score = [], {}
        for car in cars:
            if len(car.holders) < 2 or now - car.changed <= CHANGE_GAP_MS:
                continue
            for key in car.holders:
                p = by_key.get(key)
                if p is None or p.pos is None or car.pos is None:
                    continue
                sc = int(dist2(car.pos, p.pos) ** 0.5)
                if car.owner == key:
                    sc -= OWNER_BONUS
                    age = now - car.changed
                    if age < RECENT_MS:
                        sc -= int((RECENT_MS - age) * RECENT_FACTOR)
                    owner_score[car.vid] = sc
                scores.append((sc, car.vid, key))
        scores.sort(key=lambda e: (e[0], e[1]))
        done = set()
        while scores:
            budget = {k: ROUND_BUDGET for k in by_key}
            rest = []
            for sc, vid, key in scores:
                if budget[key] < CHANGE_COST:
                    rest.append((sc, vid, key))
                elif vid not in done:
                    car = self.cars[vid]
                    if car.owner != key:
                        if not by_key[key].settled():
                            continue
                        self.hand_over(car, key, now)
                    done.add(vid)
                    budget[key] -= CHANGE_COST
            scores = rest

    def grant(self, universe):
        for vid in list(self.placing):
            c = self.cars.get(vid)
            if c is None or c.pos is not None:
                del self.placing[vid]
        queue = self.requests.get(universe)
        if not queue:
            return
        i = 0
        while i < len(queue):
            entry = queue[i]
            p = self.players.get(entry[0])
            if p is None or not self.may_spawn(p):
                i += 1
                continue
            vid = next((v for v in range(MAX_CARS) if v not in self.cars and v not in self.reserved), None)
            if vid is None:
                return
            entry[1] -= 1
            if entry[1] <= 0:
                queue.pop(i)
            self.reserved[vid] = p.key
            p.granted.add(vid)
            self.out.granted(p.key, vid)

    def may_spawn(self, p):
        """Not while another player near this one is placing cars (two consoles would fill the same road)."""
        if not p.settled() or p.pos is None:
            return False
        others = list(dict.fromkeys(list(self.placing.values()) + [self.reserved[v] for v in sorted(self.reserved)]))
        for key in others:
            q = self.players.get(key)
            if q is None or q is p or q.pos is None:
                continue
            if dist2(q.pos, p.pos) <= float(q.radius) ** 2 + float(p.radius) ** 2:
                return False
        return True

    def farthest(self, p):
        """The farthest held car, sparing a player's last car of a model: (vid, distance²) or None."""
        best = None
        for vid in sorted(p.held):
            car = self.cars[vid]
            if car.pos is not None and p.models.get(car.model, 0) > 1:
                d = dist2(car.pos, p.pos)
                if best is None or best[1] < d:
                    best = (vid, d)
        return best

    def hold(self, p, car):
        p.held.add(car.vid)
        p.models[car.model] = p.models.get(car.model, 0) + 1
        if p.key not in car.holders:
            car.holders.append(p.key)

    def drop_car(self, p, car, silent, now):
        p.held.discard(car.vid)
        p.can_take.discard(car.vid)
        if p.key in car.holders:
            car.holders.remove(p.key)
        if car.model in p.models:
            p.models[car.model] -= 1
        if not silent:
            p.removing.add(car.vid)
            self.out.remove(p.key, car.vid)
        if car.owner == p.key:
            car.owner = self.next_owner(car)
            if car.owner is not None:
                car.changed = now
                self.out.take(car.owner, car.vid)
        if not car.holders:
            self.cars.pop(car.vid, None)
            self.placing.pop(car.vid, None)

    def next_owner(self, car):
        holders = [q for q in self.players.values() if q.key in car.holders]
        return next((q.key for q in holders if q.settled()), None) or \
            next((q.key for q in holders if car.vid in q.can_take), None)

    def hand_over(self, car, k, now):
        if car.owner is not None:
            self.out.drop(car.owner, car.vid)
        car.owner, car.changed = k, now
        self.out.take(k, car.vid)

    def release(self, p, silent):
        for vid in sorted(p.granted):
            self.reserved.pop(vid, None)
            if not silent:
                self.out.canceled(p.key, vid)
        p.granted = set()
        for vid in sorted(p.held):
            car = self.cars.get(vid)
            if car:
                self.drop_car(p, car, silent, car.changed)
        self.unqueue(p.key)
        for vid in [v for v, k in self.placing.items() if k == p.key]:
            del self.placing[vid]

    def unqueue(self, k):
        for u in self.requests:
            self.requests[u] = [e for e in self.requests[u] if e[0] != k]


# ---- the world: the wire, the join sequence and the timers ---------------------------------------------------------

class PeerState:
    def __init__(self):
        self.seq, self.joined, self.joined_at = 0, False, 0.0
        self.burst_n, self.burst_at = 0, None
        self.granted_at = {}
        self.rx, self.window_at, self.window_n, self.flooded = 0, 0.0, 0, False
        self.join_until, self.report_at = None, None
        self.early = {}                      # model -> loaded, reported before the join
        self.seen_seq, self.seen_order = set(), []   # its last message numbers (repeats are dropped)


class World:
    """host: send(peer, msgs, sequenced), peers() (in the session), universe(peer), car_pos(peer), log(text),
    BitWriter (nerd's), name(peer)."""

    def __init__(self, host, models):
        self.host, self.models = host, list(models)
        self.state = {}                      # peer -> PeerState
        self.enabled = bool(self.models)
        self.counters = {}
        self.manager = Manager(self)
        self.next_tick = self.next_summary = 0.0
        self.arrivals = 0
        self.summary_sent = {}
        self.batch = None
        host.log(f'traffic: {len(self.models)} car models' if self.enabled else 'traffic: off (no car models)')

    def count(self, name):
        self.counters[name] = self.counters.get(name, 0) + 1

    def st(self, p):
        s = self.state.get(p)
        if s is None:
            s = self.state[p] = PeerState()
        return s

    # messages to a console
    def client_message(self, kind, seq, a=0xFF, s14=-1, u5=31, b=0xFF, flag=0):
        return (self.host.BitWriter().bits(MSG_CLIENT_MESSAGE, 7).bits(kind, 4).bits(a & 0xFF, 8).bits(s14 & 0x3FFF, 14)
                .bits(u5 & 0x1F, 5).bits(b & 0xFF, 8).bits(flag & 1, 1).bits(seq & 0xFFFFFFFF, 32))

    def update_message(self, mtype, fields):
        w = self.host.BitWriter().bits(RELAY_ID[mtype], 7)
        for v, width in zip(fields, RELAY_LAYOUT[mtype]):
            w.bits(v, width)
        return w

    def push(self, p, kind, **kw):
        s = self.st(p)
        msg = self.client_message(kind, s.seq, **kw)
        s.seq += 1
        self.count(kind)
        if self.batch is not None:
            self.batch.append(msg)
        else:
            self.host.send(p, [msg], True)

    def emit(self, p, msg):
        if self.batch is not None:
            self.batch.append(msg)
        else:
            self.host.send(p, [msg], False)

    # the manager's Out
    def add(self, p, car):
        """AddTraffic and the car's latest state in one sequenced packet, in that order (as FH2MP sends them: one
        flush). Sent apart, the state could arrive first over the internet; the console dropped it for a car it
        didn't know yet, and the car stayed invisible while the console counted it as held (seen 2026-11-19: a
        player who joined again next to the host's cars saw no traffic at all)."""
        d = car.info
        own = self.batch is None
        if own:
            self.batch = []
        self.push(p, C_ADD_TRAFFIC, a=d.struct[0], s14=d.struct[1], u5=d.struct[2], b=d.struct[3], flag=d.struct[4])
        for m, f in d.latest():
            self.emit(p, self.update_message(m, f))
        if own:
            batch, self.batch = self.batch, None
            self.host.send(p, batch, True)

    def remove(self, p, vid):
        if p in self.state:
            self.push(p, C_REMOVE_TRAFFIC, a=vid)

    def granted(self, p, vid):
        self.st(p).granted_at[vid] = time.monotonic()
        self.push(p, C_SPAWN_GRANTED, a=vid)

    def canceled(self, p, vid):
        self.st(p).granted_at.pop(vid, None)
        self.push(p, C_SPAWN_CANCELED, a=vid)

    def withdrawn(self, p):
        self.push(p, C_WITHDRAW_ACCEPTED)

    def take(self, p, vid):
        self.push(p, C_TAKE_OWNERSHIP, a=vid)

    def drop(self, p, vid):
        if p in self.state:
            self.push(p, C_DROP_OWNERSHIP, a=vid)

    def denied(self, p, vid):
        self.push(p, C_OWNERSHIP_DENIED, a=vid)

    # the session
    def join(self, p, force=False):
        """A console entered the session: the car models, the limit, then (once it said which models it has cars
        of, or after a while) InitialJoinComplete, which the console's loading waits for."""
        if not self.enabled:
            return
        s = self.st(p)
        if s.joined and not force:
            return
        if s.joined:
            self.manager.reset(p)
        s.joined, s.joined_at = True, time.monotonic()
        pl = self.manager.join(p)
        for model, loaded in s.early.items():
            (self.manager.model_loaded if loaded else self.manager.model_unloaded)(p, model)
        s.early = {}
        s.report_at = s.joined_at if pl.models else None
        self.batch = []
        for model in self.models:
            self.push(p, C_LOAD_CAR_MODEL, s14=model)
        self.push(p, C_ALL_MODELS_SENT)
        self.push(p, C_TRAFFIC_LIMIT, a=TRAFFIC_LIMIT)
        batch, self.batch = self.batch, None
        self.host.send(p, batch, True)
        s.join_until = time.monotonic() + JOIN_WAIT_S
        self.host.log(f'traffic: join {self.host.name(p)}: {len(self.models)} models, limit {TRAFFIC_LIMIT}')

    def leave(self, p):
        if self.state.pop(p, None) is not None:
            self.manager.leave(p)
            self.host.log(f'traffic: {self.host.name(p)} left; {len(self.manager.cars)} cars remain')

    def flooding(self, p, s):
        now = time.monotonic()
        if now - s.window_at >= 1.0:
            s.window_at, s.window_n = now, 0
        s.window_n += 1
        if s.window_n <= MAX_MESSAGES_PER_S:
            return False
        if not s.flooded:
            s.flooded = True
            self.host.log(f'traffic: {self.host.name(p)} over {MAX_MESSAGES_PER_S} messages/s; the rest is ignored')
        return True

    def on_message(self, p, kind, a, s14, u5, b, flag, seq):
        """A ServerTrafficMessage (71) from a console."""
        s = self.st(p)
        # A console sends its messages again until their packet is acked, in new packets: each one counts once
        # (its own u32 sequence number).
        if seq in s.seen_seq:
            return
        s.seen_seq.add(seq)
        s.seen_order.append(seq)
        if len(s.seen_order) > 512:
            s.seen_seq.discard(s.seen_order.pop(0))
        s.rx += 1
        if s.rx <= 40 or s.rx % 200 == 0:
            held = len(self.manager.players[p].held) if p in self.manager.players else 0
            self.host.log(f'traffic: {self.host.name(p)} -> {S_NAMES[kind] if kind < len(S_NAMES) else kind} a={a} '
                          f's14={signed14(s14)} u5={u5} b={b} flag={flag} (held {held})')
        if self.enabled and not s.joined and kind in (S_CAR_MODEL_LOADED, S_CAR_MODEL_UNLOADED) and not self.flooding(p, s):
            s.early.pop(signed14(s14), None)
            if len(s.early) < 64:
                s.early[signed14(s14)] = kind == S_CAR_MODEL_LOADED
            return
        if not self.enabled or not s.joined or self.flooding(p, s):
            return
        m, now = self.manager, time.monotonic() * 1000.0
        if kind in RADIUS_KINDS:
            m.set_radius(p, signed14(s14))
        if kind == S_REQUEST_TRAFFIC:
            m.set_universe(p, self.host.universe(p))
            m.request(p, max(1, 1 if a == 0xFF else a))
        elif kind == S_WITHDRAW_REQUEST:
            m.withdraw(p)
        elif kind == S_TRAFFIC_SPAWNED:
            s.granted_at.pop(a, None)
            if m.spawned(p, a, signed14(s14), CarData([a, s14, u5, b, flag]), now):
                self.push(p, C_TAKE_OWNERSHIP, a=a)
            else:
                self.host.log(f'traffic: {self.host.name(p)} spawned car {a} it was not granted; ignored ({m.refusal})')
        elif kind == S_TRAFFIC_REMOVED:
            m.removed(p, a)
        elif kind == S_REQUEST_DESPAWN:
            m.despawn(p, a, now)
        elif kind == S_CAN_TAKE_OWNERSHIP:
            m.can_take(p, a)
        elif kind == S_REQUEST_OWNERSHIP:
            if m.request_ownership(p, a, now):
                self.host.log(f'traffic: car {a} handed to {self.host.name(p)} (it asked)')
        elif kind == S_CAR_MODEL_LOADED:
            m.model_loaded(p, signed14(s14))
            t = time.monotonic()
            if s.report_at is None:
                s.report_at = t
            s.burst_at = t
            s.burst_n += 1
        elif kind == S_CAR_MODEL_UNLOADED:
            m.model_unloaded(p, signed14(s14))

    def on_update(self, p, mtype, fields):
        """A car's state from its owner (16/17/73/74; fields as UPDATE_LAYOUT): to the others that hold it."""
        vid = fields[0]
        key = 'rx_' + UPDATE_NAMES[mtype]
        self.count(key)
        n = self.counters[key]
        if n <= 4 or n % 1000 == 0:
            self.host.log(f'traffic: {self.host.name(p)} -> {UPDATE_NAMES[mtype]} car {vid} (#{n})')
        m = self.manager
        car = m.cars.get(vid)
        if not self.enabled or car is None or car.owner != p or self.flooding(p, self.st(p)):
            return
        info = car.info
        if mtype in (MSG_SERVER_FULL_SYNC, MSG_SERVER_FULL_ROUTE, MSG_SERVER_STATIONARY):
            self.arrivals += 1
            info.order[mtype] = self.arrivals
            if mtype == MSG_SERVER_FULL_SYNC:
                info.full_sync = fields
            elif mtype == MSG_SERVER_FULL_ROUTE:
                info.full_route = fields
            else:
                info.stationary = fields
        m.car_position(p, vid, update_position(mtype, fields))
        msg = None
        for q in car.holders:
            if q is not p:
                if msg is None:
                    msg = self.update_message(mtype, fields)
                self.emit(q, msg)

    def tick(self):
        now = time.monotonic()
        if not self.enabled or not self.state or now < self.next_tick:
            return
        self.next_tick = now + TICK_S
        m = self.manager
        for p, s in list(self.state.items()):
            if s.burst_at is not None and now - s.burst_at > 0.3:
                n, s.burst_n, s.burst_at = s.burst_n, 0, None
                if n >= 4 and now - s.joined_at > 25.0:
                    # it threw its traffic away (a new area or a reload) and reported its models again
                    self.host.log(f'traffic: {self.host.name(p)} reported {n} models at once; join sequence again')
                    self.join(p, True)
            if p in m.players:
                pos = self.host.car_pos(p)
                if pos is not None:
                    m.set_position(p, pos)
                m.set_universe(p, self.host.universe(p))
            for vid, t in list(s.granted_at.items()):
                if now - t > GRANT_TIMEOUT_S:
                    del s.granted_at[vid]
                    self.host.log(f'traffic: grant of car {vid} to {self.host.name(p)} timed out')
                    m.withdraw_one(p, vid)
                    self.push(p, C_SPAWN_CANCELED, a=vid)
        done = [(p, s) for p, s in self.state.items()
                if s.join_until is not None and (now >= s.join_until or self.models_in(p, s, now))]
        m.tick(now * 1000.0)
        for p, s in done:
            pl = m.players.get(p)
            reported = sorted(x for x in self.models if pl and x in pl.models)
            ready = self.models_in(p, s, now)
            why = '' if ready else (' (gave up waiting: ' + ('no position' if pl and pl.pos is None else 'no model report') + ')')
            self.host.log(f'traffic: {self.host.name(p)} InitialJoinComplete {now - s.joined_at:.1f} s after the join, '
                          f'{len(reported)}/{len(self.models)} models with cars on the console{why}')
            s.join_until = None
            self.push(p, C_INITIAL_JOIN_COMPLETE)
        if now >= self.next_summary:
            self.next_summary = now + SUMMARY_S
            self.summary()

    def models_in(self, p, s, now):
        pl = self.manager.players.get(p)
        if not pl or pl.pos is None:
            return False
        if all(x in pl.models for x in self.models):
            return True
        if s.report_at is not None:
            return now - s.report_at >= REPORT_SETTLE_S
        return now - s.joined_at >= NO_REPORT_S

    def summary(self):
        m = self.manager
        if not m.players:
            return
        sent = {k: v for k, v in self.counters.items() if not str(k).startswith('rx_')}
        delta = {k: v - self.summary_sent.get(k, 0) for k, v in sent.items() if v != self.summary_sent.get(k, 0)}
        self.summary_sent = sent
        shared = sum(1 for c in m.cars.values() if len(c.holders) >= 2)
        people = '; '.join(f'{self.host.name(k)} u{p.universe} held {len(p.held)} owns '
                           f'{sum(1 for c in m.cars.values() if c.owner == k)}' + ('' if p.pos else ' no-pos')
                           for k, p in m.players.items())
        names = ['TrafficLimit', 'SpawnGranted', 'SpawnCanceled', 'WithdrawAccepted', 'AddTraffic', 'RemoveTraffic',
                 'TakeOwnership', 'DropOwnership', 'OwnershipDenied', 'LoadCarModel', 'RemoveCarModel', 'UnloadCarModel',
                 'AllModelsSent', 'InitialJoinComplete']
        tail = ', '.join(f'{names[k] if isinstance(k, int) and k < len(names) else k} {v}' for k, v in sorted(delta.items(), key=str)) or 'nothing'
        self.host.log(f'traffic: {len(m.cars)} cars, {shared} seen by 2+; {people}; sent {tail}')
