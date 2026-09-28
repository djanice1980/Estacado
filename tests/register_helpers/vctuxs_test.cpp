// vctuxs / vcfpuxws128 (float to unsigned word, truncating, saturating):
// simde_mm_vctuxs against a scalar oracle, including NaN, negative zero,
// the 2^31 and 2^32 boundaries and random values; and the recompiler now
// emits it (and fnmadd, dcbst) instead of a skipped instruction.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>

#define PPC_CONFIG_H_INCLUDED
#include <ppc_context.h>

namespace {

bool Expect(bool condition, const char* name) {
    if (!condition) std::cerr << "FAILED: " << name << '\n';
    return condition;
}

uint32_t Oracle(float value) {
    if (std::isnan(value) || value <= 0.0f) return 0;
    if (value >= 4294967296.0f) return 0xFFFFFFFFu;
    return static_cast<uint32_t>(static_cast<double>(value));  // truncation
}

bool Check(const float (&in)[4], const char* name) {
    PPCVRegister source{}, result{};
    std::memcpy(source.f32, in, sizeof(in));
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(result.u32),
                         simde_mm_vctuxs(simde_mm_load_ps(source.f32)));
    bool ok = true;
    for (int i = 0; i < 4; ++i) ok &= result.u32[i] == Oracle(in[i]);
    return Expect(ok, name);
}

}  // namespace

int main() {
    bool ok = true;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    ok &= Check({nan, -nan, -1.0f, -0.0f}, "NaN and negative values give 0");
    ok &= Check({0.0f, 0.99f, 1.0f, 1.9f}, "small values truncate toward zero");
    ok &= Check({2147483520.0f, 2147483648.0f, 2147483904.0f, 3000000000.0f},
                "values around and above 2^31 stay unsigned");
    ok &= Check({4294967040.0f, 4294967296.0f, 1e20f, inf}, "values from 2^32 saturate");
    ok &= Check({-inf, 65535.5f, 16777216.0f, 123456.78f}, "mixed values");
    std::mt19937 random(12345);
    std::uniform_real_distribution<float> spread(-1e10f, 1e10f);
    for (int n = 0; n < 20000 && ok; ++n) {
        const float in[4] = {spread(random), spread(random) * 1e-6f, spread(random) * 1e-3f,
                             std::ldexp(spread(random), -8)};
        ok &= Check(in, "random values");
    }
    std::cout << (ok ? "vctuxs: all checks passed\n" : "vctuxs: FAILED\n");
    return ok ? 0 : 1;
}
