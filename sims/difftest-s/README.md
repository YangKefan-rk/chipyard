# Native Chipyard Difftest-S integration

The four-hart profiles use the official Small BOOM V4 and Big Rocket core
presets, with native observations and a synchronous simulation clock:

| Configuration | Core preset | Harts |
| --- | --- | --- |
| `QuadSmallBoomV4DifftestSConfig` | `boom.v4.common.WithNSmallBooms(4)` | 4 |
| `QuadRocketDifftestSConfig` | `freechips.rocketchip.rocket.WithNBigCores(4)` | 4 |
| `QuadRocketMergeDifftestSConfig` | Big Rocket, parity ECC with two-byte granules | 4 |

The Rocket profile differs from this Chipyard revision's upstream
`QuadRocketConfig`, which uses `WithNHugeCores(4)`. These Difftest-S profiles
export observations and disable tile clock gating; they are integration
configurations rather than unchanged upstream configurations.

## Source dependencies

Initialize Chipyard's dependencies and toolchain using its normal setup
instructions. The BOOM and Rocket submodules point to the user's forks and
retain exact gitlinks; do not replace them with floating upstream branches.
For an existing checkout, synchronize their URLs before updating:

```sh
git submodule sync -- generators/boom generators/rocket-chip
git submodule update --init --recursive generators/boom generators/rocket-chip
```

Difftest-S and its reference Spike are separate checkouts, selected through
explicit build paths. The recorded BOOM four-hart acceptance cohort uses:

| Component | Revision |
| --- | --- |
| BOOM gitlink | `2f0880a9fc6dec3a6a3c587bf81b9dbf2f72f4b1` |
| Rocket gitlink | `9a2845dac099de73e04a12da7ca914f16489ca88` |
| `YangKefan-rk/difftest-s` | `499c89d9504639c95846037fb8cf1976f76c097f` |
| `YangKefan-rk/Spike` | `752638c4034e385fd910a9ea1e0e0d179ffbeea2` |

Select these exact external revisions before building. Chipyard's upstream
`toolchains/riscv-tools/riscv-isa-sim` submodule does not replace the reference
Spike checkout selected below.

## Build a complete session

First build the BOOM pure C API reference library. Use an absolute output path
separate from other CPU profiles; Spike's default CPU is NANHU.

```sh
make -C /absolute/path/to/Spike/difftest CPU=BOOM \
  MULTICORE_DIFF=1 EXTERNAL_MEMORY=1 PURE_API=1 \
  BUILD_DIR=/absolute/path/to/build/boom-ref
```

Then run the wrapper from Chipyard's `sims/verilator`, with the toolchain
activated:

```sh
make -f ../difftest-s/Makefile CONFIG=QuadSmallBoomV4DifftestSConfig \
  DIFFTEST_S_DUT=boom DIFFTEST_S_SESSION=1 \
  DIFFTEST_S_DIR=/absolute/path/to/difftest-s \
  DIFFTEST_S_SPIKE_HOME=/absolute/path/to/Spike \
  DIFFTEST_S_SPIKE_LIBRARY=/absolute/path/to/build/boom-ref/riscv64-spike-ref-api.so
```

For Rocket, build Spike with `CPU=ROCKET_CHIP` in a separate output directory,
then use `CONFIG=QuadRocketDifftestSConfig`, `DIFFTEST_S_DUT=rocket` and that
profile's library. Do not reuse BOOM reference objects for Rocket.

To exercise merged pending stores, use `CONFIG=QuadRocketMergeDifftestSConfig`
with the same Rocket reference profile. The default Rocket configuration uses
one-byte ECC granules and does not merge pending stores. The merge profile
changes only the DCache ECC settings; it retains four Big Rocket cores and the
same observation and terminal contracts. Its compatible source revisions are:

| Component | Merge-support revision |
| --- | --- |
| Rocket gitlink | `fde46422f576bd655c09e1827087fcf2d56388d9` |
| Difftest-S `main` | `60f8f0f06ad88fc959f0c1af2f3e77c265c0d87d` |
| Reference Spike | `752638c4034e385fd910a9ea1e0e0d179ffbeea2` |

Regenerate the RTL and native layout registry when selecting this profile.
The merged-store path preserves every original logical member, including
stores fully overwritten inside the group, and independently audits its
address, data and mask against REF. It checks the ECC preservation bytes
against the GlobalMem prefix without changing their logical writer identity.
One physical drain remains one observed source edge. Logical members use
modeled same-hart order only after all registered producers close a cycle
containing no other memory effect; an ambiguous cycle remains `Incomplete`.
Actual ECC correction drains also require an independent maintenance-byte
audit and currently return `Incomplete`. Selecting parity ECC does not itself
exercise that correction path.

`DIFFTEST_S_SESSION=1` compiles the native consumer, private DUT adapter and
public HostSession together. With the default `DIFFTEST_S_SESSION=0`, the
wrapper builds capture only, which cannot produce a checker Pass. The pure
reference library isolates its C++ symbols from the native HTIF loader; an
ordinary Spike shared library is not a substitute. The wrapper uses BFD by
default (`DIFFTEST_S_LINKER=bfd`) for the verified toolchain.

## Execution and termination

Set `DIFFTEST_S_ELF` to the same ELF supplied to the native harness. The scoped
reference starts at its independently loaded DRAM entry in M-mode. Every GPR
must be initialized before use, except x0 and a0=mhartid from the native ROM.
The reference image comes from ELF PT_LOAD segments; DUT RAM and observed
store data do not initialize it. Boot ROM, bootstrap CSR/MMIO and trap activity
remain outside this checked prefix. Optional `DIFFTEST_S_RAM_BASE` and
`DIFFTEST_S_RAM_SIZE` configure reference RAM (decimal or 0x notation, at most
4 GiB).

BOOM requires these host settings from the selected ELF and its independent
expected success word:

- `DIFFTEST_S_BOOM_TERMINAL_PC`: PC of the canonical FENCE.I (`0x0000100f`).
- `DIFFTEST_S_BOOM_SUCCESS_ADDRESS`: RAM address of the success word.
- `DIFFTEST_S_BOOM_SUCCESS_VALUE`: independently expected success value.

Also pass `+difftest_s_boom_terminal_enable=1` and
`+difftest_s_boom_terminal_pc=<decimal PC>` to the native simulator. The host
PC may use 0x notation, but the RTL plusarg must contain the matching decimal
PC. The policy is unarmed by default. When armed, an actual retirement of the
configured FENCE.I stops further decode/dispatch and flushes the frontend;
cache/coherence clocks continue until the observed sources drain. Marker
retirement, architectural comparison and physical drain are separate checks.
PCs and expected values must be derived for each ELF rather than copied from
another program.

Rocket uses a different terminal contract. Set `DIFFTEST_S_START_AT_ELF=1` and
all five settings together:

- `DIFFTEST_S_TERMINAL_PC`: PC of the canonical WFI (`0x10500073`).
- `DIFFTEST_S_TERMINAL_SIGNATURE_ADDRESS` and
  `DIFFTEST_S_TERMINAL_SIGNATURE_VALUE`: independent success signature.
- `DIFFTEST_S_TERMINAL_TOHOST_ADDRESS` and
  `DIFFTEST_S_TERMINAL_TOHOST_SUCCESS_VALUE`: independent tohost success word.

The two addresses must be nonzero and distinct; both expected values must be
nonzero. Completion additionally requires actual WFI, pipeline, cache,
writeback and wake-state evidence. A park loop, a WFI instruction or a RAM
signature alone does not establish completion.

A successful run must exit zero and emit exactly one `DIFFTEST_S_FINAL` report
with verdict `Pass`, all four harts checked and all required producers drained.
Missing order or terminal evidence yields `Incomplete`; unsupported operations
must not be interpreted as successful validation. The accepted scope is an
initialized scalar-RAM prefix, not full ISA or full RVWMO completeness. Generic
AMO/LRSC, FP/vector, split accesses, MMIO, traps and interrupts are outside this
acceptance scope.

## Observation transport

Generated `difftest-s-layout-*.json` files describe packed native probe fields.
The build validates the complete hart roster before compiling the matching
layout registry. These records are DUT observations, not another public event
protocol. The Small BOOM V4 profile uses ROB32, LDQ8 and STQ8; other core sizes
require matching capacity metadata and new validation.

`native_probe.h` installs the Runtime consumer before the first source edge.
It assembles one complete frame from every hart, validates transport ticks and
source-clock continuity, and forwards each complete edge to the consumer.
Neither callback arrival order nor `transport_tick` assigns memory order. A
reset or epoch change requires a new observation session. The consumer owns
conversion to shared events, independent REF execution, producer closure,
hardware order evidence, completion and the final verdict.

`DIFFTEST_S_NATIVE_TRACE` may name a binary native trace for diagnostics. Native
capture and a matching RAM signature do not establish a checker Pass. The
`DIFFTEST-S Native` diagnostic reports ingress status only. A simulator crash,
timeout or absent final checker report remains a failed run.

The scoped firtool launcher removes only annotations duplicated verbatim in
both CHIRRTL and its sidecar JSON. The wrapper excludes the unrelated Radiance
make fragment from these Rocket/BOOM profiles; ordinary Chipyard builds retain
their default generator fragments.
