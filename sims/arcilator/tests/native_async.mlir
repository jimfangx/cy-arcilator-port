module {
  hw.module @NativeAsyncHarness(in %clock : !seq.clock, in %reset : i1, out io_success : i1, out wide : i2051, out count : i32, out array_wide : i2051, out struct_wide : i2051) {
    %false = hw.constant false
    %one_bit = hw.constant true
    %padding = hw.constant 0 : i2018
    %low = hw.constant 5 : i32
    %reset_value = comb.concat %one_bit, %padding, %low : i1, i2018, i32
    %upper = hw.constant 0 : i1025
    %lower = hw.constant 3 : i1025
    %step = comb.concat %upper, %one_bit, %lower : i1025, i1, i1025
    %wide = seq.firreg %next clock %clock reset async %reset, %reset_value : i2051
    %next = comb.add %wide, %step : i2051
    %zero = hw.constant 0 : i32
    %one = hw.constant 1 : i32
    %count = seq.firreg %count_next clock %clock reset sync %reset, %zero : i32
    %count_next = comb.add %count, %one : i32
    %array_reset = hw.bitcast %reset_value : (i2051) -> !hw.array<7xi293>
    %array_q = seq.firreg %array_next clock %clock reset async %reset, %array_reset : !hw.array<7xi293>
    %array_bits = hw.bitcast %array_q : (!hw.array<7xi293>) -> i2051
    %array_added = comb.add %array_bits, %step : i2051
    %array_next = hw.bitcast %array_added : (i2051) -> !hw.array<7xi293>
    %struct_reset = hw.bitcast %reset_value : (i2051) -> !hw.struct<"lo:field": i1024, hi: !hw.array<1xi1027>>
    %struct_q = seq.firreg %struct_next clock %clock reset async %reset, %struct_reset : !hw.struct<"lo:field": i1024, hi: !hw.array<1xi1027>>
    %struct_bits = hw.bitcast %struct_q : (!hw.struct<"lo:field": i1024, hi: !hw.array<1xi1027>>) -> i2051
    %struct_added = comb.add %struct_bits, %step : i2051
    %struct_next = hw.bitcast %struct_added : (i2051) -> !hw.struct<"lo:field": i1024, hi: !hw.array<1xi1027>>
    hw.output %false, %wide, %count, %array_bits, %struct_bits : i1, i2051, i32, i2051, i2051
  }
}
