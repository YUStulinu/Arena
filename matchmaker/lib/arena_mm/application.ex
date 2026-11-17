defmodule ArenaMm.Application do
  @moduledoc """
  The matchmaking service. Configuration from the environment:
  ARENA_HTTP_PORT (8080), ARENA_STATIC (the web client's directory),
  ARENA_LADDER (the ladder file), ARENA_PUBLIC_WS / ARENA_PUBLIC_UDP (the game
  server's addresses for clients), ARENA_CONTROL_PORT (7702), ARENA_REPORT_PORT (7703).
  """
  use Application

  @impl true
  def start(_type, _args) do
    env = fn k, d -> System.get_env(k, d) end
    int = fn k, d -> String.to_integer(env.(k, d)) end

    children =
      if Application.get_env(:arena_mm, :start_services, true) do
        [
          {ArenaMm.Ladder, path: env.("ARENA_LADDER", "ladder.bin")},
          {ArenaMm.Matchmaker,
           control: {{127, 0, 0, 1}, int.("ARENA_CONTROL_PORT", "7702")},
           public: %{ws: env.("ARENA_PUBLIC_WS", "ws://127.0.0.1:7701"), udp: env.("ARENA_PUBLIC_UDP", "127.0.0.1:7700")}},
          {ArenaMm.Results, port: int.("ARENA_REPORT_PORT", "7703")},
          {ArenaMm.Http, port: int.("ARENA_HTTP_PORT", "8080"), static: env.("ARENA_STATIC", Path.expand("../web", File.cwd!()))}
        ]
      else
        []
      end

    Supervisor.start_link(children, strategy: :one_for_one, name: ArenaMm.Supervisor)
  end
end
