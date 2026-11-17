#include "arena/fixed.hpp"

#include <array>

namespace arena {

fx fx_sqrt(fx a) {
    if (a <= 0) return 0;
    uint64_t r = uint64_t(a) << 16, res = 0, bit = uint64_t(1) << 62;
    while (bit > r) bit >>= 2;
    while (bit) {
        if (r >= res + bit) {
            r -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return fx(res);
}

namespace {
// sin over a quarter turn in Q16.16, computed at compile time with integer
// arithmetic only (Taylor series in Q30 for x in [0, pi/2]; every product
// fits in 64 bits), so the table is the same everywhere
constexpr int64_t kPiQ30 = 3373259426LL;  // pi * 2^30
constexpr std::array<fx, ANGLE_STEPS / 4 + 1> make_quarter() {
    std::array<fx, ANGLE_STEPS / 4 + 1> t{};
    for (int i = 0; i <= ANGLE_STEPS / 4; ++i) {
        const int64_t x = kPiQ30 * 2 * i / ANGLE_STEPS;  // (i / ANGLE_STEPS) * 2 pi, Q30
        const int64_t x2 = (x * x) >> 30;
        int64_t term = x, sum = x;
        for (int k = 1; k < 12; ++k) {
            term = -((term * x2) >> 30) / ((2 * k) * (2 * k + 1));
            sum += term;
        }
        t[i] = fx((sum + (1LL << 13)) >> 14);
    }
    return t;
}
constexpr auto kQuarter = make_quarter();
}  // namespace

fx fx_sin(int32_t angle) {
    int32_t a = angle & (ANGLE_STEPS - 1);
    const int32_t q = ANGLE_STEPS / 4;
    if (a < q) return kQuarter[a];
    if (a < 2 * q) return kQuarter[2 * q - a];
    if (a < 3 * q) return -kQuarter[a - 2 * q];
    return -kQuarter[4 * q - a];
}

}  // namespace arena
