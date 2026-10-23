module {
  hw.module @TimeWakeupHarness(in %clock : i1, in %reset : i1, out io_success : i1, out ticks : i8) {
    %four = hw.constant 4 : i8
    %ticks = llhd.process -> i8 {
      %zero = hw.constant 0 : i8
      cf.br ^sleep(%zero : i8)
    ^sleep(%count : i8):
      %delay = llhd.constant_time <1fs, 0d, 0e>
      llhd.wait yield (%count : i8), delay %delay, ^tick(%count : i8)
    ^tick(%previous : i8):
      %one = hw.constant 1 : i8
      %limit = hw.constant 4 : i8
      %next = comb.add %previous, %one : i8
      %done = comb.icmp eq %next, %limit : i8
      cf.cond_br %done, ^stop(%next : i8), ^sleep(%next : i8)
    ^stop(%final_count : i8):
      llhd.halt %final_count : i8
    }
    %success = comb.icmp eq %ticks, %four : i8
    hw.output %success, %ticks : i1, i8
  }
}
