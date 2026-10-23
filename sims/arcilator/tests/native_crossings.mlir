module {
  hw.module.extern private @ArcRTLReturning<MASK: ui8>(in %clock : !seq.clock, in %reset : i1, in %data : i8, out q : i8)
  hw.module private @SimTSI(in %clock : !seq.clock, in %reset : i1, in %data : i8, out q : i8, out pair : !hw.struct<hi: i8, lo: i8>) {
    %three = hw.constant 3 : i8
    %five = hw.constant 5 : i8
    %next = comb.add %data, %three : i8
    %q = seq.firreg %next clock %clock reset sync %reset, %five : i8
    %mask = hw.constant 255 : i8
    %complement = comb.xor %q, %mask : i8
    %pair = hw.struct_create (%complement, %q) : !hw.struct<hi: i8, lo: i8>
    hw.output %q, %pair : i8, !hw.struct<hi: i8, lo: i8>
  }
  hw.module @NativeCrossingsHarness(in %clock : i1, in %reset : i1, in %data : i8, out first : i8, out second : i8, out sampled : i8) {
    %clk = seq.to_clock %clock
    %mask = hw.constant 128 : i8
    %other = comb.xor %data, %mask : i8
    %first = hw.instance "first" @ArcRTLReturning<MASK: ui8 = 90>(clock: %clk: !seq.clock, reset: %reset: i1, data: %data: i8) -> (q: i8)
    %second = hw.instance "second" @ArcRTLReturning<MASK: ui8 = 60>(clock: %clk: !seq.clock, reset: %reset: i1, data: %other: i8) -> (q: i8)
    %sampled = seq.firreg %first clock %clk : i8
    hw.output %first, %second, %sampled : i8, i8, i8
  }
}
