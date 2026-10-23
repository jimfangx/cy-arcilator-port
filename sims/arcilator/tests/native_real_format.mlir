module {
  hw.module.extern private @ArcRTLRealFormat(in %clock: !seq.clock, in %reset: i1, in %enable: i1, in %bits: i64, out count: i8)
  hw.module @NativeRealFormatHarness(in %clock: i1, in %reset: i1, in %enable: i1, in %bits: i64, out count: i8) {
    %clk = seq.to_clock %clock
    %count = hw.instance "formats" @ArcRTLRealFormat(clock: %clk: !seq.clock, reset: %reset: i1, enable: %enable: i1, bits: %bits: i64) -> (count: i8)
    hw.output %count : i8
  }
}
