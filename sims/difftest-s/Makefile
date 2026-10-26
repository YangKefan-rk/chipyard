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
EXTRA_SIM_LDFLAGS += -pthread

# Chisel 6 may emit the same annotation in both CHIRRTL and the sidecar JSON.
# The scoped launcher preserves all annotations while removing exact duplicates.
DIFFTEST_S_FIRTOOL_BIN ?= firtool
export DIFFTEST_S_FIRTOOL_BIN
FIRTOOL_BIN ?= $(difftest_s_bridge_dir)/run_firtool.py

# These profiles instantiate Rocket/BOOM only. Radiance's fragment otherwise
# injects unrelated Vortex source files and cache-control defines into every sim.
GENERATOR_MAKE_FRAGMENTS = $(filter-out $(base_dir)/generators/radiance/chipyard.mk,$(wildcard $(base_dir)/generators/*/chipyard.mk))

include $(difftest_s_bridge_dir)/../verilator/Makefile

DIFFTEST_S_LAYOUTS = $(wildcard $(build_dir)/$(long_name).difftest-s-layout-*.json)
$(difftest_s_registry): $(FIRRTL_FILE) $(difftest_s_bridge_dir)/generate_layout_registry.py
	python3 $(difftest_s_bridge_dir)/generate_layout_registry.py --output $@ $(DIFFTEST_S_LAYOUTS)

$(SFC_MFC_TARGETS): $(difftest_s_bridge_dir)/run_firtool.py
$(model_mk) $(model_mk_debug): $(difftest_s_bridge_dir)/Makefile
$(sim) $(sim_debug): $(EXTRA_SIM_SOURCES)
