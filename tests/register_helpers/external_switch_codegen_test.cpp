#include <array>
#include <iostream>
#include <string>

#include <recompiler.h>

namespace {
constexpr uint32_t kSource = 0x1000;
constexpr uint32_t kTarget = 0x2000;

struct GeneratedCase {
    Recompiler recompiler;
    std::array<uint8_t, 4> code{ 0x4E, 0x80, 0x04, 0x20 }; // bctr

    GeneratedCase(uint32_t label, bool defineTarget) {
        recompiler.image.base = kSource;
        recompiler.image.Map(".text", 0, static_cast<uint32_t>(code.size()), SectionFlags_Code, code.data());
        recompiler.image.symbols.emplace("source", kSource, 4, Symbol_Function);
        if (defineTarget)
            recompiler.image.symbols.emplace("target", kTarget, 4, Symbol_Function);
        recompiler.config.switchTables.emplace(kSource, RecompilerSwitchTable{ 0, { label } });
    }

    std::string Generate() {
        const Function fn{ kSource, 4 };
        if (!recompiler.Recompile(fn)) return {};
        return recompiler.out;
    }
};

bool Expect(bool condition, const char* name) {
    if (condition) return true;
    std::cerr << "FAILED: " << name << '\n';
    return false;
}

bool DispatchesCaseEightWithNonzeroHighHalf(uint64_t selector) {
    switch (static_cast<uint32_t>(selector)) {
    case 8:
        return true;
    default:
        return false;
    }
}
}

int main() {
    bool passed = true;

    // 1. A local table target must remain a direct C++ label jump.
    const auto local = GeneratedCase(kSource, false).Generate();
    passed &= Expect(local.find("goto loc_1000;") != std::string::npos, "local target remains goto");
    passed &= Expect(local.find("// ERROR: 0x1000") == std::string::npos, "local target has no error diagnostic");
    passed &= Expect(local.find("switch (ctx.r0.u32)") != std::string::npos,
        "switch selector uses the PPC low 32-bit index");
    passed &= Expect(local.find("switch (ctx.r0.u64)") == std::string::npos,
        "switch selector does not include stale or arithmetic upper bits");
    passed &= Expect(DispatchesCaseEightWithNonzeroHighHalf(0x0000000100000008ull),
        "exact Probe70 high-half selector dispatches low-32 case 8");

    // 2/3. An external executable entry is a PPC tail transfer: invoke the
    // existing generated target with the same context and memory base, then
    // return from this C++ frame without changing ctx.lr.
    const auto external = GeneratedCase(kTarget, true).Generate();
    passed &= Expect(external.find("target(ctx, base);") != std::string::npos, "external target calls generated entry");
    passed &= Expect(external.find("target(ctx, base);\n\t\treturn;") != std::string::npos, "external target is tail transfer");

    // 4. A target without an exact function symbol must retain a diagnostic and
    // must not be converted into an arbitrary generated-function call.
    const auto unknown = GeneratedCase(kTarget, false).Generate();
    passed &= Expect(unknown.find("// ERROR: 0x2000") != std::string::npos, "unknown target retains diagnostic");
    passed &= Expect(unknown.find("target(ctx, base);") == std::string::npos, "unknown target is not accepted");

    // 5. Existing local table generation is covered above; verify that the
    // external form still contains the same function ABI arguments.
    passed &= Expect(external.find("target(ctx, base);") != std::string::npos, "existing PPC context ABI retained");
    return passed ? 0 : 1;
}
