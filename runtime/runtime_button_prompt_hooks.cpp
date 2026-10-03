// Keyboard button prompts (runtime_button_prompts.h): the hooks into the
// title (game executable only; the unit tests build the module without them).

#include "runtime_button_prompts.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>

#include "ppc_recomp_shared.h"

extern "C" PPC_FUNC(__imp__sub_82310F50);

// The title's icon name for a control code (r3 = code - 128, 0..51; returns
// a guest pointer to a GUI_Button_* name). DARKNESS_PROMPT_TRACE=<file>
// logs each distinct code and name with the caller (developer runs).
PPC_FUNC(sub_82310F50) {
    const uint32_t code = ctx.r3.u32;
    const uint64_t caller = ctx.lr;
    __imp__sub_82310F50(ctx, base);
    static const char* trace = std::getenv("DARKNESS_PROMPT_TRACE");
    if (!trace || !*trace) return;
    static std::mutex mutex;
    static std::ofstream out(trace, std::ios::app);
    static uint64_t seen[64] = {};
    std::string name;
    for (uint32_t i = 0; i < 48 && ctx.r3.u32; ++i) {
        const char c = char(PPC_LOAD_U8(ctx.r3.u32 + i));
        if (!c) break;
        name.push_back(c);
    }
    std::lock_guard<std::mutex> lock(mutex);
    const uint64_t key = (caller & 0xFFFFFFFFull) ^ (uint64_t(code) << 40);
    for (uint64_t& entry : seen) {
        if (entry == key) return;
        if (!entry) {
            entry = key;
            out << "code=" << code << " name=" << name << " caller=0x" << std::hex
                << (caller & 0xFFFFFFFFull) << std::dec << "\n";
            out.flush();
            return;
        }
    }
}

