// V290 packaged pipeline seed (rex/graphics/d3d12/pipeline_storage_seed.h):
// seed records are validated exactly like the loader validates the store,
// appended only when missing, never remove store records, and a mismatched
// seed format is ignored.
#include <rex/graphics/d3d12/pipeline_storage_seed.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

namespace seed = rex::graphics::d3d12::pipeline_storage_seed;

namespace {
constexpr uint32_t kShaderVersion = 0x20201219;
constexpr uint32_t kPipelineVersion = 0x20260908;

void AppendU64(std::vector<uint8_t>& out, uint64_t value) {
  uint8_t bytes[8];
  std::memcpy(bytes, &value, sizeof(bytes));
  out.insert(out.end(), bytes, bytes + sizeof(bytes));
}

// One shader record: {XXH3 of ucode, dword count | type << 31, ucode}.
std::vector<uint8_t> ShaderRecord(uint32_t seed_value, uint32_t dwords, bool pixel) {
  std::vector<uint32_t> ucode(dwords);
  for (uint32_t i = 0; i < dwords; ++i) ucode[i] = seed_value * 2654435761u + i;
  std::vector<uint8_t> record;
  AppendU64(record, XXH3_64bits(ucode.data(), ucode.size() * sizeof(uint32_t)));
  seed::AppendU32(record, dwords | (pixel ? 0x80000000u : 0u));
  const auto* bytes = reinterpret_cast<const uint8_t*>(ucode.data());
  record.insert(record.end(), bytes, bytes + ucode.size() * sizeof(uint32_t));
  return record;
}

std::vector<uint8_t> PipelineRecord(uint8_t fill) {
  std::vector<uint8_t> description(seed::kPipelineRecordSize - 8, fill);
  description[0] = uint8_t(fill ^ 0x5A);
  std::vector<uint8_t> record;
  AppendU64(record, XXH3_64bits(description.data(), description.size()));
  record.insert(record.end(), description.begin(), description.end());
  return record;
}

std::vector<uint8_t> Concat(std::vector<uint8_t> head,
                            const std::vector<std::vector<uint8_t>>& records) {
  for (const auto& record : records) head.insert(head.end(), record.begin(), record.end());
  return head;
}

bool Write(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
  return seed::WriteWholeFile(path, data);
}

std::vector<uint8_t> Read(const std::filesystem::path& path) {
  std::vector<uint8_t> data;
  seed::ReadWholeFile(path, data);
  return data;
}
}  // namespace

int main() {
  bool passed = true;
  const auto check = [&passed](bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      passed = false;
    }
  };
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "darkness_pipeline_storage_seed_test";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root, error);

  const auto s1 = ShaderRecord(1, 16, false);
  const auto s2 = ShaderRecord(2, 40, true);
  const auto s3 = ShaderRecord(3, 8, true);
  const auto s4 = ShaderRecord(4, 12, false);
  const auto header = seed::ShaderHeader(kShaderVersion);
  const auto seed_xsh = root / "seed.xsh";
  const auto store_xsh = root / "store.xsh";
  Write(seed_xsh, Concat(header, {s1, s2, s3}));

  {
    // Missing store: created with the loader's header and every seed record.
    const auto result = seed::MergeShaders(seed_xsh, store_xsh, kShaderVersion);
    check(result.seed_valid && result.seed_records == 3 && result.added == 3 &&
              result.store_records == 0,
          "a missing store receives every seed record");
    std::vector<seed::Record> records;
    const auto store = Read(store_xsh);
    check(seed::ParseShaders(store, kShaderVersion, records) == store.size() &&
              records.size() == 3,
          "the merged store parses completely, as the loader will read it");
    const auto again = seed::MergeShaders(seed_xsh, store_xsh, kShaderVersion);
    check(again.added == 0 && again.store_records == 3 && Read(store_xsh) == store,
          "a second merge is a no-op and leaves the store untouched");
  }
  {
    // Existing records stay first and in order; only missing ones are added.
    Write(store_xsh, Concat(header, {s4, s2}));
    const auto result = seed::MergeShaders(seed_xsh, store_xsh, kShaderVersion);
    check(result.store_records == 2 && result.added == 2, "only missing shaders are appended");
    check(Read(store_xsh) == Concat(header, {s4, s2, s1, s3}),
          "store records are preserved in order before the seed additions");
  }
  {
    // A corrupt seed record stops the import there, like the loader.
    auto corrupt = s2;
    corrupt.back() ^= 0xFF;
    Write(seed_xsh, Concat(header, {s1, corrupt, s3}));
    std::filesystem::remove(store_xsh, error);
    const auto result = seed::MergeShaders(seed_xsh, store_xsh, kShaderVersion);
    check(result.seed_records == 1 && result.added == 1 &&
              Read(store_xsh) == Concat(header, {s1}),
          "records after a corrupt seed record are not imported");
  }
  {
    // A seed of another format version is ignored and the store untouched.
    Write(seed_xsh, Concat(seed::ShaderHeader(kShaderVersion + 1), {s1, s2}));
    Write(store_xsh, Concat(header, {s4}));
    const auto result = seed::MergeShaders(seed_xsh, store_xsh, kShaderVersion);
    check(!result.seed_valid && result.added == 0 && Read(store_xsh) == Concat(header, {s4}),
          "a mismatched seed version is ignored");
  }
  {
    // A store of an old format is replaced, which the loader does anyway;
    // a damaged tail is dropped exactly where the loader would stop.
    Write(seed_xsh, Concat(header, {s1, s2}));
    Write(store_xsh, Concat(seed::ShaderHeader(kShaderVersion - 1), {s3}));
    auto result = seed::MergeShaders(seed_xsh, store_xsh, kShaderVersion);
    check(result.store_records == 0 && result.added == 2 &&
              Read(store_xsh) == Concat(header, {s1, s2}),
          "an old-format store is replaced by the seed");
    auto damaged = Concat(header, {s3});
    damaged.insert(damaged.end(), {1, 2, 3, 4, 5});
    Write(store_xsh, damaged);
    result = seed::MergeShaders(seed_xsh, store_xsh, kShaderVersion);
    check(result.store_records == 1 && result.added == 2 &&
              Read(store_xsh) == Concat(header, {s3, s1, s2}),
          "a damaged store tail is dropped where the loader stops");
  }
  {
    // A missing seed changes nothing.
    std::filesystem::remove(seed_xsh, error);
    const auto before = Read(store_xsh);
    const auto result = seed::MergeShaders(seed_xsh, store_xsh, kShaderVersion);
    check(!result.seed_valid && result.added == 0 && Read(store_xsh) == before,
          "a missing seed leaves the store untouched");
  }

  // Pipeline descriptions: fixed-size records keyed by their full bytes.
  const auto p1 = PipelineRecord(0x11);
  const auto p2 = PipelineRecord(0x22);
  const auto p3 = PipelineRecord(0x33);
  const auto rtv = seed::PipelineHeader(seed::kPipelineApiRtv, kPipelineVersion);
  const auto seed_xpso = root / "seed.xpso";
  const auto store_xpso = root / "store.xpso";
  Write(seed_xpso, Concat(rtv, {p1, p2, p2, p3}));
  Write(store_xpso, Concat(rtv, {p2}));
  {
    const auto result =
        seed::MergePipelines(seed_xpso, store_xpso, seed::kPipelineApiRtv, kPipelineVersion);
    check(result.seed_records == 4 && result.store_records == 1 && result.added == 2 &&
              Read(store_xpso) == Concat(rtv, {p2, p1, p3}),
          "pipeline records are deduplicated by their full description");
    std::vector<seed::Record> records;
    const auto store = Read(store_xpso);
    check(seed::ParsePipelines(store, seed::kPipelineApiRtv, kPipelineVersion, records) ==
                  store.size() &&
              records.size() == 3,
          "the merged pipeline store parses completely");
  }
  {
    // The render-target path is part of the format: an RTV seed never enters
    // an ROV store, and an old description version is ignored.
    const auto before = Read(store_xpso);
    auto result =
        seed::MergePipelines(seed_xpso, store_xpso, seed::kPipelineApiRov, kPipelineVersion);
    check(!result.seed_valid && result.added == 0 && Read(store_xpso) == before,
          "an RTV seed is not merged into an ROV store");
    result = seed::MergePipelines(seed_xpso, store_xpso, seed::kPipelineApiRtv,
                                  kPipelineVersion + 1);
    check(!result.seed_valid && result.added == 0, "a mismatched pipeline version is ignored");
  }
  {
    auto corrupt = p3;
    corrupt[20] ^= 0x01;
    Write(seed_xpso, Concat(rtv, {p1, corrupt, p2}));
    std::filesystem::remove(store_xpso, error);
    const auto result =
        seed::MergePipelines(seed_xpso, store_xpso, seed::kPipelineApiRtv, kPipelineVersion);
    check(result.seed_records == 1 && Read(store_xpso) == Concat(rtv, {p1}),
          "pipeline records after a corrupt one are not imported");
  }

  {
    // 0.9.1 shader index (#5): hash, size, type and the hash of the first 32
    // bytes per shader, never the microcode; shaders shorter than 32 bytes
    // (s3: 8 dwords is exactly 32, kept; a 4-dword one is dropped) and other
    // versions are left out.
    const auto tiny = ShaderRecord(5, 4, false);
    const auto store = Concat(header, {s1, s2, s3, tiny});
    const auto index = seed::BuildShaderIndex(store, kShaderVersion);
    std::vector<seed::ShaderIndexEntry> entries;
    check(seed::ParseShaderIndex(index, kShaderVersion, entries) && entries.size() == 3 &&
              index.size() == 8 + 3 * seed::kShaderIndexRecordSize,
          "the index lists every shader of at least 32 bytes");
    std::vector<seed::Record> records;
    seed::ParseShaders(store, kShaderVersion, records);
    bool fields = entries.size() == 3;
    for (size_t i = 0; fields && i < 3; ++i) {
      const uint8_t* ucode = store.data() + records[i].offset + seed::kShaderRecordHeaderSize;
      const size_t bytes = records[i].size - seed::kShaderRecordHeaderSize;
      fields = entries[i].ucode_hash == records[i].hash &&
               entries[i].dword_count * 4 == bytes &&
               entries[i].type == (i == 0 ? 0u : 1u) &&
               entries[i].prefix_hash == XXH3_64bits(ucode, seed::kIndexPrefixBytes);
    }
    check(fields, "index entries carry only the storage hash, size, type and prefix hash");
    check(!seed::ParseShaderIndex(index, kShaderVersion + 1, entries) && entries.empty(),
          "an index of another shader format version is ignored");
  }

  {
    // 0.9.1 shader rebuild list (#5): ProgramCache containers (big-endian
    // header, microcode at virtual size + physical offset) and the patch list
    // (source hash, dwords as XOR masks) that turns a source into the shader
    // the game draws with.
    const auto be32 = [](std::vector<uint8_t>& out, uint32_t value) {
      out.push_back(uint8_t(value >> 24));
      out.push_back(uint8_t(value >> 16));
      out.push_back(uint8_t(value >> 8));
      out.push_back(uint8_t(value));
    };
    // One vertex container: 64 virtual bytes (header, shader block at 40),
    // then 16 bytes of microcode.
    std::vector<uint8_t> cache(5, 0xEE);  // leading bytes before the container
    const size_t container = cache.size();
    be32(cache, 0x102A1101);  // flags: vertex
    be32(cache, 64);          // virtual size
    be32(cache, 16);          // physical size
    be32(cache, 0);
    be32(cache, 36);          // constant table offset
    be32(cache, 0);
    be32(cache, 40);          // shader block offset
    be32(cache, 0);
    be32(cache, 0);
    be32(cache, 0);           // [36] constant table
    be32(cache, 0);           // [40] microcode physical offset
    be32(cache, 16);          // [44] microcode bytes
    cache.resize(container + 64, 0);
    const uint32_t source_words[4] = {0x00000688, 0, 0x05F82000, 0x12345678};
    const auto* source_bytes = reinterpret_cast<const uint8_t*>(source_words);
    cache.insert(cache.end(), source_bytes, source_bytes + sizeof(source_words));
    const auto shaders = seed::ParseProgramCache(cache);
    check(shaders.size() == 1 && shaders[0].offset == container + 64 && shaders[0].bytes == 16 &&
              shaders[0].type == 0 &&
              !std::memcmp(cache.data() + shaders[0].offset, source_words, 16),
          "a ProgramCache container yields its microcode span and stage");

    uint32_t runtime_words[4] = {0x00393A88, 3, 0x05F82000, 0x12345678};
    const uint64_t runtime_hash = XXH3_64bits(runtime_words, sizeof(runtime_words));
    std::vector<uint8_t> list;
    seed::AppendU32(list, seed::kShaderPatchMagic);
    seed::AppendU32(list, seed::ByteSwap32(kShaderVersion));
    seed::AppendU32(list, 1);
    AppendU64(list, runtime_hash);
    AppendU64(list, XXH3_64bits(source_words, sizeof(source_words)));
    seed::AppendU32(list, 4);  // dwords
    seed::AppendU32(list, 0);  // vertex
    seed::AppendU32(list, 2);  // patches
    seed::AppendU32(list, 0);
    seed::AppendU32(list, source_words[0] ^ runtime_words[0]);
    seed::AppendU32(list, 1);
    seed::AppendU32(list, source_words[1] ^ runtime_words[1]);
    std::vector<seed::ShaderPatchEntry> patches;
    check(seed::ParseShaderPatches(list, kShaderVersion, patches) && patches.size() == 1 &&
              patches[0].patches.size() == 2 && patches[0].dword_count == 4,
          "the rebuild list parses");
    uint32_t rebuilt[4];
    std::memcpy(rebuilt, cache.data() + shaders[0].offset, sizeof(rebuilt));
    for (const auto& [index, mask] : patches[0].patches) rebuilt[index] ^= mask;
    check(XXH3_64bits(rebuilt, sizeof(rebuilt)) == patches[0].runtime_hash,
          "the patched source matches the runtime shader hash");
    check(!seed::ParseShaderPatches(list, kShaderVersion + 1, patches) && patches.empty(),
          "a rebuild list of another shader format version is ignored");
    std::vector<uint8_t> bad = list;
    bad[bad.size() - 16] = 9;  // a patch beyond the shader
    check(!seed::ParseShaderPatches(bad, kShaderVersion, patches),
          "a patch outside the shader rejects the list");
    list.push_back(0);
    check(!seed::ParseShaderPatches(list, kShaderVersion, patches),
          "trailing bytes reject the list");
  }

  std::filesystem::remove_all(root, error);
  if (passed) std::cout << "Pipeline storage seed tests passed\n";
  return passed ? 0 : 1;
}
