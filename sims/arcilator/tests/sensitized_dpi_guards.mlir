module {
  sim.func.dpi private @record(in %data: i32, return result: i32)
  hw.module @Guards(in %clock: i1, in %data: i32, out pure: i32, out clocked: i32, out reference: i32, out nested_clocked: i32) {
    %clk = seq.to_clock %clock
    %pure = llhd.combinational -> i32 {
      %value = comb.add %data, %data : i32
      llhd.yield %value : i32
    }
    %clocked = sim.func.dpi.call @record(%data) clock %clk : (i32) -> i32
    %signal = llhd.sig name "unresolved_reference" : <i32>
    %reference = llhd.combinational -> i32 {
      %input = llhd.prb %signal : i32
      %value = sim.func.dpi.call @record(%input) enable %clock : (i32) -> i32
      llhd.yield %value : i32
    }
    %nested_clocked = llhd.combinational -> i32 {
      %value = sim.func.dpi.call @record(%data) clock %clk : (i32) -> i32
      llhd.yield %value : i32
    }
    llhd.process {
      cf.br ^sleep
    ^sleep:
      llhd.wait (%clock: i1), ^call
    ^call:
      %value = sim.func.dpi.call @record(%data) : (i32) -> i32
      cf.br ^sleep
    }
    hw.output %pure, %clocked, %reference, %nested_clocked : i32, i32, i32, i32
  }
  llhd.coroutine @timed(%input: i32) {
    llhd.wait (%input: i32), ^call
  ^call:
    %value = sim.func.dpi.call @record(%input) : (i32) -> i32
    llhd.return
  }
}
