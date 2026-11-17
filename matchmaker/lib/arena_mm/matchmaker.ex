defmodule ArenaMm.Matchmaker do
  @moduledoc """
  The queue. Players join with a name and get a ticket; every half second the
  waiting players are paired, nearest ratings first, when their ratings differ
  by less than a window that widens with the time waited
  (100 + 50 per second, at most 1000). A pair becomes a match on the game
  server (a CREATE datagram on its control port); results come back through
  `result/2` and update the ladder.
  """
  use GenServer
  alias ArenaMm.Ladder

  def start_link(opts), do: GenServer.start_link(__MODULE__, opts, name: Keyword.get(opts, :name, __MODULE__))

  def join(server \\ __MODULE__, name), do: GenServer.call(server, {:join, name})
  def ticket(server \\ __MODULE__, id), do: GenServer.call(server, {:ticket, id})
  def result(server \\ __MODULE__, json), do: GenServer.cast(server, {:result, json})
  def pair_now(server \\ __MODULE__), do: GenServer.call(server, :pair_now)

  @impl true
  def init(opts) do
    st = %{
      queue: [],
      tickets: %{},
      matches: %{},
      ladder: Keyword.get(opts, :ladder, Ladder),
      control: Keyword.get(opts, :control, {{127, 0, 0, 1}, 7702}),
      public: Keyword.get(opts, :public, %{ws: "ws://127.0.0.1:7701", udp: "127.0.0.1:7700"}),
      delay: Keyword.get(opts, :delay, 2),
      clock: Keyword.get(opts, :clock, fn -> System.monotonic_time(:millisecond) end),
      create: Keyword.get(opts, :create, &create_on_server/2)
    }

    if Keyword.get(opts, :auto, true), do: :timer.send_interval(500, :pair)
    {:ok, st}
  end

  @impl true
  def handle_call({:join, name}, _from, st) do
    id = Base.url_encode64(:crypto.strong_rand_bytes(9), padding: false)
    rating = Ladder.get(st.ladder, name).rating
    entry = %{ticket: id, name: name, rating: rating, since: st.clock.()}
    {:reply, id, %{st | queue: st.queue ++ [entry], tickets: Map.put(st.tickets, id, %{status: "waiting", name: name})}}
  end

  def handle_call({:ticket, id}, _from, st), do: {:reply, Map.get(st.tickets, id), st}
  def handle_call(:pair_now, _from, st), do: {:reply, :ok, pair(st)}

  @impl true
  def handle_info(:pair, st), do: {:noreply, pair(st)}

  @impl true
  def handle_cast({:result, %{"match" => mid} = r}, st) do
    case Map.pop(st.matches, mid) do
      {nil, _} ->
        {:noreply, st}

      {{n0, n1}, matches} ->
        winner = if r["winner"] in [0, 1], do: r["winner"], else: :draw
        Ladder.record(st.ladder, n0, n1, winner)
        {:noreply, %{st | matches: matches}}
    end
  end

  def handle_cast({:result, _}, st), do: {:noreply, st}

  @doc false
  def window(waited_ms), do: min(100 + 50 * waited_ms / 1000, 1000)

  # greedy pairing over the queue sorted by rating: a player is paired with the
  # neighbor when both windows allow the difference
  defp pair(st) do
    now = st.clock.()
    {pairs, left} = greedy(Enum.sort_by(st.queue, & &1.rating), now, [], [])
    Enum.reduce(pairs, %{st | queue: left}, fn {a, b}, acc -> start_match(acc, a, b) end)
  end

  defp greedy([a, b | rest], now, pairs, left) do
    diff = abs(a.rating - b.rating)

    if diff <= window(now - a.since) and diff <= window(now - b.since) and a.name != b.name do
      greedy(rest, now, [{a, b} | pairs], left)
    else
      greedy([b | rest], now, pairs, [a | left])
    end
  end

  defp greedy(rest, _now, pairs, left), do: {Enum.reverse(pairs), Enum.reverse(left) ++ rest}

  defp start_match(st, a, b) do
    mid = :rand.uniform(2_000_000_000)
    tokens = {:rand.uniform(2_000_000_000), :rand.uniform(2_000_000_000)}

    case st.create.(st, {mid, tokens}) do
      :ok ->
        info = fn p, tok -> Map.merge(%{status: "matched", match: mid, player: p, token: tok, delay: st.delay}, st.public) end

        tickets =
          st.tickets
          |> Map.put(a.ticket, Map.put(info.(0, elem(tokens, 0)), :name, a.name))
          |> Map.put(b.ticket, Map.put(info.(1, elem(tokens, 1)), :name, b.name))

        %{st | tickets: tickets, matches: Map.put(st.matches, mid, {a.name, b.name})}

      {:error, _} ->
        # the server is not there: back in the queue for the next round
        %{st | queue: st.queue ++ [a, b]}
    end
  end

  defp create_on_server(st, {mid, {t0, t1}}) do
    {:ok, sock} = :gen_udp.open(0, [:binary, active: false])
    {host, port} = st.control
    :gen_udp.send(sock, host, port, "CREATE #{mid} #{t0} #{t1} #{st.delay}")

    reply = :gen_udp.recv(sock, 0, 2000)
    :gen_udp.close(sock)

    case reply do
      {:ok, {_, _, "OK" <> _}} -> :ok
      other -> {:error, other}
    end
  end
end
