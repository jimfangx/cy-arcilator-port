module {
  hw.module.extern @FormatStringsTop(in %clock : i1, in %reset : i1, in %enable : i1, in %value : i32, out first : i32, out second : i32)
  hw.module @FormatStringsHarness(in %clock : i1, in %reset : i1, in %enable : i1, in %value : i32, out first : i32, out second : i32) {
    %original.first, %original.second = hw.instance "original" @FormatStringsTop(clock: %clock: i1, reset: %reset: i1, enable: %enable: i1, value: %value: i32) -> (first: i32, second: i32)
    hw.output %original.first, %original.second : i32, i32
  }
}
