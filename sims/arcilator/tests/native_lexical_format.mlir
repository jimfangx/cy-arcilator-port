module {
  hw.module.extern @LexicalFormat<BIAS: i32>(in %clock:i1,in %reset:i1,in %enable:i1,in %data:i8,out observed:i8,out startup:i32)
  hw.module @TestHarness(in %clock:i1,in %reset:i1,in %enable:i1,in %data:i8,out first:i8,out first_start:i32,out second:i8,out second_start:i32,out third:i8,out third_start:i32) {
    %a:2=hw.instance "first" @LexicalFormat<BIAS: i32=3>(clock:%clock:i1,reset:%reset:i1,enable:%enable:i1,data:%data:i8)->(observed:i8,startup:i32)
    %b:2=hw.instance "second" @LexicalFormat<BIAS: i32=3>(clock:%clock:i1,reset:%reset:i1,enable:%enable:i1,data:%data:i8)->(observed:i8,startup:i32)
    %c:2=hw.instance "third" @LexicalFormat<BIAS: i32=7>(clock:%clock:i1,reset:%reset:i1,enable:%enable:i1,data:%data:i8)->(observed:i8,startup:i32)
    hw.output %a#0,%a#1,%b#0,%b#1,%c#0,%c#1:i8,i32,i8,i32,i8,i32
  }
}
