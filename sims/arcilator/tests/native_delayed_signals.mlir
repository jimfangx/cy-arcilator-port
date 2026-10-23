module {
  hw.module.extern @DelayedSignalsTop(in %clock : !seq.clock, in %value : i13, in %wide65 : i65, in %wide193 : i193, out q13 : i13, out qfast : i13, out qchain : i13, out q65 : i65, out q193 : i193, out constant_value : i13, out delayed_clock : i1, out count : i32)
  hw.module @DelayedSignalsHarness(in %clock : !seq.clock, in %value : i13, in %wide65 : i65, in %wide193 : i193, out q13 : i13, out qfast : i13, out qchain : i13, out q65 : i65, out q193 : i193, out constant_value : i13, out delayed_clock : i1, out count : i32) {
    %dut.q13, %dut.qfast, %dut.qchain, %dut.q65, %dut.q193, %dut.constant_value, %dut.delayed_clock, %dut.count = hw.instance "dut" @DelayedSignalsTop(clock: %clock: !seq.clock, value: %value: i13, wide65: %wide65: i65, wide193: %wide193: i193) -> (q13: i13, qfast: i13, qchain: i13, q65: i65, q193: i193, constant_value: i13, delayed_clock: i1, count: i32)
    hw.output %dut.q13, %dut.qfast, %dut.qchain, %dut.q65, %dut.q193, %dut.constant_value, %dut.delayed_clock, %dut.count : i13, i13, i13, i65, i193, i13, i1, i32
  }
}

