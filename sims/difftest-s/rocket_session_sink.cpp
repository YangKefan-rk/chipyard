#include "rocket_native_decoder.h"
#include "native_fields.h"
#include "rocket_hartbreaker_bootstrap.h"
#include "rocket_hartbreaker_terminal.h"
#include "rocket_mie_contract.h"
#include "rvwmo/dependency.h"
#include "rvwmo/host_session.h"
#include "rvwmo/memory_image.h"
#include "rvwmo/producer_ids.h"
#include "rocket_terminal.h"
#include <verilated.h>
#include <algorithm>
#include <array>
#include <map>
#include <set>
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
    const auto hartbreaker_bootstrap = configured_number("DIFFTEST_S_HARTBREAKER_BOOTSTRAP", 0);
    terminal_contract_ = terminal_program_contract();
    const auto hartbreaker_terminal = configured_number("DIFFTEST_S_HARTBREAKER_TERMINAL", 0);
    if (hartbreaker_terminal > 1 || (hartbreaker_terminal &&
        (!hartbreaker_bootstrap || !scoped || layout->harts != 4 || terminal_contract_.enabled())))
      throw std::invalid_argument("HartBreaker terminal requires its exact quad-hart bootstrap and no custom WFI contract");
    hartbreaker_terminal_enabled_ = hartbreaker_terminal == 1;
    hartbreaker_drain_cycles_ = configured_number("DIFFTEST_S_HARTBREAKER_DRAIN_CYCLES", 8192);
    if (hartbreaker_terminal_enabled_ && (!hartbreaker_drain_cycles_ || hartbreaker_drain_cycles_ > UINT32_MAX))
      throw std::invalid_argument("HartBreaker terminal drain deadline must be 1..2^32-1 source cycles");
    if (scoped > 1 || !ram_size || ram_size > UINT64_C(0x100000000) || !memory_.initialize(ram_base, ram_size))
      throw std::invalid_argument("Rocket Runtime sink requires bounded initialized RAM and an explicit scope");
    if (terminal_contract_.enabled() && !scoped)
      throw std::invalid_argument("Rocket WFI terminal checking requires an explicit ELF-entry scope");
    if (hartbreaker_bootstrap > 1 || (hartbreaker_bootstrap && (!scoped || layout->harts != 4)))
      throw std::invalid_argument("Rocket HartBreaker bootstrap requires four harts and explicit ELF-entry scope");
    hartbreaker_bootstrap_enabled_ = hartbreaker_bootstrap == 1;
    uint64_t entry = 0;
    auto loaded = rvwmo::load_reference_elf(memory_, elf, entry);
    if (!loaded)
      throw std::invalid_argument("independent reference ELF: " + loaded.message);
    if (hartbreaker_terminal_enabled_)
      hartbreaker_terminal_contract_ = hartbreaker_terminal_contract(memory_, elf);
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
    if (hartbreaker_bootstrap)
      config.verification_scope =
          "HartBreaker ELF after ELF-byte-verified timer-disable prefix; execution of prefix CLINT MMIO and ROM "
          "is uncovered; M-mode scalar cached RAM only";
    else if (scoped)
      config.verification_scope =
          "ELF-entry M-mode scalar cached RAM; ROM/bootstrap CSR/MMIO/interrupts are uncovered";
    config.initial_harts.resize(layout->harts);
    initialized_dependencies_.resize(layout->harts);
    publication_sequences_.resize(layout->harts);
    source_prefixes_.resize(layout->harts * 2);
    terminal_facts_.resize(layout->harts);
    terminal_fact_cycles_.resize(layout->harts);
    terminal_cache_idle_.resize(layout->harts, false);
    terminal_adapter_drained_.resize(layout->harts, false);
    fetch_stop_facts_.resize(layout->harts);
    stop_request_cycles_.resize(layout->harts);
    stop_accepted_cycles_.resize(layout->harts);
    terminal_retired_.resize(layout->harts, false);
    terminal_counter_retired_.resize(layout->harts, false);
    terminal_counter_audited_.resize(layout->harts, false);
    terminal_observer_ = std::make_unique<RocketTerminalObserver>(layout->harts, terminal_contract_.park_pc);
    const std::vector<rvwmo::RuntimeEventKind> hart_kinds = {
      rvwmo::RuntimeEventKind::InstructionAllocate, rvwmo::RuntimeEventKind::InstructionRetire,
      rvwmo::RuntimeEventKind::InstructionCancelAttempt, rvwmo::RuntimeEventKind::InstructionSquash,
      rvwmo::RuntimeEventKind::LocalStore, rvwmo::RuntimeEventKind::LoadSample};
    for (uint32_t hart = 0; hart < layout->harts; ++hart) {
      const auto *hart_layout = find_native_layout(1, hart);
      if (!hart_layout || hart_layout->harts != layout->harts)
        throw std::invalid_argument("Rocket Runtime sink has an incomplete generated hart roster");
      if (hartbreaker_terminal_enabled_) {
        const auto fields = NativeFields(*hart_layout);
        terminal_retire_fields_.push_back({fields.field("core.retire.valid"),
            fields.field("core.retire.bits.instruction.pc"),
            fields.field("core.retire.bits.instruction.instruction")});
      }
      if (hartbreaker_bootstrap) {
        const auto mie = NativeFields(*hart_layout).field("core.mieValue");
        if (mie.width != 64)
          throw std::invalid_argument("Rocket reset-MIE witness requires RV64 native observation");
        const auto fields = NativeFields(*hart_layout);
        mie_fields_.push_back({mie, fields.field("core.retire.valid"),
            fields.field("core.retire.bits.instruction.pc")});
      }
      auto &initial = config.initial_harts[hart];
      initial.pc = entry;
      initial.privilege = 3;
      // The optional HartBreaker profile excludes exactly its proven CLINT
      // timer-disable prefix; no DUT registers or data seed this checkpoint.
      if (hartbreaker_bootstrap) {
        const auto checkpoint = hartbreaker_bootstrap_checkpoint(memory_, entry, hart);
        initial.pc = checkpoint.pc;
        std::copy(checkpoint.gpr.begin(), checkpoint.gpr.end(), initial.gpr);
        initial.mie = RocketMieContract::entry_value;
        for (unsigned unit = 0; unit < checkpoint.initialized.size(); ++unit)
          if (checkpoint.initialized[unit])
            initialized_dependencies_[hart].insert(unit);
      } else {
        // Except x0, GPRs must be established by independent REF execution.
        initialized_dependencies_[hart].insert(0);
      }
      const auto producer = rvwmo::hart_producer_id(hart);
      const auto fabric = rvwmo::memory_fabric_producer_id(hart);
      config.stream.required_producers.push_back({producer, hart_kinds});
      config.producers.push_back({producer, true, true, true, hart_kinds});
      const std::vector<rvwmo::RuntimeEventKind> fabric_kinds = {
        rvwmo::RuntimeEventKind::StorePublish, rvwmo::RuntimeEventKind::AtomicCommitCandidate};
      config.stream.required_producers.push_back({fabric, fabric_kinds});
      config.producers.push_back({fabric, true, true, true, fabric_kinds});
      RocketDecoderConfiguration source_config;
      source_config.publication_producer = fabric;
      source_config.entry_pc = scoped ? initial.pc : 0;
      source_config.require_terminal_observations = terminal_contract_.enabled() || hartbreaker_terminal_enabled_;
      source_config.require_fetch_stop = hartbreaker_terminal_enabled_;
      decoders_.push_back(std::make_unique<RocketNativeDecoder>(*hart_layout, source_config,
          [this, hart](uint64_t cycle) {
            auto &sequence = publication_sequences_[hart];
            if (sequence == UINT64_MAX)
              throw std::invalid_argument("Rocket native publication source sequence exhausted");
            return rvwmo::SourcePosition{cycle, 0, ++sequence, false};
          }));
    }
    if (terminal_contract_.enabled() || hartbreaker_terminal_enabled_) {
      config.reference_store_observer = [this](const rvwmo::ReferenceStoreValue &store) {
        return observe_reference_store(store);
      };
    }
    if (hartbreaker_bootstrap)
      mie_contract_ = std::make_unique<RocketMieContract>(layout->harts);
    session_ = std::make_unique<rvwmo::HostSession>(memory_, std::move(config));
    if (!session_->configuration())
      throw std::invalid_argument("Rocket Runtime session: " + session_->configuration().message);
    if (hartbreaker_terminal_enabled_ && !enable_native_terminal_drain(layout->harts))
      throw std::invalid_argument("HartBreaker terminal control could not register its fixed hart roster");
  }

  CheckResult capture(const NativeFrame &frame) {
    if (!failure_)
      return failure_;
    cycle_ = frame.source_cycle;
    if (!frame.reset && !elf_scope_)
      return fail({CheckStatus::Unsupported, 0,
          "Rocket whole-boot checking requires independent ROM state and the shared trap/interrupt protocol"});
    const auto mie_check = validate_mie_state(frame);
    if (!mie_check)
      return fail(mie_check);
    if (hartbreaker_terminal_enabled_ && !frame.reset) {
      const auto stop = request_terminal_stops(frame);
      if (!stop)
        return fail(stop);
    }
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
      if (hartbreaker_terminal_enabled_) {
        if (!ready.fetch_stop)
          return fail({CheckStatus::Unsupported, 0, "HartBreaker termination lacks actual fetch-stop observations"});
        fetch_stop_facts_[hart] = ready.fetch_stop;
        if (ready.fetch_stop->accepted && !stop_accepted_cycles_[hart])
          stop_accepted_cycles_[hart] = frame.source_cycle;
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
    if (hartbreaker_terminal_enabled_) {
      result = complete_hartbreaker_terminal();
      if (!result && result.status != CheckStatus::Pending)
        return fail(result);
    }
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
    if (hartbreaker_terminal_enabled_ && !hartbreaker_terminal_complete_)
      session_->report_source_failure({CheckStatus::Incomplete, 0,
          "HartBreaker ended without verified terminal retirement, stop acknowledgement and drained sources"});
    // Without the configured and observed terminal contract, finalization stays Incomplete.
    final_result_ = session_->finalize(cycle_);
    std::fprintf(stderr, "DIFFTEST_S_FINAL %s\n", session_->final_report().c_str());
    return failure_ ? final_result_ : failure_;
  }

private:
  GlobalMem memory_;
  std::unique_ptr<rvwmo::HostSession> session_;
  std::vector<std::unique_ptr<RocketNativeDecoder>> decoders_;
  std::vector<std::set<unsigned>> initialized_dependencies_;
  struct MieFields {
    NativeField value;
    NativeField retirement_valid;
    NativeField retirement_pc;
  };
  std::vector<MieFields> mie_fields_;
  std::unique_ptr<RocketMieContract> mie_contract_;
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
  bool hartbreaker_bootstrap_enabled_ = false;
  bool hartbreaker_terminal_enabled_ = false;
  bool hartbreaker_terminal_complete_ = false;
  uint64_t hartbreaker_drain_cycles_ = 8192;
  HartbreakerTerminalContract hartbreaker_terminal_contract_;
  struct TerminalRetireFields { NativeField valid, pc, instruction; };
  std::vector<TerminalRetireFields> terminal_retire_fields_;
  std::vector<std::optional<RocketFetchStopFacts>> fetch_stop_facts_;
  std::vector<std::optional<uint64_t>> stop_request_cycles_, stop_accepted_cycles_;
  std::vector<bool> terminal_retired_, terminal_counter_retired_, terminal_counter_audited_;
  std::optional<rvwmo::OperationId> hartbreaker_tohost_store_;
  std::map<rvwmo::OperationId, uint64_t> hartbreaker_retirement_pcs_;

  CheckResult request_terminal_stops(const NativeFrame &frame) {
    if (frame.samples.size() != terminal_retire_fields_.size())
      return {CheckStatus::Invalid, 0, "HartBreaker terminal sample lacks the complete hart roster"};
    for (uint32_t hart = 0; hart < terminal_retire_fields_.size(); ++hart) {
      const auto &sample = frame.samples[hart];
      const auto &fields = terminal_retire_fields_[hart];
      if (!fields.valid.read(sample))
        continue;
      const uint64_t pc = fields.pc.read(sample);
      const uint32_t raw = fields.instruction.read(sample);
      const bool parked = pc == hartbreaker_terminal_contract_.park_pc;
      const bool tohost = pc == hartbreaker_terminal_contract_.tohost_store_pc;
      if (!parked && !tohost)
        continue;
      if ((parked && raw != 0x10500073) || (tohost && raw != 0x01ae3023))
        return {CheckStatus::Invalid, 0, "HartBreaker terminal retirement differs from the independent ELF bytes"};
      if (!stop_request_cycles_[hart]) {
        stop_request_cycles_[hart] = frame.source_cycle;
        request_native_fetch_stop(hart);
      }
    }
    return {};
  }

  CheckResult observe_hartbreaker_reference_store(const rvwmo::ReferenceStoreValue &store) {
    const auto retired = hartbreaker_retirement_pcs_.find(store.id);
    if (retired == hartbreaker_retirement_pcs_.end())
      return {};
    if (store.id.hart >= terminal_counter_audited_.size() || store.bytes.size() != 8 || store.byte_mask.size() != 8)
      return {CheckStatus::Invalid, 0, "HartBreaker terminal REF store has an invalid hart or byte extent"};
    const bool counter = retired->second == hartbreaker_terminal_contract_.counter_amo_pc;
    for (size_t byte = 0; byte < 8; ++byte) {
      const uint8_t expected = counter ? byte < 4 : 1;
      if (store.byte_mask[byte] != expected)
        return {CheckStatus::Violation, 0, "HartBreaker terminal REF store mask differs from its ELF instruction"};
    }
    uint64_t value = 0;
    for (size_t byte = 0; byte < (counter ? 4 : 8); ++byte)
      value |= uint64_t{store.bytes[byte]} << (8 * byte);
    if (counter) {
      if (store.address != hartbreaker_terminal_contract_.counter_address || value < 1 || value > 4 ||
          terminal_counter_audited_[store.id.hart])
        return {CheckStatus::Violation, 0, "HartBreaker completion counter REF audit is invalid or duplicated"};
      terminal_counter_audited_[store.id.hart] = true;
    } else {
      if (store.address != hartbreaker_terminal_contract_.tohost_address || value != 1)
        return {CheckStatus::Violation, 0, "HartBreaker terminal REF store did not write success to its ELF tohost"};
      hartbreaker_tohost_store_ = store.id;
    }
    hartbreaker_retirement_pcs_.erase(retired);
    return {};
  }

  CheckResult complete_hartbreaker_terminal() {
    if (hartbreaker_terminal_complete_)
      return {};
    const auto waiting = [this](const char *reason) {
      std::optional<uint64_t> all_requested_cycle;
      for (const auto &requested : stop_request_cycles_) {
        if (!requested)
          return CheckResult{CheckStatus::Pending, 0, reason};
        all_requested_cycle = std::max(all_requested_cycle.value_or(0), *requested);
      }
      if (all_requested_cycle && cycle_ >= *all_requested_cycle &&
          cycle_ - *all_requested_cycle > hartbreaker_drain_cycles_)
        return CheckResult{CheckStatus::Incomplete, 0,
            std::string("HartBreaker stopped-hart drain deadline expired: ") + reason};
      return CheckResult{CheckStatus::Pending, 0, reason};
    };
    for (uint32_t hart = 0; hart < decoders_.size(); ++hart) {
      if (!stop_request_cycles_[hart] || !stop_accepted_cycles_[hart] || !fetch_stop_facts_[hart] ||
          !fetch_stop_facts_[hart]->requested || !fetch_stop_facts_[hart]->accepted ||
          !terminal_retired_[hart] || !terminal_counter_retired_[hart] || !terminal_counter_audited_[hart])
        return waiting("HartBreaker awaits every hart's checked terminal retirement and fetch-stop acknowledgement");
      if (!terminal_facts_[hart] || terminal_fact_cycles_[hart] != cycle_ ||
          !terminal_facts_[hart]->pipeline_empty || terminal_facts_[hart]->pending_integer_writeback ||
          terminal_facts_[hart]->pending_floating_writeback || terminal_facts_[hart]->wfi_wake_pending ||
          !terminal_cache_idle_[hart] || !terminal_adapter_drained_[hart])
        return waiting("HartBreaker stopped instruction streams still have pipeline, cache or adapter work");
    }
    if (!hartbreaker_tohost_store_ || !submitted_prefix_ || *submitted_prefix_ < cycle_ ||
        std::any_of(source_prefixes_.begin(), source_prefixes_.end(),
                    [this](uint64_t closed) { return closed < cycle_; }) ||
        !staged_events_.empty() || !staged_retirements_.empty() || !hartbreaker_retirement_pcs_.empty())
      return waiting("HartBreaker terminal source prefix and independent REF stores have not fully arrived");
    const auto progress = session_->poll();
    if (progress.status == CheckStatus::Pending)
      return waiting("HartBreaker checker still awaits a memory/source/REF obligation");
    if (!progress)
      return progress;
    const auto read_word = [this](uint64_t address, uint64_t &value) {
      std::array<uint8_t, 8> bytes{};
      if (!memory_.read(address, bytes.data(), bytes.size()))
        return false;
      value = 0;
      for (size_t index = 0; index < bytes.size(); ++index)
        value |= uint64_t{bytes[index]} << (8 * index);
      return true;
    };
    uint64_t counter = 0, tohost = 0;
    if (!read_word(hartbreaker_terminal_contract_.counter_address, counter) || counter != 4 ||
        !read_word(hartbreaker_terminal_contract_.tohost_address, tohost) || tohost != 1)
      return {CheckStatus::Violation, 0, "HartBreaker final independently checked counter/tohost values are not 4/1"};
    if (mie_contract_) {
      const auto mie = mie_contract_->complete();
      if (!mie)
        return mie;
    }
    for (uint32_t hart = 0; hart < decoders_.size(); ++hart) {
      const auto completed = session_->complete_hart({hart, cycle_, 0});
      if (!completed)
        return completed;
      const auto hart_drain = session_->drain_producer({rvwmo::hart_producer_id(hart), cycle_, true, true});
      if (!hart_drain)
        return hart_drain;
      const auto fabric_drain = session_->drain_producer({rvwmo::memory_fabric_producer_id(hart), cycle_, true, true});
      if (!fabric_drain)
        return fabric_drain;
    }
    hartbreaker_terminal_complete_ = true;
    complete_native_terminal_drain();
    std::fprintf(stderr, "DIFFTEST_S_ROCKET_TERMINAL {\"mode\":\"hartbreaker-fetch-stop\","
        "\"drained_cycle\":%llu,\"source_closed_through\":%llu,\"counter_value\":%llu,"
        "\"tohost_value\":%llu,\"tohost_hart\":%u,\"tohost_uid\":%llu,\"hart_count\":%zu,"
        "\"pending\":0,\"harts\":[", static_cast<unsigned long long>(cycle_),
        static_cast<unsigned long long>(*submitted_prefix_), static_cast<unsigned long long>(counter),
        static_cast<unsigned long long>(tohost), hartbreaker_tohost_store_->hart,
        static_cast<unsigned long long>(hartbreaker_tohost_store_->uid), decoders_.size());
    for (uint32_t hart = 0; hart < decoders_.size(); ++hart)
      std::fprintf(stderr, "%s{\"hart\":%u,\"stop_request_cycle\":%llu,\"stop_accepted_cycle\":%llu,"
          "\"last_allocation_token\":%llu}", hart ? "," : "", hart,
          static_cast<unsigned long long>(*stop_request_cycles_[hart]),
          static_cast<unsigned long long>(*stop_accepted_cycles_[hart]),
          static_cast<unsigned long long>(fetch_stop_facts_[hart]->last_allocation_token));
    std::fprintf(stderr, "]}\n");
    return {};
  }

  rvwmo::CheckResult observe_reference_store(const rvwmo::ReferenceStoreValue &store) {
    if (hartbreaker_terminal_enabled_)
      return observe_hartbreaker_reference_store(store);
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
          facts.pending_integer_writeback || facts.pending_floating_writeback,
          facts.wfi_wake_pending, terminal_cache_idle_[hart],
          terminal_adapter_drained_[hart]);
      if (!result)
        return result;
    }
    return {};
  }

  CheckResult complete_terminal_session() {
    if (mie_contract_) {
      const auto mie = mie_contract_->complete();
      if (!mie)
        return mie;
    }
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
          return {CheckStatus::Incomplete, 0,
              "Rocket closed retirement awaits independent actual writeback: hart=" +
              std::to_string(retired.hart) + " uid=" + std::to_string(retired.uid) +
              " pc=" + std::to_string(retired.pc) + " source=" +
              std::to_string(event.source.cycle) + " closed=" + std::to_string(through)};
        retirements.push_back(snapshot->second);
        if (hartbreaker_terminal_enabled_ &&
            (snapshot->second.pc == hartbreaker_terminal_contract_.counter_amo_pc ||
             snapshot->second.pc == hartbreaker_terminal_contract_.tohost_store_pc))
          hartbreaker_retirement_pcs_[id] = snapshot->second.pc;
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
    if (hartbreaker_terminal_enabled_) {
      for (const auto &snapshot : retirements) {
        if (snapshot.pc == hartbreaker_terminal_contract_.counter_amo_pc)
          terminal_counter_retired_[snapshot.id.hart] = true;
        if (snapshot.pc == hartbreaker_terminal_contract_.park_pc ||
            snapshot.pc == hartbreaker_terminal_contract_.tohost_store_pc)
          terminal_retired_[snapshot.id.hart] = true;
      }
    }
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

  CheckResult validate_mie_state(const NativeFrame &frame) {
    if (!mie_contract_)
      return {};
    if (frame.samples.size() != mie_fields_.size())
      return {CheckStatus::Invalid, 0, "Rocket MIE evidence has an incomplete observed hart roster"};
    std::vector<RocketMieObservation> observations;
    observations.reserve(mie_fields_.size());
    for (uint32_t hart = 0; hart < mie_fields_.size(); ++hart) {
      const auto &sample = frame.samples[hart];
      if (sample.layout != find_native_layout(1, hart))
        return {CheckStatus::Invalid, 0, "Rocket MIE observation names a mismatched hart layout"};
      const auto &fields = mie_fields_[hart];
      observations.push_back({fields.value.read(sample), fields.retirement_valid.read(sample) &&
          fields.retirement_pc.read(sample) == UINT64_C(0x80000040)});
    }
    const auto result = mie_contract_->observe(frame.reset, observations);
    if (!result)
      return result;
    for (uint32_t hart = 0; hart < mie_fields_.size(); ++hart)
      if (mie_contract_->entered(hart))
        initialized_dependencies_[hart].insert(64 + 0x304);
    return {};
  }

  CheckResult validate_entry_registers(const std::vector<rvwmo::ArchitecturalRetirement> &retirements) {
    for (const auto &retirement : retirements) {
      if (retirement.id.hart >= initialized_dependencies_.size())
        return {CheckStatus::Invalid, 0, "Rocket entry-state validation names an unknown hart"};
      rvwmo::DependencyMetadata dependencies;
      auto result = rvwmo::decode_dependencies(retirement.instruction, dependencies);
      if (!result)
        return result;
      auto &initialized = initialized_dependencies_[retirement.id.hart];
      for (const auto unit : dependencies.sources)
        if (unit && !initialized.count(unit))
          return {CheckStatus::Unsupported, 0, "Rocket entry PC " + std::to_string(retirement.pc) +
                  " reads uninitialized dependency unit " + std::to_string(unit)};
      for (const auto unit : dependencies.destinations)
        if (unit)
          initialized.insert(unit);
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
