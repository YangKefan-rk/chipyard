# Embed native probe capture in the existing Chipyard Verilator simulator.
# Run from sims/verilator: make -f ../difftest-s/Makefile CONFIG=... DIFFTEST_S_DIR=...
difftest_s_bridge_dir := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

ifeq ($(strip $(DIFFTEST_S_DIR)),)
$(error DIFFTEST_S_DIR must name the pinned Difftest-S checkout)
endif
DIFFTEST_S_DUT ?= boom
ifeq ($(filter $(DIFFTEST_S_DUT),rocket boom),)
$(error This native capture build supports DIFFTEST_S_DUT=rocket|boom)
endif

# Register before including the native rules: make expands their prerequisites
# while parsing those rules, before the generated layout registry exists.
difftest_s_registry = $(build_dir)/difftest-s-native-layouts.cpp
EXTRA_SIM_REQS += $(difftest_s_registry)
EXTRA_SIM_SOURCES += $(difftest_s_bridge_dir)/native_probe.cpp $(difftest_s_registry)
EXTRA_SIM_CXXFLAGS += -I$(difftest_s_bridge_dir) -I$(abspath $(DIFFTEST_S_DIR))/src/test/csrc/difftest
# BFD preserves weak standard-library COMDAT data when the isolated reference
# library and native HTIF are linked together in the supported toolchain.
DIFFTEST_S_LINKER ?= bfd
override EXTRA_SIM_LDFLAGS += -pthread -fuse-ld=$(DIFFTEST_S_LINKER)

# Native capture can run alone for diagnostics. The session build adds the
# matched pure Spike C API library and compiles one Difftest-S C++ cohort.
DIFFTEST_S_SESSION ?= 0
ifeq ($(DIFFTEST_S_SESSION),1)
ifeq ($(strip $(DIFFTEST_S_SPIKE_HOME)),)
$(error DIFFTEST_S_SPIKE_HOME must name the matching Spike API checkout)
endif
ifeq ($(strip $(DIFFTEST_S_SPIKE_LIBRARY)),)
$(error DIFFTEST_S_SPIKE_LIBRARY must name the built matching pure Spike API library)
endif
difftest_s_host_dir := $(abspath $(DIFFTEST_S_DIR))/src/test/csrc/difftest
# The native simulator's old -lriscv must not satisfy versioned reference API
# symbols before the selected profile library. Native HTIF still uses libfesvr.
override LRISCV :=
ifeq ($(DIFFTEST_S_DUT),boom)
difftest_s_native_sources := boom_native_decoder boom_native_source boom_session_sink
difftest_s_adapter_sources := native_collector retirement_snapshot word_bytes boom_event_adapter
else
difftest_s_native_sources := rocket_native_decoder rocket_session_sink
difftest_s_adapter_sources := native_adapter cache_tracker event_emitter retirement_capture
endif
EXTRA_SIM_SOURCES += $(addprefix $(difftest_s_bridge_dir)/,$(addsuffix .cpp,$(difftest_s_native_sources)))
EXTRA_SIM_SOURCES += $(difftest_s_host_dir)/globalmem_storage.cpp
EXTRA_SIM_SOURCES += $(addprefix $(difftest_s_host_dir)/rvwmo/,$(addsuffix .cpp,host_session memory_image spike_reference_pool spike_reference_stepper session stream checker_event_consumer checker memory ppo dependency lrsc))
EXTRA_SIM_SOURCES += $(addprefix $(difftest_s_host_dir)/adapters/$(DIFFTEST_S_DUT)/,$(addsuffix .cpp,$(difftest_s_adapter_sources)))
EXTRA_SIM_CXXFLAGS += -I$(abspath $(DIFFTEST_S_SPIKE_HOME))
override EXTRA_SIM_LDFLAGS += $(abspath $(DIFFTEST_S_SPIKE_LIBRARY)) -Wl,-rpath,$(abspath $(dir $(DIFFTEST_S_SPIKE_LIBRARY)))
endif

# Chisel 6 may emit the same annotation in both CHIRRTL and the sidecar JSON.
# The scoped launcher preserves all annotations while removing exact duplicates.
DIFFTEST_S_FIRTOOL_BIN ?= firtool
export DIFFTEST_S_FIRTOOL_BIN
FIRTOOL_BIN ?= $(difftest_s_bridge_dir)/run_firtool.py

# These profiles instantiate Rocket/BOOM only. Radiance's fragment otherwise
# injects unrelated Vortex source files and cache-control defines into every sim.
GENERATOR_MAKE_FRAGMENTS = $(filter-out $(base_dir)/generators/radiance/chipyard.mk,$(wildcard $(base_dir)/generators/*/chipyard.mk))

include $(difftest_s_bridge_dir)/../verilator/Makefile

# The native default main always returns zero. Keep its time loop but use the
# ingress/Runtime terminal status for the process exit code.
override VERILATOR := verilator --timing --cc --exe
EXTRA_SIM_SOURCES += $(difftest_s_bridge_dir)/verilator_main.cpp

DIFFTEST_S_LAYOUTS = $(wildcard $(build_dir)/$(long_name).difftest-s-layout-*.json)
$(difftest_s_registry): $(FIRRTL_FILE) $(difftest_s_bridge_dir)/generate_layout_registry.py
	python3 $(difftest_s_bridge_dir)/generate_layout_registry.py --output $@ $(DIFFTEST_S_LAYOUTS)

$(SFC_MFC_TARGETS): $(difftest_s_bridge_dir)/run_firtool.py
$(model_mk) $(model_mk_debug): $(difftest_s_bridge_dir)/Makefile
$(sim) $(sim_debug): $(EXTRA_SIM_SOURCES)
