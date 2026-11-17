defmodule ArenaMm.Http do
  @moduledoc """
  A small HTTP/1.1 server on :gen_tcp (one process per connection):

    POST /api/queue        {"name": "..."}  ->  {"ticket": "..."}
    GET  /api/ticket/ID    ->  {"status": "waiting"} | {"status": "matched", "match", "player", "token", "ws", ...}
    GET  /api/leaderboard  ->  [{"name", "rating", "rd", "games", "wins", "draws"}, ...]
    GET  /...              the browser client's files (from the static directory)
  """
  require Logger
  alias ArenaMm.{Ladder, Matchmaker}

  def child_spec(opts), do: %{id: __MODULE__, start: {Task, :start_link, [fn -> listen(opts) end]}}

  def listen(opts) do
    port = Keyword.get(opts, :port, 8080)
    static = Keyword.get(opts, :static)
    {:ok, ls} = :gen_tcp.listen(port, [:binary, packet: :raw, active: false, reuseaddr: true])
    Logger.info("matchmaker http on port #{port}")
    accept(ls, static)
  end

  defp accept(ls, static) do
    {:ok, s} = :gen_tcp.accept(ls)
    pid = spawn(fn -> serve(s, static) end)
    :gen_tcp.controlling_process(s, pid)
    accept(ls, static)
  end

  defp serve(s, static) do
    with {:ok, method, path, body} <- read_request(s, "") do
      {status, type, out} = route(method, path, body, static)

      :gen_tcp.send(s, [
        "HTTP/1.1 #{status}\r\ncontent-type: #{type}\r\ncontent-length: #{byte_size(out)}\r\n",
        "access-control-allow-origin: *\r\nconnection: close\r\n\r\n",
        out
      ])
    end

    :gen_tcp.close(s)
  end

  defp read_request(s, acc) do
    case :binary.split(acc, "\r\n\r\n") do
      [head, rest] ->
        [line | headers] = String.split(head, "\r\n")
        [method, path | _] = String.split(line, " ")

        len =
          Enum.find_value(headers, 0, fn h ->
            case String.split(h, ":", parts: 2) do
              [k, v] -> if String.downcase(k) == "content-length", do: String.to_integer(String.trim(v))
              _ -> nil
            end
          end)

        read_body(s, method, path, rest, len)

      [_] when byte_size(acc) > 16_384 ->
        {:error, :too_large}

      [_] ->
        case :gen_tcp.recv(s, 0, 5000) do
          {:ok, data} -> read_request(s, acc <> data)
          e -> e
        end
    end
  end

  defp read_body(_s, method, path, body, len) when byte_size(body) >= len, do: {:ok, method, path, binary_part(body, 0, len)}

  defp read_body(s, method, path, body, len) when len < 65_536 do
    case :gen_tcp.recv(s, 0, 5000) do
      {:ok, data} -> read_body(s, method, path, body <> data, len)
      e -> e
    end
  end

  defp read_body(_, _, _, _, _), do: {:error, :too_large}

  defp json(status, v), do: {status, "application/json", JSON.encode!(v)}

  @doc false
  def route("POST", "/api/queue", body, _) do
    case JSON.decode(body) do
      {:ok, %{"name" => name}} when is_binary(name) and byte_size(name) in 1..24 ->
        json("200 OK", %{ticket: Matchmaker.join(String.trim(name))})

      _ ->
        json("400 Bad Request", %{error: "a name of 1 to 24 characters"})
    end
  end

  def route("GET", "/api/ticket/" <> id, _, _) do
    case Matchmaker.ticket(id) do
      nil -> json("404 Not Found", %{error: "unknown ticket"})
      t -> json("200 OK", t)
    end
  end

  def route("GET", "/api/leaderboard", _, _) do
    rows =
      for p <- Ladder.leaderboard() do
        %{name: p.name, rating: Float.round(p.rating, 1), rd: Float.round(p.rd, 1), games: p.games, wins: p.wins, draws: p.draws}
      end

    json("200 OK", rows)
  end

  def route("GET", path, _, static) when is_binary(static) do
    rel = path |> String.split("?") |> hd() |> String.trim_leading("/")
    rel = if rel == "", do: "index.html", else: rel
    file = Path.expand(rel, static)

    # no paths outside the static directory
    if String.starts_with?(file, Path.expand(static)) and File.regular?(file) do
      {"200 OK", mime(file), File.read!(file)}
    else
      {"404 Not Found", "text/plain", "not found"}
    end
  end

  def route(_, _, _, _), do: {"404 Not Found", "text/plain", "not found"}

  defp mime(f) do
    case Path.extname(f) do
      ".html" -> "text/html; charset=utf-8"
      ".js" -> "text/javascript"
      ".wasm" -> "application/wasm"
      ".lua" -> "text/plain; charset=utf-8"
      ".css" -> "text/css"
      ".json" -> "application/json"
      _ -> "application/octet-stream"
    end
  end
end
