"""The game for the bots, through the engine's C interface (ctypes on the
shared library engine/build/native/libarena.*).

Observations are egocentric: player 1 sees the arena turned by half a turn,
so one policy plays both sides. Actions are 36 = 9 directions (0 none, 1..8)
x dash x pulse, held for `skip` ticks (4: decisions at 15 per second).
Rewards: +1 for a round won, -1 for a round lost.
"""

from __future__ import annotations

import ctypes
import glob
import os

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCRIPT = os.path.join(ROOT, "engine", "scripts", "arena.lua")
OBS = 32
FX = 65536.0
N_ACTIONS = 36
OBS_DIM = 19


def _load():
    cands = glob.glob(os.path.join(ROOT, "engine", "build", "native", "libarena.*")) + glob.glob(os.path.join(ROOT, "engine", "build", "native", "*", "libarena.*"))
    cands = [c for c in cands if c.endswith((".dll", ".so", ".dylib"))]
    if not cands:
        raise RuntimeError("build the engine first (engine/build.sh): no libarena shared library")
    lib = ctypes.CDLL(cands[0])
    lib.arena_game_new.restype = ctypes.c_void_p
    lib.arena_game_new.argtypes = [ctypes.c_char_p, ctypes.c_int32, ctypes.c_char_p, ctypes.c_int32]
    lib.arena_game_free.argtypes = [ctypes.c_void_p]
    lib.arena_state_size.restype = ctypes.c_int32
    lib.arena_initial.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    lib.arena_step.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint8, ctypes.c_uint8]
    lib.arena_step.restype = ctypes.c_int32
    lib.arena_hash.argtypes = [ctypes.c_void_p]
    lib.arena_hash.restype = ctypes.c_uint64
    lib.arena_observe.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    for f, args, res in [
        ("arena_session_new", [ctypes.c_void_p, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32], ctypes.c_void_p),
        ("arena_session_free", [ctypes.c_void_p], None),
        ("arena_session_add_local", [ctypes.c_void_p, ctypes.c_uint8], ctypes.c_int32),
        ("arena_session_add_remote", [ctypes.c_void_p, ctypes.c_int32, ctypes.c_uint8], None),
        ("arena_session_advance", [ctypes.c_void_p], ctypes.c_int32),
        ("arena_session_frame", [ctypes.c_void_p], ctypes.c_int32),
        ("arena_session_remote_known", [ctypes.c_void_p], ctypes.c_int32),
        ("arena_session_local_known", [ctypes.c_void_p], ctypes.c_int32),
        ("arena_session_local_input", [ctypes.c_void_p, ctypes.c_int32], ctypes.c_uint8),
        ("arena_session_state", [ctypes.c_void_p], ctypes.c_void_p),
        ("arena_session_stats", [ctypes.c_void_p, ctypes.c_void_p], None),
        ("arena_session_checksums", [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int32], ctypes.c_int32),
    ]:
        getattr(lib, f).argtypes = args
        getattr(lib, f).restype = res
    return lib


LIB = _load()


def new_game(script: str | None = None):
    src = (script or open(SCRIPT, encoding="utf-8").read()).encode()
    err = ctypes.create_string_buffer(512)
    g = LIB.arena_game_new(src, len(src), err, 512)
    if not g:
        raise RuntimeError(err.value.decode())
    return g


def action_to_input(a: int, me: int) -> int:
    """The input byte for an action, in the arena's frame (player 1's view is turned by half a turn)."""
    d, dash, pulse = a % 9, (a // 9) % 2, a // 18
    if me == 1 and d:
        d = (d - 1 + 4) % 8 + 1
    return d | (dash << 4) | (pulse << 5)


def raw_obs(state) -> np.ndarray:
    o = (ctypes.c_int32 * OBS)()
    LIB.arena_observe(state, o)
    return np.frombuffer(o, dtype=np.int32).copy()


def features(o: np.ndarray, me: int) -> np.ndarray:
    """The egocentric observation of player `me` from the raw observation."""
    sgn = -1.0 if me == 1 else 1.0
    R = o[1] / FX
    def pl(i):
        b = 4 + 11 * i
        return sgn * o[b] / FX, sgn * o[b + 1] / FX, sgn * o[b + 2] / FX, sgn * o[b + 3] / FX, o[b + 6], o[b + 7]
    mx, my, mvx, mvy, mdash, mpulse = pl(me)
    ox, oy, ovx, ovy, odash, opulse = pl(1 - me)
    dx, dy = ox - mx, oy - my
    return np.array([
        mx / 9, my / 9, mvx * 5, mvy * 5, ox / 9, oy / 9, ovx * 5, ovy * 5, dx / 9, dy / 9, np.hypot(dx, dy) / 9, R / 9,
        (R - np.hypot(mx, my)) / 9, (R - np.hypot(ox, oy)) / 9, mdash / 50, mpulse / 90, odash / 50, opulse / 90, float(o[27] > 0),
    ], dtype=np.float32)


class Match:
    """One match; both players act every `skip` ticks."""

    def __init__(self, skip: int = 4, max_seconds: int = 180, game=None):
        self.game = game or new_game()
        self.state = ctypes.create_string_buffer(LIB.arena_state_size())
        self.skip = skip
        self.max_frames = max_seconds * 60
        self.reset()

    def reset(self):
        LIB.arena_initial(self.game, self.state)
        self.o = raw_obs(self.state)
        return [features(self.o, 0), features(self.o, 1)]

    def step(self, a0: int, a1: int):
        """Advance `skip` ticks; returns (observations, rewards, done)."""
        before = (self.o[9], self.o[20])  # scores
        i0, i1 = action_to_input(a0, 0), action_to_input(a1, 1)
        for _ in range(self.skip):
            if LIB.arena_step(self.game, self.state, i0, i1) != 0:
                raise RuntimeError("gameplay script error")
        self.o = raw_obs(self.state)
        d0, d1 = self.o[9] - before[0], self.o[20] - before[1]
        r = [float(d0 - d1), float(d1 - d0)]
        done = self.o[2] == 1 or self.o[0] >= self.max_frames
        return [features(self.o, 0), features(self.o, 1)], r, bool(done)

    @property
    def scores(self):
        return int(self.o[9]), int(self.o[20])

    @property
    def winner(self):
        return int(self.o[3]) if self.o[2] == 1 else -1
