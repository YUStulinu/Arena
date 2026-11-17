#include "arena/game.hpp"

#include <cstring>
#include <stdexcept>

#ifdef ARENA_LUA_CPP  // Lua compiled as C++ (Wasm: errors become C++ exceptions instead of longjmp)
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#else
extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}
#endif

namespace arena {

// ---- the Lua API ---------------------------------------------------------------
// Scripts see two tables: `fx` (fixed-point math) and `w` (the world). All
// values are integers; floats are rejected. Scripts keep no state of their
// own: every persistent value lives in the State (w.var / w.global), so a
// rollback (copying the State) restores the script's view exactly.

struct LuaApi {
    static Game* game(lua_State* L) { return static_cast<Game*>(lua_touserdata(L, lua_upvalueindex(1))); }
    static State& st(lua_State* L) { return *game(L)->cur_; }
    static int pl(lua_State* L, int arg) {
        lua_Integer i = luaL_checkinteger(L, arg);
        luaL_argcheck(L, i >= 0 && i < PLAYERS, arg, "player index");
        return int(i);
    }
    static int32_t num(lua_State* L, int arg) {
        lua_Integer v = luaL_checkinteger(L, arg);  // errors on floats that are not whole numbers
        luaL_argcheck(L, v >= INT32_MIN && v <= INT32_MAX, arg, "out of 32-bit range");
        return int32_t(v);
    }
    static int ret(lua_State* L, lua_Integer v) {
        lua_pushinteger(L, v);
        return 1;
    }
    // fx
    static int mul(lua_State* L) { return ret(L, fx_mul(num(L, 1), num(L, 2))); }
    static int div(lua_State* L) { return ret(L, fx_div(num(L, 1), num(L, 2))); }
    static int sqrt(lua_State* L) { return ret(L, fx_sqrt(num(L, 1))); }
    static int sin(lua_State* L) { return ret(L, fx_sin(num(L, 1))); }
    static int cos(lua_State* L) { return ret(L, fx_cos(num(L, 1))); }
    static int len(lua_State* L) { return ret(L, fx_len(num(L, 1), num(L, 2))); }
    static int milli(lua_State* L) { return ret(L, fx_milli(num(L, 1))); }
    // world
    static int frame(lua_State* L) { return ret(L, st(L).frame); }
    static int px(lua_State* L) { return ret(L, st(L).p[pl(L, 1)].x); }
    static int py(lua_State* L) { return ret(L, st(L).p[pl(L, 1)].y); }
    static int vx(lua_State* L) { return ret(L, st(L).p[pl(L, 1)].vx); }
    static int vy(lua_State* L) { return ret(L, st(L).p[pl(L, 1)].vy); }
    static int radius(lua_State* L) { return ret(L, st(L).p[pl(L, 1)].radius); }
    static int set_pos(lua_State* L) {
        Player& p = st(L).p[pl(L, 1)];
        p.x = num(L, 2);
        p.y = num(L, 3);
        return 0;
    }
    static int set_vel(lua_State* L) {
        Player& p = st(L).p[pl(L, 1)];
        p.vx = num(L, 2);
        p.vy = num(L, 3);
        return 0;
    }
    static int add_vel(lua_State* L) {
        Player& p = st(L).p[pl(L, 1)];
        p.vx += num(L, 2);
        p.vy += num(L, 3);
        return 0;
    }
    static int set_radius(lua_State* L) {
        st(L).p[pl(L, 1)].radius = num(L, 2);
        return 0;
    }
    static int var(lua_State* L) {
        lua_Integer k = luaL_checkinteger(L, 2);
        luaL_argcheck(L, k >= 0 && k < VARS, 2, "var index");
        return ret(L, st(L).p[pl(L, 1)].vars[k]);
    }
    static int set_var(lua_State* L) {
        lua_Integer k = luaL_checkinteger(L, 2);
        luaL_argcheck(L, k >= 0 && k < VARS, 2, "var index");
        st(L).p[pl(L, 1)].vars[k] = num(L, 3);
        return 0;
    }
    static int global(lua_State* L) {
        lua_Integer k = luaL_checkinteger(L, 1);
        luaL_argcheck(L, k >= 0 && k < GLOBALS, 1, "global index");
        return ret(L, st(L).globals[k]);
    }
    static int set_global(lua_State* L) {
        lua_Integer k = luaL_checkinteger(L, 1);
        luaL_argcheck(L, k >= 0 && k < GLOBALS, 1, "global index");
        st(L).globals[k] = num(L, 2);
        return 0;
    }
    static int arena_radius(lua_State* L) { return ret(L, st(L).arena_radius); }
    static int set_arena_radius(lua_State* L) {
        st(L).arena_radius = num(L, 1);
        return 0;
    }
    static int score(lua_State* L) { return ret(L, st(L).p[pl(L, 1)].score); }
    static int add_score(lua_State* L) {
        st(L).p[pl(L, 1)].score += num(L, 2);
        return 0;
    }
    static int input(lua_State* L) { return ret(L, st(L).input[pl(L, 1)]); }
    static int finish(lua_State* L) {
        st(L).phase = 1;
        st(L).winner = int32_t(luaL_checkinteger(L, 1));
        return 0;
    }
    static int phase(lua_State* L) { return ret(L, st(L).phase); }
    /// a deterministic random integer in [0, n) (xorshift in the State)
    static int rand(lua_State* L) {
        lua_Integer n = luaL_checkinteger(L, 1);
        luaL_argcheck(L, n > 0, 1, "positive bound");
        uint32_t& x = st(L).rng;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return ret(L, x % uint32_t(n));
    }
    static int set_param(lua_State* L) {
        // the parameters are not part of the State: only the loading script may set them
        if (game(L)->cur_) return luaL_error(L, "w.set_param is allowed only while the script loads");
        std::string k = luaL_checkstring(L, 1);
        fx v = num(L, 2);
        Physics& ph = game(L)->phys_;
        if (k == "friction") ph.friction = v;
        else if (k == "restitution") ph.restitution = v;
        else if (k == "max_speed") ph.max_speed = v;
        else luaL_argerror(L, 1, "unknown parameter");
        return 0;
    }
    static int no_new_globals(lua_State* L) {
        return luaL_error(L, "scripts may not create globals (%s): keep state in w.var / w.global", lua_tostring(L, 2));
    }
};

static void register_table(lua_State* L, Game* g, const char* name, const luaL_Reg* fns) {
    lua_newtable(L);
    lua_pushlightuserdata(L, g);
    luaL_setfuncs(L, fns, 1);
    lua_setglobal(L, name);
}

Game::Game(const std::string& script) {
    L_ = luaL_newstate();
    // only the deterministic parts of the standard library
    luaL_requiref(L_, LUA_GNAME, luaopen_base, 1);
    luaL_requiref(L_, LUA_TABLIBNAME, luaopen_table, 1);
    luaL_requiref(L_, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L_, 3);
    // remove what could break determinism or reach outside: unordered iteration, files, code loading
    for (const char* name : {"pairs", "next", "dofile", "loadfile", "load", "collectgarbage", "print", "require"}) {
        lua_pushnil(L_);
        lua_setglobal(L_, name);
    }
    // table.sort picks a pivot from the clock: equal elements could end up in a different order
    lua_getglobal(L_, "table");
    lua_pushnil(L_);
    lua_setfield(L_, -2, "sort");
    lua_pop(L_, 1);
    const luaL_Reg fxf[] = {{"mul", LuaApi::mul}, {"div", LuaApi::div}, {"sqrt", LuaApi::sqrt}, {"sin", LuaApi::sin},
                            {"cos", LuaApi::cos}, {"len", LuaApi::len}, {"milli", LuaApi::milli}, {nullptr, nullptr}};
    register_table(L_, this, "fx", fxf);
    lua_getglobal(L_, "fx");
    lua_pushinteger(L_, FX_ONE);
    lua_setfield(L_, -2, "ONE");
    lua_pushinteger(L_, ANGLE_STEPS);
    lua_setfield(L_, -2, "TURN");
    lua_pop(L_, 1);
    const luaL_Reg wf[] = {{"frame", LuaApi::frame},         {"px", LuaApi::px},
                           {"py", LuaApi::py},               {"vx", LuaApi::vx},
                           {"vy", LuaApi::vy},               {"radius", LuaApi::radius},
                           {"set_pos", LuaApi::set_pos},     {"set_vel", LuaApi::set_vel},
                           {"add_vel", LuaApi::add_vel},     {"set_radius", LuaApi::set_radius},
                           {"var", LuaApi::var},             {"set_var", LuaApi::set_var},
                           {"global", LuaApi::global},       {"set_global", LuaApi::set_global},
                           {"arena_radius", LuaApi::arena_radius}, {"set_arena_radius", LuaApi::set_arena_radius},
                           {"score", LuaApi::score},         {"add_score", LuaApi::add_score},
                           {"input", LuaApi::input},         {"finish", LuaApi::finish},
                           {"phase", LuaApi::phase},         {"rand", LuaApi::rand},
                           {"set_param", LuaApi::set_param}, {nullptr, nullptr}};
    register_table(L_, this, "w", wf);
    if (luaL_loadbufferx(L_, script.data(), script.size(), "=gameplay", "t") != LUA_OK || lua_pcall(L_, 0, 0, 0) != LUA_OK) {
        std::string e = lua_tostring(L_, -1);
        lua_close(L_);
        throw std::runtime_error("gameplay script: " + e);
    }
    // from now on, no new globals (state must live in the State)
    lua_pushglobaltable(L_);
    lua_newtable(L_);
    lua_pushcfunction(L_, LuaApi::no_new_globals);
    lua_setfield(L_, -2, "__newindex");
    lua_setmetatable(L_, -2);
    lua_pop(L_, 1);
}

Game::~Game() { lua_close(L_); }

void Game::call(const char* fn) {
    lua_getglobal(L_, fn);
    if (lua_isnil(L_, -1)) {
        lua_pop(L_, 1);
        return;
    }
    if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
        error_ = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        throw std::runtime_error("gameplay script: " + error_);
    }
}

State Game::initial() {
    State s;
    std::memset(&s, 0, sizeof s);
    s.winner = -1;
    s.rng = 0x9E3779B9u;
    for (Player& p : s.p) p.radius = fx_milli(500);
    cur_ = &s;
    call("init");
    cur_ = nullptr;
    return s;
}

void Game::physics(State& s) {
    for (Player& p : s.p) {
        // clamp the speed, move, then friction
        fx v = fx_len(p.vx, p.vy);
        if (v > phys_.max_speed) {
            p.vx = fx_div(fx_mul(p.vx, phys_.max_speed), v);
            p.vy = fx_div(fx_mul(p.vy, phys_.max_speed), v);
        }
        p.x += p.vx;
        p.y += p.vy;
        p.vx = fx_mul(p.vx, phys_.friction);
        p.vy = fx_mul(p.vy, phys_.friction);
    }
    // the two players collide as equal discs
    Player& a = s.p[0];
    Player& b = s.p[1];
    fx dx = b.x - a.x, dy = b.y - a.y;
    fx d = fx_len(dx, dy), rr = a.radius + b.radius;
    if (d < rr) {
        fx nx = d > 0 ? fx_div(dx, d) : FX_ONE, ny = d > 0 ? fx_div(dy, d) : 0;
        fx push = (rr - d) / 2;
        a.x -= fx_mul(nx, push);
        a.y -= fx_mul(ny, push);
        b.x += fx_mul(nx, push);
        b.y += fx_mul(ny, push);
        fx rel = fx_mul(b.vx - a.vx, nx) + fx_mul(b.vy - a.vy, ny);  // closing speed along the normal
        if (rel < 0) {
            fx j = fx_mul(-rel, FX_ONE + phys_.restitution) / 2;
            a.vx -= fx_mul(j, nx);
            a.vy -= fx_mul(j, ny);
            b.vx += fx_mul(j, nx);
            b.vy += fx_mul(j, ny);
        }
    }
}

void Game::step(State& s, const uint8_t input[PLAYERS]) {
    cur_ = &s;
    for (int i = 0; i < PLAYERS; ++i) s.input[i] = input[i];
    if (s.phase == 0) {
        call("tick");
        physics(s);
        call("after_physics");
    }
    s.frame += 1;
    cur_ = nullptr;
}

}  // namespace arena
