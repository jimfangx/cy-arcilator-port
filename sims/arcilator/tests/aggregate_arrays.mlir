!Entry = !hw.struct<tag: i5, lanes: !hw.array<3xi13>, nested: !hw.struct<lo: i7, hi: i32>>
!Table = !hw.array<3x!Entry>
module {
  func.func private @table(%zero: i1) -> !Table {
    cf.cond_br %zero, ^empty, ^data
  ^empty:
    %z = hw.aggregate_constant [[0 : i5, [0 : i13, 0 : i13, 0 : i13], [0 : i7, 0 : i32]], [0 : i5, [0 : i13, 0 : i13, 0 : i13], [0 : i7, 0 : i32]], [0 : i5, [0 : i13, 0 : i13, 0 : i13], [0 : i7, 0 : i32]]] : !Table
    return %z : !Table
  ^data:
    %a = hw.aggregate_constant [[17 : i5, [701 : i13, 0 : i13, 8191 : i13], [91 : i7, 305419896 : i32]], [3 : i5, [4097 : i13, 8190 : i13, 13 : i13], [127 : i7, 3735928559 : i32]], [31 : i5, [1 : i13, 777 : i13, 4096 : i13], [5 : i7, 2147483649 : i32]]] : !Table
    return %a : !Table
  }
  func.func private @pack(%e: !Entry, %lane: i2) -> i64 {
    %tag = hw.struct_extract %e["tag"] : !Entry
    %lanes = hw.struct_extract %e["lanes"] : !Entry
    %value = hw.array_get %lanes[%lane] : !hw.array<3xi13>, i2
    %nested = hw.struct_extract %e["nested"] : !Entry
    %lo = hw.struct_extract %nested["lo"] : !hw.struct<lo: i7, hi: i32>
    %hi = hw.struct_extract %nested["hi"] : !hw.struct<lo: i7, hi: i32>
    %pad = hw.constant 0 : i7
    %packed = comb.concat %pad, %tag, %value, %lo, %hi : i7, i5, i13, i7, i32
    return %packed : i64
  }
  func.func @aggregate_lookup(%row: i2, %lane: i2, %zero: i1, %seed: i32) -> i64 {
    %a = func.call @table(%zero) : (i1) -> !Table
    %original = hw.array_get %a[%row] : !Table, i2
    %result = "arc.execute"(%a, %row, %lane, %seed) ({
    ^entry(%xs: !Table, %r: i2, %l: i2, %s: i32):
      %item = hw.array_get %xs[%r] : !Table, i2
      %nested = hw.struct_extract %item["nested"] : !Entry
      %old = hw.struct_extract %nested["hi"] : !hw.struct<lo: i7, hi: i32>
      %hi = comb.xor %old, %s : i32
      %changed = hw.struct_inject %nested["hi"], %hi : !hw.struct<lo: i7, hi: i32>
      %new_item = hw.struct_inject %item["nested"], %changed : !Entry
      %ys = hw.array_inject %xs[%r], %new_item : !Table, i2
      cf.br ^exit(%ys : !Table)
    ^exit(%answer: !Table):
      %got = hw.array_get %answer[%r] : !Table, i2
      %value = func.call @pack(%got, %l) : (!Entry, i2) -> i64
      "arc.output"(%value) : (i64) -> ()
    }) : (!Table, i2, i2, i32) -> i64
    // A later read of the borrowed original must survive array injection.
    %again = hw.array_get %a[%row] : !Table, i2
    %before = func.call @pack(%original, %lane) : (!Entry, i2) -> i64
    %after = func.call @pack(%again, %lane) : (!Entry, i2) -> i64
    %unchanged = comb.icmp eq %before, %after : i64
    %bad = hw.constant 18446744073709551615 : i64
    %checked = comb.mux %unchanged, %result, %bad : i64
    return %checked : i64
  }
  func.func @fp_table(%index: i3) -> i64 {
    %table = hw.aggregate_constant [[8 : i32, 23 : i32], [11 : i32, 52 : i32], [5 : i32, 10 : i32], [5 : i32, 2 : i32], [8 : i32, 7 : i32]] : !hw.array<5xstruct<exp_bits: i32, man_bits: i32>>
    %entry = hw.array_get %table[%index] : !hw.array<5xstruct<exp_bits: i32, man_bits: i32>>, i3
    %exp = hw.struct_extract %entry["exp_bits"] : !hw.struct<exp_bits: i32, man_bits: i32>
    %man = hw.struct_extract %entry["man_bits"] : !hw.struct<exp_bits: i32, man_bits: i32>
    %value = comb.concat %exp, %man : i32, i32
    return %value : i64
  }
}
