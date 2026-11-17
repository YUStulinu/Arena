defmodule ArenaMm.Glicko2 do
  @moduledoc """
  The Glicko-2 rating system (Glickman, "Example of the Glicko-2 system", 2012).

  A rating is `%{rating, rd, vol}` on the Glicko scale (1500 / 350 / 0.06 for a
  new player). `update/2` applies one rating period: the list of games played
  in it, each `{opponent_rating, score}` with score 1, 0.5 or 0. An empty
  period only widens the deviation.
  """

  @tau 0.5
  @scale 173.7178
  @eps 1.0e-6

  def new, do: %{rating: 1500.0, rd: 350.0, vol: 0.06}

  defp g(phi), do: 1 / :math.sqrt(1 + 3 * phi * phi / (:math.pi() * :math.pi()))
  defp e(mu, mu_j, phi_j), do: 1 / (1 + :math.exp(-g(phi_j) * (mu - mu_j)))

  def update(%{rating: r, rd: rd, vol: sigma}, []) do
    phi = rd / @scale
    %{rating: r, rd: min(:math.sqrt(phi * phi + sigma * sigma) * @scale, 350.0), vol: sigma}
  end

  def update(%{rating: r, rd: rd, vol: sigma}, games) do
    mu = (r - 1500) / @scale
    phi = rd / @scale
    opp = for {%{rating: rj, rd: rdj}, s} <- games, do: {(rj - 1500) / @scale, rdj / @scale, s}

    v =
      1 /
        Enum.reduce(opp, 0.0, fn {mu_j, phi_j, _}, acc ->
          ej = e(mu, mu_j, phi_j)
          acc + g(phi_j) * g(phi_j) * ej * (1 - ej)
        end)

    delta = v * Enum.reduce(opp, 0.0, fn {mu_j, phi_j, s}, acc -> acc + g(phi_j) * (s - e(mu, mu_j, phi_j)) end)
    sigma2 = volatility(sigma, phi, v, delta)
    phi_star = :math.sqrt(phi * phi + sigma2 * sigma2)
    phi2 = 1 / :math.sqrt(1 / (phi_star * phi_star) + 1 / v)
    mu2 = mu + phi2 * phi2 * Enum.reduce(opp, 0.0, fn {mu_j, phi_j, s}, acc -> acc + g(phi_j) * (s - e(mu, mu_j, phi_j)) end)
    %{rating: mu2 * @scale + 1500, rd: phi2 * @scale, vol: sigma2}
  end

  # the new volatility: the root of f by the Illinois algorithm (step 5 of the paper)
  defp volatility(sigma, phi, v, delta) do
    a = :math.log(sigma * sigma)

    f = fn x ->
      ex = :math.exp(x)
      ex * (delta * delta - phi * phi - v - ex) / (2 * :math.pow(phi * phi + v + ex, 2)) - (x - a) / (@tau * @tau)
    end

    b =
      if delta * delta > phi * phi + v do
        :math.log(delta * delta - phi * phi - v)
      else
        Stream.iterate(1, &(&1 + 1)) |> Enum.find(fn k -> f.(a - k * @tau) >= 0 end) |> then(&(a - &1 * @tau))
      end

    illinois(f, a, b, f.(a), f.(b)) |> then(&:math.exp(&1 / 2))
  end

  defp illinois(_f, a, b, _fa, _fb) when abs(b - a) <= @eps, do: a

  defp illinois(f, a, b, fa, fb) do
    c = a + (a - b) * fa / (fb - fa)
    fc = f.(c)

    if fc * fb <= 0 do
      illinois(f, b, c, fb, fc)
    else
      illinois(f, a, c, fa / 2, fc)
    end
  end

  @doc "The expected score of `a` against `b` (for matchmaking and reports)."
  def expected(%{rating: ra}, %{rating: rb, rd: rdb}) do
    e((ra - 1500) / @scale, (rb - 1500) / @scale, rdb / @scale)
  end
end
