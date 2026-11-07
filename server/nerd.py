"""NerdCore (Engine\\NetV2\\NerdCore) UDP packets, as the game reads and writes them.

Packet: an 11-byte header, then messages in a bit stream (LSB first, padded to a byte at the end).
  header (14176D3E0): id u16, ack u16, ack bits u32, message count u8, packet size u16 (whole packet, bytes);
  little-endian. An id of 0xFFFF skips the receiver's sequence check (Connection::ReadPacketHeader, 1417672B0).
Message: id in 7 bits (BitStream::WriteBits, 14176B380), then its fields.
  Compact uint (14176AA80): 2-bit size s, then 8*s+6 bits.  String (14176A900): compact length, then 8-bit chars.

Connection messages:
  0 OpenConnection (14176D450): version str (max 38), game str (max 16), 2 bits, u64 user id, u32, u32,
    u32 connect attempt (the client's [conn+0x24]).
  1 OpenConnectionResponse (14176D770): 2 bits (echoed), 3-bit reason, u32 (must equal the client's attempt),
    u32 (the sender's own attempt; the client keeps it in [conn+0x28]).
    Reasons (141765B30 / 141765EC0): 0 Ok, 1 version mismatch, 2 game mismatch, 3 banned, 4/5 refused,
    6 ResponseAck (the client's answer to Ok, after which both sides count as connected).
"""
import struct

HEADER = struct.Struct('<HHIBH')
NO_SEQ = 0xFFFF


class BitReader:
    def __init__(self, data, pos=0):
        self.data, self.pos = data, pos

    def bits(self, n):
        """The next n bits as an int (whole bytes at a time: a 16000-bit payload is one slice, not a loop)."""
        end = self.pos + n
        if end > len(self.data) * 8:
            raise IndexError('read past the end of the packet')
        if n == 0:
            return 0
        chunk = int.from_bytes(self.data[self.pos >> 3:(end + 7) >> 3], 'little')
        self.pos = end
        return (chunk >> ((end - n) & 7)) & ((1 << n) - 1)

    def cuint(self):
        return self.bits(8 * self.bits(2) + 6)

    def string(self):
        n = self.cuint()
        return self.bits(8 * n).to_bytes(n, 'little').decode('latin1')

    def left(self):
        return len(self.data) * 8 - self.pos


class BitWriter:
    """Bits written LSB first, kept as one int (val) of pos bits; buf is its bytes."""
    def __init__(self):
        self.val, self.pos = 0, 0

    def bits(self, v, n):
        self.val |= (v & ((1 << n) - 1)) << self.pos
        self.pos += n
        return self

    @property
    def buf(self):
        return self.val.to_bytes((self.pos + 7) >> 3, 'little')

    def cuint(self, v):
        s = 3 if v & 0xFFC00000 else 2 if v & 0xFFC000 else 1 if v & 0xFFC0 else 0
        return self.bits(s, 2).bits(v, 8 * s + 6)

    def string(self, s):
        b = s.encode('latin1')
        self.cuint(len(b))
        for c in b:
            self.bits(c, 8)
        return self


def parse(data):
    """(header dict, BitReader at the first message) or None if too short."""
    if len(data) < HEADER.size:
        return None
    pid, ack, ackbits, count, size = HEADER.unpack_from(data)
    return dict(id=pid, ack=ack, ackbits=ackbits, count=count, size=size), BitReader(data, HEADER.size * 8)


def packet(body: BitWriter, count=1, pid=NO_SEQ, ack=0xFFFE, ackbits=0):
    size = HEADER.size + len(body.buf)
    return HEADER.pack(pid, ack, ackbits, count, size) + bytes(body.buf)


def read_open_connection(r):
    return dict(version=r.string(), game=r.string(), flags=r.bits(2), user=r.bits(64),
                a=r.bits(32), b=r.bits(32), attempt=r.bits(32))


def read_open_response(r):
    return dict(flags=r.bits(2), reason=r.bits(3), attempt=r.bits(32), peer_attempt=r.bits(32))


def open_response(flags, reason, attempt, own_attempt):
    return BitWriter().bits(1, 7).bits(flags, 2).bits(reason, 3).bits(attempt, 32).bits(own_attempt, 32)


# Ping/Pong (14176DA90 / 14176DBA0): the client pings with its time (ms); the answer is Pong(its time, our
# time); it then measures its round trip and answers Pong(0, our time) for ours (1417666C0).
def read_ping(r):
    return dict(time=r.bits(32))


def read_pong(r):
    return dict(echo=r.bits(32), time=r.bits(32))


def ping(now):
    return BitWriter().bits(3, 7).bits(now & 0xFFFFFFFF, 32)


def pong(echo, now):
    return BitWriter().bits(4, 7).bits(echo & 0xFFFFFFFF, 32).bits(now & 0xFFFFFFFF, 32)


# Server to client only. TimeSync (14176DCF0): u16, u32 server time (ms). ConnectionSyncDone (14176DD80):
# u32 that must equal the value the server sent in its OpenConnectionResponse (the client's [conn+0x28]);
# the client then tells its network manager the connection is synchronised (141764C40).
def time_sync(index, now):
    return BitWriter().bits(5, 7).bits(index & 0xFFFF, 16).bits(now & 0xFFFFFFFF, 32)


def sync_done(own_attempt):
    return BitWriter().bits(6, 7).bits(own_attempt, 32)


# Object messages (NerdLib SyncedObject, 1417B1150 / 1417AB160). ObjectCreate (7) and ObjectUpdate (8):
#   router id (4 bits, 1422E4410), handle, [Create, if the router's manager has more than one type: type id],
#   seq u8, payload size u14 (bits), payload (per changed var: var index, var data).
# Handle (1417B0E40): two flag bits a, b, then the id (the manager's width). a=b=1: flag 29; b only: flag 30 and
#   an 8-bit peer index follows (the object belongs to that peer, not to the sender); a only: flag 31.
#   Plain (0, 0): the object belongs to the connection it came from.
# Routers seen (id bits, type bits or 0): 1, 2, 3: (7, 0) - the player's objects; 9: (4, 2), 3 types;
#   12: (4, 2), 2 types; 6: (2, 2), the lobby's: type 0 SessionHost, 1 GameOptions (only the server makes these).
#   10: (1, 0), the session's weather (WeatherSynced, the server's). Router ids are each class's vtable+0x38
#   (1 PlayerData, 3 ConsoleSynced, 6 SessionHost/GameOptions, 9 RoadTrip, 10 Weather, 11 CarMeet, 12 BucketList);
#   the widths come from the router's SyncedObjectManager (1417A7C60: bit length of max objects / max types).
ROUTERS = {1: (7, 0), 2: (7, 0), 3: (7, 0), 6: (2, 2), 9: (4, 2), 10: (1, 0), 12: (4, 2)}


def read_object(r, msg):
    """ObjectCreate/ObjectUpdate after the message id; the payload is kept as raw bits."""
    router = r.bits(4)
    idw, typew = ROUTERS.get(router, (None, None))
    if idw is None:
        raise ValueError(f'unknown router {router}')
    a, b = r.bits(1), r.bits(1)
    o = dict(router=router, a=a, b=b, id=r.bits(idw))
    if b and not a:
        o['peer'] = r.bits(8)
    if msg == 7 and typew:
        o['type'] = r.bits(typew)
    o['seq'] = r.bits(8)
    n = r.bits(14)
    o['payload'] = BitWriter().bits(r.bits(n), n)
    return o


def object_msg(msg, router, id, seq, payload=None, peer=None, type=0, console_made=False):
    """console_made: one of the server's objects that each console makes itself (the lobby's GameOptions 0 and 2,
    CLobby 140A2BF84): the console keeps it under flags (1, 0), not under (0, 0) like the objects we create, so
    updates of it go with flags 1, 0."""
    idw, typew = ROUTERS[router]
    w = BitWriter().bits(msg, 7).bits(router, 4)
    w.bits(1 if console_made else 0, 1).bits(1 if peer is not None else 0, 1).bits(id, idw)
    if peer is not None:
        w.bits(peer, 8)
    if msg == 7 and typew:
        w.bits(type, typew)
    w.bits(seq & 0xFF, 8).bits(payload.pos if payload else 0, 14)
    return concat(w, payload) if payload else w


def read_handle(r, idw):
    a, b = r.bits(1), r.bits(1)
    h = dict(a=a, b=b, id=r.bits(idw))
    if b and not a:
        h['peer'] = r.bits(8)
    return h


def write_handle(w, idw, id, peer=None):
    """A console's own handle (0, 0), or, with peer, the relay form (0, 1) + the owning peer's index."""
    w.bits(0, 1).bits(1 if peer is not None else 0, 1).bits(id, idw)
    if peer is not None:
        w.bits(peer, 8)
    return w


def object_delete(router, id, seq, peer=None):
    """ObjectDelete (9; 1417AB560): router, handle, seq u8 (newer than the object's last)."""
    idw, _ = ROUTERS[router]
    return write_handle(BitWriter().bits(9, 7).bits(router, 4), idw, id, peer).bits(seq & 0xFF, 8)


# CarPredictionFull (15; send 141808170, receive 1417CF0A0 / 141808240): the car's handle (7-bit id: the
# player's car object), the prediction (678 bits, fixed: 14181FFE0 96 + 141822400 326 + 4 x 141825F00 64),
# then a u32. Sent unsequenced, about 30 a second.
CAR_BODY_BITS = 678 + 32


def read_car(r):
    h = read_handle(r, 7)
    return h, BitWriter().bits(r.bits(CAR_BODY_BITS), CAR_BODY_BITS)


def car_msg(id, body, peer=None):
    return concat(write_handle(BitWriter().bits(15, 7), 7, id, peer), body)


def server_update(router, id, seq, payload):
    """An ObjectUpdate from the server for server-owned vars of a console's own object: handle flags (1, 1)
    and the console's own object id (SyncedObject::PackServerUpdateObjectId, 1417B1A20)."""
    idw, _ = ROUTERS[router]
    w = BitWriter().bits(8, 7).bits(router, 4).bits(1, 1).bits(1, 1).bits(id, idw)
    w.bits(seq & 0xFF, 8).bits(payload.pos, 14)
    return concat(w, payload)


# ConsoleSynced (router 3; NerdLib 1417DE550): 9 vars, so a 4-bit var index. 0 RenderReady, 1 SessionEntryFinalised,
# 2 ParticipatingInRace, 3 RaceInProgress, 4 Ready, 5 HasLoadedCarsForAllParticipants, 6 HasGrid,
# 7 AvailableArbitration, 8 MatchTicketArbiter. Bools are 1 bit; 1, 2 and 4 are the server's (SyncedBool<0,1>).
CONSOLE_SESSION_ENTRY_FINALISED = 1
CONSOLE_RENDER_READY = 0
CONSOLE_RACE_IN_PROGRESS, CONSOLE_HAS_LOADED_CARS, CONSOLE_HAS_GRID = 3, 5, 6


def read_console_vars(payload):
    """{index: value} from a ConsoleSynced update: 0-6 are bools, 7 AvailableArbitration 2 bits (seen: 1, 2)."""
    r, out = BitReader(bytes(payload.buf)), {}
    while r.pos + 5 <= payload.pos:
        i = r.bits(4)
        if i > 7 or r.pos + (2 if i == 7 else 1) > payload.pos:
            break
        out[i] = r.bits(2 if i == 7 else 1)
    return out
CONSOLE_PARTICIPATING_IN_RACE = 2
CONSOLE_READY = 4

# SessionHostSynced (router 6 type 0; NerdLib 14180FF70): 15 vars, so a 4-bit var index. 0 LobbyType, 1 PrivacyMode
# (SyncedEnum<SessionPrivacy>, 2 bits: 0 Public, 1 InviteOnly - stateflow get_session_privacy, NetworkGetSessionPrivacy
# 1408C80C0), 2 CountdownType, 3 CountdownEndTime, 4 RaceReady, ... The leader icon is only shown in an InviteOnly
# session (HUD mini leaderboard 140ABF1C0: SessionHost PrivacyMode 0 hides it), and Horizon Connect's sessions are
# private free roam: the server's SessionHost says InviteOnly.
SESSION_HOST_INDEX_BITS = 4
SESSION_PRIVACY = 1
SESSION_INVITE_ONLY = 1


# The rest of SessionHostSynced (its constructor 14180FF70; widths from each var's type): 2 CountdownType (enum
# like PrivacyMode, SyncedEnum<..., 0, 1, 0>: 2 bits), 3 CountdownEndTime (u32, server ms), 4 RaceReady,
# 5 RaceParticipantsSelected, 6 InRace (bools), 7 LastInactivePlayersPulledTime (u32), 8 RoundGuid (string, max 36),
# 9 RaceStartTime (u32), 10 NumLapsCompleted (u8), 11 FurthestProgress (f32), 12 GroupSatNavNode (s32).
# PrivacyMode is read as 2 bits: written as 1 bit, a joining console read past the packet and crashed (game+1767799).
SESSION_COUNTDOWN_TYPE, SESSION_COUNTDOWN_END, SESSION_RACE_READY = 2, 3, 4
SESSION_PARTICIPANTS_SELECTED, SESSION_IN_RACE, SESSION_RACE_START_TIME = 5, 6, 9


def session_host_vars():
    return BitWriter().bits(SESSION_PRIVACY, SESSION_HOST_INDEX_BITS).bits(SESSION_INVITE_ONLY, 2)


def session_host_update(**values):
    """SessionHost vars by name: countdown_type, countdown_end, race_ready, participants_selected, in_race,
    race_start_time."""
    layout = [('countdown_type', SESSION_COUNTDOWN_TYPE, 2), ('countdown_end', SESSION_COUNTDOWN_END, 32),
              ('race_ready', SESSION_RACE_READY, 1), ('participants_selected', SESSION_PARTICIPANTS_SELECTED, 1),
              ('in_race', SESSION_IN_RACE, 1), ('race_start_time', SESSION_RACE_START_TIME, 32)]
    w = BitWriter()
    for name, index, bits in layout:
        if name in values:
            w.bits(index, SESSION_HOST_INDEX_BITS).bits(int(values[name]) & ((1 << bits) - 1), bits)
    return w


# Events (client side in the game; the server's side is ours). MsgRequestRace (38, no fields): the leader starts
# the event. MsgReadyRequest (41): a lobby handle (2-bit id) and a bool - a console says it is (not) ready.
# The server answers MsgReadyRequestResponse (42: 2 bits, receive 141824265) and sets the console's Ready.
# Then MsgStartingGrid (39: 4-bit count, then that many 7-bit car handles; receive 141824057) and MsgStartRace
# (40: u32 start time; receive 1418241F5) - these three from the server begin with a lobby handle (lobby_event).
# A racer's scores: MsgInProgressScoreUpdate (43), MsgLapScore (44), MsgScoreKeyframe (45, at the finish), each
# with the racer's PlayerData handle first (formats in MSG_FORMATS).
MSG_REQUEST_RACE, MSG_STARTING_GRID, MSG_START_RACE = 38, 39, 40
MSG_READY_REQUEST, MSG_READY_RESPONSE = 41, 42
MSG_IN_PROGRESS_SCORE, MSG_LAP_SCORE, MSG_SCORE_KEYFRAME = 43, 44, 45


def lobby_event(msg, target=1):
    """A lobby event message from the server starts with the handle of the lobby object it is for (NerdLib's
    RouterEventMessageHandler, 1418091C0: a 2-bit id handle, looked up in router 6, then the ILobbyEvents of that
    object gets the fields). StartingGrid, StartRace and ReadyRequestResponse go to the server's SessionHost (the
    game's SessionHost implements them: 140A2E130, 140A2E020); without the handle the console reads the fields as
    one, finds nothing, and leaves the session."""
    return write_handle(BitWriter().bits(msg, 7), ROUTERS[6][0], target)


def starting_grid(handles, target=1):
    """MsgStartingGrid: the participants' cars in grid order, as the receiver knows them (handle dicts: its own car
    (0, 0), the others' as the relay forwards them, (0, 1) + the owner's index). Each handle is read with
    1417BA4D0 and compared with the car's own handle as it is (140A31140)."""
    w = lobby_event(MSG_STARTING_GRID, target).bits(len(handles), 4)
    for h in handles:
        w.bits(h['a'], 1).bits(h['b'], 1).bits(h['id'], 7)
        if h['b'] and not h['a']:
            w.bits(h['peer'], 8)
    return w


def start_race(start_ms, target=1):
    return lobby_event(MSG_START_RACE, target).bits(start_ms & 0xFFFFFFFF, 32)


def ready_response(code, target=1):
    return lobby_event(MSG_READY_RESPONSE, target).bits(code, 2)


# PlayerData (router 1; NerdLib, constructor 1417F3250): the player's own object, one per console. Its vars in
# order: 0 BufChoices, 1 HasLivery, 2 IsNewLivery, 3 NetworkName, 4 XUID, 5 IsLeader, 6 NumReceivedKickVotes,
# 7 HasFinalScore, 8.. race points and the vars of its sub-objects - more than 16 in all, so its var index is
# 8 bits wide (captures: a create starts with index 3, a 5-bit length and the gamertag in 16-bit chars, then
# index 4 and the 64-bit XUID). IsLeader is a SyncedBool<0,1>, the server's: consoles never send it, and the
# game reads the crown (session leader) from it alone - the local player's (NetworkIsLocalPlayerLeader,
# 1408C8490) and the others' (NetworkIsAnyRemotePlayerLeader, 1408C8200).
PLAYER_ROUTER = 1
PLAYER_INDEX_BITS = 8
PLAYER_IS_LEADER = 5
PLAYER_NETWORK_NAME, PLAYER_XUID = 3, 4


def read_player_name(payload):
    """(gamertag, xuid) from a PlayerData create's payload (it starts with NetworkName, then XUID), or None."""
    try:
        r = BitReader(bytes(payload.buf))
        if r.bits(PLAYER_INDEX_BITS) != PLAYER_NETWORK_NAME:
            return None
        name = ''.join(chr(r.bits(16)) for _ in range(r.bits(5)))
        if r.bits(PLAYER_INDEX_BITS) != PLAYER_XUID:
            return None
        return name, r.bits(64)
    except IndexError:
        return None


def bool_var(index, value, index_bits=4):
    return BitWriter().bits(index, index_bits).bits(1 if value else 0, 1)


# MsgCarCollision (21; send 141804D30, receive 141804E20): the car's handle (7-bit id), a vector (3 floats,
# 14181FFA0) and 3 floats - 192 bits after the handle. Sent unsequenced, next to the car's CarPredictionFull.
COLLISION_BODY_BITS = 192


def read_collision(r):
    h = read_handle(r, 7)
    return h, BitWriter().bits(r.bits(COLLISION_BODY_BITS), COLLISION_BODY_BITS)


def handle_msg(msg, h, body=None):
    """A message that is a car handle (7-bit id) and maybe more bits after it."""
    w = BitWriter().bits(msg, 7).bits(h['a'], 1).bits(h['b'], 1).bits(h['id'], 7)
    if h['b'] and not h['a']:
        w.bits(h['peer'], 8)
    return concat(w, body) if body else w


def collision_msg(h, body):
    return handle_msg(21, h, body)


# MsgRepairDamage (35; 141804F30): the car's handle (7-bit id). Sent sequenced when a car is repaired.
REPAIR_DAMAGE = 35


# RequestLeaderChange (59; 1418159B0): the handle (7-bit id) of the player to make leader. DropLeadership (60;
# 1417F7A50): no fields. Both are a console's requests to the server.
def read_leader_change(r):
    return read_handle(r, 7)


# Formats of the other messages a console sends (from each one's Send function; the receive side reads the
# same). A field is ('h', id bits): a handle; ('u', n): n bits; ('blob',): compact length + 8-bit bytes
# (BitStream 14176A900 - strings and the 8-byte doubles of the score messages); ('bytes', n): an n-bit count, then
# that many 8-bit bytes. The relay reads these so the rest of a packet isn't lost behind them.
U = lambda n: ('u', n)
BYTES = lambda n: ('bytes', n)
H7, H2, BLOB = ('h', 7), ('h', 2), ('blob',)
VEC3 = U(96)                   # 3 floats (14181FFA0)
MSG_FORMATS = {
    16: [U(8), U(32), VEC3, U(144), U(32), U(1), U(1)],       # ServerTrafficFullSync (1417F6E70)
    17: [U(8), U(96), VEC3],                                   # ServerTrafficRouteProgressUpdate (1417F6C00)
    20: [H7, U(32), VEC3, U(32)],                              # MsgBreakGlass (141804B10)
    22: [U(16), U(384)],                                       # MsgCollObjPos (receive 1417DDA10)
    23: [U(16), U(384), U(32)],                                # MsgCollObjRest (1417DDDC0)
    24: [U(4), BYTES(11)],                                     # MsgCollObjDetachPart (send 1417DDB10; ~1 KB seen)
    25: [U(16), U(288), U(32), U(32)],                         # MsgCollObjPosVel (1417DDCC0)
    26: [U(2), U(32)],                                         # MsgCarMeetPlayerInviteRequest (1417F75D0)
    27: [U(1), U(2), U(32)],                                   # MsgCarMeetPlayerJoinOrCancelRequest (1417F70F0)
    31: [U(32)],                                               # MsgCarMeetConfiguredGameOptionsForWagerRace
    32: [BLOB, BLOB],                                          # MsgCarMeetGameModeSessionReady (1417F7380)
    33: [U(64), BYTES(7)],                                     # MsgChatManagement (1417F5040)
    36: [H7, H7, U(12)],                                       # MsgTagCollision (1418048B0)
    38: [],                                                    # MsgRequestRace
    41: [H2, U(1)],                                            # MsgReadyRequest (1417F7B20)
    43: [H7, U(32), U(1)],                                     # MsgInProgressScoreUpdate (1418040E0)
    44: [H7, BLOB, BLOB, BLOB, U(32), U(1), U(1)],             # MsgLapScore (141804270)
    45: [H7, BLOB, BLOB, BLOB, BLOB, BLOB, U(32), U(24), U(3)],  # MsgScoreKeyframe (1418044E0)
    46: [H2, U(64)],                                           # MsgVoteToKick (141815870)
    47: [H2, U(32)],                                           # MsgVoteForTrack (141811E60)
    49: [U(1)],                                                # SelectedNextRoadTripEvent
    51: [U(32), U(16)],                                        # ConfiguredGameOptionsForRoadTripEvent
    52: [H2, U(32)],                                           # SetGroupSatNavEvent (141815730)
    54: [U(224), U(96), U(32), U(32)],                         # FullWeather (1417F6840)
    56: [U(224)],                                              # NewAtmosphericData (1417F69E0)
    58: [U(96)],                                               # NewPrecipitationData (1417F6AF0)
    62: [U(1)],                                                # BucketListChallengeEnded
    63: [U(32), U(32)],                                        # RequestSynchronizeBucketListChallenge
    64: [],                                                    # RequestDesynchronizeBucketListChallenge
    70: [H2],                                                  # MsgInactivePlayersPulled (1417F7000)
    71: [U(4), U(36), U(32)],                                  # ServerTrafficMessage (1417F5210): type, data, seq
    73: [U(8), U(130), U(96), VEC3],                          # ServerTrafficFullRoute (1417F6CD0)
    74: [U(8), VEC3, U(48), U(32), U(1)],                     # ServerTrafficStationary (1417F6DB0)
}


def read_fields(r, fmt):
    out = []
    for f in fmt:
        if f[0] == 'h':
            out.append(('h', f[1], read_handle(r, f[1])))
        elif f[0] == 'u':
            out.append(('u', f[1], r.bits(f[1])))
        else:
            n = r.cuint() if f[0] == 'blob' else r.bits(f[1])
            if n * 8 > r.left():
                raise ValueError(f'{n} bytes announced, {r.left() // 8} left')
            out.append((f[0], f[1] if f[0] == 'bytes' else None, r.bits(8 * n).to_bytes(n, 'little')))
    return out


def write_fields(msg, fields):
    w = BitWriter().bits(msg, 7)
    for f in fields:
        if f[0] == 'h':
            h = f[2]
            w.bits(h['a'], 1).bits(h['b'], 1).bits(h['id'], f[1])
            if h['b'] and not h['a']:
                w.bits(h['peer'], 8)
        elif f[0] == 'u':
            w.bits(f[2], f[1])
        else:
            if f[0] == 'blob':
                w.cuint(len(f[2]))
            else:
                w.bits(len(f[2]), f[1])
            w.bits(int.from_bytes(f[2], 'little'), 8 * len(f[2]))
    return w


# Ownership (NerdLib SyncedObjectManager; 1417B18C0 sends the request, ClientSyncedObjectManager 1417AB720 reads the
# grant). ObjectRequestOwnership (10, console -> server): router (4), the type (if the router has several), the
# object's handle - the server's objects as (a=1, b=0). ObjectGrantOwnership (11, server -> console): router, the
# handle (a=1, b=0 - asserted), then like an update of that object: seq u8 (the object's next, as the console
# counts the server's updates of it) and a 14-bit payload size + payload (1417B1E70).
OBJECT_REQUEST_OWNERSHIP, OBJECT_GRANT_OWNERSHIP = 10, 11


def read_request_ownership(r):
    router = r.bits(4)
    idw, typew = ROUTERS.get(router, (None, None))
    if idw is None:
        raise ValueError(f'unknown router {router}')
    o = dict(router=router, type=r.bits(typew) if typew else 0)
    o.update(read_handle(r, idw))
    return o


def grant_ownership(router, id, seq, payload=None):
    idw, _ = ROUTERS[router]
    w = BitWriter().bits(OBJECT_GRANT_OWNERSHIP, 7).bits(router, 4).bits(1, 1).bits(0, 1).bits(id, idw)
    w.bits(seq & 0xFF, 8).bits(payload.pos if payload else 0, 14)
    return concat(w, payload) if payload else w


def read_delete(r):
    """ObjectDelete (9) after the message id: router, handle, seq."""
    router = r.bits(4)
    idw, _ = ROUTERS.get(router, (None, None))
    if idw is None:
        raise ValueError(f'unknown router {router}')
    h = read_handle(r, idw)
    return dict(router=router, id=h['id'], a=h['a'], b=h['b'], peer=h.get('peer'), seq=r.bits(8))


# Weather. The server owns the session's WeatherSynced (router 10, object 0; NerdLib 141808990, made by
# NetworkWeather's factory 1409E9930). 11 var slots, so a 4-bit index; the vars (registered 141808FF0) are an
# atmosphere and a precipitation part (SyncedStructs 141823250 / 141823DA0):
#   0 atmosphere CurrentIntensity (float, raw 32 bits)  1 CurrentTick (u32, server ms)
#   2 DesiredIntensity (float)  3 DesiredTick (u32)  4 DesiredPresetIndex (4 x int32)
#   5 precipitation CurrentIntensity  6 CurrentTick  7 DesiredIntensity  8 DesiredTick
# A console applies it (1406F0CB0) as intensity = Current + (Desired - Current) * t, t = 0..1 between the two
# ticks on the server clock (TimeSync), plus the 4 atmosphere presets.
# The server asks the first player for its weather with RequestSendFullWeather (53, no fields); the answer is
# FullWeather (54): atmosphere data (1406F5520 from the weather manager w: f32 target w+0xC, f32 ms to reach it,
# f32 ms (w+0x14), 4 x u32 presets w+0xA8..), precipitation data (f32 target w+0x20, f32 ms to reach it, f32 ms
# (w+0x24)), u32 (int) current atmosphere intensity, u32 (int) current precipitation intensity. Later the server
# asks any player for new data: RequestSendNewAtmosphericData (55) -> NewAtmosphericData (56, the atmosphere
# data), RequestSendNewPrecipitationData (57) -> NewPrecipitationData (58).
WEATHER_ROUTER = 10
WEATHER_INDEX_BITS = 4
REQUEST_FULL_WEATHER, FULL_WEATHER = 53, 54
REQUEST_ATMOSPHERE, ATMOSPHERE = 55, 56
REQUEST_PRECIPITATION, PRECIPITATION = 57, 58


def f32(bits):
    return struct.unpack('<f', struct.pack('<I', bits & 0xFFFFFFFF))[0]


def f32_bits(v):
    return struct.unpack('<I', struct.pack('<f', v))[0]


def read_atmosphere(v):
    """The 224-bit atmosphere data: target, ms to reach it, ms (hold), 4 presets."""
    r = BitReader(v.to_bytes(28, 'little'))
    return dict(target=f32(r.bits(32)), ms=f32(r.bits(32)), hold=f32(r.bits(32)),
                presets=[r.bits(32) for _ in range(4)])


def read_precipitation(v):
    r = BitReader(v.to_bytes(12, 'little'))
    return dict(target=f32(r.bits(32)), ms=f32(r.bits(32)), hold=f32(r.bits(32)))


def weather_vars(w, only=None):
    """The WeatherSynced payload: w = dict(a_cur, a_cur_tick, a_des, a_des_tick, presets, p_cur, p_cur_tick,
    p_des, p_des_tick); only = the var indexes to write (default all)."""
    vals = [('f', w['a_cur']), ('u', w['a_cur_tick']), ('f', w['a_des']), ('u', w['a_des_tick']),
            ('a', w['presets']), ('f', w['p_cur']), ('u', w['p_cur_tick']), ('f', w['p_des']), ('u', w['p_des_tick'])]
    out = BitWriter()
    for i, (kind, v) in enumerate(vals):
        if only is not None and i not in only:
            continue
        out.bits(i, WEATHER_INDEX_BITS)
        if kind == 'f':
            out.bits(f32_bits(v), 32)
        elif kind == 'u':
            out.bits(v & 0xFFFFFFFF, 32)
        else:
            for x in v:
                out.bits(x & 0xFFFFFFFF, 32)
    return out


def concat(*msgs):
    """Messages follow each other in the bit stream with no length or padding between them."""
    w = BitWriter()
    for m in msgs:
        w.bits(m.val, m.pos)
    return w


class Acks:
    """The acks a receiver owes (141766400): ack = newest id seen; bit n-1 of the ack bits = id ack-n seen."""
    def __init__(self):
        self.seen = set()
        self.top = None

    def add(self, pid):
        if pid == NO_SEQ:
            return
        self.seen.add(pid)
        if self.top is None or 0 < (pid - self.top) % 0x10000 < 0x8000:
            self.top = pid

    def header(self):
        if self.top is None:
            return 0xFFFE, 0
        bits = 0
        for n in range(1, 33):
            if (self.top - n) % 0x10000 in self.seen:
                bits |= 1 << (n - 1)
        return self.top, bits
