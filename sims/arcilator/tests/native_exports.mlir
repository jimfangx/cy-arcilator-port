module {
  hw.module.extern @ExportNarrow(in %clock:i1,in %we:i1,in %address:i32,in %data:i13,out observed:i13)
  hw.module.extern @ExportWide(in %clock:i1,in %we:i1,in %address:i32,in %data:i65,out observed:i65)
  hw.module @ExportHarness(in %clock:i1,in %we:i1,in %address:i32,in %narrow_data:i13,in %wide_data:i65,out narrow:i13,out wide:i65) {
    %narrow=hw.instance "narrow" @ExportNarrow(clock:%clock:i1,we:%we:i1,address:%address:i32,data:%narrow_data:i13)->(observed:i13)
    %wide=hw.instance "wide" @ExportWide(clock:%clock:i1,we:%we:i1,address:%address:i32,data:%wide_data:i65)->(observed:i65)
    hw.output %narrow,%wide:i13,i65
  }
}
