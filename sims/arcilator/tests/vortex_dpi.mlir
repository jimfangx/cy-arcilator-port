module {
  sim.func.dpi private @imul(in %enable: i1, in %signed_a: i1, in %signed_b: i1, in %a: iWORD, in %b: iWORD, out low: iWORD, out high: iWORD) attributes {verilogName = "dpi_imul"}
  sim.func.dpi private @idiv(in %enable: i1, in %signed_a: i1, in %a: iWORD, in %b: iWORD, out quotient: iWORD, out remainder: iWORD) attributes {verilogName = "dpi_idiv"}
  sim.func.dpi private @uuid(in %reset: i1, in %wid: i32, in %pc: i64, return result: i64) attributes {verilogName = "dpi_uuid_gen"}
  hw.module @VortexDPIHarness(in %clock: i1, in %reset: i1, in %enable: i1, in %signed_a: i1, in %signed_b: i1, in %a: iWORD, in %b: iWORD, in %wid: i32, in %pc: i64, out low: iWORD, out high: iWORD, out quotient: iWORD, out remainder: iWORD, out uuid: i44) {
    %clk = seq.to_clock %clock
    %product:2 = sim.func.dpi.call @imul(%enable, %signed_a, %signed_b, %a, %b) clock %clk enable %enable : (i1, i1, i1, iWORD, iWORD) -> (iWORD, iWORD)
    %division:2 = sim.func.dpi.call @idiv(%enable, %signed_a, %a, %b) clock %clk enable %enable : (i1, i1, iWORD, iWORD) -> (iWORD, iWORD)
    %uuid_enable = comb.or %reset, %enable : i1
    %full_uuid = sim.func.dpi.call @uuid(%reset, %wid, %pc) clock %clk enable %uuid_enable : (i1, i32, i64) -> i64
    %uuid = comb.extract %full_uuid from 0 : (i64) -> i44
    hw.output %product#0, %product#1, %division#0, %division#1, %uuid : iWORD, iWORD, iWORD, iWORD, i44
  }
}
