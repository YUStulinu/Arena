#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include "arena/arena.h"
#include "arena/session.hpp"

using namespace arena;

struct ArenaGame {
    Game game;
    explicit ArenaGame(const std::string& s) : game(s) {}
};
struct ArenaSession {
    Session session;
    ArenaSession(Game& g, int local, int delay, int window) : session(g, local, delay, window) {}
};

extern "C" {

void* arena_alloc(int32_t size) { return std::malloc(size_t(size)); }
void arena_free(void* p) { std::free(p); }

ArenaGame* arena_game_new(const char* script, int32_t len, char* err, int32_t errlen) {
    try {
        return new ArenaGame(std::string(script, size_t(len)));
    } catch (const std::exception& e) {
        if (err && errlen > 0) {
            std::strncpy(err, e.what(), size_t(errlen) - 1);
            err[errlen - 1] = 0;
        }
        return nullptr;
    }
}

void arena_game_free(ArenaGame* g) { delete g; }
int32_t arena_state_size(void) { return int32_t(sizeof(State)); }
void arena_initial(ArenaGame* g, void* state) {
    State s = g->game.initial();
    std::memcpy(state, &s, sizeof s);
}

int32_t arena_step(ArenaGame* g, void* state, uint8_t in0, uint8_t in1) {
    try {
        uint8_t in[2] = {in0, in1};
        g->game.step(*static_cast<State*>(state), in);
        return 0;
    } catch (const std::exception&) {
        return -1;
    }
}

uint64_t arena_hash(const void* state) { return Game::hash(*static_cast<const State*>(state)); }

void arena_observe(const void* state, int32_t* o) {
    const State& s = *static_cast<const State*>(state);
    int k = 0;
    o[k++] = s.frame;
    o[k++] = s.arena_radius;
    o[k++] = s.phase;
    o[k++] = s.winner;
    for (const Player& p : s.p) {
        for (int32_t v : {p.x, p.y, p.vx, p.vy, p.radius, p.score}) o[k++] = v;
        for (int i = 0; i < 5; ++i) o[k++] = p.vars[i];
    }
    o[k++] = s.globals[1];  // round
    o[k++] = s.globals[2];  // pause
    while (k < ARENA_OBS) o[k++] = 0;
}

ArenaSession* arena_session_new(ArenaGame* g, int32_t local, int32_t delay, int32_t window) {
    try {
        return new ArenaSession(g->game, local, delay, window);
    } catch (const std::exception&) {
        return nullptr;
    }
}
void arena_session_free(ArenaSession* s) { delete s; }
int32_t arena_session_add_local(ArenaSession* s, uint8_t input) { return s->session.add_local_input(input); }
void arena_session_add_remote(ArenaSession* s, int32_t frame, uint8_t input) { s->session.add_remote_input(frame, input); }
int32_t arena_session_advance(ArenaSession* s) {
    try {
        return s->session.advance() ? 1 : 0;
    } catch (const std::exception&) {
        return -1;
    }
}
int32_t arena_session_frame(const ArenaSession* s) { return s->session.frame(); }
int32_t arena_session_remote_known(const ArenaSession* s) { return s->session.remote_known(); }
int32_t arena_session_local_known(const ArenaSession* s) { return int32_t(s->session.local_inputs().size()); }
uint8_t arena_session_local_input(const ArenaSession* s, int32_t frame) {
    const auto& v = s->session.local_inputs();
    return frame >= 0 && frame < int32_t(v.size()) ? v[size_t(frame)] : 0;
}
const void* arena_session_state(const ArenaSession* s) { return &s->session.state(); }
void arena_session_stats(const ArenaSession* s, int64_t* o) {
    const SessionStats& t = s->session.stats();
    o[0] = t.frames;
    o[1] = t.rollbacks;
    o[2] = t.resimulated;
    o[3] = t.max_rollback;
    o[4] = t.mispredicted;
    o[5] = t.predicted;
    o[6] = t.stalls;
}
int32_t arena_session_checksums(ArenaSession* s, int32_t* frames, uint64_t* hashes, int32_t max) {
    auto c = s->session.take_checksums();
    int32_t n = 0;
    for (auto& [f, h] : c) {
        if (n >= max) break;
        frames[n] = f;
        hashes[n] = h;
        ++n;
    }
    return n;
}

}  // extern "C"
