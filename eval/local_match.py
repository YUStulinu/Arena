"""One match between two native bot clients through the match server, all
on this machine: start the server, create the match over the control port,
run the clients, collect their reports and the server's result.

    python eval/local_match.py [--bots chase random] [--delay 2] [--server-bin PATH] [--client-bin PATH]
"""

import argparse
import json
import os
import random
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = ".exe" if os.name == "nt" else ""


def control(cmd, addr=("127.0.0.1", 7702)):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(2)
    for _ in range(10):
        try:
            s.sendto(cmd.encode(), addr)
            return s.recv(512).decode()
        except OSError:
            time.sleep(0.3)
    raise RuntimeError("the server does not answer on the control port")


def run_match(bots, delay=2, server_host="127.0.0.1", udp_port=7700, seed=1, client_bin=None, max_seconds=300,
              start_server=True, server_bin=None, extra_env=None, client_prefix=None):
    server = None
    if start_server:
        server_bin = server_bin or os.path.join(ROOT, "server", "target", "release", "arena-server" + EXE)
        server = subprocess.Popen([server_bin, "--script", os.path.join(ROOT, "engine", "scripts", "arena.lua"), "--udp", f"0.0.0.0:{udp_port}"],
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        time.sleep(0.5)
    try:
        mid = random.randint(1, 2**31)
        tokens = [random.randint(1, 2**31) for _ in range(2)]
        reply = control(f"CREATE {mid} {tokens[0]} {tokens[1]} {delay}")
        assert reply.startswith("OK"), reply
        client_bin = client_bin or os.path.join(ROOT, "engine", "build", "native", "arena_client" + EXE)
        procs = []
        for p in range(2):
            cmd = (client_prefix or []) + [client_bin, "--server", f"{server_host}:{udp_port}", "--match", str(mid), "--player", str(p),
                                           "--token", str(tokens[p]), "--bot", bots[p], "--seed", str(seed * 2 + p),
                                           "--script", os.path.join(ROOT, "engine", "scripts", "arena.lua"), "--max-seconds", str(max_seconds)]
            procs.append(subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env={**os.environ, **(extra_env or {})}))
        reports = []
        for pr in procs:
            out, err = pr.communicate(timeout=max_seconds + 60)
            line = [l for l in out.splitlines() if l.startswith("{")]
            if not line:
                raise RuntimeError(f"client failed: {err.strip()}")
            reports.append(json.loads(line[-1]))
        result = None
        if server:
            time.sleep(0.3)
            server.terminate()
            out, _ = server.communicate(timeout=10)
            lines = [json.loads(l) for l in out.splitlines() if l.startswith("{")]
            result = next((l for l in lines if l["match"] == mid), None)
        return {"match": mid, "bots": bots, "delay": delay, "clients": reports, "server": result}
    finally:
        if server and server.poll() is None:
            server.kill()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--bots", nargs=2, default=["chase", "random"])
    ap.add_argument("--delay", type=int, default=2)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    print(json.dumps(run_match(a.bots, a.delay, seed=a.seed), indent=1))
