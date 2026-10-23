module {
  sim.func.dpi private @record(in %count: i8, return value: i32) attributes {verilogName="scheduled_dpi_record"}
  hw.module.extern private @ClockSourceAtFreqMHz<PERIOD: f64>(in %power : i1, in %gate : i1, out clk : !seq.clock)
  hw.module @ScheduledDPIHarness(in %clock : i1, in %reset : i1, out io_success : i1, out count : i8, out observed : i32) {
    %true = hw.constant true
    %false = hw.constant false
    %zero = hw.constant 0 : i8
    %one = hw.constant 1 : i8
    %three = hw.constant 3 : i8
    %generated = hw.instance "source" @ClockSourceAtFreqMHz<PERIOD: f64 = 4.000000e+00>(power: %true: i1, gate: %false: i1) -> (clk: !seq.clock)
    %next = comb.add %counter, %one : i8
    %counter = seq.firreg %next clock %generated reset sync %reset, %zero : i8
    %observed = sim.func.dpi.call @record(%counter) : (i8) -> i32
    %done = comb.icmp eq %counter, %three : i8
    hw.output %done, %counter, %observed : i1, i8, i32
  }
}
