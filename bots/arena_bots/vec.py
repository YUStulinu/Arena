"""Matches stepped in worker processes (the engine is single-threaded per
match; Python's loop over matches is the bottleneck otherwise)."""

from __future__ import annotations

import multiprocessing as mp

import numpy as np


def _worker(conn, n, skip):
    from .env import Match

    ms = [Match(skip=skip) for _ in range(n)]
    obs = [m.reset() for m in ms]
    conn.send(np.array(obs, np.float32))
    while True:
        cmd = conn.recv()
        if cmd is None:
            break
        out_obs, rew, done, win = [], [], [], []
        for m, (a0, a1) in zip(ms, cmd):
            o, r, d = m.step(int(a0), int(a1))
            w = m.winner if d else -2
            if d:
                o = m.reset()
            out_obs.append(o)
            rew.append(r)
            done.append(d)
            win.append(w)
        conn.send((np.array(out_obs, np.float32), np.array(rew, np.float32), np.array(done), np.array(win)))


class VecMatches:
    """E matches split over W processes. step(actions[E, 2]) -> obs[E, 2, D], rewards[E, 2], done[E], winner[E]
    (winner -2 while playing; finished matches restart and return their first observation)."""

    def __init__(self, envs: int, workers: int = 8, skip: int = 4):
        ctx = mp.get_context("spawn")
        self.split = np.array_split(np.arange(envs), workers)
        self.conns, self.procs = [], []
        for part in self.split:
            a, b = ctx.Pipe()
            p = ctx.Process(target=_worker, args=(b, len(part), skip), daemon=True)
            p.start()
            self.conns.append(a)
            self.procs.append(p)
        self.obs = np.concatenate([c.recv() for c in self.conns])

    def step(self, actions: np.ndarray):
        for c, part in zip(self.conns, self.split):
            c.send(actions[part].tolist())
        res = [c.recv() for c in self.conns]
        self.obs = np.concatenate([r[0] for r in res])
        return self.obs, np.concatenate([r[1] for r in res]), np.concatenate([r[2] for r in res]), np.concatenate([r[3] for r in res])

    def close(self):
        for c in self.conns:
            c.send(None)
        for p in self.procs:
            p.join(timeout=5)
