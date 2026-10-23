module {
  hw.module @EnabledStateHarness(in %clock: i1, in %reset: i1, in %enable: i1, in %data: i17, out array_state: i34, out struct_state: i34) {
    %clk = seq.to_clock %clock
    %mask = hw.constant 0x12345 : i17
    %other = comb.xor %data, %mask : i17
    %input_array = hw.array_create %other, %data : i17
    %input_struct = hw.struct_create (%data, %other) : !hw.struct<hi: i17, lo: i17>
    %three = hw.constant 3 : i17
    %five = hw.constant 5 : i17
    %eleven = hw.constant 11 : i17
    %thirteen = hw.constant 13 : i17
    %reset_array = hw.array_create %five, %three : i17
    %reset_struct = hw.struct_create (%thirteen, %eleven) : !hw.struct<hi: i17, lo: i17>
    %sample_array = comb.mux %reset, %reset_array, %input_array : !hw.array<2xi17>
    %sample_struct = comb.mux %reset, %reset_struct, %input_struct : !hw.struct<hi: i17, lo: i17>
    %next_array = comb.mux %enable, %sample_array, %array : !hw.array<2xi17>
    %next_struct = comb.mux %enable, %sample_struct, %struct : !hw.struct<hi: i17, lo: i17>
    %array = seq.firreg %next_array clock %clk : !hw.array<2xi17>
    %struct = seq.firreg %next_struct clock %clk : !hw.struct<hi: i17, lo: i17>
    %array_bits = hw.bitcast %array : (!hw.array<2xi17>) -> i34
    %struct_bits = hw.bitcast %struct : (!hw.struct<hi: i17, lo: i17>) -> i34
    hw.output %array_bits, %struct_bits : i34, i34
  }
}
