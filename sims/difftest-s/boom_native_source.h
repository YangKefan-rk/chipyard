#ifndef CHIPYARD_DIFFTEST_S_BOOM_NATIVE_SOURCE_H
#define CHIPYARD_DIFFTEST_S_BOOM_NATIVE_SOURCE_H

#include "boom_native_decoder.h"
#include <map>
#include <optional>
#include <set>

namespace difftest_s::native {

struct UncheckedBootstrap {
  uint64_t instructions = 0;
  uint64_t loads = 0;
  uint64_t stores = 0;
  uint64_t traps = 0;
  uint64_t mmio_store_requests = 0;
  uint64_t mmio_store_acks = 0;
};

struct BoomNativeSourceConfiguration {
  uint64_t epoch = 1;
  // The caller independently initializes each REF at this same entry PC. Boot
  // instructions preceding this selected scope are counted as uncovered.
  std::vector<uint64_t> entry_pcs;
  uint64_t terminal_pc = 0;
  // Match the independently initialized GlobalMem window; non-RAM accesses
  // need a separate MMIO protocol and must not enter scalar LocalStore.
  uint64_t checked_ram_base = 0;
  uint64_t checked_ram_size = 0;
  bool skip_device_stores = false;
  bool check_csr_post = false;
  rvwmo::boom::AdapterLimits limits;
  size_t staged_events = 65536;
};

// Coordinator-owned framing of the private BOOM collectors. This class has no
// reference model and no final RVWMO verdict. Its ready batches feed HostSession.
// Register hart_producer_id(hart) and memory_fabric_producer_id(hart) for every
// native hart. Distinct L1 write ports are distinct, unqualified fabric channels.
class BoomNativeSource {
public:
  explicit BoomNativeSource(BoomNativeSourceConfiguration configuration);
  rvwmo::CheckResult capture(const NativeFrame &frame);
  rvwmo::CheckResult take_ready(rvwmo::boom::CollectedObservations &output);
  rvwmo::CheckResult audit_reference_store(const rvwmo::ReferenceStoreValue &store);
  rvwmo::CheckResult completion() const;
  const std::vector<BoomTerminalFacts> &terminal_facts() const { return terminal_facts_; }
  const std::vector<BoomWfiParkFacts> &wfi_park_facts() const { return wfi_park_facts_; }
  const std::vector<UncheckedBootstrap> &unchecked_bootstrap() const { return bootstrap_; }
  uint64_t entry_uid(uint32_t hart) const { return hart < entry_uids_.size() ? entry_uids_[hart] : 0; }

private:
  BoomNativeSourceConfiguration configuration_;
  std::vector<std::unique_ptr<BoomNativeDecoder>> decoders_;
  std::vector<std::unique_ptr<rvwmo::boom::NativeCollector>> collectors_;
  std::vector<uint64_t> entry_uids_;
  std::vector<uint64_t> watermarks_;
  std::vector<UncheckedBootstrap> bootstrap_;
  std::map<rvwmo::OperationId, uint32_t> device_requests_;
  std::set<rvwmo::OperationId> device_acks_;
  std::set<rvwmo::OperationId> skipped_device_stores_;
  std::vector<BoomTerminalFacts> terminal_facts_;
  std::vector<BoomWfiParkFacts> wfi_park_facts_;
  std::vector<std::optional<rvwmo::boom::ProbeInstruction>> terminal_retirements_;
  std::vector<uint64_t> terminal_retirement_cycles_;
  std::vector<rvwmo::RuntimeEvent> events_;
  std::map<rvwmo::OperationId, rvwmo::ArchitecturalRetirement> snapshots_;
  std::optional<uint64_t> captured_cycle_;
  std::optional<uint64_t> closed_cycle_;
  uint64_t captured_publication_sequence_ = 0;
  uint64_t emitted_publication_sequence_ = 0;
  rvwmo::CheckResult failure_;

  rvwmo::CheckResult capture_hart(rvwmo::boom::NativeFrame frame,
                                 const std::vector<BoomDeviceStoreFact> &device_events);
  rvwmo::CheckResult remember_failure(rvwmo::CheckResult result);
};

} // namespace difftest_s::native
#endif
