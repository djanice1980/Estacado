// V390: right-stick look at high frame rates (runtime_stick_look.cpp). This
// test fails when the generated emitter, look() or its callers no longer
// match what the hook relies on, and checks the carry / cadence policy
// against a model of the title's emitter: identical command streams at 60 FPS
// and below or above 250 FPS, and the title's turn per cadence window at
// 120-240 FPS with a command on every frame.
#include "../../runtime/runtime_stick_look_policy.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

namespace {
bool Check(bool condition, const std::string& message) {
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

bool Has(const std::string& body, const std::string& asmText) {
    return body.find("// " + asmText + "\n") != std::string::npos ||
           body.find("// " + asmText + " \n") != std::string::npos ||
           body.find("// " + asmText + "\r\n") != std::string::npos;
}

// The title's emitter for one axis at a constant look speed (argument per
// second), frames at the given times: the units of every command it sends.
struct Stream {
    std::vector<int32_t> units;
    std::vector<double> times;
};

double Jittered(int frame, double seconds, double jitter) {
    // Deterministic +-jitter around the frame interval.
    const uint32_t hash = uint32_t(frame) * 2654435761u;
    const double unit = double(hash % 2001u) / 1000.0 - 1.0;
    return seconds * (1.0 + jitter * unit);
}

Stream TitleStream(double fps, double jitter, double seconds, float argumentPerSecond, float sens) {
    using namespace stick_look;
    Stream out;
    const float interval = float(1.0 / 120.0);
    double now = 1.0, last = 1.0;
    for (int frame = 0; now < 1.0 + seconds; ++frame) {
        now += Jittered(frame, 1.0 / fps, jitter);
        const float elapsed = TitleElapsed(now, last, 1.0f);
        if (!TitleSends(elapsed, interval)) continue;
        const float argument = argumentPerSecond * elapsed;
        out.units.push_back(TruncateUnits(TitleUnitsFloat(argument, sens, kYawScale)));
        out.times.push_back(now);
        last = now;
    }
    return out;
}

Stream HookStream(double fps, double jitter, double seconds, float argumentPerSecond, float sens,
                  uint64_t* cadencePoints = nullptr) {
    using namespace stick_look;
    Stream out;
    Tracker tracker;
    ResetFor(tracker, 0x40000000u);
    const float titleInterval = float(1.0 / 120.0);
    const int64_t frequency = 10000000;
    double now = 1.0, last = 1.0;
    for (int frame = 0; now < 1.0 + seconds; ++frame) {
        now += Jittered(frame, 1.0 / fps, jitter);
        NoteFrame(tracker, int64_t(now * double(frequency)), frequency);
        SyncCadence(tracker, last);
        const float interval = UseHfrInterval(tracker) ? kHfrInterval : titleInterval;
        const float elapsed = TitleElapsed(now, last, 1.0f);
        if (!TitleSends(elapsed, interval)) continue;
        const float argument = argumentPerSecond * elapsed;
        const Look look = CarryLook(tracker, argument, 0.0f, sens, 1.0f);
        out.units.push_back(TruncateUnits(TitleUnitsFloat(look.yaw.argument, sens, kYawScale)));
        out.times.push_back(now);
        last = now;
        if (AfterSend(tracker, now, 1.0f, titleInterval) && cadencePoints) ++*cadencePoints;
    }
    return out;
}

// Total units the title and the hook have sent by each of the title's send
// times (the hook's commands up to that instant).
int32_t WorstWindowDifference(const Stream& title, const Stream& hook) {
    int32_t worst = 0;
    int64_t titleTotal = 0, hookTotal = 0;
    size_t h = 0;
    for (size_t t = 0; t < title.units.size(); ++t) {
        titleTotal += title.units[t];
        while (h < hook.units.size() && hook.times[h] <= title.times[t] + 1e-12) hookTotal += hook.units[h++];
        worst = std::max<int32_t>(worst, int32_t(std::llabs(titleTotal - hookTotal)));
    }
    return worst;
}
}  // namespace

int main() {
    using namespace stick_look;
    bool ok = true;

    // --- The generated code the hook relies on.
    const std::string root = DARKNESS_SOURCE_ROOT;
    const std::string gen41 = Read(root + "/generated/ppc/ppc_recomp.41.cpp");
    const std::string emitter = FunctionBody(gen41, "sub_823FC650");
    ok &= Check(!emitter.empty(), "generated sub_823FC650 (look emitter) in ppc_recomp.41.cpp");
    {
        const std::regex call(R"(ctx\.lr = 0x([0-9A-F]+);)");
        std::vector<uint32_t> returns;
        for (std::sregex_iterator it(emitter.begin(), emitter.end(), call), end; it != end; ++it) {
            returns.push_back(uint32_t(std::stoul((*it)[1].str(), nullptr, 16)));
        }
        ok &= Check(returns.size() == 4 && returns.back() == kLookReturn,
                    "the emitter's last call (look) returns to 0x823FC7E4");
    }
    for (const char* line : {"lfd f0,7064(r31)", "stfd f0,7064(r31)", "lfs f12,424(r31)",
                             "fcmpu cr6,f31,f30", "ble cr6,0x823fc7f8", "lwz r10,1056(r10)",
                             "lfs f13,8820(r31)", "lfs f12,8824(r31)", "fmuls f1,f13,f0",
                             "fmuls f2,f12,f0"}) {
        ok &= Check(Has(emitter, line), std::string("emitter: ") + line);
    }
    const std::string look =
        FunctionBody(Read(root + "/generated/ppc/ppc_recomp.42.cpp"), "sub_823FCF68");
    ok &= Check(!look.empty(), "generated sub_823FCF68 (look) in ppc_recomp.42.cpp");
    for (const char* line : {"lfs f13,8840(r31)", "fmuls f11,f13,f30", "lfs f12,8836(r31)",
                             "fmuls f10,f12,f31", "lfs f13,-8104(r11)", "fmuls f13,f11,f13",
                             "lfs f12,-30088(r11)", "fmuls f12,f10,f12", "bl 0x82432e60"}) {
        ok &= Check(Has(look, line), std::string("look(): ") + line);
    }
    ok &= Check(kSensXOffset == 8836 && kSensYOffset == 8840, "sensitivity offsets");
    const std::string encoder =
        FunctionBody(Read(root + "/generated/ppc/ppc_recomp.33.cpp"), "sub_8237C2F0");
    {
        size_t truncations = 0;
        for (size_t at = encoder.find("// fctiwz f0,f0"); at != std::string::npos;
             at = encoder.find("// fctiwz f0,f0", at + 1)) {
            ++truncations;
        }
        ok &= Check(truncations == 3, "the encoder truncates each axis toward zero (fctiwz)");
    }
    // Per-frame callers pass the float at 0x820A0000 - 5984; sub_823FC818
    // passes its own 0.05 s and stays the title's.
    ok &= Check(kTitleIntervalAddress == 0x820A0000u - 5984u, "interval constant address");
    for (const char* name : {"sub_823F9B00", "sub_823F8AF0"}) {
        const std::string body = FunctionBody(gen41, name);
        ok &= Check(Has(body, "lfs f1,-5984(r11)") && Has(body, "lwz r11,956(r10)"),
                    std::string(name) + " calls the emitter with the 1/120 s constant");
    }
    ok &= Check(Has(FunctionBody(gen41, "sub_823FC818"), "lfs f1,31892(r11)"),
                "sub_823FC818 keeps its own interval");

    // --- Policy.
    ok &= Check(IsTitleInterval(float(1.0 / 120.0)) && !IsTitleInterval(0.05f) &&
                    !IsTitleInterval(kHfrInterval),
                "only the per-frame 1/120 s interval is replaced");
    ok &= Check(FloatBits(kHfrInterval) == 0x3B5A740Eu, "HFR interval is float 1/300");
    ok &= Check(TitleElapsed(10.5, 10.0, 1.0f) == 0.5f && TitleElapsed(10.0, 10.5, 1.0f) == 0.0f &&
                    TitleElapsed(20.0, 10.0, 2.0f) == 2.0f &&
                    TitleElapsed(std::nan(""), 1.0, 1.0f) == 0.0f,
                "elapsed time is clamped to [0, 1] s then scaled, as the emitter does");
    ok &= Check(!TitleSends(float(1.0 / 120.0), float(1.0 / 120.0)) &&
                    TitleSends(std::nextafter(float(1.0 / 120.0), 1.0f), float(1.0 / 120.0)),
                "the emitter sends only after strictly more than its interval");
    {
        const AxisLook same = CarryAxis(0.0123f, 1.37f, kYawScale, 0.0);
        ok &= Check(FloatBits(same.argument) == FloatBits(0.0123f),
                    "without a carry the argument is the title's, bit for bit");
        const float units = TitleUnitsFloat(0.0123f, 1.37f, kYawScale);
        ok &= Check(same.units == int32_t(units) && std::fabs(same.carry - (double(units) - same.units)) < 1e-9,
                    "the remainder of a plain command carries");
    }
    {
        // Sweep: the argument always packs exactly the carried units.
        bool packs = true;
        double worstCarry = 0.0;
        for (float sens : {0.35f, 0.8f, 1.0f, 1.37f, 2.2f, 4.0f}) {
            for (float scale : {kYawScale, kPitchScale}) {
                for (int i = 1; i < 4000; ++i) {
                    const float argument = float(i) * 0.00137f * (i % 2 ? 1.0f : -1.0f);
                    const float units = TitleUnitsFloat(argument, sens, scale);
                    const double carry = (units > 0 ? 1.0 : -1.0) * double(i % 97) / 97.0;
                    const AxisLook out = CarryAxis(argument, sens, scale, carry);
                    const int32_t packed =
                        TruncateUnits(TitleUnitsFloat(out.argument, sens, scale));
                    const double exact = double(units) + carry;
                    packs &= packed == out.units && out.units == TruncateUnits(exact) &&
                             std::fabs(out.carry - (exact - out.units)) < 1e-6 &&
                             (out.argument != 0.0f) == (argument != 0.0f);
                    worstCarry = std::fmax(worstCarry, std::fabs(out.carry));
                }
            }
        }
        ok &= Check(packs, "carried arguments pack exactly trunc(title units + carry)");
        ok &= Check(worstCarry < 1.0, "a carry stays below one unit");
    }
    {
        const AxisLook small = CarryAxis(0.004f, 1.0f, kYawScale, -0.5);  // 0.128 units - 0.5
        ok &= Check(small.units == 0 && small.argument != 0.0f &&
                        TruncateUnits(TitleUnitsFloat(small.argument, 1.0f, kYawScale)) == 0,
                    "a command the title sends with zero units is still sent with zero units");
        const AxisLook still = CarryAxis(0.0f, 1.0f, kPitchScale, 0.4);
        ok &= Check(still.argument == 0.0f && still.units == 0 && still.carry == 0.4,
                    "an axis at rest stays at rest and keeps its remainder");
    }
    // Packet budget: per-frame looks only while frames average >= 1/250 s.
    {
        const double framesPerStep = (1.0 / kMinHfrFrameSeconds) / 30.0;
        const double bytes = framesPerStep * double(kFrameCommandBytes + kLookCommandBytes);
        ok &= Check(bytes + 32.0 <= double(kStepPacketBytes),
                    "per-frame looks leave >= 32 bytes of each 254-byte step packet at 250 FPS");
        Tracker fast;
        for (int i = 0; i < 400; ++i) NoteFrame(fast, int64_t(i) * 10000000 / 400, 10000000);
        Tracker vsync240;
        for (int i = 0; i < 400; ++i) NoteFrame(vsync240, int64_t(i) * 10000000 / 240, 10000000);
        Tracker afterMenu;
        NoteFrame(afterMenu, 0, 10000000);
        NoteFrame(afterMenu, 30000000, 10000000);
        ok &= Check(!UseHfrInterval(fast) && UseHfrInterval(vsync240) &&
                        std::fabs(afterMenu.frameSeconds - kMaxFrameSampleSeconds) < 1e-12,
                    "400 FPS keeps the title's interval, 240 FPS sends every frame, pauses count as 60 Hz");
    }

    // --- Model: the hook against the title's emitter.
    for (double speed : {40.0, 400.0, 4000.0}) {  // argument per second (slow to fast turns)
        for (double fps : {30.0, 50.0, 60.0}) {
            const Stream title = TitleStream(fps, 0.02, 20.0, float(speed), 1.37f);
            const Stream hook = HookStream(fps, 0.02, 20.0, float(speed), 1.37f);
            ok &= Check(title.units == hook.units, "identical command stream at " +
                                                      std::to_string(int(fps)) + " FPS, speed " +
                                                      std::to_string(int(speed)));
        }
        {
            const Stream title = TitleStream(400.0, 0.02, 20.0, float(speed), 1.37f);
            const Stream hook = HookStream(400.0, 0.02, 20.0, float(speed), 1.37f);
            ok &= Check(title.units == hook.units,
                        "identical command stream at 400 FPS, speed " + std::to_string(int(speed)));
        }
        for (double fps : {120.0, 144.0, 165.0, 240.0}) {
            uint64_t cadence = 0;
            const Stream title = TitleStream(fps, 0.05, 20.0, float(speed), 1.37f);
            const Stream hook = HookStream(fps, 0.05, 20.0, float(speed), 1.37f, &cadence);
            const size_t frames = size_t(20.0 * fps);
            const std::string at = std::to_string(int(fps)) + " FPS, speed " + std::to_string(int(speed));
            ok &= Check(hook.units.size() + 20 >= frames, "a command on every frame at " + at);
            ok &= Check(title.units.size() + 20 < frames, "the title skips frames at " + at);
            ok &= Check(cadence + 2 >= title.units.size() && cadence <= title.units.size() + 2,
                        "cadence points follow the title's sends at " + at);
            ok &= Check(WorstWindowDifference(title, hook) <= 1,
                        "the title's turn reaches the title by each of its sends at " + at);
        }
    }
    if (ok) std::cout << "runtime_stick_look: ok\n";
    return ok ? 0 : 1;
}
