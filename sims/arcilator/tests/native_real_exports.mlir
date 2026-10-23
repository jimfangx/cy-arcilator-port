module {
  hw.module.extern @RealExportFirst(in %clock : i1, in %enable : i1, in %input_bits : i64, out observed_real : i64, out observed_short : i32)
  hw.module.extern @RealExportSecond(in %clock : i1, in %enable : i1, in %input_bits : i64, out observed_real : i64, out observed_short : i32)
  hw.module @RealExportHarness(in %clock : i1, in %enable : i1, in %input_bits : i64, out first_real : i64, out first_short : i32, out second_real : i64, out second_short : i32) {
    %first.observed_real, %first.observed_short = hw.instance "first" @RealExportFirst(clock: %clock: i1, enable: %enable: i1, input_bits: %input_bits: i64) -> (observed_real: i64, observed_short: i32)
    %second.observed_real, %second.observed_short = hw.instance "second" @RealExportSecond(clock: %clock: i1, enable: %enable: i1, input_bits: %input_bits: i64) -> (observed_real: i64, observed_short: i32)
    hw.output %first.observed_real, %first.observed_short, %second.observed_real, %second.observed_short : i64, i32, i64, i32
  }
}

