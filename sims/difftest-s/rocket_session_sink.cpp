#include "rocket_native_decoder.h"
#include "rvwmo/dependency.h"
#include "rvwmo/host_session.h"
#include "rvwmo/memory_image.h"
#include "rvwmo/producer_ids.h"
#include <verilated.h>
#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

namespace difftest_s::native {
namespace {
using rvwmo::CheckResult;
using rvwmo::CheckStatus;

uint64_t configured_number(const char *name, uint64_t fallback) {
  const char *text = std::getenv(name);
  if (!text || !*text)
    return fallback;
  const std::string value(text);
  const bool hexadecimal = value.size() > 2 && value.substr(0, 2) == "0x";
  const char *begin = value.data() + (hexadecimal ? 2 : 0);
  uint64_t number = 0;
  const auto parsed = std::from_chars(begin, value.data() + value.size(), number, hexadecimal ? 16 : 10);
  if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size())
    throw std::invalid_argument(std::string("invalid native simulation setting ") + name);
  return number;
}

class RocketSessionSink {
public:
  explicit RocketSessionSink(const char *elf) {
    const auto *layout = find_native_layout(1, 0);
    if (!layout)
      throw std::invalid_argument("Rocket Runtime sink requires matching generated native layouts");
    const auto ram_base = configured_number("DIFFTEST_S_RAM_BASE", UINT64_C(0x80000000));
    const auto ram_size = configured_number("DIFFTEST_S_RAM_SIZE", UINT64_C(0x10000000));
    const auto scoped = configured_number("DIFFTEST_S_START_AT_ELF", 0);
    if (scoped > 1 || !ram_size || ram_size > UINT64_C(0x100000000) || !memory_.initialize(ram_base, ram_size))
      throw std::invalid_argument("Rocket Runtime sink requires bounded initialized RAM and an explicit scope");
    uint64_t entry = 0;
    auto loaded = rvwmo::load_reference_elf(memory_, elf, entry);
    if (!loaded)
      throw std::invalid_argument("independent reference ELF: " + loaded.message);

    rvwmo::HostSessionConfiguration config;
    config.platform = "rocket";
    config.reference_profile = DIFFTEST_REF_PROFILE_ID_ROCKET_CHIP;
    elf_scope_ = scoped;
    if (scoped)
      config.verification_scope =
          "ELF-entry M-mode scalar cached RAM; ROM/bootstrap CSR/MMIO/interrupts are uncovered";
    config.initial_harts.resize(layout->harts);
    initialized_registers_.resize(layout->harts);
    publication_sequences_.resize(layout->harts);
    source_prefixes_.resize(layout->harts * 2);
    const std::vector<rvwmo::RuntimeEventKind> hart_kinds = {
      rvwmo::RuntimeEventKind::InstructionAllocate, rvwmo::RuntimeEventKind::InstructionRetire,
      rvwmo::RuntimeEventKind::InstructionCancelAttempt, rvwmo::RuntimeEventKind::InstructionSquash,
      rvwmo::RuntimeEventKind::LocalStore, rvwmo::RuntimeEventKind::LoadSample};
    for (uint32_t hart = 0; hart < layout->harts; ++hart) {
      const auto *hart_layout = find_native_layout(1, hart);
      if (!hart_layout || hart_layout->harts != layout->harts)
        throw std::invalid_argument("Rocket Runtime sink has an incomplete generated hart roster");
      auto &initial = config.initial_harts[hart];
      initial.pc = entry;
      initial.privilege = 3;
      // Every other GPR must be established by independent REF execution before
      // use. In particular, mhartid is read by Spike rather than copied from DUT.
      initialized_registers_[hart][0] = true;
      const auto producer = rvwmo::hart_producer_id(hart);
      const auto fabric = rvwmo::memory_fabric_producer_id(hart);
      config.stream.required_producers.push_back({producer, hart_kinds});
      config.producers.push_back({producer, true, true, true, hart_kinds});
      config.stream.required_producers.push_back({fabric, {rvwmo::RuntimeEventKind::StorePublish}});
      config.producers.push_back({fabric, true, true, true, {rvwmo::RuntimeEventKind::StorePublish}});
      RocketDecoderConfiguration source_config;
      source_config.publication_producer = fabric;
      source_config.entry_pc = scoped ? entry : 0;
      decoders_.push_back(std::make_unique<RocketNativeDecoder>(*hart_layout, source_config,
          [this, hart](uint64_t cycle) {
            auto &sequence = publication_sequences_[hart];
            if (sequence == UINT64_MAX)
              throw std::invalid_argument("Rocket native publication source sequence exhausted");
            return rvwmo::SourcePosition{cycle, 0, ++sequence, false};
          }));
    }
    session_ = std::make_unique<rvwmo::HostSession>(memory_, std::move(config));
    if (!session_->configuration())
      throw std::invalid_argument("Rocket Runtime session: " + session_->configuration().message);
  }

  CheckResult capture(const NativeFrame &frame) {
    if (!failure_)
      return failure_;
    cycle_ = frame.source_cycle;
    if (!frame.reset && !elf_scope_)
      return fail({CheckStatus::Unsupported, 0,
          "Rocket whole-boot checking requires independent ROM state and the shared trap/interrupt protocol"});
    for (uint32_t hart = 0; hart < decoders_.size(); ++hart) {
      RocketDecodedFrame ready;
      const auto decoded = decoders_[hart]->decode(frame, ready);
      if (!decoded) {
        if (decoded.status == CheckStatus::Pending && frame.reset)
          continue;
        return fail(decoded);
      }
      if (ready.batch.events.size() > staged_limit - std::min(staged_limit, staged_events_.size()))
        return fail({CheckStatus::Incomplete, 0, "Rocket native event staging limit exhausted"});
      staged_events_.insert(staged_events_.end(), ready.batch.events.begin(), ready.batch.events.end());
      for (const auto &snapshot : ready.retirements)
        if (!staged_retirements_.emplace(snapshot.id, snapshot).second)
          return fail({CheckStatus::Invalid, 0, "Rocket native retirement snapshot was delivered twice"});
      if (ready.hart_closed_through)
        source_prefixes_[hart * 2] = *ready.hart_closed_through;
      if (ready.publication_closed_through)
        source_prefixes_[hart * 2 + 1] = *ready.publication_closed_through;
    }
    if (frame.reset)
      return {CheckStatus::Pending, 0, "Rocket source is in reset"};
    auto result = submit_closed_prefix();
    if (!result && result.status != CheckStatus::Pending)
      return fail(result);
    return result;
  }

  void transport_failed(const CheckResult &result) { fail(result); }

  CheckResult finalize() {
    if (finalized_)
      return final_result_;
    finalized_ = true;
    for (uint32_t hart = 0; hart < decoders_.size(); ++hart) {
      const auto &outside = decoders_[hart]->unchecked_bootstrap();
      std::fprintf(stderr,
          "DIFFTEST-S uncovered Rocket bootstrap hart %u: instructions=%llu loads=%llu stores=%llu traps=%llu\n",
          hart, static_cast<unsigned long long>(outside.instructions), static_cast<unsigned long long>(outside.loads),
          static_cast<unsigned long long>(outside.stores), static_cast<unsigned long long>(outside.traps));
      const auto result = decoders_[hart]->finalize();
      if (!result && failure_)
        session_->report_source_failure(result);
    }
    if (failure_ && (!staged_events_.empty() || !staged_retirements_.empty()))
      session_->report_source_failure({CheckStatus::Incomplete, 0,
          "Rocket native events or architectural snapshots remain outside the delivered source prefix"});
    // Native HTIF success does not establish architectural hart completion or
    // physically idle caches. Explicit missing evidence remains Incomplete.
    final_result_ = session_->finalize(cycle_);
    std::fprintf(stderr, "DIFFTEST_S_FINAL %s\n", session_->final_report().c_str());
    return failure_ ? final_result_ : failure_;
  }

private:
  GlobalMem memory_;
  std::unique_ptr<rvwmo::HostSession> session_;
  std::vector<std::unique_ptr<RocketNativeDecoder>> decoders_;
  std::vector<std::array<bool, 32>> initialized_registers_;
  std::vector<uint64_t> publication_sequences_;
  static constexpr size_t staged_limit = 262144;
  std::vector<rvwmo::RuntimeEvent> staged_events_;
  std::map<rvwmo::OperationId, rvwmo::ArchitecturalRetirement> staged_retirements_;
  std::vector<uint64_t> source_prefixes_;
  std::optional<uint64_t> submitted_prefix_;
  uint64_t cycle_ = 0;
  CheckResult failure_, final_result_;
  bool finalized_ = false;
  bool elf_scope_ = false;

  CheckResult fail(const CheckResult &result) {
    if (failure_)
      failure_ = result;
    session_->report_source_failure(failure_);
    std::fprintf(stderr, "DIFFTEST-S Rocket source cycle %llu: status %u rule %u: %s\n",
                 static_cast<unsigned long long>(cycle_), static_cast<unsigned>(failure_.status),
                 failure_.rule, failure_.message.c_str());
    Verilated::gotFinish(true);
    return failure_;
  }

  CheckResult submit_closed_prefix() {
    const auto through = *std::min_element(source_prefixes_.begin(), source_prefixes_.end());
    if (submitted_prefix_ && through <= *submitted_prefix_)
      return {CheckStatus::Pending, 0, "Rocket native joins keep the shared source prefix open"};
    std::stable_sort(staged_events_.begin(), staged_events_.end(), [](const auto &lhs, const auto &rhs) {
      return lhs.source.cycle < rhs.source.cycle;
    });
    rvwmo::EventBatch batch;
    std::vector<rvwmo::ArchitecturalRetirement> retirements;
    std::vector<rvwmo::OperationId> retired_ids;
    for (const auto &event : staged_events_) {
      if (event.source.cycle > through)
        continue;
      batch.events.push_back(event);
      if (event.kind == rvwmo::RuntimeEventKind::InstructionRetire) {
        const auto &retired = std::get<rvwmo::InstructionRetireEvent>(event.payload).instruction;
        const rvwmo::OperationId id{retired.hart, retired.epoch, retired.uid, 0};
        const auto snapshot = staged_retirements_.find(id);
        if (snapshot == staged_retirements_.end())
          return {CheckStatus::Incomplete, 0, "Rocket closed retirement awaits independent actual writeback"};
        retirements.push_back(snapshot->second);
        retired_ids.push_back(id);
      }
    }
    for (uint32_t hart = 0; hart < decoders_.size(); ++hart) {
      batch.watermarks.push_back({rvwmo::hart_producer_id(hart), rvwmo::SourcePosition::end_of_cycle(through)});
      batch.watermarks.push_back({rvwmo::memory_fabric_producer_id(hart), rvwmo::SourcePosition::end_of_cycle(through)});
    }
    auto result = validate_entry_registers(retirements);
    if (!result)
      return result;
    const auto submitted = session_->submit(batch, retirements);
    if (!submitted.accepted)
      return submitted.check.status == CheckStatus::Pending
          ? CheckResult{CheckStatus::Incomplete, 0, "Rocket native closed prefix exceeded session capacity"}
          : submitted.check;
    staged_events_.erase(std::remove_if(staged_events_.begin(), staged_events_.end(), [through](const auto &event) {
      return event.source.cycle <= through;
    }), staged_events_.end());
    for (const auto &id : retired_ids)
      staged_retirements_.erase(id);
    submitted_prefix_ = through;
    return submitted.check;
  }

  CheckResult validate_entry_registers(const std::vector<rvwmo::ArchitecturalRetirement> &retirements) {
    for (const auto &retirement : retirements) {
      if (retirement.id.hart >= initialized_registers_.size())
        return {CheckStatus::Invalid, 0, "Rocket entry-state validation names an unknown hart"};
      rvwmo::DependencyMetadata dependencies;
      auto result = rvwmo::decode_dependencies(retirement.instruction, dependencies);
      if (!result)
        return result;
      auto &initialized = initialized_registers_[retirement.id.hart];
      for (const auto reg : dependencies.sources)
        if (reg >= initialized.size() || !initialized[reg])
          return {CheckStatus::Unsupported, 0,
                  "Rocket entry program reads a register outside its independently initialized state"};
      for (const auto reg : dependencies.destinations)
        if (reg < initialized.size())
          initialized[reg] = true;
    }
    return {};
  }
};

struct InstallRocketSession {
  InstallRocketSession() {
    const char *elf = std::getenv("DIFFTEST_S_ELF");
    if (!elf || !*elf)
      return;
    try {
      auto sink = std::make_shared<RocketSessionSink>(elf);
      if (!register_native_consumer([sink](const NativeFrame &frame) { return sink->capture(frame); },
                                    [sink] { return sink->finalize(); },
                                    [sink](const CheckResult &result) { sink->transport_failed(result); }))
        throw std::runtime_error("Rocket Runtime sink was registered more than once or after sampling started");
    } catch (const std::exception &error) {
      std::fprintf(stderr, "DIFFTEST-S Rocket Runtime initialization failed: %s\n", error.what());
      std::abort();
    }
  }
};
InstallRocketSession install;
} // namespace
} // namespace difftest_s::native
