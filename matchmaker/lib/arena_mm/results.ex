defmodule ArenaMm.Results do
  @moduledoc "Receives the game server's match results (JSON datagrams) and hands them to the matchmaker."
  use GenServer

  def start_link(opts), do: GenServer.start_link(__MODULE__, opts, name: __MODULE__)

  @impl true
  def init(opts) do
    {:ok, sock} = :gen_udp.open(Keyword.get(opts, :port, 7703), [:binary, active: true, ip: {127, 0, 0, 1}])
    {:ok, %{sock: sock, mm: Keyword.get(opts, :matchmaker, ArenaMm.Matchmaker)}}
  end

  @impl true
  def handle_info({:udp, _s, _ip, _port, data}, st) do
    case JSON.decode(data) do
      {:ok, %{} = r} -> ArenaMm.Matchmaker.result(st.mm, r)
      _ -> :ignored
    end

    {:noreply, st}
  end
end
