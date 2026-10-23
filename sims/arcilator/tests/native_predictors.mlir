// Original BOOM harness port contracts, instantiated through compiled CIRCT
// wiring. The unchanged C++ drivers are supplied at the native link boundary.
module {
  hw.module.extern private @BranchPredictorHarness(in %clock : !seq.clock, in %reset : i1, in %req_valid : i1, in %req_pc : i64, in %req_hist : i64, out req_taken : i1, in %update_valid : i1, in %update_pc : i64, in %update_hist : i64, in %update_taken : i1)
  hw.module.extern private @BTBHarness(in %clock : !seq.clock, in %reset : i1, in %req_valid : i1, in %req_pc : i64, in %req_hist : i64, out req_target_valid : i1, out req_target_pc : i64, out req_is_br : i1, out req_is_jal : i1, in %update_valid : i1, in %update_pc : i64, in %update_hist : i64, in %update_target : i64, in %update_is_br : i1, in %update_is_jal : i1)
  hw.module @PredictorModelProbe(in %clock : i1, in %reset : i1, in %bp_req_valid : i1, in %bp_req_pc : i64, in %bp_req_hist : i64, in %bp_update_valid : i1, in %bp_update_pc : i64, in %bp_update_hist : i64, in %bp_update_taken : i1, in %btb_req_valid : i1, in %btb_req_pc : i64, in %btb_req_hist : i64, in %btb_update_valid : i1, in %btb_update_pc : i64, in %btb_update_hist : i64, in %btb_update_target : i64, in %btb_update_is_br : i1, in %btb_update_is_jal : i1, out bp_req_taken : i1, out btb_req_target_valid : i1, out btb_req_target_pc : i64, out btb_req_is_br : i1, out btb_req_is_jal : i1) {
    %clk = seq.to_clock %clock
    %bp_taken = hw.instance "bp" @BranchPredictorHarness(clock: %clk: !seq.clock, reset: %reset: i1, req_valid: %bp_req_valid: i1, req_pc: %bp_req_pc: i64, req_hist: %bp_req_hist: i64, update_valid: %bp_update_valid: i1, update_pc: %bp_update_pc: i64, update_hist: %bp_update_hist: i64, update_taken: %bp_update_taken: i1) -> (req_taken: i1)
    %bt_valid, %bt_target, %bt_br, %bt_jal = hw.instance "btb" @BTBHarness(clock: %clk: !seq.clock, reset: %reset: i1, req_valid: %btb_req_valid: i1, req_pc: %btb_req_pc: i64, req_hist: %btb_req_hist: i64, update_valid: %btb_update_valid: i1, update_pc: %btb_update_pc: i64, update_hist: %btb_update_hist: i64, update_target: %btb_update_target: i64, update_is_br: %btb_update_is_br: i1, update_is_jal: %btb_update_is_jal: i1) -> (req_target_valid: i1, req_target_pc: i64, req_is_br: i1, req_is_jal: i1)
    hw.output %bp_taken, %bt_valid, %bt_target, %bt_br, %bt_jal : i1, i1, i64, i1, i1
  }
}
