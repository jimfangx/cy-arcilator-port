module {
  hw.module.extern @ContextTop(in %clock:i1,in %reset:i1,in %enable:i1,in %side:i32,out observed:i32)
  hw.module @ContextHarness(in %clock:i1,in %reset:i1,in %enable:i1,out left:i32,out right:i32) {
    %c0=hw.constant 0:i32
    %c1=hw.constant 1:i32
    %a=hw.instance "left" @ContextTop(clock:%clock:i1,reset:%reset:i1,enable:%enable:i1,side:%c0:i32)->(observed:i32)
    %b=hw.instance "right" @ContextTop(clock:%clock:i1,reset:%reset:i1,enable:%enable:i1,side:%c1:i32)->(observed:i32)
    hw.output %a,%b:i32,i32
  }
}
