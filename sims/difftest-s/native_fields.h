#ifndef CHIPYARD_DIFFTEST_S_NATIVE_FIELDS_H
#define CHIPYARD_DIFFTEST_S_NATIVE_FIELDS_H

#include "native_probe.h"
#include <algorithm>
#include <charconv>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace difftest_s::native {

// A resolved field avoids path lookup on every RTL edge. Wide fields may cross
// three 32-bit DPI words; all shifts below are strictly smaller than 64 bits.
struct NativeField {
  uint32_t offset;
  uint32_t width;

  uint64_t read(const NativeHartSample &sample) const {
    const uint64_t end = static_cast<uint64_t>(offset) + width;
    if (!sample.layout || !width || width > 64 || end > sample.layout->payload_bits ||
        (end + 31) / 32 > sample.words.size())
      throw std::invalid_argument("native field exceeds the received snapshot");
    uint64_t value = 0;
    uint32_t consumed = 0;
    while (consumed < width) {
      const uint32_t bit = offset + consumed;
      const uint32_t shift = bit % 32;
      const uint32_t count = std::min(width - consumed, 32 - shift);
      const uint32_t mask = count == 32 ? UINT32_MAX : (UINT32_C(1) << count) - 1;
      const uint32_t chunk = (sample.words[bit / 32] >> shift) & mask;
      value |= static_cast<uint64_t>(chunk) << consumed;
      consumed += count;
    }
    return value;
  }
};

class NativeFields {
public:
  explicit NativeFields(const NativeProbeLayout &layout) {
    if (layout.abi_version != 1 || !layout.fields || !layout.field_count || !layout.payload_bits)
      throw std::invalid_argument("native field metadata is missing or unsupported");
    uint64_t next = 0;
    for (size_t index = 0; index < layout.field_count; ++index) {
      const auto &spec = layout.fields[index];
      if (!spec.path || !*spec.path || spec.offset != next || !spec.width || spec.width > 64 ||
          !fields_.emplace(spec.path, NativeField{spec.offset, spec.width}).second)
        throw std::invalid_argument("native field metadata is overlapping, duplicated or invalid");
      next += spec.width;
    }
    if (next != layout.payload_bits)
      throw std::invalid_argument("native field metadata does not cover its declared payload");
  }

  NativeField field(std::string_view path) const {
    const auto found = fields_.find(path);
    if (found == fields_.end())
      throw std::invalid_argument("missing native probe field: " + std::string(path));
    return found->second;
  }

  // Vec entries are discovered from their Valid bits. Require a contiguous,
  // bounded vector; a silently missing lane would lose lifecycle observations.
  std::vector<std::string> records(std::string_view group) const {
    const std::string prefix = std::string(group) + '[';
    constexpr std::string_view suffix = "].valid";
    std::map<unsigned, std::string> indexed;
    for (const auto &entry : fields_) {
      const auto &path = entry.first;
      if (path.size() <= prefix.size() + suffix.size() || path.substr(0, prefix.size()) != prefix ||
          path.substr(path.size() - suffix.size()) != suffix)
        continue;
      const auto number = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
      unsigned index = 0;
      const auto parsed = std::from_chars(number.data(), number.data() + number.size(), index);
      if (parsed.ec != std::errc() || parsed.ptr != number.data() + number.size() || index >= 256 ||
          !indexed.emplace(index, std::string(group) + '[' + std::to_string(index) + ']').second)
        throw std::invalid_argument("invalid native probe vector: " + std::string(group));
    }
    if (indexed.empty())
      throw std::invalid_argument("missing native probe vector: " + std::string(group));
    std::vector<std::string> result;
    for (const auto &entry : indexed) {
      if (entry.first != result.size())
        throw std::invalid_argument("native probe vector has a missing lane: " + std::string(group));
      result.push_back(entry.second);
    }
    return result;
  }

private:
  std::map<std::string_view, NativeField, std::less<>> fields_;
};

} // namespace difftest_s::native
#endif
