module {
  hw.module @TimeOutputHarness(in %clock : i1, in %reset : i1, out io_success : i1, out stamp : i64) {
    %context = arc.inferred_context
    %now = arc.current_time %context
    %limit = hw.constant 780000000 : i64
    %done = comb.icmp uge %now, %limit : i64
    hw.output %done, %now : i1, i64
  }
}
