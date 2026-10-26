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
