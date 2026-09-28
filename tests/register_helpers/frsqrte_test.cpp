#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

#define PPC_CONFIG_H_INCLUDED
#include <ppc_context.h>
#include <recompiler.h>

namespace {
constexpr uint32_t kSource = 0x1000;
constexpr uint32_t EncodeFrsqrte(uint32_t frt, uint32_t frb, bool record = false) {
    return (63u << 26) | (frt << 21) | (frb << 11) | (26u << 1) |
           static_cast<uint32_t>(record);
}

struct Vector {
    uint64_t input;
    uint64_t output;
    uint8_t cr1;
};

// These are the exact scalar frsqrte expectations checked into Xenia
// Canary's Xbox-native instruction corpus at 6e5b8324f4101464de0f8c2334edb03cac8826c4.
constexpr Vector kNativeVectors[] = {
    {0x0000000000000000ull, 0x7FF0000000000000ull, 0x8},
    {0x8000000000000000ull, 0xFFF0000000000000ull, 0x8},
    {0x0000000000000001ull, 0x617F100000000000ull, 0x0},
    {0x000FFFFFFFFFFFFFull, 0x5FE0800000000000ull, 0x0},
    {0x3FF0000000000000ull, 0x3FEF100000000000ull, 0x0},
    {0xBFF0000000000000ull, 0x7FF8000000000000ull, 0xA},
    {0xC1E0000000000000ull, 0x7FF8000000000000ull, 0xA},
    {0x41DFFFFFFFC00000ull, 0x3EF7000000000000ull, 0x0},
    {0x7FF0000000000000ull, 0x0000000000000000ull, 0x0},
    {0xFFF0000000000000ull, 0x7FF8000000000000ull, 0xA},
    {0xFFF8000000000000ull, 0xFFF8000000000000ull, 0x0},
    {0xFFF4000000000000ull, 0xFFFC000000000000ull, 0xA},
};

bool Expect(bool condition, const char* name) {
    if (condition) return true;
    std::cerr << "FAILED: " << name << '\n';
    return false;
}

uint8_t PackCr(const PPCCRRegister& cr) {
    return static_cast<uint8_t>((cr.lt << 3) | (cr.gt << 2) |
                                (cr.eq << 1) | cr.so);
}

std::string Generate(bool record) {
    const uint32_t instruction = EncodeFrsqrte(3, 4, record);
    std::array<uint8_t, 4> code{
        static_cast<uint8_t>(instruction >> 24),
        static_cast<uint8_t>(instruction >> 16),
        static_cast<uint8_t>(instruction >> 8),
        static_cast<uint8_t>(instruction),
    };
    Recompiler recompiler;
    recompiler.image.base = kSource;
    recompiler.image.Map(".text", 0, static_cast<uint32_t>(code.size()),
                         SectionFlags_Code, code.data());
    recompiler.image.symbols.emplace("frsqrte_test", kSource, 4, Symbol_Function);
    if (!recompiler.Recompile(Function{kSource, 4})) return {};
    return recompiler.out;
}
}  // namespace

int main() {
    bool passed = true;
    for (const Vector& vector : kNativeVectors) {
        PPCRegister source{};
        PPCRegister destination{};
        PPCFPSCRRegister fpscr{};
        PPCCRRegister cr1{};
        source.u64 = vector.input;
        destination.u64 = 0x0123456789ABCDEFull;
        PPC_FRSQRTE(destination, fpscr, source, &cr1);
        if (destination.u64 != vector.output || PackCr(cr1) != vector.cr1) {
            std::cerr << "FAILED: native vector input=0x" << std::hex
                      << vector.input << " output=0x" << destination.u64
                      << " cr1=" << unsigned(PackCr(cr1)) << std::dec << '\n';
            return 1;
        }
    }

    // The table's 16 buckets must stay inside frsqrte's architectural 1/32
    // relative-error bound at both ends of each bucket.
    for (uint32_t exponent : {1022u, 1023u}) {
        for (uint32_t top = 0; top < 8; ++top) {
            const uint64_t low = (uint64_t(exponent) << 52) |
                                 (uint64_t(top) << 49);
            const uint64_t high = low | ((uint64_t(1) << 49) - 1);
            for (uint64_t bits : {low, high}) {
                PPCRegister source{};
                PPCRegister destination{};
                PPCFPSCRRegister fpscr{};
                source.u64 = bits;
                PPC_FRSQRTE(destination, fpscr, source);
                PPCRegister input{};
                input.u64 = bits;
                const double exact = 1.0 / std::sqrt(input.f64);
                const double relative = std::abs(destination.f64 - exact) / exact;
                if (relative > 1.0 / 32.0) {
                    std::cerr << "FAILED: architectural estimate bound\n";
                    return 1;
                }
            }
        }
    }

    // Enabled exceptions suppress the destination update and retain FPRF,
    // while still setting sticky detail/summary state.
    {
        PPCRegister source{};
        PPCRegister destination{};
        PPCFPSCRRegister fpscr{};
        source.f64 = -1.0;
        destination.u64 = 0x0123456789ABCDEFull;
        fpscr.value = PPC_FPSCR_VE | PPC_FPSCR_FR | PPC_FPSCR_FI | 0x00004000u;
        PPC_FRSQRTE(destination, fpscr, source);
        passed &= Expect(destination.u64 == 0x0123456789ABCDEFull,
                         "VE suppresses invalid result");
        passed &= Expect((fpscr.value & (PPC_FPSCR_FX | PPC_FPSCR_FEX |
                                         PPC_FPSCR_VX | PPC_FPSCR_VXSQRT)) ==
                            (PPC_FPSCR_FX | PPC_FPSCR_FEX |
                             PPC_FPSCR_VX | PPC_FPSCR_VXSQRT),
                         "VE sets invalid summaries");
        passed &= Expect((fpscr.value & (PPC_FPSCR_FR | PPC_FPSCR_FI)) == 0,
                         "invalid clears FR/FI");
        passed &= Expect((fpscr.value & PPC_FPSCR_FPRF) == 0x00004000u,
                         "VE retains FPRF");
    }
    {
        PPCRegister source{};
        PPCRegister destination{};
        PPCFPSCRRegister fpscr{};
        source.u64 = 0;
        destination.u64 = 0xFEDCBA9876543210ull;
        fpscr.value = PPC_FPSCR_ZE | PPC_FPSCR_FR | PPC_FPSCR_FI | 0x00008000u;
        PPC_FRSQRTE(destination, fpscr, source);
        passed &= Expect(destination.u64 == 0xFEDCBA9876543210ull,
                         "ZE suppresses zero-divide result");
        passed &= Expect((fpscr.value & (PPC_FPSCR_FX | PPC_FPSCR_FEX |
                                         PPC_FPSCR_ZX)) ==
                            (PPC_FPSCR_FX | PPC_FPSCR_FEX | PPC_FPSCR_ZX),
                         "ZE sets zero-divide summaries");
        passed &= Expect((fpscr.value & PPC_FPSCR_FPRF) == 0x00008000u,
                         "ZE retains FPRF");
    }

    const std::string plain = Generate(false);
    const std::string record = Generate(true);
    passed &= Expect(plain.find("PPC_FRSQRTE(ctx.f3, ctx.fpscr, ctx.f4)") !=
                         std::string::npos,
                     "plain frsqrte emits helper");
    passed &= Expect(record.find("PPC_FRSQRTE(ctx.f3, ctx.fpscr, ctx.f4, &ctx.cr1)") !=
                         std::string::npos,
                     "record frsqrte emits helper and CR1");
    passed &= Expect(plain.find("Unrecognized instruction") == std::string::npos &&
                         record.find("Unrecognized instruction") == std::string::npos,
                     "frsqrte has no unsupported-instruction output");
    return passed ? 0 : 1;
}
