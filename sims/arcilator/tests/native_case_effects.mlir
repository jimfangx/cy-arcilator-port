module {
  hw.module.extern @CaseEffects(in %clock: i1, in %value: i8, out calls: i32, out result: i8)
  hw.module @CaseEffectsHarness(in %clock: i1, in %value: i8, out calls: i32, out result: i8) {
    %v:2 = hw.instance "cases" @CaseEffects(clock: %clock: i1, value: %value: i8) -> (calls: i32, result: i8)
    hw.output %v#0, %v#1 : i32, i8
  }
}
