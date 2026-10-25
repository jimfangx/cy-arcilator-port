module {
  hw.module.extern private @NativeDeclarationDPI<TAG: ui32>(in %clock: !seq.clock, in %reset: i1, in %data: i32, out value: i64, out derived: i64, out shared: i64, out observed: i64, out live: i32)
  hw.module @DeclarationDPIProbe(in %clock: i1, in %reset: i1, in %data: i32, out first: i64, out first_derived: i64, out first_shared: i64, out first_observed: i64, out first_live: i32, out second: i64, out second_derived: i64, out second_shared: i64, out second_observed: i64, out second_live: i32) {
    %clk = seq.to_clock %clock
    %a, %b, %c, %d, %e = hw.instance "first" @NativeDeclarationDPI<TAG: ui32 = 17>(clock: %clk: !seq.clock, reset: %reset: i1, data: %data: i32) -> (value: i64, derived: i64, shared: i64, observed: i64, live: i32)
    %f, %g, %h, %i, %j = hw.instance "second" @NativeDeclarationDPI<TAG: ui32 = 29>(clock: %clk: !seq.clock, reset: %reset: i1, data: %data: i32) -> (value: i64, derived: i64, shared: i64, observed: i64, live: i32)
    hw.output %a, %b, %c, %d, %e, %f, %g, %h, %i, %j : i64, i64, i64, i64, i32, i64, i64, i64, i64, i32
  }
}
