"""A bot that plays through the match server: the engine's rollback session
(ctypes) over UDP, 60 frames per second of wall-clock time, a decision every
4 frames from the bot's view of the (predicted) state. The protocol is
server/src/proto.rs."""

from __future__ import annotations

import ctypes
import json
import socket
import struct
import time
import urllib.request

from .env import LIB, action_to_input, features, new_game, raw_obs


def play_online(bot, server: str, match: int, player: int, token: int, max_seconds: float = 300.0) -> dict:
    host, port = server.rsplit(":", 1)
    addr = (host, int(port))
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setblocking(False)
    game = new_game()
    hello = struct.pack("<BIBI", 1, match, player, token)
    delay, started, session = None, False, None
    have_mine, winner, desyncs = 0, None, 0
    held = 0
    t_join = time.perf_counter()
    last_hello = last_send = 0.0
    t0 = 0.0
    bytes_out = bytes_in = 0
    try:
        while winner is None:
            now = time.perf_counter()
            if not started and now - last_hello > 0.2:
                sock.sendto(hello, addr)
                bytes_out += len(hello)
                last_hello = now
                if now - t_join > 30:
                    raise TimeoutError("the match did not start")
            while True:
                try:
                    data = sock.recv(2048)
                except (BlockingIOError, ConnectionResetError):
                    break
                bytes_in += len(data)
                kind = data[0]
                if kind == 10:
                    delay = data[6]
                elif kind == 11 and not started and delay is not None:
                    started = True
                    session = LIB.arena_session_new(game, player, delay, 12)
                    t0 = time.perf_counter()
                elif kind == 12 and session:
                    have, first, n = struct.unpack_from("<IIB", data, 1)
                    have_mine = max(have_mine, have)
                    for k in range(n):
                        LIB.arena_session_add_remote(session, first + k, data[10 + k])
                elif kind == 13:
                    winner = struct.unpack_from("<b", data, 1)[0]
                    desyncs = struct.unpack_from("<I", data, 2)[0]
                elif kind == 14:
                    raise RuntimeError(data[1:].decode(errors="replace"))
            if started and winner is None:
                target = int((time.perf_counter() - t0) * 60)
                advanced = False
                for _ in range(3):
                    f = LIB.arena_session_frame(session)
                    if f >= target:
                        break
                    if LIB.arena_session_local_known(session) <= f + delay:
                        nxt = LIB.arena_session_local_known(session)
                        if nxt % 4 == 0:  # a decision every 4 frames, held in between
                            o = raw_obs(LIB.arena_session_state(session))
                            held = action_to_input(bot.act(features(o, player)), player)
                        LIB.arena_session_add_local(session, held)
                    if LIB.arena_session_advance(session) != 1:
                        break
                    advanced = True
                # one packet per simulated frame (or a keep-alive every 50 ms while waiting)
                if not advanced and time.perf_counter() - last_send < 0.05:
                    time.sleep(0.002)
                    continue
                last_send = time.perf_counter()
                known = LIB.arena_session_local_known(session)
                first = max(have_mine, known - 64)
                inputs = bytes(LIB.arena_session_local_input(session, i) for i in range(first, known))
                frames = (ctypes.c_int32 * 20)()
                hashes = (ctypes.c_uint64 * 20)()
                k = LIB.arena_session_checksums(session, frames, hashes, 20)
                pkt = struct.pack("<BIBIIB", 2, match, player, LIB.arena_session_remote_known(session), first, len(inputs)) + inputs
                pkt += bytes([k]) + b"".join(struct.pack("<IQ", frames[i], hashes[i]) for i in range(k))
                sock.sendto(pkt, addr)
                bytes_out += len(pkt)
                if time.perf_counter() - t0 > max_seconds:
                    break
            time.sleep(0.004)
        stats = (ctypes.c_int64 * 9)()
        if session:
            LIB.arena_session_stats(session, stats)
        return {"winner": winner, "desyncs": desyncs, "seconds": time.perf_counter() - t0, "frames": stats[0], "rollbacks": stats[1],
                "mispredicted": stats[4], "predicted": stats[5], "bytes_out": bytes_out, "bytes_in": bytes_in}
    finally:
        if session:
            LIB.arena_session_free(session)
        LIB.arena_game_free(game)
        sock.close()


def queue_and_play(bot, name: str, matchmaker: str = "http://127.0.0.1:8080", **kw) -> dict:
    """Join the matchmaker's queue, wait for a match, play it."""
    req = urllib.request.Request(f"{matchmaker}/api/queue", data=json.dumps({"name": name}).encode(), method="POST",
                                 headers={"content-type": "application/json"})
    ticket = json.load(urllib.request.urlopen(req))["ticket"]
    while True:
        t = json.load(urllib.request.urlopen(f"{matchmaker}/api/ticket/{ticket}"))
        if t["status"] == "matched":
            break
        time.sleep(0.25)
    return play_online(bot, t["udp"], t["match"], t["player"], t["token"], **kw) | {"name": name, "player": t["player"], "match": t["match"]}
