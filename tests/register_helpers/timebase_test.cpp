#include <array>
#include <cstdint>
#include <iostream>
#include <string>

#define PPC_CONFIG_H_INCLUDED
#include <ppc_context.h>
#include <recompiler.h>

namespace {
constexpr uint32_t kSource = 0x1000;
constexpr uint32_t kMftbR3 = 0x7C6C42E6;

bool Expect(bool condition, const char* name) {
    if (condition) return true;
    std::cerr << "FAILED: " << name << '\n';
    return false;
}

std::string GenerateMftb() {
    std::array<uint8_t, 4> code{
        static_cast<uint8_t>(kMftbR3 >> 24),
        static_cast<uint8_t>(kMftbR3 >> 16),
        static_cast<uint8_t>(kMftbR3 >> 8),
        static_cast<uint8_t>(kMftbR3)
    };
    Recompiler recompiler;
    recompiler.image.base = kSource;
    recompiler.image.Map(".text", 0, static_cast<uint32_t>(code.size()), SectionFlags_Code, code.data());
    recompiler.image.symbols.emplace("timebase_test", kSource, 4, Symbol_Function);
    if (!recompiler.Recompile(Function{ kSource, 4 })) return {};
    return recompiler.out;
}
}

int main() {
    bool passed = true;
    passed &= Expect(PPC_TIME_BASE_FREQUENCY == 50'000'000ULL,
                     "Xenon timebase frequency is 50 MHz");
    passed &= Expect(PPC_TIME_BASE_FROM_NANOSECONDS(0) == 0,
                     "zero nanoseconds converts to zero ticks");
    passed &= Expect(PPC_TIME_BASE_FROM_NANOSECONDS(19) == 0,
                     "conversion truncates below one tick");
    passed &= Expect(PPC_TIME_BASE_FROM_NANOSECONDS(20) == 1,
                     "twenty nanoseconds converts to one tick");
    passed &= Expect(PPC_TIME_BASE_FROM_NANOSECONDS(1'000'000'000ULL) == 50'000'000ULL,
                     "one second converts to exactly 50 million ticks");

    const uint64_t first = PPC_READ_TIME_BASE();
    uint64_t second = first;
    for (size_t i = 0; i < 1'000'000 && second == first; ++i) {
        second = PPC_READ_TIME_BASE();
    }
    passed &= Expect(second >= first, "timebase is monotonic");
    passed &= Expect(second > first, "timebase advances");

    const std::string generated = GenerateMftb();
    passed &= Expect(generated.find("PPC_READ_TIME_BASE()") != std::string::npos,
                     "mftb emits the Xenon-scaled timebase helper");
    passed &= Expect(generated.find("__rdtsc()") == std::string::npos,
                     "mftb does not emit the unscaled host TSC");
    passed &= Expect(generated.find("Unrecognized instruction") == std::string::npos,
                     "mftb has no unsupported-instruction output");
    return passed ? 0 : 1;
}
