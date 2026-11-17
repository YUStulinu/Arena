import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from arena_bots.env import Match, action_to_input  # noqa: E402
from arena_bots.policies import ChaseBot, Net, NetBot, RandomBot, direction_to  # noqa: E402
from evaluate import elo  # noqa: E402


def test_player_one_sees_the_arena_turned_half_a_turn():
    assert action_to_input(1, 0) == 1 and action_to_input(1, 1) == 5  # east becomes west
    assert action_to_input(8, 1) == 4  # south-east -> north-west
    assert action_to_input(0, 1) == 0
    assert action_to_input(9 + 18 + 3, 0) == 3 | 16 | 32


def test_the_start_looks_the_same_from_both_seats():
    m = Match()
    o0, o1 = m.reset()
    assert np.allclose(o0, o1)


def test_a_match_between_scripted_bots_ends():
    m = Match(max_seconds=240)
    obs = m.reset()
    bots = [ChaseBot(1), RandomBot(2)]
    done = False
    while not done:
        obs, r, done = m.step(bots[0].act(obs[0]), bots[1].act(obs[1]))
        assert r[0] == -r[1]
    assert m.winner in (0, 1, -1)
    assert max(m.scores) <= 3


def test_directions():
    assert direction_to(1, 0) == 1 and direction_to(0, 1) == 3 and direction_to(-1, -1) == 6 and direction_to(0, 0) == 0


def test_net_bot_acts():
    bot = NetBot(Net(), greedy=True)
    a = bot.act(np.zeros(19, np.float32))
    assert 0 <= a < 36


def test_bradley_terry_elo():
    # A scores 75% against B: 400 log10(3) ~ 191 points
    res = [("A", "B", 0, 0, 0)] * 75 + [("A", "B", 1, 0, 0)] * 25
    r = elo(["A", "B"], res)
    assert abs((r[0] - r[1]) - 190.8) < 3
