// darkness_game_setup: the launcher's game setup from the command line (the
// build-from-source script uses it before anything else is built).
//
//   darkness_game_setup check <folder>             default.xex version check
//   darkness_game_setup inspect <disc image>       files, size, version
//   darkness_game_setup extract <disc image> <dir> check, then extract
//
// Exit code 0 = supported game (and extracted), 1 = anything else.
#include "runtime_game_setup.h"

#include <cstdio>
#include <string>

namespace setup = darkness::game_setup;

namespace {

int CheckXex(const std::string& sha256) {
    if (sha256.empty()) {
        std::fprintf(stderr, "no readable default.xex\n");
        return 1;
    }
    if (sha256 != setup::kSupportedXexSha256) {
        std::fprintf(stderr,
                     "default.xex SHA-256 %s is not the supported version (The Darkness, Xbox 360, "
                     "USA/Europe disc, title 545407EE)\n",
                     sha256.c_str());
        return 1;
    }
    std::printf("default.xex: supported version\n");
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: darkness_game_setup check <folder> | inspect <disc image> | "
                     "extract <disc image> <folder>\n");
        return 1;
    }
    const std::wstring command = argv[1];
    const std::filesystem::path source = argv[2];
    if (command == L"check") {
        return CheckXex(setup::CheckGameFolder(source).sha256);
    }
    const setup::DiscImageInfo info = setup::InspectDiscImage(source);
    if (!info.ok) {
        std::fprintf(stderr, "disc image: %s\n", info.error.c_str());
        return 1;
    }
    std::printf("disc image: %llu files, %llu MB\n", static_cast<unsigned long long>(info.files),
                static_cast<unsigned long long>(info.bytes >> 20));
    if (CheckXex(info.xexSha256) != 0) return 1;
    if (command == L"inspect") return 0;
    if (command != L"extract" || argc < 4) {
        std::fprintf(stderr, "extract needs a destination folder\n");
        return 1;
    }
    const std::filesystem::path destination = argv[3];
    const std::filesystem::path partial = destination.wstring() + L".partial";
    std::error_code error;
    if (std::filesystem::exists(destination, error) || std::filesystem::exists(partial, error)) {
        std::fprintf(stderr, "%s or its .partial folder already exists\n",
                     destination.u8string().c_str());
        return 1;
    }
    int lastPercent = -1;
    std::string extractError;
    const bool ok = setup::ExtractDiscImage(
        source, partial,
        [&lastPercent](uint64_t done, uint64_t total) {
            const int percent = total ? int(done * 100 / total) : 100;
            if (percent != lastPercent) {
                lastPercent = percent;
                std::printf("\rextracting: %3d%%", percent);
                std::fflush(stdout);
            }
            return true;
        },
        &extractError);
    std::printf("\n");
    if (!ok) {
        std::fprintf(stderr, "extraction failed: %s\n", extractError.c_str());
        return 1;
    }
    std::filesystem::rename(partial, destination, error);
    if (error) {
        std::fprintf(stderr, "unable to rename the extracted folder: %s\n", error.message().c_str());
        return 1;
    }
    std::printf("extracted to %s\n", destination.u8string().c_str());
    return 0;
}
