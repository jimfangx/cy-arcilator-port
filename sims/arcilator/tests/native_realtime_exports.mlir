module {
  hw.module.extern @RealtimeNano(in %clock : i1, in %enable : i1, in %input_bits : i64, out observed : i64)
  hw.module.extern @RealtimePico(in %clock : i1, in %enable : i1, in %input_bits : i64, out observed : i64)
  hw.module @RealtimeHarness(in %clock : i1, in %enable : i1, in %input_bits : i64, out first_value : i64, out second_value : i64) {
    %first.observed = hw.instance "first" @RealtimeNano(clock: %clock: i1, enable: %enable: i1, input_bits: %input_bits: i64) -> (observed: i64)
    %second.observed = hw.instance "second" @RealtimePico(clock: %clock: i1, enable: %enable: i1, input_bits: %input_bits: i64) -> (observed: i64)
    hw.output %first.observed, %second.observed : i64, i64
  }
}

