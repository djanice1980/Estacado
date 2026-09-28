// V292 deferred presenter completion: the command processor no longer drains
// the async submission worker at every swap. The presenter's post-refresh
// steps (refresher fence signal, mailbox publication, immediate paint) run as
// an ordered worker task after the frame's command lists, and the next refresh
// waits for the previous completion. These source-level checks keep the
// ordering contract from being lost in later edits.
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {
std::string Read(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}
}  // namespace

int main() {
  const std::string root = REXGLUE_SOURCE_ROOT;
  const std::string cp = Read(root + "/src/graphics/d3d12/command_processor.cpp");
  const std::string presenter = Read(root + "/src/ui/presenter.cpp");
  const std::string d3d12_presenter = Read(root + "/src/ui/d3d12/d3d12_presenter.cpp");
  bool passed = !cp.empty() && !presenter.empty() && !d3d12_presenter.empty();
  const auto require = [&passed](bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      passed = false;
    }
  };
  require(cp.find("if (!presenter_completion_deferred_) {\n          DrainSubmissions();") !=
              std::string::npos,
          "the swap refresher drains only when the presenter completion is not deferred");
  require(cp.find("EnqueueSubmissionTask(std::move(task))") != std::string::npos,
          "the presenter completion executor queues ordered worker tasks");
  require(cp.find("if (!job.allocator) {") != std::string::npos &&
              cp.find("job.task();") != std::string::npos,
          "the submission worker runs task jobs in queue order");
  require(cp.find("SetGuestOutputCompletionExecutor(nullptr)") != std::string::npos,
          "shutdown drains pending completions and returns to inline completion");
  const size_t refresh = presenter.find("bool Presenter::RefreshGuestOutput(");
  require(refresh != std::string::npos &&
              presenter.find("AwaitGuestOutputCompletion();", refresh) <
                  presenter.find("guest_output_mailbox_writable_", refresh),
          "a refresh waits for the previous completion before using the writable image");
  const size_t complete = d3d12_presenter.find("void D3D12Presenter::CompleteGuestOutputRefreshImpl");
  require(complete != std::string::npos &&
              d3d12_presenter.find("NextSubmission();", complete) != std::string::npos,
          "the D3D12 refresher fence is signaled in the completion step");
  const size_t impl = d3d12_presenter.find("bool D3D12Presenter::RefreshGuestOutputImpl(");
  require(impl != std::string::npos &&
              d3d12_presenter.find("NextSubmission", impl) > complete,
          "RefreshGuestOutputImpl itself no longer signals before the command lists are queued");
  if (passed) std::cout << "Presenter completion policy passed\n";
  return passed ? 0 : 1;
}
