#include "runtime_owned_camera_mode.h"
#include <iostream>
int main() {
    bool ok = true;
    RuntimeOwnedCameraMode disabled, enabled, late, missing;
    ok &= disabled.ConfigureAtStartup(false, false) && !disabled.Enabled();
    ok &= !disabled.ConfigureAtStartup(true, true);
    ok &= enabled.ConfigureAtStartup(true, true) && enabled.Enabled();
    ok &= enabled.ConfigureAtStartup(true, true) && !enabled.ConfigureAtStartup(false, true);
    ok &= !late.Enabled() && !late.ConfigureAtStartup(true, true);
    ok &= !missing.ConfigureAtStartup(true, false) && !missing.Enabled();
    for (unsigned i = 0; i < 10000; ++i) ok &= enabled.Enabled() && !disabled.Enabled();
    RuntimeOwnedCameraFollowup followup;
    ok &= !followup.Active() && !followup.Tick() && followup.Ticks() == 0;
    followup.Arm();
    for (unsigned tick = 1; tick <= 256; ++tick) {
        if (tick == 32) followup.Arm(); // Cannot restart an active interval.
        ok &= followup.Tick() == (tick == 16 || tick == 64 || tick == 256);
        ok &= followup.Ticks() == tick && followup.Active() == (tick < 256);
    }
    followup.Arm();
    for (unsigned i = 0; i < 1000; ++i) ok &= !followup.Tick();
    ok &= !followup.Active() && followup.Ticks() == 256;
    std::cout << (ok ? "owned camera mode PASS\n" : "owned camera mode FAIL\n");
    return ok ? 0 : 1;
}
