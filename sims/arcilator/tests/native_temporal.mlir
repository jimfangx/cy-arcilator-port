module {
  hw.module.extern @TemporalRTL(in %clock : i1, in %reset : i1, in %enable : i1, in %a : i1, in %b : i1, in %c : i1, in %data : i8, out io_success : i1)
  hw.module @NativeTemporalHarness(in %clock : i1, in %reset : i1, in %enable : i1, in %a : i1, in %b : i1, in %c : i1, in %data : i8, out io_success : i1) {
    %rtl.io_success = hw.instance "rtl" @TemporalRTL(clock: %clock: i1, reset: %reset: i1, enable: %enable: i1, a: %a: i1, b: %b: i1, c: %c: i1, data: %data: i8) -> (io_success: i1)
    hw.output %rtl.io_success : i1
  }
}

