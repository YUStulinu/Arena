"""The whole system with bots as players: the game server, the matchmaker
(Elixir) and several bots that keep joining the queue and playing online
matches. At the end: the matchmaker's Glicko-2 leaderboard, and the network
statistics of every match (desyncs, rollbacks, bandwidth).

    python eval/ladder.py --minutes 15 random chase bots/models/ppo_010M.pt bots/models/ppo_final.pt
"""

import argparse
import json
import os
import subprocess
import sys
import threading
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "bots"))
EXE = ".exe" if os.name == "nt" else ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bots", nargs="+")
    ap.add_argument("--minutes", type=float, default=15)
    ap.add_argument("--out", default="results/ladder.json")
    a = ap.parse_args()
    from arena_bots.net_client import queue_and_play
    from arena_bots.policies import ChaseBot, NetBot, RandomBot

    env = {**os.environ, "ARENA_LADDER": os.path.join(ROOT, "run", "ladder.bin"), "ARENA_HTTP_PORT": "8080"}
    if os.path.exists(env["ARENA_LADDER"]):
        os.remove(env["ARENA_LADDER"])
    server = subprocess.Popen([os.path.join(ROOT, "server", "target", "release", "arena-server" + EXE), "--script",
                               os.path.join(ROOT, "engine", "scripts", "arena.lua"), "--report", "127.0.0.1:7703"],
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    mm = subprocess.Popen("mix run --no-halt", cwd=os.path.join(ROOT, "matchmaker"), env=env, shell=True,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(120):
        try:
            urllib.request.urlopen("http://127.0.0.1:8080/api/leaderboard", timeout=1)
            break
        except OSError:
            time.sleep(0.5)
    deadline = time.time() + a.minutes * 60
    games = []
    lock = threading.Lock()

    def player(spec, k):
        name = os.path.basename(spec).replace(".pt", "")
        seed = 0
        while time.time() < deadline:
            bot = RandomBot(seed) if spec == "random" else ChaseBot(seed) if spec == "chase" else NetBot.load(spec, seed=seed)
            try:
                r = queue_and_play(bot, name)
                with lock:
                    games.append(r)
            except Exception as e:  # a failed match: try again
                print(name, "error:", e, flush=True)
            seed += 1

    threads = [threading.Thread(target=player, args=(b, k), daemon=True) for k, b in enumerate(a.bots)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=a.minutes * 60 + 400)
    time.sleep(2)
    board = json.load(urllib.request.urlopen("http://127.0.0.1:8080/api/leaderboard"))
    mm.terminate()
    subprocess.run("taskkill /F /T /PID %d" % mm.pid, shell=True, capture_output=True) if os.name == "nt" else None
    server.terminate()
    out, _ = server.communicate(timeout=10)
    results = [json.loads(l) for l in out.splitlines() if l.startswith("{")]
    summary = {
        "minutes": a.minutes,
        "matches": len(results),
        "desyncs": sum(r["desyncs"] for r in results),
        "checksums_compared": sum(r["checksums"] for r in results),
        "forfeits": sum(1 for r in results if r["forfeit"]),
        "leaderboard": board,
        "client_bytes_out_per_s": round(sum(g["bytes_out"] / max(g["seconds"], 1e-9) for g in games) / max(len(games), 1), 1),
        "client_bytes_in_per_s": round(sum(g["bytes_in"] / max(g["seconds"], 1e-9) for g in games) / max(len(games), 1), 1),
    }
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    json.dump({"summary": summary, "server_results": results, "client_reports": games}, open(a.out, "w"), indent=1)
    print(json.dumps(summary, indent=1))


if __name__ == "__main__":
    main()
