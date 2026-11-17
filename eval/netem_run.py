"""One netem scenario (run inside the server's network namespace by
eval/netem.sh): start the server, play matches between a chase bot and a
random bot whose clients run in their own namespaces, collect the reports."""

import argparse
import json
import os
import random
import socket
import subprocess
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def control(cmd):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(2)
    for _ in range(20):
        try:
            s.sendto(cmd.encode(), ("127.0.0.1", 7702))
            return s.recv(512).decode()
        except OSError:
            time.sleep(0.3)
    raise RuntimeError("no answer on the control port")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenario")
    ap.add_argument("--matches", type=int, default=4)
    ap.add_argument("--out")
    ap.add_argument("--server-bin")
    ap.add_argument("--client-bin")
    ap.add_argument("--ns0")
    ap.add_argument("--ns1")
    ap.add_argument("--addr0")
    ap.add_argument("--addr1")
    ap.add_argument("--delay", type=int, default=2)
    a = ap.parse_args()
    script = os.path.join(ROOT, "engine", "scripts", "arena.lua")
    server = subprocess.Popen([a.server_bin, "--script", script], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    time.sleep(0.5)
    matches = []
    try:
        for k in range(a.matches):
            mid = random.randint(1, 2**31)
            tok = [random.randint(1, 2**31) for _ in range(2)]
            assert control(f"CREATE {mid} {tok[0]} {tok[1]} {a.delay}").startswith("OK")
            bots = ["chase", "random"] if k % 2 == 0 else ["random", "chase"]
            procs = []
            for p, (ns, addr) in enumerate([(a.ns0, a.addr0), (a.ns1, a.addr1)]):
                cmd = ["ip", "netns", "exec", ns, a.client_bin, "--server", f"{addr}:7700", "--match", str(mid), "--player", str(p),
                       "--token", str(tok[p]), "--bot", bots[p], "--seed", str(100 * k + p), "--script", script, "--max-seconds", "240"]
                procs.append(subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True))
            clients = []
            for pr in procs:
                out, err = pr.communicate(timeout=400)
                lines = [l for l in out.splitlines() if l.startswith("{")]
                clients.append(json.loads(lines[-1]) if lines else {"error": err.strip()[-300:]})
            matches.append({"match": mid, "bots": bots, "clients": clients})
            print(f"  match {k + 1}: " + ", ".join(f"p{c.get('player')} rollbacks {c.get('rollbacks')} stalls {c.get('stalls')} winner {c.get('winner')}" for c in clients), flush=True)
    finally:
        time.sleep(0.5)
        server.terminate()
        out, _ = server.communicate(timeout=10)
    results = {r["match"]: r for r in (json.loads(l) for l in out.splitlines() if l.startswith("{"))}
    for m in matches:
        m["server"] = results.get(m["match"])
    json.dump({"scenario": a.scenario, "matches": matches}, open(a.out, "w"), indent=1)


if __name__ == "__main__":
    main()
