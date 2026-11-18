#include "../boom_testchipip_boot_entry.h"
#include <cassert>
#include <memory>

using difftest_s::native::initialize_boom_testchipip_boot_csrs;
using difftest_s::native::validate_boom_testchipip_boot_entry;

void rejects_other_boot_configuration(uint64_t pc, unsigned harts) {
  try {
    validate_boom_testchipip_boot_entry(pc, harts);
    assert(false);
  } catch (const std::invalid_argument &) {
  }
}

int main() {
  constexpr uint64_t dram_entry = 0x80000000;
  rejects_other_boot_configuration(dram_entry + 4, 4);
  rejects_other_boot_configuration(dram_entry, 2);
  validate_boom_testchipip_boot_entry(dram_entry, 4);

  for (unsigned hart = 0; hart < 4; ++hart) {
    DifftestRefOptions options{};
    options.struct_size = sizeof(options);
    options.hart_id = hart;
    // Only exercising the REF's state API: no instruction or data memory.
    options.physical_memory.contains = [](void *, uint64_t, size_t) { return false; };
    options.physical_memory.read = [](void *, uint64_t, size_t, uint8_t *) { return false; };
    options.physical_memory.write = [](void *, uint64_t, size_t, const uint8_t *) { return false; };
    const std::unique_ptr<void, decltype(&difftest_ref_destroy)> ref(
        difftest_ref_create(&options), difftest_ref_destroy);
    assert(ref);
    assert(difftest_ref_hart_id(ref.get()) == hart);
    difftest_ref_profile_id_v1 profile{};
    profile.abi_version = DIFFTEST_REF_PROFILE_ID_ABI_VERSION_1;
    profile.struct_size = sizeof(profile);
    assert(difftest_ref_get_profile_id_v1(ref.get(), &profile) == DIFFTEST_REF_API_OK);
    assert(profile.profile_id == DIFFTEST_REF_PROFILE_ID_BOOM);

    difftest_ref_state_v1 entry{};
    entry.abi_version = DIFFTEST_REF_STATE_ABI_VERSION_1;
    entry.struct_size = sizeof(entry);
    entry.pc = dram_entry;
    entry.privilege = 3;
    entry.gpr[10] = hart;
    initialize_boom_testchipip_boot_csrs(entry);
    assert(difftest_ref_set_state_v1(ref.get(), &entry) == DIFFTEST_REF_API_OK);

    auto actual = entry;
    assert(difftest_ref_get_state_v1(ref.get(), &actual) == DIFFTEST_REF_API_OK);
    assert(actual.pc == dram_entry && actual.privilege == 3 && actual.gpr[10] == hart);
    assert(actual.mtvec == 0x10040 && actual.mepc == dram_entry);
    assert(actual.mstatus == 0xa00000080 && actual.sstatus == 0x200000000);
    assert(actual.mie == 8 && actual.mcause == 0x8000000000000003);
  }
}
