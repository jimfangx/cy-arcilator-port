module {
  hw.module.extern @ArrayExportFirst(in %clock:i1,in %enable:i1,in %seed:i32,out observed:i32)
  hw.module.extern @ArrayExportSecond(in %clock:i1,in %enable:i1,in %seed:i32,out observed:i32)
  hw.module @ArrayExportHarness(in %clock:i1,in %enable:i1,in %seed:i32,out first:i32,out second:i32) {
    %first=hw.instance "first" @ArrayExportFirst(clock:%clock:i1,enable:%enable:i1,seed:%seed:i32)->(observed:i32)
    %second=hw.instance "second" @ArrayExportSecond(clock:%clock:i1,enable:%enable:i1,seed:%seed:i32)->(observed:i32)
    hw.output %first,%second:i32,i32
  }
}
