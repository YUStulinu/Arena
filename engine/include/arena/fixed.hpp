// Q16.16 fixed point: every quantity of the simulation is an integer, so the
// same inputs give the same bits on every compiler and target (x86-64, Wasm).
// No floating point anywhere in the engine.
#pragma once

#include <cstddef>
#include <cstdint>

namespace arena {

using fx = int32_t;  // value * 65536
constexpr fx FX_ONE = 1 << 16;
constexpr int ANGLE_STEPS = 4096;  // a full turn

constexpr fx fx_int(int32_t n) { return n * FX_ONE; }
/// a / 1000 as fixed point (for constants written in thousandths)
constexpr fx fx_milli(int32_t m) { return fx((int64_t(m) << 16) / 1000); }
inline fx fx_mul(fx a, fx b) { return fx((int64_t(a) * b) >> 16); }
inline fx fx_div(fx a, fx b) { return b == 0 ? 0 : fx((int64_t(a) << 16) / b); }
/// square root of a fixed-point value (integer Newton-free bit method on a 64-bit radicand)
fx fx_sqrt(fx a);
/// sine and cosine of an angle in 1/4096 turns
fx fx_sin(int32_t angle);
inline fx fx_cos(int32_t angle) { return fx_sin(angle + ANGLE_STEPS / 4); }
/// length of (x, y)
inline fx fx_len(fx x, fx y) { return fx_sqrt(fx((int64_t(x) * x + int64_t(y) * y) >> 16)); }

/// 64-bit FNV-1a, for state checksums
inline uint64_t fnv1a(const void* data, std::size_t n, uint64_t h = 1469598103934665603ull) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

}  // namespace arena
