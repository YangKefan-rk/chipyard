#ifndef CHIPYARD_DIFFTEST_S_NATIVE_PROBE_H
#define CHIPYARD_DIFFTEST_S_NATIVE_PROBE_H

#include "rvwmo/event.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace difftest_s::native {

struct NativeFieldSpec {
  const char *path;
  uint32_t offset;
  uint32_t width;
};

struct NativeProbeLayout {
  uint32_t abi_version;
  uint32_t kind;
  uint32_t hart;
  uint32_t harts;
  uint32_t payload_bits;
  size_t field_count;
  const NativeFieldSpec *fields;
};

struct NativeHartSample {
  const NativeProbeLayout *layout = nullptr;
  std::vector<uint32_t> words;
};

// An immutable, completely received source-edge roster. transport_tick names
// this framing interval only; source_cycle and each probe's source_cycle keep
// their RTL meanings, and this structure does not assign a memory-order rank.
struct NativeFrame {
  uint64_t transport_tick = 0;
  uint64_t epoch = 0;
  uint64_t source_cycle = 0;
  bool reset = false;
  std::vector<NativeHartSample> samples;
};

using NativeFrameConsumer = std::function<rvwmo::CheckResult(const NativeFrame &)>;
using NativeFinalizer = std::function<rvwmo::CheckResult()>;

// Install before the first source edge. The registered callbacks own Runtime
// semantics; this ingress validates and frames native snapshots only.
bool register_native_consumer(NativeFrameConsumer consume, NativeFinalizer finish);
const NativeProbeLayout *find_native_layout(uint32_t kind, uint32_t hart);

} // namespace difftest_s::native
#endif
