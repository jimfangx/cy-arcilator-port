module {
  hw.module.extern @UnitStatic(in %clock:i1,in %enable:i1,out observed:i32)
  hw.module @UnitHarness(in %clock:i1,in %enable:i1,out observed:i32){
  %v=hw.instance "unit" @UnitStatic(clock:%clock:i1,enable:%enable:i1)->(observed:i32)
  hw.output %v:i32
 }
}
