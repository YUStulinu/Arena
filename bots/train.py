"""Self-play PPO for Arena.

In each of the parallel matches the learner holds one seat (chosen at random
per match); the other seat is the learner itself (40%), a past checkpoint
(40%), or a scripted bot (chase 10%, random 10%). Rewards: +1 / -1 per round,
plus the potential-based shaping 0.2 (|opponent| - |me|) / R toward pushing
the opponent to the edge (it does not change the optimal policy).

    python bots/train.py --steps 20000000 --out bots/models
"""

from __future__ import annotations

import argparse
import copy
import json
import os
import sys
import time

import numpy as np
import torch
import torch.nn.functional as F

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from arena_bots.env import OBS_DIM  # noqa: E402
from arena_bots.policies import ChaseBot, Net, RandomBot  # noqa: E402
from arena_bots.vec import VecMatches  # noqa: E402


def potential(o: np.ndarray) -> np.ndarray:
    """o: [..., OBS_DIM] egocentric features -> 0.2 (|opp| - |me|) / R"""
    me = np.hypot(o[..., 0], o[..., 1])
    op = np.hypot(o[..., 4], o[..., 5])
    return 0.2 * (op - me) / np.maximum(o[..., 11], 1e-3)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--steps", type=int, default=20_000_000, help="learner decisions")
    ap.add_argument("--envs", type=int, default=128)
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--horizon", type=int, default=128)
    ap.add_argument("--lr", type=float, default=3e-4)
    ap.add_argument("--gamma", type=float, default=0.995)
    ap.add_argument("--lam", type=float, default=0.95)
    ap.add_argument("--snapshot-every", type=int, default=1_000_000)
    ap.add_argument("--out", default="bots/models")
    ap.add_argument("--seed", type=int, default=0)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    torch.manual_seed(a.seed)
    rng = np.random.default_rng(a.seed)
    dev = "cuda" if torch.cuda.is_available() else "cpu"
    net = Net().to(dev)
    opt = torch.optim.Adam(net.parameters(), lr=a.lr, eps=1e-5)
    pool: list[Net] = []  # frozen snapshots
    E, H = a.envs, a.horizon
    vec = VecMatches(E, a.workers)
    seat = rng.integers(2, size=E)
    scripted = [ChaseBot(k) for k in range(E)] + [RandomBot(k) for k in range(E)]

    def draw_opponent():
        u = rng.random()
        if u < 0.4 or not pool:
            return -1  # self
        if u < 0.8:
            return int(rng.integers(len(pool)))
        return -2 if u < 0.9 else -3  # chase, random

    opp = np.array([draw_opponent() for _ in range(E)])
    log, ep_ret, ep_win = [], [], []
    cur = np.zeros(E)
    steps, next_snap = 0, a.snapshot_every
    t0 = time.time()

    @torch.no_grad()
    def policy(model, x):
        logits, v = model(torch.as_tensor(x, device=next(model.parameters()).device))
        d = torch.distributions.Categorical(logits=logits)
        act = d.sample()
        return act.cpu().numpy(), d.log_prob(act).cpu().numpy(), v.cpu().numpy()

    while steps < a.steps:
        obs_b = np.zeros((H, E, OBS_DIM), np.float32)
        act_b = np.zeros((H, E), np.int64)
        logp_b = np.zeros((H, E), np.float32)
        val_b = np.zeros((H, E), np.float32)
        rew_b = np.zeros((H, E), np.float32)
        done_b = np.zeros((H, E), np.float32)
        for t in range(H):
            idx = np.arange(E)
            me_obs = vec.obs[idx, seat]
            op_obs = vec.obs[idx, 1 - seat]
            selfplay = opp == -1
            x = np.concatenate([me_obs, op_obs[selfplay]])
            acts, logp, v = policy(net, x)
            a_me = acts[:E]
            a_op = np.zeros(E, np.int64)
            a_op[selfplay] = acts[E:]
            for k in np.unique(opp[opp >= 0]):
                sel = opp == k
                a_op[sel], _, _ = policy(pool[k], op_obs[sel])
            for e in np.where(opp < -1)[0]:
                bot = scripted[e] if opp[e] == -2 else scripted[E + e]
                a_op[e] = bot.act(op_obs[e])
            pairs = np.where(seat[:, None] == 0, np.stack([a_me, a_op], 1), np.stack([a_op, a_me], 1))
            phi0 = potential(me_obs)
            obs, r, done, win = vec.step(pairs)
            new_me = obs[idx, seat]
            phi1 = np.where(done, 0.0, potential(new_me))
            rr = r[idx, seat]
            obs_b[t], act_b[t], logp_b[t], val_b[t] = me_obs, a_me, logp[:E], v[:E]
            rew_b[t] = rr + a.gamma * phi1 - phi0
            done_b[t] = done
            cur += rr
            for e in np.where(done)[0]:
                ep_ret.append(cur[e])
                ep_win.append(1.0 if win[e] == seat[e] else 0.0)
                cur[e] = 0
                seat[e] = rng.integers(2)
                opp[e] = draw_opponent()
        steps += H * E
        # GAE
        with torch.no_grad():
            _, last_v = net(torch.as_tensor(vec.obs[np.arange(E), seat], device=dev))
        last_v = last_v.cpu().numpy()
        adv = np.zeros((H, E), np.float32)
        gae = np.zeros(E, np.float32)
        for t in reversed(range(H)):
            nv = last_v if t == H - 1 else val_b[t + 1]
            nonterm = 1.0 - done_b[t]
            gae = rew_b[t] + a.gamma * nv * nonterm - val_b[t] + a.gamma * a.lam * nonterm * gae
            adv[t] = gae
        ret = adv + val_b
        O = torch.as_tensor(obs_b.reshape(-1, OBS_DIM), device=dev)
        A = torch.as_tensor(act_b.reshape(-1), device=dev)
        LP = torch.as_tensor(logp_b.reshape(-1), device=dev)
        ADV = torch.as_tensor(adv.reshape(-1), device=dev)
        ADV = (ADV - ADV.mean()) / (ADV.std() + 1e-8)
        RET = torch.as_tensor(ret.reshape(-1), device=dev)
        for g in opt.param_groups:
            g["lr"] = a.lr * max(1 - steps / a.steps, 0.05)
        n = O.shape[0]
        for _ in range(4):
            perm = torch.randperm(n, device=dev)
            for i in range(0, n, 4096):
                j = perm[i : i + 4096]
                logits, v = net(O[j])
                d = torch.distributions.Categorical(logits=logits)
                ratio = torch.exp(d.log_prob(A[j]) - LP[j])
                pg = -torch.min(ratio * ADV[j], torch.clamp(ratio, 0.8, 1.2) * ADV[j]).mean()
                vl = F.mse_loss(v, RET[j])
                ent = d.entropy().mean()
                loss = pg + 0.5 * vl - 0.01 * ent
                opt.zero_grad()
                loss.backward()
                torch.nn.utils.clip_grad_norm_(net.parameters(), 0.5)
                opt.step()
        if steps >= next_snap:
            pool.append(copy.deepcopy(net).eval())
            torch.save(net.state_dict(), os.path.join(a.out, f"ppo_{steps // 1_000_000:03d}M.pt"))
            next_snap += a.snapshot_every
        if len(ep_ret) >= 200 or steps >= a.steps:
            rec = {"steps": steps, "minutes": round((time.time() - t0) / 60, 2), "decisions_per_s": round(steps / (time.time() - t0)),
                   "mean_round_diff": float(np.mean(ep_ret)), "win_rate_mixed_opponents": float(np.mean(ep_win)),
                   "entropy": round(float(ent), 3), "value_loss": round(float(vl), 4)}
            log.append(rec)
            print(json.dumps(rec), flush=True)
            ep_ret, ep_win = [], []
    torch.save(net.state_dict(), os.path.join(a.out, "ppo_final.pt"))
    json.dump(log, open(os.path.join(a.out, "training_log.json"), "w"), indent=1)
    vec.close()


if __name__ == "__main__":
    main()
