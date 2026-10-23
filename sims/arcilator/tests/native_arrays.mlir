module {
  hw.module @NativeArrayHarness(in %clock:i1, in %reset:i1, in %data:i16, in %index:i2, in %data40:i40,
                               out selected:i16, out state:i64, out expanded:i64) {
    %clk = seq.to_clock %clock
    %zero = hw.constant 0:i16
    %one = hw.constant 1:i16
    %two = hw.constant 2:i16
    %three = hw.constant 3:i16
    %i0 = hw.constant 0:i2
    %i1 = hw.constant 1:i2
    %i2 = hw.constant 2:i2
    %head = hw.array_create %data:i16
    %v0 = hw.array_get %q[%i0]:!hw.array<4xi16>, i2
    %v1 = hw.array_get %q[%i1]:!hw.array<4xi16>, i2
    %v2 = hw.array_get %q[%i2]:!hw.array<4xi16>, i2
    %tail = hw.array_create %v2, %v1, %v0:i16
    %next = hw.array_concat %head, %tail:!hw.array<1xi16>, !hw.array<3xi16>
    %initial = hw.array_create %three, %two, %one, %zero:i16
    %q = seq.firreg %next clock %clk reset sync %reset, %initial:!hw.array<4xi16>
    %selected = hw.array_get %q[%index]:!hw.array<4xi16>, i2
    %state = hw.bitcast %q:(!hw.array<4xi16>) -> i64
    %data_array = hw.bitcast %data40:(i40) -> !hw.array<40xi1>
    %reset40 = hw.constant 0xABCDE00001:i40
    %reset_array = hw.bitcast %reset40:(i40) -> !hw.array<40xi1>
    %q40 = seq.firreg %data_array clock %clk reset sync %reset, %reset_array:!hw.array<40xi1>
    %index0 = hw.constant 0:i6
    %low_bit = hw.array_get %q40[%index0]:!hw.array<40xi1>, i6
    %repeated = hw.array_create %low_bit, %low_bit, %low_bit, %low_bit, %low_bit, %low_bit, %low_bit, %low_bit, %low_bit, %low_bit, %low_bit, %low_bit, %low_bit, %low_bit, %low_bit, %low_bit, %low_bit, %low_bit, %low_bit, %low_bit, %low_bit, %low_bit, %low_bit, %low_bit:i1
    %extended = hw.array_concat %repeated, %q40:!hw.array<24xi1>, !hw.array<40xi1>
    %expanded = hw.bitcast %extended:(!hw.array<64xi1>) -> i64
    hw.output %selected, %state, %expanded:i16, i64, i64
  }
}
