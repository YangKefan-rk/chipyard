#include "boom_native_decoder.h"
#include "native_fields.h"
#include <array>
#include <string>
#include <utility>

namespace difftest_s::native {
namespace {
using Sample = NativeHartSample;
namespace boom = rvwmo::boom;

struct IdentityFields {
  NativeField epoch, uid, rob, memory, attempt;
  IdentityFields(const NativeFields &fields, const std::string &path)
      : epoch(fields.field(path + ".epoch")), uid(fields.field(path + ".uid")),
        rob(fields.field(path + ".rob_generation")), memory(fields.field(path + ".memory_generation")),
        attempt(fields.field(path + ".attempt")) {}
  boom::ProbeIdentity read(const Sample &sample) const {
    return {epoch.read(sample), uid.read(sample), rob.read(sample), memory.read(sample),
            static_cast<uint32_t>(attempt.read(sample))};
  }
};

struct RecordFields {
  NativeField valid, cycle;
  IdentityFields identity;
  RecordFields(const NativeFields &fields, const std::string &path)
      : valid(fields.field(path + ".valid")), cycle(fields.field(path + ".bits.source_cycle")),
        identity(fields, path + ".bits.identity") {}
  void require_current(const Sample &sample, uint64_t source_cycle) const {
    if (cycle.read(sample) != source_cycle)
      throw std::invalid_argument("BOOM native handshake cycle differs from the captured source edge");
  }
};

struct InstructionFields : RecordFields {
  NativeField pc, instruction, rob, ldq, stq, uses_ldq, uses_stq, atomic, architectural;
  NativeField size, destination_type, destination, writeback;
  InstructionFields(const NativeFields &fields, const std::string &path)
      : RecordFields(fields, path), pc(fields.field(path + ".bits.pc")),
        instruction(fields.field(path + ".bits.instruction")), rob(fields.field(path + ".bits.rob_index")),
        ldq(fields.field(path + ".bits.ldq_index")), stq(fields.field(path + ".bits.stq_index")),
        uses_ldq(fields.field(path + ".bits.uses_ldq")), uses_stq(fields.field(path + ".bits.uses_stq")),
        atomic(fields.field(path + ".bits.atomic")), architectural(fields.field(path + ".bits.architectural")),
        size(fields.field(path + ".bits.mem_size")), destination_type(fields.field(path + ".bits.destination_type")),
        destination(fields.field(path + ".bits.destination")), writeback(fields.field(path + ".bits.write_data")) {}
  boom::ProbeInstruction read(const Sample &sample) const {
    boom::ProbeInstruction result;
    result.identity = identity.read(sample);
    result.pc = pc.read(sample);
    result.instruction = instruction.read(sample);
    result.rob_index = rob.read(sample);
    result.ldq_index = ldq.read(sample);
    result.stq_index = stq.read(sample);
    result.uses_ldq = uses_ldq.read(sample);
    result.uses_stq = uses_stq.read(sample);
    result.atomic = atomic.read(sample);
    result.architectural = architectural.read(sample);
    result.log2_size = size.read(sample);
    result.destination_type = static_cast<boom::IntegerDestinationType>(destination_type.read(sample));
    result.logical_destination = destination.read(sample);
    result.writeback_value = writeback.read(sample);
    return result;
  }
};

struct QueueFields : RecordFields {
  NativeField index, load;
  QueueFields(const NativeFields &fields, const std::string &path)
      : RecordFields(fields, path), index(fields.field(path + ".bits.index")),
        load(fields.field(path + ".bits.load")) {}
  boom::ProbeQueueAllocation read(const Sample &sample) const {
    return {identity.read(sample), static_cast<uint32_t>(index.read(sample)), static_cast<bool>(load.read(sample))};
  }
};

struct AttemptFields : RecordFields {
  NativeField index, address;
  AttemptFields(const NativeFields &fields, const std::string &path)
      : RecordFields(fields, path), index(fields.field(path + ".bits.queue_index")),
        address(fields.field(path + ".bits.address")) {}
  boom::NativeAttempt read(const Sample &sample) const {
    return {identity.read(sample), static_cast<uint32_t>(index.read(sample))};
  }
};

struct LoadFields : RecordFields {
  NativeField index, address, word, size, mask, writer_mask, source_valid, source_kind;
  bool sram_capture_boundary;
  std::vector<IdentityFields> writers;
  LoadFields(const NativeFields &fields, const std::string &path, bool capture_boundary = false)
      : RecordFields(fields, path), index(fields.field(path + ".bits.queue_index")),
        address(fields.field(path + ".bits.address")), word(fields.field(path + ".bits.raw_word")),
        size(fields.field(path + ".bits.mem_size")), mask(fields.field(path + ".bits.byte_mask")),
        writer_mask(fields.field(path + ".bits.writer_mask")), source_valid(fields.field(path + ".bits.source_valid")),
        source_kind(fields.field(path + ".bits.source_kind")), sram_capture_boundary(capture_boundary) {
    for (unsigned byte = 0; byte < 8; ++byte)
      writers.emplace_back(fields, path + ".bits.writers[" + std::to_string(byte) + ']');
  }
  boom::NativeLoad read(const Sample &sample) const {
    boom::NativeLoad result;
    auto &probe = result.probe;
    probe.identity = identity.read(sample);
    probe.queue_index = index.read(sample);
    probe.address = address.read(sample);
    probe.raw_word = word.read(sample);
    probe.log2_size = size.read(sample);
    probe.byte_mask = mask.read(sample);
    probe.known_writer_mask = writer_mask.read(sample);
    for (unsigned byte = 0; byte < writers.size(); ++byte)
      probe.writers[byte] = writers[byte].read(sample);
    // Private RTL source values are defined in boom.v4.difftest.MemoryProbe.
    const auto kind = source_kind.read(sample);
    switch (kind) {
    case 1: probe.sampling_point = boom::LoadSamplingPoint::CacheDataArray; break;
    case 2: case 3: probe.sampling_point = boom::LoadSamplingPoint::StoreForwarding; break;
    case 4: probe.sampling_point = boom::LoadSamplingPoint::RefillData; break;
    default: probe.sampling_point = boom::LoadSamplingPoint::Unavailable; break;
    }
    probe.source_complete = source_valid.read(sample) &&
                            probe.sampling_point != boom::LoadSamplingPoint::Unavailable;
    result.sampled_at.cycle = cycle.read(sample);
    // Under the shared physical clock, pipeline/STQ/line-buffer forwarding
    // selects existing registers before the active array-write edge. SRAM
    // gains the same phase only with the generator's explicit capture-boundary
    // certificate; its source cycle still comes unchanged from the DUT.
    result.sampled_at.order_qualified = (kind == 1 && sram_capture_boundary) || kind == 2 || kind == 3 || kind == 4;
    return result;
  }
};

struct StoreFields : RecordFields {
  NativeField index, address, word, size, mask, atomic;
  StoreFields(const NativeFields &fields, const std::string &path)
      : RecordFields(fields, path), index(fields.field(path + ".bits.queue_index")),
        address(fields.field(path + ".bits.address")), word(fields.field(path + ".bits.word")),
        size(fields.field(path + ".bits.mem_size")), mask(fields.field(path + ".bits.byte_mask")),
        atomic(fields.field(path + ".bits.atomic")) {}
  boom::ProbeStore read(const Sample &sample) const {
    return {identity.read(sample), static_cast<uint32_t>(index.read(sample)), address.read(sample), word.read(sample),
            static_cast<uint8_t>(size.read(sample)), static_cast<uint8_t>(mask.read(sample)),
            static_cast<bool>(atomic.read(sample))};
  }
};

struct DeviceStoreFields : RecordFields {
  NativeField source_id, address, word, mask, size, tl_data, tl_mask, denied, corrupt;
  DeviceStoreFields(const NativeFields &fields, const std::string &path)
      : RecordFields(fields, path), source_id(fields.field(path + ".bits.source_id")),
        address(fields.field(path + ".bits.address")), word(fields.field(path + ".bits.word")),
        mask(fields.field(path + ".bits.byte_mask")), size(fields.field(path + ".bits.mem_size")),
        tl_data(fields.field(path + ".bits.tl_data")), tl_mask(fields.field(path + ".bits.tl_mask")),
        denied(fields.field(path + ".bits.denied")), corrupt(fields.field(path + ".bits.corrupt")) {}
  BoomDeviceStoreFact read(const Sample &sample, uint64_t source_cycle, bool request) const {
    require_current(sample, source_cycle);
    BoomDeviceStoreFact result;
    result.request = request;
    result.identity = identity.read(sample);
    result.source_cycle = cycle.read(sample);
    result.tl_source = static_cast<uint32_t>(source_id.read(sample));
    result.address = address.read(sample);
    result.word = word.read(sample);
    result.byte_mask = static_cast<uint8_t>(mask.read(sample));
    result.mem_size = static_cast<uint8_t>(size.read(sample));
    result.tl_data = tl_data.read(sample);
    result.tl_mask = static_cast<uint8_t>(tl_mask.read(sample));
    result.denied = denied.read(sample);
    result.corrupt = corrupt.read(sample);
    if (!result.identity.uid || !result.identity.epoch || (request && (result.denied || result.corrupt)))
      throw std::invalid_argument("BOOM MMIO probe has missing identity or impossible TL A status");
    return result;
  }
};

struct CsrPostFields : RecordFields {
  NativeField pc, value, address;
  CsrPostFields(const NativeFields &fields, const std::string &path)
      : RecordFields(fields, path), pc(fields.field(path + ".bits.pc")),
        value(fields.field(path + ".bits.value")),
        address(fields.field(path + ".bits.address")) {}
  boom::NativeCsrPost read(const Sample &sample) const {
    return {identity.read(sample), cycle.read(sample), pc.read(sample), value.read(sample),
            static_cast<uint16_t>(address.read(sample))};
  }
};

struct WfiParkFields {
  NativeField valid, cycle, csr_wait, rob_only_entry, interrupt_pending, writeback_active;
  NativeField marker_pc, marker_instruction, marker_architectural;
  IdentityFields marker_identity;
  WfiParkFields(const NativeFields &fields, const std::string &path)
      : valid(fields.field(path + ".valid")), cycle(fields.field(path + ".source_cycle")),
        csr_wait(fields.field(path + ".csr_wait")), rob_only_entry(fields.field(path + ".rob_only_entry")),
        interrupt_pending(fields.field(path + ".interrupt_pending")),
        writeback_active(fields.field(path + ".writeback_active")),
        marker_pc(fields.field(path + ".marker.pc")),
        marker_instruction(fields.field(path + ".marker.instruction")),
        marker_architectural(fields.field(path + ".marker.architectural")),
        marker_identity(fields, path + ".marker.identity") {}
  BoomWfiParkFacts read(const Sample &sample, uint64_t source_cycle) const {
    BoomWfiParkFacts result;
    result.available = true;
    result.valid = valid.read(sample);
    if (!result.valid)
      return result;
    result.source_cycle = cycle.read(sample);
    result.csr_wait = csr_wait.read(sample);
    result.rob_only_entry = rob_only_entry.read(sample);
    result.interrupt_pending = interrupt_pending.read(sample);
    result.writeback_active = writeback_active.read(sample);
    result.marker = marker_identity.read(sample);
    result.marker_pc = marker_pc.read(sample);
    result.marker_instruction = static_cast<uint32_t>(marker_instruction.read(sample));
    if (result.source_cycle != source_cycle || !result.marker.uid || result.marker_pc % 2 ||
        result.marker_instruction != 0x10500073 || marker_architectural.read(sample) ||
        !result.csr_wait || !result.rob_only_entry || result.interrupt_pending || result.writeback_active)
      throw std::invalid_argument("BOOM WFI park lacks a live unretired unique ROB/CSR-stall witness");
    return result;
  }
};

struct SquashFields : RecordFields {
  NativeField rob;
  SquashFields(const NativeFields &fields, const std::string &path)
      : RecordFields(fields, path), rob(fields.field(path + ".bits.rob_index")) {}
  boom::NativeSquash read(const Sample &sample) const {
    return {identity.read(sample), static_cast<uint32_t>(rob.read(sample))};
  }
};

template <class Fields, class... Options>
std::vector<Fields> vector_fields(const NativeFields &fields, std::string_view group, Options... options) {
  std::vector<Fields> result;
  for (const auto &path : fields.records(group))
    result.emplace_back(fields, path, options...);
  return result;
}

template <class Fields, class Record>
void capture_current(const Sample &sample, uint64_t cycle, const std::vector<Fields> &fields,
                     std::vector<Record> &records) {
  for (const auto &field : fields) {
    if (!field.valid.read(sample))
      continue;
    field.require_current(sample, cycle);
    records.push_back(field.read(sample));
  }
}
} // namespace

struct BoomNativeDecoder::Fields {
  const NativeProbeLayout *layout;
  BoomPublicationSource publication_source;
  NativeFields metadata;
  NativeField hart, queues_empty, cache_idle;
  RecordFields trap;
  std::vector<InstructionFields> allocate, retire;
  std::vector<QueueFields> queues;
  std::vector<AttemptFields> begin, cancel, nack, retired_load;
  std::vector<LoadFields> complete;
  std::vector<StoreFields> local_store;
  std::vector<SquashFields> squash;
  std::vector<NativeField> retire_data;
  AttemptFields release;
  StoreFields publication;
  NativeField terminal_armed, terminal_pc, terminal_reached, terminal_stopped, terminal_empty;
  NativeField terminal_writeback, terminal_interrupt, terminal_cycle, marker_cycle, marker_pc, marker_instruction;
  IdentityFields marker_identity;
  std::optional<WfiParkFields> wfi;
  std::optional<CsrPostFields> csr_post;
  std::optional<CsrPostFields> csr_write_post;
  std::vector<DeviceStoreFields> mmio_requests, mmio_acks;

  Fields(const NativeProbeLayout &source, BoomPublicationSource publications)
      : layout(&source), publication_source(std::move(publications)), metadata(source),
        hart(metadata.field("core.hartid")), queues_empty(metadata.field("lsu.queues_empty")),
        cache_idle(metadata.field("cache.idle")), trap(metadata, "core.trap"),
        allocate(vector_fields<InstructionFields>(metadata, "core.allocate")),
        retire(vector_fields<InstructionFields>(metadata, "core.retire")),
        queues(vector_fields<QueueFields>(metadata, "lsu.allocate")),
        begin(vector_fields<AttemptFields>(metadata, "lsu.begin")),
        cancel(vector_fields<AttemptFields>(metadata, "lsu.cancel")),
        nack(vector_fields<AttemptFields>(metadata, "lsu.nack")),
        retired_load(vector_fields<AttemptFields>(metadata, "lsu.retire_load")),
        complete(vector_fields<LoadFields>(metadata, "lsu.complete", source.sram_capture_boundary)),
        local_store(vector_fields<StoreFields>(metadata, "lsu.local_store")),
        squash(vector_fields<SquashFields>(metadata, "core.squash")),
        release(metadata, "lsu.store_release"), publication(metadata, "cache.store_applied"),
        terminal_armed(metadata.field("core.terminal.armed")), terminal_pc(metadata.field("core.terminal.configured_pc")),
        terminal_reached(metadata.field("core.terminal.reached")), terminal_stopped(metadata.field("core.terminal.stopped")),
        terminal_empty(metadata.field("core.terminal.core_empty")),
        terminal_writeback(metadata.field("core.terminal.writeback_active")),
        terminal_interrupt(metadata.field("core.terminal.interrupt_pending")),
        terminal_cycle(metadata.field("core.terminal.source_cycle")),
        marker_cycle(metadata.field("core.terminal.marker.source_cycle")),
        marker_pc(metadata.field("core.terminal.marker.pc")),
        marker_instruction(metadata.field("core.terminal.marker.instruction")),
        marker_identity(metadata, "core.terminal.marker.identity") {
    if (metadata.has_field("core.wfi_park.valid"))
      wfi.emplace(metadata, "core.wfi_park");
    if (metadata.has_field("core.csr_post.valid"))
      csr_post.emplace(metadata, "core.csr_post");
    if (metadata.has_field("core.csr_write_post.valid"))
      csr_write_post.emplace(metadata, "core.csr_write_post");
    const bool requests = metadata.has_field("cache.mmio_request[0].valid");
    const bool acknowledgments = metadata.has_field("cache.mmio_ack[0].valid");
    if (requests != acknowledgments)
      throw std::invalid_argument("BOOM MMIO probe lacks either its TL A or D vector");
    if (requests) {
      mmio_requests = vector_fields<DeviceStoreFields>(metadata, "cache.mmio_request");
      mmio_acks = vector_fields<DeviceStoreFields>(metadata, "cache.mmio_ack");
      if (mmio_requests.size() != mmio_acks.size())
        throw std::invalid_argument("BOOM MMIO probe source roster differs between TL A and D");
    }
    if (source.kind != 2 || !publication_source)
      throw std::invalid_argument("BOOM native decoder requires BOOM metadata and a fabric source allocator");
    for (size_t lane = 0; lane < retired_load.size(); ++lane)
      retire_data.push_back(metadata.field("lsu.retire_load_data[" + std::to_string(lane) + ']'));
  }

  void capture(const Sample &sample, boom::NativeFrame &frame) const {
    if (hart.read(sample) != layout->hart)
      throw std::invalid_argument("BOOM native probe hartid differs from its exported port");
    capture_current(sample, frame.cycle, allocate, frame.allocations);
    capture_current(sample, frame.cycle, queues, frame.queue_allocations);
    capture_current(sample, frame.cycle, begin, frame.begins);
    capture_current(sample, frame.cycle, cancel, frame.cancellations);
    capture_current(sample, frame.cycle, nack, frame.cancellations);
    capture_current(sample, frame.cycle, local_store, frame.local_stores);
    capture_current(sample, frame.cycle, retire, frame.retirements);
    frame.csr_probe_available = csr_post.has_value();
    if (csr_post && csr_post->valid.read(sample)) {
      auto observation = csr_post->read(sample);
      if (!observation.source_cycle || observation.source_cycle == UINT64_MAX ||
          observation.source_cycle + 1 != frame.cycle)
        throw std::invalid_argument("BOOM CSR observation is not one cycle after its retired owner");
      frame.csr_post = observation;
    }
    frame.csr_write_probe_available = csr_write_post.has_value();
    if (csr_write_post && csr_write_post->valid.read(sample)) {
      auto observation = csr_write_post->read(sample);
      if (!observation.source_cycle || observation.source_cycle == UINT64_MAX ||
          observation.source_cycle + 1 != frame.cycle)
        throw std::invalid_argument("BOOM counter CSR sample is not one cycle after the accepted write");
      frame.csr_write_post = observation;
    }
    capture_current(sample, frame.cycle, squash, frame.squashes);
    for (const auto &field : complete)
      if (field.valid.read(sample))
        frame.completions.push_back(field.read(sample));
    for (size_t lane = 0; lane < retired_load.size(); ++lane) {
      const auto &field = retired_load[lane];
      if (!field.valid.read(sample))
        continue;
      field.require_current(sample, frame.cycle);
      frame.retired_loads.push_back({field.identity.read(sample), static_cast<uint32_t>(field.index.read(sample)),
                                    field.address.read(sample), retire_data[lane].read(sample)});
    }
    if (release.valid.read(sample)) {
      release.require_current(sample, frame.cycle);
      frame.store_releases.push_back(release.read(sample));
    }
    if (publication.valid.read(sample)) {
      publication.require_current(sample, frame.cycle);
      // Each observed scalar data-array write carries one native STQ writer,
      // including per-request MSHR replay. This is not an ACK or a slot release.
      auto source = publication_source(frame.cycle);
      if (source.position.cycle != frame.cycle)
        throw std::invalid_argument("BOOM fabric source allocator changed the hardware publication cycle");
      // dataWriteArb.fire writes the exclusive data array at this active edge.
      // Different L1s share phase 1, so this does not order their writes.
      source.position.qualified_phase = 1;
      source.position.order_qualified = true;
      frame.publications.push_back({publication.read(sample), source,
          source.position.producer_sequence, boom::StorePublicationEvidence::ExclusiveCacheDataWrite, true});
    }
    frame.trap = trap.valid.read(sample);
    if (frame.trap)
      trap.require_current(sample, frame.cycle);
    frame.queues_empty = queues_empty.read(sample);
    frame.cache_idle = cache_idle.read(sample);
  }
};

BoomNativeDecoder::BoomNativeDecoder(const NativeProbeLayout &layout, BoomPublicationSource publication_source)
    : fields_(std::make_unique<Fields>(layout, std::move(publication_source))) {}
BoomNativeDecoder::~BoomNativeDecoder() = default;

rvwmo::CheckResult BoomNativeDecoder::decode(const NativeFrame &source, boom::NativeFrame &output) const {
  const auto hart = fields_->layout->hart;
  if (source.reset)
    return {rvwmo::CheckStatus::Pending, 0, "BOOM native source is in reset"};
  if (source.samples.size() != fields_->layout->harts || hart >= source.samples.size() ||
      source.samples[hart].layout != fields_->layout)
    return {rvwmo::CheckStatus::Invalid, 0, "BOOM native snapshot does not match the configured layout or roster"};
  try {
    boom::NativeFrame frame;
    frame.hart = hart;
    frame.epoch = source.epoch;
    frame.cycle = source.source_cycle;
    fields_->capture(source.samples[hart], frame);
    output = std::move(frame);
    return {};
  } catch (const std::invalid_argument &error) {
    return {rvwmo::CheckStatus::Invalid, 0, error.what()};
  }
}
rvwmo::CheckResult BoomNativeDecoder::terminal(const NativeFrame &source, BoomTerminalFacts &output) const {
  try {
    const auto &sample = source.samples.at(fields_->layout->hart);
    output = {bool(fields_->terminal_armed.read(sample)), bool(fields_->terminal_reached.read(sample)),
      bool(fields_->terminal_stopped.read(sample)), bool(fields_->terminal_empty.read(sample)),
      bool(fields_->terminal_writeback.read(sample)), bool(fields_->terminal_interrupt.read(sample)),
      fields_->terminal_pc.read(sample), fields_->terminal_cycle.read(sample), fields_->marker_cycle.read(sample),
      fields_->marker_identity.read(sample), fields_->marker_pc.read(sample),
      static_cast<uint32_t>(fields_->marker_instruction.read(sample))};
    if (output.source_cycle != source.source_cycle)
      return {rvwmo::CheckStatus::Invalid, 0, "BOOM terminal facts differ from the actual source edge"};
    return {};
  } catch (const std::exception &error) {
    return {rvwmo::CheckStatus::Invalid, 0, error.what()};
  }
}
rvwmo::CheckResult BoomNativeDecoder::wfi_park(const NativeFrame &source, BoomWfiParkFacts &output) const {
  try {
    output = {};
    if (!fields_->wfi)
      return {}; // Missing hardware witness remains unavailable to the host.
    output = fields_->wfi->read(source.samples.at(fields_->layout->hart), source.source_cycle);
    return {};
  } catch (const std::exception &error) {
    return {rvwmo::CheckStatus::Invalid, 0, error.what()};
  }
}
rvwmo::CheckResult BoomNativeDecoder::device_stores(const NativeFrame &source,
    std::vector<BoomDeviceStoreFact> &output, bool &probe_available) const {
  try {
    std::vector<BoomDeviceStoreFact> decoded;
    probe_available = !fields_->mmio_requests.empty();
    if (!probe_available) {
      output.clear();
      return {}; // Old RTL lacks this witness; never claim an MMIO audit.
    }
    const auto &sample = source.samples.at(fields_->layout->hart);
    for (const auto &record : fields_->mmio_requests)
      if (record.valid.read(sample))
        decoded.push_back(record.read(sample, source.source_cycle, true));
    for (const auto &record : fields_->mmio_acks)
      if (record.valid.read(sample))
        decoded.push_back(record.read(sample, source.source_cycle, false));
    output = std::move(decoded);
    return {};
  } catch (const std::exception &error) {
    return {rvwmo::CheckStatus::Invalid, 0, error.what()};
  }
}
} // namespace difftest_s::native
