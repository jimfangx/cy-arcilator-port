module {
  hw.module.extern private @SimDTM(in %clk : !seq.clock, in %reset : i1, out debug_req_valid : i1, in %debug_req_ready : i1, out debug_req_bits_addr : i7, out debug_req_bits_op : i2, out debug_req_bits_data : i32, in %debug_resp_valid : i1, out debug_resp_ready : i1, in %debug_resp_bits_resp : i2, in %debug_resp_bits_data : i32, out exit : i32)
  hw.module @NativeDTMHarness(in %clock : i1, in %reset : i1, in %request_ready : i1, in %response_valid : i1, in %response : i2, in %response_data : i32, out request_valid : i1, out address : i7, out operation : i2, out data : i32, out response_ready : i1, out exit : i32) {
    %clk = seq.to_clock %clock
    %req_valid, %addr, %op, %data, %resp_ready, %exit = hw.instance "dtm" @SimDTM(clk: %clk: !seq.clock, reset: %reset: i1, debug_req_ready: %request_ready: i1, debug_resp_valid: %response_valid: i1, debug_resp_bits_resp: %response: i2, debug_resp_bits_data: %response_data: i32) -> (debug_req_valid: i1, debug_req_bits_addr: i7, debug_req_bits_op: i2, debug_req_bits_data: i32, debug_resp_ready: i1, exit: i32)
    hw.output %req_valid, %addr, %op, %data, %resp_ready, %exit : i1, i7, i2, i32, i1, i32
  }
}
