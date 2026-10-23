module {
  hw.module.extern private @ForeignDelay(in %d : i1, out q : i1)
  hw.module @DelayHarness(in %clock : i1, out q : i1) {
    %q = hw.instance "delay" @ForeignDelay(d: %clock: i1) -> (q: i1)
    hw.output %q : i1
  }
}
