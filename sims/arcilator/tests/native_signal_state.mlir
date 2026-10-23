module {
  hw.module.extern private @ArcRTLSignalState(in %clock : !seq.clock, in %reset : i1, in %write : i1, in %data : i129, out q : i129)
  hw.module @NativeSignalStateHarness(in %clock : i1, in %reset : i1, in %write : i1, in %data : i129, out q : i129, out sampled : i129) {
    %clk = seq.to_clock %clock
    %q = hw.instance "state" @ArcRTLSignalState(clock: %clk: !seq.clock, reset: %reset: i1, write: %write: i1, data: %data: i129) -> (q: i129)
    %sampled = seq.firreg %q clock %clk : i129
    hw.output %q, %sampled : i129, i129
  }
}
