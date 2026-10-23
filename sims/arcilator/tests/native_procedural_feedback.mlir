module {
  hw.module.extern @FeedbackRTL(in %clock: i1, in %reset: i1, in %enable: i1, in %oscillate: i1, in %data: i8, in %mask: i8, out first: i8, out second: i8, out q: i8, out io_success: i1)
  hw.module @NativeLoopHarness(in %clock: i1, in %reset: i1, in %enable: i1, in %oscillate: i1, in %data: i8, in %mask: i8, out first: i8, out second: i8, out q: i8, out io_success: i1) {
    %a,%b,%q,%done=hw.instance "rtl" @FeedbackRTL(clock: %clock: i1, reset: %reset: i1, enable: %enable: i1, oscillate: %oscillate: i1, data: %data: i8, mask: %mask: i8) -> (first: i8, second: i8, q: i8, io_success: i1)
    hw.output %a,%b,%q,%done : i8,i8,i8,i1
  }
}
