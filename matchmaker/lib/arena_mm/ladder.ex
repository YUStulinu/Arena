defmodule ArenaMm.Ladder do
  @moduledoc """
  The ladder: every player's Glicko-2 rating and record. Each finished match
  is one rating period for both players. Saved to a file after every change
  when a path is given.
  """
  use GenServer
  alias ArenaMm.Glicko2

  def start_link(opts), do: GenServer.start_link(__MODULE__, opts, name: Keyword.get(opts, :name, __MODULE__))

  def get(server \\ __MODULE__, name), do: GenServer.call(server, {:get, name})
  def leaderboard(server \\ __MODULE__), do: GenServer.call(server, :leaderboard)

  @doc "Record a match: winner 0 or 1 (or :draw) between two players."
  def record(server \\ __MODULE__, p0, p1, winner), do: GenServer.call(server, {:record, p0, p1, winner})

  @impl true
  def init(opts) do
    path = Keyword.get(opts, :path)

    players =
      with p when is_binary(p) <- path, {:ok, bin} <- File.read(p) do
        :erlang.binary_to_term(bin)
      else
        _ -> %{}
      end

    {:ok, %{path: path, players: players}}
  end

  defp player(players, name), do: Map.get(players, name, Map.merge(Glicko2.new(), %{games: 0, wins: 0, draws: 0}))

  @impl true
  def handle_call({:get, name}, _from, st), do: {:reply, player(st.players, name), st}

  def handle_call(:leaderboard, _from, st) do
    rows =
      st.players
      |> Enum.map(fn {name, p} -> Map.put(p, :name, name) end)
      |> Enum.sort_by(&(&1.rating - 2 * &1.rd), :desc)

    {:reply, rows, st}
  end

  def handle_call({:record, n0, n1, winner}, _from, st) do
    a = player(st.players, n0)
    b = player(st.players, n1)
    {sa, sb} =
      case winner do
        0 -> {1.0, 0.0}
        1 -> {0.0, 1.0}
        _ -> {0.5, 0.5}
      end

    a2 = Map.merge(a, Glicko2.update(a, [{b, sa}]))
    b2 = Map.merge(b, Glicko2.update(b, [{a, sb}]))
    a2 = %{a2 | games: a.games + 1, wins: a.wins + trunc(sa), draws: a.draws + if(sa == 0.5, do: 1, else: 0)}
    b2 = %{b2 | games: b.games + 1, wins: b.wins + trunc(sb), draws: b.draws + if(sb == 0.5, do: 1, else: 0)}
    players = st.players |> Map.put(n0, a2) |> Map.put(n1, b2)
    if st.path, do: File.write!(st.path, :erlang.term_to_binary(players))
    {:reply, {a2, b2}, %{st | players: players}}
  end
end
