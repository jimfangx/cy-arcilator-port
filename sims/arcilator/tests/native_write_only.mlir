module {
  hw.module.extern @WriteOnly(in %clock:i1,in %peer:i1,in %enable:i1,in %peer_enable:i1,in %data:i65,out narrow:i13,out wide:i65)
  hw.module @WriteOnlyHarness(in %clock:i1,in %peer:i1,in %enable:i1,in %peer_enable:i1,in %data:i65,out narrow:i13,out wide:i65) {
  %a, %b = hw.instance "dut" @WriteOnly(clock:%clock:i1,peer:%peer:i1,enable:%enable:i1,peer_enable:%peer_enable:i1,data:%data:i65)->(narrow:i13,wide:i65)
  hw.output %a, %b : i13, i65
 }
}
