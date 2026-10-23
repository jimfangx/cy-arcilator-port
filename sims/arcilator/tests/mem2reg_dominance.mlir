module {
  hw.module @ConditionalDefs(in %select: i1, in %enable: i1, in %data: i8, out before: i8, out after: i8) {
    %init = hw.constant 13 : i8
    %time = llhd.constant_time <0ns, 0d, 1e>
    %signal = llhd.sig %init : <i8>
    %old = llhd.combinational -> i8 {
      %before = llhd.prb %signal : i8
      cf.cond_br %select, ^left, ^right
    ^left:
      llhd.drv %signal, %data after %time if %enable : i8
      cf.br ^join
    ^right:
      llhd.drv %signal, %data after %time if %enable : i8
      cf.br ^join
    ^join:
      llhd.yield %before : i8
    }
    %after = llhd.prb %signal : i8
    hw.output %old, %after : i8, i8
  }
}
