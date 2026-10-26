#include "rocket_native_decoder.h"
#include "rvwmo/dependency.h"
#include "rvwmo/host_session.h"
#include "rvwmo/memory_image.h"
#include "rvwmo/producer_ids.h"
#include "rocket_terminal.h"
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

std::optional<uint64_t> optional_configured_number(const char *name) {
  const char *text = std::getenv(name);
  if (!text || !*text)
    return std::nullopt;
  return configured_number(name, 0);
}

RocketTerminalProgramContract terminal_program_contract() {
  const auto park_pc = optional_configured_number("DIFFTEST_S_TERMINAL_PC");
  const auto signature_address = optional_configured_number("DIFFTEST_S_TERMINAL_SIGNATURE_ADDRESS");
  const auto signature_value = optional_configured_number("DIFFTEST_S_TERMINAL_SIGNATURE_VALUE");
  const auto tohost_address = optional_configured_number("DIFFTEST_S_TERMINAL_TOHOST_ADDRESS");
  const auto tohost_value = optional_configured_number("DIFFTEST_S_TERMINAL_TOHOST_SUCCESS_VALUE");
  const unsigned configured = static_cast<unsigned>(park_pc.has_value()) +
      static_cast<unsigned>(signature_address.has_value()) + static_cast<unsigned>(signature_value.has_value()) +
      static_cast<unsigned>(tohost_address.has_value()) + static_cast<unsigned>(tohost_value.has_value());
  if (!configured)
    return {};
  if (configured != 5)
    throw std::invalid_argument("Rocket terminal contract requires WFI PC, signature and tohost settings together");
  RocketTerminalProgramContract contract{*park_pc, *signature_address, *signature_value, *tohost_address, *tohost_value};
  if ((contract.park_pc & 1) || !contract.signature_address || !contract.signature_value ||
      !contract.tohost_address || !contract.tohost_success_value ||
      contract.signature_address == contract.tohost_address)
    throw std::invalid_argument("Rocket terminal contract contains an invalid WFI or success-store value");
  return contract;
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
    terminal_contract_ = terminal_program_contract();
    if (scoped > 1 || !ram_size || ram_size > UINT64_C(0x100000000) || !memory_.initialize(ram_base, ram_size))
      throw std::invalid_argument("Rocket Runtime sink requires bounded initialized RAM and an explicit scope");
    if (terminal_contract_.enabled() && !scoped)
      throw std::invalid_argument("Rocket WFI terminal checking requires an explicit ELF-entry scope");
    uint64_t entry = 0;
    auto loaded = rvwmo::load_reference_elf(memory_, elf, entry);
    if (!loaded)
      throw std::invalid_argument("independent reference ELF: " + loaded.message);
    if (terminal_contract_.enabled()) {
      std::array<uint8_t, 4> instruction{};
      if (!memory_.read(terminal_contract_.park_pc, instruction.data(), instruction.size()) ||
          instruction != std::array<uint8_t, 4>{0x73, 0x00, 0x50, 0x10})
        throw std::invalid_argument("Rocket terminal PC is not an initialized canonical WFI");
    }

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
    terminal_facts_.resize(layout->harts);
    terminal_fact_cycles_.resize(layout->harts);
    terminal_cache_idle_.resize(layout->harts, false);
    terminal_adapter_drained_.resize(layout->harts, false);
    terminal_observer_ = std::make_unique<RocketTerminalObserver>(layout->harts, terminal_contract_.park_pc);
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
      source_config.require_terminal_observations = terminal_contract_.enabled();
      decoders_.push_back(std::make_unique<RocketNativeDecoder>(*hart_layout, source_config,
          [this, hart](uint64_t cycle) {
            auto &sequence = publication_sequences_[hart];
            if (sequence == UINT64_MAX)
              throw std::invalid_argument("Rocket native publication source sequence exhausted");
            return rvwmo::SourcePosition{cycle, 0, ++sequence, false};
          }));
    }
    if (terminal_contract_.enabled()) {
      config.reference_store_observer = [this](const rvwmo::ReferenceStoreValue &store) {
        return observe_reference_store(store);
      };
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
      if (terminal_contract_.enabled() && (!ready.terminal || !ready.physically_idle))
        return fail({CheckStatus::Unsupported, 0, "Rocket WFI terminal facts are incomplete in the generated layout"});
      if (ready.terminal) {
        terminal_facts_[hart] = ready.terminal;
        terminal_fact_cycles_[hart] = frame.source_cycle;
        terminal_cache_idle_[hart] = ready.physically_idle.value_or(false);
        terminal_adapter_drained_[hart] = ready.tracked_work_drained;
      }
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
    result = observe_terminal_cycle(frame.source_cycle);
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
    if (failure_ && terminal_contract_.enabled()) {
      const auto terminal_result = complete_terminal_session();
      if (!terminal_result && terminal_result.status != CheckStatus::Incomplete)
        session_->report_source_failure(terminal_result);
    }
    // Without the configured and observed terminal contract, finalization stays Incomplete.
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
  RocketTerminalProgramContract terminal_contract_;
  std::unique_ptr<RocketTerminalObserver> terminal_observer_;
  std::vector<std::optional<RocketTerminalFacts>> terminal_facts_;
  std::vector<uint64_t> terminal_fact_cycles_;
  std::vector<bool> terminal_cache_idle_;
  std::vector<bool> terminal_adapter_drained_;
  std::optional<uint64_t> signature_store_uid_;
  std::optional<uint64_t> tohost_store_uid_;
  static constexpr size_t staged_limit = 262144;
  std::vector<rvwmo::RuntimeEvent> staged_events_;
  std::map<rvwmo::OperationId, rvwmo::ArchitecturalRetirement> staged_retirements_;
  std::vector<uint64_t> source_prefixes_;
  std::optional<uint64_t> submitted_prefix_;
  uint64_t cycle_ = 0;
  CheckResult failure_, final_result_;
  bool finalized_ = false;
  bool elf_scope_ = false;

  rvwmo::CheckResult observe_reference_store(const rvwmo::ReferenceStoreValue &store) {
    if (store.id.hart != 0 || store.bytes.size() != sizeof(uint64_t) ||
        store.byte_mask.size() != store.bytes.size() ||
        std::any_of(store.byte_mask.begin(), store.byte_mask.end(), [](uint8_t enabled) { return enabled != 1; }))
      return {};
    uint64_t value = 0;
    for (size_t index = 0; index < store.bytes.size(); ++index)
      value |= static_cast<uint64_t>(store.bytes[index]) << (index * 8);
    if (store.address == terminal_contract_.signature_address && value == terminal_contract_.signature_value)
      signature_store_uid_ = store.id.uid;
    if (store.address == terminal_contract_.tohost_address && value == terminal_contract_.tohost_success_value &&
        signature_store_uid_ && *signature_store_uid_ < store.id.uid)
      tohost_store_uid_ = store.id.uid;
    return {};
  }

  rvwmo::CheckResult observe_terminal_cycle(uint64_t cycle) {
    if (!terminal_contract_.enabled())
      return {};
    for (uint32_t hart = 0; hart < terminal_facts_.size(); ++hart) {
      if (!terminal_facts_[hart] || terminal_fact_cycles_[hart] != cycle)
        return {};
      const auto &facts = *terminal_facts_[hart];
      auto result = terminal_observer_->observe(hart, cycle, facts.wfi, facts.pipeline_empty,
          facts.pending_integer_writeback, facts.wfi_wake_pending, terminal_cache_idle_[hart],
          terminal_adapter_drained_[hart]);
      if (!result)
        return result;
    }
    return {};
  }

  CheckResult complete_terminal_session() {
    if (!terminal_observer_->ready(cycle_) || !signature_store_uid_ || !tohost_store_uid_)
      return {CheckStatus::Incomplete, 0,
              "Rocket WFI terminal, successful REF signature/tohost, or stable quiescence evidence is incomplete"};
    if (!submitted_prefix_ || *submitted_prefix_ < cycle_ ||
        std::any_of(source_prefixes_.begin(), source_prefixes_.end(),
                    [this](uint64_t prefix) { return prefix < cycle_; }) ||
        !staged_events_.empty() || !staged_retirements_.empty())
      return {CheckStatus::Incomplete, 0, "Rocket WFI terminal source prefix has not been fully delivered"};
    const auto progress = session_->poll();
    if (progress.status == CheckStatus::Pending)
      return {CheckStatus::Incomplete, 0, "Rocket WFI terminal reached with pending checker observations"};
    if (!progress)
      return progress;
    const auto final_word_matches = [this](uint64_t address, uint64_t expected) {
      std::array<uint8_t, sizeof(uint64_t)> bytes{};
      if (!memory_.read(address, bytes.data(), bytes.size()))
        return false;
      uint64_t value = 0;
      for (size_t index = 0; index < bytes.size(); ++index)
        value |= static_cast<uint64_t>(bytes[index]) << (index * 8);
      return value == expected;
    };
    if (!final_word_matches(terminal_contract_.signature_address, terminal_contract_.signature_value) ||
        !final_word_matches(terminal_contract_.tohost_address, terminal_contract_.tohost_success_value))
      return {CheckStatus::Violation, 0, "Rocket terminal success words differ from the final checked memory"};
    for (const auto &completion : terminal_observer_->completions(cycle_)) {
      const auto completed = session_->complete_hart(completion);
      if (!completed)
        return completed;
    }
    for (uint32_t hart = 0; hart < decoders_.size(); ++hart) {
      const auto &facts = *terminal_facts_[hart];
      const auto hart_drain = session_->drain_producer({rvwmo::hart_producer_id(hart), cycle_,
          facts.wfi && facts.pipeline_empty && !facts.pending_integer_writeback && !facts.wfi_wake_pending &&
              terminal_adapter_drained_[hart], true});
      if (!hart_drain)
        return hart_drain.status == CheckStatus::Pending
            ? CheckResult{CheckStatus::Incomplete, 0, "Rocket hart producer has undelivered work"}
            : hart_drain;
      const auto fabric_drain = session_->drain_producer({rvwmo::memory_fabric_producer_id(hart), cycle_,
          terminal_cache_idle_[hart] && terminal_adapter_drained_[hart], true});
      if (!fabric_drain)
        return fabric_drain.status == CheckStatus::Pending
            ? CheckResult{CheckStatus::Incomplete, 0, "Rocket cache/fabric producer is not physically drained"}
            : fabric_drain;
    }
    return {};
  }

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
    if (terminal_contract_.enabled()) {
      for (const auto &event : batch.events) {
        if (event.kind != rvwmo::RuntimeEventKind::InstructionRetire)
          continue;
        const auto &retired = std::get<rvwmo::InstructionRetireEvent>(event.payload).instruction;
        if (retired.hart >= decoders_.size())
          return {CheckStatus::Invalid, 0, "Rocket terminal retirement names an unknown hart"};
        const rvwmo::OperationId id{retired.hart, retired.epoch, retired.uid, 0};
        const auto snapshot = staged_retirements_.find(id);
        if (snapshot == staged_retirements_.end())
          return {CheckStatus::Incomplete, 0, "Rocket WFI terminal retirement has no actual snapshot"};
        const auto terminal = terminal_observer_->retire(snapshot->second, event.source.cycle);
        if (!terminal)
          return terminal;
      }
    }
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
