module {
  hw.module.extern @GlobalWriter<TAG: i32>(in %clock: i1, in %enable: i1, in %data: i13, in %index: i65, in %address: i2, out q: i13)
  hw.module.extern @GlobalReader(out shadow: i13, out wide: i65, out m0: i13, out m1: i13, out m2: i13)
  hw.module @GlobalHarness(in %clock_a: i1, in %clock_b: i1, in %enable_a: i1, in %enable_b: i1, in %data_a: i13, in %data_b: i13, in %index_a: i65, in %index_b: i65, in %address_a: i2, in %address_b: i2, out qa: i13, out qb: i13, out shadow: i13, out wide: i65, out m0: i13, out m1: i13, out m2: i13) {
    %qa = hw.instance "a" @GlobalWriter<TAG: i32 = 3>(clock: %clock_a: i1, enable: %enable_a: i1, data: %data_a: i13, index: %index_a: i65, address: %address_a: i2) -> (q: i13)
    %qb = hw.instance "b" @GlobalWriter<TAG: i32 = 9>(clock: %clock_b: i1, enable: %enable_b: i1, data: %data_b: i13, index: %index_b: i65, address: %address_b: i2) -> (q: i13)
    %shadow, %wide, %m0, %m1, %m2 = hw.instance "reader" @GlobalReader() -> (shadow: i13, wide: i65, m0: i13, m1: i13, m2: i13)
    hw.output %qa, %qb, %shadow, %wide, %m0, %m1, %m2 : i13, i13, i13, i65, i13, i13, i13
  }
}
