// Keyboard button prompts (0.9.1): device tracking, activity rules, the
// keyboard wording of the title's controller prompt strings and the GPU
// configuration key.

#include "runtime_button_prompts.h"

#include <cstdint>
#include <cstdio>
#include <string>

// The plugin side is not loaded in this test.
static uint32_t g_noted_device = 0;
void RuntimeGraphicsNoteInputDevice(uint32_t device) noexcept { g_noted_device = device; }
uint32_t RuntimeGraphicsPromptLabels(char* buffer, uint32_t size) noexcept {
    if (buffer && size) buffer[0] = '\0';
    return 0;
}

namespace {
int failures = 0;
void Check(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}
}  // namespace

int main() {
    RuntimePromptDeviceTracker tracker;
    Check(tracker.Current() == 0, "no device before any input");
    Check(tracker.Observe(false, false) == 0, "an idle poll reports nothing");
    Check(tracker.Observe(false, true) == 2, "keyboard input reports the keyboard");
    Check(tracker.Observe(false, true) == 0, "the same device is reported once");
    Check(tracker.Observe(true, true) == 0, "both in one poll keep the current device");
    Check(tracker.Observe(true, false) == 1, "controller input reports the controller");
    Check(tracker.Current() == 1, "the controller is current");

    Check(!RuntimeControllerActivity(0x0010, 0x0010, 0, 0, 3000, -3000, 0, 0),
          "a held button and a resting stick are not activity");
    Check(RuntimeControllerActivity(0x1010, 0x0010, 0, 0, 0, 0, 0, 0),
          "a newly pressed button is activity");
    Check(RuntimeControllerActivity(0, 0, 0, 200, 0, 0, 0, 0), "a pulled trigger is activity");
    Check(!RuntimeControllerActivity(0, 0, 40, 0, 0, 0, 0, 0), "a worn trigger is not activity");
    Check(RuntimeControllerActivity(0, 0, 0, 0, 0, 0, -20000, 0), "a pushed stick is activity");
    Check(!RuntimeKeyboardMouseActivity(0, 0, 0, 0, 0, 0, 0), "idle keys are not activity");
    Check(RuntimeKeyboardMouseActivity(0, 0, 0, 0, 32767, 0, 0), "a movement key is activity");
    Check(RuntimeKeyboardMouseActivity(0, 255, 0, 0, 0, 0, 0), "a mouse button is activity");

    RuntimeNoteInputActivity(false, true);
    Check(g_noted_device == 2, "the plugin hears about the keyboard");
    g_noted_device = 0;
    RuntimeNoteInputActivity(false, true);
    Check(g_noted_device == 0, "and only once");

    const std::string labels = "a=E\nb=Shift\ny=Space\nlstick_press=C\nright_trigger=LMB\n"
                               "dpad_up=Up\nx=\n";
    const auto press = RuntimeKeyboardPromptTextFor("CONTROLLER_XBOX_BUTTON_1_A", labels);
    Check(press && *press == u"Press \u00A7p0", "the button template reads like the PC one");
    const auto hold = RuntimeKeyboardPromptTextFor("CONTROLLER_XBOX_BUTTON_2_C2", labels);
    Check(hold && *hold == u"holding \u00A7p0", "hold forms keep their verb");
    const auto pull = RuntimeKeyboardPromptTextFor("CONTROLLER_XBOX_TRIGGER_B", labels);
    Check(pull && *pull == u"press \u00A7p0", "triggers are pressed keys");
    const auto a = RuntimeKeyboardPromptTextFor("CONTROLLER_XBOX_00", labels);
    Check(a && *a == u"E", "the A button reads as its key");
    const auto jump = RuntimeKeyboardPromptTextFor("CONTROLLER_XBOX_03", labels);
    Check(jump && *jump == u"Space", "the Y button reads as Space");
    const auto click = RuntimeKeyboardPromptTextFor("CONTROLLER_XBOX_06", labels);
    Check(click && *click == u"C", "the left stick click reads as its key");
    const auto fire = RuntimeKeyboardPromptTextFor("CONTROLLER_XBOX_0B", labels);
    Check(fire && *fire == u"LMB", "the right trigger reads as the mouse button");
    const auto up = RuntimeKeyboardPromptTextFor("CONTROLLER_XBOX_10", labels);
    Check(up && *up == u"Up", "the D-pad reads as its key");
    Check(!RuntimeKeyboardPromptTextFor("CONTROLLER_XBOX_02", labels),
          "an unbound button keeps the title's name");
    Check(!RuntimeKeyboardPromptTextFor("CONTROLLER_XBOX_THUMBSTICK_A", labels),
          "stick movement keeps the title's text");
    Check(!RuntimeKeyboardPromptTextFor("MENU_RESUME", labels), "other strings are untouched");
    Check(!RuntimeKeyboardPromptText("CONTROLLER_XBOX_00"),
          "nothing changes while the plugin wants Xbox prompts");

    const std::string config = "pc_config_version = 1\n[input]\n";
    const std::string with = RuntimePcConfigWithPromptIconSource(
        config, std::filesystem::path(u8"C:\\Games\\Dark \"ness\"\\Content\\Textures\\GUI.xtc"));
    Check(with.rfind("gpu_prompt_icon_source = \"C:\\\\Games\\\\Dark \\\"ness\\\"\\\\Content"
                     "\\\\Textures\\\\GUI.xtc\"\n",
                     0) == 0 &&
              with.find(config) != std::string::npos,
          "the icon source is a top-level escaped TOML string");
    Check(RuntimePcConfigWithPromptIconSource(config, {}) == config,
          "no source leaves the configuration unchanged");
    const std::string bom = "\xEF\xBB\xBFpc_config_version = 1\n";
    Check(RuntimePcConfigWithPromptIconSource(bom, "G.xtc").rfind("\xEF\xBB\xBFgpu_prompt", 0) == 0,
          "the key goes after a byte order mark");

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("runtime_button_prompts: all checks passed\n");
    return 0;
}
