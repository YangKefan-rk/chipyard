# Native Chipyard observations

Run the wrapper from `sims/verilator`, with an exact Difftest-S checkout:

```sh
make -f ../difftest-s/Makefile CONFIG=QuadSmallBoomV4DifftestSConfig \
  DIFFTEST_S_DUT=boom DIFFTEST_S_DIR=/path/to/pinned/difftest-s
```

Rocket uses `QuadRocketDifftestSConfig` and `DIFFTEST_S_DUT=rocket`. The profiles
retain their native cores and use one synchronous source clock without tile
clock gating. Generated `difftest-s-layout-*.json` files describe packed native
probe fields; the build validates the complete hart roster before compiling
its matching layout registry. These records are DUT observations, not another
public event protocol.

`native_probe.h` accepts a Runtime consumer before the first source edge. It
assembles one complete frame from every hart, validates transport ticks and
source-clock continuity, and forwards each complete edge to that consumer.
Neither callback arrival order nor `transport_tick` assigns memory order. A
runtime reset or epoch change requires a new observation session. The installed
consumer owns conversion to the shared events, independent REF execution,
producer closure, hardware order evidence, completion and the final verdict.

Without a consumer, capture remains Incomplete. `DIFFTEST_S_NATIVE_TRACE` may
name a binary native trace for diagnostics; native capture and a matching RAM
signature do not establish a checker Pass. The `DIFFTEST-S Native` diagnostic
reports ingress status only. A simulator crash, timeout or absent final checker
report remains a failed run even when part of its trace can be inspected.

The scoped firtool launcher removes only annotations duplicated verbatim in
both CHIRRTL and its sidecar JSON. The wrapper excludes the unrelated Radiance
make fragment from these Rocket/BOOM profiles; the ordinary Chipyard build
keeps its default generator fragments.

## BOOM session embedding

The first online scope starts from an independently initialized DRAM entry. Its
entry program must initialize each GPR before reading it, except a0=mhartid from
the native ROM. The reference image is loaded from the ELF's PT_LOAD segments;
DUT RAM and observed store data never initialize the reference. Boot CSR, MMIO
and trap activity is retained as explicitly uncovered bootstrap statistics.
The native Small BOOM V4 profile uses ROB32, LDQ8 and STQ8.

Build the complete session with `DIFFTEST_S_SESSION=1`, a Difftest-S revision
containing the BOOM native collector and HostSession, `DIFFTEST_S_SPIKE_HOME`,
and `DIFFTEST_S_SPIKE_LIBRARY` pointing to the matching BOOM pure C API library.
Build that library with Spike's `PURE_API=1 MULTICORE_DIFF=1 EXTERNAL_MEMORY=1`
target. An ordinary Spike library exports HTIF internals that can intercept the
native simulator's loader; its C++ symbols must remain isolated. The native
wrapper uses BFD by default (`DIFFTEST_S_LINKER=bfd`) for the verified toolchain.
A local mold build dropped weak standard-library lookup data in the combined
executable; it is not a validated linker for this profile.

Set `DIFFTEST_S_ELF` to the same ELF supplied to the native harness. Optional
`DIFFTEST_S_RAM_BASE` and `DIFFTEST_S_RAM_SIZE` configure independent reference
RAM (decimal or 0x notation, at most 4 GiB). The sink installs before source
sampling, drives the public HostSession and forwards actual independent REF
store audits to the private adapter's bookkeeping. Native main returns nonzero
unless both ingress and its installed final checker complete successfully.

Current online execution verifies scalar prefixes but still returns Incomplete
for missing hardware order or terminal drain evidence. No hart completion is
inferred from a park loop, WFI, elapsed cycles or a correct memory signature.
