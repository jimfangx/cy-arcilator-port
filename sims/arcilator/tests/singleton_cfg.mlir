module {
  func.func @singleton_cfg(%empty:i0 {chipyard.slot="discard"},%input:i64 {chipyard.slot="data"},%limit:i8 {chipyard.slot="limit"},%mode:i1 {chipyard.slot="mode"})->i64 {
    %zero0=hw.constant 0:i0
    %zero1=hw.constant false
    %zero51=hw.constant 0:i51
    %zero56=hw.constant 0:i56
    %one8=hw.constant 1:i8
    %step=hw.constant 17:i13
    %initial=hw.aggregate_constant [[[37:i13]],0:i8]:!hw.struct<payload:!hw.array<1xarray<1xi13>>,count:i8>
    %payload=hw.struct_extract %initial["payload"]:!hw.struct<payload:!hw.array<1xarray<1xi13>>,count:i8>
    %a=hw.array_get %payload[%zero0]:!hw.array<1xarray<1xi13>>,i0
    %constant=hw.array_get %a[%zero1]:!hw.array<1xi13>,i1
    %low=comb.extract %input from 0:(i64)->i13
    %value=comb.xor %low,%constant:i13
    %single=hw.array_create %value:i13
    %nested=hw.array_create %single:!hw.array<1xi13>
    %count=hw.struct_extract %initial["count"]:!hw.struct<payload:!hw.array<1xarray<1xi13>>,count:i8>
    %frame=hw.struct_create (%nested,%count):!hw.struct<payload:!hw.array<1xarray<1xi13>>,count:i8>
    %write_index=comb.extract %input from 63:(i64)->i1
    cf.br ^loop(%frame,%empty:!hw.struct<payload:!hw.array<1xarray<1xi13>>,count:i8>,i0)
  ^loop(%state:!hw.struct<payload:!hw.array<1xarray<1xi13>>,count:i8>,%none:i0):
    %n=hw.struct_extract %state["count"]:!hw.struct<payload:!hw.array<1xarray<1xi13>>,count:i8>
    %more=comb.icmp ult %n,%limit:i8
    cf.cond_br %more,^body(%state,%none:!hw.struct<payload:!hw.array<1xarray<1xi13>>,count:i8>,i0),^done(%state:!hw.struct<payload:!hw.array<1xarray<1xi13>>,count:i8>)
  ^body(%old:!hw.struct<payload:!hw.array<1xarray<1xi13>>,count:i8>,%unused:i0):
    %selector=comb.extract %limit from 0:(i8)->i2
    %extra1=hw.constant 1:i13
    %extra3=hw.constant 3:i13
    %extra7=hw.constant 7:i13
    cf.switch %selector:i2,[
      default:^calculate(%old,%unused,%extra7:!hw.struct<payload:!hw.array<1xarray<1xi13>>,count:i8>,i0,i13),
      0:^case0(%old,%unused:!hw.struct<payload:!hw.array<1xarray<1xi13>>,count:i8>,i0),
      1:^case1(%old,%unused:!hw.struct<payload:!hw.array<1xarray<1xi13>>,count:i8>,i0)
    ]
  ^case0(%a0:!hw.struct<payload:!hw.array<1xarray<1xi13>>,count:i8>,%e0:i0):
    cf.br ^calculate(%a0,%e0,%extra1:!hw.struct<payload:!hw.array<1xarray<1xi13>>,count:i8>,i0,i13)
  ^case1(%a1:!hw.struct<payload:!hw.array<1xarray<1xi13>>,count:i8>,%e1:i0):
    cf.br ^calculate(%a1,%e1,%extra3:!hw.struct<payload:!hw.array<1xarray<1xi13>>,count:i8>,i0,i13)
  ^calculate(%active:!hw.struct<payload:!hw.array<1xarray<1xi13>>,count:i8>,%empty_forwarded:i0,%extra:i13):
    %old_nested=hw.struct_extract %active["payload"]:!hw.struct<payload:!hw.array<1xarray<1xi13>>,count:i8>
    %old_count=hw.struct_extract %active["count"]:!hw.struct<payload:!hw.array<1xarray<1xi13>>,count:i8>
    %old_single=hw.array_get %old_nested[%mode]:!hw.array<1xarray<1xi13>>,i1
    %old_word=hw.array_get %old_single[%zero0]:!hw.array<1xi13>,i0
    %count13=comb.concat %zero1,%zero1,%zero1,%zero1,%zero1,%old_count:i1,i1,i1,i1,i1,i8
    %next_word=comb.add %old_word,%step,%count13,%extra:i13
    %new_single=hw.array_create %next_word:i13
    %new_nested=hw.array_inject %old_nested[%write_index],%new_single:!hw.array<1xarray<1xi13>>,i1
    %next_count=comb.add %old_count,%one8:i8
    %updated=hw.struct_create (%new_nested,%next_count):!hw.struct<payload:!hw.array<1xarray<1xi13>>,count:i8>
    cf.br ^loop(%updated,%empty_forwarded:!hw.struct<payload:!hw.array<1xarray<1xi13>>,count:i8>,i0)
  ^done(%final:!hw.struct<payload:!hw.array<1xarray<1xi13>>,count:i8>):
    %final_array=hw.struct_extract %final["payload"]:!hw.struct<payload:!hw.array<1xarray<1xi13>>,count:i8>
    %final_single=hw.array_get %final_array[%zero0]:!hw.array<1xarray<1xi13>>,i0
    %final_word=hw.array_get %final_single[%zero1]:!hw.array<1xi13>,i1
    %final_count=hw.struct_extract %final["count"]:!hw.struct<payload:!hw.array<1xarray<1xi13>>,count:i8>
    %low64=comb.concat %zero51,%final_word:i51,i13
    %count64=comb.concat %zero56,%final_count:i56,i8
    %amount=hw.constant 16:i64
    %shift=comb.shl %count64,%amount:i64
    %result=comb.or %low64,%shift:i64
    return %result:i64
  }
}
