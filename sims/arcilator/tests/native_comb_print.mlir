module {
  hw.module.extern @PrintRTL(in %clock: i1, in %reset: i1, in %enable: i1, in %literal_enable: i1, in %data: i8, out result: i8, out q: i8, out io_success: i1)
  hw.module @NativePrintHarness(in %clock: i1, in %reset: i1, in %enable: i1, in %literal_enable: i1, in %data: i8, out result: i8, out q: i8, out io_success: i1) {
    %r,%q,%done=hw.instance "rtl" @PrintRTL(clock: %clock: i1, reset: %reset: i1, enable: %enable: i1, literal_enable: %literal_enable: i1, data: %data: i8) -> (result: i8, q: i8, io_success: i1)
    hw.output %r,%q,%done : i8,i8,i1
  }
}
