module {
  sim.func.dpi private @native_bare_feedback_tick(in %data: i32, in %state: i32, return result: i32)
  hw.module @NativeBareHarness(in %clock: i1, in %reset: i1, in %data: i32, out value: i32, out q: i32, out io_success: i1) {
    %zero=hw.constant 0:i32
    %false=hw.constant false
    %clk=seq.to_clock %clock
    %value=sim.func.dpi.call @native_bare_feedback_tick(%data,%value) : (i32,i32) -> i32
    %q=seq.firreg %value clock %clk reset sync %reset,%zero:i32
    hw.output %value,%q,%false:i32,i32,i1
  }
}
