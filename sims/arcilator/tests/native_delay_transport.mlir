module {
  hw.module @DelayTransportHarness(in %clock : i1, in %value : i13, in %wide65 : i65, in %wide193 : i193, out q13 : i13, out qfast : i13, out qchain : i13, out q65 : i65, out q193 : i193, out constant_value : i13, out delayed_clock : i1, out count : i32) {
    %c123 = hw.constant 291 : i13
    %c0 = hw.constant 0 : i32
    %c1 = hw.constant 1 : i32
    %q13 = llhd.delay %value by <1000fs, 0d, 0e> : i13
    %qfast = llhd.delay %value by <250fs, 0d, 0e> : i13
    %qchain = llhd.delay %qfast by <750fs, 0d, 0e> : i13
    %q65 = llhd.delay %wide65 by <1750fs, 0d, 0e> : i65
    %q193 = llhd.delay %wide193 by <1500fs, 0d, 0e> : i193
    %constant_value = llhd.delay %c123 by <300fs, 0d, 0e> : i13
    %delayed_clock = llhd.delay %clock by <750fs, 0d, 0e> : i1
    %clk = seq.to_clock %delayed_clock
    %next = comb.add %count, %c1 : i32
    %count = seq.compreg %next, %clk : i32
    hw.output %q13, %qfast, %qchain, %q65, %q193, %constant_value, %delayed_clock, %count : i13, i13, i13, i65, i193, i13, i1, i32
  }
}
