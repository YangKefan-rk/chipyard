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
  // Generator certificate for the selected BOOM duplicated-array macro. Legacy
  // layouts default false; it never changes a DUT-supplied sampling cycle.
  bool sram_capture_boundary = false;
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
using NativeFailureObserver = std::function<void(const rvwmo::CheckResult &)>;

// Install before the first source edge. The registered callbacks own Runtime
// semantics; this ingress validates and frames native snapshots only.
// The optional failure observer may stop the simulator and latch the error in
// its checker session. It must not reenter this ingress while being notified.
bool register_native_consumer(NativeFrameConsumer consume, NativeFinalizer finish,
                              NativeFailureObserver failure = {});
const NativeProbeLayout *find_native_layout(uint32_t kind, uint32_t hart);
// Native mains use a nonzero exit status unless ingress and its registered
// Runtime finalizer both completed successfully. Capture alone is Incomplete.
int native_exit_status();

// Explicit verification shutdown control. This stops new instruction admission,
// not clocks or cache traffic; source observations must prove the actual drain.
bool enable_native_terminal_drain(uint32_t harts);
void request_native_fetch_stop(uint32_t hart);
void complete_native_terminal_drain();
bool native_terminal_drain_complete();

} // namespace difftest_s::native
#endif
