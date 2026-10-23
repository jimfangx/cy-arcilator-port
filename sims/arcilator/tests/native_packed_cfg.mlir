// Reference descriptors cross duplicate branch edges and loop backedges.
module {
  sim.func.dpi private @record(in %before: i32, in %after: i32) attributes {verilogName="cfg_memory_record"}
  hw.module @CFGMemoryHarness(in %clock: i1, in %select: i1, in %address: i3, in %count: i3, in %low: i4, in %data: i13, in %enable: i1, in %partial_enable: i1, in %advance_ref: i1, out a: i65, out b: i65, out peer_a: i65, out peer_b: i65, out sensitivity: i13) {
    %zero0 = hw.constant 0 : i0
    %zero3 = hw.constant 0 : i3
    %one3 = hw.constant 1 : i3
    %zero13 = hw.constant 0 : i13
    %immediate = llhd.constant_time <0ns, 0d, 1e>
    %nba = llhd.constant_time <0ns, 1d, 0e>
    %a0 = hw.aggregate_constant [5 : i13, 4 : i13, 3 : i13, 2 : i13, 1 : i13] : !hw.struct<a4: i13, a3: i13, a2: i13, a1: i13, a0: i13>
    %b0 = hw.aggregate_constant [15 : i13, 14 : i13, 13 : i13, 12 : i13, 11 : i13] : !hw.struct<a4: i13, a3: i13, a2: i13, a1: i13, a0: i13>
    %mem_a = llhd.sig %a0 : <!hw.struct<a4: i13, a3: i13, a2: i13, a1: i13, a0: i13>>
    %mem_b = llhd.sig %b0 : <!hw.struct<a4: i13, a3: i13, a2: i13, a1: i13, a0: i13>>
    %zero64 = hw.constant 0 : i64
    %pad61 = hw.constant 0 : i61
    %word13 = hw.constant 13 : i64
    %clk = seq.to_clock %clock
    llhd.process {
      cf.br ^wait
    ^wait:
      llhd.wait (%clock : i1), ^event
    ^event:
      cf.cond_br %clock, ^choose, ^wait
    ^choose:
      %ar = "builtin.unrealized_conversion_cast"(%mem_a, %zero64) {chipyard.packed_ref_slice} : (!llhd.ref<!hw.struct<a4: i13, a3: i13, a2: i13, a1: i13, a0: i13>>, i64) -> !llhd.ref<i65>
      %br = "builtin.unrealized_conversion_cast"(%mem_b, %zero64) {chipyard.packed_ref_slice} : (!llhd.ref<!hw.struct<a4: i13, a3: i13, a2: i13, a1: i13, a0: i13>>, i64) -> !llhd.ref<i65>
      cf.cond_br %select, ^join(%br : !llhd.ref<i65>), ^join(%ar : !llhd.ref<i65>)
    ^join(%selected: !llhd.ref<i65>):
      cf.br ^loop(%selected, %address, %count, %zero13, %zero13 : !llhd.ref<i65>, i3, i3, i13, i13)
    ^loop(%carried: !llhd.ref<i65>, %index: i3, %remaining: i3, %sum_before: i13, %sum_after: i13):
      %done = comb.icmp eq %remaining, %zero3 : i3
      cf.cond_br %done, ^exit, ^body
    ^body:
      %index64 = comb.concat %pad61, %index : i61, i3
      %bitindex = comb.mul %index64, %word13 : i64
      %cell = "builtin.unrealized_conversion_cast"(%carried, %bitindex) {chipyard.packed_ref_slice} : (!llhd.ref<i65>, i64) -> !llhd.ref<i13>
      %prior = llhd.prb %cell : i13
      llhd.drv %cell, %data after %nba if %enable : i13
      %partial = "builtin.unrealized_conversion_cast"(%cell, %low) {chipyard.packed_ref_slice} : (!llhd.ref<i13>, i4) -> !llhd.ref<i8>
      %byte = comb.extract %data from 0 : (i13) -> i8
      %partial_write = comb.and %enable, %partial_enable : i1
      llhd.drv %partial, %byte after %immediate if %partial_write : i8
      %updated = llhd.prb %cell : i13
      %next_before = comb.add %sum_before, %prior : i13
      %next_after = comb.add %sum_after, %updated : i13
      %next_index = comb.add %index, %one3 : i3
      %next_remaining = comb.sub %remaining, %one3 : i3
      %stride = comb.mux %advance_ref, %one3, %zero3 : i3
      %stride64 = comb.concat %pad61, %stride : i61, i3
      %bitstride = comb.mul %stride64, %word13 : i64
      %advanced = "builtin.unrealized_conversion_cast"(%carried, %bitstride) {chipyard.packed_ref_slice} : (!llhd.ref<i65>, i64) -> !llhd.ref<i65>
      cf.br ^loop(%advanced, %next_index, %next_remaining, %next_before, %next_after : !llhd.ref<i65>, i3, i3, i13, i13)
    ^exit:
      %pad = hw.constant 0 : i19
      %wide_before = comb.concat %pad, %sum_before : i19, i13
      %wide_after = comb.concat %pad, %sum_after : i19, i13
      sim.func.dpi.call @record(%wide_before, %wide_after) : (i32, i32) -> ()
      cf.br ^wait
    }
    %raw_a = llhd.prb %mem_a : !hw.struct<a4: i13, a3: i13, a2: i13, a1: i13, a0: i13>
    %raw_b = llhd.prb %mem_b : !hw.struct<a4: i13, a3: i13, a2: i13, a1: i13, a0: i13>
    %a = hw.bitcast %raw_a : (!hw.struct<a4: i13, a3: i13, a2: i13, a1: i13, a0: i13>) -> i65
    %b = hw.bitcast %raw_b : (!hw.struct<a4: i13, a3: i13, a2: i13, a1: i13, a0: i13>) -> i65
    %peer_a = seq.firreg %a clock %clk : i65
    %peer_b = seq.firreg %b clock %clk : i65
    %sensitivity = hw.instance "sensitivity" @CFGSensitivity(clock: %clock: i1, select: %select: i1, data: %data: i13) -> (q: i13)
    hw.output %a, %b, %peer_a, %peer_b, %sensitivity : i65, i65, i65, i65, i13
  }
  hw.module private @CFGSensitivity(in %clock: i1, in %select: i1, in %data: i13, out q: i13) {
    %zero = hw.constant 0 : i13
    %immediate = llhd.constant_time <0ns, 0d, 1e>
    %a = llhd.sig %zero : <i13>
    %b = llhd.sig %zero : <i13>
    %q = llhd.combinational -> i13 {
      llhd.drv %a, %zero after %immediate : i13
      cf.cond_br %select, ^join(%b : !llhd.ref<i13>), ^join(%a : !llhd.ref<i13>)
    ^join(%selected: !llhd.ref<i13>):
      %v = llhd.prb %selected : i13
      llhd.yield %v : i13
    }
    llhd.process {
      cf.br ^wait
    ^wait:
      llhd.wait (%clock : i1), ^event
    ^event:
      cf.cond_br %clock, ^write, ^wait
    ^write:
      llhd.drv %b, %data after %immediate : i13
      cf.br ^wait
    }
    hw.output %q : i13
  }
}
