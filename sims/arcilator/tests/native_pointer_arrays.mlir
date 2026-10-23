module {
  hw.module.extern @PointerImport(in %clock:i1,in %reset:i1,in %enable:i1,in %seed:i32,out observed:i32)
  hw.module @PointerHarness(in %clock:i1,in %reset:i1,in %enable:i1,in %seed:i32,out observed:i32) {
   %o=hw.instance "rtl" @PointerImport(clock:%clock:i1,reset:%reset:i1,enable:%enable:i1,seed:%seed:i32)->(observed:i32)
   hw.output %o:i32
  }
}
