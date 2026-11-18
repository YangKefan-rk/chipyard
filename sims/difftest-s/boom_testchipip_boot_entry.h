#ifndef DIFFTEST_S_BOOM_TESTCHIPIP_BOOT_ENTRY_H
#define DIFFTEST_S_BOOM_TESTCHIPIP_BOOT_ENTRY_H

#include "difftest/ref_api.h"
#include <stdexcept>

namespace difftest_s::native {

inline void validate_boom_testchipip_boot_entry(uint64_t entry, unsigned harts) {
  if (entry != UINT64_C(0x80000000) || harts != 4)
    throw std::invalid_argument("BOOM TestChipIP boot state requires the quad-hart fixed DRAM entry");
}

inline void initialize_boom_testchipip_boot_csrs(difftest_ref_state_v1 &state) {
  // The pinned RV64 TestChipIP ROM sets mtvec and MSIE before the software
  // interrupt, writes the fixed BootAddrReg value to mepc, clears MPIE and
  // executes mret. Pending interrupt inputs are not part of this contract.
  constexpr uint64_t mpie = UINT64_C(1) << 7;
  constexpr uint64_t uxl64 = UINT64_C(2) << 32;
  constexpr uint64_t sxl64 = UINT64_C(2) << 34;
  state.mtvec = 0x10040;
  state.mie = UINT64_C(1) << 3;
  state.mcause = (UINT64_C(1) << 63) | 3;
  state.mepc = state.pc;
  state.mstatus = sxl64 | uxl64 | mpie;
  state.sstatus = uxl64;
}

} // namespace difftest_s::native

#endif
