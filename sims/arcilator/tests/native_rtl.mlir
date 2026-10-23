module {
  hw.module.extern private @ArcRTLState<INIT: ui8>(in %clock : !seq.clock, in %reset : i1, in %enable : i1, in %data : i8, out q : i8)
  hw.module.extern private @ArcRTLClock(in %source : !seq.clock, out clock_out : !seq.clock)
  hw.module @NativeRTLHarness(in %clock : i1, in %reset : i1, in %enable : i1, in %data : i8, out first : i8, out second : i8) {
    %clk = seq.to_clock %clock
    %pass = hw.instance "clock" @ArcRTLClock(source: %clk: !seq.clock) -> (clock_out: !seq.clock)
    %a = hw.instance "first" @ArcRTLState<INIT: ui8 = 165>(clock: %pass: !seq.clock, reset: %reset: i1, enable: %enable: i1, data: %data: i8) -> (q: i8)
    %b = hw.instance "second" @ArcRTLState<INIT: ui8 = 60>(clock: %pass: !seq.clock, reset: %reset: i1, enable: %enable: i1, data: %data: i8) -> (q: i8)
    hw.output %a, %b : i8, i8
  }
}
