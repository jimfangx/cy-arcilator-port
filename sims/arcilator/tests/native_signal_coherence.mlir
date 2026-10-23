module {
  hw.module.extern private @ArcRTLSignalCoherence(in %clock: !seq.clock, in %reset: i1, in %step: i44, in %low: i8, in %selector: i2, in %payload: i17, out counter: i44, out floating: i257, out bundle: i57, out seeded: i129, out partial: i17, out merged: i51)
  hw.module @NativeSignalCoherenceHarness(in %clock: i1, in %reset: i1, in %step: i44, in %low: i8, in %selector: i2, in %payload: i17, out counter: i44, out sampled: i44, out floating: i257, out bundle: i57, out seeded: i129, out partial: i17, out merged: i51) {
    %clk = seq.to_clock %clock
    %counter, %floating, %bundle, %seeded, %partial, %merged = hw.instance "coherence" @ArcRTLSignalCoherence(clock: %clk: !seq.clock, reset: %reset: i1, step: %step: i44, low: %low: i8, selector: %selector: i2, payload: %payload: i17) -> (counter: i44, floating: i257, bundle: i57, seeded: i129, partial: i17, merged: i51)
    %sampled = seq.firreg %counter clock %clk : i44
    hw.output %counter, %sampled, %floating, %bundle, %seeded, %partial, %merged : i44, i44, i257, i57, i129, i17, i51
  }
}
