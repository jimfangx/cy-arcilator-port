module {
  hw.module.extern @Multi2A(in %clock : i1, out observed : i1)
  hw.module.extern @Multi2B(in %clock : i1, out observed : i1)
  hw.module.extern @Multi3A(in %clock : i1, out observed : i1)
  hw.module.extern @Multi3B(in %clock : i1, out observed : i1)
  hw.module @ReadMemMultiHarness(in %clock : i1, out observed : i4) {
    %a.observed = hw.instance "a" @Multi2A(clock: %clock: i1) -> (observed: i1)
    %b.observed = hw.instance "b" @Multi2B(clock: %clock: i1) -> (observed: i1)
    %c.observed = hw.instance "c" @Multi3A(clock: %clock: i1) -> (observed: i1)
    %d.observed = hw.instance "d" @Multi3B(clock: %clock: i1) -> (observed: i1)
    %0 = comb.concat %a.observed, %b.observed, %c.observed, %d.observed : i1, i1, i1, i1
    hw.output %0 : i4
  }
}

