module {
  hw.module.extern @StringImports(in %clock:i1,in %reset:i1,in %enable:i1,in %key:i8,out lengths:i32,out firsts:i32,out old_length:i32)
  hw.module @StringImportsHarness(in %clock:i1,in %reset:i1,in %enable:i1,in %key:i8,out lengths:i32,out firsts:i32,out old_length:i32) {
    %a,%b,%c = hw.instance "dut" @StringImports(clock:%clock:i1,reset:%reset:i1,enable:%enable:i1,key:%key:i8)->(lengths:i32,firsts:i32,old_length:i32)
    hw.output %a,%b,%c : i32,i32,i32
  }
}
