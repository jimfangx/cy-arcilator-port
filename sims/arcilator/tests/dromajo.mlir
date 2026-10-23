module {
  hw.module.extern private @SimDromajoCosimBlackBox<COMMIT_WIDTH: ui32, XLEN: ui32, INST_BITS: ui32, HARTID_LEN: ui32>(in %clock: !seq.clock, in %reset: i1, in %valid: i3, in %hartid: i5, in %pc: i192, in %inst: i96, in %wdata: i192, in %mstatus: i192, in %check: i3, in %int_xcpt: i1, in %cause: i64)
  hw.module @DromajoHarness(in %clock: i1, in %reset: i1, in %valid: i3, in %hartid: i5, in %pc: i192, in %inst: i96, in %wdata: i192, in %mstatus: i192, in %check: i3, in %int_xcpt: i1, in %cause: i64, out io_success: i1) {
    %clk = seq.to_clock %clock
    %false = hw.constant false
    hw.instance "monitor" @SimDromajoCosimBlackBox<COMMIT_WIDTH: ui32 = 3, XLEN: ui32 = 64, INST_BITS: ui32 = 32, HARTID_LEN: ui32 = 5>(clock: %clk: !seq.clock, reset: %reset: i1, valid: %valid: i3, hartid: %hartid: i5, pc: %pc: i192, inst: %inst: i96, wdata: %wdata: i192, mstatus: %mstatus: i192, check: %check: i3, int_xcpt: %int_xcpt: i1, cause: %cause: i64) -> ()
    hw.output %false : i1
  }
}
