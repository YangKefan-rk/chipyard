#ifndef CHIPYARD_DIFFTEST_S_BOOM_NATIVE_DECODER_H
#define CHIPYARD_DIFFTEST_S_BOOM_NATIVE_DECODER_H

#include "native_probe.h"
#include "adapters/boom/native_collector.h"
#include <memory>

namespace difftest_s::native {

// The coordinator supplies a fabric source sequence, never a memory-order rank.
// It must be unique across all BOOM harts sharing the registered fabric channel.
using BoomPublicationSource = std::function<rvwmo::boom::EventSource(uint64_t)>;

class BoomNativeDecoder {
public:
  BoomNativeDecoder(const NativeProbeLayout &layout, BoomPublicationSource publication_source);
  ~BoomNativeDecoder();
  rvwmo::CheckResult decode(const NativeFrame &source, rvwmo::boom::NativeFrame &output) const;

private:
  struct Fields;
  std::unique_ptr<Fields> fields_;
};

} // namespace difftest_s::native
#endif
