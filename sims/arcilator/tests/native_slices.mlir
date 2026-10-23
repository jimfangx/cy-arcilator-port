module {
  hw.module.extern private @ArcRTLSlices(in %clock_a : !seq.clock, in %clock_b : !seq.clock, in %reset : i1, in %enable : i1, in %data : i8, in %index : i2, out bits_q : i8, out lanes_q : i36, out fields_q : i14, out selected : i9, out prefix_q : i16)
  hw.module @NativeSlicesHarness(in %clock : i1, in %clock_b : i1, in %reset : i1, in %enable : i1, in %data : i8, in %index : i2, out bits_q : i8, out lanes_q : i36, out fields_q : i14, out selected : i9, out prefix_q : i16) {
    %clk_a = seq.to_clock %clock
    %clk_b = seq.to_clock %clock_b
    %bits, %lanes, %fields, %selected, %prefix = hw.instance "slices" @ArcRTLSlices(clock_a: %clk_a: !seq.clock, clock_b: %clk_b: !seq.clock, reset: %reset: i1, enable: %enable: i1, data: %data: i8, index: %index: i2) -> (bits_q: i8, lanes_q: i36, fields_q: i14, selected: i9, prefix_q: i16)
    hw.output %bits, %lanes, %fields, %selected, %prefix : i8, i36, i14, i9, i16
  }
}
