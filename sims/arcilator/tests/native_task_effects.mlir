module {
  hw.module.extern @TaskEffects(in %clock:i1,in %reset:i1,in %enable:i1,in %halt:i1,in %data:i8,out observed:i8,out startup:i32)
  hw.module @TestHarness(in %clock:i1,in %reset:i1,in %enable:i1,in %halt:i1,in %data:i8,out observed:i8,out startup:i32) {
    %a:2=hw.instance "original" @TaskEffects(clock:%clock:i1,reset:%reset:i1,enable:%enable:i1,halt:%halt:i1,data:%data:i8)->(observed:i8,startup:i32)
    hw.output %a#0,%a#1:i8,i32
  }
}
