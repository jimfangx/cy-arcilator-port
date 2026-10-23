module {
  hw.module.extern private @ArcRTLPortRules<WIDTH: ui32, BIAS: si32, TAG: none>(in %clock: !seq.clock, in %reset: i1, in %signed: i3, in %wide: i9, out narrow_seen: i9, out unsigned_out: i9, out signed_out: i9, out truncated: i5, out defaults: i13, out flags: i8, out q: i9)
  hw.module @NativePortRulesHarness(in %clock: i1, in %reset: i1, in %small: i3, in %wide: i9, out a_narrow: i9, out a_unsigned: i9, out a_signed: i9, out a_truncated: i5, out a_defaults: i13, out a_flags: i8, out a_q: i9, out a_sampled: i9, out b_narrow: i9, out b_unsigned: i9, out b_signed: i9, out b_truncated: i5, out b_defaults: i13, out b_flags: i8, out b_q: i9, out b_sampled: i9) {
    %clk = seq.to_clock %clock
    %a0, %a1, %a2, %a3, %a4, %a5, %aq = hw.instance "five" @ArcRTLPortRules<WIDTH: ui32 = 5, BIAS: si32 = 9, TAG: none = "five">(clock: %clk: !seq.clock, reset: %reset: i1, signed: %small: i3, wide: %wide: i9) -> (narrow_seen: i9, unsigned_out: i9, signed_out: i9, truncated: i5, defaults: i13, flags: i8, q: i9)
    %b0, %b1, %b2, %b3, %b4, %b5, %bq = hw.instance "seven" @ArcRTLPortRules<WIDTH: ui32 = 7, BIAS: si32 = -7, TAG: none = "seven">(clock: %clk: !seq.clock, reset: %reset: i1, signed: %small: i3, wide: %wide: i9) -> (narrow_seen: i9, unsigned_out: i9, signed_out: i9, truncated: i5, defaults: i13, flags: i8, q: i9)
    %as = seq.firreg %aq clock %clk : i9
    %bs = seq.firreg %bq clock %clk : i9
    hw.output %a0, %a1, %a2, %a3, %a4, %a5, %aq, %as, %b0, %b1, %b2, %b3, %b4, %b5, %bq, %bs : i9, i9, i9, i5, i13, i8, i9, i9, i9, i9, i9, i5, i13, i8, i9, i9
  }
}
