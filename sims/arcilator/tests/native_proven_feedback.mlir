module {
  hw.module @NativeFeedbackHarness(in %clock: i1, in %mode: i2, in %a: i13, in %b: i13, out x: i13, out y: i13, out sampled: i13) {
    %zero = hw.constant 0 : i13
    %mode0 = hw.constant 0 : i2
    %mode1 = hw.constant 1 : i2
    %p = comb.icmp eq %mode, %mode0 : i2
    %q = comb.icmp eq %mode, %mode1 : i2
    %a_selected = comb.mux %p, %a, %zero : i13
    %b_selected = comb.mux %q, %b, %zero : i13
    %through_b = comb.mux %q, %y, %zero : i13
    %x = comb.or %a_selected, %through_b : i13
    %through_a = comb.mux %p, %x, %zero : i13
    %y = comb.or %b_selected, %through_a : i13
    %clk = seq.to_clock %clock
    %sampled = seq.firreg %x clock %clk : i13
    hw.output %x, %y, %sampled : i13, i13, i13
  }
}
