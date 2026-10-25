// Multi-bit parameters are converted to the original DPI bit inputs.
module {
  hw.module.extern private @SimMemTrace<FILENAME: none, HAS_SOURCE: i32, NUM_LANES: i32>(in %clock: !seq.clock, in %reset: i1, in %trace_read_cycle: i64, in %trace_read_ready: i1, out trace_read_valid: i3, out trace_read_address: i192, out trace_read_is_store: i3, out trace_read_size: i24, out trace_read_data: i192, out trace_read_finished: i1)
  hw.module.extern private @SimMemTraceLogger<IS_RESPONSE: i32, FILENAME_BASE: none, FILENAME_SUFFIX: none, NUM_LANES: i32>(in %clock: !seq.clock, in %reset: i1, in %trace_log_valid: i3, in %trace_log_source: i96, in %trace_log_address: i192, in %trace_log_is_store: i3, in %trace_log_size: i24, in %trace_log_data: i192, out trace_log_ready: i1)
  hw.module @MemoryTraceModelProbe(in %clock: i1, in %reset: i1, in %trace_cycle: i64, in %trace_ready: i1, out trace_valid: i3, out trace_address: i192, out trace_is_store: i3, out trace_size: i24, out trace_data: i192, out trace_finished: i1, in %log_valid: i3, in %log_source: i96, in %log_address: i192, in %log_is_store: i3, in %log_size: i24, in %log_data: i192, out log_ready: i1) {
    %clk = seq.to_clock %clock
    %valid, %address, %store, %size, %data, %finished = hw.instance "reader" @SimMemTrace<FILENAME: none = "unused-reader", HAS_SOURCE: i32 = 2, NUM_LANES: i32 = 3>(clock: %clk: !seq.clock, reset: %reset: i1, trace_read_cycle: %trace_cycle: i64, trace_read_ready: %trace_ready: i1) -> (trace_read_valid: i3, trace_read_address: i192, trace_read_is_store: i3, trace_read_size: i24, trace_read_data: i192, trace_read_finished: i1)
    %ready = hw.instance "logger" @SimMemTraceLogger<IS_RESPONSE: i32 = 3, FILENAME_BASE: none = "unused-logger", FILENAME_SUFFIX: none = ".native-log", NUM_LANES: i32 = 3>(clock: %clk: !seq.clock, reset: %reset: i1, trace_log_valid: %log_valid: i3, trace_log_source: %log_source: i96, trace_log_address: %log_address: i192, trace_log_is_store: %log_is_store: i3, trace_log_size: %log_size: i24, trace_log_data: %log_data: i192) -> (trace_log_ready: i1)
    hw.output %valid, %address, %store, %size, %data, %finished, %ready : i3, i192, i3, i24, i192, i1, i1
  }
}
