module {
  hw.module.extern private @ArcRTLProcedural<BIAS: ui32>(in %clock : !seq.clock, in %reset : i1, in %enable : i1, in %data : i8, in %lanes : i16, out q : i8, out ticks : i8, out stamp : i64, out clock_stamp : i64, out held_stamp : i64, out payload : i24, out inverted : i16)
  hw.module @NativeProceduralHarness(in %clock : i1, in %reset : i1, in %enable : i1, in %data : i8, in %lanes : i16, out q : i8, out ticks : i8, out stamp : i64, out clock_stamp : i64, out held_stamp : i64, out payload : i24, out inverted : i16) {
    %clk = seq.to_clock %clock
    %q, %ticks, %stamp, %clock_stamp, %held_stamp, %payload, %inverted = hw.instance "procedural" @ArcRTLProcedural<BIAS: ui32 = 3>(clock: %clk: !seq.clock, reset: %reset: i1, enable: %enable: i1, data: %data: i8, lanes: %lanes: i16) -> (q: i8, ticks: i8, stamp: i64, clock_stamp: i64, held_stamp: i64, payload: i24, inverted: i16)
    hw.output %q, %ticks, %stamp, %clock_stamp, %held_stamp, %payload, %inverted : i8, i8, i64, i64, i64, i24, i16
  }
}
