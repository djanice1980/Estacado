#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

std::string ReadText(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream output;
  output << input.rdbuf();
  return output.str();
}

int Fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

}  // namespace

int main() {
  const std::string root = DARKNESS_SOURCE_ROOT;
  const std::string audio = ReadText(root + "/runtime/runtime_audio.cpp");
  const std::string main_source = ReadText(root + "/runtime/main.cpp");
  const std::string launcher = ReadText(
      root + "/scripts/start-pc-phase1-validation-probe.ps1");

  if (audio.find("std::atomic<bool> runtimeAudioDiagnosticsEnabled{}") ==
      std::string::npos) {
    return Fail("audio diagnostics must be disabled by default");
  }
  if (audio.find("const bool recordDiagnostics = RuntimeAudioDiagnosticsEnabled();") ==
          std::string::npos ||
      audio.find("firstNonSilentPcm = recordDiagnostics &&") ==
          std::string::npos ||
      audio.find("if (RuntimeAudioDiagnosticsEnabled()) {\n"
                 "            const auto completionTime") ==
          std::string::npos ||
      audio.find("const bool recordDiagnostics = RuntimeAudioDiagnosticsEnabled();\n"
                 "        uint64_t callbackOrdinal") ==
          std::string::npos) {
    return Fail("PCM capture and callback/device timing must use the opt-in gate");
  }
  const size_t conversion = audio.find("RuntimeAudioConvertFrameToStereoPcm16(");
  const size_t submission = audio.find("writeResult = writer(");
  if (conversion == std::string::npos || submission == std::string::npos ||
      conversion > submission) {
    return Fail("audio diagnostics gating must not bypass conversion or playback");
  }
  const size_t volume_read = audio.find(
      "const uint32_t masterVolume = RuntimeAudioMasterVolumeQ16();");
  const size_t volume_guard = audio.find(
      "if (masterVolume != kRuntimeAudioMasterVolumeOneQ16)", volume_read);
  const size_t volume_apply = audio.find(
      "RuntimeAudioScaleHostPcm16(buffer.samples.data()", volume_guard);
  const size_t identity_return = audio.find(
      "if (!samples || scaleQ16 >= kRuntimeAudioMasterVolumeOneQ16) return;");
  const size_t scale_loop = audio.find(
      "for (size_t index = 0; index < sampleCount; ++index)", identity_return);
  if (volume_read == std::string::npos || volume_guard == std::string::npos ||
      volume_apply == std::string::npos || identity_return == std::string::npos ||
      scale_loop == std::string::npos || volume_read < conversion ||
      volume_apply > submission || identity_return > scale_loop) {
    return Fail("identity master volume must bypass host PCM sample scaling");
  }
  if (main_source.find(
          "ConfigureRuntimeAudioDiagnostics(launchOptions.audioDiagnostics)") ==
          std::string::npos ||
      main_source.find(
          "ConfigureRuntimeAudioMasterVolume(\n"
          "            RuntimeAudioMasterVolumeFromPcConfig(") ==
          std::string::npos ||
      launcher.find("--audio-diagnostics") == std::string::npos) {
    return Fail("audio diagnostics and startup-only output volume must be explicitly wired");
  }

  std::cout << "Runtime audio diagnostics policy regression passed\n";
  return 0;
}
