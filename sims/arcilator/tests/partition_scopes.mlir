module {
 hw.module @ScopeHarness(in %clock:i1, in %reset:i1, out io_success:i1) {
  %false=hw.constant false
  %true=hw.constant true
  llhd.process {
   cf.br ^wait
  ^wait:
   llhd.wait (%clock:i1), ^event(%clock:i1)
  ^event(%0:i1):
   %1=sim.fmt.dec %0 specifierWidth 0 : i1
   sim.proc.print %1
   cf.br ^wait
  }
  llhd.process {
   cf.br ^wait
  ^wait:
   llhd.wait (%clock:i1), ^event(%clock:i1)
  ^event(%0:i1):
   %1=comb.xor %0,%true : i1
   %2=sim.fmt.dec %1 specifierWidth 0 : i1
   sim.proc.print %2
   cf.br ^wait
  }
  hw.output %false : i1
 }
}
