#include "boom_native_source.h"
#include "rvwmo/producer_ids.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace difftest_s::native {
namespace {
namespace boom = rvwmo::boom;
using rvwmo::CheckResult;
using rvwmo::CheckStatus;

CheckResult invalid(const char *message) { return {CheckStatus::Invalid, 0, message}; }
CheckResult incomplete(const char *message) { return {CheckStatus::Incomplete, 0, message}; }
CheckResult pending(const char *message) { return {CheckStatus::Pending, 0, message}; }

template <class Record, class Identity>
void exclude_bootstrap(std::vector<Record> &records, uint64_t entry_uid, Identity identity) {
  records.erase(std::remove_if(records.begin(), records.end(), [&](const Record &record) {
    return identity(record).uid < entry_uid;
  }), records.end());
}

void select_entry_scope(boom::NativeFrame &frame, uint64_t entry_uid) {
  const auto identity = [](const auto &record) { return record.identity; };
  exclude_bootstrap(frame.allocations, entry_uid, identity);
  exclude_bootstrap(frame.queue_allocations, entry_uid, identity);
  exclude_bootstrap(frame.begins, entry_uid, identity);
  exclude_bootstrap(frame.cancellations, entry_uid, identity);
  exclude_bootstrap(frame.local_stores, entry_uid, identity);
  exclude_bootstrap(frame.retirements, entry_uid, identity);
  exclude_bootstrap(frame.retired_loads, entry_uid, identity);
  exclude_bootstrap(frame.store_releases, entry_uid, identity);
  exclude_bootstrap(frame.squashes, entry_uid, identity);
  const auto probe_identity = [](const auto &record) { return record.probe.identity; };
  exclude_bootstrap(frame.completions, entry_uid, probe_identity);
  exclude_bootstrap(frame.publications, entry_uid, probe_identity);
}
} // namespace

BoomNativeSource::BoomNativeSource(BoomNativeSourceConfiguration configuration)
    : configuration_(std::move(configuration)), collectors_(configuration_.entry_pcs.size()),
      entry_uids_(collectors_.size()), watermarks_(collectors_.size()), bootstrap_(collectors_.size()),
      terminal_facts_(collectors_.size()), wfi_park_facts_(collectors_.size()),
      terminal_retirements_(collectors_.size()),
      terminal_retirement_cycles_(collectors_.size()) {
  if (!configuration_.epoch || collectors_.empty() || collectors_.size() > 256 || !configuration_.staged_events)
    throw std::invalid_argument("BOOM native source requires a bounded hart roster, epoch and staging limit");
  for (uint32_t hart = 0; hart < collectors_.size(); ++hart) {
    const auto *layout = find_native_layout(2, hart);
    if (!layout || layout->harts != collectors_.size() || !configuration_.entry_pcs[hart])
      throw std::invalid_argument("BOOM native entry scope differs from the generated hart roster");
    decoders_.push_back(std::make_unique<BoomNativeDecoder>(*layout, [this, hart](uint64_t cycle) {
      if (captured_publication_sequence_ == UINT64_MAX)
        throw std::invalid_argument("BOOM native fabric source sequence exhausted");
      return boom::EventSource{rvwmo::memory_fabric_producer_id(hart),
                              {cycle, 0, ++captured_publication_sequence_, false}};
    }));
  }
}

CheckResult BoomNativeSource::remember_failure(CheckResult result) {
  if (result.status != CheckStatus::Pass && result.status != CheckStatus::Pending)
    failure_ = result;
  return result;
}

CheckResult BoomNativeSource::capture(const NativeFrame &frame) {
  if (!failure_)
    return failure_;
  if (frame.reset)
    return captured_cycle_ ? remember_failure(incomplete("BOOM source reset requires a new checked scope"))
                           : pending("BOOM source is in reset");
  if (frame.epoch != configuration_.epoch || frame.samples.size() != collectors_.size() ||
      (captured_cycle_ && (*captured_cycle_ == UINT64_MAX || frame.source_cycle != *captured_cycle_ + 1)))
    return remember_failure(invalid("BOOM complete source frame changed epoch, roster or cycle"));
  for (uint32_t hart = 0; hart < collectors_.size(); ++hart) {
    boom::NativeFrame decoded;
    auto result = decoders_[hart]->decode(frame, decoded);
    if (result)
      result = decoders_[hart]->terminal(frame, terminal_facts_[hart]);
    if (result)
      result = decoders_[hart]->wfi_park(frame, wfi_park_facts_[hart]);
    std::vector<BoomDeviceStoreFact> device_events;
    bool device_probe_available = false;
    if (result)
      result = decoders_[hart]->device_stores(frame, device_events, device_probe_available);
    if (!result)
      return remember_failure(result);
    if (!device_probe_available && !device_events.empty())
      return remember_failure(invalid("BOOM MMIO event has no complete physical probe layout"));
    if (!(result = capture_hart(std::move(decoded), device_events)))
      return remember_failure(result);
  }
  captured_cycle_ = frame.source_cycle;
  return {};
}

CheckResult BoomNativeSource::capture_hart(boom::NativeFrame frame,
                                           const std::vector<BoomDeviceStoreFact> &device_events) {
  const auto hart = frame.hart;
  if (!entry_uids_[hart]) {
    auto &outside = bootstrap_[hart];
    outside.instructions += frame.retirements.size();
    outside.loads += frame.completions.size();
    outside.stores += frame.publications.size();
    outside.traps += frame.trap;
    // Bootstrap RAM writes require an explicit initialized-state contract. This
    // source deliberately does not derive reference RAM from observed DUT data.
    if (!frame.publications.empty())
      return incomplete("BOOM bootstrap store publication is outside the initialized entry-state contract");
    for (const auto &allocation : frame.allocations) {
      if (allocation.pc != configuration_.entry_pcs[hart])
        continue;
      if (entry_uids_[hart])
        return invalid("BOOM native frame allocates its selected entry more than once");
      entry_uids_[hart] = allocation.identity.uid;
      boom::NativeCollectorConfiguration config;
      config.hart = hart;
      config.epoch = configuration_.epoch;
      config.initial_cycle = frame.cycle;
      config.limits = configuration_.limits;
      config.staged_events = configuration_.staged_events;
      config.checked_ram_base = configuration_.checked_ram_base;
      config.checked_ram_size = configuration_.checked_ram_size;
      config.skip_device_stores = configuration_.skip_device_stores;
      config.check_csr_post = configuration_.check_csr_post;
      collectors_[hart] = std::make_unique<boom::NativeCollector>(config);
    }
    if (!entry_uids_[hart]) {
      for (const auto &device : device_events) {
        if (device.request)
          ++outside.mmio_store_requests;
        else
          ++outside.mmio_store_acks;
      }
      watermarks_[hart] = frame.cycle;
      return {};
    }
  }
  for (const auto &device : device_events) {
    if (device.identity.uid < entry_uids_[hart]) {
      // BOOTROM MMIO is explicitly outside the independently initialized
      // DRAM-entry scope. Track late ACKs by UID, not by their delivery time.
      if (device.request)
        ++bootstrap_[hart].mmio_store_requests;
      else
        ++bootstrap_[hart].mmio_store_acks;
      continue;
    }
    // LinkNan-style opt-in: this device write is outside coherent RAM and
    // REF value comparison, but source identity and TL A/D delivery must close.
    if (!configuration_.skip_device_stores)
      return {CheckStatus::Unsupported, 0, "BOOM uncached TL store awaits device policy"};
    const uint64_t own_timer = UINT64_C(0x02004000) + 8 * hart;
    if (device.address != own_timer || device.mem_size != 3 ||
        device.byte_mask != UINT8_MAX || (device.request && device.tl_mask != UINT8_MAX) ||
        device.denied || device.corrupt)
      return {CheckStatus::Unsupported, 0,
              "BOOM device-store skip rejected physical request/ACK: hart=" + std::to_string(hart) +
              " uid=" + std::to_string(device.identity.uid) + " address=" + std::to_string(device.address) +
              " mem_size=" + std::to_string(device.mem_size) + " mask=" + std::to_string(device.byte_mask) +
              " tl_mask=" + std::to_string(device.tl_mask) + " denied=" + std::to_string(device.denied) +
              " corrupt=" + std::to_string(device.corrupt)};
    const rvwmo::OperationId id{hart, device.identity.epoch, device.identity.uid, 0};
    if (device.request) {
      if (!device_requests_.emplace(id, device.tl_source).second)
        return invalid("BOOM device-store skip repeats a TL A request identity");
    } else {
      auto request = device_requests_.find(id);
      if (request == device_requests_.end() || request->second != device.tl_source ||
          !device_acks_.insert(id).second)
        return invalid("BOOM device-store skip has no matching unique TL A/D identity");
      device_requests_.erase(request);
    }
  }
  select_entry_scope(frame, entry_uids_[hart]);
  if (frame.csr_post && frame.csr_post->identity.uid < entry_uids_[hart])
    frame.csr_post.reset();
  if (frame.csr_write_post && frame.csr_write_post->identity.uid < entry_uids_[hart])
    frame.csr_write_post.reset();
  if (configuration_.terminal_pc) {
    const auto &facts = terminal_facts_[hart];
    if (!facts.armed || facts.configured_pc != configuration_.terminal_pc)
      return invalid("BOOM DUT terminal policy differs from the independently configured PC");
    for (const auto &retirement : frame.retirements) {
      if (terminal_retirements_[hart])
        return invalid("BOOM architectural instruction retires after its terminal marker");
      if (retirement.architectural && retirement.pc == configuration_.terminal_pc &&
          retirement.instruction == 0x0000100f) {
        terminal_retirements_[hart] = retirement;
        terminal_retirement_cycles_[hart] = frame.cycle;
      }
    }
    if (facts.reached) {
      if (!terminal_retirements_[hart] || !facts.stopped ||
          facts.marker.uid != terminal_retirements_[hart]->identity.uid ||
          facts.marker.epoch != terminal_retirements_[hart]->identity.epoch ||
          facts.marker.rob_generation != terminal_retirements_[hart]->identity.rob_generation ||
          facts.marker.memory_generation != terminal_retirements_[hart]->identity.memory_generation ||
          facts.marker.attempt != terminal_retirements_[hart]->identity.attempt ||
          facts.marker_pc != configuration_.terminal_pc || facts.marker_instruction != 0x0000100f ||
          facts.marker_cycle != terminal_retirement_cycles_[hart])
        return invalid("BOOM stopped marker lacks its original actual architectural retirement");
    }
  }
  const auto captured = collectors_[hart]->capture(frame);
  if (captured.accepted && configuration_.skip_device_stores)
    for (const auto &store : frame.local_stores) {
      if (store.address >= configuration_.checked_ram_base &&
          store.address < configuration_.checked_ram_base + configuration_.checked_ram_size)
        continue;
      if (store.address != UINT64_C(0x02004000) + 8 * hart ||
          store.log2_size != 3 || store.byte_mask != UINT8_MAX)
        return {CheckStatus::Unsupported, 0, "BOOM skipped local store is not this hart's full-width CLINT timer write"};
      const rvwmo::OperationId id{hart, store.identity.epoch, store.identity.uid, 0};
      if (!skipped_device_stores_.insert(id).second)
        return invalid("BOOM device-store skip repeats a local store identity");
    }
  if (!captured.accepted)
    return captured.check.status == CheckStatus::Pending
               ? incomplete("BOOM source collector could not accept its complete native frame") : captured.check;
  boom::CollectedObservations ready;
  auto result = collectors_[hart]->take_ready(ready);
  if (result.status == CheckStatus::Pending)
    return {};
  if (!result)
    return result;
  for (const auto &watermark : ready.batch.watermarks) {
    if (watermark.producer_id != rvwmo::hart_producer_id(hart))
      return invalid("BOOM collector closed an unexpected source producer");
    watermarks_[hart] = watermark.closed_through.cycle;
  }
  if (ready.batch.events.size() > configuration_.staged_events -
          std::min(configuration_.staged_events, events_.size()))
    return incomplete("BOOM combined source staging limit exceeded");
  events_.insert(events_.end(), ready.batch.events.begin(), ready.batch.events.end());
  for (const auto &snapshot : ready.retirements)
    if (!snapshots_.emplace(snapshot.id, snapshot).second)
      return invalid("BOOM source repeats an architectural retirement snapshot");
  return {};
}

CheckResult BoomNativeSource::take_ready(boom::CollectedObservations &output) {
  if (!failure_)
    return failure_;
  if (!captured_cycle_)
    return pending("BOOM source has no complete active edge");
  const auto through = *std::min_element(watermarks_.begin(), watermarks_.end());
  if (closed_cycle_ && through <= *closed_cycle_)
    return pending("BOOM delayed observations keep the combined source prefix open");
  std::stable_sort(events_.begin(), events_.end(), [](const auto &lhs, const auto &rhs) {
    return lhs.source.cycle < rhs.source.cycle;
  });
  boom::CollectedObservations ready;
  std::vector<rvwmo::OperationId> retired;
  uint64_t next_fabric_sequence = emitted_publication_sequence_;
  for (const auto &event : events_) {
    if (event.source.cycle > through)
      continue;
    auto converted = event;
    if (event.kind == rvwmo::RuntimeEventKind::StorePublish) {
      if (next_fabric_sequence == UINT64_MAX)
        return remember_failure(invalid("BOOM public fabric source sequence exhausted"));
      // One channel per native L1 preserves its single write port. Cross-L1
      // simultaneous writes remain on distinct unqualified producers. Delivery
      // numbering within each channel never creates a cross-L1 GMO phase.
      converted.source.producer_sequence = ++next_fabric_sequence;
      for (auto &member : std::get<rvwmo::StorePublishEvent>(converted.payload).members)
        member.publish_position = converted.source;
    }
    if (event.kind == rvwmo::RuntimeEventKind::InstructionRetire) {
      const auto &instruction = std::get<rvwmo::InstructionRetireEvent>(event.payload).instruction;
      const rvwmo::OperationId id{instruction.hart, instruction.epoch, instruction.uid, 0};
      const auto snapshot = snapshots_.find(id);
      if (snapshot == snapshots_.end())
        return remember_failure(invalid("BOOM retired instruction has no independent actual result snapshot"));
      ready.retirements.push_back(snapshot->second);
      retired.push_back(id);
    }
    ready.batch.events.push_back(std::move(converted));
  }
  for (uint32_t hart = 0; hart < collectors_.size(); ++hart)
    ready.batch.watermarks.push_back({rvwmo::hart_producer_id(hart), rvwmo::SourcePosition::end_of_cycle(through)});
  for (uint32_t hart = 0; hart < collectors_.size(); ++hart)
    ready.batch.watermarks.push_back({rvwmo::memory_fabric_producer_id(hart), rvwmo::SourcePosition::end_of_cycle(through)});
  events_.erase(std::remove_if(events_.begin(), events_.end(), [through](const auto &event) {
    return event.source.cycle <= through;
  }), events_.end());
  for (const auto &id : retired)
    snapshots_.erase(id);
  emitted_publication_sequence_ = next_fabric_sequence;
  closed_cycle_ = through;
  output = std::move(ready);
  return {};
}

CheckResult BoomNativeSource::audit_reference_store(const rvwmo::ReferenceStoreValue &store) {
  if (!failure_)
    return failure_;
  if (store.id.hart >= collectors_.size() || !collectors_[store.id.hart])
    return remember_failure(invalid("independent REF store audit names an inactive BOOM hart"));
  return remember_failure(collectors_[store.id.hart]->audit_reference_store(
      {store.id, store.address, store.bytes, store.byte_mask}));
}

CheckResult BoomNativeSource::completion() const {
  if (!failure_)
    return failure_;
  if (!captured_cycle_ || !closed_cycle_ || *captured_cycle_ != *closed_cycle_ ||
      !events_.empty() || !snapshots_.empty())
    return incomplete("BOOM native source still has undelivered events or snapshots");
  if (configuration_.terminal_pc)
    for (uint32_t hart = 0; hart < collectors_.size(); ++hart) {
      const auto &facts = terminal_facts_[hart];
      if (!terminal_retirements_[hart] || !facts.reached || !facts.stopped || !facts.core_empty ||
          facts.writeback_active || facts.interrupt_pending || facts.source_cycle != *captured_cycle_)
        return incomplete("BOOM terminal retirement or current physical core drain is incomplete");
    }
  if (configuration_.skip_device_stores && (!device_requests_.empty() || device_acks_ != skipped_device_stores_))
    return incomplete("BOOM skipped device stores lack paired physical request and ACK evidence");
  for (const auto &collector : collectors_) {
    if (!collector)
      return incomplete("BOOM native hart never entered its independently initialized checked scope");
    auto result = collector->completion();
    if (!result)
      return result;
  }
  return {};
}
} // namespace difftest_s::native
