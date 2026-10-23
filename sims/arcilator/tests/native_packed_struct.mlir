module {
  hw.module.extern private @ArcRTLPackedSelect(in %clock: !seq.clock, in %reset: i1, in %enable: i1, in %mode: i2, in %index: i65, in %offset: i65, in %data: i17, out observed: i84, out before_q: i84, out after_q: i84)
  hw.module @PackedSelectHarness(in %clock: i1, in %reset: i1, in %enable: i1, in %mode: i2, in %index: i65, in %offset: i65, in %data: i17, out q: i84, out before: i84, out after: i84) {
    %clk = seq.to_clock %clock
    %q, %before, %after = hw.instance "record" @ArcRTLPackedSelect(clock: %clk: !seq.clock, reset: %reset: i1, enable: %enable: i1, mode: %mode: i2, index: %index: i65, offset: %offset: i65, data: %data: i17) -> (observed: i84, before_q: i84, after_q: i84)
    hw.output %q, %before, %after : i84, i84, i84
  }
}
