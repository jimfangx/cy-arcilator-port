module {
  hw.module.extern @HandleDPI(in %clock:i1,in %reset:i1,in %enable:i1,in %key:i8,out values:i32,out flags:i32)
  hw.module @HandlesHarness(in %clock:i1,in %reset:i1,in %enable:i1,in %key:i8,out values:i32,out flags:i32) {
    %a,%b = hw.instance "dut" @HandleDPI(clock:%clock:i1,reset:%reset:i1,enable:%enable:i1,key:%key:i8)->(values:i32,flags:i32)
    hw.output %a,%b : i32,i32
  }
}
