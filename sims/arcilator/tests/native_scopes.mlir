module {
  hw.module.extern @ArcScopeProbe(in %clock: !seq.clock, in %data: i8, out q: i8)
  hw.module @NativeScopesHarness(in %clock: i1, in %reset: i1, in %data: i8, out left: i8, out right: i8, out io_success: i1) {
    %false = hw.constant false
    %clk = seq.to_clock %clock
    %left = hw.instance "left" @ArcScopeProbe(clock: %clk: !seq.clock, data: %data: i8) -> (q: i8)
    %right = hw.instance "nested/right" @ArcScopeProbe(clock: %clk: !seq.clock, data: %data: i8) -> (q: i8)
    hw.output %left, %right, %false : i8, i8, i1
  }
}
