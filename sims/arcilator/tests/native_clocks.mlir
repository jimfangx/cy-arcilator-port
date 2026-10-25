// Original Rocket Chip fixed/plusarg clocks and Chipyard's harness clock.
module {
  hw.module.extern private @ClockSourceAtFreq<PERIOD_PS: f64>(in %power: i1, in %gate: i1, out clk: !seq.clock)
  hw.module.extern private @ClockSourceAtFreqMHz<PERIOD: f64>(in %power: i1, in %gate: i1, out clk: !seq.clock)
  hw.module.extern private @ClockSourceAtFreqFromPlusArgUNIT_CLOCK_MHZ(in %power: i1, in %gate: i1, out clk: !seq.clock)
  hw.module @OriginalClockProbe(in %clock: i1, in %reset: i1, in %power: i1, in %gate: i1, out fixed: i1, out harness: i1, out plusarg: i1, out fixed_count: i16, out harness_count: i16, out plusarg_count: i16) {
    %zero = hw.constant 0 : i16
    %one = hw.constant 1 : i16
    %a = hw.instance "fixed" @ClockSourceAtFreq<PERIOD_PS: f64 = 1.000000e+04>(power: %power: i1, gate: %gate: i1) -> (clk: !seq.clock)
    %b = hw.instance "harness" @ClockSourceAtFreqMHz<PERIOD: f64 = 1.000000e+01>(power: %power: i1, gate: %gate: i1) -> (clk: !seq.clock)
    %c = hw.instance "plusarg" @ClockSourceAtFreqFromPlusArgUNIT_CLOCK_MHZ(power: %power: i1, gate: %gate: i1) -> (clk: !seq.clock)
    %next_a = comb.add %count_a, %one : i16
    %next_b = comb.add %count_b, %one : i16
    %next_c = comb.add %count_c, %one : i16
    %count_a = seq.firreg %next_a clock %a reset sync %reset, %zero : i16
    %count_b = seq.firreg %next_b clock %b reset sync %reset, %zero : i16
    %count_c = seq.firreg %next_c clock %c reset sync %reset, %zero : i16
    %level_a = seq.from_clock %a
    %level_b = seq.from_clock %b
    %level_c = seq.from_clock %c
    hw.output %level_a, %level_b, %level_c, %count_a, %count_b, %count_c : i1, i1, i1, i16, i16, i16
  }
}
