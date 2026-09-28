#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main() {
  const std::string root = DARKNESS_SOURCE_ROOT;
  std::ifstream source_file(
      root + "/external/ReXGlue/src/graphics/plugin_main.cpp",
      std::ios::binary);
  const std::string source((std::istreambuf_iterator<char>(source_file)),
                           std::istreambuf_iterator<char>());
  if (source.empty()) {
    std::cerr << "embedded GPU source was not readable\n";
    return 1;
  }

  const size_t resolve_pack =
      source.find("ResolveEmbeddedShaderReplacementPackPath(");
  const size_t configure_pack =
      source.find("ConfigureShaderReplacementPack(replacement_pack",
                  resolve_pack);
  const size_t hash_pack =
      source.find("rex::crypto::sha256_file(replacement_pack)",
                  configure_pack);
  const size_t host_root = source.find(
      "const std::filesystem::path host_cache_root", hash_pack);
  const size_t host_leaf = source.find(
      "host_cache_root.filename().u8string()", host_root);
  const size_t backend = source.find("effective_identity += backend", host_leaf);
  const size_t replacement = source.find(
      "effective_identity += replacement_pack_identity", backend);
  const size_t effective_hash = source.find(
      "rex::crypto::sha256(effective_identity)", replacement);
  const size_t effective_root = source.find(
      "host_cache_root.parent_path() / effective_leaf", effective_hash);
  const size_t initialize = source.find(
      "InitializeShaderStorage(\n        cache_root", effective_root);
  const size_t report = source.find("REX_GRAPHICS_CACHE_EFFECTIVE", initialize);

  const bool passed = resolve_pack != std::string::npos &&
                      configure_pack != std::string::npos &&
                      hash_pack != std::string::npos &&
                      host_root != std::string::npos &&
                      host_leaf != std::string::npos &&
                      backend != std::string::npos &&
                      replacement != std::string::npos &&
                      effective_hash != std::string::npos &&
                      effective_root != std::string::npos &&
                      initialize != std::string::npos &&
                      report != std::string::npos &&
                      resolve_pack < configure_pack &&
                      configure_pack < hash_pack && hash_pack < host_root &&
                      host_root < host_leaf && host_leaf < backend &&
                      backend < replacement && replacement < effective_hash &&
                      effective_hash < effective_root &&
                      effective_root < initialize && initialize < report;
  if (!passed) {
    std::cerr << "effective GPU cache identity no longer includes the resolved replacement pack and backend\n";
    return 1;
  }

  std::cout << "GPU graphics cache identity policy passed\n";
  return 0;
}
