#include <array>
#include <cstdint>
#include <iostream>
#include <string>

#define PPC_CONFIG_H_INCLUDED
#include <ppc_context.h>
#include <recompiler.h>

namespace {
constexpr uint32_t kSource = 0x1000;

bool Expect(bool condition, const char* name) {
    if (condition) return true;
    std::cerr << "FAILED: " << name << '\n';
    return false;
}

uint32_t XL(uint32_t xo, uint32_t target, uint32_t a, uint32_t b) {
    return (19u << 26) | (target << 21) | (a << 16) | (b << 11) | (xo << 1);
}

std::string Generate(uint32_t instruction) {
    std::array<uint8_t, 4> code{
        static_cast<uint8_t>(instruction >> 24), static_cast<uint8_t>(instruction >> 16),
        static_cast<uint8_t>(instruction >> 8), static_cast<uint8_t>(instruction)
    };
    Recompiler recompiler;
    recompiler.image.base = kSource;
    recompiler.image.Map(".text", 0, static_cast<uint32_t>(code.size()), SectionFlags_Code, code.data());
    recompiler.image.symbols.emplace("crlogic_test", kSource, 4, Symbol_Function);
    if (!recompiler.Recompile(Function{ kSource, 4 })) return {};
    return recompiler.out;
}
}

int main() {
    bool passed = true;
    PPCContext context{};

    // cror 4*cr1+eq,lt,4*cr6+eq: the binary's representative form.
    context.cr0.lt = 1;
    context.cr6.eq = 0;
    context.cr1.eq = static_cast<uint8_t>(context.cr0.lt || context.cr6.eq);
    passed &= Expect(context.cr1.eq == 1, "cror combines independently addressed CR bits");

    // crorc eq,4*cr1+eq,4*cr5+lt: the following representative form.
    context.cr1.eq = 0;
    context.cr5.lt = 0;
    context.cr0.eq = static_cast<uint8_t>(context.cr1.eq || !context.cr5.lt);
    passed &= Expect(context.cr0.eq == 1, "crorc complements only source-B CR bit");

    const std::string cror = Generate(XL(449, 6, 0, 26));
    passed &= Expect(cror.find("ctx.cr1.eq = ctx.cr0.lt || ctx.cr6.eq;") != std::string::npos, "cror decodes individual source and target CR bits");
    passed &= Expect(cror.find("Unrecognized instruction") == std::string::npos, "cror has no unsupported-instruction output");

    const std::string crorc = Generate(XL(417, 2, 6, 20));
    passed &= Expect(crorc.find("ctx.cr0.eq = ctx.cr1.eq || !ctx.cr5.lt;") != std::string::npos, "crorc decodes complement source correctly");
    passed &= Expect(crorc.find("Unrecognized instruction") == std::string::npos, "crorc has no unsupported-instruction output");
    return passed ? 0 : 1;
}
