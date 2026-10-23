!pair = !hw.struct<upper: i13, lower: !hw.array<4xi13>>
!u = !hw.union<raw: i65, parts: !pair>
!array = !hw.array<2x!u>
!offset = !hw.union<whole: i65, lane: i17 offset 41>
!empty = !hw.union<empty: i0>
!record = !hw.struct<items: !array, tag: i17>
module {
  hw.module.extern @UnionExtern(in %data: !u, out copy: !u)
  func.func private @choose(%a: !u, %b: !u, %select: i1) -> !u {
    cf.cond_br %select, ^left, ^right
  ^left:
    cf.br ^join(%a : !u)
  ^right:
    cf.br ^join(%b : !u)
  ^join(%value: !u):
    func.return %value : !u
  }
  hw.module @UnionHarness(in %a: !u, in %b: i65, in %select: i1, in %tag: i17,
      out view: i65, out back: i65, out selected: i65, out array_a: i65, out array_b: i65, out tag_out: i17, out shifted: i65, out lane: i17, out empty: i0) {
    %zero = hw.constant 0 : i1
    %one = hw.constant 1 : i1
    %parts = hw.union_extract %a["parts"] : !u
    %view = hw.bitcast %parts : (!pair) -> i65
    %bparts = hw.bitcast %b : (i65) -> !pair
    %ub = hw.union_create "parts", %bparts : !u
    %back = hw.union_extract %ub["raw"] : !u
    %selected = llhd.combinational -> !u {
      %result = func.call @choose(%a, %ub, %select) : (!u, !u, i1) -> !u
      llhd.yield %result : !u
    }
    %selected_bits = hw.union_extract %selected["raw"] : !u
    %array = hw.array_create %ub, %a : !u
    %record = hw.struct_create (%array, %tag) : !record
    %items = hw.struct_extract %record["items"] : !record
    %item_a = hw.array_get %items[%zero] : !array, i1
    %item_b = hw.array_get %items[%one] : !array, i1
    %a_bits = hw.union_extract %item_a["raw"] : !u
    %b_bits = hw.union_extract %item_b["raw"] : !u
    %out_tag = hw.struct_extract %record["tag"] : !record
    %tag_union = hw.union_create "lane", %tag : !offset
    %shifted = hw.union_extract %tag_union["whole"] : !offset
    %raw_a = hw.union_extract %a["raw"] : !u
    %raw_union = hw.union_create "whole", %raw_a : !offset
    %lane = hw.union_extract %raw_union["lane"] : !offset
    %empty_bits = hw.constant 0 : i0
    %empty_union = hw.union_create "empty", %empty_bits : !empty
    %empty = hw.union_extract %empty_union["empty"] : !empty
    hw.output %view, %back, %selected_bits, %a_bits, %b_bits, %out_tag, %shifted, %lane, %empty : i65, i65, i65, i65, i65, i17, i65, i17, i0
  }
}
