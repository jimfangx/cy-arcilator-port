module {
  func.func private @state_partition_low_frontier_event()
  arc.model @graph_low_frontier io !hw.modty<> storageBytes 128 {
  ^bb0(%storage: !arc.storage):
    %a = arc.alloc_state %storage {name = "a", offset = 0 : i32} : (!arc.storage) -> !arc.state<i64>
    %input = arc.alloc_state %storage {name = "input", offset = 8 : i32} : (!arc.storage) -> !arc.state<i64>
    %prefix_out = arc.alloc_state %storage {name = "prefix_out", offset = 16 : i32} : (!arc.storage) -> !arc.state<i64>
    %out0 = arc.alloc_state %storage {name = "out0", offset = 32 : i32} : (!arc.storage) -> !arc.state<i64>
    %out1 = arc.alloc_state %storage {name = "out1", offset = 40 : i32} : (!arc.storage) -> !arc.state<i64>
    %out2 = arc.alloc_state %storage {name = "out2", offset = 48 : i32} : (!arc.storage) -> !arc.state<i64>
    %out3 = arc.alloc_state %storage {name = "out3", offset = 56 : i32} : (!arc.storage) -> !arc.state<i64>
    %out4 = arc.alloc_state %storage {name = "out4", offset = 64 : i32} : (!arc.storage) -> !arc.state<i64>
    %out_sum = arc.alloc_state %storage {name = "out_sum", offset = 72 : i32} : (!arc.storage) -> !arc.state<i64>
    %old = arc.state_read %a : <i64>
    %x = arc.state_read %input : <i64>
    %s0 = comb.add %old, %old : i64
    %r0 = comb.xor %s0, %x : i64
    %s1 = comb.add %r0, %old : i64
    %r1 = comb.xor %s1, %x : i64
    %s2 = comb.add %r1, %old : i64
    %r2 = comb.xor %s2, %x : i64
    %s3 = comb.add %r2, %old : i64
    %r3 = comb.xor %s3, %x : i64
    arc.state_write %prefix_out = %r3 {test.node = "prefix_write"} : <i64>
    %v0 = comb.add %r3, %x {test.node = "frontier0"} : i64
    %v1 = comb.xor %v0, %x {test.node = "frontier1"} : i64
    %v2 = comb.add %v1, %old {test.node = "frontier2"} : i64
    %v3 = comb.xor %v2, %x {test.node = "frontier3"} : i64
    %v4 = comb.add %v3, %x {test.node = "frontier4"} : i64
    func.call @state_partition_low_frontier_event() : () -> ()
    arc.state_write %out0 = %v0 : <i64>
    arc.state_write %out1 = %v1 : <i64>
    arc.state_write %out2 = %v2 : <i64>
    arc.state_write %out3 = %v3 : <i64>
    arc.state_write %out4 = %v4 : <i64>
    %sum = comb.add %v0, %v1, %v2, %v3, %v4 : i64
    arc.state_write %out_sum = %sum : <i64>
  }
}
