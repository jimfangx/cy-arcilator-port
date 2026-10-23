module {
  hw.module @NativeControlHarness(in %clock : i1, in %reset : i1, in %fail : i1, out io_success : i1) {
    %false = hw.constant false
    %clk = seq.to_clock %clock
    %message = sim.fmt.literal "NATIVE_GUARD_MARKER\0A"
    sim.triggered %clk {
      sim.proc.print %message
      scf.if %fail {
        sim.terminate failure, quiet
      } else {
        sim.terminate success, quiet
      }
    }
    hw.output %false : i1
  }
}
