#include <array>
#include <cstdint>
#include <iostream>
#include <string>

#define PPC_CONFIG_H_INCLUDED
#include <ppc_context.h>
#include <recompiler.h>

namespace {
constexpr uint32_t kSource = 0x1000;
constexpr uint32_t kDb16Cyc = 0x7FFFFB78;

bool Expect(bool condition, const char* name) {
    if (condition) return true;
    std::cerr << "FAILED: " << name << '\n';
    return false;
}

std::string GenerateDb16Cyc() {
    std::array<uint8_t, 4> code{
        static_cast<uint8_t>(kDb16Cyc >> 24),
        static_cast<uint8_t>(kDb16Cyc >> 16),
        static_cast<uint8_t>(kDb16Cyc >> 8),
        static_cast<uint8_t>(kDb16Cyc)
    };
    Recompiler recompiler;
    recompiler.image.base = kSource;
    recompiler.image.Map(".text", 0, static_cast<uint32_t>(code.size()),
                         SectionFlags_Code, code.data());
    recompiler.image.symbols.emplace("db16cyc_test", kSource, 4, Symbol_Function);
    if (!recompiler.Recompile(Function{ kSource, 4 })) return {};
    return recompiler.out;
}
}

int main() {
    const std::string generated = GenerateDb16Cyc();
    bool passed = true;
    passed &= Expect(generated.find("PPC_RUNTIME_DB16CYC();") != std::string::npos,
                     "db16cyc emits the runtime spin-hint hook");
    passed &= Expect(generated.find("Unrecognized instruction") == std::string::npos,
                     "db16cyc has no unsupported-instruction output");
    return passed ? 0 : 1;
}
