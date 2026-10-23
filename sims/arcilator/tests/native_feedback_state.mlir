module {
  hw.module.extern @NativeFeedbackStateForeign(in %clock: i1, in %set_i: i1, in %clear_i: i1, in %oscillate: i1, in %set_bits: i129, in %clear_bits: i129, out q: i1, out wide: i129, out sampled: i1, out unstable: i1)
  hw.module @NativeFeedbackStateHarness(in %clock: i1, in %set_i: i1, in %clear_i: i1, in %oscillate: i1, in %set_bits: i129, in %clear_bits: i129, out q: i1, out wide: i129, out sampled: i1, out unstable: i1, out wire_q: i129, out wire_sample: i129, out wire_unstable: i1) {
    %v:4 = hw.instance "feedback" @NativeFeedbackStateForeign(clock: %clock: i1, set_i: %set_i: i1, clear_i: %clear_i: i1, oscillate: %oscillate: i1, set_bits: %set_bits: i129, clear_bits: %clear_bits: i129) -> (q: i1, wide: i129, sampled: i1, unstable: i1)
    %ones = hw.constant -1 : i129
    %true = hw.constant true
    %set = comb.or %next, %set_bits : i129
    %keep = comb.xor %clear_bits, %ones : i129
    %next = comb.and %set, %keep : i129
    %invert = comb.xor %bad, %true : i1
    %bad = comb.and %invert, %oscillate : i1
    %clk = seq.to_clock %clock
    %sample = seq.compreg %next, %clk : i129
    hw.output %v#0, %v#1, %v#2, %v#3, %next, %sample, %bad : i1, i129, i1, i1, i129, i129, i1
  }
}
