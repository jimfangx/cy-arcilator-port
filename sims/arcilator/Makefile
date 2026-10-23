# Arcilator executes the generated HW with native event scheduling and bindings
# to the original C/C++ peripheral drivers. The hybrid runtime is a reference.
base_dir := $(abspath ../..)
sim_dir := $(abspath .)
include $(base_dir)/variables.mk
sim_name := arcilator
sim_prefix := simulator
ARC_RUNTIME ?= $(if $(filter hybrid,$(MAKECMDGOALS)),hybrid,native)
ifeq ($(filter $(ARC_RUNTIME),native hybrid),)
$(error ARC_RUNTIME must be native or hybrid)
endif
sim_hybrid := $(sim_dir)/$(sim_prefix)-$(MODEL_PACKAGE)-$(CONFIG)
sim_hybrid_debug := $(sim_hybrid)-debug
sim_native := $(sim_dir)/simulator-native-$(MODEL_PACKAGE)-$(CONFIG)
sim_native_debug := $(sim_native)-debug
sim := $(if $(filter native,$(ARC_RUNTIME)),$(sim_native),$(sim_hybrid))
sim_debug := $(sim)-debug
include $(base_dir)/sims/common-sim-flags.mk
.DEFAULT_GOAL := default
.DELETE_ON_ERROR:
.PHONY: default debug
default: $(sim)
debug: $(sim_debug)
.PHONY: native native-debug hybrid
native: $(sim_native)
native-debug: $(sim_native_debug)
hybrid: $(sim_hybrid)
SIM_FILE_REQS += $(ROCKETCHIP_RSRCS_DIR)/vsrc/TestDriver.v
$(sim_files): $(SIM_FILE_REQS) $(ALL_MODS_FILELIST) | $(GEN_COLLATERAL_DIR)
	cp -f $(SIM_FILE_REQS) $(GEN_COLLATERAL_DIR)
	printf '%s\n' $(addprefix $(GEN_COLLATERAL_DIR)/,$(notdir $(SIM_FILE_REQS))) > $@
include $(base_dir)/common.mk

# Discover the CIRCT suite on PATH after sourcing env.sh. Keep the frontend,
# optimizer, simulator and LLVM tools in that installation so their ABIs agree.
# CIRCT_BIN remains an optional override for an alternate SDK.
ifdef ARCILATOR
CIRCT_BIN ?= $(patsubst %/,%,$(dir $(shell command -v $(ARCILATOR))))
else
CIRCT_BIN ?= $(patsubst %/,%,$(dir $(shell command -v arcilator)))
ARCILATOR = $(CIRCT_BIN)/arcilator
endif
ARC_FIRTOOL ?= $(CIRCT_BIN)/firtool
CIRCT_OPT ?= $(CIRCT_BIN)/circt-opt
ifeq ($(origin CIRCT_VERILOG),undefined)
ARC_FRONTEND_PATCHES ?= auto
else
ARC_FRONTEND_PATCHES ?= off
endif
CIRCT_VERILOG ?= $(CIRCT_BIN)/circt-verilog
LLVM_OPT ?= $(CIRCT_BIN)/opt
LLC ?= $(CIRCT_BIN)/llc
LLVM_CONFIG ?= $(CIRCT_BIN)/llvm-config
MLIR_TRANSLATE ?= $(CIRCT_BIN)/mlir-translate
CIRCT_ROOT ?= $(abspath $(CIRCT_BIN)/..)
PYTHON ?= python3
VERILATOR ?= verilator
ARCILATOR_FLAGS ?= --split-funcs-threshold=8192
ARC_FIRTOOL_FLAGS ?=
ARC_RTL_FLAGS ?=
ARC_PARTITION_OPS ?= 4096
ARC_PARTITION_PORTS ?= 16384
ARC_MODEL_OPS ?= 16384
ARC_SHARED_PROCEDURES ?= 1
ARC_ABI_BUFFERS ?= 1
ARC_EARLY_INLINE ?= 1
ARC_LATE_WIDE_ARITHMETIC ?= 1
ARC_CODEGEN_PIPELINE ?= bounded
ARC_DISTRIBUTE_WIDE_STORES ?= 0
ARC_VECTOR_PACKING ?= 0
ifneq ($(ARC_VECTOR_PACKING),0)
ifneq ($(ARC_VECTOR_PACKING),1)
$(error ARC_VECTOR_PACKING must be 0 or 1)
endif
endif
ifneq ($(ARC_DISTRIBUTE_WIDE_STORES),0)
ifneq ($(ARC_DISTRIBUTE_WIDE_STORES),1)
$(error ARC_DISTRIBUTE_WIDE_STORES must be 0 or 1)
endif
endif
ifneq ($(ARC_ABI_BUFFERS),0)
ifneq ($(ARC_ABI_BUFFERS),1)
$(error ARC_ABI_BUFFERS must be 0 or 1)
endif
endif
ifneq ($(ARC_CODEGEN_PIPELINE),bounded)
ifneq ($(ARC_CODEGEN_PIPELINE),stock)
$(error ARC_CODEGEN_PIPELINE must be bounded or stock)
endif
endif
ifneq ($(ARC_EARLY_INLINE),0)
ifneq ($(ARC_EARLY_INLINE),1)
$(error ARC_EARLY_INLINE must be 0 or 1)
endif
endif
ifneq ($(ARC_LATE_WIDE_ARITHMETIC),0)
ifneq ($(ARC_LATE_WIDE_ARITHMETIC),1)
$(error ARC_LATE_WIDE_ARITHMETIC must be 0 or 1)
endif
endif
ARC_OUTPUT_CONVERGENCE ?= 1
ARC_IDLE_CLOCKS ?= 1
ARC_STABLE_EVALUATOR ?= 1
ifneq ($(ARC_STABLE_EVALUATOR),0)
ifneq ($(ARC_STABLE_EVALUATOR),1)
$(error ARC_STABLE_EVALUATOR must be 0 or 1)
endif
endif
ifneq ($(ARC_IDLE_CLOCKS),0)
ifneq ($(ARC_IDLE_CLOCKS),1)
$(error ARC_IDLE_CLOCKS must be 0 or 1)
endif
endif
ifneq ($(ARC_OUTPUT_CONVERGENCE),0)
ifneq ($(ARC_OUTPUT_CONVERGENCE),1)
$(error ARC_OUTPUT_CONVERGENCE must be 0 or 1)
endif
endif
ifneq ($(ARC_SHARED_PROCEDURES),0)
ifneq ($(ARC_SHARED_PROCEDURES),1)
$(error ARC_SHARED_PROCEDURES must be 0 or 1)
endif
endif
ARC_OPT_LEVEL ?= 3
ARC_OPT_FLAGS ?=
ARC_LLC_OPT_LEVEL ?= $(ARC_OPT_LEVEL)
ARC_BUILD_JOBS ?= $(shell nproc)
ARC_VERILATOR_FLAGS ?=
ARC_CFLAGS ?=
ARC_RUNTIME_LIBS ?= -L$(CIRCT_ROOT)/lib -lCIRCTArcRuntime $(shell $(LLVM_CONFIG) --libs support 2>/dev/null) -lz -lpthread -ldl
get_waveform_flag = +vcdfile=$(1).vcd
ifneq ($(RANDOM_SEED),)
SEED_FLAG := $(if $(filter native,$(ARC_RUNTIME)),+seed=$(RANDOM_SEED),+verilator+seed+$(RANDOM_SEED))
endif
HELP_COMPILATION_VARIABLES += \
"   CIRCT_BIN              = optional CIRCT tool directory override; defaults to arcilator on PATH from env.sh" \
"   ARCILATOR_FLAGS        = extra Arcilator compiler options" \
"   ARC_FIRTOOL_FLAGS      = extra native HW frontend options" \
"   ARC_RUNTIME            = native (default) or hybrid reference runtime" \
"   ARC_RTL_FLAGS          = extra CIRCT Verilog frontend options" \
"   ARC_PARTITION_OPS      = operation budget per compiler module [$(ARC_PARTITION_OPS)]" \
"   ARC_PARTITION_PORTS    = compiler module port budget [$(ARC_PARTITION_PORTS)]" \
"   ARC_MODEL_OPS          = operation budget per model procedure; 0 disables outlining [$(ARC_MODEL_OPS)]" \
"   ARC_SHARED_PROCEDURES  = share eligible private procedure argument/result storage, 0 or 1 [$(ARC_SHARED_PROCEDURES)]" \
"   ARC_ABI_BUFFERS        = buffer large private LLVM arguments/results, 0 or 1 [$(ARC_ABI_BUFFERS)]" \
"   ARC_EARLY_INLINE       = inline forwarding expressions before arithmetic ABI lowering, 0 or 1 [$(ARC_EARLY_INLINE)]" \
"   ARC_LATE_WIDE_ARITHMETIC = retain pure wide shifts through Arc optimization, 0 or 1 [$(ARC_LATE_WIDE_ARITHMETIC)]" \
"   ARC_CODEGEN_PIPELINE   = bounded native passes or stock LLVM codegen [$(ARC_CODEGEN_PIPELINE)]" \
"   ARC_DISTRIBUTE_WIDE_STORES = distribute closed wide stores over CFG edges, 0 or 1 [$(ARC_DISTRIBUTE_WIDE_STORES)]" \
"   ARC_VECTOR_PACKING     = use target-legal vectors for pure integer packing DAGs, 0 or 1 [$(ARC_VECTOR_PACKING)]" \
"   ARC_OUTPUT_CONVERGENCE = use compiler-certified write-only outputs in native settling, 0 or 1 [$(ARC_OUTPUT_CONVERGENCE)]" \
"   ARC_STABLE_EVALUATOR   = use a smaller native evaluator for proven stable states, 0 or 1 [$(ARC_STABLE_EVALUATOR)]" \
"   ARC_OPT_LEVEL          = LLVM IR optimization level [$(ARC_OPT_LEVEL)]" \
"   ARC_OPT_FLAGS          = extra LLVM opt host target/CPU/vectorization options" \
"   ARC_LLC_OPT_LEVEL      = machine-code optimization level [$(ARC_LLC_OPT_LEVEL)]" \
"   ARC_VERILATOR_FLAGS    = extra options for the foreign-model runtime compiler" \
"   ARC_BUILD_JOBS         = parallel LLVM/host/frontend compilation jobs; defaults to available CPUs [$(ARC_BUILD_JOBS)]"

arc_dir := $(build_dir)/arcilator$(if $(filter native,$(ARC_RUNTIME)),-native)
arc_debug_dir := $(arc_dir)-debug

$(arc_dir) $(arc_debug_dir):
	mkdir -p $@

# FORCE checks settings on each requested build; unchanged contents preserve the
# dependency timestamp, so an identical invocation does not rebuild the model.
# Quote each complete NAME=VALUE argument, including embedded shell metacharacters.
arc_shell_quote = '$(subst ','"'"',$(1))'
define arc_settings
$(1): $(sim_dir)/settings.py FORCE | $(3)
	$$(PYTHON) $$(sim_dir)/settings.py --output $$@ $$(foreach name,$(2),$$(call arc_shell_quote,$$(name)=$$($$(name))))
endef
$(eval $(call arc_settings,$(arc_dir)/frontend-settings.json,PYTHON ARC_FIRTOOL ARC_FIRTOOL_FLAGS VLOG_MODEL,$(arc_dir)))
ifeq ($(ARC_RUNTIME),native)
$(eval $(call arc_settings,$(arc_dir)/collateral-settings.json,TOP MODEL,$(arc_dir)))
$(MFC_EXTRA_ANNO_FILE) $(FINAL_ANNO_FILE): $(arc_dir)/collateral-settings.json
endif
$(eval $(call arc_settings,$(arc_dir)/filelist-settings.json,BB_MODS_FILELIST EXT_FILELISTS EXTRA_SIM_SOURCES GEN_COLLATERAL_DIR TOP_SMEMS_FILE MODEL_SMEMS_FILE ARC_RUNTIME,$(arc_dir)))
$(eval $(call arc_settings,$(arc_dir)/partition-settings.json,PYTHON CIRCT_OPT CXX VLOG_MODEL ARC_RUNTIME ARC_PARTITION_OPS ARC_PARTITION_PORTS,$(arc_dir)))

$(arc_dir)/design.mlir: $(FIRRTL_FILE) $(sim_dir)/frontend.py $(arc_dir)/frontend-settings.json | $(arc_dir) check-arcilator-tools
	$(call require_cmd,$(ARC_FIRTOOL))
	$(PYTHON) $(sim_dir)/frontend.py --input $< --output $@.tmp --top $(VLOG_MODEL) \
		--firtool $(ARC_FIRTOOL) -- $(ARC_FIRTOOL_FLAGS)
	mv $@.tmp $@

arc_input := $(arc_dir)/design.mlir
ifeq ($(ARC_RUNTIME),native)
arc_input := $(arc_dir)/native-design.mlir
ARC_SOURCE_METADATA ?= 0
$(eval $(call arc_settings,$(arc_dir)/rtl-settings.json,PYTHON CIRCT_VERILOG ARC_FRONTEND_PATCHES CIRCT_OPT CXX SIM_PREPROC_DEFINES EXT_INCDIRS ARC_RTL_FLAGS ARC_SOURCE_METADATA,$(arc_dir)))
arc_frontend_sources := $(wildcard $(sim_dir)/compiler/verilog/*.py $(sim_dir)/compiler/verilog/*.patch $(sim_dir)/compiler/verilog/*.json $(sim_dir)/compiler/verilog/CMakeLists.txt)
$(arc_dir)/hdl-inputs.json $(arc_dir)/driver-inputs.json &: $(arc_dir)/blackboxes.f $(sim_dir)/filelists.py FORCE
	$(PYTHON) $(sim_dir)/filelists.py --filelist=$(arc_dir)/blackboxes.f \
		--hdl-output=$(arc_dir)/hdl-inputs.json --driver-output=$(arc_dir)/driver-inputs.json
$(arc_dir)/rtl-libraries.json: $(arc_dir)/blackboxes.f $(sim_dir)/rtl_libraries.py $(sim_dir)/filelists.py FORCE
	$(PYTHON) $(sim_dir)/rtl_libraries.py --filelist $(arc_dir)/blackboxes.f --root $(base_dir) --output $@
$(arc_input) $(arc_dir)/rtl.json: $(arc_dir)/rtl-libraries.json $(sim_dir)/rtl_libraries.py $(sim_dir)/filelists.py $(arc_dir)/hdl-inputs.json
$(arc_input) $(arc_dir)/rtl.json &: $(arc_dir)/design.mlir $(arc_dir)/blackboxes.f $(sim_dir)/native_hw.py $(sim_dir)/native_exports.py $(sim_dir)/native/models.def $(sim_dir)/compiler/RTL.cpp $(sim_dir)/compiler/SignalWrites.cpp $(sim_dir)/compiler/SignalState.cpp $(sim_dir)/compiler/SignalMemory.cpp $(sim_dir)/compiler/NativeHandles.cpp $(sim_dir)/compiler/NativeContexts.cpp $(sim_dir)/compiler/HierarchyFormats.cpp $(sim_dir)/compiler/ScopeFrames.h $(sim_dir)/compiler/NativeStrings.cpp $(sim_dir)/compiler/FormatStrings.cpp $(sim_dir)/compiler/UnionBits.cpp $(sim_dir)/compiler/NativeMem2Reg.cpp $(sim_dir)/compiler/LocalMemory.cpp $(sim_dir)/compiler/partition.py $(arc_frontend_sources) $(arc_dir)/rtl-settings.json
	$(PYTHON) $(sim_dir)/native_hw.py --input $< --output $(arc_input) \
		--filelist $(arc_dir)/blackboxes.f --work $(arc_dir)/rtl \
		--library-manifest $(arc_dir)/rtl-libraries.json \
		--circt-verilog $(CIRCT_VERILOG) --frontend-patches=$(ARC_FRONTEND_PATCHES) --circt-opt $(CIRCT_OPT) --cxx="$(CXX)" --jobs=$(ARC_BUILD_JOBS) $(if $(filter 1,$(ARC_SOURCE_METADATA)),--source-metadata) \
		--flags="$(SIM_PREPROC_DEFINES) $(addprefix -I,$(EXT_INCDIRS)) $(ARC_RTL_FLAGS)"
endif

$(arc_dir)/model.mlir $(arc_dir)/ArcHarness.sv $(arc_dir)/partition.json &: $(arc_input) $(sim_dir)/partition.py $(sim_dir)/compiler/native_combin.py $(sim_dir)/compiler/memories.py $(sim_dir)/compiler/partition.py $(sim_dir)/compiler/Partition.cpp $(sim_dir)/compiler/Properties.cpp $(sim_dir)/compiler/HierarchyFormats.cpp $(sim_dir)/compiler/ScopeFrames.h $(sim_dir)/compiler/TemporalProperties.cpp $(sim_dir)/compiler/ProceduralFeedback.cpp $(arc_dir)/partition-settings.json
	$(PYTHON) $(sim_dir)/partition.py --input $< --output $(arc_dir) --top $(VLOG_MODEL) --circt-opt $(CIRCT_OPT) --cxx="$(CXX)" $(if $(filter native,$(ARC_RUNTIME)),--native-sparse-memories --native-combinational-models)
	$(PYTHON) $(sim_dir)/compiler/partition.py --input $(arc_dir)/partitioned.mlir --output $(arc_dir)/outlined.mlirbc \
		--circt-opt $(CIRCT_OPT) --cache $(gen_dir)/compiler --cxx="$(CXX)" \
		--max-ops=$(ARC_PARTITION_OPS) --max-ports=$(ARC_PARTITION_PORTS) --emit-bytecode $(if $(filter native,$(ARC_RUNTIME)),--native-feedback)
	$(CIRCT_OPT) $(arc_dir)/outlined.mlirbc --symbol-dce --canonicalize -o $(arc_dir)/model.mlir.tmp
	# Refresh every grouped output after a successful partition build. Leaving an
	# identical model older than its settings would repeat this stage forever.
	mv $(arc_dir)/model.mlir.tmp $(arc_dir)/model.mlir

# Separate debug compilation keeps observation and waveform instrumentation out
# of the optimized normal model.
define arc_compile
$$(eval $$(call arc_settings,$(1)/model-settings.json,PYTHON ARCILATOR CIRCT_OPT MLIR_TRANSLATE LLVM_OPT CXX ARC_MODEL_OPS ARC_SHARED_PROCEDURES ARC_ABI_BUFFERS ARC_EARLY_INLINE ARC_LATE_WIDE_ARITHMETIC ARC_OUTPUT_CONVERGENCE ARC_IDLE_CLOCKS ARC_STABLE_EVALUATOR ARC_PARTITION_PORTS ARCILATOR_FLAGS,$(1)))
$$(eval $$(call arc_settings,$(1)/codegen-settings.json,PYTHON LLVM_OPT LLC CXX ARC_OPT_LEVEL ARC_LLC_OPT_LEVEL ARC_ABI_BUFFERS ARC_CODEGEN_PIPELINE ARC_DISTRIBUTE_WIDE_STORES ARC_VECTOR_PACKING ARC_OPT_FLAGS,$(1)))
$(1)/model.ll $(1)/model.json &: $(arc_dir)/model.mlir $(sim_dir)/compiler/model.py $(sim_dir)/compiler/Model.cpp $(sim_dir)/compiler/WideArithmetic.cpp $(sim_dir)/compiler/WideArithmetic.h $(sim_dir)/compiler/SamplingFreeze.cpp $(sim_dir)/compiler/Convergence.cpp $(sim_dir)/compiler/IdleClock.cpp $(sim_dir)/compiler/ArcToLLVM.cpp $(sim_dir)/compiler/FloatMath.cpp $(sim_dir)/compiler/FormatStrings.cpp $(sim_dir)/compiler/NativePlusArgs.cpp $(sim_dir)/compiler/StateProperties.cpp $(sim_dir)/compiler/LowerArrays.cpp $(sim_dir)/compiler/RemoveI0Types.cpp $(sim_dir)/compiler/CompactStructs.cpp $(sim_dir)/compiler/partition.py $(sim_dir)/compiler/llvm.py $(if $(filter 1,$(ARC_SHARED_PROCEDURES)),$(sim_dir)/compiler/SharedProcedures.cpp) $(sim_dir)/compiler/LargeReturns.cpp $(sim_dir)/compiler/LargeArguments.cpp $(1)/model-settings.json $(if $(filter native,$(ARC_RUNTIME)),$(arc_dir)/partition.json) | $(1)
	$(PYTHON) $(sim_dir)/compiler/model.py --input=$$< --state-file=$(1)/model.json.tmp --output=$(1)/model.ll.tmp \
		--arcilator=$(ARCILATOR) --circt-opt=$(CIRCT_OPT) --mlir-translate=$(MLIR_TRANSLATE) \
		--cache=$(gen_dir)/compiler --cxx="$(CXX)" --max-ops=$(ARC_MODEL_OPS) --max-ports=$(ARC_PARTITION_PORTS) --output-convergence=$(if $(filter 1,$(ARC_OUTPUT_CONVERGENCE)),true,false) --idle-clocks=$(if $(filter 1,$(ARC_IDLE_CLOCKS)),true,false) --stable-evaluator=$(if $(filter 1,$(ARC_STABLE_EVALUATOR)),true,false) --late-wide-arithmetic=$(if $(filter 1,$(ARC_LATE_WIDE_ARITHMETIC)),true,false) $(if $(filter native,$(ARC_RUNTIME)),--partition=$(arc_dir)/partition.json) \
		-- $(ARCILATOR_FLAGS) $(2)
	$(PYTHON) $(sim_dir)/compiler/llvm.py $(1)/model.ll.tmp --llvm-opt=$(LLVM_OPT) --cache=$(gen_dir)/compiler --cxx="$(CXX)" $(if $(filter 1,$(ARC_SHARED_PROCEDURES)),--share-procedures,--no-share-procedures) $(if $(filter 0,$(ARC_ABI_BUFFERS)),--no-abi-buffers) $(if $(filter 0,$(ARC_EARLY_INLINE)),--no-early-inline)
	mv $(1)/model.json.tmp $(1)/model.json
	mv $(1)/model.ll.tmp $(1)/model.ll
$(1)/model.o: $(1)/model.ll $(1)/codegen-settings.json $(sim_dir)/compiler/codegen.py $(sim_dir)/compiler/Codegen.cpp $(sim_dir)/compiler/AggregateStores.cpp $(sim_dir)/compiler/SplitCodegen.cpp $(sim_dir)/compiler/LargeReturns.cpp $(sim_dir)/compiler/LargeArguments.cpp $(sim_dir)/compiler/partition.py $(if $(filter 1,$(ARC_DISTRIBUTE_WIDE_STORES)),$(sim_dir)/compiler/WideStores.cpp) $(if $(filter 1,$(ARC_VECTOR_PACKING)),$(sim_dir)/compiler/VectorPacking.cpp)
	$(PYTHON) $(sim_dir)/compiler/codegen.py --input=$$< --output=$$@ --bitcode=$(1)/model.bc \
		--llvm-opt=$(LLVM_OPT) --llc=$(LLC) --cache=$(gen_dir)/compiler --cxx="$(CXX)" \
		--opt-level=$(ARC_OPT_LEVEL) --opt-flags=$(call arc_shell_quote,$(ARC_OPT_FLAGS)) --llc-opt-level=$(ARC_LLC_OPT_LEVEL) --jobs=$(ARC_BUILD_JOBS) --pipeline=$(ARC_CODEGEN_PIPELINE) $(if $(filter 0,$(ARC_ABI_BUFFERS)),--no-abi-buffers) $(if $(filter 1,$(ARC_DISTRIBUTE_WIDE_STORES)),--distribute-wide-stores) $(if $(filter 1,$(ARC_VECTOR_PACKING)),--vector-packing) \
		--report=$(1)/codegen-report.json
$(1)/adapter.cpp: $(1)/model.json $(arc_dir)/partition.json $(sim_dir)/adapter.py $(sim_dir)/trace.h
	$(PYTHON) $(sim_dir)/adapter.py $$< $(arc_dir)/partition.json $$@
endef
$(eval $(call arc_compile,$(arc_dir),))
$(eval $(call arc_compile,$(arc_debug_dir),--observe-registers --observe-wires --observe-ports))

# The shared filelists include every selected blackbox's HDL and C++ sources,
# plus externally supplied IP. The native generated modules are replaced by the
# Arcilator shell; they must never also be compiled into the Verilator model.
$(arc_dir)/blackboxes.f: $(sim_dir)/Makefile $(sim_common_files) $(BB_MODS_FILELIST) $(EXT_FILELISTS) $(EXTRA_SIM_SOURCES) $(arc_dir)/filelist-settings.json $(if $(filter native,$(ARC_RUNTIME)),$(TOP_SMEMS_FILE) $(MODEL_SMEMS_FILE)) | $(arc_dir)
	awk '!/\.(h|svh|conf)$$/' $(BB_MODS_FILELIST) $(EXT_FILELISTS) > $@
	# Newer firtool versions put peripheral drivers in the model filelist while
	# leaving the blackbox filelist empty. Collect C/C++ from the complete list;
	# native_build.py deduplicates sources shared with the blackbox list.
	$(if $(filter native,$(ARC_RUNTIME)),awk '/\.(c|cc|cpp|cxx)$$/' $(sim_common_files) >> $@,true)
	printf '%s\n' $(GEN_COLLATERAL_DIR)/TestDriver.v >> $@
	$(if $(EXTRA_SIM_SOURCES),printf '%s\n' $(EXTRA_SIM_SOURCES) >> $@,true)
	$(if $(filter native,$(ARC_RUNTIME)),printf '%s\n' $(TOP_SMEMS_FILE) $(MODEL_SMEMS_FILE) >> $@,true)

ARC_CXXFLAGS = -I$(CIRCT_ROOT)/include $(SIM_CXXFLAGS) -I$(sim_dir) -DVERILATOR -DARCILATOR
ARC_LDFLAGS = $(SIM_LDFLAGS) $(ARC_RUNTIME_LIBS)

# The native scheduler and model bindings call the existing peripheral drivers
# directly. This target does not invoke or link Verilator, including its DPI ABI.
define arc_native_build
$$(eval $$(call arc_settings,$(1)/native-build-settings.json,PYTHON CXX CC CIRCT_ROOT SIM_CXXFLAGS ARC_CFLAGS ARC_LDFLAGS,$(1)))
$(1)/native-main.cpp: $(1)/model.json $(arc_dir)/partition.json $(arc_dir)/blackboxes.f $(arc_dir)/rtl.json $(sim_dir)/native.py $(sim_dir)/native_exports.py $(sim_dir)/filelists.py
	$(PYTHON) $(sim_dir)/native.py $(1)/model.json $(arc_dir)/partition.json $$@ --filelist=$(arc_dir)/blackboxes.f
$(2): $(1)/native-main.cpp $(1)/model.o $(arc_dir)/blackboxes.f $(arc_dir)/driver-inputs.json $(sim_dir)/native_build.py $(sim_dir)/filelists.py $(wildcard $(sim_dir)/native/*.cpp $(sim_dir)/native/*.h $(sim_dir)/native/*.def $(sim_dir)/native/include/*.h) $(dramsim_lib) $(EXTRA_SIM_REQS) $(1)/native-build-settings.json
	$(PYTHON) $(sim_dir)/native_build.py --filelist=$(arc_dir)/blackboxes.f \
		--main=$(1)/native-main.cpp --model=$(1)/model.o \
		--output=$$@ --build=$(1)/native-objects --cxx="$(CXX)" --cc="$(CC)" \
		--cxxflags="-I$(CIRCT_ROOT)/include $(SIM_CXXFLAGS)" --cflags="$(ARC_CFLAGS)" \
		--ldflags="$(ARC_LDFLAGS)" --jobs=$(ARC_BUILD_JOBS)
endef
$(eval $(call arc_native_build,$(arc_dir),$(sim_native)))
$(eval $(call arc_native_build,$(arc_debug_dir),$(sim_native_debug)))
ARC_VFLAGS = --main --timing --cc --exe --vpi --assert \
	--unroll-count 256 --max-num-width 1048576 --x-assign fast --x-initial fast \
	-Wno-fatal --top-module $(TB) --timescale 1ns/1ps \
	$(SIM_PREPROC_DEFINES) +define+VERILATOR +define+ARCILATOR \
	$(addprefix +incdir+,$(GEN_COLLATERAL_DIR) $(EXT_INCDIRS)) \
	-f $(arc_dir)/blackboxes.f $(arc_dir)/ArcHarness.sv $(EXTRA_SIM_SOURCES) $(ARC_VERILATOR_FLAGS)

$(eval $(call arc_settings,$(arc_dir)/hybrid-build-settings.json,PYTHON VERILATOR ARC_VFLAGS ARC_CXXFLAGS ARC_LDFLAGS ARC_CFLAGS CXX CC,$(arc_dir)))
$(eval $(call arc_settings,$(arc_debug_dir)/hybrid-build-settings.json,PYTHON VERILATOR ARC_VFLAGS ARC_CXXFLAGS ARC_LDFLAGS ARC_CFLAGS CXX CC,$(arc_debug_dir)))

$(sim_hybrid): $(arc_dir)/adapter.cpp $(arc_dir)/model.o $(arc_dir)/ArcHarness.sv $(arc_dir)/blackboxes.f $(sim_dir)/c_sources.py $(sim_dir)/native/wide.cpp $(sim_dir)/native/format.cpp $(sim_dir)/native/evaluation.cpp $(dramsim_lib) $(EXTRA_SIM_REQS) $(arc_dir)/hybrid-build-settings.json | check-arcilator-tools
	$(require_riscv)
	$(VERILATOR) $(ARC_VFLAGS) -Mdir $(arc_dir)/verilated -o $@ \
		-CFLAGS "$(ARC_CXXFLAGS)" -LDFLAGS "$(ARC_LDFLAGS)" $(arc_dir)/adapter.cpp $(sim_dir)/native/wide.cpp $(sim_dir)/native/format.cpp $(sim_dir)/native/evaluation.cpp $(arc_dir)/model.o
	$(PYTHON) $(sim_dir)/c_sources.py $(arc_dir)/verilated/V$(TB).mk $(arc_dir)/verilated/arc-c-sources.mk
	$(MAKE) -C $(arc_dir)/verilated -f V$(TB).mk -f arc-c-sources.mk -j $(ARC_BUILD_JOBS) ARC_CFLAGS="$(ARC_CFLAGS)"

$(sim_hybrid_debug): $(arc_debug_dir)/adapter.cpp $(arc_debug_dir)/model.o $(arc_dir)/ArcHarness.sv $(arc_dir)/blackboxes.f $(sim_dir)/c_sources.py $(sim_dir)/native/wide.cpp $(sim_dir)/native/format.cpp $(sim_dir)/native/evaluation.cpp $(dramsim_lib) $(EXTRA_SIM_REQS) $(arc_debug_dir)/hybrid-build-settings.json | check-arcilator-tools
	$(require_riscv)
	$(VERILATOR) $(ARC_VFLAGS) +define+DEBUG --trace -Mdir $(arc_debug_dir)/verilated -o $@ \
		-CFLAGS "$(ARC_CXXFLAGS) -DARC_TRACE" -LDFLAGS "$(ARC_LDFLAGS)" $(arc_debug_dir)/adapter.cpp $(sim_dir)/native/wide.cpp $(sim_dir)/native/format.cpp $(sim_dir)/native/evaluation.cpp $(arc_debug_dir)/model.o
	$(PYTHON) $(sim_dir)/c_sources.py $(arc_debug_dir)/verilated/V$(TB).mk $(arc_debug_dir)/verilated/arc-c-sources.mk
	$(MAKE) -C $(arc_debug_dir)/verilated -f V$(TB).mk -f arc-c-sources.mk -j $(ARC_BUILD_JOBS) ARC_CFLAGS="$(ARC_CFLAGS)"

.PHONY: clean clean-sim clean-sim-debug
clean:
	rm -rf $(gen_dir) $(sim_prefix)-*
clean-sim:
	rm -rf $(arc_dir) $(sim)
clean-sim-debug:
	rm -rf $(arc_debug_dir) $(sim_debug)

# Compiled fixtures require no Scala elaboration. The SV comparison is optional.
.PHONY: test native-test hybrid-test frontend-test llvm-native-test native-vpi-test native-analog-test native-source-metadata-test native-filelist-test native-jtag-test native-predictor-test native-predictor-model-test native-block-device-test native-network-test native-debug-rob-test native-traffic-test
test: native-test
hybrid-test:
	CIRCT_BIN=$(CIRCT_BIN) VERILATOR=$(VERILATOR) $(PYTHON) $(sim_dir)/tests/run.py
frontend-test:
	CIRCT_BIN=$(CIRCT_BIN) CXX="$(CXX)" $(PYTHON) $(sim_dir)/tests/frontend_compat.py --circt-bin=$(CIRCT_BIN)
llvm-native-test:
	CIRCT_BIN=$(CIRCT_BIN) CXX="$(CXX)" $(PYTHON) $(sim_dir)/tests/codegen.py
	CIRCT_BIN=$(CIRCT_BIN) CXX="$(CXX)" $(PYTHON) $(sim_dir)/tests/compiler_modes.py
	CIRCT_BIN=$(CIRCT_BIN) CXX="$(CXX)" $(PYTHON) $(sim_dir)/tests/compiler_opt_flags.py
	CIRCT_BIN=$(CIRCT_BIN) CXX="$(CXX)" $(PYTHON) $(sim_dir)/tests/large_returns.py
	CIRCT_BIN=$(CIRCT_BIN) CXX="$(CXX)" $(PYTHON) $(sim_dir)/tests/large_returns_cfg.py
	CIRCT_BIN=$(CIRCT_BIN) CXX="$(CXX)" $(PYTHON) $(sim_dir)/tests/large_arguments.py
	CIRCT_BIN=$(CIRCT_BIN) CXX="$(CXX)" $(PYTHON) $(sim_dir)/tests/compact_structs.py --input=$(sim_dir)/generated-src/large-return-test/input.ll --reference=$(sim_dir)/generated-src/large-return-test/check.cpp
native-filelist-test:
	CIRCT_BIN=$(CIRCT_BIN) CXX="$(CXX)" CC="$(CC)" $(PYTHON) $(sim_dir)/tests/native_filelists.py --circt-bin=$(CIRCT_BIN)
native-jtag-test:
	CIRCT_BIN=$(CIRCT_BIN) CXX="$(CXX)" $(PYTHON) $(sim_dir)/tests/jtag_timing.py --circt-bin=$(CIRCT_BIN)
native-predictor-test:
	CIRCT_BIN=$(CIRCT_BIN) CXX="$(CXX)" $(PYTHON) $(sim_dir)/tests/native_predictors.py --circt-bin=$(CIRCT_BIN)
native-predictor-model-test:
	CIRCT_BIN=$(CIRCT_BIN) CXX="$(CXX)" $(PYTHON) $(sim_dir)/tests/native_predictor_model.py --circt-bin=$(CIRCT_BIN)
native-block-device-test:
	CIRCT_BIN=$(CIRCT_BIN) CXX="$(CXX)" $(PYTHON) $(sim_dir)/tests/native_block_device.py --circt-bin=$(CIRCT_BIN) $(if $(RISCV),--riscv="$(RISCV)")
native-network-test:
	CIRCT_BIN=$(CIRCT_BIN) CXX="$(CXX)" $(PYTHON) $(sim_dir)/tests/native_network.py --circt-bin=$(CIRCT_BIN) $(if $(RISCV),--riscv="$(RISCV)")
native-debug-rob-test:
	CIRCT_BIN=$(CIRCT_BIN) CXX="$(CXX)" $(PYTHON) $(sim_dir)/tests/native_debug_rob.py --circt-bin=$(CIRCT_BIN)
native-traffic-test:
	CIRCT_BIN=$(CIRCT_BIN) CXX="$(CXX)" CC="$(CC)" $(PYTHON) $(sim_dir)/tests/native_traffic.py --circt-bin=$(CIRCT_BIN)
native-vpi-test:
	CIRCT_BIN=$(CIRCT_BIN) CXX="$(CXX)" CC="$(CC)" $(PYTHON) $(sim_dir)/tests/vpi_lifecycle.py
native-analog-test:
	CIRCT_BIN=$(CIRCT_BIN) CXX="$(CXX)" $(PYTHON) $(sim_dir)/tests/analog_nets.py --circt-bin=$(CIRCT_BIN)
native-source-metadata-test:
	CIRCT_BIN=$(CIRCT_BIN) CXX="$(CXX)" $(PYTHON) $(sim_dir)/tests/vpi_source_metadata.py --circt-bin=$(CIRCT_BIN) --work=$(sim_dir)/generated-src/native-vpi-source-metadata-test $(if $(ARC_SOURCE_METADATA_FRONTEND),--frontend=$(ARC_SOURCE_METADATA_FRONTEND))
native-test: frontend-test llvm-native-test native-vpi-test native-analog-test native-source-metadata-test native-filelist-test native-jtag-test native-predictor-test native-predictor-model-test native-block-device-test native-network-test native-debug-rob-test native-traffic-test
	$(PYTHON) $(sim_dir)/tests/rtl_libraries.py
	CIRCT_BIN=$(CIRCT_BIN) $(PYTHON) $(sim_dir)/tests/run_native.py

# Use the selected configuration's original generated cipher driver.
.PHONY: caliptra-aes-test
caliptra-aes-test: $(arc_dir)/design.mlir
	CXX="$(CXX)" $(PYTHON) $(sim_dir)/tests/caliptra_aes.py --circt-bin=$(CIRCT_BIN) \
		--design=$(arc_dir)/design.mlir --work=$(arc_dir)/aes-regression

# Full Chipyard elaboration/build/runtime tests, with logs and a JSON report.
ARC_TEST_CASES ?=
ARC_TEST_TIMEOUT ?= 1800
.PHONY: integration-test
integration-test: check-arcilator-tools
	$(require_riscv)
	CIRCT_BIN=$(CIRCT_BIN) RISCV=$(RISCV) $(PYTHON) $(sim_dir)/tests/integration.py --runtime $(ARC_RUNTIME) --timeout $(ARC_TEST_TIMEOUT) $(if $(ARC_TEST_CASES),--cases $(ARC_TEST_CASES))

.PHONY: check-arcilator-tools
check-arcilator-tools:
	@for tool in $(ARC_FIRTOOL) $(CIRCT_VERILOG) $(CIRCT_OPT) $(ARCILATOR) $(LLVM_OPT) $(LLC) $(LLVM_CONFIG) $(MLIR_TRANSLATE); do \
	  command -v "$$tool" >/dev/null 2>&1 || { echo "Missing $$tool. Source env.sh and install the full CIRCT suite pinned in conda-reqs/circt.json using build-setup.sh step 10." >&2; exit 1; }; \
	done
	@test -r $(CIRCT_ROOT)/include/circt/Dialect/Arc/Runtime/ArcRuntime.h -a -r $(CIRCT_ROOT)/lib/libCIRCTArcRuntime.a || \
	  { echo "CIRCT_ROOT=$(CIRCT_ROOT) must contain the matching CIRCT headers and runtime libraries." >&2; exit 1; }
	@$(ARC_FIRTOOL) --help | grep -q -- --lower-to-core || \
	  { echo "The native firtool needs --lower-to-core; use a compatible full CIRCT release (tested: 1.162.0)." >&2; exit 1; }
