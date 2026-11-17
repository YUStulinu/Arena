"""Bot strength: a round-robin tournament (both seats, matches in parallel
processes), the score matrix, and Elo ratings fitted by maximum likelihood
(Bradley-Terry; a draw counts half), anchored at random = 1000.

    python bots/evaluate.py --matches 40 bots/models/ppo_002M.pt bots/models/ppo_final.pt ...
"""

from __future__ import annotations

import argparse
import itertools
import json
import multiprocessing as mp
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

_cache: dict = {}


def make_bot(name: str, seed: int):
    from arena_bots.policies import ChaseBot, NetBot, RandomBot

    if name == "random":
        return RandomBot(seed)
    if name == "chase":
        return ChaseBot(seed)
    if name not in _cache:
        _cache[name] = NetBot.load(name).net
    from arena_bots.policies import NetBot as NB

    return NB(_cache[name], greedy=False, seed=seed)


def play(job):
    from arena_bots.env import Match

    a, b, seed = job
    bots = [make_bot(a, seed), make_bot(b, seed + 1)]
    m = Match(max_seconds=240)
    obs = m.reset()
    done = False
    while not done:
        obs, _, done = m.step(bots[0].act(obs[0]), bots[1].act(obs[1]))
    s0, s1 = m.scores
    return a, b, m.winner, s0, s1


def elo(names, results):
    """Bradley-Terry by MM iterations (Hunter 2004), on the Elo scale with names[0] at 1000."""
    k = len(names)
    w = np.zeros((k, k))  # w[i, j]: points of i against j
    for a, b, winner, *_ in results:
        i, j = names.index(a), names.index(b)
        if winner == 0:
            w[i, j] += 1
        elif winner == 1:
            w[j, i] += 1
        else:
            w[i, j] += 0.5
            w[j, i] += 0.5
    n = w + w.T
    p = np.ones(k)
    for _ in range(2000):
        wins = w.sum(1) + 0.01  # a tiny prior keeps undefeated or winless players finite
        denom = np.array([sum(n[i, j] / (p[i] + p[j]) for j in range(k) if j != i) for i in range(k)]) + 0.02 / (p + 1)
        p = wins / denom
        p /= np.exp(np.mean(np.log(p)))
    r = 400 * np.log10(p)
    return r - r[0] + 1000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("models", nargs="*")
    ap.add_argument("--matches", type=int, default=40, help="per pair (half in each seat)")
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--out", default="results/bots.json")
    a = ap.parse_args()
    names = ["random", "chase"] + a.models
    jobs = []
    for x, y in itertools.combinations(names, 2):
        for k in range(a.matches):
            jobs.append((x, y, 1000 * k + 7) if k % 2 == 0 else (y, x, 1000 * k + 7))
    with mp.get_context("spawn").Pool(a.workers) as pool:
        results = pool.map(play, jobs, chunksize=4)
    ratings = elo(names, results)
    short = [os.path.basename(n).replace(".pt", "") for n in names]
    table = {}
    for x, y in itertools.permutations(names, 2):
        pts = games = 0.0
        for a_, b_, wnr, *_ in results:
            if {a_, b_} != {x, y}:
                continue
            games += 1
            me = 0 if a_ == x else 1
            pts += 1.0 if wnr == me else (0.5 if wnr == -1 else 0.0)
        table.setdefault(os.path.basename(x).replace(".pt", ""), {})[os.path.basename(y).replace(".pt", "")] = round(pts / games, 3)
    draws = sum(1 for r in results if r[2] == -1)
    out = {"matches_per_pair": a.matches, "total_matches": len(results), "draws": draws,
           "elo": {s: round(float(r), 1) for s, r in zip(short, ratings)}, "score_matrix": table}
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    json.dump(out, open(a.out, "w"), indent=1)
    print(json.dumps(out["elo"], indent=1))


if __name__ == "__main__":
    main()
