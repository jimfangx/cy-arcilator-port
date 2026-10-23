module {
  hw.module.extern @NativeLatchesForeign(in %clock: i1, in %peer_clock: i1, in %enable: i1, in %test_enable: i1, in %open: i1, in %nba_open: i1, in %data: i129, out q: i129, out nba_q: i129, out aggregate_q: i129, out sampled: i129, out sampled_nba: i129, out count: i32, out gated_clock: i1)
  hw.module @NativeLatchesHarness(in %clock: i1, in %peer_clock: i1, in %enable: i1, in %test_enable: i1, in %open: i1, in %nba_open: i1, in %data: i129, out q: i129, out nba_q: i129, out aggregate_q: i129, out sampled: i129, out sampled_nba: i129, out count: i32, out gated_clock: i1) {
    %v:7 = hw.instance "latches" @NativeLatchesForeign(clock: %clock: i1, peer_clock: %peer_clock: i1, enable: %enable: i1, test_enable: %test_enable: i1, open: %open: i1, nba_open: %nba_open: i1, data: %data: i129) -> (q: i129, nba_q: i129, aggregate_q: i129, sampled: i129, sampled_nba: i129, count: i32, gated_clock: i1)
    hw.output %v#0, %v#1, %v#2, %v#3, %v#4, %v#5, %v#6 : i129, i129, i129, i129, i129, i32, i1
  }
}
