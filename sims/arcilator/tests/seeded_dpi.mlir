module {
  sim.func.dpi private @partial(in %mode: i32, out low: i32, inout %acc: i32, in %data: i32, out flag: i1, ref %storage: !llvm.ptr, out wide: i129, return result: i64) attributes {verilogName="chipyard_seeded_partial"}
  sim.func.dpi private @partial_alias(in %m: i32, out l: i32, inout %a: i32, in %d: i32, out f: i1, ref %s: !llvm.ptr, out w: i129, return r: i64) attributes {verilogName="chipyard_seeded_partial"}
  hw.module @SeededDPIHarness(in %mode: i32, in %data: i32, in %seed: i32, in %unrelated: i32, out low: i32, out acc: i32, out flag: i1, out wide: i129, out result: i64, out storage: i32) {
    %low, %acc, %flag, %wide, %result, %storage = llhd.combinational -> i32, i32, i1, i129, i64, i32 {
      %one = llvm.mlir.constant(1 : i64) : i64
      %ref = llvm.alloca %one x i32 : (i64) -> !llvm.ptr
      llvm.store %data, %ref : i32, !llvm.ptr
      %alias = comb.extract %mode from 4 : (i32) -> i1
      cf.cond_br %alias, ^alias, ^original
    ^original:
      %a:5 = sim.func.dpi.call @partial(%mode, %seed, %data, %ref) : (i32, i32, i32, !llvm.ptr) -> (i32, i32, i1, i129, i64)
      cf.br ^join(%a#0, %a#1, %a#2, %a#3, %a#4 : i32, i32, i1, i129, i64)
    ^alias:
      %b:5 = sim.func.dpi.call @partial_alias(%mode, %seed, %data, %ref) : (i32, i32, i32, !llvm.ptr) -> (i32, i32, i1, i129, i64)
      cf.br ^join(%b#0, %b#1, %b#2, %b#3, %b#4 : i32, i32, i1, i129, i64)
    ^join(%l: i32, %accumulated: i32, %f: i1, %w: i129, %r: i64):
      %s = llvm.load %ref : !llvm.ptr -> i32
      llhd.yield %l, %accumulated, %f, %w, %r, %s : i32, i32, i1, i129, i64, i32
    }
    hw.output %low, %acc, %flag, %wide, %result, %storage : i32, i32, i1, i129, i64, i32
  }
}
