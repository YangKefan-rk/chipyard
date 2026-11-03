#ifndef CHIPYARD_DIFFTEST_S_ROCKET_SEMANTIC_FEEDBACK_H
#define CHIPYARD_DIFFTEST_S_ROCKET_SEMANTIC_FEEDBACK_H

#include "rvwmo/host_session.h"
#include "rvwmo/producer_ids.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace difftest_s::native {

// An optional, read-only source tap. Nothing here supplies a REF value or a
// checker order. A failed certificate merely withholds feedback.
class RocketSemanticFeedback {
public:
  explicit RocketSemanticFeedback(const char *path) : path_(path && *path ? path : "") {}
  bool enabled() const { return !path_.empty(); }
  const char *unavailable_reason() const { return unavailable_reason_; }

  void bind_capabilities(const std::vector<rvwmo::ProducerCapability> &producers) noexcept {
    if (!enabled())
      return;
    try {
      for (const auto &producer : producers) {
        if (!producer.lossless_events || !producer.provides_source_positions ||
            !producer.provides_watermarks ||
            !certified_producers_.insert(producer.producer_id).second) {
          failed_ = true;
          unavailable_reason_ = "producer_not_lossless";
          break;
        }
      }
      if (certified_producers_.size() != 8) {
        failed_ = true;
        unavailable_reason_ = "missing_lossless_producer";
      }
    } catch (...) {
      failed_ = true;
      unavailable_reason_ = "producer_capability_allocation";
    }
  }

  void observe(const rvwmo::RuntimeEvent &event) noexcept {
    if (!enabled() || failed_)
      return;
    try {
      if (events_.size() >= 100000) {
        failed_ = true;
        return;
      }
      events_.push_back(event); // The callback's RuntimeEvent reference is ephemeral.
    } catch (...) {
      failed_ = true;
    }
  }

  // A read-only candidate for isolated source-contract tests. It is not a
  // terminal Pass certificate; only publish() may expose a completed run.
  std::optional<std::string> candidate_from_closed_sources(
      const std::map<uint32_t, rvwmo::SourcePosition> &marks,
      uint64_t completed_cycle, unsigned harts) const {
    return enabled() && !failed_ ? certified_record(marks, completed_cycle, harts) : std::nullopt;
  }

  bool publish(const rvwmo::HostSession &session, rvwmo::CheckResult result,
               uint64_t completed_cycle, unsigned harts) noexcept {
    if (!enabled() || failed_ || result.status != rvwmo::CheckStatus::Pass ||
        !session.semantic_feedback_publishable() || harts != 4)
      return false;
    try {
      const auto report = candidate_from_closed_sources(session.semantic_source_watermarks(), completed_cycle, harts);
      if (!report)
        return false;
      const auto temporary = path_ + ".tmp";
      if (std::filesystem::exists(path_) || std::filesystem::exists(temporary))
        return false; // Never replace another run's published or pending artifact.
      {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output || !(output << *report) || !(output.flush())) {
          std::remove(temporary.c_str());
          return false;
        }
      }
      if (std::rename(temporary.c_str(), path_.c_str()) != 0) {
        std::remove(temporary.c_str());
        return false;
      }
      return true;
    } catch (...) {
      return false;
    }
  }

private:
  static constexpr const char *clock_ = "rocket_tile_clock_posedge_pre_nba";
  std::optional<std::string> unavailable(const char *reason) const {
    unavailable_reason_ = reason;
    return std::nullopt;
  }
  struct Effect {
    const rvwmo::RuntimeEvent *event = nullptr;
    rvwmo::MemoryOperation operation;
    std::vector<rvwmo::MemoryOperation> members;
  };

  static std::optional<uint64_t> mask_bits(const std::vector<uint8_t> &mask) {
    if (mask.empty() || mask.size() > 8)
      return std::nullopt;
    uint64_t bits = 0;
    for (size_t byte = 0; byte < mask.size(); ++byte) {
      if (mask[byte] > 1)
        return std::nullopt;
      bits |= static_cast<uint64_t>(mask[byte]) << byte;
    }
    return bits ? std::optional<uint64_t>{bits} : std::nullopt;
  }

  std::optional<std::string> certified_record(
      const std::map<uint32_t, rvwmo::SourcePosition> &marks,
      uint64_t completed_cycle, unsigned harts) const {
    using namespace rvwmo;
    if (marks.size() != harts * 2 || certified_producers_.size() != marks.size() || events_.empty())
      return unavailable("missing_roster_or_events");
    for (const auto &[producer, position] : marks)
      if (!certified_producers_.count(producer))
        return unavailable("unqualified_source_capability");
    std::map<OperationId, MemoryOperation> retired;
    std::set<std::pair<OperationId, uint32_t>> canceled;
    std::set<OperationId> squashed;
    std::optional<uint64_t> epoch;
    for (const auto &event : events_) {
      if (event.source.cycle > completed_cycle || event.source.qualified_phase != 0 ||
          event.source.order_qualified || !event.source.producer_sequence)
        return unavailable("unqualified_event_position"); // A common intra-cycle order has not been qualified.
      if (event.kind == RuntimeEventKind::InstructionCancelAttempt) {
        const auto &cancel = std::get<InstructionCancelAttemptEvent>(event.payload);
        canceled.insert({cancel.id, cancel.attempt});
      } else if (event.kind == RuntimeEventKind::InstructionSquash) {
        squashed.insert(std::get<InstructionSquashEvent>(event.payload).id);
      }
    }
    for (const auto &event : events_) {
      if (event.kind == RuntimeEventKind::InstructionRetire) {
        const auto &instruction = std::get<InstructionRetireEvent>(event.payload).instruction;
        if (!epoch)
          epoch = instruction.epoch;
        if (*epoch != instruction.epoch || instruction.hart >= harts)
          return unavailable("retired_instruction_epoch_or_hart");
        for (const auto &operation : instruction.memory) {
          if (operation.id.hart != instruction.hart || operation.id.epoch != *epoch ||
              operation.id.uid != instruction.uid || squashed.count(operation.id) ||
              canceled.count({operation.id, operation.attempt}) ||
              !retired.emplace(operation.id, operation).second)
            return unavailable("retired_memory_identity");
        }
      }
    }
    if (!epoch || !*epoch)
      return unavailable("missing_retirement_epoch");
    std::set<OperationId> sampled_loads, published_stores;
    std::set<std::pair<uint32_t, uint64_t>> source_ids;
    std::set<std::pair<uint32_t, uint64_t>> transactions;
    std::vector<Effect> effects;
    uint64_t first = completed_cycle, last = 0;
    for (const auto &event : events_) {
      if (event.kind == RuntimeEventKind::LoadSample) {
        const auto &sample = std::get<LoadSampleEvent>(event.payload);
        const auto &operation = sample.operation;
        const auto member = retired.find(operation.id);
        if (member == retired.end() || member->second.attempt != operation.attempt ||
            !operation.load || canceled.count({operation.id, operation.attempt}))
          continue; // Speculation and replay are not architectural interactions.
        if (member->second.address != operation.address || member->second.byte_mask != operation.byte_mask ||
            event.producer_id != hart_producer_id(operation.id.hart) ||
            !mask_bits(operation.byte_mask) || !sampled_loads.insert(operation.id).second ||
            !source_ids.insert({event.producer_id, event.source.producer_sequence}).second)
          return unavailable("final_load_sample_conflict");
        effects.push_back({&event, operation, {}});
      } else if (event.kind == RuntimeEventKind::StorePublish) {
        const auto &publish = std::get<StorePublishEvent>(event.payload);
        if (publish.members.empty() || !publish.transaction_id || !mask_bits(publish.byte_mask) ||
            !transactions.insert({event.producer_id, publish.transaction_id}).second ||
            !source_ids.insert({event.producer_id, event.source.producer_sequence}).second)
          return unavailable("publish_transaction_or_source_conflict");
        Effect effect;
        effect.event = &event;
        for (const auto &member : publish.members) {
          const auto found = retired.find(member.id);
          if (found == retired.end() || !found->second.store ||
              found->second.id.epoch != *epoch ||
              event.producer_id != memory_fabric_producer_id(found->second.id.hart) ||
              !published_stores.insert(member.id).second)
            return unavailable("published_member_not_retired");
          effect.members.push_back(found->second);
        }
        if (publish.byte_writers.size() != publish.byte_mask.size())
          return unavailable("publication_writer_count");
        for (size_t byte = 0; byte < publish.byte_mask.size(); ++byte) {
          if (!publish.byte_mask[byte])
            continue;
          const auto writer = publish.byte_writers[byte];
          const auto belongs = std::find_if(effect.members.begin(), effect.members.end(),
              [&](const auto &member) { return member.id == writer; });
          if (belongs == effect.members.end())
            return unavailable("published_byte_writer_not_member");
        }
        effects.push_back(std::move(effect));
      }
    }
    for (const auto &[id, operation] : retired) {
      if ((operation.load && !sampled_loads.count(id)) ||
          (operation.store && !published_stores.count(id)))
        return unavailable("missing_final_load_or_publish");
    }
    if (effects.empty() || effects.size() > 100000)
      return unavailable("no_bounded_memory_effects");
    for (const auto &effect : effects) {
      first = std::min(first, effect.event->source.cycle);
      last = std::max(last, effect.event->source.cycle);
    }
    for (uint32_t hart = 0; hart < harts; ++hart) {
      for (const auto producer : {hart_producer_id(hart), memory_fabric_producer_id(hart)}) {
        const auto mark = marks.find(producer);
        if (mark == marks.end() || mark->second.cycle < last)
          return unavailable("source_watermark_before_effect");
      }
    }
    std::ostringstream output;
    output << "{\"schema_version\":1,\"kind\":\"rocket-source-closed.v1\","
           << "\"source_clock\":\"" << clock_ << "\",\"epoch\":" << *epoch
           << ",\"completed_cycle\":" << completed_cycle << ",\"first_cycle\":" << first
           << ",\"last_cycle\":" << last << ",\"cache_line_bytes\":64,\"intra_cycle_order_qualified\":false,"
           << "\"physical_address_verified\":true,\"complete\":true,\"producer_watermarks\":[";
    bool first_mark = true;
    for (const auto &[producer, mark] : marks) {
      if (!first_mark)
        output << ',';
      first_mark = false;
      output << "{\"producer_id\":" << producer << ",\"closed_through_cycle\":" << mark.cycle
             << ",\"epoch\":" << *epoch << ",\"source_clock\":\"" << clock_
             << "\",\"lossless\":true,\"complete\":true}";
    }
    output << "],\"events\":[";
    bool first_event = true;
    for (const auto &effect : effects) {
      const auto &event = *effect.event;
      const bool load = event.kind == RuntimeEventKind::LoadSample;
      const auto &publish = load ? StorePublishEvent{} : std::get<StorePublishEvent>(event.payload);
      const auto &op = load ? effect.operation : effect.members.front();
      if (!first_event)
        output << ',';
      first_event = false;
      output << "{\"kind\":\"" << (load ? "retired_load_sample" : "store_publish_group")
             << "\",\"producer_id\":" << event.producer_id
             << ",\"producer_sequence\":" << event.source.producer_sequence
             << ",\"source_cycle\":" << event.source.cycle
             << ",\"qualified_phase\":0,\"order_qualified\":false,\"hart\":" << op.id.hart
             << ",\"physical_address\":" << (load ? op.address : publish.address)
             << ",\"byte_mask\":" << *mask_bits(load ? op.byte_mask : publish.byte_mask)
             << ",\"address_is_physical\":true,\"final_attempt\":true";
      if (load)
        output << ",\"retired\":true,\"uid\":" << op.id.uid
               << ",\"attempt\":" << op.attempt << ",\"subaccess\":" << op.id.subaccess;
      else {
        output << ",\"transaction_id\":" << publish.transaction_id
               << ",\"allocation_generation\":1,\"retired_members\":[";
        bool first_member = true;
        for (const auto &member : effect.members) {
          if (!first_member)
            output << ',';
          first_member = false;
          output << "{\"hart\":" << member.id.hart << ",\"uid\":" << member.id.uid
                 << ",\"attempt\":" << member.attempt << ",\"subaccess\":" << member.id.subaccess
                 << ",\"retired\":true}";
        }
        output << ']';
      }
      output << '}';
    }
    output << "]}\n";
    return output.str();
  }

  std::string path_;
  std::vector<rvwmo::RuntimeEvent> events_;
  std::set<uint32_t> certified_producers_;
  bool failed_ = false;
  mutable const char *unavailable_reason_ = "none";
};

} // namespace difftest_s::native
#endif
