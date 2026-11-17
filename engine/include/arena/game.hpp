// The deterministic game: the state (plain integers, copied whole for
// rollback and hashed for desync checks), the physics step in C++, and the
// gameplay rules in a Lua script run in a sandbox.
#pragma once

#include <cstdint>
#include <string>

#include "fixed.hpp"

struct lua_State;

namespace arena {

constexpr int PLAYERS = 2;
constexpr int VARS = 16;     // per-player integers owned by the script
constexpr int GLOBALS = 16;  // global integers owned by the script
constexpr int TICK_HZ = 60;

// input byte: bits 0-3 direction (0 none, 1..8 = E, NE, N, NW, W, SW, S, SE), bit 4 dash, bit 5 pulse
constexpr uint8_t IN_DASH = 1 << 4, IN_PULSE = 1 << 5;

struct Player {
    fx x, y, vx, vy;
    fx radius;
    int32_t score;
    int32_t vars[VARS];
};

struct State {
    int32_t frame;
    fx arena_radius;
    uint32_t rng;
    int32_t phase;  // 0 playing, 1 finished
    int32_t winner;  // -1 none
    int32_t globals[GLOBALS];
    Player p[PLAYERS];
    uint8_t input[PLAYERS];  // the inputs of the last step (for the script)
    uint8_t pad[2];
};

/// The physics constants (the script may change them through w.set_param).
struct Physics {
    fx friction = fx_milli(965);  // velocity kept per tick
    fx restitution = fx_milli(800);
    fx max_speed = fx_milli(400);  // units per tick
};

class Game {
public:
    /// Load the gameplay script (Lua source); throws on errors.
    explicit Game(const std::string& script);
    ~Game();
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    /// The state at the start of a match (the script's init()).
    State initial();
    /// One tick: the script's tick() with the inputs, physics, then after_physics().
    void step(State& s, const uint8_t input[PLAYERS]);
    static uint64_t hash(const State& s) { return fnv1a(&s, sizeof(State)); }
    /// The last Lua error (empty if none).
    const std::string& error() const { return error_; }

private:
    lua_State* L_;
    State* cur_ = nullptr;  // the state the script works on during a call
    Physics phys_;
    std::string error_;
    void call(const char* fn);
    void physics(State& s);
    friend struct LuaApi;
};

}  // namespace arena
