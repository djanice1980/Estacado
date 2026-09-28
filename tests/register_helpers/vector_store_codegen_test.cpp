#include <array>
#include <cstdint>
#include <iostream>
#include <string>

#include <recompiler.h>

namespace {
constexpr uint32_t kSource = 0x1000;
// stvx v3, r4, r5
constexpr uint32_t kStvx = (31u << 26) | (3u << 21) | (4u << 16) |
                           (5u << 11) | (231u << 1);

std::string GenerateStvx() {
    std::array<uint8_t, 4> code{
        static_cast<uint8_t>(kStvx >> 24), static_cast<uint8_t>(kStvx >> 16),
        static_cast<uint8_t>(kStvx >> 8), static_cast<uint8_t>(kStvx)};
    Recompiler recompiler;
    recompiler.image.base = kSource;
    recompiler.image.Map(".text", 0, static_cast<uint32_t>(code.size()),
                         SectionFlags_Code, code.data());
    recompiler.image.symbols.emplace("stvx_test", kSource, 4, Symbol_Function);
    if (!recompiler.Recompile(Function{kSource, 4})) return {};
    return recompiler.out;
}
}  // namespace

int main() {
    const std::string generated = GenerateStvx();
    if (generated.find("PPC_STORE_V128((ctx.r4.u32 + ctx.r5.u32) & ~0xF") ==
        std::string::npos) {
        std::cerr << "stvx did not use the overridable vector-store boundary\n";
        return 1;
    }
    if (generated.find("simde_mm_store_si128((simde__m128i*)(base") !=
        std::string::npos) {
        std::cerr << "stvx still emitted a direct guest-memory store\n";
        return 1;
    }
    std::cout << "vector store codegen test passed\n";
    return 0;
}
