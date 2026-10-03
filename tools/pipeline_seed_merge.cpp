// Builds the packaged pipeline seed as the union of persistent D3D12 shader
// storages, using the same validation the runtime applies when it imports the
// seed (rex/graphics/d3d12/pipeline_storage_seed.h).
//
// usage: pipeline_seed_merge --out DIR --title 545407EE
//            --shader-version 0x20201219 --pipeline-version 0x20260908
//            [--api rtv|rov] SHAREABLE_DIR...
//
// Each SHAREABLE_DIR is a "shaders/shareable" directory of a persistent store.
// Inputs with another format version are skipped and reported. The output
// directory receives <title>.xsh, <title>.<api>.d3d12.xpso and the shader
// index <title>.xshi (no microcode; the only shader file a public package
// may carry, with the .xpso).
//
// Build (from the repository root):
//   clang-cl /nologo /O2 /EHsc /std:c++20 /W3 /I external/ReXGlue/include
//   /I external/ReXGlue/thirdparty/xxHash tools/pipeline_seed_merge.cpp
//   /Fe:build/tools/pipeline_seed_merge.exe /Fo:build/tools/
#include <rex/graphics/d3d12/pipeline_storage_seed.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace seed = rex::graphics::d3d12::pipeline_storage_seed;

int main(int argc, char** argv) {
  std::filesystem::path out;
  std::string title;
  std::string api = "rtv";
  uint32_t shader_version = 0;
  uint32_t pipeline_version = 0;
  std::vector<std::filesystem::path> inputs;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--out" && has_value) {
      out = argv[++i];
    } else if (arg == "--title" && has_value) {
      title = argv[++i];
    } else if (arg == "--api" && has_value) {
      api = argv[++i];
    } else if (arg == "--shader-version" && has_value) {
      shader_version = uint32_t(std::strtoul(argv[++i], nullptr, 0));
    } else if (arg == "--pipeline-version" && has_value) {
      pipeline_version = uint32_t(std::strtoul(argv[++i], nullptr, 0));
    } else {
      inputs.emplace_back(arg);
    }
  }
  if (out.empty() || title.size() != 8 || !shader_version || !pipeline_version ||
      (api != "rtv" && api != "rov") || inputs.empty()) {
    std::fprintf(stderr,
                 "usage: pipeline_seed_merge --out DIR --title XXXXXXXX --shader-version V "
                 "--pipeline-version V [--api rtv|rov] SHAREABLE_DIR...\n");
    return 2;
  }
  std::error_code error;
  std::filesystem::create_directories(out, error);
  const std::string shader_name = title + ".xsh";
  const std::string pipeline_name = title + "." + api + ".d3d12.xpso";
  const uint32_t api_magic = api == "rov" ? seed::kPipelineApiRov : seed::kPipelineApiRtv;

  size_t used = 0;
  for (const auto& input : inputs) {
    const auto shaders = seed::MergeShaders(input / shader_name, out / shader_name, shader_version);
    const auto pipelines = seed::MergePipelines(input / pipeline_name, out / pipeline_name,
                                                api_magic, pipeline_version);
    if (shaders.write_failed || pipelines.write_failed) {
      std::fprintf(stderr, "write failed while merging %s\n", input.string().c_str());
      return 1;
    }
    used += (shaders.seed_valid && pipelines.seed_valid) ? 1 : 0;
    std::printf("input=%s shaders_valid=%d shaders=%zu shaders_added=%zu pipelines_valid=%d "
                "pipelines=%zu pipelines_added=%zu\n",
                input.string().c_str(), shaders.seed_valid ? 1 : 0, shaders.seed_records,
                shaders.added, pipelines.seed_valid ? 1 : 0, pipelines.seed_records,
                pipelines.added);
  }

  std::vector<uint8_t> data;
  std::vector<seed::Record> records;
  seed::ReadWholeFile(out / shader_name, data);
  const bool shaders_ok = seed::ParseShaders(data, shader_version, records) == data.size();
  const size_t shader_count = records.size();
  // The shader index (<title>.xshi): hashes and sizes only, no microcode, so
  // packages without the microcode can still prewarm (pipeline_storage_seed.h).
  const std::vector<uint8_t> index = seed::BuildShaderIndex(data, shader_version);
  const bool index_ok = !index.empty() && seed::WriteWholeFile(out / (title + ".xshi"), index);
  seed::ReadWholeFile(out / pipeline_name, data);
  const bool pipelines_ok =
      seed::ParsePipelines(data, api_magic, pipeline_version, records) == data.size();
  std::printf("seed=%s inputs_used=%zu shaders=%zu indexed=%zu pipelines=%zu complete=%d\n",
              out.string().c_str(), used, shader_count,
              index.size() > 8 ? (index.size() - 8) / seed::kShaderIndexRecordSize : size_t(0),
              records.size(), (shaders_ok && pipelines_ok && index_ok) ? 1 : 0);
  return shaders_ok && pipelines_ok && index_ok ? 0 : 1;
}
