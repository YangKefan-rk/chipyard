#ifndef CHIPYARD_DIFFTEST_S_ROCKET_TERMINAL_H
#define CHIPYARD_DIFFTEST_S_ROCKET_TERMINAL_H

#include "rvwmo/host_session.h"
#include <optional>
#include <vector>

namespace difftest_s::native {

// Test-specific WFI identity is configured by the runner. The RTL facts only
// prove that this retired instruction has reached a stable, drained stop.
class RocketTerminalObserver {
public:
  RocketTerminalObserver(size_t hart_count, uint64_t terminal_pc)
      : states_(hart_count), terminal_pc_(terminal_pc) {}

  rvwmo::CheckResult retire(const rvwmo::ArchitecturalRetirement &instruction, uint64_t cycle) {
    if (!terminal_pc_)
      return {};
    if (instruction.id.hart >= states_.size())
      return invalid("Rocket terminal retirement names an unknown hart");
    auto &state = states_[instruction.id.hart];
    if (state.retirement_cycle)
      return invalid("Rocket retired another instruction after its configured terminal WFI");
    if (instruction.pc != terminal_pc_)
      return {};
    if (instruction.instruction != canonical_wfi)
      return invalid("configured Rocket terminal PC did not retire canonical WFI");
    state.retirement_cycle = cycle;
    return {};
  }

  rvwmo::CheckResult observe(uint32_t hart, uint64_t cycle, bool wfi, bool pipeline_empty,
                             bool pending_integer_writeback, bool wfi_wake_pending,
                             bool cache_idle, bool adapter_drained) {
    if (!terminal_pc_)
      return {};
    if (hart >= states_.size())
      return invalid("Rocket terminal sample names an unknown hart");
    auto &state = states_[hart];
    state.observed_cycle = cycle;
    state.ready = false;
    if (!state.retirement_cycle || cycle <= *state.retirement_cycle)
      return {};
    if (wfi_wake_pending)
      return invalid("Rocket terminal WFI has a pending wake condition");
    if (state.observed_wfi && !wfi)
      return invalid("Rocket left WFI after its configured terminal retirement");
    state.observed_wfi = state.observed_wfi || wfi;
    state.ready = state.observed_wfi && wfi && pipeline_empty && !pending_integer_writeback &&
                  cache_idle && adapter_drained;
    return {};
  }

  bool ready(uint64_t cycle) const {
    if (!terminal_pc_ || states_.empty())
      return false;
    for (const auto &state : states_)
      if (!state.retirement_cycle || !state.ready || state.observed_cycle != cycle ||
          *state.retirement_cycle >= cycle)
        return false;
    return true;
  }

  std::vector<rvwmo::HartCompletion> completions(uint64_t stable_cycle) const {
    std::vector<rvwmo::HartCompletion> result;
    if (!ready(stable_cycle))
      return result;
    for (uint32_t hart = 0; hart < states_.size(); ++hart)
      result.push_back({hart, stable_cycle, 0});
    return result;
  }

private:
  struct State {
    std::optional<uint64_t> retirement_cycle;
    uint64_t observed_cycle = 0;
    bool observed_wfi = false;
    bool ready = false;
  };

  static constexpr uint32_t canonical_wfi = 0x10500073;
  std::vector<State> states_;
  uint64_t terminal_pc_ = 0;

  static rvwmo::CheckResult invalid(const char *message) {
    return {rvwmo::CheckStatus::Invalid, 0, message};
  }
};

struct RocketTerminalProgramContract {
  uint64_t park_pc = 0;
  uint64_t signature_address = 0;
  uint64_t signature_value = 0;
  uint64_t tohost_address = 0;
  uint64_t tohost_success_value = 0;

  bool enabled() const { return park_pc != 0; }
};

} // namespace difftest_s::native
#endif
