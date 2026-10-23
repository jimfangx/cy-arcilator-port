module {
  sim.func.dpi private @chain(in %site: i32, in %data: i32, return result: i32) attributes {verilogName="chipyard_dpi_chain"}
  hw.module @DPIChainHarness(in %clock: i1, in %reset: i1, in %data: i32, out sampled: i32, out count: i32) {
    %clk = seq.to_clock %clock
    %zero = hw.constant 0 : i32
    %one = hw.constant 1 : i32
    %next = comb.add %count, %one : i32
    %count = seq.firreg %next clock %clk reset sync %reset, %zero {name="count"} : i32
    %input = comb.xor %data, %count : i32
    %s0 = hw.constant 0 : i32
    %r0 = sim.func.dpi.call @chain(%s0, %input) : (i32, i32) -> i32
    %s1 = hw.constant 1 : i32
    %r1 = sim.func.dpi.call @chain(%s1, %r0) : (i32, i32) -> i32
    %s2 = hw.constant 2 : i32
    %r2 = sim.func.dpi.call @chain(%s2, %r1) : (i32, i32) -> i32
    %s3 = hw.constant 3 : i32
    %r3 = sim.func.dpi.call @chain(%s3, %r2) : (i32, i32) -> i32
    %s4 = hw.constant 4 : i32
    %r4 = sim.func.dpi.call @chain(%s4, %r3) : (i32, i32) -> i32
    %s5 = hw.constant 5 : i32
    %r5 = sim.func.dpi.call @chain(%s5, %r4) : (i32, i32) -> i32
    %s6 = hw.constant 6 : i32
    %r6 = sim.func.dpi.call @chain(%s6, %r5) : (i32, i32) -> i32
    %s7 = hw.constant 7 : i32
    %r7 = sim.func.dpi.call @chain(%s7, %r6) : (i32, i32) -> i32
    %s8 = hw.constant 8 : i32
    %r8 = sim.func.dpi.call @chain(%s8, %r7) : (i32, i32) -> i32
    %s9 = hw.constant 9 : i32
    %r9 = sim.func.dpi.call @chain(%s9, %r8) : (i32, i32) -> i32
    %s10 = hw.constant 10 : i32
    %r10 = sim.func.dpi.call @chain(%s10, %r9) : (i32, i32) -> i32
    %s11 = hw.constant 11 : i32
    %r11 = sim.func.dpi.call @chain(%s11, %r10) : (i32, i32) -> i32
    %s12 = hw.constant 12 : i32
    %r12 = sim.func.dpi.call @chain(%s12, %r11) : (i32, i32) -> i32
    %s13 = hw.constant 13 : i32
    %r13 = sim.func.dpi.call @chain(%s13, %r12) : (i32, i32) -> i32
    %s14 = hw.constant 14 : i32
    %r14 = sim.func.dpi.call @chain(%s14, %r13) : (i32, i32) -> i32
    %s15 = hw.constant 15 : i32
    %r15 = sim.func.dpi.call @chain(%s15, %r14) : (i32, i32) -> i32
    %sampled = seq.firreg %r15 clock %clk reset sync %reset, %zero {name="sampled"} : i32
    hw.output %sampled, %count : i32, i32
  }
}
