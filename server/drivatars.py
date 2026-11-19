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
