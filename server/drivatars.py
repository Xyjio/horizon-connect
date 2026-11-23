"""Drivatars in online races (step 1: the network side).

The relay adds the game's own single-player Drivatars (their names from media\\drivatarglobalcache\\DefaultCache.bin,
the file fh2srv already serves for solo races) to an online event as players without a console of their own. Each
one is a copy of the session leader's player objects - PlayerData (router 1), its car (router 2) and ConsoleSynced
(router 3) - under a peer index of its own, with the Drivatar's name and a player id of its own. The consoles see it
as another player: in the participant list, on the starting grid, in the race.

For now a Drivatar's car is a copy of the leader's car and follows it a few metres away (as FH2MP's test bots do:
GPL-3.0, https://github.com/Charelene/FH2MP, native/internal/game/bots.go). Their own cars and the game's AI
driving them come in later steps.

Testing only: server\\test-drivatars.txt holds how many Drivatars join each race (0-10; read when an event starts).
"""

import math
import os
import struct

BOT_XUID_BASE = 0x9F00000000000          # player ids of Drivatars (FH2MP's bot range; consoles have 0x9000...)
POSITION_BITS = 96                       # a CarPredictionFull body starts with the car's position (3 x f32)
TEST_FILE = 'test-drivatars.txt'
MAX_DRIVATARS = 10


def wanted(server_dir):
    """How many Drivatars to add to the next race (the test file; 0 without it)."""
    try:
        with open(os.path.join(server_dir, TEST_FILE), encoding='utf-8-sig') as f:
            n = int(f.read().split()[0])
    except (OSError, ValueError, IndexError):
        return 0
    return max(0, min(MAX_DRIVATARS, n))


def offset(k):
    """Where Drivatar k drives relative to the car it follows (x, z in metres): rows of two, side by side."""
    side = -1 if k % 2 == 0 else 1
    return side * 4.0, -6.0 * (k // 2 + 1)


def _f32(v):
    return struct.unpack('<f', struct.pack('<I', v & 0xFFFFFFFF))[0]


def _f32_bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def shifted(body, dx, dz):
    """A copy of a CarPredictionFull body (a nerd.BitWriter) with the position moved by (dx, 0, dz)."""
    import nerd
    x, y, z = (_f32(body.val >> (32 * i)) for i in range(3))
    if not all(math.isfinite(v) and abs(v) < 1e6 for v in (x, y, z)):
        return body
    pos = _f32_bits(x + dx) | _f32_bits(y) << 32 | _f32_bits(z + dz) << 64
    out = nerd.BitWriter()
    out.val = (body.val & ~((1 << POSITION_BITS) - 1)) | pos
    out.pos = body.pos
    return out


def replace_xuid(payload, old, new):
    """A copy of an object payload (nerd.BitWriter) with every 64-bit run equal to `old` set to `new` (the player
    id sits in PlayerData and may sit in other objects of the player; FH2MP replaceUserID)."""
    import nerd
    val, n = payload.val, payload.pos
    if old:
        mask = (1 << 64) - 1
        off = 0
        while off + 64 <= n:
            if (val >> off) & mask == old:
                val = (val & ~(mask << off)) | (new << off)
                off += 64
            else:
                off += 1
    out = nerd.BitWriter()
    out.val, out.pos = val, n
    return out


def rename_player(payload, name, xuid, index_bits=8):
    """A PlayerData create payload (it starts with NetworkName, then XUID) with another name and player id; None
    when it doesn't start that way."""
    import nerd
    r = nerd.BitReader(bytes(payload.buf))
    try:
        if r.bits(index_bits) != nerd.PLAYER_NETWORK_NAME:
            return None
        r.bits(16 * r.bits(5))
        if r.bits(index_bits) != nerd.PLAYER_XUID:
            return None
        r.bits(64)
    except IndexError:
        return None
    rest_bits = payload.pos - r.pos
    rest = payload.val >> r.pos
    name = name[:31]
    w = nerd.BitWriter().bits(nerd.PLAYER_NETWORK_NAME, index_bits).bits(len(name), 5)
    for c in name:
        w.bits(ord(c) if ord(c) < 0x10000 else 0x3F, 16)
    w.bits(nerd.PLAYER_XUID, index_bits).bits(xuid, 64)
    w.bits(rest, rest_bits)
    return w


# PlayerData (LobbyPlayer) vars: [8-bit id][value] pairs in ascending order; the widths (negative: a string of up to
# that many UTF-16 chars after a length prefix). From FH2MP (GPL-3.0, native/internal/playersetup/playersetup.go).
PLAYER_WIDTHS = [
    64, 1, 1, -16, 64, 1, 32, 1, 32, 32, 32, 32, 1, 1, 1, 1, 1, 1,
    1, 4, 3, 16, 16, 32, 32, 32, 32, 32, 32, 1, 7, 8, 1, 1, 1, 1,
    1, 32, 2, -4, 5, 64, 1, 1, 64, 1, 1, 64, 1, 1, 64, 1, 1, 64,
    1, 1, 64, 1, 1, 64, 1, 1, 64, 1, 1, 64, 1, 1, 64, 1, 1, 64,
    1, 1, 64, 1, 1, 3, 17, 16, 16, 32, 32, 32, 32, 32, 32, 32, 32, 1,
    1, 3, 32, 64, 64, 2, 2, 2, 1, 1, 2, 1, 3, 32, 32, 32, 32, 1376,
    32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 288, 32, 32, 32, 32,
    32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32,
    1, 32, 32, 32, 32, 1, 1, 1, 1, 1, 2, 2, 2, 2, 32, 32, 32, 32,
]
PLAYER_MODEL, PLAYER_USE_STOCK = 103, 144     # the car the others draw (FH2MP: from the model, not the part list)
# (The car object, router 2, is 37 bits: a 5-bit var id 0 and 32 bits that were a float (80.0) in every capture of
# 2026-11-23, not the model id FH2MP's carModelID reads there; it is left as it is.)


def player_fields(payload):
    """{var: (offset, width)} of a PlayerData payload; None when it doesn't parse."""
    val, n, pos, prev, out = payload.val, payload.pos, 0, -1, {}
    while pos < n:
        if n - pos < 8:
            return None
        vid = (val >> pos) & 0xFF
        pos += 8
        if vid <= prev or vid >= len(PLAYER_WIDTHS):
            return None
        w = PLAYER_WIDTHS[vid]
        if w < 0:
            prefix = (-w).bit_length()
            count = (val >> pos) & ((1 << prefix) - 1)
            if count > -w:
                return None
            w = prefix + 16 * count
        if pos + w > n:
            return None
        out[vid] = (pos, w)
        prev, pos = vid, pos + w
    return out


def _set_bits(payload, offset, width, value):
    import nerd
    out = nerd.BitWriter()
    mask = ((1 << width) - 1) << offset
    out.val = (payload.val & ~mask) | ((value & ((1 << width) - 1)) << offset)
    out.pos = payload.pos
    return out


def player_car(payload, car):
    """A PlayerData payload with the car model set to `car` and stock parts asked for, where it has those vars."""
    f = player_fields(payload)
    if not f:
        return payload
    if PLAYER_MODEL in f:
        payload = _set_bits(payload, f[PLAYER_MODEL][0], 32, car)
    if PLAYER_USE_STOCK in f:
        payload = _set_bits(payload, f[PLAYER_USE_STOCK][0], 1, 1)
    return payload

