// V308: at a scaled draw resolution, a texture binding change takes the guest
// extents for the scaled-resolve check from an existing texture of either
// variant (the guest layout does not depend on scaled_resolve) instead of
// recomputing the full mip layout, and checks both ranges under one lock.
// Source policy only: the texture cache needs a device to run.
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main() {
  bool passed = true;
  const auto check = [&passed](bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      passed = false;
    }
  };
  std::ifstream file(std::string(REXGLUE_SOURCE_ROOT) + "/src/graphics/pipeline/texture/cache.cpp");
  const std::string source((std::istreambuf_iterator<char>(file)), {});
  const auto find_or_create =
      source.find("TextureCache::Texture* TextureCache::FindOrCreateTexture(TextureKey key) {");
  const auto scaled_branch = source.find(
      "if (IsDrawResolutionScaled() && key.tiled && IsScaledResolveSupportedForFormat(key)) {",
      find_or_create);
  const auto lookup = source.find("auto unscaled_it = textures_.find(key);", scaled_branch);
  const auto layout = source.find("key.GetGuestLayout();", scaled_branch);
  const auto host_checks = source.find("uint32_t max_host_width_height", scaled_branch);
  check(find_or_create != std::string::npos && scaled_branch != std::string::npos &&
            lookup != std::string::npos && layout != std::string::npos && lookup < layout &&
            layout < host_checks,
        "existing textures are looked up before the guest layout is computed");
  check(source.find("unscaled_texture->GetGuestBaseSize()", scaled_branch) < host_checks &&
            source.find("scaled_it->second->GetGuestMipsSize()", scaled_branch) < host_checks,
        "extents come from the stored texture layout");
  const auto one_lock = source.find("auto global_lock = global_critical_region_.Acquire();", scaled_branch);
  const auto locked_base = source.find("IsRangeScaledResolvedLocked(key.base_page << 12", scaled_branch);
  const auto locked_mips = source.find("IsRangeScaledResolvedLocked(key.mip_page << 12", scaled_branch);
  check(one_lock < locked_base && locked_base < locked_mips && locked_mips < host_checks,
        "base and mips ranges are checked under one lock");
  const auto public_check = source.find(
      "bool TextureCache::IsRangeScaledResolved(uint32_t start_unscaled, uint32_t length_unscaled) {");
  const auto public_lock = source.find("global_critical_region_.Acquire();", public_check);
  const auto locked_def = source.find("bool TextureCache::IsRangeScaledResolvedLocked(");
  check(public_check != std::string::npos && public_lock < locked_def,
        "the public range check still takes the lock itself");
  const auto locked_body_end = source.find("\n}\n", locked_def);
  check(source.find("Acquire()", locked_def) > locked_body_end,
        "the locked variant does not lock again");

  if (!passed) return 1;
  std::cout << "gpu texture scaled lookup: PASS\n";
  return 0;
}
