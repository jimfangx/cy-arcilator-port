module {
  hw.module.extern private @SimEmulator<NUM_LANES: ui32>(in %clock: !seq.clock, in %reset: i1, in %a_ready: i3, out a_valid: i3, out a_address: i192, out a_is_store: i3, out a_size: i24, out a_data: i192, out d_ready: i3, in %d_valid: i3, in %d_is_store: i3, in %d_size: i24, in %d_data: i192, in %inflight: i1, out finished: i1)
  hw.module @CyclotronEmulatorHarness(in %clock: i1, in %reset: i1, in %a_ready: i3, in %d_valid: i3, in %d_size: i24, in %d_data: i192, in %inflight: i1, out a_valid: i3, out a_address: i192, out a_is_store: i3, out a_size: i24, out a_data: i192, out d_ready: i3, out finished: i1, out io_success: i1) {
    %clk = seq.to_clock %clock
    %false = hw.constant false
    %no_stores = hw.constant 0 : i3
    %valid, %address, %store, %size, %data, %ready, %finished = hw.instance "emulator" @SimEmulator<NUM_LANES: ui32 = 3>(clock: %clk: !seq.clock, reset: %reset: i1, a_ready: %a_ready: i3, d_valid: %d_valid: i3, d_is_store: %no_stores: i3, d_size: %d_size: i24, d_data: %d_data: i192, inflight: %inflight: i1) -> (a_valid: i3, a_address: i192, a_is_store: i3, a_size: i24, a_data: i192, d_ready: i3, finished: i1)
    hw.output %valid, %address, %store, %size, %data, %ready, %finished, %false : i3, i192, i3, i24, i192, i3, i1, i1
  }
}
