module {
  hw.module.extern @StaticScopes(in %clock: i1, in %reset: i1, in %enable_a: i1, in %enable_b: i1, in %latch_enable: i1, in %data_a: i13, in %data_b: i13, in %latch_data: i13, out a0: i13, out a1: i13, out b0: i13, out b1: i13, out as0: i13, out as1: i13, out bs0: i13, out bs1: i13, out latched: i13, out fresh_seen: i13)
  hw.module @StaticScopesHarness(in %clock: i1, in %reset: i1, in %enable_a: i1, in %enable_b: i1, in %latch_enable: i1, in %data_a: i13, in %data_b: i13, in %latch_data: i13, out a0: i13, out a1: i13, out b0: i13, out b1: i13, out as0: i13, out as1: i13, out bs0: i13, out bs1: i13, out latched: i13, out fresh_seen: i13) {
    %a0, %a1, %b0, %b1, %as0, %as1, %bs0, %bs1, %latched, %fresh_seen = hw.instance "dut" @StaticScopes(clock: %clock: i1, reset: %reset: i1, enable_a: %enable_a: i1, enable_b: %enable_b: i1, latch_enable: %latch_enable: i1, data_a: %data_a: i13, data_b: %data_b: i13, latch_data: %latch_data: i13) -> (a0: i13, a1: i13, b0: i13, b1: i13, as0: i13, as1: i13, bs0: i13, bs1: i13, latched: i13, fresh_seen: i13)
    hw.output %a0, %a1, %b0, %b1, %as0, %as1, %bs0, %bs1, %latched, %fresh_seen : i13, i13, i13, i13, i13, i13, i13, i13, i13, i13
  }
}
