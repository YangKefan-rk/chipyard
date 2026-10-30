#ifndef CHIPYARD_DIFFTEST_S_BOOM_NATIVE_DECODER_H
#define CHIPYARD_DIFFTEST_S_BOOM_NATIVE_DECODER_H

#include "native_probe.h"
#include "adapters/boom/native_collector.h"
#include <memory>

namespace difftest_s::native {

// The coordinator supplies a fabric source sequence, never a memory-order rank.
// It must be unique across all BOOM harts sharing the registered fabric channel.
using BoomPublicationSource = std::function<rvwmo::boom::EventSource(uint64_t)>;

struct BoomTerminalFacts {
  bool armed = false, reached = false, stopped = false;
  bool core_empty = false, writeback_active = false, interrupt_pending = false;
  uint64_t configured_pc = 0, source_cycle = 0, marker_cycle = 0;
  rvwmo::boom::ProbeIdentity marker;
  uint64_t marker_pc = 0;
  uint32_t marker_instruction = 0;
};

struct BoomWfiParkFacts {
  // Legacy RTL layouts do not carry a WFI parking probe; absence cannot be
  // interpreted as an unretired architectural completion.
  bool available = false, valid = false;
  bool csr_wait = false, rob_only_entry = false;
  bool interrupt_pending = false, writeback_active = false;
  uint64_t source_cycle = 0, marker_pc = 0;
  rvwmo::boom::ProbeIdentity marker;
  uint32_t marker_instruction = 0;
};

class BoomNativeDecoder {
public:
  BoomNativeDecoder(const NativeProbeLayout &layout, BoomPublicationSource publication_source);
  ~BoomNativeDecoder();
  rvwmo::CheckResult decode(const NativeFrame &source, rvwmo::boom::NativeFrame &output) const;

  rvwmo::CheckResult terminal(const NativeFrame &source, BoomTerminalFacts &output) const;
  rvwmo::CheckResult wfi_park(const NativeFrame &source, BoomWfiParkFacts &output) const;

private:
  struct Fields;
  std::unique_ptr<Fields> fields_;
};

} // namespace difftest_s::native
#endif
