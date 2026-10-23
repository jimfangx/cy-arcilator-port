module {
  hw.module.extern private @ArcRTLInitial8<SIZE: ui32, BIAS: ui32, ADDRW: ui32>(in %clock : !seq.clock, in %reset : i1, in %enable : i1, in %waddr : i3, in %raddr : i3, in %data : i13, out selected : i13, out scalar : i8) attributes {verilogName = "ArcRTLInitial"}
  hw.module.extern private @ArcRTLInitial<SIZE: ui32, BIAS: ui32, ADDRW: ui32>(in %clock : !seq.clock, in %reset : i1, in %enable : i1, in %waddr : i5, in %raddr : i5, in %data : i13, out selected : i13, out scalar : i8)
  hw.module @NativeInitialHarness(in %clock : i1, in %clock_b : i1, in %reset : i1, in %enable : i1, in %waddr : i5, in %raddr : i5, in %data : i13, out first : i13, out second : i13, out scalar_first : i8, out scalar_second : i8) {
    %clk_a = seq.to_clock %clock
    %clk_b = seq.to_clock %clock_b
    %wa = comb.extract %waddr from 0 : (i5) -> i3
    %ra = comb.extract %raddr from 0 : (i5) -> i3
    %first, %sfirst = hw.instance "first" @ArcRTLInitial8<SIZE: ui32 = 8, BIAS: ui32 = 3, ADDRW: ui32 = 3>(clock: %clk_a: !seq.clock, reset: %reset: i1, enable: %enable: i1, waddr: %wa: i3, raddr: %ra: i3, data: %data: i13) -> (selected: i13, scalar: i8)
    %second, %ssecond = hw.instance "second" @ArcRTLInitial<SIZE: ui32 = 17, BIAS: ui32 = 6161, ADDRW: ui32 = 5>(clock: %clk_b: !seq.clock, reset: %reset: i1, enable: %enable: i1, waddr: %waddr: i5, raddr: %raddr: i5, data: %data: i13) -> (selected: i13, scalar: i8)
    hw.output %first, %second, %sfirst, %ssecond : i13, i13, i8, i8
  }
}
