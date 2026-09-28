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
  const std::string source = ReadText(
      std::string(DARKNESS_SOURCE_ROOT) +
      "/external/ReXGlue/src/graphics/d3d12/command_processor.cpp");
  const std::string pipeline = ReadText(
      std::string(DARKNESS_SOURCE_ROOT) +
      "/external/ReXGlue/src/graphics/d3d12/pipeline_cache.cpp");
  const std::string command_processor_header = ReadText(
      std::string(DARKNESS_SOURCE_ROOT) +
      "/external/ReXGlue/include/rex/graphics/d3d12/command_processor.h");
  const std::string deferred_header = ReadText(
      std::string(DARKNESS_SOURCE_ROOT) +
      "/external/ReXGlue/include/rex/graphics/d3d12/deferred_command_list.h");
  const std::string deferred_source = ReadText(
      std::string(DARKNESS_SOURCE_ROOT) +
      "/external/ReXGlue/src/graphics/d3d12/deferred_command_list.cpp");

  const size_t cvar = source.find(
      "embedded_scene_vertex_output_trace, false, \"GPU/Diagnostics\"");
  if (cvar == std::string::npos ||
      source.find("Lifecycle::kInitOnly", cvar) == std::string::npos) {
    return Fail("vertex output tracing must be opt-in and init-only");
  }
  if (source.find("constexpr uint32_t kMaximumIndexOrdinals = 128") ==
          std::string::npos ||
      source.find("constexpr uint32_t kMaximumUniqueVertices = 24") ==
          std::string::npos ||
      source.find(
          "constexpr uint32_t kMaximumFullHashIndexOrdinals = 16384") ==
          std::string::npos ||
      source.find(
          "constexpr uint32_t kMaximumFullHashUniqueVertices = 8192") ==
          std::string::npos ||
      source.find("embedded_scene_vertex_output_trace_count < 8") ==
          std::string::npos) {
    return Fail("vertex output tracing must remain bounded");
  }
  if (source.find("ShaderInterpreter interpreter(regs, memory)") ==
          std::string::npos ||
      source.find("REX_EMBEDDED_SCENE_VERTEX_OUTPUT_BEGIN") ==
          std::string::npos ||
      source.find("REX_EMBEDDED_SCENE_VERTEX_OUTPUT_FULL") ==
          std::string::npos ||
      source.find("REX_EMBEDDED_SCENE_VERTEX_OUTPUT_END") ==
          std::string::npos ||
      source.find("IsCurrentEmbeddedGameplayCaptureFrame()") ==
          std::string::npos) {
    return Fail("trace must observe real guest vertices only in a bounded capture frame");
  }
  if (source.find("0x52472DD3CF459B83") == std::string::npos ||
      source.find("0x818A2B33A7AB4ECE") == std::string::npos ||
      source.find("0xBE763931E2AB7D56") == std::string::npos) {
    return Fail("trace must remain keyed to the verified prepass/shaded pair");
  }
  if (source.find("guest_screen=% .9g,% .9g,% .9g") ==
          std::string::npos ||
      source.find("subpixel16=% .9g,% .9g") == std::string::npos ||
      source.find("floor16=%d,%d round16=%d,%d") ==
          std::string::npos) {
    return Fail("trace must expose guest screen and 1/16-subpixel evidence");
  }
  if (source.find("index_hash=0x%016llX clip_hash=0x%016llX") ==
          std::string::npos ||
      source.find("screen_hash=0x%016llX depth_hash=0x%016llX") ==
          std::string::npos ||
      source.find("killed=%u nonfinite=%u outside=%u") ==
          std::string::npos) {
    return Fail("full trace must expose exact bounded vertex-output hashes");
  }

  const size_t host_cvar = source.find(
      "d3d12_embedded_scene_host_vertex_output_diagnostic, false");
  if (host_cvar == std::string::npos ||
      source.find("Lifecycle::kRequiresRestart", host_cvar) ==
          std::string::npos ||
      command_processor_header.find(
          "kEmbeddedSceneHostVertexOutputRecordCount = 8") ==
          std::string::npos ||
      command_processor_header.find(
          "kEmbeddedSceneHostVertexOutputMaximumVertices =") ==
          std::string::npos ||
      command_processor_header.find("16384") == std::string::npos ||
      source.find("IsCurrentEmbeddedGameplayCaptureFrame()") ==
          std::string::npos) {
    return Fail("host SV_Position capture must remain bounded and opt-in");
  }
  if (source.find("D3D12_RESOURCE_STATE_STREAM_OUT") ==
          std::string::npos ||
      source.find("D3DSOSetTargets(0, 1, &output_view)") ==
          std::string::npos ||
      source.find("D3DSOSetTargets(0, 0, nullptr)") ==
          std::string::npos ||
      source.find("REX_EMBEDDED_SCENE_HOST_VERTEX_OUTPUT") ==
          std::string::npos ||
      source.find("filled_bytes=%llu") == std::string::npos ||
      source.find("overflow=%u partial_vertex=%u") ==
          std::string::npos ||
      source.find("AwaitAllQueueOperationsCompletion()") ==
          std::string::npos) {
    return Fail("host SV_Position capture must use real bounded stream output");
  }
  if (pipeline.find("SemanticName = \"SV_Position\"") ==
          std::string::npos ||
      pipeline.find("StreamOutput.RasterizedStream = 0") ==
          std::string::npos ||
      pipeline.find("UINT64_C(0x539FB8DE2DD7715A)") ==
          std::string::npos ||
      pipeline.find("UINT64_C(0x7EF6E3B55D32AEB4)") ==
          std::string::npos ||
      source.find("D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT") ==
          std::string::npos) {
    return Fail("stream-output PSOs must preserve rasterization and exact scope");
  }
  if (deferred_header.find("void D3DSOSetTargets") ==
          std::string::npos ||
      deferred_header.find("kD3DSOSetTargets") == std::string::npos ||
      deferred_source.find("command_list->SOSetTargets") ==
          std::string::npos) {
    return Fail("deferred command list must faithfully carry stream-output bindings");
  }

  std::cout << "scene vertex output trace policy verified\n";
  return 0;
}
