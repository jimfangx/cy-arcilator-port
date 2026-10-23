!array = !hw.array<5xi13>
!record = !hw.struct<head: i26, body: !array>
module {
 sim.func.dpi private @record(in %before: i32, in %after: i32, in %prior: i32) attributes {verilogName="local_memory_record"}
 hw.module @LocalHarness(in %select: i1, in %address: i3, in %count: i3, in %low: i65, in %data: i17, in %enable: i1, in %advance: i1, in %seed: i91,
   out a: i65, out b: i91, out before: i13, out after: i13, out scratch: i13, out tiny: i65, out prior: i13) {
  %zero13 = hw.constant 0 : i13
  %zero64 = hw.constant 0 : i64
  %pad61 = hw.constant 0 : i61
  %one3 = hw.constant 1 : i3
  %zero3 = hw.constant 0 : i3
  %stride = hw.constant 13 : i64
  %init = hw.aggregate_constant [5 : i13, 4 : i13, 3 : i13, 2 : i13, 1 : i13] : !array
  %seed_record = hw.bitcast %seed : (i91) -> !record
  %immediate = llhd.constant_time <0ns, 0d, 1e>
  %out_a, %out_b, %out_before, %out_after, %out_scratch, %out_tiny, %out_prior = llhd.combinational -> i65, i91, i13, i13, i13, i65, i13 {
   %a = llhd.sig %init : <!array>
   %b = llhd.sig %seed_record : <!record>
   %ar = "builtin.unrealized_conversion_cast"(%a, %zero64) {chipyard.packed_ref_slice} : (!llhd.ref<!array>, i64) -> !llhd.ref<i65>
   %br = "builtin.unrealized_conversion_cast"(%b, %zero64) {chipyard.packed_ref_slice} : (!llhd.ref<!record>, i64) -> !llhd.ref<i65>
   %first_cell = "builtin.unrealized_conversion_cast"(%ar, %zero64) {chipyard.packed_ref_slice} : (!llhd.ref<i65>, i64) -> !llhd.ref<i13>
   cf.cond_br %select, ^join(%br : !llhd.ref<i65>), ^join(%ar : !llhd.ref<i65>)
  ^join(%selected: !llhd.ref<i65>):
   cf.br ^loop(%selected, %address, %count, %zero13, %zero13, %zero13, %first_cell, %zero13 : !llhd.ref<i65>, i3, i3, i13, i13, i13, !llhd.ref<i13>, i13)
  ^loop(%ref: !llhd.ref<i65>, %index: i3, %remaining: i3, %sum_before: i13, %sum_after: i13, %sum_scratch: i13, %previous: !llhd.ref<i13>, %sum_prior: i13):
   %done = comb.icmp eq %remaining, %zero3 : i3
   cf.cond_br %done, ^exit, ^body
  ^body:
   %index64 = comb.concat %pad61, %index : i61, i3
   %bitindex = comb.mul %index64, %stride : i64
   %cell = "builtin.unrealized_conversion_cast"(%ref, %bitindex) {chipyard.packed_ref_slice} : (!llhd.ref<i65>, i64) -> !llhd.ref<i13>
   %before = llhd.prb %cell : i13
   %partial = "builtin.unrealized_conversion_cast"(%cell, %low) {chipyard.packed_ref_slice} : (!llhd.ref<i13>, i65) -> !llhd.ref<i17>
   llhd.drv %partial, %data after %immediate if %enable : i17
   %after = llhd.prb %cell : i13
   %fresh = llhd.sig : <!array>
   %scratch_before = llhd.prb %fresh : !array
   %scratch_bits = hw.bitcast %scratch_before : (!array) -> i65
   %old_low = comb.extract %scratch_bits from 0 : (i65) -> i13
   %scratch_cell = llhd.sig.array_get %fresh[%index] : <!array>
   %data13 = comb.extract %data from 0 : (i17) -> i13
   llhd.drv %scratch_cell, %data13 after %immediate if %enable : i13
   %old_prior = llhd.prb %previous : i13
   %next_prior = comb.add %sum_prior, %old_prior : i13
   %scratch_after = llhd.prb %scratch_cell : i13
   %scratch_value = comb.add %old_low, %scratch_after : i13
   %next_before = comb.add %sum_before, %before : i13
   %next_after = comb.add %sum_after, %after : i13
   %next_scratch = comb.add %sum_scratch, %scratch_value : i13
   %pad19 = hw.constant 0 : i19
   %before32 = comb.concat %pad19, %next_before : i19, i13
   %after32 = comb.concat %pad19, %next_after : i19, i13
   %prior32 = comb.concat %pad19, %old_prior : i19, i13
   sim.func.dpi.call @record(%before32, %after32, %prior32) : (i32, i32, i32) -> ()
   %stride_or_zero = comb.mux %advance, %stride, %zero64 : i64
   %next_ref = "builtin.unrealized_conversion_cast"(%ref, %stride_or_zero) {chipyard.packed_ref_slice} : (!llhd.ref<i65>, i64) -> !llhd.ref<i65>
   %next_index = comb.add %index, %one3 : i3
   %next_count = comb.sub %remaining, %one3 : i3
   cf.br ^loop(%next_ref, %next_index, %next_count, %next_before, %next_after, %next_scratch, %scratch_cell, %next_prior : !llhd.ref<i65>, i3, i3, i13, i13, i13, !llhd.ref<i13>, i13)
  ^exit:
   %tiny = llhd.sig : <i1>
   %tiny_ref = "builtin.unrealized_conversion_cast"(%tiny, %low) {chipyard.packed_ref_slice} : (!llhd.ref<i1>, i65) -> !llhd.ref<i129>
   %zero112 = hw.constant 0 : i112
   %wide_data = comb.concat %zero112, %data : i112, i17
   llhd.drv %tiny_ref, %wide_data after %immediate if %enable : i129
   %tiny_value = llhd.prb %tiny_ref : i129
   %tiny_bits = comb.extract %tiny_value from 0 : (i129) -> i65
   %a_value = llhd.prb %a : !array
   %b_value = llhd.prb %b : !record
   %a_bits = hw.bitcast %a_value : (!array) -> i65
   %b_bits = hw.bitcast %b_value : (!record) -> i91
   llhd.yield %a_bits, %b_bits, %sum_before, %sum_after, %sum_scratch, %tiny_bits, %sum_prior : i65, i91, i13, i13, i13, i65, i13
  }
  hw.output %out_a, %out_b, %out_before, %out_after, %out_scratch, %out_tiny, %out_prior : i65, i91, i13, i13, i13, i65, i13
 }
}
