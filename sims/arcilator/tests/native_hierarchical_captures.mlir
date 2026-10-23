module {
  hw.module.extern @CapturedFixture(in %clock : !seq.clock, in %write : i1, in %address : i3, in %data : i13, in %index : i2, out first_value : i13, out second_value : i13, out first_function : i13, out second_function : i13)
  hw.module.extern @CapturedDuplicateFixture(in %clock : !seq.clock, in %write : i1, in %address : i3, in %data : i13, in %index : i2, out first_value : i13, out second_value : i13, out first_function : i13, out second_function : i13)
  hw.module @CapturedHarness(in %clock : !seq.clock, in %fixture : i1, in %write : i1, in %address : i3, in %data : i13, in %index : i2, out different_first_value : i13, out different_second_value : i13, out different_first_function : i13, out different_second_function : i13, out same_first_value : i13, out same_second_value : i13, out same_first_function : i13, out same_second_function : i13) {
    %true = hw.constant true
    %0 = comb.xor %fixture, %true : i1
    %1 = comb.and %write, %0 : i1
    %2 = comb.and %write, %fixture : i1
    %different.first_value, %different.second_value, %different.first_function, %different.second_function = hw.instance "different" @CapturedFixture(clock: %clock: !seq.clock, write: %1: i1, address: %address: i3, data: %data: i13, index: %index: i2) -> (first_value: i13, second_value: i13, first_function: i13, second_function: i13)
    %same.first_value, %same.second_value, %same.first_function, %same.second_function = hw.instance "same" @CapturedDuplicateFixture(clock: %clock: !seq.clock, write: %2: i1, address: %address: i3, data: %data: i13, index: %index: i2) -> (first_value: i13, second_value: i13, first_function: i13, second_function: i13)
    hw.output %different.first_value, %different.second_value, %different.first_function, %different.second_function, %same.first_value, %same.second_value, %same.first_function, %same.second_function : i13, i13, i13, i13, i13, i13, i13, i13
  }
}

