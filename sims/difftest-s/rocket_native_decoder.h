#ifndef CHIPYARD_DIFFTEST_S_ROCKET_NATIVE_DECODER_H
#define CHIPYARD_DIFFTEST_S_ROCKET_NATIVE_DECODER_H

#include "native_probe.h"
#include "rvwmo/session.h"
#include <memory>
#include <optional>

namespace difftest_s::native {

struct RocketDecoderConfiguration {
  size_t capacity = 65536;
  uint32_t publication_producer = 0;
  // Zero checks the whole source. A nonzero entry explicitly excludes bootstrap.
  uint64_t entry_pc = 0;
  bool require_terminal_observations = false;
  bool require_fetch_stop = false;
};

struct RocketTerminalFacts {
  bool wfi = false;
  bool pipeline_empty = false;
  bool pending_integer_writeback = false;
  bool wfi_wake_pending = false;
  bool pending_floating_writeback = false;
};

struct RocketFetchStopFacts {
  bool requested = false;
  bool accepted = false;
  uint64_t last_allocation_token = 0;
};

struct RocketBootstrapCounts {
  uint64_t instructions = 0;
  uint64_t loads = 0;
  uint64_t stores = 0;
  uint64_t traps = 0;
};

struct RocketDecodedFrame {
  rvwmo::EventBatch batch;
  std::vector<rvwmo::ArchitecturalRetirement> retirements;
  std::optional<uint64_t> hart_closed_through;
  // The coordinator closes the fabric channel only through the minimum complete
  // prefix from every participating decoder. This is not a physical-idle proof.
  std::optional<uint64_t> publication_closed_through;
  bool tracked_work_drained = false;
  std::optional<bool> physically_idle;
  std::optional<RocketTerminalFacts> terminal;
  std::optional<RocketFetchStopFacts> fetch_stop;
};

// Allocates a unique fabric source identity at the observed drain edge. The
// callback must preserve its cycle; sequence allocation never establishes GMO.
using RocketPublicationSource = std::function<rvwmo::SourcePosition(uint64_t)>;

// Maps complete, immutable native snapshots to the existing Rocket adapters.
// The layout must outlive this decoder. decode() consumes each frame once;
// capacity exhaustion is terminal because the RTL source cannot be stalled.
class RocketNativeDecoder {
public:
  RocketNativeDecoder(const NativeProbeLayout &layout, RocketDecoderConfiguration configuration,
                      RocketPublicationSource publication_source);
  ~RocketNativeDecoder();
  RocketNativeDecoder(const RocketNativeDecoder &) = delete;
  RocketNativeDecoder &operator=(const RocketNativeDecoder &) = delete;

  rvwmo::CheckResult decode(const NativeFrame &source, RocketDecodedFrame &output);
  rvwmo::CheckResult finalize() const;
  const RocketBootstrapCounts &unchecked_bootstrap() const;

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace difftest_s::native
#endif
