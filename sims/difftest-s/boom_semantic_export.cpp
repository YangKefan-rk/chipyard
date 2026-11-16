#include "boom_semantic_export.h"
#include "boom_semantic_long_export.h"
#include "rvwmo/host_session.h"
#include "rvwmo/producer_ids.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unistd.h>

namespace difftest_s::native {
namespace {
constexpr std::string_view kClock = "boom_tile_clock_posedge_pre_nba";
constexpr size_t kMaxObserved = 100000;
constexpr size_t kLineBytes = 64;

std::string required(const char *name) {
  const char *value = std::getenv(name);
  if (!value || !*value)
    throw std::invalid_argument(std::string("BOOM semantic feedback requires ") + name);
  return value;
}

bool chunked_export_requested() {
  const char *width = std::getenv("DIFFTEST_S_SEMANTIC_CHUNK_CYCLES");
  return width && *width;
}

uint64_t number(const char *name) {
  const auto value = required(name);
  uint64_t parsed = 0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc() || result.ptr != value.data() + value.size())
    throw std::invalid_argument(std::string("BOOM semantic feedback has invalid ") + name);
  return parsed;
}

void digest(const std::string &value, const char *name) {
  if (value.size() != 64 || !std::all_of(value.begin(), value.end(), [](char digit) {
        return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
      }))
    throw std::invalid_argument(std::string("BOOM semantic feedback has invalid digest ") + name);
}

std::string json_string(std::string_view input) {
  std::ostringstream output;
  output << '"';
  constexpr char hex[] = "0123456789abcdef";
  for (const unsigned char byte : input) {
    if (byte == '"' || byte == '\\') output << '\\' << static_cast<char>(byte);
    else if (byte < 0x20) output << "\\u00" << hex[byte >> 4] << hex[byte & 15];
    else output << static_cast<char>(byte);
  }
  output << '"';
  return output.str();
}

uint32_t byte_mask(const std::vector<uint8_t> &lanes, uint64_t address) {
  if (lanes.empty() || lanes.size() > 8)
    throw std::invalid_argument("BOOM semantic byte mask exceeds one scalar data word");
  uint32_t mask = 0;
  for (size_t lane = 0; lane < lanes.size(); ++lane)
    if (lanes[lane]) {
      if (address % kLineBytes + lane >= kLineBytes)
        throw std::invalid_argument("BOOM semantic byte mask crosses the physical cache line");
      mask |= uint32_t{1} << lane;
    }
  if (!mask)
    throw std::invalid_argument("BOOM semantic observation has no enabled byte");
  return mask;
}

struct Producer {
  uint32_t id;
  std::string name;
};
std::array<Producer, 8> producers() {
  std::array<Producer, 8> roster{};
  for (uint32_t hart = 0; hart < 4; ++hart) {
    roster[hart] = {rvwmo::hart_producer_id(hart), "h" + std::to_string(hart)};
    roster[hart + 4] = {rvwmo::memory_fabric_producer_id(hart), "f" + std::to_string(hart)};
  }
  return roster;
}

std::string producer_name(uint32_t id) {
  for (const auto &producer : producers())
    if (producer.id == id)
      return producer.name;
  throw std::invalid_argument("BOOM semantic event refers to an unregistered source");
}

using rvwmo::OperationId;
using rvwmo::RuntimeEvent;
using rvwmo::RuntimeEventKind;

// Memory operation ID includes subaccess; instruction retirement ID does not.
OperationId instruction_id(OperationId id) {
  id.subaccess = 0;
  return id;
}

struct RetiredMemory {
  uint64_t pc = 0;
  uint32_t instruction = 0;
  uint64_t retirement_source_cycle = 0;
  rvwmo::MemoryOperation operation;
};

void write_exclusive(const std::string &path, const std::string &contents) {
  const std::string temporary = path + ".partial." + std::to_string(::getpid());
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (fd < 0)
    throw std::runtime_error("cannot create BOOM semantic temporary output");
  bool closed = false;
  try {
    size_t offset = 0;
    while (offset < contents.size()) {
      const ssize_t count = ::write(fd, contents.data() + offset, contents.size() - offset);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) throw std::runtime_error("failed to write BOOM semantic output");
      offset += static_cast<size_t>(count);
    }
    if (::fsync(fd) != 0)
      throw std::runtime_error("failed to sync BOOM semantic output");
    const int close_result = ::close(fd);
    closed = true;
    if (close_result != 0)
      throw std::runtime_error("failed to close BOOM semantic output");
    // link is atomic and refuses to replace an existing case-local artifact.
    if (::link(temporary.c_str(), path.c_str()) != 0)
      throw std::runtime_error("BOOM semantic output already exists or cannot be published");
    ::unlink(temporary.c_str());
  } catch (...) {
    if (!closed) ::close(fd);
    ::unlink(temporary.c_str());
    throw;
  }
}

} // namespace

BoomSemanticExport::BoomSemanticExport(const char *elf)
    : output_(required("DIFFTEST_S_SEMANTIC_OUTPUT")),
      case_id_(required("SFUZZ_FUNCTIONAL_CASE_ID")),
      run_id_(required("SFUZZ_FUNCTIONAL_RUN_ID")),
      elf_sha256_(required("SFUZZ_FUNCTIONAL_ELF_SHA256")),
      backend_sha256_(required("SFUZZ_FUNCTIONAL_SIMULATOR_SHA256")),
      layout_sha256_(required("DIFFTEST_S_SEMANTIC_LAYOUT_SHA256")),
      last_cycle_(number(chunked_export_requested()
          ? "DIFFTEST_S_SEMANTIC_MAX_CYCLE_BUDGET" : "DIFFTEST_S_SEMANTIC_LAST_CYCLE")) {
  if (!elf || !*elf || !std::filesystem::is_regular_file(elf) || run_id_.size() < 16 ||
      !last_cycle_ || case_id_.empty())
    throw std::invalid_argument("BOOM semantic export lacks a pinned ELF, run or window");
  digest(elf_sha256_, "ELF");
  digest(backend_sha256_, "simulator");
  digest(layout_sha256_, "layout");
  const std::filesystem::path output(output_);
  const std::filesystem::path functional(required("SFUZZ_FUNCTIONAL_OUTPUT"));
  if (!output.is_absolute() || !functional.is_absolute() ||
      std::filesystem::weakly_canonical(output.parent_path()) !=
          std::filesystem::weakly_canonical(functional.parent_path()) ||
      output.filename() != "interaction-feedback.json")
    throw std::invalid_argument("BOOM semantic output must share its pinned functional case directory");
  if (chunked_export_requested()) {
    if (std::getenv("DIFFTEST_S_SEMANTIC_LAST_CYCLE"))
      throw std::invalid_argument("BOOM semantic v2 uses MAX_CYCLE_BUDGET, not LAST_CYCLE");
    const uint64_t chunk_cycles = number("DIFFTEST_S_SEMANTIC_CHUNK_CYCLES");
    const uint64_t first_cycle = number("DIFFTEST_S_SEMANTIC_FIRST_CYCLE");
    const uint64_t max_pair_gap_cycles = number("DIFFTEST_S_SEMANTIC_MAX_PAIR_GAP_CYCLES");
    if (!chunk_cycles || chunk_cycles > 1048576)
      throw std::invalid_argument("BOOM semantic chunk cycles must be within 1..1048576");
    long_export_ = std::make_unique<BoomSemanticLongExport>(output_, case_id_, run_id_,
        elf_sha256_, backend_sha256_, layout_sha256_, first_cycle, last_cycle_, chunk_cycles, max_pair_gap_cycles);
  }
}

BoomSemanticExport::~BoomSemanticExport() = default;

void BoomSemanticExport::observe(const rvwmo::RuntimeEvent &event) {
  if (long_export_) {
    if (published_) throw std::runtime_error("BOOM semantic export was already published");
    long_export_->observe(event);
    return;
  }
  if (published_ || observed_.size() >= kMaxObserved)
    throw std::runtime_error("BOOM semantic observer exhausted its bounded case-local storage");
  observed_.push_back(event);  // Never retain the ephemeral checker callback reference.
}

void BoomSemanticExport::observe_ordered(const rvwmo::RuntimeEvent &event,
                                         const rvwmo::ObservationTime &order) {
  if (long_export_ || (event.kind != RuntimeEventKind::LoadSample &&
                       event.kind != RuntimeEventKind::StorePublish))
    return;
  if (published_ || !order.valid || !order.rank || ordered_.size() >= kMaxObserved ||
      !ordered_.emplace(std::make_pair(event.producer_id, event.source.producer_sequence), order).second)
    throw std::runtime_error("BOOM semantic accepted effect has no unique checker GMO rank");
}

void BoomSemanticExport::publish(const rvwmo::HostSession &session) {
  const auto completed = session.semantic_completed_cycle();
  if (published_ || !session.semantic_feedback_publishable() ||
      (!long_export_ && !session.ordered_semantic_observer_healthy()) || !completed ||
      (long_export_ ? !*completed || *completed > last_cycle_ : *completed < last_cycle_))
    throw std::runtime_error("BOOM semantic export requires a complete independent checker Pass within its pinned horizon");
  const uint64_t horizon = long_export_ ? *completed : last_cycle_;
  const auto roster = producers();
  const auto &watermarks = session.semantic_source_watermarks();
  if (watermarks.size() != roster.size())
    throw std::runtime_error("BOOM semantic source roster is incomplete");
  for (const auto &producer : roster) {
    const auto watermark = watermarks.find(producer.id);
    if (watermark == watermarks.end() || watermark->second.cycle < horizon)
      throw std::runtime_error("BOOM semantic source watermark does not close the scoring horizon");
  }
  if (long_export_) {
    long_export_->publish(watermarks, horizon);
    published_ = true;
    return;
  }
  std::map<OperationId, rvwmo::RetiredInstruction> retired;
  std::map<OperationId, RetiredMemory> memory;
  std::set<OperationId> squashed;
  std::set<std::pair<OperationId, uint32_t>> canceled;
  for (const auto &event : observed_) {
    if (event.kind == RuntimeEventKind::InstructionCancelAttempt) {
      const auto &entry = std::get<rvwmo::InstructionCancelAttemptEvent>(event.payload);
      canceled.emplace(entry.id, entry.attempt);
    } else if (event.kind == RuntimeEventKind::InstructionSquash) {
      squashed.insert(std::get<rvwmo::InstructionSquashEvent>(event.payload).id);
    } else if (event.kind == RuntimeEventKind::InstructionRetire) {
      const auto &instruction = std::get<rvwmo::InstructionRetireEvent>(event.payload).instruction;
      const OperationId id{instruction.hart, instruction.epoch, instruction.uid, 0};
      if (!retired.emplace(id, instruction).second)
        throw std::runtime_error("BOOM semantic instruction retired twice");
      for (const auto &operation : instruction.memory)
        if (!memory.emplace(operation.id, RetiredMemory{
                instruction.pc, instruction.instruction, event.source.cycle, operation}).second)
          throw std::runtime_error("BOOM semantic retired memory operation repeated");
    }
  }
  std::set<std::pair<OperationId, uint32_t>> emitted_loads;
  std::set<std::pair<uint32_t, uint64_t>> emitted_sources;
  std::ostringstream output;
  output << "{\"schema_version\":1,\"kind\":\"difftest-s-semantic.v1\",\"case_id\":"
         << json_string(case_id_) << ",\"run_id\":" << json_string(run_id_)
         << ",\"elf_sha256\":" << json_string(elf_sha256_)
         << ",\"backend_sha256\":" << json_string(backend_sha256_)
         << ",\"layout_sha256\":" << json_string(layout_sha256_)
         << ",\"source_clock\":" << json_string(kClock)
         << ",\"epoch\":1,\"complete\":true,\"required_producers\":[";
  for (size_t index = 0; index < roster.size(); ++index) {
    if (index) output << ',';
    output << json_string(roster[index].name);
  }
  output << "],\"producer_watermarks\":{";
  for (size_t index = 0; index < roster.size(); ++index) {
    if (index) output << ',';
    output << json_string(roster[index].name)
           << ":{\"epoch\":1,\"source_clock\":" << json_string(kClock)
           << ",\"complete\":true,\"lossless\":true,\"closed_through_cycle\":"
           << watermarks.at(roster[index].id).cycle
           << ",\"event_sequence\":" << watermarks.at(roster[index].id).producer_sequence << '}';
  }
  output << "},\"events\":[";
  bool first = true;
  size_t loads = 0, stores = 0;
  std::set<uint16_t> phases;
  for (const RuntimeEvent &event : observed_) {
    if (event.source.cycle > last_cycle_ || (event.kind != RuntimeEventKind::LoadSample &&
                                            event.kind != RuntimeEventKind::StorePublish))
      continue;
    std::ostringstream row;
    const uint64_t cycle = event.source.cycle;
    const auto &position = event.source;
    if (!position.producer_sequence || position.qualified_phase == rvwmo::SourcePosition::cycle_closed_phase)
      throw std::runtime_error("BOOM semantic source position is unqualified or missing");
    const std::string producer = producer_name(event.producer_id);
    if (event.kind == RuntimeEventKind::LoadSample) {
      const auto &sample = std::get<rvwmo::LoadSampleEvent>(event.payload).operation;
      const auto id = sample.id;
      const auto instruction = retired.find(instruction_id(id));
      if (instruction == retired.end()) continue; // Replayed or squashed attempt.
      const auto operation = memory.find(id);
      if (operation == memory.end() || !operation->second.operation.load ||
          operation->second.operation.atomic || operation->second.operation.lr ||
          operation->second.operation.sc || sample.attempt != operation->second.operation.attempt)
        continue;
      if (squashed.count(instruction_id(id)) || canceled.count({id, sample.attempt}))
        throw std::runtime_error("BOOM semantic final load was canceled or squashed");
      if (sample.address != operation->second.operation.address ||
          sample.byte_mask != operation->second.operation.byte_mask ||
          id.epoch != 1 || id.uid == 0 || id.hart >= 4 ||
          !emitted_loads.emplace(id, sample.attempt).second)
        throw std::runtime_error("BOOM semantic final load identity or address disagrees with retirement");
      ++loads;
      row << "\"kind\":\"retired_load_sample\",\"retired\":true,\"hart\":" << id.hart
          << ",\"uid\":" << id.uid << ",\"attempt\":" << sample.attempt
          << ",\"subaccess\":" << id.subaccess << ",\"pc\":" << instruction->second.pc
          << ",\"instruction\":" << operation->second.instruction
          << ",\"retirement_source_cycle\":" << operation->second.retirement_source_cycle
          << ",\"physical_address\":" << sample.address
          << ",\"address_is_physical\":true,\"byte_mask\":" << byte_mask(sample.byte_mask, sample.address);
    } else {
      const auto &store = std::get<rvwmo::StorePublishEvent>(event.payload);
      if (!store.transaction_id || !store.allocation_generation || store.members.empty() ||
          store.byte_mask.size() != store.bytes.size())
        throw std::runtime_error("BOOM semantic store lacks observed publication identity or byte lanes");
      const uint32_t hart = store.members.front().id.hart;
      if (hart >= 4 || (store.members.size() > 1 && !store.coalesced_program_order))
        throw std::runtime_error("BOOM semantic store group lacks a supported physical publication certificate");
      row << "\"kind\":\"store_publish_group\",\"hart\":" << hart
          << ",\"physical_address\":" << store.address
          << ",\"address_is_physical\":true,\"byte_mask\":" << byte_mask(store.byte_mask, store.address)
          << ",\"transaction_id\":" << store.transaction_id
          << ",\"allocation_generation\":" << store.allocation_generation
          << ",\"retired_members\":[";
      for (size_t index = 0; index < store.members.size(); ++index) {
        const auto &member = store.members[index];
        const auto found = memory.find(member.id);
        if (member.id.hart != hart || member.id.epoch != 1 || !member.id.uid ||
            !member.memory_generation || found == memory.end() ||
            !found->second.operation.store || found->second.operation.atomic ||
            found->second.operation.attempt != member.attempt ||
            canceled.count({member.id, member.attempt}) || squashed.count(instruction_id(member.id)))
          throw std::runtime_error("BOOM semantic store member lacks its real retired writer attempt");
        const auto &logical = found->second.operation;
        byte_mask(logical.byte_mask, logical.address); // Validate the original scalar footprint.
        if (store.byte_writers.size() != store.byte_mask.size())
          throw std::runtime_error("BOOM coalesced group lacks a physical byte-writer roster");
        uint32_t visible_member_mask = 0;
        for (size_t byte = 0; byte < store.byte_mask.size(); ++byte) {
          if (!store.byte_mask[byte] || store.byte_writers[byte] != member.id)
            continue;
          const uint64_t address = store.address + byte;
          if (address < logical.address || address - logical.address >= logical.byte_mask.size() ||
              !logical.byte_mask[address - logical.address])
            throw std::runtime_error("BOOM physical byte writer differs from the retired member");
          visible_member_mask |= uint32_t{1} << (address - logical.address);
        }
        if (index) row << ',';
        row << "{\"hart\":" << hart << ",\"uid\":" << member.id.uid
            << ",\"attempt\":" << member.attempt
            << ",\"subaccess\":" << member.id.subaccess
            << ",\"memory_generation\":" << member.memory_generation
            << ",\"retired\":true,\"pc\":" << found->second.pc
            << ",\"instruction\":" << found->second.instruction
            << ",\"retirement_source_cycle\":" << found->second.retirement_source_cycle
            << ",\"physical_address\":" << logical.address
            << ",\"byte_mask\":" << visible_member_mask
            << ",\"byte_provenance_verified\":true}";
      }
      row << ']';
      ++stores;
    }
    phases.insert(position.qualified_phase);
    if (!emitted_sources.emplace(event.producer_id, position.producer_sequence).second)
      throw std::runtime_error("BOOM semantic producer sequence was exported twice");
    const auto accepted = ordered_.find({event.producer_id, position.producer_sequence});
    if (accepted == ordered_.end())
      throw std::runtime_error("BOOM final semantic effect lacks a checker-accepted GMO rank");
    const auto &order = accepted->second;
    if (!first) output << ',';
    first = false;
    output << "{\"producer_id\":" << json_string(producer)
           << ",\"producer_sequence\":" << position.producer_sequence
           << ",\"source_cycle\":" << cycle
           << ",\"qualified_phase\":" << position.qualified_phase
           << ",\"source_clock\":" << json_string(kClock)
           << ",\"epoch\":1,\"final_attempt\":true,\"gmo\":{\"rank\":" << order.rank
           << ",\"unordered_read_group\":" << (order.unordered_read_group ? "true" : "false")
           << ",\"unordered_effect_group\":" << (order.unordered_effect_group ? "true" : "false")
           << ",\"isolated_store_group\":" << (order.isolated_store_group ? "true" : "false")
           << "}," << row.str() << '}';
  }
  output << ']';
  std::array<uint64_t, 4> retired_counts{}, ordinals{}, last_sequences{};
  std::array<bool, 4> source_ordered{};
  source_ordered.fill(true);
  for (const auto &event : observed_)
    if (event.kind == RuntimeEventKind::InstructionRetire) {
      const auto &instruction = std::get<rvwmo::InstructionRetireEvent>(event.payload).instruction;
      if (instruction.hart >= 4 || event.producer_id != rvwmo::hart_producer_id(instruction.hart) ||
          event.source.cycle > *completed || !event.source.producer_sequence)
        throw std::runtime_error("BOOM retirement has an unsupported source position");
      auto &prior = last_sequences[instruction.hart];
      source_ordered[instruction.hart] &= event.source.producer_sequence > prior;
      prior = event.source.producer_sequence;
      ++retired_counts[instruction.hart];
    }
  bool retirement_complete = true;
  const auto &statistics = session.session()->hart_statistics();
  for (uint32_t hart = 0; hart < 4; ++hart) {
    const auto count = statistics.find(hart);
    const auto mark = watermarks.find(rvwmo::hart_producer_id(hart));
    retirement_complete &= retired_counts[hart] && source_ordered[hart] &&
                           count != statistics.end() &&
                           count->second.retired_instructions == retired_counts[hart] &&
                           mark != watermarks.end() && mark->second.cycle >= *completed;
  }
  output << ",\"retirement_trace_complete\":" << (retirement_complete ? "true" : "false");
  if (retirement_complete) {
    output << ",\"retirement_source_clock\":" << json_string(kClock)
           << ",\"retirement_closed_through_cycle\":" << *completed
           << ",\"retirements\":[";
    bool first_retirement = true;
    for (const auto &event : observed_)
      if (event.kind == RuntimeEventKind::InstructionRetire) {
        const auto &instruction = std::get<rvwmo::InstructionRetireEvent>(event.payload).instruction;
        if (!first_retirement) output << ',';
        first_retirement = false;
        output << "{\"hart\":" << instruction.hart << ",\"epoch\":" << instruction.epoch
               << ",\"uid\":" << instruction.uid << ",\"pc\":" << instruction.pc
               << ",\"instruction\":" << instruction.instruction
               << ",\"source_cycle\":" << event.source.cycle
               << ",\"producer_id\":" << json_string(producer_name(event.producer_id))
               << ",\"producer_sequence\":" << event.source.producer_sequence
               << ",\"program_order\":" << ordinals[instruction.hart]++ << '}';
      }
    output << ']';
  }
  output << '}';
  write_exclusive(output_, output.str());
  published_ = true;
  std::fprintf(stderr, "DIFFTEST_S_SEMANTIC_EXPORT {\"loads\":%zu,\"stores\":%zu,\"last_cycle\":%llu,\"phases\":[",
               loads, stores, static_cast<unsigned long long>(last_cycle_));
  bool first_phase = true;
  for (const auto phase : phases) {
    std::fprintf(stderr, "%s%u", first_phase ? "" : ",", static_cast<unsigned>(phase));
    first_phase = false;
  }
  std::fprintf(stderr, "]}\n");
}

} // namespace difftest_s::native
