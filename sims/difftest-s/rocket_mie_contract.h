#ifndef CHIPYARD_DIFFTEST_S_ROCKET_MIE_CONTRACT_H
#define CHIPYARD_DIFFTEST_S_ROCKET_MIE_CONTRACT_H

#include "rvwmo/event.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace difftest_s::native {

struct RocketMieObservation {
  uint64_t mie = 0;
  bool enters_scope = false;
};

// The selected Rocket resets mie to zero; the selected Chipyard boot ROM then
// writes MSIE=1 before entering the ELF. These are distinct state contracts.
// Neither reference value is derived from a DUT load or a sampled CSR value.
class RocketMieContract {
public:
  static constexpr uint64_t entry_value = 8;
  explicit RocketMieContract(size_t harts) : entered_(harts, false) {}

  rvwmo::CheckResult observe(bool reset, const std::vector<RocketMieObservation> &harts) {
    using rvwmo::CheckStatus;
    if (harts.size() != entered_.size() || harts.empty())
      return {CheckStatus::Invalid, 0, "Rocket MIE evidence has an incomplete hart roster"};
    if (reset) {
      if (checked_reset_)
        return {CheckStatus::Unsupported, 0, "Rocket MIE session cannot span another reset"};
      observed_reset_ = true;
      return {};
    }
    if (!checked_reset_) {
      if (!observed_reset_)
        return {CheckStatus::Incomplete, 0, "Rocket MIE evidence has no observed reset"};
      for (const auto &hart : harts)
        if (hart.mie != 0)
          return {CheckStatus::Violation, 0, "Rocket MIE reset differs from independently fixed zero"};
      checked_reset_ = true;
    }
    for (size_t hart = 0; hart < harts.size(); ++hart) {
      if (!harts[hart].enters_scope || entered_[hart])
        continue;
      if (harts[hart].mie != entry_value)
        return {CheckStatus::Violation, 0, "Rocket MIE entry differs from independent boot-ROM MSIE setup"};
      entered_[hart] = true;
    }
    return {};
  }

  bool entered(size_t hart) const { return hart < entered_.size() && entered_[hart]; }

  rvwmo::CheckResult complete() const {
    if (!checked_reset_ || !std::all_of(entered_.begin(), entered_.end(), [](bool value) { return value; }))
      return {rvwmo::CheckStatus::Incomplete, 0, "Rocket MIE evidence has not covered every hart entry"};
    return {};
  }

private:
  std::vector<bool> entered_;
  bool observed_reset_ = false;
  bool checked_reset_ = false;
};

} // namespace difftest_s::native
#endif
