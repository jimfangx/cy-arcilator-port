module {
  func.func private @chipyard_task_record(i32) -> i32
  llhd.coroutine private @leaf(%data: i32, %select: i1) -> i32 {
    cf.cond_br %select, ^first, ^second
  ^first:
    %seven = hw.constant 7 : i32
    %a = comb.add %data, %seven : i32
    %r1 = func.call @chipyard_task_record(%a) : (i32) -> i32
    llhd.return %r1 : i32
  ^second:
    %mask = hw.constant 1515870810 : i32
    %b = comb.xor %data, %mask : i32
    %r2 = func.call @chipyard_task_record(%b) : (i32) -> i32
    llhd.return %r2 : i32
  }
  llhd.coroutine private @pair(%data: i32, %select: i1) -> i32 {
    %first = llhd.call_coroutine @leaf(%data, %select) : (i32, i1) -> i32
    %second = llhd.call_coroutine @leaf(%first, %select) : (i32, i1) -> i32
    %result = comb.xor %first, %second : i32
    llhd.return %result : i32
  }
  hw.module @TaskHarness(in %clock: i1, in %reset: i1, in %enable: i1, in %data: i32, out value: i32) {
    %zero = hw.constant 0 : i32
    %true = hw.constant true
    %select = comb.extract %data from 0 : (i32) -> i1
    %value = llhd.process -> i32 {
      cf.br ^sleep(%zero: i32)
    ^sleep(%saved: i32):
      llhd.wait yield (%saved: i32), (%clock: i1), ^wake(%clock: i1)
    ^wake(%previous: i1):
      %low = comb.xor %previous, %true : i1
      %rising = comb.and %low, %clock : i1
      cf.cond_br %rising, ^edge, ^sleep(%saved: i32)
    ^edge:
      cf.cond_br %reset, ^sleep(%zero: i32), ^enabled
    ^enabled:
      cf.cond_br %enable, ^call, ^sleep(%saved: i32)
    ^call:
      %result = llhd.call_coroutine @pair(%data, %select) : (i32, i1) -> i32
      cf.br ^sleep(%result: i32)
    }
    hw.output %value : i32
  }
}
