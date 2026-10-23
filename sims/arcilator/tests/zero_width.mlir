module {
  func.func @concat_empty(%a: i5, %b: i9, %flag: i1) -> i32 {
    %empty = hw.constant 0 : i0
    %pad = hw.constant 0 : i17
    %left = comb.concat %empty, %a : i0, i5
    %right = comb.concat %b, %empty : i9, i0
    %middle = comb.concat %empty, %right, %empty, %left, %flag, %empty : i0, i9, i0, i5, i1, i0
    %result = comb.concat %pad, %middle : i17, i15
    return %result : i32
  }
  func.func @concat_all_empty(%a: i32) -> (i32, i0) {
    %empty = hw.constant 0 : i0
    %result = comb.concat %empty, %empty : i0, i0
    return %a, %result : i32, i0
  }
}
