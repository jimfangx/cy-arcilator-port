module {
  hw.module @NativeArrayFeedbackHarness(in %clock: i1, in %reset: i1, in %data: i6, in %fallback: i8, in %choose: i1, in %choose_second: i1, out first: i6, out second: i6, out q: i6, out io_success: i1) {
    %i0 = hw.constant 0 : i2
    %i1 = hw.constant 1 : i2
    %i2 = hw.constant 2 : i2
    %false = hw.constant false
    %zero = hw.constant 0 : i6
    %a = comb.extract %data from 0 : (i6) -> i2
    %b = comb.extract %data from 2 : (i6) -> i2
    %c = comb.extract %data from 4 : (i6) -> i2
    %fallback_array = hw.bitcast %fallback : (i8) -> !hw.array<4xi2>
    // Lane 3 is never assigned or observed. Lanes 0/1/2 have no feedback.
    %root = comb.mux %choose, %fallback_array, %filled : !hw.array<4xi2>
    %fill0 = hw.array_inject %root[%i0], %a : !hw.array<4xi2>, i2
    %fill1 = hw.array_inject %fill0[%i1], %b : !hw.array<4xi2>, i2
    %filled = hw.array_inject %fill1[%i2], %c : !hw.array<4xi2>, i2
    %first_a = hw.array_get %root[%i0] : !hw.array<4xi2>, i2
    %first_b = hw.array_get %root[%i1] : !hw.array<4xi2>, i2
    %first_c = hw.array_get %root[%i2] : !hw.array<4xi2>, i2
    %first = comb.concat %first_c, %first_b, %first_a : i2, i2, i2
    %left_a = comb.xor %a, %c : i2
    %left_b = comb.xor %b, %a : i2
    %left_c = comb.xor %c, %b : i2
    %right_a = comb.add %a, %b : i2
    %right_b = comb.add %b, %c : i2
    %right_c = comb.add %c, %a : i2
    %other_root = comb.mux %choose_second, %left, %right : !hw.array<4xi2>
    %left0 = hw.array_inject %other_root[%i2], %left_c : !hw.array<4xi2>, i2
    %left1 = hw.array_inject %left0[%i0], %left_a : !hw.array<4xi2>, i2
    %left = hw.array_inject %left1[%i1], %left_b : !hw.array<4xi2>, i2
    %right0 = hw.array_inject %other_root[%i1], %right_b : !hw.array<4xi2>, i2
    %right1 = hw.array_inject %right0[%i2], %right_c : !hw.array<4xi2>, i2
    %right = hw.array_inject %right1[%i0], %right_a : !hw.array<4xi2>, i2
    %second_a = hw.array_get %other_root[%i0] : !hw.array<4xi2>, i2
    %second_b = hw.array_get %other_root[%i1] : !hw.array<4xi2>, i2
    %second_c = hw.array_get %other_root[%i2] : !hw.array<4xi2>, i2
    %second = comb.concat %second_c, %second_b, %second_a : i2, i2, i2
    %d = comb.xor %first, %second : i6
    %clk = seq.to_clock %clock
    %q = seq.firreg %d clock %clk reset sync %reset, %zero : i6
    hw.output %first, %second, %q, %false : i6, i6, i6, i1
  }
}
