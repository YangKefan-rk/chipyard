#include "rocket_native_decoder.h"
#include "native_fields.h"
#include "adapters/rocket/native_adapter.h"
#include "adapters/rocket/store_groups.h"
#include "rvwmo/producer_ids.h"
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace difftest_s::native {
namespace {
namespace rocket = rocket_difftest;
using rvwmo::CheckResult;
using rvwmo::CheckStatus;
using rvwmo::SourcePosition;
using Sample = NativeHartSample;

uint8_t instruction_length(uint32_t raw) {
  if ((raw & 3) != 3) return 2;
  if ((raw & 0x1f) != 0x1f) return 4;
  if ((raw & 0x3f) == 0x1f) return 6;
  if ((raw & 0x7f) == 0x3f) return 8;
  return static_cast<uint8_t>(10 + 2 * ((raw >> 12) & 7));
}

uint8_t narrow_byte(uint64_t value) {
  if (value > UINT8_MAX)
    throw std::invalid_argument("Rocket native byte field exceeds its supported width");
  return static_cast<uint8_t>(value);
}

struct ValidFields {
  NativeField valid;
  ValidFields(const NativeFields &fields, const std::string &path) : valid(fields.field(path + ".valid")) {
    if (valid.width != 1)
      throw std::invalid_argument("Rocket native Valid field must have one bit");
  }
};

struct InstructionFields {
  NativeField token, pc, raw, memory, command;
  InstructionFields(const NativeFields &fields, const std::string &path)
      : token(fields.field(path + ".token")), pc(fields.field(path + ".pc")),
        raw(fields.field(path + ".instruction")), memory(fields.field(path + ".memory")),
        command(fields.field(path + ".memoryCommand")) {
    if (token.width != 64 || raw.width != 32 || memory.width != 1)
      throw std::invalid_argument("Rocket native instruction widths are unsupported");
  }
  rocket::InstructionObservation read(const Sample &sample, SourcePosition source) const {
    rocket::InstructionObservation instruction;
    instruction.token = token.read(sample);
    instruction.pc = pc.read(sample);
    instruction.instruction = static_cast<uint32_t>(raw.read(sample));
    instruction.length = instruction_length(instruction.instruction);
    instruction.memory = memory.read(sample);
    instruction.memory_command = narrow_byte(command.read(sample));
    instruction.source = source;
    return instruction;
  }
};

struct AccessFields {
  NativeField id, address, tag, command, size;
  AccessFields(const NativeFields &fields, const std::string &path, const std::string &identity)
      : id(fields.field(path + '.' + identity)), address(fields.field(path + ".address")),
        tag(fields.field(path + ".tag")), command(fields.field(path + ".command")),
        size(fields.field(path + ".size")) {}
  rocket::RequestDetails read(const Sample &sample) const {
    return {address.read(sample), tag.read(sample), narrow_byte(command.read(sample)), narrow_byte(size.read(sample))};
  }
};

struct StoreFields : ValidFields {
  NativeField id, address, word, mask;
  StoreFields(const NativeFields &fields, const std::string &path)
      : ValidFields(fields, path), id(fields.field(path + ".bits.requestId")),
        address(fields.field(path + ".bits.address")), word(fields.field(path + ".bits.data")),
        mask(fields.field(path + ".bits.mask")) {}
  rocket::CacheStoreValue read(const Sample &sample, SourcePosition source) const {
    return {id.read(sample), address.read(sample), word.read(sample), narrow_byte(mask.read(sample)), source};
  }
};

struct StoreGroupFields {
  NativeField ecc_bytes, advance, advance_id, advance_maintenance, merge_target, physical_mask, second_slot;

  explicit StoreGroupFields(const NativeFields &fields)
      : ecc_bytes(fields.field("cache.eccBytes")), advance(fields.field("cache.storeAdvance")),
        advance_id(fields.field("cache.storeAdvanceRequestId")),
        advance_maintenance(fields.field("cache.storeAdvanceMaintenance")),
        merge_target(fields.field("cache.storeMergeTargetRequestId")),
        physical_mask(fields.field("cache.storeDrain.bits.physicalMask")),
        second_slot(fields.field("cache.storeDrain.bits.secondSlot")) {
    if (ecc_bytes.width > 8 || advance.width != 1 || advance_id.width != 64 ||
        advance_maintenance.width != 1 || merge_target.width != 64 || physical_mask.width != 8 || second_slot.width != 1)
      throw std::invalid_argument("Rocket store-group observation widths are unsupported");
  }
};

struct FpWriteFields {
  ValidFields write;
  NativeField token, known, destination, data;
  FpWriteFields(const NativeFields &fields, const std::string &path)
      : write(fields, path), token(fields.field(path + ".bits.token")),
        known(fields.field(path + ".bits.identityKnown")),
        destination(fields.field(path + ".bits.destination")), data(fields.field(path + ".bits.data")) {
    if (token.width != 64 || known.width != 1 || destination.width != 5 || data.width != 64)
      throw std::invalid_argument("Rocket floating writeback observation has unsupported widths");
  }
};

struct Fields {
  NativeFields metadata;
  NativeField hart, core_requestor, cache_requestors, cache_available;
  std::optional<NativeField> cache_idle;
  std::optional<StoreGroupFields> store_groups;
  ValidFields allocate, core_request, cache_request, s1_rejected, result, load, drain, retire, late_write, trap;
  InstructionFields allocation, retirement;
  AccessFields core_access, cache_access, result_access, load_access;
  StoreFields accepted;
  NativeField array_read, rejected_id, hit, cached, killed, nacked, exception, load_data;
  NativeField drain_id, drain_address, drain_data, drain_mask, maintenance, merge;
  NativeField integer_write, deferred_write, destination, write_data, floating_point;
  NativeField retire_memory, retire_command, late_token, late_known, late_destination, late_data;
  NativeField execute_killed, execute_token, memory_killed, memory_token, wb_rejected, wb_token;
  std::optional<NativeField> terminal_wfi, pipeline_empty, pending_integer_writeback, wfi_wake_pending;
  std::optional<NativeField> floating_write, pending_floating_writeback, fcsr;
  std::vector<FpWriteFields> floating_writes;

  explicit Fields(const NativeProbeLayout &layout)
      : metadata(layout), hart(metadata.field("core.hart")),
        core_requestor(metadata.field("coreRequestor")), cache_requestors(metadata.field("cacheRequestors")),
        cache_available(metadata.field("cache.available")), allocate(metadata, "core.allocate"),
        core_request(metadata, "core.request"), cache_request(metadata, "cache.request"),
        s1_rejected(metadata, "cache.s1Rejected"), result(metadata, "cache.result"), load(metadata, "cache.load"),
        drain(metadata, "cache.storeDrain"), retire(metadata, "core.retire"),
        late_write(metadata, "core.lateWrite"), trap(metadata, "core.trap"),
        allocation(metadata, "core.allocate.bits"), retirement(metadata, "core.retire.bits.instruction"),
        core_access(metadata, "core.request.bits", "token"),
        cache_access(metadata, "cache.request.bits.access", "requestId"),
        result_access(metadata, "cache.result.bits.access", "requestId"),
        load_access(metadata, "cache.load.bits.access", "requestId"), accepted(metadata, "cache.storeAccepted"),
        array_read(metadata.field("cache.request.bits.arrayRead")), rejected_id(metadata.field("cache.s1Rejected.bits")),
        hit(metadata.field("cache.result.bits.hit")), cached(metadata.field("cache.result.bits.cached")),
        killed(metadata.field("cache.result.bits.killed")), nacked(metadata.field("cache.result.bits.nacked")),
        exception(metadata.field("cache.result.bits.exception")), load_data(metadata.field("cache.load.bits.data")),
        drain_id(metadata.field("cache.storeDrain.bits.store.requestId")),
        drain_address(metadata.field("cache.storeDrain.bits.store.address")),
        drain_data(metadata.field("cache.storeDrain.bits.store.data")),
        drain_mask(metadata.field("cache.storeDrain.bits.store.mask")),
        maintenance(metadata.field("cache.storeDrain.bits.maintenance")), merge(metadata.field("cache.storeMerge")),
        integer_write(metadata.field("core.retire.bits.integerWrite")),
        deferred_write(metadata.field("core.retire.bits.deferredIntegerWrite")),
        destination(metadata.field("core.retire.bits.destination")), write_data(metadata.field("core.retire.bits.writeData")),
        floating_point(metadata.field("core.retire.bits.floatingPoint")),
        retire_memory(metadata.field("core.retire.bits.memory")), retire_command(metadata.field("core.retire.bits.memoryCommand")),
        late_token(metadata.field("core.lateWrite.bits.token")), late_known(metadata.field("core.lateWrite.bits.identityKnown")),
        late_destination(metadata.field("core.lateWrite.bits.destination")), late_data(metadata.field("core.lateWrite.bits.data")),
        execute_killed(metadata.field("core.executeKilled.valid")), execute_token(metadata.field("core.executeKilled.bits")),
        memory_killed(metadata.field("core.memoryKilled.valid")), memory_token(metadata.field("core.memoryKilled.bits")),
        wb_rejected(metadata.field("core.writebackRejected.valid")), wb_token(metadata.field("core.writebackRejected.bits")) {
    auto optional_flag = [&](std::string_view path) -> std::optional<NativeField> {
      for (size_t index = 0; index < layout.field_count; ++index) {
        if (std::string_view(layout.fields[index].path) != path)
          continue;
        const auto field = metadata.field(path);
        if (field.width != 1)
          throw std::invalid_argument("Rocket terminal observation must have one bit: " + std::string(path));
        return field;
      }
      return std::nullopt;
    };
    for (size_t index = 0; index < layout.field_count; ++index)
      if (std::string_view(layout.fields[index].path) == "cache.idle") {
        cache_idle = metadata.field("cache.idle");
        if (cache_idle->width != 1)
          throw std::invalid_argument("Rocket physical cache-idle proof must have one bit");
      }
    size_t store_group_fields = 0;
    for (size_t index = 0; index < layout.field_count; ++index) {
      const std::string_view path(layout.fields[index].path);
      if (path == "cache.eccBytes" || path == "cache.storeAdvanceRequestId" ||
          path == "cache.storeAdvanceMaintenance" || path == "cache.storeMergeTargetRequestId" ||
          path == "cache.storeDrain.bits.physicalMask")
        ++store_group_fields;
    }
    if (store_group_fields && store_group_fields != 5)
      throw std::invalid_argument("Rocket native layout has partial store-group observations");
    if (store_group_fields)
      store_groups.emplace(metadata);
    floating_write = optional_flag("core.retire.bits.floatingPointWrite");
    pending_floating_writeback = optional_flag("core.pendingFloatingPointWriteback");
    if (floating_write) {
      fcsr = metadata.field("core.fcsrValue");
      if (fcsr->width != 8 || !pending_floating_writeback)
        throw std::invalid_argument("Rocket FP layout lacks FCSR or pending-write evidence");
      const auto ports = metadata.records("core.floatingPointWrites");
      if (ports.size() != 2)
        throw std::invalid_argument("Rocket FP layout must describe both real FPU write ports");
      for (const auto &path : ports)
        floating_writes.emplace_back(metadata, path);
    } else if (pending_floating_writeback || !metadata.records("core.floatingPointWrites").empty()) {
      throw std::invalid_argument("Rocket FP layout contains only a partial observation set");
    }
    terminal_wfi = optional_flag("terminal.wfi");
    pipeline_empty = optional_flag("core.pipelineEmpty");
    pending_integer_writeback = optional_flag("core.pendingIntegerWriteback");
    wfi_wake_pending = optional_flag("core.wfiWakePending");
    const bool any_terminal_field = terminal_wfi || pipeline_empty || pending_integer_writeback || wfi_wake_pending;
    const bool all_terminal_fields = terminal_wfi && pipeline_empty && pending_integer_writeback && wfi_wake_pending;
    if (any_terminal_field && !all_terminal_fields)
      throw std::invalid_argument("Rocket native layout has a partial terminal observation set");
  }
};
} // namespace

struct RocketNativeDecoder::State {
  struct Instruction {
    rocket::InstructionObservation allocation;
    rvwmo::OperationId id;
    uint32_t next_attempt = 0;
    std::optional<uint64_t> request;
    bool retired = false;
    bool emitted = false;
    bool snapshot_delivered = false;
    bool unsupported_memory = false;
  };
  struct Request {
    rocket::RequestDetails details;
    std::optional<uint64_t> token;
    bool result_seen = false;
    bool qualified_store = false;
    bool discarded = false;
  };

  const NativeProbeLayout *layout;
  RocketDecoderConfiguration configuration;
  RocketPublicationSource publication_source;
  Fields fields;
  std::unique_ptr<rocket::NativeAdapter> adapter;
  std::unique_ptr<rocket::StoreGroupTracker> store_groups;
  std::map<uint64_t, Instruction> instructions;
  std::map<uint64_t, uint64_t> uid_tokens;
  std::map<uint64_t, Request> requests;
  std::map<uint64_t, SourcePosition> publications;
  std::optional<uint64_t> last_transport_tick, last_cycle, last_request, last_closed, entry_token;
  RocketBootstrapCounts bootstrap;
  RocketDecodedFrame *output = nullptr;
  uint64_t epoch = 0, next_sequence = 1, next_uid = 1;
  uint32_t core_requestor = 0, requestors = 0;
  uint8_t ecc_bytes = 1;
  bool maintenance_pending = false;
  CheckResult failure;
  bool started = false;

  State(const NativeProbeLayout &metadata, RocketDecoderConfiguration config, RocketPublicationSource publications)
      : layout(&metadata), configuration(config), publication_source(std::move(publications)), fields(metadata) {
    if (metadata.kind != 1 || !metadata.harts || metadata.hart >= metadata.harts || config.capacity < 3 ||
        config.publication_producer < rvwmo::kMemoryFabricProducerBase || !publication_source)
      throw std::invalid_argument("Rocket native decoder requires a valid layout, fabric producer and bounded capacity");
    if (config.require_terminal_observations && (!fields.terminal_wfi || !fields.cache_idle))
      failure = {CheckStatus::Unsupported, 0,
                 "Rocket WFI terminal checking requires WFI, pipeline, writeback, wake, and cache-idle observations"};
  }

  SourcePosition source(uint64_t cycle) {
    if (next_sequence == UINT64_MAX)
      throw std::invalid_argument("Rocket native source sequence exhausted");
    return {cycle, 0, next_sequence++, false};
  }

  CheckResult remember(CheckResult result) {
    if (!result && result.status != CheckStatus::Pending)
      failure = result;
    return result;
  }

  void require(CheckResult result) {
    if (!result && result.status != CheckStatus::Pending)
      throw result;
  }

  void require_store_transition(CheckResult result) {
    if (result.status == CheckStatus::Pending)
      throw CheckResult{CheckStatus::Incomplete, result.rule,
                        "Rocket store-group transition could not retain a complete source edge: " + result.message};
    if (!result)
      throw result;
  }

  bool in_scope(uint64_t token) const {
    return !configuration.entry_pc || (entry_token && token >= *entry_token);
  }

  Instruction &instruction(uint64_t token) {
    auto found = instructions.find(token);
    if (found == instructions.end())
      throw std::invalid_argument("Rocket native fact has no live instruction token");
    return found->second;
  }

  Request &request(uint64_t id) {
    auto found = requests.find(id);
    if (found == requests.end())
      throw std::invalid_argument("Rocket native cache fact has no live request identity: hart=" +
          std::to_string(layout->hart) + " request=" + std::to_string(id));
    return found->second;
  }

  void collect_output() {
    while (const auto *ready = adapter->output()) {
      if (!output)
        throw std::invalid_argument("Rocket native output has no staging owner");
      for (const auto &event : ready->batch.events) {
        if (event.kind == rvwmo::RuntimeEventKind::InstructionRetire) {
          const auto &retired = std::get<rvwmo::InstructionRetireEvent>(event.payload).instruction;
          auto &live = instruction(uid_tokens.at(retired.uid));
          live.emitted = true;
          if (live.request && !(live.allocation.memory && live.allocation.memory_command == 1)) {
            requests.erase(*live.request);
            live.request.reset();
          }
        } else if (event.kind == rvwmo::RuntimeEventKind::StorePublish) {
          const auto &published = std::get<rvwmo::StorePublishEvent>(event.payload);
          if (published.members.empty())
            throw std::invalid_argument("Rocket publication has no logical members");
          std::set<uint64_t> released_requests;
          for (const auto &member : published.members) {
            auto &live = instruction(uid_tokens.at(member.id.uid));
            if (live.id != member.id || !live.request || !released_requests.insert(*live.request).second ||
                !publications.erase(*live.request))
              throw std::invalid_argument("Rocket publication member has no unique retained physical drain");
            requests.erase(*live.request);
            live.request.reset();
          }
        }
        output->batch.events.push_back(event);
      }
      output->batch.watermarks.insert(output->batch.watermarks.end(), ready->batch.watermarks.begin(),
                                      ready->batch.watermarks.end());
      for (const auto &snapshot : ready->retirements) {
        instruction(uid_tokens.at(snapshot.id.uid)).snapshot_delivered = true;
        output->retirements.push_back(snapshot);
      }
      const auto serial = ready->serial;
      // The prior serial is consumed even when polling the next join is Pending.
      require(adapter->acknowledge_output(serial));
      for (auto live = instructions.begin(); live != instructions.end();) {
        if (live->second.emitted && live->second.snapshot_delivered && !live->second.request) {
          uid_tokens.erase(live->second.id.uid);
          live = instructions.erase(live);
        } else {
          ++live;
        }
      }
    }
  }

  void capture(rocket::CaptureResult result) {
    require(result.check);
    if (!result.consumed)
      throw CheckResult{CheckStatus::Incomplete, result.check.rule,
                       "Rocket native source cannot stall an unconsumed input: " + result.check.message};
    collect_output();
  }

  void initialize(const Sample &sample, uint64_t observed_epoch) {
    core_requestor = static_cast<uint32_t>(fields.core_requestor.read(sample));
    requestors = static_cast<uint32_t>(fields.cache_requestors.read(sample));
    if (!requestors || requestors > 1024 || core_requestor >= requestors || !observed_epoch)
      throw std::invalid_argument("Rocket native requestor metadata or epoch is invalid");
    epoch = observed_epoch;
    rocket::NativeConfiguration config;
    config.emitter = {layout->hart, rvwmo::hart_producer_id(layout->hart), epoch,
                      configuration.capacity, 64, configuration.publication_producer};
    config.cache = {core_requestor, requestors, configuration.capacity};
    adapter = std::make_unique<rocket::NativeAdapter>(config);
    if (fields.store_groups) {
      ecc_bytes = narrow_byte(fields.store_groups->ecc_bytes.read(sample));
      if (!ecc_bytes || ecc_bytes > 8 || (ecc_bytes & (ecc_bytes - 1)))
        throw std::invalid_argument("Rocket cache reports an invalid ECC granule");
      store_groups = std::make_unique<rocket::StoreGroupTracker>(rocket::StoreGroupConfiguration{
          ecc_bytes, configuration.capacity});
    }
    started = true;
  }

  void allocate(const Sample &sample, uint64_t cycle) {
    if (!fields.allocate.valid.read(sample))
      return;
    auto allocation = fields.allocation.read(sample, source(cycle));
    if (!entry_token && configuration.entry_pc == allocation.pc)
      entry_token = allocation.token;
    if (!in_scope(allocation.token))
      return;
    if (instructions.size() >= configuration.capacity || !next_uid)
      throw CheckResult{CheckStatus::Incomplete, 0, "Rocket native instruction table capacity exhausted"};
    Instruction live;
    live.allocation = allocation;
    live.id = {layout->hart, epoch, next_uid++, 0};
    if (!instructions.emplace(allocation.token, live).second)
      throw std::invalid_argument("Rocket native instruction token was allocated twice");
    uid_tokens.emplace(live.id.uid, allocation.token);
    capture(adapter->allocate(allocation));
  }

  void bind(const Sample &sample, uint64_t cycle) {
    const bool core_valid = fields.core_request.valid.read(sample);
    const bool cache_valid = fields.cache_request.valid.read(sample);
    if (core_valid && !cache_valid)
      throw std::invalid_argument("Rocket core request handshake has no matching cache handshake");
    if (!cache_valid)
      return;
    const auto id = fields.cache_access.id.read(sample);
    const auto details = fields.cache_access.read(sample);
    if (!id || (last_request && id <= *last_request))
      throw std::invalid_argument("Rocket native cache request ID is zero or reused");
    if (requests.size() >= configuration.capacity)
      throw CheckResult{CheckStatus::Incomplete, 0, "Rocket native cache association capacity exhausted"};
    last_request = id;
    unsigned tag_bits = 0;
    for (uint32_t count = requestors - 1; count; count >>= 1) ++tag_bits;
    const uint64_t mask = (UINT64_C(1) << tag_bits) - 1;
    const bool belongs_to_core = (details.tag & mask) == core_requestor;
    if (belongs_to_core != core_valid)
      throw std::invalid_argument("Rocket cache requestor tag disagrees with the native core handshake");
    Request association{details, std::nullopt, false, false, false};
    if (belongs_to_core) {
      const uint64_t token = fields.core_access.id.read(sample);
      if (in_scope(token)) {
        auto &live = instruction(token);
        if (live.request)
          throw std::invalid_argument("Rocket native instruction has more than one uncancelled cache request");
        const auto position = source(cycle);
        const rocket::CoreRequest core{token, live.next_attempt, fields.core_access.read(sample), position};
        const rocket::CacheRequest cache{id, details, position, static_cast<bool>(fields.array_read.read(sample))};
        live.request = id;
        association.token = token;
        requests.emplace(id, association);
        capture(adapter->bind_request(core, cache));
        return;
      }
    }
    requests.emplace(id, association);
  }

  void release(uint64_t id, bool cancelled) {
    auto &association = request(id);
    if (association.token) {
      const uint64_t token = *association.token;
      if (!cancelled)
        throw std::invalid_argument("Rocket native release requires an observed cancellation");
      auto &live = instruction(token);
      capture(adapter->cancel_request(id));
      if (live.next_attempt == UINT32_MAX)
        throw std::invalid_argument("Rocket native load attempt exhausted");
      ++live.next_attempt;
      live.request.reset();
    }
    requests.erase(id);
  }

  void cache_results(const Sample &sample, uint64_t cycle) {
    if (fields.s1_rejected.valid.read(sample))
      release(fields.rejected_id.read(sample), true);
    if (fields.result.valid.read(sample)) {
      const auto id = fields.result_access.id.read(sample);
      auto &association = request(id);
      const auto details = fields.result_access.read(sample);
      if (association.result_seen || association.details.command != details.command ||
          association.details.size != details.size || association.details.tag != details.tag)
        throw std::invalid_argument("Rocket native cache result is duplicated or changed request metadata");
      association.result_seen = true;
      association.qualified_store = fields.hit.read(sample) && fields.cached.read(sample) &&
          !fields.killed.read(sample) && !fields.nacked.read(sample) && !fields.exception.read(sample);
      if (fields.killed.read(sample) || fields.nacked.read(sample) || fields.exception.read(sample)) {
        release(id, true);
      } else if (association.token && !fields.cached.read(sample)) {
        instruction(*association.token).unsupported_memory = true;
      } else if (!association.token &&
                 (!fields.load.valid.read(sample) || fields.load_access.id.read(sample) != id) &&
                 (!fields.accepted.valid.read(sample) || fields.accepted.id.read(sample) != id)) {
        requests.erase(id);
      }
    }
    if (fields.load.valid.read(sample)) {
      const auto id = fields.load_access.id.read(sample);
      auto &association = request(id);
      if (!association.result_seen || !fields.result.valid.read(sample) ||
          fields.result_access.id.read(sample) != id || !fields.hit.read(sample) || !fields.cached.read(sample))
        throw std::invalid_argument("Rocket load sample has no matching qualified cache result");
      const auto details = fields.load_access.read(sample);
      if (details.command != association.details.command || details.size != association.details.size ||
          details.tag != association.details.tag)
        throw std::invalid_argument("Rocket load sample changed its request metadata");
      if (association.token) {
        capture(adapter->sample_load({id, fields.load_access.address.read(sample), fields.load_data.read(sample)}));
      } else {
        requests.erase(id);
      }
    }
    cache_stores(sample, cycle);
  }

  void accept_store(const Sample &sample, uint64_t cycle) {
    if (!fields.accepted.valid.read(sample))
      return;
    const auto id = fields.accepted.id.read(sample);
    auto &association = request(id);
    if (association.discarded)
      throw CheckResult{CheckStatus::Violation, 0, "Rocket squashed store reached cache acceptance"};
    if (!association.token) {
      ++bootstrap.stores;
      throw CheckResult{CheckStatus::Incomplete, 0,
        "Rocket cached bootstrap or implicit store changes memory outside the independent entry-state contract"};
    }
    if (!association.qualified_store)
      throw std::invalid_argument("Rocket store acceptance lacks a successful cached permission hit");
    capture(adapter->accept_store(fields.accepted.read(sample, source(cycle))));
    if (store_groups) {
      rocket::CacheStoreMember member;
      require_store_transition(adapter->store_member(id, member));
      require_store_transition(store_groups->accept(member));
    }
  }

  SourcePosition publication_position(uint64_t cycle) {
    const auto position = publication_source(cycle);
    if (position.cycle != cycle || !position.producer_sequence ||
        position.qualified_phase == SourcePosition::cycle_closed_phase)
      throw std::invalid_argument("Rocket fabric source allocator changed the observed drain edge");
    return position;
  }

  void publish_singleton(const Sample &sample, uint64_t cycle) {
    const auto id = fields.drain_id.read(sample);
    const auto &association = request(id);
    if (!association.token || !association.qualified_store)
      throw CheckResult{CheckStatus::Incomplete, 0, "Rocket physical drain has no qualified committed writer"};
    const auto position = publication_position(cycle);
    if (!publications.emplace(id, position).second)
      throw std::invalid_argument("Rocket native store drain was observed twice");
    capture(adapter->publish({{id, fields.drain_address.read(sample), fields.drain_data.read(sample),
                             narrow_byte(fields.drain_mask.read(sample)), position}, position.producer_sequence}));
  }

  void capture_store_group(const Sample &sample, uint64_t cycle) {
    const auto &group_fields = *fields.store_groups;
    const auto position = publication_position(cycle);
    rocket::CacheStoreDrain drain{{fields.drain_id.read(sample), fields.drain_address.read(sample),
        fields.drain_data.read(sample), narrow_byte(fields.drain_mask.read(sample)), position},
        narrow_byte(group_fields.physical_mask.read(sample)), static_cast<bool>(group_fields.second_slot.read(sample))};
    rocket::StoreGroupObservation group;
    require_store_transition(store_groups->capture_drain(drain, group));
    for (const auto &member : group.members) {
      const auto &association = request(member.request_id);
      if (!association.token || !association.qualified_store || *association.token != member.store.token ||
          instruction(*association.token).next_attempt != member.attempt)
        throw std::invalid_argument("Rocket merged publication member changed its qualified cache attempt");
      if (!publications.emplace(member.request_id, position).second)
        throw std::invalid_argument("Rocket merged publication member was physically drained twice");
    }
    const auto result = adapter->publish_group(group, position.producer_sequence);
    require(result.check);
    if (!result.consumed)
      throw CheckResult{CheckStatus::Incomplete, 0, "Rocket merged publication could not transfer lossless ownership"};
    require_store_transition(store_groups->acknowledge_drain(group.anchor_request_id));
    collect_output();
  }

  void cache_stores(const Sample &sample, uint64_t cycle) {
    const bool drain = fields.drain.valid.read(sample);
    const bool maintenance = drain && fields.maintenance.read(sample);
    if (!store_groups) {
      accept_store(sample, cycle);
      if (fields.merge.read(sample))
        throw CheckResult{CheckStatus::Unsupported, 0, "Rocket merged stores require complete per-byte writer membership"};
      if (drain && !maintenance)
        publish_singleton(sample, cycle);
      return;
    }
    const auto &group_fields = *fields.store_groups;
    const bool advance = group_fields.advance.read(sample);
    const bool merge = fields.merge.read(sample);
    const bool second_slot = group_fields.second_slot.read(sample);
    const bool maintenance_advance = advance && group_fields.advance_maintenance.read(sample);
    if (merge && (drain || advance || !fields.accepted.valid.read(sample)))
      throw std::invalid_argument("Rocket merge has no accepted incoming member or conflicts with another slot transition");
    if (maintenance && (!second_slot || !maintenance_pending))
      throw std::invalid_argument("Rocket maintenance drain has no matching pending maintenance slot");
    if (maintenance)
      throw CheckResult{CheckStatus::Incomplete, 0,
                       "Rocket ECC correction requires an independent maintenance-byte audit"};
    bool first_slot_drained = false;
    if (drain && second_slot) {
      if (maintenance)
        maintenance_pending = false;
      else {
        if (maintenance_pending)
          throw std::invalid_argument("Rocket logical store drain replaced an unobserved maintenance write");
        capture_store_group(sample, cycle);
      }
    } else if (drain) {
      const auto drain_id = fields.drain_id.read(sample);
      const bool accepted_is_drain = fields.accepted.valid.read(sample) &&
                                     fields.accepted.id.read(sample) == drain_id;
      if (!accepted_is_drain) {
        if (maintenance)
          throw std::invalid_argument("Rocket first-slot drain cannot be a maintenance write");
        capture_store_group(sample, cycle);
        first_slot_drained = true;
      }
    }
    // The drain names pre-edge storage. When it names the incoming store itself,
    // retain that accepted member before releasing the physical first slot.
    accept_store(sample, cycle);
    if (merge)
      require_store_transition(store_groups->merge(fields.accepted.id.read(sample), group_fields.merge_target.read(sample), source(cycle)));
    if (advance) {
      if (maintenance_advance) {
        if (fields.accepted.valid.read(sample) || maintenance_pending)
          throw std::invalid_argument("Rocket maintenance advance conflicts with a logical store or occupied slot");
        maintenance_pending = true;
      } else {
        if (maintenance_pending)
          throw std::invalid_argument("Rocket logical store advances over a pending maintenance write");
        require_store_transition(store_groups->advance(group_fields.advance_id.read(sample), source(cycle)));
      }
    }
    if (drain && !second_slot && !first_slot_drained) {
      if (maintenance)
        throw std::invalid_argument("Rocket first-slot drain cannot be a maintenance write");
      capture_store_group(sample, cycle);
    }
  }

  void squash(uint64_t token) {
    if (!in_scope(token))
      return;
    const auto found = instructions.find(token);
    if (found != instructions.end()) {
      if (found->second.request && publications.count(*found->second.request))
        throw CheckResult{CheckStatus::Violation, 0, "Rocket squashed instruction already changed the cache data array"};
      for (auto pending = requests.begin(); pending != requests.end();) {
        if (pending->second.token != token) {
          ++pending;
          continue;
        }
        if (pending->second.result_seen) {
          pending = requests.erase(pending);
          continue;
        }
        // The physical S1/S2 rejection can arrive after the core kill. Keep
        // its exact request identity until that terminal cache fact arrives.
        pending->second.token.reset();
        pending->second.discarded = true;
        ++pending;
      }
      uid_tokens.erase(found->second.id.uid);
      instructions.erase(found);
    }
    capture(adapter->squash(token));
  }

  void retire(const Sample &sample, uint64_t cycle) {
    if (!fields.retire.valid.read(sample))
      return;
    const auto token = fields.retirement.token.read(sample);
    if (!in_scope(token)) {
      ++bootstrap.instructions;
      if (fields.retire_memory.read(sample)) {
        const auto command = fields.retire_command.read(sample);
        bootstrap.loads += command == 0;
        bootstrap.stores += command == 1;
      }
      return;
    }
    auto &live = instruction(token);
    const auto observed = fields.retirement.read(sample, source(cycle));
    if (observed.memory != static_cast<bool>(fields.retire_memory.read(sample)) ||
        (observed.memory && observed.memory_command != narrow_byte(fields.retire_command.read(sample))) ||
        observed.memory != live.allocation.memory ||
        (observed.memory && observed.memory_command != live.allocation.memory_command))
      throw std::invalid_argument("Rocket native retirement decode qualifiers changed within one instruction");
    if (live.retired)
      throw std::invalid_argument("Rocket native retirement was observed twice");
    if (live.unsupported_memory)
      throw CheckResult{CheckStatus::Unsupported, 0, "Rocket retired an uncached or implicit memory access"};
    if (fields.floating_point.read(sample) && !fields.floating_write)
      throw CheckResult{CheckStatus::Unsupported, 0, "Rocket FP retirement requires both typed FPU write ports"};
    rocket::NativeRetirement retirement{token, observed.pc, observed.instruction, source(cycle),
        static_cast<bool>(fields.integer_write.read(sample)), static_cast<bool>(fields.deferred_write.read(sample)),
        narrow_byte(fields.destination.read(sample)), fields.write_data.read(sample),
        static_cast<bool>(fields.floating_point.read(sample))};
    retirement.floating_write = fields.floating_write && fields.floating_write->read(sample);
    const uint32_t raw = observed.instruction;
    const uint32_t function = (raw >> 12) & 7;
    const uint32_t csr = raw >> 20;
    const bool readonly_fcsr = (raw & 0x7f) == 0x73 && (csr == 1 || csr == 2 || csr == 3) &&
        (function == 2 || function == 3 || function == 6 || function == 7) && ((raw >> 15) & 31) == 0;
    if (readonly_fcsr && fields.fcsr && fields.pending_floating_writeback &&
        !fields.pending_floating_writeback->read(sample))
      retirement.fcsr = narrow_byte(fields.fcsr->read(sample));
    live.retired = true;
    capture(adapter->retire({retirement, observed.source}));
  }

  void close_prefix(uint64_t cycle) {
    require(adapter->poll());
    collect_output();
    std::optional<uint64_t> earliest;
    for (const auto &entry : instructions)
      if (!entry.second.emitted || !entry.second.snapshot_delivered)
        earliest = !earliest ? entry.second.allocation.source.cycle
                            : std::min(*earliest, entry.second.allocation.source.cycle);
    if (!earliest || *earliest != 0) {
      const auto complete = earliest ? std::min(cycle, *earliest - 1) : cycle;
      if (!last_closed || complete > *last_closed) {
        auto closure = adapter->close_hart_prefix(complete);
        if (!closure)
          throw CheckResult{closure.status == CheckStatus::Pending ? CheckStatus::Incomplete : closure.status,
                            closure.rule, closure.message};
        collect_output();
        last_closed = complete;
      }
      output->hart_closed_through = complete;
    }
    auto fabric = cycle;
    for (const auto &publication : publications) {
      if (!publication.second.cycle) {
        output->publication_closed_through.reset();
        return;
      }
      fabric = std::min(fabric, publication.second.cycle - 1);
    }
    output->publication_closed_through = fabric;
    output->tracked_work_drained = instructions.empty() && requests.empty() && publications.empty() &&
                                   !maintenance_pending && (!store_groups || store_groups->finalize()) &&
                                   (!configuration.entry_pc || entry_token.has_value());
  }

  CheckResult decode(const NativeFrame &frame, RocketDecodedFrame &converted) {
    if (!failure)
      return failure;
    try {
      if (frame.samples.size() != layout->harts || layout->hart >= frame.samples.size() ||
          frame.samples[layout->hart].layout != layout)
        throw std::invalid_argument("Rocket native frame does not contain the complete configured source roster");
      for (size_t hart = 0; hart < frame.samples.size(); ++hart) {
        const auto &sample = frame.samples[hart];
        if (!sample.layout || sample.layout->kind != 1 || sample.layout->hart != hart ||
            sample.layout->harts != layout->harts ||
            sample.words.size() != (sample.layout->payload_bits + 31) / 32)
          throw std::invalid_argument("Rocket native frame contains a missing or mismatched hart snapshot");
        const auto width = sample.layout->payload_bits % 32;
        if (width && (sample.words.back() >> width))
          throw std::invalid_argument("Rocket native snapshot contains bits outside its packed layout");
      }
      if ((!last_transport_tick && frame.transport_tick != 0) ||
          (last_transport_tick && (*last_transport_tick == UINT64_MAX || frame.transport_tick != *last_transport_tick + 1)))
        throw std::invalid_argument("Rocket native decoder frame tick skipped, repeated or wrapped");
      last_transport_tick = frame.transport_tick;
      if (frame.reset) {
        if (started)
          throw CheckResult{CheckStatus::Unsupported, 0, "Rocket native reset requires a fresh reference and observation session"};
        return {CheckStatus::Pending, 0, "Rocket native source is in reset"};
      }
      const auto &sample = frame.samples[layout->hart];
      if (fields.hart.read(sample) != layout->hart)
        throw std::invalid_argument("Rocket actual hart ID differs from the exported native port");
      if (!fields.cache_available.read(sample))
        throw CheckResult{CheckStatus::Unsupported, 0, "Rocket native cache observations are unavailable"};
      // The CPU SRAM read and the pstore write share one physical data-array
      // arbiter. Use the request's read edge, not a potentially later S2 load
      // result: an apparent same-hart read/drain edge is inconsistent evidence.
      if (fields.array_read.read(sample) && fields.drain.valid.read(sample))
        throw std::invalid_argument("Rocket single-port SRAM reports a load read and store drain on one source edge");
      if (!started)
        initialize(sample, frame.epoch);
      if (frame.epoch != epoch || (last_cycle && (*last_cycle == UINT64_MAX || frame.source_cycle != *last_cycle + 1)))
        throw std::invalid_argument("Rocket native source epoch or cycle changed discontinuously");
      if (fields.core_requestor.read(sample) != core_requestor || fields.cache_requestors.read(sample) != requestors)
        throw std::invalid_argument("Rocket native cache requestor roster changed");
      if (fields.store_groups && fields.store_groups->ecc_bytes.read(sample) != ecc_bytes)
        throw std::invalid_argument("Rocket cache ECC granule changed within one observation epoch");
      last_cycle = frame.source_cycle;
      RocketDecodedFrame staged;
      output = &staged;
      if (fields.trap.valid.read(sample)) {
        if (!configuration.entry_pc || entry_token)
          throw CheckResult{CheckStatus::Unsupported, 0, "Rocket trap or interrupt requires the shared architectural trap protocol"};
        ++bootstrap.traps;
      }
      allocate(sample, frame.source_cycle);
      bind(sample, frame.source_cycle);
      cache_results(sample, frame.source_cycle);
      if ((fields.execute_killed.read(sample) && !fields.execute_token.read(sample)) ||
          (fields.memory_killed.read(sample) && !fields.memory_token.read(sample)) ||
          (fields.wb_rejected.read(sample) && !fields.wb_token.read(sample)))
        throw std::invalid_argument("Rocket native termination has no instruction token");
      for (const auto token : std::set<uint64_t>{
          fields.execute_killed.read(sample) ? fields.execute_token.read(sample) : 0,
          fields.memory_killed.read(sample) ? fields.memory_token.read(sample) : 0,
          fields.wb_rejected.read(sample) ? fields.wb_token.read(sample) : 0})
        if (token)
          squash(token);
      retire(sample, frame.source_cycle);
      if (fields.late_write.valid.read(sample) && in_scope(fields.late_token.read(sample)))
        capture(adapter->late_write({fields.late_token.read(sample), static_cast<bool>(fields.late_known.read(sample)),
            narrow_byte(fields.late_destination.read(sample)), fields.late_data.read(sample), source(frame.source_cycle)}));
      for (const auto &port : fields.floating_writes) {
        if (port.write.valid.read(sample) && in_scope(port.token.read(sample)))
          capture(adapter->floating_write({port.token.read(sample), static_cast<bool>(port.known.read(sample)),
              narrow_byte(port.destination.read(sample)), port.data.read(sample), source(frame.source_cycle)}));
      }
      close_prefix(frame.source_cycle);
      if (fields.cache_idle)
        staged.physically_idle = fields.cache_idle->read(sample);
      if (fields.terminal_wfi) {
        staged.terminal = RocketTerminalFacts{
            static_cast<bool>(fields.terminal_wfi->read(sample)),
            static_cast<bool>(fields.pipeline_empty->read(sample)),
            static_cast<bool>(fields.pending_integer_writeback->read(sample)),
            static_cast<bool>(fields.wfi_wake_pending->read(sample)),
            fields.pending_floating_writeback && fields.pending_floating_writeback->read(sample)};
      }
      output = nullptr;
      converted = std::move(staged);
      return {};
    } catch (const CheckResult &error) {
      output = nullptr;
      return remember(error);
    } catch (const std::exception &error) {
      output = nullptr;
      return remember({CheckStatus::Invalid, 0, error.what()});
    }
  }
};

RocketNativeDecoder::RocketNativeDecoder(const NativeProbeLayout &layout, RocketDecoderConfiguration configuration,
                                         RocketPublicationSource publication_source)
    : state_(std::make_unique<State>(layout, configuration, std::move(publication_source))) {}
RocketNativeDecoder::~RocketNativeDecoder() = default;

CheckResult RocketNativeDecoder::decode(const NativeFrame &source, RocketDecodedFrame &output) {
  return state_->decode(source, output);
}

CheckResult RocketNativeDecoder::finalize() const {
  if (!state_->failure)
    return state_->failure;
  if (!state_->started || (state_->configuration.entry_pc && !state_->entry_token))
    return {CheckStatus::Incomplete, 0, "Rocket native hart never entered its checked scope"};
  auto result = state_->adapter->finalize();
  if (!result)
    return {result.status == CheckStatus::Pending ? CheckStatus::Incomplete : result.status, result.rule, result.message};
  if (state_->store_groups) {
    result = state_->store_groups->finalize();
    if (!result)
      return {result.status == CheckStatus::Pending ? CheckStatus::Incomplete : result.status, result.rule, result.message};
  }
  if (!state_->instructions.empty() || !state_->requests.empty() || !state_->publications.empty() || state_->maintenance_pending)
    return {CheckStatus::Incomplete, 0, "Rocket native decoder has undrained instruction or cache facts"};
  return {};
}

const RocketBootstrapCounts &RocketNativeDecoder::unchecked_bootstrap() const { return state_->bootstrap; }

} // namespace difftest_s::native
