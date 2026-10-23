module {
  hw.module @NativeAggregateHarness(in %clock: i1, in %reset: i1, in %enable: i1, in %data: i13, out lo: i13, out a0: i13, out a1: i13, out a2: i13, out wide_lo: i64, out wide_hi: i1, out q: i13, out io_success: i1) {
    %false=hw.constant false
    %zero=hw.constant 0 : i13
    %c0=hw.constant 0 : i2
    %c1=hw.constant 1 : i2
    %c2=hw.constant 2 : i2
    %c7=hw.constant 7 : i13
    %c291=hw.constant 291 : i13
    %clk=seq.to_clock %clock
    %packet=llhd.combinational -> !hw.struct<lo: i13, arr: !hw.array<3xi13>, wide: i65> {
      %v0=comb.xor %data,%c291 : i13
      %v1=comb.add %data,%c7 : i13
      %arr=hw.array_create %data,%v1,%v0 : i13
      %wide=comb.concat %data,%data,%data,%data,%data : i13,i13,i13,i13,i13
      %result=hw.struct_create (%data,%arr,%wide) : !hw.struct<lo: i13, arr: !hw.array<3xi13>, wide: i65>
      %captured=hw.struct_extract %packet["lo"] : !hw.struct<lo: i13, arr: !hw.array<3xi13>, wide: i65>
      %prefix=sim.fmt.literal "NATIVE_AGG_FEEDBACK "
      %value=sim.fmt.dec %captured specifierWidth 0 : i13
      %newline=sim.fmt.literal "\0A"
      %message=sim.fmt.concat (%prefix,%value,%newline)
      scf.if %enable { sim.proc.print %message }
      llhd.yield %result : !hw.struct<lo: i13, arr: !hw.array<3xi13>, wide: i65>
    }
    %feedback=hw.struct_extract %packet["lo"] : !hw.struct<lo: i13, arr: !hw.array<3xi13>, wide: i65>
    %array=hw.struct_extract %packet["arr"] : !hw.struct<lo: i13, arr: !hw.array<3xi13>, wide: i65>
    %a0=hw.array_get %array[%c0] : !hw.array<3xi13>,i2
    %a1=hw.array_get %array[%c1] : !hw.array<3xi13>,i2
    %a2=hw.array_get %array[%c2] : !hw.array<3xi13>,i2
    %wide=hw.struct_extract %packet["wide"] : !hw.struct<lo: i13, arr: !hw.array<3xi13>, wide: i65>
    %wide_lo=comb.extract %wide from 0 : (i65) -> i64
    %wide_hi=comb.extract %wide from 64 : (i65) -> i1
    %q=seq.firreg %feedback clock %clk reset sync %reset,%zero : i13
    hw.output %feedback,%a0,%a1,%a2,%wide_lo,%wide_hi,%q,%false : i13,i13,i13,i13,i64,i1,i13,i1
  }
}
