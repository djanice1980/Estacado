#include <array>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>

#define PPC_CONFIG_H_INCLUDED
#include <ppc_context.h>
#include <recompiler.h>

namespace {
constexpr uint32_t kSource = 0x1000;
constexpr uint32_t kMulhd = (31u << 26) | (3u << 21) | (4u << 16) | (5u << 11) | (73u << 1);

bool Expect(bool condition, const char* name) {
    if (condition) return true;
    std::cerr << "FAILED: " << name << '\n';
    return false;
}

uint64_t Oracle(uint64_t a, uint64_t b) {
    const __int128 product = static_cast<__int128>(static_cast<int64_t>(a)) * static_cast<__int128>(static_cast<int64_t>(b));
    return static_cast<uint64_t>(product >> 64);
}

std::string GenerateMulhd() {
    std::array<uint8_t, 4> code{
        static_cast<uint8_t>(kMulhd >> 24), static_cast<uint8_t>(kMulhd >> 16),
        static_cast<uint8_t>(kMulhd >> 8), static_cast<uint8_t>(kMulhd)
    };
    Recompiler recompiler;
    recompiler.image.base = kSource;
    recompiler.image.Map(".text", 0, static_cast<uint32_t>(code.size()), SectionFlags_Code, code.data());
    recompiler.image.symbols.emplace("mulhd_test", kSource, 4, Symbol_Function);
    if (!recompiler.Recompile(Function{ kSource, 4 })) return {};
    return recompiler.out;
}
}

int main() {
    bool passed = true;
    const std::array<int64_t, 12> values{
        0, 1, -1, 2, -2, 0x7FFFFFFF, -0x80000000LL,
        INT64_MAX, INT64_MIN, 0x123456789ABCDELL, -0x123456789ABCDELL,
        static_cast<int64_t>(0x8000000000000001ULL)
    };
    for (int64_t a : values) {
        for (int64_t b : values) {
            const auto actual = PPC_MULHD(static_cast<uint64_t>(a), static_cast<uint64_t>(b));
            if (actual != Oracle(static_cast<uint64_t>(a), static_cast<uint64_t>(b))) {
                std::cerr << "FAILED: mulhd edge case\n";
                return 1;
            }
        }
    }

    std::mt19937_64 random{ 0x4D554C4844ULL };
    for (size_t i = 0; i < 100000; ++i) {
        const uint64_t a = random();
        const uint64_t b = random();
        if (PPC_MULHD(a, b) != Oracle(a, b)) {
            std::cerr << "FAILED: mulhd randomized oracle case " << i << '\n';
            return 1;
        }
    }

    const std::string generated = GenerateMulhd();
    passed &= Expect(generated.find("PPC_MULHD") != std::string::npos, "mulhd emits portable signed high-product helper");
    passed &= Expect(generated.find("Unrecognized instruction") == std::string::npos, "mulhd has no unsupported-instruction output");
    return passed ? 0 : 1;
}
