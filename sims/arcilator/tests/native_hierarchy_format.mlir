module {
  hw.module.extern @HierarchyFormat<BIAS: i32>(in %clock:i1,in %reset:i1,in %enable:i1,in %data:i8,out observed:i8)
  hw.module @TestHarness(in %clock:i1,in %reset:i1,in %enable:i1,in %data:i8,out first:i8,out second:i8,out third:i8) {
    %a=hw.instance "first" @HierarchyFormat<BIAS: i32=3>(clock:%clock:i1,reset:%reset:i1,enable:%enable:i1,data:%data:i8)->(observed:i8)
    %b=hw.instance "second" @HierarchyFormat<BIAS: i32=3>(clock:%clock:i1,reset:%reset:i1,enable:%enable:i1,data:%data:i8)->(observed:i8)
    %c=hw.instance "third" @HierarchyFormat<BIAS: i32=7>(clock:%clock:i1,reset:%reset:i1,enable:%enable:i1,data:%data:i8)->(observed:i8)
    hw.output %a,%b,%c:i8,i8,i8
  }
}
