// arena_client: a native player that connects to the match server over UDP,
// runs the rollback session at 60 frames per second of wall-clock time and is
// steered by a built-in bot. Used for the network evaluation (netem) and for
// matches between bots.
//
//   arena_client --server HOST:PORT --match ID --player 0|1 --token T
//                [--bot chase|random|idle] [--seed S] [--script engine/scripts/arena.lua]
//                [--max-seconds 300] [--report out.json]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "arena/session.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace arena;
using clk = std::chrono::steady_clock;

namespace {

std::string arg(int argc, char** argv, const char* name, const char* def) {
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], name)) return argv[i + 1];
    return def;
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int k = 0; k < 4; ++k) v.push_back(uint8_t(x >> (8 * k)));
}
void put64(std::vector<uint8_t>& v, uint64_t x) {
    for (int k = 0; k < 8; ++k) v.push_back(uint8_t(x >> (8 * k)));
}
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// the bots (they see the predicted state, like a human would)
struct Bot {
    std::string kind;
    std::mt19937 rng;
    uint8_t held = 0;
    int hold = 0;
    uint8_t act(const State& s, int me) {
        if (kind == "idle") return 0;
        if (kind == "random") {
            // a random input held for a random time (humans do not change input every frame)
            if (hold-- <= 0) {
                held = uint8_t(rng() % 9 | (rng() % 10 == 0 ? IN_DASH : 0) | (rng() % 12 == 0 ? IN_PULSE : 0));
                hold = int(rng() % 20);
            }
            return held;
        }
        // chase: steer at the other drone, dash and pulse when close
        const Player& a = s.p[me];
        const Player& b = s.p[1 - me];
        fx dx = b.x - a.x, dy = b.y - a.y;
        int best = 1;
        int64_t bestdot = INT64_MIN;
        for (int d = 1; d <= 8; ++d) {
            int ang = (d - 1) * ANGLE_STEPS / 8;
            int64_t dot = int64_t(dx) * fx_cos(ang) + int64_t(dy) * fx_sin(ang);
            if (dot > bestdot) {
                bestdot = dot;
                best = d;
            }
        }
        uint8_t in = uint8_t(best);
        fx d = fx_len(dx, dy);
        if (d < fx_milli(3000) && rng() % 20 == 0) in |= IN_DASH;
        if (d < fx_milli(1800) && rng() % 6 == 0) in |= IN_PULSE;
        return in;
    }
};

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    const std::string server = arg(argc, argv, "--server", "127.0.0.1:7700");
    const uint32_t match = uint32_t(std::stoul(arg(argc, argv, "--match", "1")));
    const int player = std::stoi(arg(argc, argv, "--player", "0"));
    const uint32_t token = uint32_t(std::stoul(arg(argc, argv, "--token", "0")));
    const double max_seconds = std::stod(arg(argc, argv, "--max-seconds", "300"));
    Bot bot{arg(argc, argv, "--bot", "chase"), std::mt19937(unsigned(std::stoul(arg(argc, argv, "--seed", "1")))), 0, 0};
    std::ifstream sf(arg(argc, argv, "--script", "engine/scripts/arena.lua"), std::ios::binary);
    std::stringstream ss;
    ss << sf.rdbuf();
    Game game(ss.str());

    // the socket
    auto colon = server.rfind(':');
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(server.substr(0, colon).c_str(), server.substr(colon + 1).c_str(), &hints, &res) != 0) {
        std::fprintf(stderr, "cannot resolve %s\n", server.c_str());
        return 1;
    }
    sockaddr_in to = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    freeaddrinfo(res);
    auto sock = socket(AF_INET, SOCK_DGRAM, 0);
#if defined(_WIN32)
    u_long nb = 1;
    ioctlsocket(sock, FIONBIO, &nb);
#else
    fcntl(sock, F_SETFL, O_NONBLOCK);
#endif
    uint64_t bytes_out = 0, bytes_in = 0, packets_out = 0, packets_in = 0;
    auto send = [&](const std::vector<uint8_t>& v) {
        sendto(sock, reinterpret_cast<const char*>(v.data()), int(v.size()), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
        bytes_out += v.size();
        ++packets_out;
    };

    // join: HELLO until WELCOME and START
    int delay = -1;
    bool started = false;
    std::vector<uint8_t> hello = {1};
    put32(hello, match);
    hello.push_back(uint8_t(player));
    put32(hello, token);
    auto t_join = clk::now();
    uint8_t buf[2048];
    std::unique_ptr<Session> session;
    uint32_t server_has_mine = 0;
    int winner = -2;
    uint32_t desyncs = 0;
    auto last_hello = clk::now() - std::chrono::seconds(1);
    clk::time_point t0;
    bool done = false;
    while (!done) {
        auto now = clk::now();
        if (!started && now - last_hello > std::chrono::milliseconds(200)) {
            send(hello);
            last_hello = now;
            if (now - t_join > std::chrono::seconds(30)) {
                std::fprintf(stderr, "no match start\n");
                return 2;
            }
        }
        // receive everything waiting
        for (;;) {
            sockaddr_in from{};
            socklen_t fl = sizeof from;
            int n = int(recvfrom(sock, reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl));
            if (n <= 0) break;
            bytes_in += size_t(n);
            ++packets_in;
            switch (buf[0]) {
                case 10:  // WELCOME
                    if (n >= 7) delay = buf[6];
                    break;
                case 11:  // START (carries the delay: it may arrive before WELCOME)
                    if (n >= 2) delay = buf[1];
                    if (!started && delay >= 0) {
                        started = true;
                        session = std::make_unique<Session>(game, player, delay);
                        t0 = clk::now();
                    }
                    break;
                case 12:  // INPUTS from the opponent
                    if (session && n >= 10) {
                        server_has_mine = std::max(server_has_mine, get32(buf + 1));
                        uint32_t first = get32(buf + 5);
                        int cnt = buf[9];
                        for (int k = 0; k < cnt && 10 + k < n; ++k) session->add_remote_input(int(first) + k, buf[10 + k]);
                    }
                    break;
                case 13:  // RESULT
                    if (n >= 6) {
                        winner = int8_t(buf[1]);
                        desyncs = get32(buf + 2);
                        done = true;
                    }
                    break;
                case 14:
                    std::fprintf(stderr, "server: %.*s\n", n - 1, reinterpret_cast<char*>(buf + 1));
                    return 3;
            }
        }
        if (started && !done) {
            // frames due by wall-clock time (with a little catch-up), each with its local input
            int target = int(std::chrono::duration<double>(clk::now() - t0).count() * TICK_HZ);
            for (int k = 0; k < 3 && session->frame() < target; ++k) {
                if (session->local_inputs().size() <= size_t(session->frame() + delay))
                    session->add_local_input(bot.act(session->state(), player));
                if (!session->advance()) break;
            }
            // send our inputs the server may lack, the ack, and new checksums
            const auto& li = session->local_inputs();
            uint32_t first = std::max<uint32_t>(server_has_mine, li.size() > 64 ? uint32_t(li.size() - 64) : 0);
            std::vector<uint8_t> pkt = {2};
            put32(pkt, match);
            pkt.push_back(uint8_t(player));
            put32(pkt, uint32_t(session->remote_known()));
            put32(pkt, first);
            pkt.push_back(uint8_t(li.size() - first));
            pkt.insert(pkt.end(), li.begin() + first, li.end());
            auto ck = session->take_checksums();
            pkt.push_back(uint8_t(std::min<size_t>(ck.size(), 20)));
            for (size_t i = 0; i < ck.size() && i < 20; ++i) {
                put32(pkt, uint32_t(ck[i].first));
                put64(pkt, ck[i].second);
            }
            send(pkt);
            if (std::chrono::duration<double>(clk::now() - t0).count() > max_seconds) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(started ? 16 : 5));
    }
    double secs = started ? std::chrono::duration<double>(clk::now() - t0).count() : 0;
    const SessionStats st = session ? session->stats() : SessionStats{};
    char json[1024];
    std::snprintf(json, sizeof json,
                  "{\"player\": %d, \"bot\": \"%s\", \"winner\": %d, \"desyncs\": %u, \"seconds\": %.2f, \"frames\": %lld, \"rollbacks\": %lld, "
                  "\"resimulated\": %lld, \"max_rollback\": %d, \"mispredicted\": %lld, \"predicted\": %lld, \"stalls\": %lld, "
                  "\"error_sum\": %.4f, \"error_max\": %.4f, \"bytes_out\": %llu, \"bytes_in\": %llu, \"packets_out\": %llu, \"packets_in\": %llu}",
                  player, bot.kind.c_str(), winner, desyncs, secs, (long long)st.frames, (long long)st.rollbacks, (long long)st.resimulated,
                  st.max_rollback, (long long)st.mispredicted, (long long)st.predicted, (long long)st.stalls, st.error_sum / 65536.0,
                  st.error_max / 65536.0, (unsigned long long)bytes_out, (unsigned long long)bytes_in, (unsigned long long)packets_out,
                  (unsigned long long)packets_in);
    std::printf("%s\n", json);
    std::string rep = arg(argc, argv, "--report", "");
    if (!rep.empty()) std::ofstream(rep) << json << "\n";
    return winner == -2 ? 4 : 0;
}
