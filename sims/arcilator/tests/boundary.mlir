module {
  hw.hierpath private @unused_module_path [@NativeCounter]
  hw.module.extern private @ForeignRegister(in %clock : !seq.clock, in %reset : i1, in %d : i32, out q : i32) attributes {verilogName = "ForeignRegister"}
  hw.module.extern private @ForeignWideRegister(in %clock : !seq.clock, in %reset : i1, in %d : i129, out q : i129) attributes {verilogName = "ForeignWideRegister"}
  hw.module.extern private @ForeignClockBuffer(in %in : !seq.clock, out out : !seq.clock)
  hw.module.extern private @ForeignCDriver(in %d : i32, out q : i32)
  hw.module private @NativeCounter(in %clock : !seq.clock, in %reset : i1, in %step : i32, out q : i32) {
    %zero = hw.constant 0 : i32
    %q = seq.firreg %next clock %clock reset sync %reset, %zero : i32
    %next = comb.add %q, %step : i32
    hw.output %q : i32
  }
  hw.module @Harness(in %clock : !seq.clock, in %reset : i1, in %clock2 : !seq.clock, in %force_fail : i1, in %force_finish : i1, in %force_stop : i1, out mem_q : i32, out wide_q : i129, out native_q : i32, out foreign_q : i32, out async_q : i32, out replica_a : i32, out replica_b : i32, out c_q : i32, out async_wide : i2051) {
    %zero = hw.constant 0 : i32
    %one = hw.constant 1 : i32
    %seven = hw.constant 7 : i32
    %replica_a = hw.instance "replica_a" @NativeCounter(clock: %clock: !seq.clock, reset: %reset: i1, step: %one: i32) -> (q: i32)
    %replica_b = hw.instance "replica_b" @NativeCounter(clock: %clock2: !seq.clock, reset: %reset: i1, step: %seven: i32) -> (q: i32)
    %c_result = hw.instance "c_driver" @ForeignCDriver(d: %replica_a: i32) -> (q: i32)
    %c_q = comb.add %c_result, %one : i32
    %buffered_clock = hw.instance "clock_buffer" @ForeignClockBuffer(in: %clock: !seq.clock) -> (out: !seq.clock)
    %native = seq.firreg %next clock %buffered_clock reset sync %reset, %zero : i32
    %foreign = hw.instance "foreign" @ForeignRegister(clock: %buffered_clock: !seq.clock, reset: %reset: i1, d: %native: i32) -> (q: i32)
    %next = comb.add %foreign, %one : i32
    %async = seq.firreg %next clock %clock reset async %reset, %zero : i32
    %wide_reset = hw.constant 129268024285244029202859506754679807841776410678861936128521381710098620555471563572788805646091653854754871843687592077976478236601963684380352609545793132482523509469203984367000791001558608427184230553536270273107168874570479024647352377353904681882326583408145220171550303566164263234430209596495721542087657333558673369682739899146258277979424704141305288232311222637324104770833841256601034371456708466903774414873847429430393404609206583617614790452933148924908502738843280838900628406907725293878714170322626791740183989073411994552862100665557749342174408541732918418581273914419808592774223414444238384922629 : i2051
    %wide_step = hw.constant 359538626972463181545861038157804946723595395788461314546860162315465351611001926265416954644815072042240227759742786715317579537628833244985694861278948248755535786849730970552604439202492188238906165904170011537676301364684925762947826221081654474326701021369172596479894491876959432609670712659248448274435 : i2051
    %async_wide = seq.firreg %async_wide_next clock %clock reset async %reset, %wide_reset : i2051
    %async_wide_next = comb.add %async_wide, %wide_step : i2051
    %zero129 = hw.constant 0 : i129
    %step129 = hw.constant 18446744073709551617 : i129
    %wide = seq.firreg %wide_next clock %clock2 reset sync %reset, %zero129 : i129
    %foreign_wide = hw.instance "wide" @ForeignWideRegister(clock: %clock2: !seq.clock, reset: %reset: i1, d: %wide: i129) -> (q: i129)
    %wide_next = comb.add %foreign_wide, %step129 : i129
    %count = seq.firreg %count_next clock %clock2 reset sync %reset, %zero : i32
    %count_next = comb.add %count, %one : i32
    %addr = comb.extract %count from 0 : (i32) -> i2
    %mem = seq.firmem name "test_mem" 0, 1, undefined, port_order : <4 x 32>
    seq.firmem.write_port %mem[%addr] = %count, clock %clock2 : <4 x 32>
    %mem_read = seq.firmem.read_port %mem[%addr], clock %clock2 : <4 x 32>
    %clock_bit = seq.from_clock %clock
    %true = hw.constant true
    %okay = comb.xor %force_fail, %true : i1
    verif.clocked_assert %okay, posedge %clock_bit label "test_assert" : i1
    %message = sim.fmt.literal "ARC_PRINT: hw.instance fake, seq.firreg clock %message reset async, verif.clocked_assert %message\0A"
    sim.triggered %clock {
      sim.proc.print %message
      scf.if %force_finish {
        sim.terminate success, quiet
      }
      scf.if %force_stop {
        sim.terminate failure, quiet
      }
    }
    hw.output %mem_read, %foreign_wide, %native, %foreign, %async, %replica_a, %replica_b, %c_q, %async_wide : i32, i129, i32, i32, i32, i32, i32, i32, i2051
  }
}
