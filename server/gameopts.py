"""The lobby's GameOptions (the event the leader set up): reading the transfer object's values.

Ported from FH2MP (GPL-3.0, https://github.com/Charelene/FH2MP, native/internal/gameopts and the tag-mode
detection in native/internal/game/events.go). A payload is [variable id: 10 bits][value] pairs in ascending id
order, bits LSB first; an update carries only the values that changed, so the state is all updates merged.
"""

import struct

ID_BITS = 10
MAX_TIME_ID = 420
MAX_TIME_LIMIT = 86400.0

_BUCKET = [-24, 32, 1, 8, 8, 32, 32, 32, 8, 16, 16, 32, 32, 32, 32, 32, 32,
           2, 8, 2, 8, 2, 8, 2, 8, 2, 16, 2, 16, 2, 16, 2, 8, 2, 8, 2, 8,
           2, 8, 2, 8, 2, 8, 1, 32, 800, 800]
_LAP_PREFIX = ([8, 8, 3, 4, 32, 32, 8, 32, 32, 8, 32, 32, 4, 4, 8, 3, 2, 3, 1, 3, 8, 8] + _BUCKET * 8
               + [32, 32, 2, 3, 1, 1, 1, 1, 8, 8, 32, 32, 32, 1, 8, 8, 8, 32, 32, 32, 3, 32])
WIDTHS = _LAP_PREFIX + [
    32, 8, 32, 32, 4, 1, 1, 1, 2, 3, 8, 1, 1, 1, 1, 8, 96, 1, 1,
    32, 32, 8, 8, 8, 8, 8, 1, 1, 1, 1, 1, 2, 2, 2, 3, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 3, 32, 16, 16, 32, 32, 32, 32, 1, 1, 1,
    1, 32, 4, 1, 1, 32, 32, 32, 32, 1, 1, 4, 1, 1, 1, 1, 1, 1]

# RaceModes ids of the tag modes (the game's database)
RACE_MODE_KING, RACE_MODE_INFECTION = 25, 26
_TAG_MIN_BITS = 400
_INFECTION_HEADER = 0x600C2000


def _field(val, pos, width):
    return (val >> pos) & ((1 << width) - 1)


def fields(size, val):
    """{variable id: (width, value)} of one payload; ValueError if it isn't one."""
    if not 0 <= size < 1 << 14:
        raise ValueError('invalid GameOptions size')
    pos, previous, out = 0, -1, {}
    while pos < size:
        if pos + ID_BITS > size:
            raise ValueError('truncated GameOptions id')
        vid = _field(val, pos, ID_BITS)
        pos += ID_BITS
        if vid <= previous or vid >= len(WIDTHS):
            raise ValueError('unknown or unordered GameOptions variable')
        width = WIDTHS[vid]
        if width < 0:                        # a string: 5-bit length, 16-bit characters
            count = _field(val, pos, 5)
            if count > -width or pos + 5 > size:
                raise ValueError('invalid GameOptions string')
            width = 5 + 16 * count
        if pos + width > size:
            raise ValueError('truncated GameOptions value')
        out[vid] = (width, _field(val, pos, width))
        pos += width
        previous = vid
    return out


def encode(values):
    """(size, val) of {variable id: (width, value)}."""
    size = val = 0
    for vid in sorted(values):
        width, v = values[vid]
        val |= (vid | (v << ID_BITS)) << size
        size += ID_BITS + width
    return size, val


def merge(updates):
    """The state that updates [(size, val), ...] make together, as (size, val); None if one can't be read."""
    values = {}
    try:
        for size, val in updates:
            values.update(fields(size, val))
    except ValueError:
        return None
    return encode(values)


def max_time(size, val):
    """The time limit the leader picked (seconds), or None."""
    try:
        f = fields(size, val).get(MAX_TIME_ID)
    except ValueError:
        return None
    if not f or f[0] != 32:
        return None
    sec = struct.unpack('<f', struct.pack('<I', f[1]))[0]
    return sec if 0.0 < sec <= MAX_TIME_LIMIT else None


def tag_mode(size, val):
    """RACE_MODE_INFECTION or RACE_MODE_KING when the options look like a tag event (FH2MP's reading: King can't
    be told from Infection with certainty here, so it is a guess the first tag corrects), else None."""
    if size >= _TAG_MIN_BITS and _field(val, 0, 32) == _INFECTION_HEADER:
        return RACE_MODE_INFECTION
    if size < _TAG_MIN_BITS or _field(val, 0, 8) != 0 or _field(val, 10, 3) != 0b111:
        return None
    return RACE_MODE_KING
