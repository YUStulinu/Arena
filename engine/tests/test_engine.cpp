// Engine tests: fixed-point math, determinism, the Lua sandbox, and rollback
// sessions that exchange inputs through a lossy, delaying, reordering channel
// and must agree with a straight simulation of the true inputs.
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>

#include "arena/session.hpp"

using namespace arena;

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const char* what) {
    if (ok) ++g_pass;
    else {
        ++g_fail;
        std::printf("FAIL %s\n", what);
    }
}

static std::string read_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

static void test_fixed() {
    check(fx_sin(0) == 0 && fx_sin(1024) == FX_ONE && fx_sin(2048) == 0 && fx_sin(3072) == -FX_ONE, "sin at the axes");
    check(std::abs(fx_sin(512) - 46341) <= 1, "sin(45 degrees)");
    check(fx_cos(0) == FX_ONE && fx_cos(-1024) == 0, "cos");
    check(fx_sqrt(fx_int(4)) == fx_int(2) && fx_sqrt(fx_int(2)) == 92681, "sqrt");
    check(fx_mul(fx_int(3), fx_milli(500)) == fx_milli(1500) && fx_div(fx_int(1), fx_int(4)) == fx_milli(250), "mul and div");
    check(fx_len(fx_int(3), fx_int(4)) == fx_int(5), "len 3-4-5");
}

// a simple scripted opponent: steer at the other drone, pulse when close
static uint8_t chase(const State& s, int me, std::mt19937& rng) {
    const Player& a = s.p[me];
    const Player& b = s.p[1 - me];
    int dir = 1;
    fx dx = b.x - a.x, dy = b.y - a.y;
    // the nearest of 8 directions (integer comparison of slopes)
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
    dir = best;
    uint8_t in = uint8_t(dir);
    if (rng() % 7 == 0) in |= IN_DASH;
    if (fx_len(dx, dy) < fx_milli(2000) && rng() % 4 == 0) in |= IN_PULSE;
    if (rng() % 9 == 0) in = uint8_t(rng() % 64);  // some noise
    return in;
}

static void test_determinism(const std::string& script) {
    Game g1(script), g2(script);
    State a = g1.initial(), b = g2.initial();
    check(Game::hash(a) == Game::hash(b), "same initial state");
    std::mt19937 r1(5), r2(5);
    int finished_at = -1;
    for (int t = 0; t < 60 * 300 && a.phase == 0; ++t) {
        uint8_t in[2] = {chase(a, 0, r1), chase(a, 1, r1)};
        uint8_t in2[2] = {chase(b, 0, r2), chase(b, 1, r2)};
        g1.step(a, in);
        g2.step(b, in2);
        if (a.phase == 1 && finished_at < 0) finished_at = t;
    }
    check(Game::hash(a) == Game::hash(b), "two games with the same inputs end in the same state");
    check(a.phase == 1 && (a.winner == 0 || a.winner == 1), "a game between chasers ends with a winner");
    std::printf("  chase vs chase: finished at frame %d, score %d-%d\n", finished_at, a.p[0].score, a.p[1].score);
}

static void test_sandbox(const std::string& script) {
    auto fails = [](const std::string& s) {
        try {
            Game g(s);
            State st = g.initial();
            uint8_t in[2] = {0, 0};
            g.step(st, in);
        } catch (const std::exception&) {
            return true;
        }
        return false;
    };
    check(fails("function tick() counter = 1 end"), "a script may not create globals");
    check(fails("function tick() w.set_pos(0, 1.5, 0) end"), "floats are rejected");
    check(fails("function tick() for k in pairs({}) do end end"), "pairs is not available");
    check(fails("function tick() w.set_param('friction', 1) end"), "physics parameters only while loading");
    check(fails("this is not lua"), "syntax errors are reported");
    check(fails("function tick() table.sort({2, 1}) end"), "table.sort is not available");
    check(!fails(script), "the gameplay script loads and runs");
}

// two sessions, one per player, linked through a channel with random delay
// (in frames), loss and reordering; inputs are sent with redundancy
struct Packet {
    int deliver_at;
    int to;
    int first;
    std::vector<uint8_t> inputs;
};

static void test_rollback(const std::string& script, double loss, int min_delay, int max_delay, int delay_frames, unsigned seed) {
    Game g0(script), g1(script), gref(script);
    Session s0(g0, 0, delay_frames), s1(g1, 1, delay_frames);
    Session* ss[2] = {&s0, &s1};
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> U(0, 1);
    std::vector<Packet> wire;
    int acked[2] = {0, 0};  // acked[p]: how many of player p's inputs the peer has (known when it is delivered)
    std::mt19937 brng[2] = {std::mt19937(seed + 1), std::mt19937(seed + 2)};
    const int T = 60 * 40;
    std::vector<uint8_t> truth[2];
    for (int t = 0; t < T; ++t) {
        for (int p = 0; p < 2; ++p) {
            Session& s = *ss[p];
            // the local input for frame + delay, from what the player sees now
            if (int(s.local_inputs().size()) <= s.frame() + delay_frames) s.add_local_input(chase(s.state(), p, brng[p]));
            // send every local input the peer may lack (at most 16)
            int from = std::max(acked[p], int(s.local_inputs().size()) - 16);  // what the peer already has of ours
            Packet pk{t + min_delay + int(rng() % unsigned(max_delay - min_delay + 1)), 1 - p, from,
                      std::vector<uint8_t>(s.local_inputs().begin() + from, s.local_inputs().end())};
            if (U(rng) >= loss) wire.push_back(pk);
        }
        // deliver what is due, in random order
        std::shuffle(wire.begin(), wire.end(), rng);
        for (auto it = wire.begin(); it != wire.end();) {
            if (it->deliver_at <= t) {
                for (size_t k = 0; k < it->inputs.size(); ++k) ss[it->to]->add_remote_input(it->first + int(k), it->inputs[k]);
                acked[1 - it->to] = std::max(acked[1 - it->to], ss[it->to]->remote_known());
                it = wire.erase(it);
            } else {
                ++it;
            }
        }
        for (Session* s : ss) s->advance();
    }
    // the reference: a straight simulation of the true inputs
    int n = std::min({int(s0.local_inputs().size()), int(s1.local_inputs().size()), s0.confirmed(), s1.confirmed()});
    State ref = gref.initial();
    std::vector<uint64_t> ref_hash;
    for (int f = 0; f < n; ++f) {
        if (f % 30 == 0) ref_hash.push_back(Game::hash(ref));
        uint8_t in[2] = {s0.local_inputs()[size_t(f)], s1.local_inputs()[size_t(f)]};
        gref.step(ref, in);
    }
    int compared = 0, mismatched = 0;
    for (Session* s : ss)
        for (auto [f, h] : s->take_checksums()) {
            if (f % 30 || f / 30 >= int(ref_hash.size())) continue;
            ++compared;
            mismatched += h != ref_hash[size_t(f / 30)];
        }
    char msg[200];
    std::snprintf(msg, sizeof msg, "rollback (loss %.0f%%, delay %d-%d frames): %d checksums, all equal to the reference", loss * 100, min_delay,
                  max_delay, compared);
    check(compared > 50 && mismatched == 0, msg);
    const SessionStats& st = s0.stats();
    std::printf("  %s  rollbacks %lld, resimulated %lld, max depth %d, mispredicted %lld/%lld, stalls %lld\n", msg, (long long)st.rollbacks,
                (long long)st.resimulated, st.max_rollback, (long long)st.mispredicted, (long long)st.predicted, (long long)st.stalls);
    if (max_delay > delay_frames) check(st.rollbacks > 0, "rollbacks happened");
}

// a fixed replay (inputs from an LCG) whose hashes every build must reproduce:
// native here, the Rust server's MSVC/gcc build and the browser's Wasm
static void test_golden(const std::string& script) {
    Game g(script);
    State s = g.initial();
    uint32_t x = 12345;
    std::string got = "{\"frames\": [1000, 2000, 3000], \"hashes\": [";
    for (int f = 1; f <= 3000; ++f) {
        uint8_t in[2];
        for (auto& v : in) {
            x = x * 1103515245u + 12345u;
            v = uint8_t((x >> 16) % 64);
        }
        g.step(s, in);
        if (f % 1000 == 0) {
            char h[32];
            std::snprintf(h, sizeof h, "\"%016llx\"%s", (unsigned long long)Game::hash(s), f < 3000 ? ", " : "]}");
            got += h;
        }
    }
    if (std::getenv("ARENA_WRITE_GOLDEN")) {
        std::ofstream("tests/golden.json") << got << "\n";
        std::printf("  wrote tests/golden.json\n");
    }
    std::string want = read_file("tests/golden.json");
    while (!want.empty() && (want.back() == '\n' || want.back() == '\r')) want.pop_back();
    check(got == want, "golden replay hashes");
}

int main() {
    const std::string script = read_file("scripts/arena.lua");
    if (script.empty()) {
        std::printf("cannot read scripts/arena.lua\n");
        return 1;
    }
    test_fixed();
    test_determinism(script);
    test_sandbox(script);
    test_golden(script);
    test_rollback(script, 0.0, 0, 0, 2, 1);
    test_rollback(script, 0.05, 1, 6, 2, 2);
    test_rollback(script, 0.2, 2, 10, 2, 3);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
