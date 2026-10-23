module {
  hw.module.extern @ReadAscending(in %clock:i1,in %we:i1,in %address:i32,in %data:i13,out observed:i13)
  hw.module.extern @ReadDescending(in %clock:i1,in %we:i1,in %address:i32,in %data:i65,out observed:i65)
  hw.module @ReadMemHarness(in %clock:i1,in %we:i1,in %address:i32,in %narrow_data:i13,in %wide_data:i65,out narrow:i13,out wide:i65) {
    %narrow=hw.instance "ascending" @ReadAscending(clock:%clock:i1,we:%we:i1,address:%address:i32,data:%narrow_data:i13)->(observed:i13)
    %wide=hw.instance "descending" @ReadDescending(clock:%clock:i1,we:%we:i1,address:%address:i32,data:%wide_data:i65)->(observed:i65)
    hw.output %narrow,%wide:i13,i65
  }
}
