#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace darkness::xbox_path {

inline wchar_t FoldAsciiCase(wchar_t value) noexcept {
  return value >= L'A' && value <= L'Z' ? value + (L'a' - L'A') : value;
}

inline bool EqualsAsciiCaseInsensitive(std::wstring_view left,
                                       std::wstring_view right) noexcept {
  if (left.size() != right.size()) return false;
  for (size_t index = 0; index < left.size(); ++index) {
    if (FoldAsciiCase(left[index]) != FoldAsciiCase(right[index])) return false;
  }
  return true;
}

struct ComponentMatch {
  size_t index{};
  bool found{};
  bool ambiguous{};
};

// Xbox paths are case-insensitive, while a Proton-backed host directory may
// be case-sensitive. A case-colliding host directory cannot be represented by
// the Xbox namespace, so fail closed rather than selecting by enumeration
// order. The extracted title corpus is ASCII-only; non-ASCII code units retain
// exact comparison until a verified title path requires broader folding.
inline ComponentMatch SelectUniqueComponent(
    const std::vector<std::wstring>& candidates,
    std::wstring_view requested) noexcept {
  ComponentMatch result{};
  for (size_t index = 0; index < candidates.size(); ++index) {
    if (!EqualsAsciiCaseInsensitive(candidates[index], requested)) continue;
    if (result.found) {
      result.ambiguous = true;
      return result;
    }
    result.index = index;
    result.found = true;
  }
  return result;
}

}  // namespace darkness::xbox_path
