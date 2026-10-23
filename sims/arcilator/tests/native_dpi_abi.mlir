module {
  hw.module.extern @NativeDPIABIForeign(in %clock: i1, in %enable: i1, in %data: i32, in %fp32: i32, in %fp64: i64, in %wide: i129, out atom: i32, out atom_bit: i1, out atom_logic: i1, out vector_bits: i129, out vector_logic: i65, out accumulator: i33, out clocked: i33, out real_bits: i64, out shortreal_bits: i32, out clocked_real_bits: i64, out clocked_shortreal_bits: i32)
  hw.module @NativeDPIABIHarness(in %clock: i1, in %enable: i1, in %data: i32, in %fp32: i32, in %fp64: i64, in %wide: i129, out atom: i32, out atom_bit: i1, out atom_logic: i1, out vector_bits: i129, out vector_logic: i65, out accumulator: i33, out clocked: i33, out real_bits: i64, out shortreal_bits: i32, out clocked_real_bits: i64, out clocked_shortreal_bits: i32) {
    %v:11 = hw.instance "dpi" @NativeDPIABIForeign(clock: %clock: i1, enable: %enable: i1, data: %data: i32, fp32: %fp32: i32, fp64: %fp64: i64, wide: %wide: i129) -> (atom: i32, atom_bit: i1, atom_logic: i1, vector_bits: i129, vector_logic: i65, accumulator: i33, clocked: i33, real_bits: i64, shortreal_bits: i32, clocked_real_bits: i64, clocked_shortreal_bits: i32)
    hw.output %v#0, %v#1, %v#2, %v#3, %v#4, %v#5, %v#6, %v#7, %v#8, %v#9, %v#10 : i32, i1, i1, i129, i65, i33, i33, i64, i32, i64, i32
  }
}
