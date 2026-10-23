module {
  hw.module.extern @OriginalSramNarrow(in %clock:i1,in %reset_n:i1,in %req:i1,in %we:i1,in %address:i4,in %data:i13,in %mask:i2,out observed:i13)
  hw.module.extern @OriginalSramWide(in %clock:i1,in %reset_n:i1,in %req:i1,in %we:i1,in %address:i4,in %data:i65,in %mask:i9,out observed:i65)
  hw.module @OriginalSramHarness(in %clock:i1,in %reset_n:i1,in %req:i1,in %we:i1,in %address:i4,in %narrow_data:i13,in %wide_data:i65,in %narrow_mask:i2,in %wide_mask:i9,out narrow:i13,out wide:i65) {
    %narrow=hw.instance "narrow" @OriginalSramNarrow(clock:%clock:i1,reset_n:%reset_n:i1,req:%req:i1,we:%we:i1,address:%address:i4,data:%narrow_data:i13,mask:%narrow_mask:i2)->(observed:i13)
    %wide=hw.instance "wide" @OriginalSramWide(clock:%clock:i1,reset_n:%reset_n:i1,req:%req:i1,we:%we:i1,address:%address:i4,data:%wide_data:i65,mask:%wide_mask:i9)->(observed:i65)
    hw.output %narrow,%wide:i13,i65
  }
}
