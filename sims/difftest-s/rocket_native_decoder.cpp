#include "rocket_native_decoder.h"
#include "native_fields.h"
#include "adapters/rocket/cache_tracker.h"
#include "adapters/rocket/retirement_capture.h"
#include "rvwmo/producer_ids.h"
#include <algorithm>
#include <deque>
#include <limits>
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

struct Fields {
  NativeFields metadata;
  NativeField hart, core_requestor, cache_requestors, cache_available;
  ValidFields allocate, core_request, cache_request, s1_rejected, result, load, drain, retire, late_write, trap;
  InstructionFields allocation, retirement;
  AccessFields core_access, cache_access, result_access, load_access;
  StoreFields accepted;
  NativeField array_read, rejected_id, hit, cached, killed, nacked, exception, load_data;
  NativeField drain_id, drain_address, drain_data, drain_mask, maintenance, merge;
  NativeField integer_write, deferred_write, destination, write_data, floating_point;
  NativeField retire_memory, retire_command, late_token, late_known, late_destination, late_data;
  NativeField execute_killed, execute_token, memory_killed, memory_token, wb_rejected, wb_token;

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
        wb_rejected(metadata.field("core.writebackRejected.valid")), wb_token(metadata.field("core.writebackRejected.bits")) {}
};
} // namespace

struct RocketNativeDecoder::State {
  struct Instruction {
    rocket::InstructionObservation allocation;
    rvwmo::OperationId id;
    uint32_t next_attempt = 0;
    std::optional<uint64_t> request;
    std::optional<rocket::NativeRetirement> retirement;
    std::optional<SourcePosition> local_source;
    bool local_committed = false;
    bool emitted = false;
    bool snapshot_delivered = false;
    bool unsupported_memory = false;
  };
  struct Request {
    rocket::RequestDetails details;
    std::optional<uint64_t> token;
    bool result_seen = false;
  };
  struct Publication {
    rocket::CacheStoreValue value;
    uint64_t token = 0;
  };

  const NativeProbeLayout *layout;
  RocketDecoderConfiguration configuration;
  RocketPublicationSource publication_source;
  Fields fields;
  std::unique_ptr<rocket::CacheTracker> tracker;
  std::unique_ptr<rocket::EventEmitter> emitter;
  std::unique_ptr<rocket::RetirementCapture> capture;
  std::map<uint64_t, Instruction> instructions;
  std::map<uint64_t, uint64_t> uid_tokens;
  std::map<uint64_t, Request> requests;
  std::deque<Publication> publications;
  std::optional<uint64_t> last_transport_tick, last_cycle, last_request, last_closed;
  uint64_t epoch = 0;
  uint64_t next_sequence = 1;
  uint32_t core_requestor = 0, requestors = 0;
  CheckResult failure;
  bool started = false;

  State(const NativeProbeLayout &metadata, RocketDecoderConfiguration config, RocketPublicationSource publications)
      : layout(&metadata), configuration(config), publication_source(std::move(publications)), fields(metadata) {
    if (metadata.kind != 1 || !metadata.harts || metadata.hart >= metadata.harts || config.capacity < 3 ||
        config.publication_producer < rvwmo::kMemoryFabricProducerBase || !publication_source)
      throw std::invalid_argument("Rocket native decoder requires a valid layout, fabric producer and bounded capacity");
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
    if (result)
      return;
    if (result.status == CheckStatus::Pending)
      result = {CheckStatus::Incomplete, 0, "Rocket native capture cannot retain this source edge: " + result.message};
    failure = std::move(result);
    throw failure;
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
      throw std::invalid_argument("Rocket native cache fact has no live request identity");
    return found->second;
  }

  void initialize(const Sample &sample, uint64_t observed_epoch) {
    core_requestor = static_cast<uint32_t>(fields.core_requestor.read(sample));
    requestors = static_cast<uint32_t>(fields.cache_requestors.read(sample));
    if (!requestors || requestors > 1024 || core_requestor >= requestors || !observed_epoch)
      throw std::invalid_argument("Rocket native requestor metadata or epoch is invalid");
    epoch = observed_epoch;
    tracker = std::make_unique<rocket::CacheTracker>(rocket::CacheConfiguration{core_requestor, requestors, configuration.capacity});
    emitter = std::make_unique<rocket::EventEmitter>(rocket::EmitterConfiguration{
        layout->hart, rvwmo::hart_producer_id(layout->hart), epoch, configuration.capacity, 64,
        configuration.publication_producer});
    capture = std::make_unique<rocket::RetirementCapture>(rocket::RetirementConfiguration{layout->hart, epoch, configuration.capacity});
    started = true;
  }

  void allocate(const Sample &sample, uint64_t cycle) {
    if (!fields.allocate.valid.read(sample))
      return;
    auto allocation = fields.allocation.read(sample, source(cycle));
    rvwmo::OperationId id;
    require(emitter->begin(allocation, id));
    require(capture->allocate(allocation, id));
    if (instructions.size() >= configuration.capacity)
      throw CheckResult{CheckStatus::Incomplete, 0, "Rocket native instruction table capacity exhausted"};
    if (instructions.count(allocation.token))
      throw std::invalid_argument("Rocket native instruction token was allocated twice");
    Instruction state;
    state.allocation = allocation;
    state.id = id;
    instructions.emplace(allocation.token, std::move(state));
    uid_tokens.emplace(id.uid, allocation.token);
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
    Request association{details, std::nullopt, false};
    if (belongs_to_core) {
      const uint64_t token = fields.core_access.id.read(sample);
      auto &live = instruction(token);
      if (live.request)
        throw std::invalid_argument("Rocket native instruction has more than one uncancelled cache request");
      const auto position = source(cycle);
      const rocket::CoreRequest core{token, live.next_attempt, fields.core_access.read(sample), position};
      const rocket::CacheRequest cache{id, details, position, static_cast<bool>(fields.array_read.read(sample))};
      require(tracker->bind(core, cache));
      live.request = id;
      association.token = token;
    }
    requests.emplace(id, std::move(association));
  }

  void release(uint64_t id, bool cancelled) {
    auto &association = request(id);
    if (association.token) {
      auto &live = instruction(*association.token);
      if (cancelled && association.details.command == 0) {
        require(emitter->cancel_load(*association.token, live.next_attempt));
        if (live.next_attempt == UINT32_MAX)
          throw std::invalid_argument("Rocket native load attempt exhausted");
        ++live.next_attempt;
      }
      require(tracker->release(id));
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
      if (fields.killed.read(sample) || fields.nacked.read(sample) || fields.exception.read(sample)) {
        release(id, true);
      } else if (association.token && !fields.cached.read(sample)) {
        instruction(*association.token).unsupported_memory = true;
      } else if (!association.token && association.details.command == 0 &&
                 (!fields.load.valid.read(sample) || fields.load_access.id.read(sample) != id)) {
        release(id, false);
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
        rocket::LoadObservation load;
        auto converted = tracker->load_sample({id, fields.load_access.address.read(sample), fields.load_data.read(sample)}, load);
        if (converted.status != CheckStatus::Pending) {
          require(converted);
          require(emitter->sample_load(load));
        }
      }
      // Non-core implicit reads carry no explicit architectural retirement.
      if (!association.token)
        requests.erase(id);
    }
    if (fields.accepted.valid.read(sample)) {
      const auto id = fields.accepted.id.read(sample);
      auto &association = request(id);
      if (!association.token)
        throw CheckResult{CheckStatus::Unsupported, 0, "Rocket implicit non-core stores need an observed translation-write protocol"};
      if (!association.result_seen)
        throw std::invalid_argument("Rocket store acceptance has no matching cache result");
      auto converted = tracker->accept_store(fields.accepted.read(sample, source(cycle)));
      if (converted.status != CheckStatus::Pending)
        require(converted);
    }
    if (fields.merge.read(sample))
      require(tracker->observe_merge());
    if (fields.drain.valid.read(sample) && !fields.maintenance.read(sample)) {
      const auto id = fields.drain_id.read(sample);
      auto &association = request(id);
      if (!association.token)
        throw CheckResult{CheckStatus::Unsupported, 0, "Rocket non-core store drain has no architectural writer identity"};
      const auto position = publication_source(cycle);
      if (position.cycle != cycle || !position.producer_sequence ||
          position.qualified_phase == SourcePosition::cycle_closed_phase)
        throw std::invalid_argument("Rocket fabric source allocator changed the observed drain edge");
      if (publications.size() >= configuration.capacity)
        throw CheckResult{CheckStatus::Incomplete, 0, "Rocket native publication buffer exhausted"};
      if (std::any_of(publications.begin(), publications.end(), [id](const auto &entry) { return entry.value.request_id == id; }))
        throw std::invalid_argument("Rocket native store drain was observed twice");
      publications.push_back({{id, fields.drain_address.read(sample), fields.drain_data.read(sample),
                               narrow_byte(fields.drain_mask.read(sample)), position}, *association.token});
    }
  }

  void squash(uint64_t token) {
    if (std::any_of(publications.begin(), publications.end(), [token](const auto &entry) { return entry.token == token; }))
      throw CheckResult{CheckStatus::Violation, 0, "Rocket squashed instruction already changed the cache data array"};
    require(emitter->squash(token));
    require(capture->squash(token));
    const auto found = instructions.find(token);
    if (found == instructions.end())
      return; // Both adapters certified a repeated native termination fact.
    for (auto request = requests.begin(); request != requests.end();) {
      if (request->second.token == token)
        request = requests.erase(request);
      else
        ++request;
    }
    tracker->discard_instruction(token);
    uid_tokens.erase(found->second.id.uid);
    instructions.erase(found);
  }

  void retire(const Sample &sample, uint64_t cycle) {
    if (!fields.retire.valid.read(sample))
      return;
    const auto token = fields.retirement.token.read(sample);
    auto &live = instruction(token);
    const auto observed = fields.retirement.read(sample, source(cycle));
    if (observed.memory != static_cast<bool>(fields.retire_memory.read(sample)) ||
        (observed.memory && observed.memory_command != narrow_byte(fields.retire_command.read(sample))) ||
        observed.memory != live.allocation.memory ||
        (observed.memory && observed.memory_command != live.allocation.memory_command))
      throw std::invalid_argument("Rocket native retirement decode qualifiers changed within one instruction");
    if (live.retirement)
      throw std::invalid_argument("Rocket native retirement was observed twice");
    if (live.unsupported_memory)
      throw CheckResult{CheckStatus::Unsupported, 0, "Rocket retired an uncached or implicit memory access"};
    if (observed.memory && observed.memory_command == 1)
      live.local_source = observed.source;
    rocket::NativeRetirement retirement{token, observed.pc, observed.instruction, source(cycle),
        static_cast<bool>(fields.integer_write.read(sample)), static_cast<bool>(fields.deferred_write.read(sample)),
        narrow_byte(fields.destination.read(sample)), fields.write_data.read(sample),
        static_cast<bool>(fields.floating_point.read(sample))};
    require(capture->retire(retirement));
    live.retirement = retirement;
  }

  void emit_ready() {
    for (auto &entry : instructions) {
      auto &live = entry.second;
      if (!live.retirement || live.emitted)
        continue;
      if (live.local_source && !live.local_committed) {
        rocket::StoreObservation store;
        auto local = tracker->local_store(entry.first, *live.local_source, store);
        if (local.status == CheckStatus::Pending)
          continue;
        require(local);
        require(emitter->commit_store(store));
        live.local_committed = true;
      }
      auto retired = emitter->retire(entry.first, live.retirement->source);
      if (retired.status == CheckStatus::Pending)
        continue;
      require(retired);
      live.emitted = true;
      if (live.request && !live.local_source)
        release(*live.request, false);
    }
    for (auto publication = publications.begin(); publication != publications.end();) {
      rocket::StoreObservation store;
      auto converted = tracker->publication(publication->value, store);
      if (converted.status == CheckStatus::Pending) {
        ++publication;
        continue;
      }
      require(converted);
      auto published = emitter->publish_store(store, publication->value.source.producer_sequence);
      if (published.status == CheckStatus::Pending) {
        ++publication;
        continue;
      }
      require(published);
      release(publication->value.request_id, false);
      publication = publications.erase(publication);
    }
  }

  void capture_snapshots(RocketDecodedFrame &output) {
    for (const auto &snapshot : capture->take_ready()) {
      const auto token = uid_tokens.find(snapshot.id.uid);
      if (token == uid_tokens.end())
        throw std::invalid_argument("Rocket captured retirement has no logical native identity");
      auto &live = instruction(token->second);
      live.snapshot_delivered = true;
      output.retirements.push_back({snapshot.id, snapshot.pc, snapshot.instruction,
          snapshot.integer_write ? rvwmo::ArchitecturalDestination::Integer : rvwmo::ArchitecturalDestination::None,
          snapshot.destination, snapshot.value});
    }
    for (auto live = instructions.begin(); live != instructions.end();) {
      if (live->second.emitted && live->second.snapshot_delivered && !live->second.request) {
        uid_tokens.erase(live->second.id.uid);
        live = instructions.erase(live);
      } else {
        ++live;
      }
    }
  }

  void close_prefix(uint64_t cycle, RocketDecodedFrame &output) {
    output.batch = emitter->take_batch();
    std::optional<uint64_t> earliest;
    for (const auto &entry : instructions)
      if (!entry.second.emitted)
        earliest = !earliest ? entry.second.allocation.source.cycle
                            : std::min(*earliest, entry.second.allocation.source.cycle);
    if (!earliest || *earliest != 0) {
      const auto complete = earliest ? std::min(cycle, *earliest - 1) : cycle;
      if (!last_closed || complete > *last_closed) {
        require(emitter->close_through(complete));
        auto closure = emitter->take_batch();
        output.batch.watermarks = std::move(closure.watermarks);
        last_closed = complete;
      }
      output.hart_closed_through = complete;
    }
    auto fabric = cycle;
    for (const auto &publication : publications) {
      if (publication.value.source.cycle == 0) {
        output.publication_closed_through.reset();
        return;
      }
      fabric = std::min(fabric, publication.value.source.cycle - 1);
    }
    output.publication_closed_through = fabric;
    output.tracked_work_drained = instructions.empty() && requests.empty() && publications.empty();
  }

  CheckResult decode(const NativeFrame &frame, RocketDecodedFrame &output) {
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
      if (!started)
        initialize(sample, frame.epoch);
      if (frame.epoch != epoch || (last_cycle && (*last_cycle == UINT64_MAX || frame.source_cycle != *last_cycle + 1)))
        throw std::invalid_argument("Rocket native source epoch or cycle changed discontinuously");
      if (fields.core_requestor.read(sample) != core_requestor || fields.cache_requestors.read(sample) != requestors)
        throw std::invalid_argument("Rocket native cache requestor roster changed");
      last_cycle = frame.source_cycle;
      if (fields.trap.valid.read(sample))
        throw CheckResult{CheckStatus::Unsupported, 0, "Rocket trap or interrupt requires the shared architectural trap protocol"};
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
      if (fields.late_write.valid.read(sample))
        require(capture->late_write({fields.late_token.read(sample), static_cast<bool>(fields.late_known.read(sample)),
            narrow_byte(fields.late_destination.read(sample)), fields.late_data.read(sample), source(frame.source_cycle)}));
      emit_ready();
      RocketDecodedFrame converted;
      capture_snapshots(converted);
      close_prefix(frame.source_cycle, converted);
      output = std::move(converted);
      return {};
    } catch (const CheckResult &error) {
      return remember(error);
    } catch (const std::invalid_argument &error) {
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
  if (!state_->started)
    return {CheckStatus::Incomplete, 0, "Rocket native source never left reset"};
  for (const auto &result : {state_->emitter->finalize(), state_->capture->finalize(), state_->tracker->finalize()})
    if (!result)
      return {result.status == CheckStatus::Pending ? CheckStatus::Incomplete : result.status, result.rule, result.message};
  if (!state_->instructions.empty() || !state_->requests.empty() || !state_->publications.empty())
    return {CheckStatus::Incomplete, 0, "Rocket native decoder has undrained instruction or cache facts"};
  return {};
}

} // namespace difftest_s::native
