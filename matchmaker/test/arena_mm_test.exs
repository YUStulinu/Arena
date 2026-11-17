defmodule ArenaMmTest do
  use ExUnit.Case
  alias ArenaMm.{Glicko2, Ladder, Matchmaker}

  test "Glicko-2: the worked example of Glickman's paper" do
    p = %{rating: 1500.0, rd: 200.0, vol: 0.06}
    games = [{%{rating: 1400.0, rd: 30.0}, 1.0}, {%{rating: 1550.0, rd: 100.0}, 0.0}, {%{rating: 1700.0, rd: 300.0}, 0.0}]
    r = Glicko2.update(p, games)
    assert_in_delta r.rating, 1464.06, 0.01
    assert_in_delta r.rd, 151.52, 0.01
    assert_in_delta r.vol, 0.05999, 0.00001
  end

  test "Glicko-2: no games widens the deviation, capped at 350" do
    assert Glicko2.update(%{rating: 1500.0, rd: 50.0, vol: 0.06}, []).rd > 50.0
    assert Glicko2.update(Glicko2.new(), []).rd == 350.0
  end

  test "the ladder records wins, losses and draws" do
    {:ok, l} = Ladder.start_link(name: nil)
    {a, b} = Ladder.record(l, "ann", "bob", 0)
    assert a.rating > 1500 and b.rating < 1500 and a.wins == 1 and b.games == 1
    {a2, _} = Ladder.record(l, "ann", "bob", :draw)
    assert a2.draws == 1 and a2.rating < a.rating
    assert [%{name: "ann"}, %{name: "bob"}] = Ladder.leaderboard(l)
  end

  test "the matchmaker pairs close ratings at once and far ratings only after waiting" do
    {:ok, l} = Ladder.start_link(name: nil)
    for _ <- 1..6, do: Ladder.record(l, "strong", "weak", 0)
    t = :counters.new(1, [])
    clock = fn -> :counters.get(t, 1) end
    me = self()
    create = fn _st, {mid, _} -> send(me, {:created, mid}); :ok end
    {:ok, m} = Matchmaker.start_link(name: nil, ladder: l, auto: false, clock: clock, create: create)
    s = Matchmaker.join(m, "strong")
    w = Matchmaker.join(m, "weak")
    Matchmaker.pair_now(m)
    assert Matchmaker.ticket(m, s).status == "waiting"
    gap = Ladder.get(l, "strong").rating - Ladder.get(l, "weak").rating
    # wait until the window covers the gap
    :counters.put(t, 1, round((gap - 100) / 50 * 1000) + 1000)
    Matchmaker.pair_now(m)
    ts = Matchmaker.ticket(m, s)
    tw = Matchmaker.ticket(m, w)
    assert ts.status == "matched" and tw.status == "matched" and ts.match == tw.match and {ts.player, tw.player} in [{0, 1}, {1, 0}]
    assert_received {:created, _}
    # the server's result updates the ladder
    before = Ladder.get(l, "weak").rating
    Matchmaker.result(m, %{"match" => ts.match, "winner" => tw.player})
    Process.sleep(50)
    assert Ladder.get(l, "weak").rating > before
  end

  test "the same name is not paired with itself" do
    {:ok, l} = Ladder.start_link(name: nil)
    {:ok, m} = Matchmaker.start_link(name: nil, ladder: l, auto: false, create: fn _, _ -> :ok end)
    a = Matchmaker.join(m, "solo")
    Matchmaker.join(m, "solo")
    Matchmaker.pair_now(m)
    assert Matchmaker.ticket(m, a).status == "waiting"
  end

  test "the http routes" do
    assert {"400 Bad Request", _, _} = ArenaMm.Http.route("POST", "/api/queue", "{}", nil)
    assert {"404 Not Found", _, _} = ArenaMm.Http.route("GET", "/../mix.exs", "", Path.expand("test"))
  end
end
