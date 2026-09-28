// V385: the frame-start wait hook (runtime_frame_wait.cpp) reproduces the
// generated sub_825A4278 pass by pass. This test reads the generated function
// and fails when its calls, return addresses or constants no longer match the
// hook's (a regeneration must be re-verified, never silently diverge).
#include "../../runtime/runtime_frame_wait.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace {
bool Check(bool condition, const char* message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

std::string Read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}

std::string FunctionBody(const std::string& source, const std::string& name) {
    const std::string key = "PPC_FUNC_IMPL(__imp__" + name + ")";
    const size_t begin = source.find(key);
    if (begin == std::string::npos) return {};
    const size_t end = source.find("PPC_FUNC_IMPL(", begin + key.size());
    return source.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}
}  // namespace

int main() {
    using namespace frame_wait;
    bool ok = true;
    ok &= Check(kDefaultMode == Mode::kSleepThenSpin,
                "V387 default: sleep then spin (0 = the title's own spin, by environment)");
    ok &= Check(FreeItemReady(0x40001000u), "an untagged first node is a free item");
    ok &= Check(!FreeItemReady(0x40001001u), "the tagged list end is no free item");
    // Mode 4: sleep only while a step still ends a spin margin before the
    // predicted release; short or unknown waits are spun.
    ok &= Check(SleepStepFits(0, 4000, 600) && SleepStepFits(2400, 4000, 600) &&
                    !SleepStepFits(2401, 4000, 600),
                "sleep steps stop a spin margin before the predicted release");
    ok &= Check(!SleepStepFits(0, 0, 600) && !SleepStepFits(0, 1499, 0), "short waits are spun");
    {
        RecentWaits recent;
        ok &= Check(recent.Shortest() == 0, "no prediction before the first wait");
        for (uint32_t value : {4100u, 3900u, 4200u}) recent.Add(value);
        ok &= Check(recent.Shortest() == 3900u, "prediction is the shortest recent wait");
        for (uint32_t i = 0; i < kPredictionWaits; ++i) recent.Add(5000u);
        ok &= Check(recent.Shortest() == 5000u, "old waits leave the window");
    }

    const std::string root = DARKNESS_SOURCE_ROOT;
    const std::string body =
        FunctionBody(Read(root + "/generated/ppc/ppc_recomp.62.cpp"), "sub_825A4278");
    ok &= Check(!body.empty(), "generated sub_825A4278 found in ppc_recomp.62.cpp");

    // Every call of the original, in order: return address and callee.
    const std::regex call(R"(ctx\.lr = 0x([0-9A-F]+);\s*\n\s*(\w+)\(ctx(?:\.ctr\.u32)?)");
    std::vector<std::pair<uint32_t, std::string>> calls;
    for (std::sregex_iterator it(body.begin(), body.end(), call), end; it != end; ++it) {
        calls.emplace_back(uint32_t(std::stoul((*it)[1].str(), nullptr, 16)), (*it)[2].str());
    }
    const std::vector<std::pair<uint32_t, std::string>> expected = {
        {0x825A4280u, "__savegprlr_27"},
        {kReturnFirstPump, "sub_822360B8"},
        {kReturnFirstEnter, "sub_820C85A8"},
        {kReturnFlushLeave, "sub_820C88B0"},
        {kReturnFlushPump, "sub_822360B8"},
        {kReturnFlushSleep, "sub_828A7CF0"},
        {kReturnFlushEnter, "PPC_CALL_INDIRECT_FUNC"},
        {kReturnLeaveReady, "PPC_CALL_INDIRECT_FUNC"},
        {kReturnFirstTime, "sub_820CB6C0"},
        {kReturnPassTime, "sub_820CB6C0"},
        {kReturnPassLeave, "sub_820C88B0"},
        {kReturnPassPump, "sub_822360B8"},
        {kReturnPassEnter, "PPC_CALL_INDIRECT_FUNC"},
        {kReturnLeaveTimeout, "PPC_CALL_INDIRECT_FUNC"},
    };
    ok &= Check(calls == expected, "the original's calls and return addresses match the hook");
    if (calls != expected) {
        for (const auto& [lr, callee] : calls) std::cerr << "  0x" << std::hex << lr << ' ' << callee << '\n';
    }

    // Queue r30 + 0x20000 + 6272 with r30 = 0x82A70000 - 25856; the timeout
    // double at 0x820A0000 - 9040; the flush flag, lock and free list offsets.
    for (const char* text : {"ctx.r11.s64 = -2102984704;", "ctx.r30.s64 = ctx.r11.s64 + -25856;",
                             "ctx.r11.s64 = ctx.r30.s64 + 131072;", "ctx.r3.s64 = ctx.r11.s64 + 6272;",
                             "ctx.r11.s64 = -2113273856;", "PPC_LOAD_U64(ctx.r11.u32 + -9040);",
                             "ctx.r31.s64 = ctx.r28.s64 + 44;", "PPC_LOAD_U32(ctx.r28.u32 + 36);",
                             "PPC_LOAD_U32(ctx.r28.u32 + 8);", "ctx.r11.u64 = ctx.r11.u32 & 0x1;",
                             "ea = -144 + ctx.r1.u32;", "ctx.r3.s64 = ctx.r1.s64 + 80;",
                             "ctx.r3.s64 = 10;", "ctx.r27.s64 = ctx.r11.s64 + 14080;",
                             "ctx.r29.s64 = ctx.r11.s64 + 15864;", "if (!ctx.cr6.lt) goto loc_825A4398;"}) {
        ok &= Check(body.find(text) != std::string::npos, text);
    }
    ok &= Check(uint32_t(int64_t(-2102984704) - 25856 + 131072 + 6272) == kReleaseQueue,
                "release queue address");
    ok &= Check(uint32_t(int64_t(-2113273856) - 9040) == kTimeoutConstant, "timeout constant address");
    ok &= Check(uint32_t(int64_t(-2112028672) + 14080) == kLeaveThunk &&
                    uint32_t(int64_t(-2112880640) + 15864) == kEnterThunk,
                "lock thunk addresses");
    ok &= Check(kPoolLockOffset == 44 && kPoolFlushOffset == 36 && kPoolFreeListOffset == 8 &&
                    kFrameBytes == 144 && kTimeOutputOffset == 80,
                "pool offsets and frame layout");
    // The thunks branch to the title's own Enter and Leave.
    ok &= Check(FunctionBody(Read(root + "/generated/ppc/ppc_recomp.3.cpp"), "sub_82103DF8")
                        .find("// b 0x820c85a8") != std::string::npos,
                "Enter thunk");
    ok &= Check(FunctionBody(Read(root + "/generated/ppc/ppc_recomp.9.cpp"), "sub_821D3700")
                        .find("// b 0x820c88b0") != std::string::npos,
                "Leave thunk");
    if (!ok) return 1;
    std::cout << "Frame-start wait hook matches the generated function PASS\n";
    return 0;
}
