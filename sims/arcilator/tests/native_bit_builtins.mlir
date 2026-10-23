module {
  hw.module.extern @BitBuiltins(in %value: i129, out flags: i64, out constants: i32)
  hw.module @BitBuiltinsHarness(in %value: i129, out flags: i64, out constants: i32) {
    %v:2 = hw.instance "builtins" @BitBuiltins(value: %value: i129) -> (flags: i64, constants: i32)
    hw.output %v#0, %v#1 : i64, i32
  }
}
