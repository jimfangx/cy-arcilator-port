module {
  hw.module.extern @GI(in %pad : i1, in %ie : i1, out i : i1) attributes {verilogName = "GenericDigitalInIOCell"}
  hw.module.extern @GO(in %o : i1, in %oe : i1, out pad : i1) attributes {verilogName = "GenericDigitalOutIOCell"}
  hw.module.extern @INV(in %clockIn : !seq.clock, out clockOut : !seq.clock) attributes {verilogName = "ClockInverter"}
  hw.module.extern @OR(in %clocksIn_0 : !seq.clock, in %clocksIn_1 : !seq.clock, out clockOut : !seq.clock) attributes {verilogName = "ClockOr2"}
  hw.module.extern @NOR(in %clockIn : !seq.clock, in %signalIn : i1, out clockOut : !seq.clock) attributes {verilogName = "ClockSignalNor2"}
  hw.module.extern @GATE(in %in : !seq.clock, in %en : i1, in %test_en : i1, out out : !seq.clock) attributes {verilogName = "EICG_wrapper"}
  hw.module @TestHarness(in %clock : !seq.clock, in %peer : !seq.clock, in %reset : i1, in %en : i1, in %test : i1, in %second_en : i1, in %pad : i1, in %ie : i1, in %oe : i1, in %signal : i1, out gated : i1, out chained : i1, out inverted : i1, out joined : i1, out nor_clock : i1, out read : i1, out written : i1, out count0 : i8, out count1 : i8, out count2 : i8, out count3 : i8) {
    %c0_i8 = hw.constant 0 : i8
    %c1_i8 = hw.constant 1 : i8
    %false = hw.constant false
    %input.i = hw.instance "input" @GI(pad: %pad: i1, ie: %ie: i1) -> (i: i1)
    %output.pad = hw.instance "output" @GO(o: %input.i: i1, oe: %oe: i1) -> (pad: i1)
    %inverter.clockOut = hw.instance "inverter" @INV(clockIn: %clock: !seq.clock) -> (clockOut: !seq.clock)
    %or.clockOut = hw.instance "or" @OR(clocksIn_0: %clock: !seq.clock, clocksIn_1: %peer: !seq.clock) -> (clockOut: !seq.clock)
    %nor.clockOut = hw.instance "nor" @NOR(clockIn: %clock: !seq.clock, signalIn: %signal: i1) -> (clockOut: !seq.clock)
    %gate.out = hw.instance "gate" @GATE(in: %clock: !seq.clock, en: %en: i1, test_en: %test: i1) -> (out: !seq.clock)
    %chain.out = hw.instance "chain" @GATE(in: %gate.out: !seq.clock, en: %second_en: i1, test_en: %false: i1) -> (out: !seq.clock)
    %0 = seq.from_clock %gate.out
    %1 = seq.from_clock %chain.out
    %2 = seq.from_clock %inverter.clockOut
    %3 = seq.from_clock %or.clockOut
    %4 = seq.from_clock %nor.clockOut
    %5 = comb.add %q0, %c1_i8 : i8
    %q0 = seq.firreg %5 clock %gate.out reset async %reset, %c0_i8 : i8
    %6 = comb.add %q1, %c1_i8 : i8
    %q1 = seq.firreg %6 clock %chain.out reset async %reset, %c0_i8 : i8
    %7 = comb.add %q2, %c1_i8 : i8
    %q2 = seq.firreg %7 clock %inverter.clockOut reset async %reset, %c0_i8 : i8
    %8 = comb.add %q3, %c1_i8 : i8
    %q3 = seq.firreg %8 clock %nor.clockOut reset async %reset, %c0_i8 : i8
    hw.output %0, %1, %2, %3, %4, %input.i, %output.pad, %q0, %q1, %q2, %q3 : i1, i1, i1, i1, i1, i1, i1, i8, i8, i8, i8
  }
}

