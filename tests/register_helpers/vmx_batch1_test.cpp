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
constexpr size_t kRandomCases = 10000;

bool Expect(bool condition, const char* name) {
    if (condition) return true;
    std::cerr << "FAILED: " << name << '\n';
    return false;
}

bool Equal(const PPCVRegister& a, const PPCVRegister& b) {
    return std::memcmp(a.u8, b.u8, sizeof(a.u8)) == 0;
}

PPCVRegister VsrahOracle(const PPCVRegister& a, const PPCVRegister& b) {
    PPCVRegister result{};
    for (size_t i = 0; i < 8; ++i) {
        const uint32_t sh = b.u16[i] % 16;
        const int32_t value = static_cast<int16_t>(a.u16[i]);
        const int32_t divisor = 1 << sh;
        result.s16[i] = static_cast<int16_t>(value >= 0 ? value / divisor : -(((-value) + divisor - 1) / divisor));
    }
    return result;
}

PPCVRegister SplatOracle(int32_t immediate) {
    PPCVRegister result{};
    const int16_t value = static_cast<int16_t>(immediate);
    for (auto& lane : result.s16) lane = value;
    return result;
}

PPCVRegister VandcOracle(const PPCVRegister& a, const PPCVRegister& b) {
    PPCVRegister result{};
    for (size_t i = 0; i < 16; ++i) result.u8[i] = a.u8[i] & static_cast<uint8_t>(~b.u8[i]);
    return result;
}

PPCVRegister VmaxshOracle(const PPCVRegister& a, const PPCVRegister& b) {
    PPCVRegister result{};
    for (size_t i = 0; i < 8; ++i) result.s16[i] = a.s16[i] >= b.s16[i] ? a.s16[i] : b.s16[i];
    return result;
}

PPCVRegister VminshOracle(const PPCVRegister& a, const PPCVRegister& b) {
    PPCVRegister result{};
    for (size_t i = 0; i < 8; ++i) result.s16[i] = a.s16[i] < b.s16[i] ? a.s16[i] : b.s16[i];
    return result;
}

std::string GenerateVsel128() {
    // VX128(5, 848): all encoded register fields are zero; this is enough to
    // exercise the distinct Xenon decoder instruction id and its four-operand
    // code-generation path.
    std::array<uint8_t, 4> code{ 0x14, 0x00, 0x03, 0x50 };
    Recompiler recompiler;
    recompiler.image.base = 0x1000;
    recompiler.image.Map(".text", 0, static_cast<uint32_t>(code.size()), SectionFlags_Code, code.data());
    recompiler.image.symbols.emplace("vsel128_test", 0x1000, 4, Symbol_Function);
    if (!recompiler.Recompile(Function{ 0x1000, 4 })) return {};
    return recompiler.out;
}
}

int main() {
    bool passed = true;
    PPCVRegister values{};
    const std::array<uint16_t, 8> lanes{ 0x0000, 0x0001, 0x7FFF, 0x8000, 0xFFFF, 0xAAAA, 0x5555, 0x4001 };
    PPCVRegister shifts{};
    for (size_t i = 0; i < lanes.size(); ++i) {
        values.u16[i] = lanes[i];
        shifts.u16[i] = static_cast<uint16_t>(i * 0x21 + i);
    }
    PPCVRegister actual{};
    PPC_VSRAH(actual, values, shifts);
    passed &= Expect(Equal(actual, VsrahOracle(values, shifts)), "vsrah deterministic signs, lane counts, and high count bits");

    PPCVRegister aliasA = values;
    const PPCVRegister aliasAExpected = VsrahOracle(aliasA, shifts);
    PPC_VSRAH(aliasA, aliasA, shifts);
    passed &= Expect(Equal(aliasA, aliasAExpected), "vsrah destination aliases source A");
    PPCVRegister aliasB = shifts;
    const PPCVRegister aliasBExpected = VsrahOracle(values, aliasB);
    PPC_VSRAH(aliasB, values, aliasB);
    passed &= Expect(Equal(aliasB, aliasBExpected), "vsrah destination aliases source B");

    std::mt19937 random{ 0x56535241 };
    for (size_t test = 0; test < kRandomCases; ++test) {
        PPCVRegister a{};
        PPCVRegister b{};
        for (size_t lane = 0; lane < 8; ++lane) {
            a.u16[lane] = static_cast<uint16_t>(random());
            b.u16[lane] = static_cast<uint16_t>(random());
        }
        PPC_VSRAH(actual, a, b);
        if (!Equal(actual, VsrahOracle(a, b))) {
            std::cerr << "FAILED: vsrah randomized oracle case " << test << '\n';
            return 1;
        }
    }

    for (int32_t immediate : { 0, 1, 15, -1, -16 }) {
        PPC_VSPLTISH(actual, immediate);
        if (!Equal(actual, SplatOracle(immediate))) {
            std::cerr << "FAILED: vspltish immediate " << immediate << '\n';
            return 1;
        }
    }

    PPCVRegister bitA{};
    PPCVRegister bitB{};
    for (size_t i = 0; i < 16; ++i) {
        bitA.u8[i] = static_cast<uint8_t>(0xA5u ^ static_cast<uint8_t>(i * 17));
        bitB.u8[i] = static_cast<uint8_t>(0x3Cu + static_cast<uint8_t>(i * 11));
    }
    PPC_VANDC(actual, bitA, bitB);
    passed &= Expect(Equal(actual, VandcOracle(bitA, bitB)), "vandc deterministic bit selection");
    PPCVRegister vandcAliasA = bitA;
    PPC_VANDC(vandcAliasA, vandcAliasA, bitB);
    passed &= Expect(Equal(vandcAliasA, VandcOracle(bitA, bitB)), "vandc destination aliases source A");
    PPCVRegister vandcAliasB = bitB;
    PPC_VANDC(vandcAliasB, bitA, vandcAliasB);
    passed &= Expect(Equal(vandcAliasB, VandcOracle(bitA, bitB)), "vandc destination aliases source B");

    PPCVRegister signedA{};
    PPCVRegister signedB{};
    const std::array<int16_t, 8> valuesA{ -32768, -32767, -1, 0, 1, 32766, 32767, 42 };
    const std::array<int16_t, 8> valuesB{ 32767, -32768, -1, 1, 0, 32767, 32766, 42 };
    for (size_t i = 0; i < 8; ++i) {
        signedA.s16[i] = valuesA[i];
        signedB.s16[i] = valuesB[i];
    }
    PPC_VMAXSH(actual, signedA, signedB);
    passed &= Expect(Equal(actual, VmaxshOracle(signedA, signedB)), "vmaxsh signed limits and equality");
    PPC_VMINSH(actual, signedA, signedB);
    passed &= Expect(Equal(actual, VminshOracle(signedA, signedB)), "vminsh signed limits and equality");
    PPCVRegister maxAlias = signedA;
    PPC_VMAXSH(maxAlias, maxAlias, signedB);
    passed &= Expect(Equal(maxAlias, VmaxshOracle(signedA, signedB)), "vmaxsh destination aliases source A");
    PPCVRegister minAlias = signedB;
    PPC_VMINSH(minAlias, signedA, minAlias);
    passed &= Expect(Equal(minAlias, VminshOracle(signedA, signedB)), "vminsh destination aliases source B");

    std::mt19937 group2Random{ 0x564D5832 };
    for (size_t test = 0; test < kRandomCases; ++test) {
        PPCVRegister a{};
        PPCVRegister b{};
        for (size_t lane = 0; lane < 16; ++lane) {
            a.u8[lane] = static_cast<uint8_t>(group2Random());
            b.u8[lane] = static_cast<uint8_t>(group2Random());
        }
        PPC_VANDC(actual, a, b);
        if (!Equal(actual, VandcOracle(a, b))) {
            std::cerr << "FAILED: vandc randomized oracle case " << test << '\n';
            return 1;
        }
        PPC_VMAXSH(actual, a, b);
        if (!Equal(actual, VmaxshOracle(a, b))) {
            std::cerr << "FAILED: vmaxsh randomized oracle case " << test << '\n';
            return 1;
        }
        PPC_VMINSH(actual, a, b);
        if (!Equal(actual, VminshOracle(a, b))) {
            std::cerr << "FAILED: vminsh randomized oracle case " << test << '\n';
            return 1;
        }
    }

    const std::string vsel128 = GenerateVsel128();
    passed &= Expect(vsel128.find("simde_mm_or_si128") != std::string::npos, "vsel128 uses verified vsel bit-select implementation");
    passed &= Expect(vsel128.find("Unrecognized instruction") == std::string::npos, "vsel128 has no unsupported-instruction output");

    PPCVRegister subA{};
    PPCVRegister subB{};
    const std::array<int16_t, 8> subValuesA{ -32768, 32767, -12, 12, 0, 1, -1, 42 };
    const std::array<int16_t, 8> subValuesB{ 1, -1, -4, 4, 0, 1, -1, -42 };
    for (size_t i = 0; i < 8; ++i) { subA.s16[i] = subValuesA[i]; subB.s16[i] = subValuesB[i]; }
    uint32_t vscr = 0;
    PPC_VSUBSHS(actual, vscr, subA, subB);
    const std::array<int16_t, 8> subExpected{ -32768, 32767, -8, 8, 0, 0, 0, 84 };
    for (size_t i = 0; i < 8; ++i) passed &= Expect(actual.s16[i] == subExpected[i], "vsubshs signed saturation result");
    passed &= Expect((vscr & PPC_VSCR_SAT) != 0, "vsubshs sets persistent VSCR SAT");

    PPCVRegister packA{};
    PPCVRegister packB{};
    packA.s32[0] = 100; packA.s32[1] = 40000; packA.s32[2] = -40000; packA.s32[3] = -1;
    packB.s32[0] = 0; packB.s32[1] = 1; packB.s32[2] = 32767; packB.s32[3] = -32768;
    vscr = 0;
    PPC_VPKSWSS(actual, vscr, packA, packB);
    const std::array<int16_t, 8> packSignedExpected{ 0, 1, 32767, -32768, 100, 32767, -32768, -1 };
    for (size_t i = 0; i < 8; ++i) passed &= Expect(actual.s16[i] == packSignedExpected[i], "vpkswss host-order pack and saturation");
    passed &= Expect((vscr & PPC_VSCR_SAT) != 0, "vpkswss sets persistent VSCR SAT");

    packA.s32[0] = 65535; packA.s32[1] = 65536; packA.s32[2] = -1; packA.s32[3] = 7;
    packB.s32[0] = 0; packB.s32[1] = 42; packB.s32[2] = 100000; packB.s32[3] = -100000;
    vscr = 0;
    PPC_VPKSWUS(actual, vscr, packA, packB);
    const std::array<uint16_t, 8> packUnsignedExpected{ 0, 42, 65535, 0, 65535, 65535, 0, 7 };
    for (size_t i = 0; i < 8; ++i) passed &= Expect(actual.u16[i] == packUnsignedExpected[i], "vpkswus host-order pack and saturation");
    passed &= Expect((vscr & PPC_VSCR_SAT) != 0, "vpkswus sets persistent VSCR SAT");

    packA.u32[0] = 0x12345678; packA.u32[1] = 0x00010002; packA.u32[2] = 0xFFFF0000; packA.u32[3] = 0x0000FFFF;
    packB.u32[0] = 0; packB.u32[1] = 1; packB.u32[2] = 0xABCD4321; packB.u32[3] = 0x80008000;
    PPC_VPKUWUM(actual, packA, packB);
    const std::array<uint16_t, 8> packModuloExpected{ 0, 1, 0x4321, 0x8000, 0x5678, 2, 0, 0xFFFF };
    for (size_t i = 0; i < 8; ++i) passed &= Expect(actual.u16[i] == packModuloExpected[i], "vpkuwum host-order modulo pack");
    PPCVRegister packModuloAlias = packA;
    PPC_VPKUWUM(packModuloAlias, packModuloAlias, packB);
    for (size_t i = 0; i < 8; ++i) passed &= Expect(packModuloAlias.u16[i] == packModuloExpected[i], "vpkuwum destination aliases source A");

    const std::array<int16_t, 8> signedHalfA{ -129, -128, -1, 0, 1, 127, 128, 1000 };
    const std::array<int16_t, 8> signedHalfB{ -1000, -127, 42, 126, 127, 128, 129, 32767 };
    for (size_t i = 0; i < 8; ++i) { packA.s16[i] = signedHalfA[i]; packB.s16[i] = signedHalfB[i]; }
    vscr = 0;
    PPC_VPKSHSS(actual, vscr, packA, packB);
    const std::array<int8_t, 16> packByteSignedExpected{ -128, -127, 42, 126, 127, 127, 127, 127, -128, -128, -1, 0, 1, 127, 127, 127 };
    for (size_t i = 0; i < 16; ++i) passed &= Expect(actual.s8[i] == packByteSignedExpected[i], "vpkshss host-order signed saturating pack");
    passed &= Expect((vscr & PPC_VSCR_SAT) != 0, "vpkshss sets persistent VSCR SAT");

    const std::array<uint16_t, 8> unsignedHalfA{ 0, 1, 254, 255, 256, 512, 1000, 65535 };
    const std::array<uint16_t, 8> unsignedHalfB{ 255, 256, 0, 42, 254, 65535, 1, 2 };
    for (size_t i = 0; i < 8; ++i) { packA.u16[i] = unsignedHalfA[i]; packB.u16[i] = unsignedHalfB[i]; }
    vscr = 0;
    PPC_VPKUHUS(actual, vscr, packA, packB);
    const std::array<uint8_t, 16> packByteUnsignedExpected{ 255, 255, 0, 42, 254, 255, 1, 2, 0, 1, 254, 255, 255, 255, 255, 255 };
    for (size_t i = 0; i < 16; ++i) passed &= Expect(actual.u8[i] == packByteUnsignedExpected[i], "vpkuhus host-order unsigned saturating pack");
    passed &= Expect((vscr & PPC_VSCR_SAT) != 0, "vpkuhus sets persistent VSCR SAT");

    const std::array<uint16_t, 8> subUnsignedA{ 0, 1, 100, 65535, 42, 200, 500, 9 };
    const std::array<uint16_t, 8> subUnsignedB{ 1, 1, 99, 1, 43, 0, 500, 10 };
    for (size_t i = 0; i < 8; ++i) { packA.u16[i] = subUnsignedA[i]; packB.u16[i] = subUnsignedB[i]; }
    vscr = 0;
    PPC_VSUBUHS(actual, vscr, packA, packB);
    const std::array<uint16_t, 8> subUnsignedExpected{ 0, 0, 1, 65534, 0, 200, 0, 0 };
    for (size_t i = 0; i < 8; ++i) passed &= Expect(actual.u16[i] == subUnsignedExpected[i], "vsubuhs unsigned saturation result");
    passed &= Expect((vscr & PPC_VSCR_SAT) != 0, "vsubuhs sets persistent VSCR SAT");
    return passed ? 0 : 1;
}
