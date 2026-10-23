module {
  hw.module.extern private @ArcRTLCombDrives(in %clock : !seq.clock, in %reset : i1, in %valid : i1, in %enabled : i4, in %flags : i4, in %selectors : i12, out mask : i10, out sfu : i4, out any : i2, out captured : i16)
  hw.module @NativeCombDrivesHarness(in %clock : i1, in %reset : i1, in %valid : i1, in %enabled : i4, in %flags : i4, in %selectors : i12, out mask : i10, out sfu : i4, out any : i2, out captured : i16) {
    %clk = seq.to_clock %clock
    %mask, %sfu, %any, %captured = hw.instance "comb" @ArcRTLCombDrives(clock: %clk: !seq.clock, reset: %reset: i1, valid: %valid: i1, enabled: %enabled: i4, flags: %flags: i4, selectors: %selectors: i12) -> (mask: i10, sfu: i4, any: i2, captured: i16)
    hw.output %mask, %sfu, %any, %captured : i10, i4, i2, i16
  }
}
