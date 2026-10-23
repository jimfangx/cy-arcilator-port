// Original TrafficEval.v interface and an embedded multiline CONFIG_STR.
module {
  hw.module.extern private @TrafficEvalIngress<INGRESS_ID: i64, CONFIG_STR: none>(in %clock: !seq.clock, in %reset: i1, in %current_cycle: i64, in %flit_out_ready: i1, out flit_out_valid: i1, out flit_out_head: i1, out flit_out_tail: i1, out flit_out_egress_id: i64, out flit_out_unique_id: i64)
  hw.module.extern private @TrafficEvalEgress<EGRESS_ID: i64, CONFIG_STR: none>(in %clock: !seq.clock, in %reset: i1, in %current_cycle: i64, out flit_in_ready: i1, in %flit_in_valid: i1, in %flit_in_head: i1, in %flit_in_tail: i1, in %flit_in_ingress_id: i64, in %flit_in_unique_id: i64, out success: i1, out fatal: i1)
  hw.module @TrafficModelProbe(in %clock: i1, in %reset: i1, in %current_cycle: i64, in %tx_ready: i1, out tx_valid: i1, out tx_head: i1, out tx_tail: i1, out tx_egress: i64, out tx_unique: i64, out rx_ready: i1, in %rx_valid: i1, in %rx_head: i1, in %rx_tail: i1, in %rx_ingress: i64, in %rx_unique: i64, out success: i1, out fatal: i1) {
    %clk = seq.to_clock %clock
    %valid, %head, %tail, %endpoint, %unique = hw.instance "ingress" @TrafficEvalIngress<INGRESS_ID: i64 = 0, CONFIG_STR: none = "warmup 0\0Ameasurement 6\0Adrain 32\0Aflits_per_packet 2\0Aflow 0 0 2.0\0A">(clock: %clk: !seq.clock, reset: %reset: i1, current_cycle: %current_cycle: i64, flit_out_ready: %tx_ready: i1) -> (flit_out_valid: i1, flit_out_head: i1, flit_out_tail: i1, flit_out_egress_id: i64, flit_out_unique_id: i64)
    %ready, %success, %fatal = hw.instance "egress" @TrafficEvalEgress<EGRESS_ID: i64 = 0, CONFIG_STR: none = "warmup 0\0Ameasurement 6\0Adrain 32\0Aflits_per_packet 2\0Aflow 0 0 2.0\0A">(clock: %clk: !seq.clock, reset: %reset: i1, current_cycle: %current_cycle: i64, flit_in_valid: %rx_valid: i1, flit_in_head: %rx_head: i1, flit_in_tail: %rx_tail: i1, flit_in_ingress_id: %rx_ingress: i64, flit_in_unique_id: %rx_unique: i64) -> (flit_in_ready: i1, success: i1, fatal: i1)
    hw.output %valid, %head, %tail, %endpoint, %unique, %ready, %success, %fatal : i1, i1, i1, i64, i64, i1, i1, i1
  }
}
