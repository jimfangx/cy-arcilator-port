module {
  hw.module @ShiftHarness(in %clock:i1, in %reset:i1, in %data:i2051, in %amount:i2051,
                         out left:i2051, out right:i2051, out signed:i2051, out latched:i2051) {
    %left = comb.shl %data, %amount:i2051
    %right = comb.shru %data, %amount:i2051
    %signed = comb.shrs %data, %amount:i2051
    %clk = seq.to_clock %clock
    %zero = hw.constant 0:i2051
    %latched = seq.firreg %left clock %clk reset sync %reset, %zero:i2051
    hw.output %left, %right, %signed, %latched:i2051, i2051, i2051, i2051
  }
}
