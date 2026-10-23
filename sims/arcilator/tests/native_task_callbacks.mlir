module {
  hw.module.extern @TaskCallbacks(in %clock : i1, in %reset : i1, in %enable : i1, out first : i32, out second : i32)
  hw.module @TaskCallbacksHarness(in %clock : i1, in %reset : i1, in %enable : i1, out first : i32, out second : i32) {
    %original.first, %original.second = hw.instance "original" @TaskCallbacks(clock: %clock: i1, reset: %reset: i1, enable: %enable: i1) -> (first: i32, second: i32)
    hw.output %original.first, %original.second : i32, i32
  }
}

