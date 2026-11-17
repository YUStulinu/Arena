/* The engine's C interface: for the Rust server (FFI), the Python bots
 * (ctypes) and the browser (Wasm exports). States are opaque byte blocks of
 * arena_state_size() bytes owned by the caller. */
#ifndef ARENA_H
#define ARENA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__wasm__)
#define ARENA_API(name) __attribute__((export_name(#name)))
#elif defined(_WIN32) && defined(ARENA_SHARED)
#define ARENA_API(name) __declspec(dllexport)
#else
#define ARENA_API(name)
#endif

typedef struct ArenaGame ArenaGame;
typedef struct ArenaSession ArenaSession;

/* number of int32 values written by arena_observe */
#define ARENA_OBS 32

/* memory for the caller's buffers (used by the browser to pass the script and states) */
ARENA_API(arena_alloc) void* arena_alloc(int32_t size);
ARENA_API(arena_free) void arena_free(void* p);

ARENA_API(arena_game_new) ArenaGame* arena_game_new(const char* script, int32_t len, char* err, int32_t errlen);
ARENA_API(arena_game_free) void arena_game_free(ArenaGame* g);
ARENA_API(arena_state_size) int32_t arena_state_size(void);
ARENA_API(arena_initial) void arena_initial(ArenaGame* g, void* state);
/* one tick; 0 on success, -1 on a script error */
ARENA_API(arena_step) int32_t arena_step(ArenaGame* g, void* state, uint8_t in0, uint8_t in1);
ARENA_API(arena_hash) uint64_t arena_hash(const void* state);
/* frame, arena radius, phase, winner, then per player x, y, vx, vy, radius, score, dash cd, pulse cd, facing, pulse flash, fallen,
 * then the round number and the pause counter (fixed-point values raw, Q16.16) */
ARENA_API(arena_observe) void arena_observe(const void* state, int32_t* out);

ARENA_API(arena_session_new) ArenaSession* arena_session_new(ArenaGame* g, int32_t local, int32_t delay, int32_t window);
ARENA_API(arena_session_free) void arena_session_free(ArenaSession* s);
ARENA_API(arena_session_add_local) int32_t arena_session_add_local(ArenaSession* s, uint8_t input);
ARENA_API(arena_session_add_remote) void arena_session_add_remote(ArenaSession* s, int32_t frame, uint8_t input);
/* 1 advanced, 0 stalled, -1 script error */
ARENA_API(arena_session_advance) int32_t arena_session_advance(ArenaSession* s);
ARENA_API(arena_session_frame) int32_t arena_session_frame(const ArenaSession* s);
ARENA_API(arena_session_remote_known) int32_t arena_session_remote_known(const ArenaSession* s);
ARENA_API(arena_session_local_known) int32_t arena_session_local_known(const ArenaSession* s);
ARENA_API(arena_session_local_input) uint8_t arena_session_local_input(const ArenaSession* s, int32_t frame);
ARENA_API(arena_session_state) const void* arena_session_state(const ArenaSession* s);
/* frames, rollbacks, resimulated, max rollback, mispredicted, predicted, stalls, position error sum, max (fixed point) */
ARENA_API(arena_session_stats) void arena_session_stats(const ArenaSession* s, int64_t* out9);
/* up to max (frame, hash) pairs of newly confirmed checksums; returns the count */
ARENA_API(arena_session_checksums) int32_t arena_session_checksums(ArenaSession* s, int32_t* frames, uint64_t* hashes, int32_t max);

#ifdef __cplusplus
}
#endif
#endif
