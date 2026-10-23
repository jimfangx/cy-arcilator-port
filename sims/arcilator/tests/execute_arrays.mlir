module {
  func.func private @execute_callback(i32, i32) -> i32
  func.func @execute_test(%seed: i32, %flags: i8) -> (i32, !hw.array<4xi32>) {
    %zero = hw.constant 0 : i2
    %k17 = hw.constant 17 : i32
    %k34 = hw.constant 34 : i32
    %k51 = hw.constant 51 : i32
    %a1 = comb.add %seed, %k17 : i32
    %a2 = comb.add %seed, %k34 : i32
    %a3 = comb.add %seed, %k51 : i32
    %array = hw.array_create %a3, %a2, %a1, %seed : i32
    %result:2 = "arc.execute"(%array, %seed, %flags) ({
    ^entry(%a: !hw.array<4xi32>, %s: i32, %f: i8):
      %z = hw.constant 0 : i32
      %one = hw.constant 1 : i32
      %four = hw.constant 4 : i32
      %early = comb.extract %f from 0 : (i8) -> i1
      cf.br ^loop(%z, %a, %s : i32, !hw.array<4xi32>, i32)
    ^loop(%i: i32, %current: !hw.array<4xi32>, %sum: i32):
      %idx = comb.extract %i from 0 : (i32) -> i2
      %inner:2 = "arc.execute"(%current, %idx, %s, %sum) ({
      ^inside(%xs: !hw.array<4xi32>, %lane: i2, %seed_arg: i32, %total: i32):
        %old = hw.array_get %xs[%lane] : !hw.array<4xi32>, i2
        %value = func.call @execute_callback(%old, %seed_arg) : (i32, i32) -> i32
        %next = hw.array_inject %xs[%lane], %value : !hw.array<4xi32>, i2
        %added = comb.add %total, %value : i32
        %odd = comb.extract %seed_arg from 0 : (i32) -> i1
        cf.cond_br %odd, ^odd_return, ^even_return
      ^odd_return:
        %magic = hw.constant 2779096485 : i32
        %mixed = comb.xor %added, %magic : i32
        "arc.output"(%next, %mixed) : (!hw.array<4xi32>, i32) -> ()
      ^even_return:
        "arc.output"(%next, %added) : (!hw.array<4xi32>, i32) -> ()
      }) : (!hw.array<4xi32>, i2, i32, i32) -> (!hw.array<4xi32>, i32)
      %second = comb.icmp eq %i, %one : i32
      %stop = comb.and %early, %second : i1
      cf.cond_br %stop, ^short_exit, ^continue
    ^continue:
      %j = comb.add %i, %one : i32
      %more = comb.icmp ult %j, %four : i32
      cf.cond_br %more, ^loop(%j, %inner#0, %inner#1 : i32, !hw.array<4xi32>, i32), ^full_exit
    ^short_exit:
      "arc.output"(%inner#0, %inner#1) : (!hw.array<4xi32>, i32) -> ()
    ^full_exit:
      "arc.output"(%inner#0, %inner#1) : (!hw.array<4xi32>, i32) -> ()
    }) : (!hw.array<4xi32>, i32, i8) -> (!hw.array<4xi32>, i32)
    %original = hw.array_get %array[%zero] : !hw.array<4xi32>, i2
    %checksum = comb.xor %result#1, %original : i32
    return %checksum, %result#0 : i32, !hw.array<4xi32>
  }
}
