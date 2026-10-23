module {
  hw.module.extern @TaskFinish(in %clock : i1, in %reset : i1, out preflights : i32, out resumed : i32)
  hw.module @TaskFinishHarness(in %clock : i1, in %reset : i1, out preflights : i32, out resumed : i32, out io_success : i1) {
    %original.preflights, %original.resumed = hw.instance "original" @TaskFinish(clock: %clock: i1, reset: %reset: i1) -> (preflights: i32, resumed: i32)
    %false = hw.constant false
    hw.output %original.preflights, %original.resumed, %false : i32, i32, i1
  }
}
