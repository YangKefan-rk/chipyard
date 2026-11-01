#include "boom_native_source.h"
#include "boom_hartbreaker_elf.h"
#include "rvwmo/host_session.h"
#include "rvwmo/dependency.h"
#include "rvwmo/memory_image.h"
#include "rvwmo/producer_ids.h"
#include <verilated.h>
#include <charconv>
#include <array>
#include <algorithm>
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

class BoomSessionSink {
public:
  explicit BoomSessionSink(const char *elf) {
    const auto *layout = find_native_layout(2, 0);
    if (!layout)
      throw std::invalid_argument("BOOM Runtime sink requires matching generated native layouts");
    const uint64_t ram_base = configured_number("DIFFTEST_S_RAM_BASE", UINT64_C(0x80000000));
    const uint64_t ram_size = configured_number("DIFFTEST_S_RAM_SIZE", UINT64_C(0x10000000));
    if (!ram_size || ram_size > UINT64_C(0x100000000) || !memory_.initialize(ram_base, ram_size))
      throw std::invalid_argument("BOOM Runtime sink requires bounded initialized independent RAM");
    uint64_t entry = 0;
    auto loaded = rvwmo::load_reference_elf(memory_, elf, entry);
    if (!loaded)
      throw std::invalid_argument("independent reference ELF: " + loaded.message);

    const char *mode = std::getenv("DIFFTEST_S_BOOM_TERMINAL_MODE");
    if (mode && *mode) {
      const std::string terminal_mode(mode);
      if (terminal_mode != "hartbreaker-tohost-v1" && terminal_mode != "hartbreaker-tohost-wfi-v1")
        throw std::invalid_argument("unknown BOOM terminal mode");
      adapted_wfi_terminal_ = terminal_mode == "hartbreaker-tohost-wfi-v1";
      const char *expected = std::getenv("DIFFTEST_S_BOOM_TOHOST_SUCCESS_VALUE");
      if (!expected || !*expected)
        throw std::invalid_argument("HartBreaker terminal needs an independent expected tohost value");
      tohost_success_value_ = configured_number("DIFFTEST_S_BOOM_TOHOST_SUCCESS_VALUE", 0);
      // HartBreaker at the pinned generator revision defines TERMSIG=1.
      // A caller-selected nonzero value must not turn a failing ELF into Pass.
      if (tohost_success_value_ != 1)
        throw std::invalid_argument("HartBreaker tohost success value must equal the generator's TERMSIG=1");
      if (layout->harts != 4)
        throw std::invalid_argument("HartBreaker terminal requires exactly four native BOOM harts");
      const auto terminal = resolve_hartbreaker_terminal(elf, memory_);
      tohost_address_ = terminal.tohost;
      parked_wfi_pc_ = terminal.parked_wfi;
      if (adapted_wfi_terminal_) {
        if (terminal.success_wfi_pcs.empty())
          throw std::invalid_argument("adapted HartBreaker ELF has no independently verified success-tail WFI");
        success_wfi_pcs_ = std::move(terminal.success_wfi_pcs);
        terminal_wfi_pcs_ = success_wfi_pcs_;
        terminal_wfi_pcs_.push_back(parked_wfi_pc_);
      }
    }
    terminal_pc_ = configured_number("DIFFTEST_S_BOOM_TERMINAL_PC", 0);
    if (tohost_address_ && terminal_pc_)
      throw std::invalid_argument("HartBreaker and fixed-PC BOOM terminal policies are mutually exclusive");
    if (terminal_pc_) {
      success_address_ = configured_number("DIFFTEST_S_BOOM_SUCCESS_ADDRESS", 0);
      const char *success = std::getenv("DIFFTEST_S_BOOM_SUCCESS_VALUE");
      if (!success_address_ || !success || !*success)
        throw std::invalid_argument("BOOM terminal policy requires an independent success word address and value");
      success_value_ = configured_number("DIFFTEST_S_BOOM_SUCCESS_VALUE", 0);
      std::array<uint8_t, 4> marker{};
      if (!memory_.read(terminal_pc_, marker.data(), marker.size()) ||
          marker != std::array<uint8_t, 4>{0x0f, 0x10, 0, 0})
        throw std::invalid_argument("BOOM terminal PC is not an independently initialized canonical FENCE.I");
    }
    BoomNativeSourceConfiguration source_config;
    source_config.terminal_pc = terminal_pc_;
    source_config.terminal_wfi_pcs = terminal_wfi_pcs_;
    source_config.checked_ram_base = ram_base;
    source_config.checked_ram_size = ram_size;
    source_config.skip_device_stores = tohost_address_ != 0;
    source_config.check_csr_post = tohost_address_ != 0 || terminal_pc_ != 0;
    source_config.check_fp_post = tohost_address_ != 0 || terminal_pc_ != 0;
    source_config.entry_pcs.assign(layout->harts, entry);
    // Exact native Small BOOM V4 profile: ROB32/LDQ8/STQ8. Other configurations
    // require their real capacity metadata rather than the adapter maximums.
    source_config.limits.rob_slots = 32;
    source_config.limits.ldq_slots = 8;
    source_config.limits.stq_slots = 8;
    source_ = std::make_unique<BoomNativeSource>(std::move(source_config));

    rvwmo::HostSessionConfiguration config;
    config.platform = "boom";
    config.verification_scope = tohost_address_
        ? "initialized DRAM-entry scalar RAM; own-hart CLINT device stores skipped without value or I/O-order verification; boot ROM uncovered"
        : "initialized DRAM-entry scalar RAM; boot ROM, bootstrap CSR/MMIO/traps are uncovered";
    config.skip_device_stores = tohost_address_ != 0;
    config.reference_profile = DIFFTEST_REF_PROFILE_ID_BOOM;
    config.initial_harts.resize(layout->harts);
    initialized_registers_.resize(layout->harts);
    initialized_fpr_.resize(layout->harts);
    submitted_markers_.resize(layout->harts);
    parked_harts_.resize(layout->harts);
    success_stores_.resize(layout->harts);
    const std::vector<rvwmo::RuntimeEventKind> hart_kinds = {
      rvwmo::RuntimeEventKind::InstructionAllocate, rvwmo::RuntimeEventKind::InstructionRetire,
      rvwmo::RuntimeEventKind::InstructionCancelAttempt, rvwmo::RuntimeEventKind::InstructionSquash,
      rvwmo::RuntimeEventKind::LocalStore, rvwmo::RuntimeEventKind::LoadSample,
      rvwmo::RuntimeEventKind::AtomicLifecycle};
    for (uint32_t hart = 0; hart < layout->harts; ++hart) {
      auto &initial = config.initial_harts[hart];
      initial.pc = entry;
      initial.privilege = 3;
      // This explicit test profile requires an entry program whose only GPR
      // read before initialization is a0, set by the native ROM's mhartid read.
      initial.gpr[10] = hart;
      initialized_registers_[hart][0] = true;
      initialized_registers_[hart][10] = true;
      const auto producer = rvwmo::hart_producer_id(hart);
      config.stream.required_producers.push_back({producer, hart_kinds});
      config.producers.push_back({producer, true, true, true, hart_kinds});
      const auto fabric = rvwmo::memory_fabric_producer_id(hart);
      const std::vector<rvwmo::RuntimeEventKind> fabric_kinds = {
        rvwmo::RuntimeEventKind::StorePublish, rvwmo::RuntimeEventKind::AtomicLifecycle};
      config.stream.required_producers.push_back({fabric, fabric_kinds});
      config.producers.push_back({fabric, true, true, true, fabric_kinds});
    }
    config.reference_store_observer = [this](const rvwmo::ReferenceStoreValue &store) {
      auto result = source_->audit_reference_store(store);
      if (!result || !tohost_address_ || store.address != tohost_address_)
        return result;
      if (store.bytes.size() != 8 || store.byte_mask.size() != 8 ||
          std::any_of(store.byte_mask.begin(), store.byte_mask.end(), [](uint8_t mask) { return mask != 1; }))
        return CheckResult{CheckStatus::Unsupported, 0, "HartBreaker tohost requires an audited full-width store"};
      uint64_t value = 0;
      for (size_t index = 0; index < store.bytes.size(); ++index)
        value |= uint64_t(store.bytes[index]) << (index * 8);
      if (value != tohost_success_value_)
        return CheckResult{CheckStatus::Violation, 0, "HartBreaker audited REF tohost store has an incorrect exit value"};
      // Each hart has a termination loop. Audit every success store; the
      // first observed owner is only a terminal-progress hint, not a unique
      // winner or a substitute for physical drain.
      if (store.id.hart >= success_stores_.size())
        return CheckResult{CheckStatus::Invalid, 0, "HartBreaker tohost store has an unknown hart"};
      success_stores_[store.id.hart] = store.id;
      if (!first_tohost_store_)
        first_tohost_store_ = store.id;
      return result;
    };
    session_ = std::make_unique<rvwmo::HostSession>(memory_, std::move(config));
    if (!session_->configuration())
      throw std::invalid_argument("BOOM Runtime session: " + session_->configuration().message);
  }

  CheckResult capture(const NativeFrame &frame) {
    if (!failure_)
      return failure_;
    auto result = source_->capture(frame);
    if (frame.reset)
      return result;
    cycle_ = frame.source_cycle;
    if (result && tohost_address_)
      result = observe_parked_wfi();
    if (result) {
      rvwmo::boom::CollectedObservations ready;
      result = source_->take_ready(ready);
      if (result)
        result = validate_entry_registers(ready.retirements);
      // A delayed architectural event from before parking may arrive in a
      // later native batch. Reject only effects sourced at or after the WFI
      // stall, not an earlier retirement still awaiting its load evidence.
      if (result && tohost_address_)
        for (const auto &event : ready.batch.events) {
          if (event.kind != rvwmo::RuntimeEventKind::InstructionRetire)
            continue;
          const auto hart = std::get<rvwmo::InstructionRetireEvent>(event.payload).instruction.hart;
          if (hart >= parked_harts_.size()) {
            result = {CheckStatus::Invalid, 0, "HartBreaker retirement names an unknown hart"};
            break;
          }
          if (parked_harts_[hart] && event.source.cycle >= parked_harts_[hart]->source_cycle) {
            result = {CheckStatus::Incomplete, 0, "HartBreaker hart retired at or after its physical WFI stall"};
            break;
          }
        }
      if (result) {
        const auto submitted = session_->submit(ready.batch, ready.retirements);
        result = submitted.check;
        // Acceptance owns these snapshots even when an earlier semantic fact
        // keeps REF pending. Completion below still requires a successful poll.
        if (submitted.accepted && (result || result.status == CheckStatus::Pending))
          for (const auto &event : ready.batch.events) {
            if (event.kind != rvwmo::RuntimeEventKind::InstructionRetire)
              continue;
            const auto &instruction = std::get<rvwmo::InstructionRetireEvent>(event.payload).instruction;
            for (const auto &snapshot : ready.retirements) {
              if (snapshot.id.hart != instruction.hart || snapshot.id.epoch != instruction.epoch ||
                  snapshot.id.uid != instruction.uid)
                continue;
              if (terminal_pc_ && snapshot.pc == terminal_pc_ && snapshot.instruction == 0x0000100f)
                submitted_markers_[instruction.hart] = SubmittedTerminalMarker{snapshot.id, event.source.cycle};
            }
          }
      }
      if (result && terminal_pc_) {
        auto terminal = complete_terminal();
        if (terminal) {
          completed_ = true;
          Verilated::gotFinish(true);
        } else if (terminal.status != CheckStatus::Incomplete && terminal.status != CheckStatus::Pending) {
          result = terminal;
        }
      }
      if (result && adapted_wfi_terminal_) {
        auto terminal = complete_wfi_terminal();
        if (terminal) {
          completed_ = true;
          Verilated::gotFinish(true);
        } else if (terminal.status != CheckStatus::Incomplete && terminal.status != CheckStatus::Pending) {
          result = terminal;
        }
      }
    }
    if (!result && result.status != CheckStatus::Pending) {
      failure_ = result;
      session_->report_source_failure(result);
      std::fprintf(stderr, "DIFFTEST-S BOOM source cycle %llu: status %u rule %u: %s\n",
                   static_cast<unsigned long long>(cycle_), static_cast<unsigned>(result.status),
                   result.rule, result.message.c_str());
      // Return to the native main loop so its SV final processes run and emit a
      // terminal checker report. The runner must reject every non-Pass report.
      Verilated::gotFinish(true);
    }
    return result;
  }

  void transport_failed(const CheckResult &result) {
    failure_ = result;
    session_->report_source_failure(result);
    Verilated::gotFinish(true);
  }

  CheckResult finalize() {
    for (uint32_t hart = 0; hart < source_->unchecked_bootstrap().size(); ++hart) {
      const auto &outside = source_->unchecked_bootstrap()[hart];
      std::fprintf(stderr,
        "DIFFTEST-S uncovered bootstrap hart %u: instructions=%llu loads=%llu stores=%llu traps=%llu mmio_requests=%llu mmio_acks=%llu\n",
        hart, static_cast<unsigned long long>(outside.instructions), static_cast<unsigned long long>(outside.loads),
        static_cast<unsigned long long>(outside.stores), static_cast<unsigned long long>(outside.traps),
        static_cast<unsigned long long>(outside.mmio_store_requests),
        static_cast<unsigned long long>(outside.mmio_store_acks));
    }
    // Native HTIF may call $finish on the first tohost publication. Only the
    // explicitly adapted profile may complete after four current physical WFI
    // stalls, all REF steps and all native store/cache effects have drained.
    if (tohost_address_ && failure_ && !completed_) {
      const auto parked = std::count_if(parked_harts_.begin(), parked_harts_.end(),
          [](const auto &marker) { return marker.has_value(); });
      session_->report_source_failure({CheckStatus::Incomplete, 0,
          !first_tohost_store_ ? "HartBreaker has no audited retiring tohost success store" :
          adapted_wfi_terminal_ ? "adapted HartBreaker four-hart WFI stop or physical drain is unproven" :
          parked != parked_harts_.size() - 1 || parked_harts_[first_tohost_store_->hart]
            ? "HartBreaker first success and other WFI parking boundaries are unproven"
            : "HartBreaker first success stop and four-hart physical drain are unproven"});
    }
    auto result = session_->finalize(cycle_);
    std::fprintf(stderr, "DIFFTEST_S_FINAL %s\n", session_->final_report().c_str());
    return failure_ ? result : failure_;
  }

private:
  GlobalMem memory_;
  std::unique_ptr<BoomNativeSource> source_;
  std::unique_ptr<rvwmo::HostSession> session_;
  uint64_t cycle_ = 0;
  CheckResult failure_;
  uint64_t terminal_pc_ = 0;
  uint64_t success_address_ = 0, success_value_ = 0;
  uint64_t tohost_address_ = 0, tohost_success_value_ = 0, parked_wfi_pc_ = 0;
  bool adapted_wfi_terminal_ = false;
  std::vector<uint64_t> terminal_wfi_pcs_;
  std::vector<uint64_t> success_wfi_pcs_;
  std::vector<std::optional<rvwmo::OperationId>> success_stores_;
  std::optional<rvwmo::OperationId> first_tohost_store_;
  bool completed_ = false;
  struct SubmittedTerminalMarker { rvwmo::OperationId id; uint64_t source_cycle; };
  std::vector<std::optional<SubmittedTerminalMarker>> submitted_markers_;
  struct ParkedWfiObservation {
    rvwmo::OperationId id;
    uint64_t rob_generation = 0;
    uint64_t source_cycle = 0;
    uint64_t pc = 0;
  };
  std::vector<std::optional<ParkedWfiObservation>> parked_harts_;

  CheckResult observe_parked_wfi() {
    const auto &facts = source_->wfi_park_facts();
    if (facts.size() != parked_harts_.size())
      return {CheckStatus::Invalid, 0, "HartBreaker WFI roster differs from four-hart REF"};
    for (uint32_t hart = 0; hart < facts.size(); ++hart) {
      const auto &park = facts[hart];
      const uint64_t entry_uid = source_->entry_uid(hart);
      // BootROM contains its own WFI but is explicitly outside the checked
      // ELF-entry scope. A later delivery is classified by stable UID.
      if (!entry_uid)
        continue;
      if (park.valid && park.marker.uid < entry_uid) {
        if (parked_harts_[hart])
          return {CheckStatus::Incomplete, 0, "HartBreaker parked WFI switched back to a BootROM owner"};
        continue;
      }
      if (!park.available) {
        if (parked_harts_[hart])
          return {CheckStatus::Incomplete, 0, "HartBreaker WFI hardware witness disappeared"};
        continue; // Old RTL cannot prove parking; finalization remains Incomplete.
      }
      if (parked_harts_[hart] && (!park.valid || park.marker.uid != parked_harts_[hart]->id.uid ||
                                  park.marker.epoch != parked_harts_[hart]->id.epoch ||
                                  park.marker.rob_generation != parked_harts_[hart]->rob_generation ||
                                  park.marker_pc != parked_harts_[hart]->pc))
        return {CheckStatus::Incomplete, 0, "HartBreaker parked WFI woke or changed its ROB identity"};
      if (!park.valid)
        continue;
      const bool expected_pc = adapted_wfi_terminal_
          ? std::find(terminal_wfi_pcs_.begin(), terminal_wfi_pcs_.end(), park.marker_pc) != terminal_wfi_pcs_.end()
          : park.marker_pc == parked_wfi_pc_;
      if (!expected_pc || park.marker_instruction != 0x10500073)
        return {CheckStatus::Invalid, 0, "BOOM physical WFI is not an independently verified ELF terminal"};
      if (!parked_harts_[hart])
        parked_harts_[hart] = ParkedWfiObservation{{hart, park.marker.epoch, park.marker.uid, 0},
                                                    park.marker.rob_generation, park.source_cycle, park.marker_pc};
    }
    return {};
  }
  std::vector<std::array<bool, 32>> initialized_registers_;
  std::vector<std::array<bool, 32>> initialized_fpr_;

  CheckResult complete_wfi_terminal() {
    if (completed_)
      return {};
    if (!first_tohost_store_)
      return {CheckStatus::Incomplete, 0, "adapted HartBreaker lacks an audited tohost success store"};
    if (std::any_of(parked_harts_.begin(), parked_harts_.end(),
                    [](const auto &park) { return !park.has_value(); }))
      return {CheckStatus::Incomplete, 0, "adapted HartBreaker has not physically parked all four harts"};
    bool success_tail_parked = false;
    for (uint32_t hart = 0; hart < parked_harts_.size(); ++hart) {
      const auto &park = *parked_harts_[hart];
      const auto &store = success_stores_[hart];
      if (store && park.id.epoch == store->epoch && park.id.uid > store->uid &&
          std::find(success_wfi_pcs_.begin(), success_wfi_pcs_.end(), park.pc) != success_wfi_pcs_.end()) {
        success_tail_parked = true;
        break;
      }
    }
    if (!success_tail_parked)
      return {CheckStatus::Incomplete, 0, "adapted HartBreaker has no success-store hart physically parked at its success-tail WFI"};
    auto result = source_->certify_wfi_terminals();
    if (!result)
      return result;
    result = source_->completion();
    if (!result)
      return result;
    result = session_->poll();
    if (!result)
      return result;
    uint64_t tohost_word = 0;
    if (!memory_.read(tohost_address_, &tohost_word, sizeof(tohost_word)))
      return {CheckStatus::Invalid, 0, "adapted HartBreaker tohost is outside initialized RAM"};
    if (tohost_word != tohost_success_value_)
      return {CheckStatus::Violation, 0, "adapted HartBreaker physical tohost value differs from expected success"};
    for (uint32_t hart = 0; hart < parked_harts_.size(); ++hart) {
      result = session_->complete_hart({hart, parked_harts_[hart]->source_cycle, 0});
      if (!result)
        return result;
    }
    for (uint32_t hart = 0; hart < parked_harts_.size(); ++hart) {
      result = session_->drain_producer({rvwmo::hart_producer_id(hart), cycle_, true, true});
      if (!result)
        return result;
      result = session_->drain_producer({rvwmo::memory_fabric_producer_id(hart), cycle_, true, true});
      if (!result)
        return result;
    }
    return {};
  }

  CheckResult complete_terminal() {
    if (completed_)
      return {};
    for (const auto &marker : submitted_markers_)
      if (!marker)
        return {CheckStatus::Incomplete, 0, "BOOM terminal marker has not been accepted for every hart REF comparison"};
    auto result = source_->completion();
    if (!result)
      return result;
    result = session_->poll();
    if (!result)
      return result;
    std::array<uint8_t, 8> success_bytes{};
    if (!memory_.read(success_address_, success_bytes.data(), success_bytes.size()))
      return {CheckStatus::Invalid, 0, "BOOM success word is outside independently initialized RAM"};
    uint64_t success_word = 0;
    for (size_t byte = 0; byte < success_bytes.size(); ++byte)
      success_word |= static_cast<uint64_t>(success_bytes[byte]) << (byte * 8);
    if (success_word != success_value_)
      return {CheckStatus::Violation, 0, "BOOM workload success word differs from its independent expected value"};
    const auto &facts = source_->terminal_facts();
    for (uint32_t hart = 0; hart < facts.size(); ++hart) {
      result = session_->complete_hart({hart, submitted_markers_[hart]->source_cycle, 0});
      if (!result)
        return result;
    }
    for (uint32_t hart = 0; hart < facts.size(); ++hart) {
      result = session_->drain_producer({rvwmo::hart_producer_id(hart), cycle_, true, true});
      if (!result)
        return result;
      result = session_->drain_producer({rvwmo::memory_fabric_producer_id(hart), cycle_, true, true});
      if (!result)
        return result;
    }
    return {};
  }

  CheckResult validate_entry_registers(const std::vector<rvwmo::ArchitecturalRetirement> &retirements) {
    for (const auto &retirement : retirements) {
      if (retirement.id.hart >= initialized_registers_.size())
        return {CheckStatus::Invalid, 0, "entry-state validation names an unknown hart"};
      rvwmo::DependencyMetadata dependencies;
      auto result = rvwmo::decode_dependencies(retirement.instruction, dependencies);
      if (!result)
        return result;
      auto &initialized = initialized_registers_[retirement.id.hart];
      auto &initialized_fp = initialized_fpr_[retirement.id.hart];
      const bool certified_csr = (retirement.instruction & 0x7f) == 0x73 &&
          retirement.csr_post && retirement.csr_post->first == (retirement.instruction >> 20);
      for (const auto reg : dependencies.sources) {
        if (reg < initialized.size() && initialized[reg])
          continue;
        // A certified CSR retirement compares its physical post-state with REF;
        // only GPRs need an independent entry value before that comparison.
        if (reg >= 32 && reg < 64 && retirement.fp_post && initialized_fp[reg - 32])
          continue;
        if (reg >= 64 && certified_csr)
          continue;
        return {CheckStatus::Unsupported, 0, "entry program reads an uninitialized GPR, FPR or uncertified CSR"};
      }
      for (const auto reg : dependencies.destinations) {
        if (reg < initialized.size())
          initialized[reg] = true;
        else if (reg < 64 && retirement.fp_post)
          initialized_fp[reg - 32] = true;
      }
    }
    return {};
  }
};

struct InstallBoomSession {
  InstallBoomSession() {
    const char *elf = std::getenv("DIFFTEST_S_ELF");
    if (!elf || !*elf)
      return;
    try {
      auto sink = std::make_shared<BoomSessionSink>(elf);
      if (!register_native_consumer([sink](const NativeFrame &frame) { return sink->capture(frame); },
                                    [sink] { return sink->finalize(); },
                                    [sink](const CheckResult &failure) { sink->transport_failed(failure); }))
        throw std::runtime_error("native Runtime sink was registered more than once or after sampling started");
    } catch (const std::exception &error) {
      std::fprintf(stderr, "DIFFTEST-S BOOM Runtime initialization failed: %s\n", error.what());
      // There is no configured checker session to finalize here. Reject this
      // run as an invalid setup without a native process SIGABRT/core dump.
      std::exit(EXIT_FAILURE);
    }
  }
};
InstallBoomSession install;
} // namespace
} // namespace difftest_s::native
