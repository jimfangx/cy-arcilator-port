module {
  hw.module.extern private @ArcRTLTasks(in %clock: !seq.clock, in %reset: i1, in %enable: i1, in %data: i8, out q: i16, out counter: i16)
  hw.module @NativeTasksHarness(in %clock: i1, in %reset: i1, in %enable: i1, in %data: i8, out q: i16, out counter: i16) {
    %clk = seq.to_clock %clock
    %q, %counter = hw.instance "tasks" @ArcRTLTasks(clock: %clk: !seq.clock, reset: %reset: i1, enable: %enable: i1, data: %data: i8) -> (q: i16, counter: i16)
    hw.output %q, %counter : i16, i16
  }
}
