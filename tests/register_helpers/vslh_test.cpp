#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <string>

#define PPC_CONFIG_H_INCLUDED
#include <ppc_context.h>
#include <recompiler.h>

namespace {
constexpr uint32_t kSource = 0x1000;
constexpr uint32_t kVslh = (4u << 26) | (5u << 21) | (6u << 16) | (7u << 11) | 324u;
constexpr size_t kRandomCases = 10000;

bool Expect(bool condition, const char* name) {
    if (condition) return true;
    std::cerr << "FAILED: " << name << '\n';
    return false;
}

PPCVRegister Oracle(const PPCVRegister& a, const PPCVRegister& b) {
    PPCVRegister result{};
    for (size_t lane = 0; lane < 8; ++lane) {
        const uint16_t shift = static_cast<uint16_t>(b.u16[lane] % 16);
        // This test-only scalar oracle deliberately does not call PPC_VSLH
        // or use a SIMD intrinsic.
        result.u16[lane] = static_cast<uint16_t>(uint64_t(a.u16[lane]) * (uint64_t{ 1 } << shift));
    }
    return result;
}

bool Equal(const PPCVRegister& a, const PPCVRegister& b) {
    return std::memcmp(a.u8, b.u8, sizeof(a.u8)) == 0;
}

bool RunCase(const PPCVRegister& a, const PPCVRegister& b, const char* name) {
    PPCVRegister actual{};
    PPC_VSLH(actual, a, b);
    return Expect(Equal(actual, Oracle(a, b)), name);
}

std::string GenerateVslh() {
    std::array<uint8_t, 4> code{
        static_cast<uint8_t>(kVslh >> 24), static_cast<uint8_t>(kVslh >> 16),
        static_cast<uint8_t>(kVslh >> 8), static_cast<uint8_t>(kVslh)
    };
    Recompiler recompiler;
    recompiler.image.base = kSource;
    recompiler.image.Map(".text", 0, static_cast<uint32_t>(code.size()), SectionFlags_Code, code.data());
    recompiler.image.symbols.emplace("source", kSource, 4, Symbol_Function);
    if (!recompiler.Recompile(Function{ kSource, 4 })) return {};
    return recompiler.out;
}
}

int main() {
    bool passed = true;

    const std::array<uint16_t, 8> source{ 0x0000, 0x0001, 0x8000, 0xFFFF, 0x7FFF, 0xAAAA, 0x5555, 0x1234 };
    PPCVRegister a{};
    for (size_t lane = 0; lane < source.size(); ++lane) a.u16[lane] = source[lane];

    PPCVRegister shifts{};
    shifts.u16[0] = 0;
    passed &= RunCase(a, shifts, "shift by zero");

    for (auto& lane : shifts.u16) lane = 1;
    passed &= RunCase(a, shifts, "shift by one");

    for (auto& lane : shifts.u16) lane = 15;
    passed &= RunCase(a, shifts, "maximum in-range shift");

    const std::array<uint16_t, 8> masked{ 0xFFF0, 0xFFF1, 0xFFFE, 0xFFFF, 0x0010, 0x0021, 0x7FF2, 0x800F };
    for (size_t lane = 0; lane < masked.size(); ++lane) shifts.u16[lane] = masked[lane];
    passed &= RunCase(a, shifts, "high shift bits are masked");

    PPCVRegister zero{};
    passed &= RunCase(zero, shifts, "zero source values");

    PPCVRegister ones{};
    for (auto& lane : ones.u16) lane = 0xFFFF;
    passed &= RunCase(ones, shifts, "all one source values");

    PPCVRegister alternating{};
    for (size_t lane = 0; lane < 8; ++lane) {
        alternating.u16[lane] = lane & 1 ? 0x5555 : 0xAAAA;
        shifts.u16[lane] = static_cast<uint16_t>(lane * 3 + 1);
    }
    passed &= RunCase(alternating, shifts, "alternating bits and per-lane shifts");

    PPCVRegister laneOrder{};
    for (size_t lane = 0; lane < 8; ++lane) {
        laneOrder.u16[lane] = static_cast<uint16_t>(0x1100 + lane * 0x11);
        shifts.u16[lane] = 0;
    }
    passed &= RunCase(laneOrder, shifts, "unique lane-order vector");

    // Destination/source aliasing must be safe even though the instruction has
    // three vector operands.
    PPCVRegister aliasA = alternating;
    const PPCVRegister expectedAliasA = Oracle(aliasA, shifts);
    PPC_VSLH(aliasA, aliasA, shifts);
    passed &= Expect(Equal(aliasA, expectedAliasA), "destination aliases source A");

    PPCVRegister aliasB = shifts;
    const PPCVRegister expectedAliasB = Oracle(alternating, aliasB);
    PPC_VSLH(aliasB, alternating, aliasB);
    passed &= Expect(Equal(aliasB, expectedAliasB), "destination aliases source B");

    std::mt19937 random{ 0x564D5848 };
    for (size_t test = 0; test < kRandomCases; ++test) {
        PPCVRegister randomA{};
        PPCVRegister randomB{};
        for (size_t lane = 0; lane < 8; ++lane) {
            randomA.u16[lane] = static_cast<uint16_t>(random());
            randomB.u16[lane] = static_cast<uint16_t>(random());
        }
        PPCVRegister actual{};
        PPC_VSLH(actual, randomA, randomB);
        if (!Equal(actual, Oracle(randomA, randomB))) {
            std::cerr << "FAILED: randomized oracle case " << test << '\n';
            return 1;
        }
    }

    const auto generated = GenerateVslh();
    passed &= Expect(generated.find("PPC_VSLH(ctx.v5, ctx.v6, ctx.v7);") != std::string::npos,
        "decoder and code generator emit PPC_VSLH with the correct operands");
    passed &= Expect(generated.find("Unrecognized instruction") == std::string::npos,
        "vslh no longer uses the unsupported-instruction path");

    return passed ? 0 : 1;
}
