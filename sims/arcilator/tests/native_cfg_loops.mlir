module {
  hw.module.extern private @ArcRTLCFGLoops<XLEN: ui32>(in %select: i3, in %q: i128, in %data: i64, out d: i128)
  hw.module @NativeCFGLoopsHarness(in %clock: i1, in %select: i3, in %q: i128, in %data: i64, out d32: i128, out d64: i128, out sampled32: i128, out sampled64: i128) {
    %clk = seq.to_clock %clock
    %d32 = hw.instance "rv32" @ArcRTLCFGLoops<XLEN: ui32 = 32>(select: %select: i3, q: %q: i128, data: %data: i64) -> (d: i128)
    %d64 = hw.instance "rv64" @ArcRTLCFGLoops<XLEN: ui32 = 64>(select: %select: i3, q: %q: i128, data: %data: i64) -> (d: i128)
    %s32 = seq.firreg %d32 clock %clk : i128
    %s64 = seq.firreg %d64 clock %clk : i128
    hw.output %d32, %d64, %s32, %s64 : i128, i128, i128, i128
  }
}
