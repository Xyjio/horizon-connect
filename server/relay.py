"""The game server's UDP side: a NerdCore relay (nerd.py has the formats).

Each console (peer) gets the server's side of the connection: OpenConnection is answered Ok, the console's
ResponseAck gets TimeSync and ConnectionSyncDone, Pings get Pongs, sequenced packets are acked, and the server
pings every half second (consoles drop a server that is quiet for 10 s).

Objects: the server owns one object itself, the lobby's SessionHost (router 6, type 0), which consoles wait for
before they finish joining (stateflow network_matchmaking.xml wait_for_session_host). It says the session is
InviteOnly (private), which is what shows the session leader. Every object a console
creates or updates is forwarded to the other consoles with its handle marked as that console's (the relay
handle: peer index), and kept in a history that is replayed in order to consoles that join later. A console's
car (CarPredictionFull, ~30 a second, unsequenced) is forwarded the same way but not kept, and so are its
collisions (MsgCarCollision), whose car handle is turned into what it means to each receiver.

Object updates are numbered per receiver (each object's link takes them strictly in order, seq + 1 mod 255), so
the relay can add updates of its own to a console's object: a console's updates are put back in order if they
arrive out of it, and every receiver gets them numbered from the create it was sent.

Session leader (the crown): the server's var IsLeader on each console's PlayerData (router 1). One console is
the leader - the host's (--leader-xuid, the player id of the PC the server runs on) when it joins, otherwise the
console that has been in the session longest; it passes on when the leader leaves, asks for another one
(RequestLeaderChange) or gives it up (DropLeadership). Each console gets IsLeader on its own PlayerData as a
server update and on everyone else's PlayerData as a relayed update.

Names: the gamertag in each console's PlayerData (NetworkName) is kept, and "FH2N?" is answered with
FH2N!{xuid: gamertag}. Every player's own local server asks this when the game wants another player's
profile (fh2srv.py), since it only knows its own player's name.

Testing alone (fh2srv --mirror <seconds>): each console also gets its own objects back as another player's
(peer 250), and its car that many seconds late - a ghost of itself to show that car sync works.

A console that starts over (an OpenConnection in packet 0: a new launch), goes quiet for 15 s or closes the
connection is dropped: the others get an ObjectDelete for each of its objects.

Reliability: the relay's sequenced packets (objects, server updates, deletes, requests) are kept until the
console acks them (packet header: newest id + 32 bits for the ids before it) and sent again in a new packet
after RESEND_S - a console takes each object's updates strictly in order, so one lost packet would otherwise
stall that object for good. A message nobody acks is given up after RESEND_GIVE_UP_S.

Game messages carry no length, so a packet is read only as far as its messages are understood; the rest of it
is logged and dropped (the packet is still acked).
"""
import itertools, json, math, os, socket, threading, time

import nerd, netsim

# Message ids are the order of the game's message table (names at 1422E4470).
NAMES = dict(enumerate([
    'OpenConnection', 'OpenConnectionResponse', 'CloseConnection', 'Ping', 'Pong', 'TimeSync', 'ConnectionSyncDone',
    'ObjectCreate', 'ObjectUpdate', 'ObjectDelete', 'ObjectRequestOwnership', 'ObjectGrantOwnership',
    'ObjectTransferOwnership', 'ObjectNeedsUpdates', 'PlayerInput', 'CarPredictionFull', 'ServerTrafficFullSync',
    'ServerTrafficRouteProgressUpdate', 'ClientTrafficFullSync', 'ClientTrafficRouteProgressUpdate', 'MsgBreakGlass',
    'MsgCarCollision', 'MsgCollObjPos', 'MsgCollObjRest', 'MsgCollObjDetachPart', 'MsgCollObjPosVel',
    'MsgCarMeetPlayerInviteRequest', 'MsgCarMeetPlayerJoinOrCancelRequest', 'MsgCarMeetLaunchWagerRace',
    'MsgCarMeetNotifyPendingGameMode', 'MsgCarMeetConfigureGameOptionsForWagerRace',
    'MsgCarMeetConfiguredGameOptionsForWagerRace', 'MsgCarMeetGameModeSessionReady', 'MsgChatManagement',
    'FinaliseSessionEntry', 'MsgRepairDamage', 'MsgTagCollision', 'MsgTagBuckets', 'MsgRequestRace', 'MsgStartingGrid',
    'MsgStartRace', 'MsgReadyRequest', 'MsgReadyRequestResponse', 'MsgInProgressScoreUpdate', 'MsgLapScore',
    'MsgScoreKeyframe', 'MsgVoteToKick', 'MsgVoteForTrack', 'SelectNextRoadTripEvent', 'SelectedNextRoadTripEvent',
    'ConfigureGameOptionsForRoadTripEvent', 'ConfiguredGameOptionsForRoadTripEvent', 'SetGroupSatNavEvent',
    'RequestSendFullWeather', 'FullWeather', 'RequestSendNewAtmosphericData', 'NewAtmosphericData',
    'RequestSendNewPrecipitationData', 'NewPrecipitationData', 'RequestLeaderChange', 'DropLeadership',
    'BucketListChallengeStart', 'BucketListChallengeEnded', 'RequestSynchronizeBucketListChallenge',
    'RequestDesynchronizeBucketListChallenge', 'RespondBucketListPartner', 'BucketListStatusPlayerWaiting',
    'BucketListStatusChallengeStarted', 'BucketListStatusChallengeEnded', 'MsgDebugLog', 'MsgInactivePlayersPulled',
    'ServerTrafficMessage', 'ClientTrafficMessage', 'ServerTrafficFullRoute', 'ServerTrafficStationary',
    'ClientTrafficFullRoute', 'ClientTrafficStationary']))
FORWARD_UNSEQUENCED = (20, 43)   # broken glass, a racer's score as it drives (MsgInProgressScoreUpdate): passed on like a car
                                 # (24, a part torn off a smashable, is read, not passed on yet)
FORWARD_SEQUENCED = (44, 45)     # MsgLapScore, MsgScoreKeyframe (a racer's lap and final score): passed on sequenced
TRAFFIC = (16, 17, 71, 73, 74)   # the traffic director's messages (Turn 10's server ran it): read, not answered
SERVER_OWNER = 0                 # owner index of the server's own objects in Peer.out_seq (consoles are 1..)
# Online the time of day follows the server clock (StartNetworkSync, 14197AF90): seconds of day =
# server ms / 1000 * 48 mod 86400, so a game day lasts 30 minutes. The clock starts at start_hour.
TOD_SCALE = 48
WEATHER_MIN_MS, WEATHER_MAX_MS = 60_000, 20 * 60_000   # how soon / late the server asks for the next weather
RESEND_S = 0.4                   # an unacked sequenced packet's messages go again after this long
RESEND_GIVE_UP_S = 12.0          # ... until this long after they were first sent
# What one console may leave in the history (replayed to everyone who joins later). A console sends a few object
# updates a minute; one that sends this many is broken or hostile and is dropped, so memory stays bounded.
MAX_HISTORY_PER_PEER = 5000
OWN_ATTEMPT = 1
# The lobby's objects (router 6) are the server's. Every console makes the GameOptions itself as the server's
# objects 0 and 2 (CLobby, 140A2BF24 / 140A2BF84: handles flag 31, ids 2 and 0) - so the server's SessionHost
# must not be 0 (it was until 1.0.6, which made that GameOptions unreachable). The leader asks for ownership of
# GameOptions 0 when it starts an event (stateflow network.get_game_options_ownership).
SESSION_HOST = (6, 1)        # router, object id of the server's SessionHost
LOBBY_ROUTER = 6
GAME_OPTIONS_IDS = (0, 2)    # made by every console, never created by the server
QUIET_DROP_S = 15
MAX_PACKET = 1500                # the game's packets are at most 1200 bytes; bigger ones aren't read
MAX_PLAYERS = 12             # FH2's session size (gameServerPlatform maxAllowedPlayers)
# Peer indexes stay small and are reused: the game keeps per-peer state in a fixed-size table indexed by
# the 8-bit peer index of a relay handle, so a large index reads past it (crash at game+F842E5 with
# index 250, the mirror's old one - Azimuth 1.0.4 port, 2026-11-04). Consoles get 1..MAX_PLAYERS-1;
# the mirror's ghost takes the last one.
MIRROR_INDEX = MAX_PLAYERS - 1 # the peer index of the mirror's ghost player


class Peer:
    def __init__(self, addr, index, joined):
        self.addr, self.index, self.joined = addr, index, joined
        self.acks = nerd.Acks()
        self.seq = itertools.count(1)        # our sequenced packet ids to this peer
        self.synced = False
        self.sync_index = 0
        self.user = None
        self.finalised = False               # SessionEntryFinalised sent
        self.console_id = None               # its ConsoleSynced object (router 3)
        self.player_id = None                # its PlayerData object (router 1)
        self.server_seq = {}                 # (router, id) -> last server-update seq sent
        self.objects = {}                    # (router, id) -> last seq: what it owns
        self.out_seq = {}                    # (owner index, router, id) -> last seq of that object sent to this peer
        self.leader_shown = {}               # owner index -> IsLeader last sent to this peer for that PlayerData
        self.own_leader = None               # IsLeader last sent for its own PlayerData
        self.last_seen = time.monotonic()
        self.cars = 0
        self.collisions = 0
        self.traffic = 0
        self.car_id = None                   # its car's object id (from its CarPredictionFull)
        self.ready = False                   # it said it is ready for the event (MsgReadyRequest)
        self.console_vars = {}               # its ConsoleSynced vars as it set them
        self.finished = False                # in the event: it sent its final score (MsgScoreKeyframe)
        self.last_fields = {}                # message id -> (fields, time) last passed on (it repeats them)
        self.unacked = {}                    # our packet id -> (sent at, first sent at, msgs): waiting for its ack
        self.resent = 0
        self.bad = 0                         # packets that couldn't be read
        self.kept = 0                        # its entries in the relay's history


class Relay:
    def __init__(self, out_dir, lock, port=5101, host='127.0.0.1', mirror=None, leader_xuid=None, start_hour=10.0):
        self.out_dir, self.lock = out_dir, lock
        self.mirror = mirror                 # seconds: each console also gets itself back as a ghost player
        self.leader_xuid = leader_xuid       # the host's player id: its console takes the crown when it joins
        self.delayed = []                    # (due, peer, msgs) for the mirror's cars
        # host '127.0.0.1' (the default): only this PC - its own console and the playit agent, which brings the
        # friends' packets from 127.x addresses. '::' (LAN play): every address, IPv4 too (as ::ffff:a.b.c.d).
        if ':' in host:
            self.sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
            self.sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
        else:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((host, port))
        sim = netsim.load(os.path.dirname(os.path.abspath(__file__)))
        if sim:                              # testing only: a bad line to the players who come through the tunnel
            self.sock = netsim.SimSocket(self.sock, sim)
            print(f'TEST: simulated internet for tunnel players: {sim["delay"] * 1000:.0f} ms + up to '
                  f'{sim["jitter"] * 1000:.0f} ms each way, {sim["loss"]:.0%} of packets lost each way', flush=True)
        self.mu = threading.RLock()
        self.t0 = time.monotonic() - (start_hour % 24) * 3600 / TOD_SCALE   # server clock at start: start_hour o'clock
        self.peers = {}
        self.joins = itertools.count()
        self.free = list(range(1, MAX_PLAYERS - (1 if mirror is not None else 0)))   # unused peer indexes
        self.history = []                    # (owner index, msg id, object dict) in order
        self.streams = {}                    # (owner, router, id) -> {'next': seq, 'pending': {seq: (msg, o)}}
        self.seen = set()                    # (owner, msg, router, id, seq): consoles resend unacked messages
        self.leader = None                   # peer index of the session leader
        self.weather = None                  # the session's WeatherSynced vars (nerd.weather_vars), once known
        self.weather_asked = None            # when (now) a console was last asked for its full weather
        self.next_weather = {}               # 'a' / 'p' -> when to ask for new atmosphere / precipitation data
        self.n = 0
        self.strangers = 0                   # packets from addresses that aren't consoles (ignored)
        self.owners = {}                     # (router, id) of a server object -> peer index that was granted it
        self.event = {}                      # the SessionHost's event vars as set so far
        self.event_state, self.event_due, self.event_start, self.participants = None, 0, 0, []
        self.grants = {}                     # (router, id) -> (peer index, when) of the last ownership grant
        self.lobby_seq = {}                  # (router, id) -> the owner's last update seq
        self.names = {}                      # xuid (text) -> gamertag, from the consoles' PlayerData
        print(f'fh2srv game UDP (relay) on [{host}]:{port}' + (f', leader: player {leader_xuid} when present' if leader_xuid else ''), flush=True)

    def now(self):
        return int((time.monotonic() - self.t0) * 1000) + 1

    def send(self, peer, msgs, sequenced=False, first=None):
        ack, ackbits = peer.acks.header()
        pid = nerd.NO_SEQ
        if sequenced:
            pid = next(peer.seq) & 0xFFFF
            while pid >= 0xFFFE:             # 0xFFFF means "no sequence"; 0xFFFE is the "nothing acked yet" ack
                pid = next(peer.seq) & 0xFFFF
            now = time.monotonic()
            peer.unacked[pid] = (now, first if first is not None else now, list(msgs))
        data = nerd.packet(nerd.concat(*msgs), count=len(msgs), pid=pid, ack=ack, ackbits=ackbits)
        try:
            self.sock.sendto(data, peer.addr)
        except OSError:
            pass
        return data

    def on_acks(self, peer, ack, ackbits):
        """The console's packet header acks our packet `ack` and, in `ackbits`, the 32 before it."""
        if not peer.unacked or ack >= 0xFFFE:
            return
        peer.unacked.pop(ack, None)
        for n in range(1, 33):
            if ackbits >> (n - 1) & 1:
                peer.unacked.pop((ack - n) & 0xFFFF, None)

    def resend(self, peer):
        """Unacked sequenced packets: their messages again, in a new packet (each object's seq makes the copy
        harmless if the first one did arrive)."""
        now = time.monotonic()
        for pid, (sent, first, msgs) in list(peer.unacked.items()):
            if now - sent < RESEND_S:
                continue
            del peer.unacked[pid]
            if now - first > RESEND_GIVE_UP_S:
                print(f'{time.strftime("%H:%M:%S")} peer {peer.index}: {len(msgs)} messages never acked; given up', flush=True)
                continue
            peer.resent += 1
            self.send(peer, msgs, sequenced=True, first=first)

    def send_all(self, peer, msgs, sequenced=True):
        for i in range(0, len(msgs), 8):     # a few messages per packet, well under the packet size limit
            self.send(peer, msgs[i:i + 8], sequenced=sequenced)

    def others(self, peer):
        return [p for p in self.peers.values() if p is not peer and p.synced]

    # ---- objects ------------------------------------------------------------------------------------------------

    def relayed(self, to, owner, msg, router, id, payload, type=0, create_seq=0):
        """`owner`'s object as an ObjectCreate/ObjectUpdate for `to`, numbered in `to`'s own sequence of it."""
        key = (owner, router, id)
        if msg == 7 or key not in to.out_seq:
            seq = create_seq if msg == 7 else 0
        else:
            seq = (to.out_seq[key] + 1) % 255
        to.out_seq[key] = seq
        return nerd.object_msg(msg, router, id, seq, payload, peer=owner, type=type)

    def in_order(self, peer, msg, o):
        """A console's object messages in its own order: (msg, o) pairs ready to pass on."""
        key = (peer.index, o['router'], o['id'])
        st = self.streams.get(key)
        if msg == 7:
            st = self.streams[key] = {'next': (o['seq'] + 1) % 255, 'pending': {}}
            return [(msg, o)]
        if st is None:                       # an update to an object we never saw made: pass it on as it is
            return [(msg, o)]
        if o['seq'] != st['next']:
            if (o['seq'] - st['next']) % 255 < 128:
                st['pending'][o['seq']] = (msg, o)
                if len(st['pending']) <= 64:
                    return []
                st['next'] = min(st['pending'], key=lambda q: (q - st['next']) % 255)   # give up waiting
            else:
                return []                    # older than what was passed on already
        else:
            st['pending'][o['seq']] = (msg, o)
        ready = []
        while st['next'] in st['pending']:
            ready.append(st['pending'].pop(st['next']))
            st['next'] = (st['next'] + 1) % 255
        return ready

    def drop(self, peer, why):
        """Forget a console: its objects go from the history, and the others delete them."""
        self.peers.pop(peer.addr, None)
        if peer.index not in self.free:
            self.free.append(peer.index); self.free.sort()
        self.history = [h for h in self.history if h[0] != peer.index]
        self.seen = {k for k in self.seen if k[0] != peer.index}
        self.streams = {k: v for k, v in self.streams.items() if k[0] != peer.index}
        n = 0
        for p in self.peers.values():
            mine = [k for k in p.out_seq if k[0] == peer.index]
            deletes = [nerd.object_delete(router, id, (p.out_seq.pop((o, router, id)) + 1) % 255, peer=peer.index) for o, router, id in mine]
            p.leader_shown.pop(peer.index, None)
            if deletes and p.synced:
                self.send_all(p, deletes)
            n = max(n, len(deletes))
        print(f'{time.strftime("%H:%M:%S")} peer {peer.index} [{peer.addr[0]}]:{peer.addr[1]} dropped ({why}); '
              f'{n} objects deleted for the others', flush=True)
        if peer.index == self.leader:
            self.leader = None
        self.update_leader()
        if not self.peers:                   # the session is over: the next one gets its own weather
            self.weather, self.weather_asked, self.next_weather = None, None, {}

    def delete_object(self, peer, router, id):
        """A console deleted one of its objects: it goes from the history and from the others."""
        key = (peer.index, router, id)
        self.history = [h for h in self.history if not (h[0] == peer.index and h[2]['router'] == router and h[2]['id'] == id)]
        peer.kept = sum(1 for h in self.history if h[0] == peer.index)
        self.streams.pop(key, None)
        self.seen = {k for k in self.seen if not (k[0] == peer.index and k[2] == router and k[3] == id)}
        peer.objects.pop((router, id), None)
        if router == nerd.PLAYER_ROUTER and id == peer.player_id:
            peer.player_id = None
        for p in self.others(peer):
            if key in p.out_seq:
                self.send_all(p, [nerd.object_delete(router, id, (p.out_seq.pop(key) + 1) % 255, peer=peer.index)])

    def server_var(self, peer, router, id, payload):
        """A server update of server vars on one of the peer's own objects."""
        # The receiver takes updates strictly in order (link seq + 1, mod 255; 1417B1E70) and the first one
        # it expects from us is 0; later ones wait for the gap to fill.
        key = (router, id)
        seq = peer.server_seq[key] = (peer.server_seq.get(key, -1) + 1) % 255
        return nerd.server_update(router, id, seq, payload)

    def set_console_var(self, peer, index, value):
        """One of the server's bools on the peer's own ConsoleSynced object (returned, for the peer). The others
        see the same var on their copy of it: the event flow reads the others' Ready and ParticipatingInRace
        (NetworkAnyRemoteConsolesReady, the grid and the participant list)."""
        payload = nerd.bool_var(index, value)
        for p in self.others(peer):
            if (peer.index, 3, peer.console_id) in p.out_seq:
                self.send_all(p, [self.relayed(p, peer.index, 8, 3, peer.console_id, payload)])
        return self.server_var(peer, 3, peer.console_id, payload)

    # ---- the session leader (crown) -----------------------------------------------------------------------------

    def leader_var(self, value):
        return nerd.bool_var(nerd.PLAYER_IS_LEADER, value, nerd.PLAYER_INDEX_BITS)

    def update_leader(self, prefer=None):
        """Choose the leader if there is none (or `prefer`, the host's console that just joined), then tell
        every console what changed."""
        cands = [p for p in self.peers.values() if p.synced and p.player_id is not None]
        by_index = {p.index: p for p in cands}
        new = self.leader if self.leader in by_index else None
        if prefer is not None and prefer.index in by_index:
            new = prefer.index
        if new is None and cands:
            new = min(cands, key=lambda p: p.joined).index
        if new != self.leader:
            who = by_index[new] if new is not None else None
            print(f'{time.strftime("%H:%M:%S")} session leader: ' +
                  (f'peer {new} (player {who.user})' if who else 'none'), flush=True)
            self.leader = new
        for to in self.peers.values():
            if to.synced:
                self.show_leader(to)

    def show_leader(self, to):
        """IsLeader for every PlayerData `to` has (its own and the others'), where it isn't what `to` knows."""
        msgs = []
        if to.player_id is not None and to.own_leader != (to.index == self.leader):
            to.own_leader = to.index == self.leader
            msgs.append(self.server_var(to, nerd.PLAYER_ROUTER, to.player_id, self.leader_var(to.own_leader)))
        for p in self.peers.values():
            if p is to or p.player_id is None or (p.index, nerd.PLAYER_ROUTER, p.player_id) not in to.out_seq:
                continue
            value = p.index == self.leader
            if to.leader_shown.get(p.index) != value:
                to.leader_shown[p.index] = value
                msgs.append(self.relayed(to, p.index, 8, nerd.PLAYER_ROUTER, p.player_id, self.leader_var(value)))
        if msgs:
            self.send_all(to, msgs)

    def leader_request(self, peer, target):
        """RequestLeaderChange / DropLeadership from `peer`: only the leader can pass the crown on."""
        if peer.index != self.leader:
            return '(not the leader: ignored)'
        cands = [p for p in self.peers.values() if p.synced and p.player_id is not None and p is not peer]
        if target is None:                   # DropLeadership: to the console in the session longest
            if not cands:
                return '(nobody else to take it)'
            target = min(cands, key=lambda p: p.joined).index
        if target not in [p.index for p in cands] + [peer.index]:
            return f'(no player {target})'
        self.leader = target
        print(f'{time.strftime("%H:%M:%S")} session leader: peer {target} (passed on by peer {peer.index})', flush=True)
        self.update_leader()
        return f'-> leader peer {target}'

    # ---- the server's own objects and the weather ---------------------------------------------------------------

    def server_object(self, to, msg, router, id, payload, only_new=False, type=0):
        """The server's object as an ObjectCreate/ObjectUpdate for `to` (handle flags 0, 0: the sender's own)."""
        key = (SERVER_OWNER, router, id)
        if msg == 7:
            if only_new and key in to.out_seq:
                return None
            seq = 0
        elif key not in to.out_seq:
            return None                      # `to` hasn't got the object yet: its create will carry the values
        else:
            seq = (to.out_seq[key] + 1) % 255
        to.out_seq[key] = seq
        return nerd.object_msg(msg, router, id, seq, payload, type=type)

    def session_host_payload(self):
        """The SessionHost as it is now (for a create)."""
        w = nerd.concat(nerd.session_host_vars(), nerd.session_host_update(**self.event)) if self.event else nerd.session_host_vars()
        return w

    def set_session_host(self, **values):
        """Changes the server's SessionHost for everyone."""
        self.event.update(values)
        payload = nerd.session_host_update(**values)
        for p in self.peers.values():
            if p.synced:
                m = self.server_object(p, 8, SESSION_HOST[0], SESSION_HOST[1], payload)
                if m is not None:
                    self.send_all(p, [m])
        return ', '.join(f'{k}={v}' for k, v in values.items())

    # The event, as Turn 10's server ran it (our reading of the game's flow, stateflow network_freeroam_states):
    # the leader's MsgRequestRace makes the server race ready and starts a countdown ("An event starts soon" for the
    # others). When it runs out, the consoles that said they are ready take part (ParticipatingInRace) and the
    # participants are selected. Each then picks its car and loads the others' (it sets HasLoadedCarsForAllParticipants
    # and waits for the route switch - if the server is InRace before that, it decides the event started without it).
    # When all have loaded (or after a while): the starting grid, then the start time, and only then InRace.
    EVENT_COUNTDOWN_MS = 20_000
    EVENT_GATHER_MS = 60_000                 # longest wait for the participants to load
    EVENT_START_DELAY_MS = 6_000
    EVENT_FINISH_WINDOW_MS = 30_000      # after the first racer finishes, the others have this long
    EVENT_END_DELAY_MS = 3_000           # after the last one: the results screen
    EVENT_MAX_RACE_MS = 30 * 60_000      # nobody finished: over after this

    def on_request_race(self, peer):
        """The leader starts the event: the server is race ready and counts down."""
        if peer.index != self.leader:
            return '(not the leader: ignored)'
        if self.event_state:
            return f'(event already {self.event_state})'
        end = self.now() + self.EVENT_COUNTDOWN_MS
        self.event_state, self.event_due = 'countdown', end
        note = '-> SessionHost ' + self.set_session_host(race_ready=1, countdown_type=1, countdown_end=end)
        # The others get the leader's MsgRequestRace: their lobby fires NetworkRaceRequestingParticipants
        # (1417F6FB0 -> CLobby 140A2DFB0), which shows the join prompt (race_starting_prompt). Who joins says
        # it is ready (MsgReadyRequest) and takes part.
        others = [p for p in self.peers.values() if p is not peer and p.synced]
        for p in others:
            self.send_all(p, [nerd.BitWriter().bits(nerd.MSG_REQUEST_RACE, 7)])
        return note + f'; join prompt to {[p.index for p in others]}'

    def on_ready_request(self, peer, fields):
        ready = bool(fields[1][2])
        peer.ready = ready
        self.send_all(peer, [self.set_console_var(peer, nerd.CONSOLE_READY, ready), nerd.ready_response(0)])
        note = f'{fields[0][2]} ready={int(ready)} -> Ready set, response 0'
        if not ready and peer in self.participants and self.event_state not in (None, 'countdown'):
            # it left the event (or gave up waiting for it): it no longer takes part; with nobody left the
            # event is over (event_tick)
            self.participants.remove(peer)
            self.send_all(peer, [self.set_console_var(peer, nerd.CONSOLE_PARTICIPATING_IN_RACE, False)])
            note += '; left the event'
        return note

    def on_final_score(self, peer):
        """A racer crossed the line (its MsgScoreKeyframe). The race ends when all have, or a while after the
        first one: then InRace goes off, which lets the results screen go on (NetworkWaitForRaceEnd 1408EEB20
        waits for SessionHost InRace = 0) to the rewards and back to free roam."""
        if self.event_state != 'racing' or peer not in self.participants or peer.finished:
            return ''
        peer.finished = True
        now = self.now()
        if all(p.finished for p in self.participants):
            self.event_due = now + self.EVENT_END_DELAY_MS
            return '-> all finished'
        if not any(p.finished for p in self.participants if p is not peer):
            self.event_due = now + self.EVENT_FINISH_WINDOW_MS
        return '-> finished'

    def event_reset(self, why):
        for p in self.participants:
            if p.addr in self.peers:
                self.send_all(p, [self.set_console_var(p, nerd.CONSOLE_PARTICIPATING_IN_RACE, False)])
            p.ready = p.finished = False
        self.set_session_host(race_ready=0, participants_selected=0, in_race=0)
        self.event_state, self.participants = None, []
        print(f'{time.strftime("%H:%M:%S")} event: over ({why})', flush=True)

    def event_tick(self):
        now = self.now()
        if not self.event_state:
            return
        self.participants = [p for p in self.participants if p.addr in self.peers]
        if self.event_state != 'countdown' and not self.participants:
            self.event_reset('no participants left')
            return
        if self.event_state == 'countdown' and now < self.event_due:
            synced = [p for p in self.peers.values() if p.synced and p.console_id is not None]
            if len(synced) > 1 and all(p.ready for p in synced):
                self.event_due = now                 # everyone joined: no need to wait for the rest
        if self.event_state == 'countdown' and now >= self.event_due:
            self.participants = [p for p in self.peers.values() if p.synced and p.console_id is not None and p.ready]
            if not self.participants:
                self.event_reset('nobody ready')
                return
            for p in self.participants:
                # what each says from here on counts, not what it said in the last event
                for var in (nerd.CONSOLE_RENDER_READY, nerd.CONSOLE_HAS_LOADED_CARS, nerd.CONSOLE_HAS_GRID):
                    p.console_vars.pop(var, None)
                p.finished = False
                self.send_all(p, [self.set_console_var(p, nerd.CONSOLE_PARTICIPATING_IN_RACE, True)])
            self.set_session_host(participants_selected=1)
            self.event_state, self.event_due = 'loading', now + self.EVENT_GATHER_MS
            print(f'{time.strftime("%H:%M:%S")} event: participants {[p.index for p in self.participants]}', flush=True)
        elif self.event_state == 'loading':
            loaded = [p for p in self.participants if p.console_vars.get(nerd.CONSOLE_HAS_LOADED_CARS)]
            if len(loaded) == len(self.participants) or now >= self.event_due:
                cars = [p for p in loaded if p.car_id is not None]
                for to in self.participants:
                    grid = [dict(a=0, b=0, id=p.car_id) if p is to else dict(a=0, b=1, id=p.car_id, peer=p.index) for p in cars]
                    self.send_all(to, [nerd.starting_grid(grid)])
                    to.console_vars.pop(nerd.CONSOLE_RENDER_READY, None)   # from here: ready in the race scene
                self.event_state, self.event_due = 'grid', now + self.EVENT_GATHER_MS
                print(f'{time.strftime("%H:%M:%S")} event: grid {[(p.index, p.car_id) for p in cars]} '
                      f'(loaded: {[p.index for p in loaded]})', flush=True)
        elif self.event_state == 'grid':
            # With the grid each console switches to the race route; it says RenderReady when the race scene is
            # up (the starting grid screen). Then the start time goes out, a few seconds ahead.
            rendered = [p for p in self.participants if p.console_vars.get(nerd.CONSOLE_RENDER_READY)]
            if not rendered and now >= self.event_due:
                self.event_reset('no console got to the starting grid')
                return
            if len(rendered) == len(self.participants) or now >= self.event_due:
                start = now + self.EVENT_START_DELAY_MS
                for to in self.participants:
                    self.send_all(to, [nerd.start_race(start)])
                self.event_state, self.event_due, self.event_start = 'starting', start, start
                print(f'{time.strftime("%H:%M:%S")} event: start at {start} (render ready: {[p.index for p in rendered]})',
                      flush=True)
        elif self.event_state == 'starting' and now >= self.event_due:
            self.set_session_host(in_race=1, race_start_time=self.event_start)
            self.event_state, self.event_due = 'racing', now + self.EVENT_MAX_RACE_MS
            print(f'{time.strftime("%H:%M:%S")} event: in race', flush=True)
        elif self.event_state == 'racing' and now >= self.event_due:
            self.event_reset('race over: ' + ', '.join(f'{p.index} {"finished" if p.finished else "not finished"}'
                                                       for p in self.participants))

    def on_request_ownership(self, peer, o):
        """A console asks to own one of the server's objects (the leader: GameOptions, to set up an event). Granted
        in the object's update sequence, so it is counted like the server's updates."""
        key = (SERVER_OWNER, o['router'], o['id'])
        made_by_console = o['router'] == LOBBY_ROUTER and o['id'] in GAME_OPTIONS_IDS
        if not o['a'] or o['b'] or (key not in peer.out_seq and not made_by_console):
            return '(not an object of the server it knows: ignored)'
        granted = self.grants.get((o['router'], o['id']))
        if granted and granted[0] == peer.index and time.monotonic() - granted[1] < 2:
            return '(granted already)'                 # it asks every frame until the grant has arrived
        self.grants[(o['router'], o['id'])] = (peer.index, time.monotonic())
        seq = peer.out_seq[key] = (peer.out_seq.get(key, -1) + 1) % 255     # the console's own copy expects 0 first
        self.send_all(peer, [nerd.grant_ownership(o['router'], o['id'], seq)])
        self.owners[(o['router'], o['id'])] = peer.index
        return f'-> granted (seq {seq})'

    def on_lobby_update(self, peer, msg, o):
        """An update of a lobby object from the console that owns it (the leader's GameOptions: the event it set
        up) goes to the others as the server's update. Consoles make the GameOptions themselves, so the first
        update another console gets from us is seq 0."""
        key = (o['router'], o['id'])
        if msg != 8 or not o['a'] or o['b'] or self.owners.get(key) != peer.index:
            return '(not from its owner: ignored)'
        if self.lobby_seq.get(key) == o['seq']:
            return '(seen)'
        self.lobby_seq[key] = o['seq']
        sent = []
        for p in self.peers.values():
            if p is peer or not p.synced:
                continue
            okey = (SERVER_OWNER,) + key
            msgs = []
            # Its own copy was made by the console, not by us: whether it waits for update 0 or 1 first is not
            # known, so the first update goes as both (the same values; one of them is taken, a repeat is
            # harmless).
            for _ in range(1 if okey in p.out_seq else 2):
                seq = p.out_seq[okey] = (p.out_seq.get(okey, -1) + 1) % 255
                msgs.append(nerd.object_msg(8, o['router'], o['id'], seq, o['payload'], console_made=True))
            self.send_all(p, msgs)
            sent.append(p.index)
        return f'-> to {sent}'

    def ask_full_weather(self, peer):
        self.weather_asked = self.now()
        self.send(peer, [nerd.BitWriter().bits(nerd.REQUEST_FULL_WEATHER, 7)], sequenced=True)
        print(f'{time.strftime("%H:%M:%S")} weather: asked peer {peer.index} for its weather', flush=True)

    def weather_ms(self, ms, hold):
        return int(min(max(ms + hold, WEATHER_MIN_MS), WEATHER_MAX_MS))

    def weather_now(self, part):
        """The intensity the consoles show now, as they work it out (1406F0CB0)."""
        w, now = self.weather, self.now()
        c, ct, d, dt = (w[part + '_cur'], w[part + '_cur_tick'], w[part + '_des'], w[part + '_des_tick'])
        t = 0.0 if now <= ct else 1.0 if now >= dt else (now - ct) / (dt - ct)
        return c + (d - c) * t

    def send_weather(self, only=None):
        payload = nerd.weather_vars(self.weather, only)
        for p in self.peers.values():
            if p.synced:
                m = self.server_object(p, 7 if only is None else 8, nerd.WEATHER_ROUTER, 0, payload, only_new=True)
                if m is None and only is not None:      # a console that joined before there was weather
                    m = self.server_object(p, 7, nerd.WEATHER_ROUTER, 0, nerd.weather_vars(self.weather), only_new=True)
                if m is not None:
                    self.send_all(p, [m])

    @staticmethod
    def weather_ok(part):
        """Values every console can use: finite numbers in sane ranges, preset indexes the game's tables have
        (a console uses them as indexes; seen: 0..8)."""
        nums = (part['target'], part['ms'], part['hold'])
        return (all(math.isfinite(x) for x in nums) and -10 <= part['target'] <= 10
                and 0 <= part['ms'] <= 1e8 and 0 <= part['hold'] <= 1e8
                and all(0 <= x < 64 for x in part.get('presets', [])))

    def on_weather(self, peer, msg, fields):
        now = self.now()
        parts = [nerd.read_atmosphere(fields[0][2]) if msg != nerd.PRECIPITATION else nerd.read_precipitation(fields[0][2])]
        if msg == nerd.FULL_WEATHER:
            parts.append(nerd.read_precipitation(fields[1][2]))
        if not all(self.weather_ok(x) for x in parts):
            return f'{NAMES[msg]} {parts} (not usable: ignored)'
        if msg == nerd.FULL_WEATHER:
            a, p = parts
            print(f'{time.strftime("%H:%M:%S")} weather from peer {peer.index}: atmosphere {a}, now {fields[2][2]}; '
                  f'precipitation {p}, now {fields[3][2]}', flush=True)
            if self.weather is not None:
                return 'FullWeather (already have the weather)'
            # Everyone starts at the sending console's targets (its own weather jumps there too).
            self.weather = dict(a_cur=a['target'], a_cur_tick=now, a_des=a['target'], a_des_tick=now,
                                presets=a['presets'], p_cur=p['target'], p_cur_tick=now, p_des=p['target'], p_des_tick=now)
            self.next_weather = {'a': now + self.weather_ms(a['ms'], a['hold']), 'p': now + self.weather_ms(p['ms'], p['hold'])}
            self.send_weather()
            return 'FullWeather -> the session weather'
        w = self.weather
        if w is None:
            return f'{NAMES[msg]} (no session weather yet)'
        if msg == nerd.ATMOSPHERE:
            a = parts[0]
            w.update(a_cur=self.weather_now('a'), a_cur_tick=now, a_des=a['target'],
                     a_des_tick=now + max(int(a['ms']), 0), presets=a['presets'])
            self.next_weather['a'] = now + self.weather_ms(a['ms'], a['hold'])
            self.send_weather(only={0, 1, 2, 3, 4})
            return f'NewAtmosphericData {a}'
        p = parts[0]
        w.update(p_cur=self.weather_now('p'), p_cur_tick=now, p_des=p['target'], p_des_tick=now + max(int(p['ms']), 0))
        self.next_weather['p'] = now + self.weather_ms(p['ms'], p['hold'])
        self.send_weather(only={5, 6, 7, 8})
        return f'NewPrecipitationData {p}'

    def weather_tick(self):
        """Ask the leader (or anyone) for new weather data when it's due."""
        synced = [p for p in self.peers.values() if p.synced]
        if not synced:
            return
        now = self.now()
        who = next((p for p in synced if p.index == self.leader), synced[0])
        if self.weather is None:
            if self.weather_asked is None or now - self.weather_asked > 10_000:
                self.ask_full_weather(who)
            return
        for part, req in (('a', nerd.REQUEST_ATMOSPHERE), ('p', nerd.REQUEST_PRECIPITATION)):
            if now >= self.next_weather.get(part, now + 1):
                self.next_weather[part] = now + 30_000      # asked again if no answer comes
                self.send(who, [nerd.BitWriter().bits(req, 7)], sequenced=True)

    # ---- the handshake and packets ------------------------------------------------------------------------------

    def on_synced(self, peer):
        """The peer finished the handshake: TimeSync and ConnectionSyncDone, then the server's objects and
        everything the others own."""
        self.send(peer, [nerd.time_sync(peer.sync_index, self.now()), nerd.sync_done(OWN_ATTEMPT)])
        peer.sync_index += 1
        world = [self.server_object(peer, 7, SESSION_HOST[0], SESSION_HOST[1], self.session_host_payload(), type=0)]
        if self.weather is not None:
            world.append(self.server_object(peer, 7, nerd.WEATHER_ROUTER, 0, nerd.weather_vars(self.weather)))
        for owner, msg, o in self.history:
            world.append(self.relayed(peer, owner, msg, o['router'], o['id'], o['payload'], o.get('type', 0), o['seq']))
        self.send_all(peer, world)
        self.show_leader(peer)
        if self.weather is None and (self.weather_asked is None or self.now() - self.weather_asked > 10_000):
            self.ask_full_weather(peer)      # the first console's weather becomes the session's
        return len(world)

    def handle_for(self, h, owner, to):
        """A car handle from `owner`, as `to` knows that car (None: a car `to` doesn't have)."""
        if h['a']:
            return h
        if not h['b']:                       # the owner's own car
            return dict(h, b=1, peer=owner.index)
        if h['peer'] == to.index:            # the receiver's own car
            return dict(h, b=0)
        if not any(p.index == h['peer'] for p in self.peers.values()):
            return None
        return h

    @staticmethod
    def not_a_console(data, r):
        """Why a packet from an unknown address isn't a console's first one (None: it is): it must start with an
        OpenConnection for FH2 (game "Flagship", version = the game's 8-digit build hash)."""
        try:
            q = nerd.BitReader(data, r.pos)
            if q.bits(7) != 0:
                return 'not an OpenConnection'
            oc = nerd.read_open_connection(q)
        except Exception:
            return 'unreadable'
        if oc['game'] != 'Flagship' or len(oc['version']) != 8 or not all(c in '0123456789ABCDEFabcdef' for c in oc['version']):
            return f'not FH2 ({oc["game"][:16]!r} {oc["version"][:38]!r})'
        return None

    def handle(self, data, addr):
        p = nerd.parse(data)
        if not p:
            return ['short packet'], None
        hdr, r = p
        peer = self.peers.get(addr)
        # A new launch from the same address starts over at packet 0 with an OpenConnection.
        if peer and peer.synced and hdr['id'] == 0 and r.left() >= 7 and nerd.BitReader(data, r.pos).bits(7) == 0:
            self.drop(peer, 'new connection')
            peer = None
        if peer is None:
            # Only a console's OpenConnection gets a player slot - not port scans or stray packets (seen through
            # the tunnel: RakNet pings), which would otherwise fill the session.
            why = self.not_a_console(data, r)
            if why:
                self.strangers += 1
                if self.strangers <= 5 or self.strangers % 100 == 0:
                    print(f'{time.strftime("%H:%M:%S")} [{addr[0]}]:{addr[1]} ignored ({why}; {self.strangers} so far)', flush=True)
                return [f'ignored: {why}'], None
            if not self.free:
                print(f'{time.strftime("%H:%M:%S")} [{addr[0]}]:{addr[1]} refused: the session is full', flush=True)
                return ['refused: the session is full'], None
            peer = self.peers[addr] = Peer(addr, self.free.pop(0), next(self.joins))
        peer.last_seen = time.monotonic()
        out, notes, ready, out_seq, unseq, seqd, mirror_objs = [], [], [], [], [], [], []
        new_player = False
        peer.acks.add(hdr['id'])
        self.on_acks(peer, hdr['ack'], hdr['ackbits'])
        notes.append(f'peer {peer.index} id {hdr["id"]} cnt {hdr["count"]}')
        try:
            new_player = self.read_messages(peer, hdr, r, data, out, notes, ready, out_seq, unseq, seqd, mirror_objs)
        except Exception as e:               # a cut-off or malformed packet: keep what was read before it
            peer.bad += 1
            notes.append(f'unreadable from here ({type(e).__name__}: {e})')
        if peer.addr not in self.peers:      # it closed the connection
            return notes, None
        if peer.kept > MAX_HISTORY_PER_PEER:
            self.drop(peer, f'more than {MAX_HISTORY_PER_PEER} object messages')
            return notes, None
        return self.pass_on(peer, hdr, out, notes, ready, out_seq, unseq, seqd, mirror_objs, new_player)

    def read_messages(self, peer, hdr, r, data, out, notes, ready, out_seq, unseq, seqd, mirror_objs):
        """Reads the packet's messages into the lists; returns whether its PlayerData was just made."""
        new_player = False
        for _ in range(hdr['count']):
            if r.left() < 7:
                break
            msg = r.bits(7)
            name = NAMES.get(msg, f'msg {msg}')
            if msg == 0:
                oc = nerd.read_open_connection(r)
                peer.user = oc['user']
                notes.append(f'{name} user {oc["user"]} attempt {oc["attempt"]}')
                out.append(nerd.open_response(oc['flags'], 0, oc['attempt'], OWN_ATTEMPT))
            elif msg == 1:
                resp = nerd.read_open_response(r)
                notes.append(f'{name} reason {resp["reason"]}')
                if resp['reason'] == 6 and not peer.synced:
                    peer.synced = True
                    notes.append(f'synced; sent the world ({self.on_synced(peer)} objects)')
            elif msg == 2:
                notes.append(name)
                self.drop(peer, 'closed')
                return False
            elif msg == 3:
                out.append(nerd.pong(nerd.read_ping(r)['time'], self.now()))
                notes.append(name)
            elif msg == 4:
                nerd.read_pong(r)
                notes.append(name)
            elif msg in (7, 8):
                try:
                    o = nerd.read_object(r, msg)
                except ValueError as e:
                    notes.append(f'{name}: {e}; rest not read ({r.left()} bits)')
                    break
                # A console sends a message again until it is acked. A resent update is caught by in_order (its
                # seq is behind); a resent create would start the object over, so creates are remembered.
                key = (peer.index, msg, o['router'], o['id'], o['seq'])
                again = msg == 7 and key in self.seen
                notes.append(f'{name} router {o["router"]} id {o["id"]}' + (f' type {o["type"]}' if 'type' in o else '')
                             + f' seq {o["seq"]} {o["payload"].pos} bits' + (' (resent)' if again else ''))
                if again:
                    continue
                if o['router'] == LOBBY_ROUTER:     # the lobby's objects belong to the server: an owner's changes
                    notes.append(self.on_lobby_update(peer, msg, o))
                    continue
                if msg == 7:
                    self.seen.add(key)
                if msg == 7 and o['router'] == 3:
                    peer.console_id = o['id']
                if o['router'] == 3:
                    peer.console_vars.update(nerd.read_console_vars(o['payload']))
                if msg == 7 and o['router'] == nerd.PLAYER_ROUTER:
                    peer.player_id, peer.own_leader, new_player = o['id'], None, True
                    named = nerd.read_player_name(o['payload'])
                    if named and named[0] and len(self.names) < 256:
                        self.names[str(named[1])] = named[0][:32]
                    for q in self.peers.values():
                        q.leader_shown.pop(peer.index, None)
                peer.objects[(o['router'], o['id'])] = o['seq']
                for m, ro in self.in_order(peer, msg, o):
                    peer.kept += 1
                    self.history.append((peer.index, m, ro))
                    ready.append((m, ro))
                    mirror_objs.append((m, ro))
            elif msg == 15:
                h, body = nerd.read_car(r)
                unseq.append(('car', h, body))
                if self.mirror is not None:
                    self.delayed.append((time.monotonic() + self.mirror, peer, nerd.car_msg(h['id'], body, peer=MIRROR_INDEX)))
                peer.cars += 1
                if not h['a'] and not h['b']:
                    peer.car_id = h['id']
                notes.append(name)
            elif msg == 21:
                # MsgCarCollision: the car's handle, then 192 bits (all 1251 captured 2026-11-04 are followed by a
                # well-formed CarPredictionFull exactly there). Passed on like the car, its handle as each
                # receiver knows that car.
                h, body = nerd.read_collision(r)
                unseq.append(('collision', h, body))
                peer.collisions += 1
                notes.append(name)
            elif msg == nerd.REPAIR_DAMAGE:  # the car's handle: passed on (sequenced) like a collision
                h = nerd.read_handle(r, 7)
                seqd.append((msg, h))
                notes.append(name)
            elif msg == 34:                  # no fields: the console has loaded the session's resources
                notes.append(name)
                if peer.console_id is not None and not peer.finalised:
                    peer.finalised = True
                    out_seq.append(self.set_console_var(peer, nerd.CONSOLE_SESSION_ENTRY_FINALISED, True))
                    notes.append('-> SessionEntryFinalised')
            elif msg == 9:
                d = nerd.read_delete(r)
                notes.append(f'{name} router {d["router"]} id {d["id"]} seq {d["seq"]}')
                self.delete_object(peer, d['router'], d['id'])
            elif msg in nerd.MSG_FORMATS:
                fields = nerd.read_fields(r, nerd.MSG_FORMATS[msg])
                if msg in FORWARD_UNSEQUENCED or msg in FORWARD_SEQUENCED:
                    last = peer.last_fields.get(msg)
                    if msg in FORWARD_SEQUENCED and last and last[0] == fields and time.monotonic() - last[1] < 5:
                        notes.append(f'{name} (again)')       # sent again until acked: passed on once
                        continue
                    peer.last_fields[msg] = (fields, time.monotonic())
                    (unseq if msg in FORWARD_UNSEQUENCED else seqd).append(('fields', msg, fields))
                    notes.append(name)
                    if msg == nerd.MSG_SCORE_KEYFRAME:
                        notes.append(self.on_final_score(peer))
                elif msg in (nerd.FULL_WEATHER, nerd.ATMOSPHERE, nerd.PRECIPITATION):
                    notes.append(self.on_weather(peer, msg, fields))
                elif msg == nerd.MSG_READY_REQUEST:
                    notes.append(f'{name} ' + self.on_ready_request(peer, fields))
                elif msg == nerd.MSG_REQUEST_RACE:
                    notes.append(f'{name} ' + self.on_request_race(peer))
                elif msg in TRAFFIC:
                    peer.traffic += 1
                    notes.append(name)
                else:
                    notes.append(f'{name} (not handled)')
            elif msg == nerd.OBJECT_REQUEST_OWNERSHIP:
                o = nerd.read_request_ownership(r)
                notes.append(f'{name} router {o["router"]} type {o["type"]} handle {o["a"]},{o["b"]} id {o["id"]} '
                             + self.on_request_ownership(peer, o))
            elif msg == 59:
                h = nerd.read_leader_change(r)
                target = peer.index if not h['b'] else h.get('peer')
                notes.append(f'{name} {target} ' + self.leader_request(peer, target))
            elif msg == 60:
                notes.append(f'{name} ' + self.leader_request(peer, None))
            else:
                notes.append(f'{name} (+{r.left()} bits, not read)')
                break
        return new_player

    def pass_on(self, peer, hdr, out, notes, ready, out_seq, unseq, seqd, mirror_objs, new_player):
        """Sends on what a packet brought: to the others, and the answers and the ack to the sender."""
        if ready:
            for p in self.others(peer):
                self.send_all(p, [self.relayed(p, peer.index, m, o['router'], o['id'], o['payload'], o.get('type', 0), o['seq'])
                                  for m, o in ready])
            if self.mirror is not None:      # the console's own objects, back to it as the ghost's
                self.send_all(peer, [nerd.object_msg(m, o['router'], o['id'], o['seq'], o['payload'], peer=MIRROR_INDEX, type=o.get('type', 0))
                                     for m, o in mirror_objs])
        if new_player:                       # after its create went out: the others can be told its IsLeader
            self.update_leader(prefer=peer if self.leader_xuid and peer.user == self.leader_xuid else None)
        if unseq:
            for p in self.others(peer):
                msgs = []
                for kind, h, body in unseq:
                    if kind == 'car':
                        msgs.append(nerd.car_msg(h['id'], body, peer=peer.index))
                    elif kind == 'fields':            # h: the message id, body: its fields
                        fields = [f if f[0] != 'h' else ('h', f[1], self.handle_for(f[2], peer, p)) for f in body]
                        if all(f[0] != 'h' or f[2] is not None for f in fields):
                            msgs.append(nerd.write_fields(h, fields))
                    else:
                        th = self.handle_for(h, peer, p)
                        if th is not None:
                            msgs.append(nerd.collision_msg(th, body))
                self.send_all(p, msgs, sequenced=False)
        if seqd:
            for p in self.others(peer):
                msgs = []
                for item in seqd:
                    if item[0] == 'fields':
                        fields = [f if f[0] != 'h' else ('h', f[1], self.handle_for(f[2], peer, p)) for f in item[2]]
                        if all(f[0] != 'h' or f[2] is not None for f in fields):
                            msgs.append(nerd.write_fields(item[1], fields))
                    else:
                        th = self.handle_for(item[1], peer, p)
                        if th is not None:
                            msgs.append(nerd.handle_msg(item[0], th))
                if msgs:
                    self.send_all(p, msgs)
        if out_seq:
            self.send(peer, out_seq, sequenced=True)
        reply = None
        if out or hdr['id'] != nerd.NO_SEQ:
            reply = self.send(peer, out)
        return notes, reply

    def run(self):
        while True:
            try:
                data, addr = self.sock.recvfrom(65536)
            except ConnectionResetError:     # Windows: a send of ours reached a closed port (a console left)
                continue
            if data.startswith(b'FH2N?'):    # a local server asks for the players' gamertags
                with self.mu:
                    reply = b'FH2N!' + json.dumps(self.names, ensure_ascii=True).encode()
                try:
                    self.sock.sendto(reply[:1400], addr)
                except OSError:
                    pass
                continue
            if data.startswith(b'FH2O?'):    # Horizon Connect's server list asks whether this server is up
                with self.mu:
                    info = {'players': len(self.peers), 'max': MAX_PLAYERS - 1 - (1 if self.mirror is not None else 0)}
                try:
                    self.sock.sendto(b'FH2O!' + json.dumps(info).encode(), addr)
                except OSError:
                    pass
                continue
            if len(data) > MAX_PACKET:
                continue
            with self.mu:
                self.n += 1
                try:
                    notes, reply = self.handle(data, addr)
                except Exception as e:           # never let one packet stop the relay
                    notes, reply = [f'error {type(e).__name__}: {e}'], None
            rec = {'n': self.n, 'time': time.time(), 'from': f'[{addr[0]}]:{addr[1]}', 'hex': data.hex(), 'what': ' | '.join(notes)}
            if reply:
                rec['reply'] = reply.hex()
            routine = all(x in ('CarPredictionFull', 'MsgCarCollision') for x in notes[1:])
            if not routine:                  # the cars would fill the log; they are counted instead
                with self.lock, open(os.path.join(self.out_dir, 'udp.jsonl'), 'a', encoding='utf-8') as f:
                    f.write(json.dumps(rec) + '\n')
            quiet = all(x in ('Ping', 'Pong', 'CarPredictionFull', 'MsgCarCollision') or x.startswith('peer ') for x in notes) and self.n > 30
            if not quiet or self.n % 500 == 0:
                print(f'{time.strftime("%H:%M:%S")} udp #{self.n} [{addr[0]}]:{addr[1]} {len(data)} bytes: {rec["what"]}', flush=True)

    def mirror_cars(self):
        """The mirror's car messages, each sent back to its console once its delay is up."""
        while True:
            time.sleep(0.005)
            with self.mu:
                now = time.monotonic()
                due = [d for d in self.delayed if d[0] <= now]
                self.delayed = [d for d in self.delayed if d[0] > now]
                for _, p, m in due:
                    if p.addr in self.peers:
                        self.send(p, [m])

    def keepalive(self):
        """Ping each synced peer every half second (with the current acks); drop peers quiet for 15 s."""
        while True:
            time.sleep(0.5)
            with self.mu:
                for p in list(self.peers.values()):
                    if time.monotonic() - p.last_seen > QUIET_DROP_S:
                        self.drop(p, f'quiet for {QUIET_DROP_S} s')
                    else:
                        if p.synced:
                            self.send(p, [nerd.ping(self.now())])
                        self.resend(p)
                try:
                    self.weather_tick()
                    self.event_tick()
                except Exception as e:           # never let one bad value stop the pings
                    print(f'{time.strftime("%H:%M:%S")} tick: {type(e).__name__}: {e}', flush=True)


def start(out_dir, lock, port=5101, host='127.0.0.1', mirror=None, leader_xuid=None, start_hour=10.0):
    relay = Relay(out_dir, lock, port, host, mirror, leader_xuid, start_hour)
    threading.Thread(target=relay.run, daemon=True).start()
    if mirror is not None:
        threading.Thread(target=relay.mirror_cars, daemon=True).start()
    threading.Thread(target=relay.keepalive, daemon=True).start()
    return relay
