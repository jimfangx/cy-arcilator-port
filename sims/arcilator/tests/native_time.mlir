module {
  hw.module @NativeTimeHarness(in %clock : i1, in %reset : i1, out io_success : i1, out ticks : i8, out stamp : i64, out clocked : i8, out held_stamp : i64) {
    %four = hw.constant 4 : i8
    %zero_count = hw.constant 0 : i8
    %one_count = hw.constant 1 : i8
    %generated = llhd.process -> i1 {
      %low = hw.constant false
      %start = hw.constant 0 : i8
      cf.br ^sleep(%low, %start : i1, i8)
    ^sleep(%level : i1, %edges : i8):
      %delay = llhd.constant_time <1500fs, 0d, 0e>
      llhd.wait yield (%level : i1), delay %delay, ^toggle(%level, %edges : i1, i8)
    ^toggle(%previous_level : i1, %previous_edges : i8):
      %true = hw.constant true
      %one = hw.constant 1 : i8
      %eight = hw.constant 8 : i8
      %next_level = comb.xor %previous_level, %true : i1
      %next_edges = comb.add %previous_edges, %one : i8
      %done = comb.icmp eq %next_edges, %eight : i8
      cf.cond_br %done, ^stop(%next_level : i1), ^sleep(%next_level, %next_edges : i1, i8)
    ^stop(%final_level : i1):
      llhd.halt %final_level : i1
    }
    %clk = seq.to_clock %generated
    %clocked_next = comb.add %clocked, %one_count : i8
    %clocked = seq.firreg %clocked_next clock %clk reset sync %reset, %zero_count : i8
    %ticks, %stamp = llhd.process -> i8, i64 {
      %zero = hw.constant 0 : i8
      %time_zero = hw.constant 0 : i64
      cf.br ^sleep(%zero, %time_zero : i8, i64)
    ^sleep(%count : i8, %last : i64):
      %delay = llhd.constant_time <1500fs, 0d, 0e>
      llhd.wait yield (%count, %last : i8, i64), delay %delay, ^tick(%count : i8)
    ^tick(%previous : i8):
      %one = hw.constant 1 : i8
      %limit = hw.constant 4 : i8
      %next = comb.add %previous, %one : i8
      %context = arc.inferred_context
      %now = arc.current_time %context
      %done = comb.icmp eq %next, %limit : i8
      cf.cond_br %done, ^stop(%next, %now : i8, i64), ^sleep(%next, %now : i8, i64)
    ^stop(%final_count : i8, %final_time : i64):
      llhd.halt %final_count, %final_time : i8, i64
    }
    %held_stamp = llhd.process -> i64 {
      %zero = hw.constant 0 : i64
      %first_delay = llhd.constant_time <2000fs, 0d, 0e>
      llhd.wait yield (%zero : i64), delay %first_delay, ^sample
    ^sample:
      %context = arc.inferred_context
      %captured = arc.current_time %context
      %second_delay = llhd.constant_time <3000fs, 0d, 0e>
      llhd.wait yield (%zero : i64), delay %second_delay, ^publish
    ^publish:
      // A direct SSA use across the wait must retain the time sampled at 2 ps.
      llhd.halt %captured : i64
    }
    %success = comb.icmp eq %ticks, %four : i8
    hw.output %success, %ticks, %stamp, %clocked, %held_stamp : i1, i8, i64, i8, i64
  }
}
