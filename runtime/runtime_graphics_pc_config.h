#pragma once

#include "runtime_pc_config_snapshot.h"

// Owned by the graphics adapter before guest execution starts. The selected
// bytes and resource origin survive until deferred GPU creation (and shutdown).
class RuntimeGraphicsPcConfig {
public:
    RuntimeGraphicsPcConfig(const RuntimePcConfigSnapshot& snapshot,
                            const std::filesystem::path& assetRoot)
        : snapshot_(snapshot), originUtf8_(snapshot.origin().u8string()),
          assetRootUtf8_(assetRoot.u8string()) {
        if (snapshot.origin().empty() || assetRoot.empty() || !assetRoot.is_absolute()) {
            throw std::runtime_error(
                "PC graphics configuration or package asset root is invalid");
        }
    }
    const RuntimePcConfigSnapshot& snapshot() const noexcept { return snapshot_; }
    const std::string& originUtf8() const noexcept { return originUtf8_; }
    const std::string& assetRootUtf8() const noexcept { return assetRootUtf8_; }
private:
    RuntimePcConfigSnapshot snapshot_;
    std::string originUtf8_;
    std::string assetRootUtf8_;
};
