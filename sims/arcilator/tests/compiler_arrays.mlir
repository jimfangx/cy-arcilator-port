module {
  func.func private @choose(%selector:i2, %source:!hw.array<4xi8>) -> !hw.array<4xi8> {
    cf.switch %selector : i2, [
      default: ^keep(%source : !hw.array<4xi8>),
      1: ^reverse(%source : !hw.array<4xi8>),
      2: ^rotate(%source : !hw.array<4xi8>)
    ]
  ^keep(%value:!hw.array<4xi8>):
    func.return %value : !hw.array<4xi8>
  ^reverse(%r_value:!hw.array<4xi8>):
    %r_c0=hw.constant 0 : i2
    %r_c1=hw.constant 1 : i2
    %r_c2=hw.constant 2 : i2
    %r_c3=hw.constant 3 : i2
    %r_a=hw.array_get %r_value[%r_c0] : !hw.array<4xi8>, i2
    %r_b=hw.array_get %r_value[%r_c1] : !hw.array<4xi8>, i2
    %r_c=hw.array_get %r_value[%r_c2] : !hw.array<4xi8>, i2
    %r_d=hw.array_get %r_value[%r_c3] : !hw.array<4xi8>, i2
    %r_result=hw.array_create %r_a,%r_b,%r_c,%r_d : i8
    func.return %r_result : !hw.array<4xi8>
  ^rotate(%t_value:!hw.array<4xi8>):
    %t_c0=hw.constant 0 : i2
    %t_c1=hw.constant 1 : i2
    %t_c2=hw.constant 2 : i2
    %t_c3=hw.constant 3 : i2
    %t_a=hw.array_get %t_value[%t_c0] : !hw.array<4xi8>, i2
    %t_b=hw.array_get %t_value[%t_c1] : !hw.array<4xi8>, i2
    %t_c=hw.array_get %t_value[%t_c2] : !hw.array<4xi8>, i2
    %t_d=hw.array_get %t_value[%t_c3] : !hw.array<4xi8>, i2
    %t_result=hw.array_create %t_c,%t_b,%t_a,%t_d : i8
    func.return %t_result : !hw.array<4xi8>
  }
  func.func @driver(%selector:i2, %input:i32) -> i32 {
    %source=hw.bitcast %input : (i32) -> !hw.array<4xi8>
    %result=func.call @choose(%selector,%source) : (i2,!hw.array<4xi8>) -> !hw.array<4xi8>
    %packed=hw.bitcast %result : (!hw.array<4xi8>) -> i32
    func.return %packed : i32
  }
  func.func @local_block(%condition:i1,%data:i32) -> i32 {
    %array=hw.bitcast %data : (i32) -> !hw.array<4xi8>
    cf.cond_br %condition, ^first(%array:!hw.array<4xi8>), ^second(%array:!hw.array<4xi8>)
  ^first(%a:!hw.array<4xi8>):
    %x=hw.bitcast %a : (!hw.array<4xi8>) -> i32
    func.return %x : i32
  ^second(%b:!hw.array<4xi8>):
    %y=hw.bitcast %b : (!hw.array<4xi8>) -> i32
    func.return %y : i32
  }

  func.func private @external_array(i32, !hw.array<4xi8>) -> !hw.array<4xi8>
  func.func private @pair(%selector:i2, %input:!hw.array<4xi8>) -> (!hw.array<4xi8>,!hw.array<4xi8>) {
    %changed=func.call @choose(%selector,%input) : (i2,!hw.array<4xi8>) -> !hw.array<4xi8>
    func.return %input,%changed : !hw.array<4xi8>,!hw.array<4xi8>
  }
  func.func @driver_pair(%selector:i2, %input:i32) -> i64 {
    %source=hw.bitcast %input : (i32) -> !hw.array<4xi8>
    %first,%second=func.call @pair(%selector,%source) : (i2,!hw.array<4xi8>) -> (!hw.array<4xi8>,!hw.array<4xi8>)
    %high=hw.bitcast %first : (!hw.array<4xi8>) -> i32
    %low=hw.bitcast %second : (!hw.array<4xi8>) -> i32
    %packed=comb.concat %high,%low : i32,i32
    func.return %packed : i64
  }

  func.func @borrow_and_copy(%borrowed: !arc.arrayref<8xi32>, %input: !hw.array<8xi32>) -> (!arc.arrayref<8xi32>, !hw.array<8xi32>) {
    func.return %borrowed, %input : !arc.arrayref<8xi32>, !hw.array<8xi32>
  }
  func.func private @tagged_row(%input: !hw.array<8xi32>) -> (i32, !hw.array<8xi32>) {
    %tag = hw.constant 42 : i32
    func.return %tag, %input : i32, !hw.array<8xi32>
  }
  func.func @mixed_forward(%seed: i32 {llvm.zeroext}) -> !hw.array<8xi32> {
    %one = hw.constant 1 : i32
    %other = comb.xor %seed, %one : i32
    %input = hw.array_create %other, %seed, %other, %seed, %other, %seed, %other, %seed : i32
    %index = hw.constant 0 : i3
    %changed = hw.array_inject %input[%index], %other : !hw.array<8xi32>, i3
    %tag, %result = func.call @tagged_row(%changed) : (!hw.array<8xi32>) -> (i32, !hw.array<8xi32>)
    func.return %result : !hw.array<8xi32>
  }
  func.func private @nested_choose(%matrix: !hw.array<3xarray<8xi32>>, %row0: !hw.array<8xi32>, %row1: !hw.array<8xi32>, %row2: !hw.array<8xi32>, %choose: i1) -> (!hw.array<3xarray<8xi32>>, !hw.array<8xi32>) {
    cf.cond_br %choose, ^changed, ^original
  ^changed:
    %shuffled = hw.array_create %row0, %row2, %row1 : !hw.array<8xi32>
    func.return %shuffled, %row1 : !hw.array<3xarray<8xi32>>, !hw.array<8xi32>
  ^original:
    func.return %matrix, %row2 : !hw.array<3xarray<8xi32>>, !hw.array<8xi32>
  }
  func.func @nested_driver(%row: i2, %lane: i3, %seed: i32, %choose: i1) -> i64 {

    %left0 = hw.constant 1 : i32
    %right0 = hw.constant 31 : i32
    %a0 = comb.shl %seed, %left0 : i32
    %b0 = comb.shru %seed, %right0 : i32
    %rot0 = comb.or %a0, %b0 : i32
    %k0_0 = hw.constant 136201 : i32
    %v0_0 = comb.xor %rot0, %k0_0 : i32
    %k0_1 = hw.constant 50599953 : i32
    %v0_1 = comb.xor %rot0, %k0_1 : i32
    %k0_2 = hw.constant 33954841 : i32
    %v0_2 = comb.xor %rot0, %k0_2 : i32
    %k0_3 = hw.constant 84410401 : i32
    %v0_3 = comb.xor %rot0, %k0_3 : i32
    %k0_4 = hw.constant 67765289 : i32
    %v0_4 = comb.xor %rot0, %k0_4 : i32
    %k0_5 = hw.constant 118229041 : i32
    %v0_5 = comb.xor %rot0, %k0_5 : i32
    %k0_6 = hw.constant 101583929 : i32
    %v0_6 = comb.xor %rot0, %k0_6 : i32
    %k0_7 = hw.constant 152055873 : i32
    %v0_7 = comb.xor %rot0, %k0_7 : i32
    %row0 = hw.array_create %v0_7, %v0_6, %v0_5, %v0_4, %v0_3, %v0_2, %v0_1, %v0_0 : i32
    %left1 = hw.constant 2 : i32
    %right1 = hw.constant 30 : i32
    %a1 = comb.shl %seed, %left1 : i32
    %b1 = comb.shru %seed, %right1 : i32
    %rot1 = comb.or %a1, %b1 : i32
    %k1_0 = hw.constant 50471946 : i32
    %v1_0 = comb.xor %rot1, %k1_0 : i32
    %k1_1 = hw.constant 272402 : i32
    %v1_1 = comb.xor %rot1, %k1_1 : i32
    %k1_2 = hw.constant 17181722 : i32
    %v1_2 = comb.xor %rot1, %k1_2 : i32
    %k1_3 = hw.constant 101199906 : i32
    %v1_3 = comb.xor %rot1, %k1_3 : i32
    %k1_4 = hw.constant 118109226 : i32
    %v1_4 = comb.xor %rot1, %k1_4 : i32
    %k1_5 = hw.constant 67909682 : i32
    %v1_5 = comb.xor %rot1, %k1_5 : i32
    %k1_6 = hw.constant 84819002 : i32
    %v1_6 = comb.xor %rot1, %k1_6 : i32
    %k1_7 = hw.constant 168820802 : i32
    %v1_7 = comb.xor %rot1, %k1_7 : i32
    %row1 = hw.array_create %v1_7, %v1_6, %v1_5, %v1_4, %v1_3, %v1_2, %v1_1, %v1_0 : i32
    %left2 = hw.constant 3 : i32
    %right2 = hw.constant 29 : i32
    %a2 = comb.shl %seed, %left2 : i32
    %b2 = comb.shru %seed, %right2 : i32
    %rot2 = comb.or %a2, %b2 : i32
    %k2_0 = hw.constant 33698827 : i32
    %v2_0 = comb.xor %rot2, %k2_0 : i32
    %k2_1 = hw.constant 17053715 : i32
    %v2_1 = comb.xor %rot2, %k2_1 : i32
    %k2_2 = hw.constant 408603 : i32
    %v2_2 = comb.xor %rot2, %k2_2 : i32
    %k2_3 = hw.constant 117973027 : i32
    %v2_3 = comb.xor %rot2, %k2_3 : i32
    %k2_4 = hw.constant 101327915 : i32
    %v2_4 = comb.xor %rot2, %k2_4 : i32
    %k2_5 = hw.constant 84682803 : i32
    %v2_5 = comb.xor %rot2, %k2_5 : i32
    %k2_6 = hw.constant 68037691 : i32
    %v2_6 = comb.xor %rot2, %k2_6 : i32
    %k2_7 = hw.constant 185602115 : i32
    %v2_7 = comb.xor %rot2, %k2_7 : i32
    %row2 = hw.array_create %v2_7, %v2_6, %v2_5, %v2_4, %v2_3, %v2_2, %v2_1, %v2_0 : i32
    %matrix = hw.array_create %row2, %row1, %row0 : !hw.array<8xi32>
    %nested, %flat = func.call @nested_choose(%matrix, %row0, %row1, %row2, %choose) : (!hw.array<3xarray<8xi32>>, !hw.array<8xi32>, !hw.array<8xi32>, !hw.array<8xi32>, i1) -> (!hw.array<3xarray<8xi32>>, !hw.array<8xi32>)
    %picked = hw.array_get %nested[%row] : !hw.array<3xarray<8xi32>>, i2
    %low = hw.array_get %picked[%lane] : !hw.array<8xi32>, i3
    %high = hw.array_get %flat[%lane] : !hw.array<8xi32>, i3
    %packed = comb.concat %high, %low : i32, i32
    func.return %packed : i64
  }
}
