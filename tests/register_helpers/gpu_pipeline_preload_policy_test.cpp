#include <rex/graphics/d3d12/pipeline_preload_policy.h>
#include <rex/graphics/pipeline_storage_policy.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

bool Check(bool condition, const char* message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

}  // namespace

int main() {
  using rex::graphics::d3d12::pipeline_preload_policy::MustAwaitCompletion;
  using rex::graphics::d3d12::pipeline_preload_policy::ResolveWorkerTarget;
  using rex::graphics::pipeline_storage_policy::HasSelectedWork;
  using rex::graphics::pipeline_storage_policy::MayExitAfterDrain;

  bool passed = true;
  passed &= Check(ResolveWorkerTarget(6, 24, 16) == 15,
                  "blocking preload did not expand to the available CPUs");
  passed &= Check(ResolveWorkerTarget(12, 4, 16) == 12,
                  "preload reduced the normal worker pool");
  passed &= Check(ResolveWorkerTarget(6, 0, 16) == 6,
                  "empty preload changed the worker pool");
  passed &= Check(MustAwaitCompletion(true, 1),
                  "blocking preload may return with a busy worker");
  passed &= Check(!MustAwaitCompletion(true, 0),
                  "idle blocking preload requested a needless wait");
  passed &= Check(!MustAwaitCompletion(false, 3),
                  "nonblocking preload was made synchronous");
  passed &= Check(!MayExitAfterDrain(true,
                                    HasSelectedWork(true, false, false, false)),
                  "shutdown discarded a queued shader");
  passed &= Check(!MayExitAfterDrain(true,
                                    HasSelectedWork(false, true, false, false)),
                  "shutdown discarded a queued pipeline");
  passed &= Check(!MayExitAfterDrain(true,
                                    HasSelectedWork(false, false, true, false)),
                  "shutdown discarded a shader flush request");
  passed &= Check(!MayExitAfterDrain(true,
                                    HasSelectedWork(false, false, false, true)),
                  "shutdown discarded a pipeline flush request");
  passed &= Check(MayExitAfterDrain(
                      true, HasSelectedWork(false, false, false, false)),
                  "drained shutdown did not permit writer exit");
  passed &= Check(!MayExitAfterDrain(
                      false, HasSelectedWork(false, false, false, false)),
                  "idle live writer exited without a shutdown request");

  const std::string source_path = std::string(REXGLUE_SOURCE_ROOT) +
      "/src/graphics/d3d12/pipeline_cache.cpp";
  std::ifstream source_file(source_path, std::ios::binary);
  const std::string source((std::istreambuf_iterator<char>(source_file)),
                           std::istreambuf_iterator<char>());
  passed &= Check(!source.empty(), "pipeline cache source was not readable");
  passed &= Check(source.find(
      "creation_threads_.size() < creation_thread_needed_count") !=
          std::string::npos,
      "computed preload worker target is not used");
  passed &= Check(source.find(
      "MustAwaitCompletion(\n                blocking, creation_threads_busy_)") !=
          std::string::npos,
      "blocking preload no longer checks all busy workers");
  passed &= Check(source.find(
      "MayExitAfterDrain(\n              storage_write_thread_shutdown_, "
      "writer_has_selected_work)") != std::string::npos,
      "persistent writer no longer drains selected work before shutdown");
  passed &= Check(source.find("if (!writer_has_selected_work)") !=
                      std::string::npos,
                  "flush-only storage work may sleep before being executed");

  const std::string vulkan_source_path = std::string(REXGLUE_SOURCE_ROOT) +
      "/src/graphics/vulkan/pipeline_cache.cpp";
  std::ifstream vulkan_source_file(vulkan_source_path, std::ios::binary);
  const std::string vulkan_source(
      (std::istreambuf_iterator<char>(vulkan_source_file)),
      std::istreambuf_iterator<char>());
  passed &= Check(!vulkan_source.empty(),
                  "Vulkan pipeline cache source was not readable");
  passed &= Check(vulkan_source.find(
      "MayExitAfterDrain(\n              storage_write_thread_shutdown_, "
      "writer_has_selected_work)") != std::string::npos,
      "Vulkan persistent writer no longer drains selected work before shutdown");
  passed &= Check(vulkan_source.find("if (!writer_has_selected_work)") !=
                      std::string::npos,
                  "Vulkan flush-only storage work may sleep before execution");

  if (passed) {
    std::cout << "Cross-backend persistent-pipeline storage policy passed\n";
  }
  return passed ? 0 : 1;
}
