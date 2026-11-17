"""The bots: scripted ones (from the egocentric observation) and the neural
policy trained by self-play."""

from __future__ import annotations

import math

import numpy as np
import torch
import torch.nn as nn

from .env import N_ACTIONS, OBS_DIM


def direction_to(dx: float, dy: float) -> int:
    """The nearest of the 8 directions (1 = east, counter-clockwise)."""
    if dx == 0 and dy == 0:
        return 0
    a = math.atan2(dy, dx) % (2 * math.pi)
    return int(round(a / (math.pi / 4))) % 8 + 1


def action(direction: int, dash: bool = False, pulse: bool = False) -> int:
    return direction + 9 * int(dash) + 18 * int(pulse)


class RandomBot:
    """A random action held for a random time."""

    def __init__(self, seed=0):
        self.rng = np.random.default_rng(seed)
        self.a = 0
        self.hold = 0

    def act(self, obs: np.ndarray) -> int:
        if self.hold <= 0:
            self.a = int(self.rng.integers(N_ACTIONS))
            self.hold = int(self.rng.integers(1, 6))
        self.hold -= 1
        return self.a


class ChaseBot:
    """Steer at the opponent; dash and pulse when close."""

    def __init__(self, seed=0):
        self.rng = np.random.default_rng(seed)

    def act(self, obs: np.ndarray) -> int:
        dx, dy, dist = obs[8], obs[9], obs[10] * 9
        d = direction_to(dx, dy)
        dash = dist < 3.0 and obs[14] == 0 and self.rng.random() < 0.3
        pulse = dist < 1.8 and obs[15] == 0
        return action(d, dash, pulse)


class Net(nn.Module):
    def __init__(self, hidden=128):
        super().__init__()
        self.body = nn.Sequential(nn.Linear(OBS_DIM, hidden), nn.Tanh(), nn.Linear(hidden, hidden), nn.Tanh())
        self.pi = nn.Linear(hidden, N_ACTIONS)
        self.v = nn.Linear(hidden, 1)

    def forward(self, x):
        h = self.body(x)
        return self.pi(h), self.v(h).squeeze(-1)


class NetBot:
    """A trained policy; greedy or sampling."""

    def __init__(self, net: Net, greedy=False, seed=0):
        self.net = net.eval()
        self.greedy = greedy
        self.gen = torch.Generator().manual_seed(seed)

    @torch.no_grad()
    def act(self, obs: np.ndarray) -> int:
        logits, _ = self.net(torch.as_tensor(obs[None]))
        if self.greedy:
            return int(logits.argmax())
        return int(torch.multinomial(torch.softmax(logits, -1), 1, generator=self.gen))

    @staticmethod
    def load(path: str, **kw) -> "NetBot":
        net = Net()
        net.load_state_dict(torch.load(path, map_location="cpu"))
        return NetBot(net, **kw)
