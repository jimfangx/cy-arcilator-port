module {
  sim.func.dpi private @opaque_callback(in %data: i64, return result: i64) attributes {verilogName = "chipyard_test_opaque_callback"}
  sim.func.dpi private @opaque_callback_alias(in %renamed_input: i64, return renamed_result: i64) attributes {verilogName = "chipyard_test_opaque_callback"}
  sim.func.dpi private @mixed_callback(in %mode: i1, in %signed_a: i1, in %signed_b: i1, in %a: i32, in %b: i32, inout %accumulator: i32, out low: i32, out high: i32, out flag: i1, return result: i1) attributes {verilogName = "chipyard_test_mixed_callback"}
  sim.func.dpi private @mixed_callback_alias(in %rename_mode: i1, in %rename_signed_a: i1, in %rename_signed_b: i1, in %rename_a: i32, in %rename_b: i32, inout %rename_accumulator: i32, out rename_low: i32, out rename_high: i32, out rename_flag: i1, return rename_result: i1) attributes {verilogName = "chipyard_test_mixed_callback"}
  hw.module @OpaqueCallbackHarness(in %clock: i1, in %enable: i1, in %data: i64, out value: i44, out valid: i1, out comb_value: i8, out accumulator: i32, out low: i32, out high: i32, out flag: i1, out boolean_result: i1) {
    %true = hw.constant true
    %false = hw.constant false
    %zero = hw.constant 0 : i44
    %zero32 = hw.constant 0 : i32
    %immediate = llhd.constant_time <0ns, 0d, 1e>
    %visible = llhd.sig name "visible_callback_result" : <i44>
    %value, %valid, %accumulator, %low, %high, %flag, %boolean_result = llhd.process -> i44, i1, i32, i32, i32, i1, i1 {
      cf.br ^sleep(%zero, %false, %zero32, %zero32, %zero32, %false, %false : i44, i1, i32, i32, i32, i1, i1)
    ^sleep(%last: i44, %write: i1, %acc: i32, %lo: i32, %hi: i32, %f: i1, %r: i1):
      llhd.wait yield (%last, %write, %acc, %lo, %hi, %f, %r : i44, i1, i32, i32, i32, i1, i1), (%clock : i1), ^wake(%clock : i1)
    ^wake(%previous_clock: i1):
      %was_low = comb.xor %previous_clock, %true : i1
      %rising = comb.and %was_low, %clock : i1
      %active = comb.and %rising, %enable : i1
      cf.cond_br %active, ^call, ^sleep(%zero, %false, %zero32, %zero32, %zero32, %false, %false : i44, i1, i32, i32, i32, i1, i1)
    ^call:
      %choose_alias = comb.extract %data from 3 : (i64) -> i1
      cf.cond_br %choose_alias, ^opaque_alias, ^opaque_original
    ^opaque_original:
      %original_result = sim.func.dpi.call @opaque_callback(%data) : (i64) -> i64
      cf.br ^convert(%original_result : i64)
    ^opaque_alias:
      %alias_result = sim.func.dpi.call @opaque_callback_alias(%data) : (i64) -> i64
      cf.br ^convert(%alias_result : i64)
    ^convert(%result: i64):
      %narrow = comb.extract %result from 0 : (i64) -> i44
      %mode = comb.extract %data from 2 : (i64) -> i1
      %signed_a = comb.extract %data from 0 : (i64) -> i1
      %signed_b = comb.extract %data from 1 : (i64) -> i1
      %a = comb.extract %data from 0 : (i64) -> i32
      %b = comb.extract %data from 32 : (i64) -> i32
      cf.cond_br %choose_alias, ^mixed_alias, ^mixed_original
    ^mixed_original:
      %original_mixed:5 = sim.func.dpi.call @mixed_callback(%mode, %signed_a, %signed_b, %a, %b, %a) : (i1, i1, i1, i32, i32, i32) -> (i32, i32, i32, i1, i1)
      cf.br ^publish(%original_mixed#0, %original_mixed#1, %original_mixed#2, %original_mixed#3, %original_mixed#4 : i32, i32, i32, i1, i1)
    ^mixed_alias:
      %alias_mixed:5 = sim.func.dpi.call @mixed_callback_alias(%mode, %signed_a, %signed_b, %a, %b, %a) : (i1, i1, i1, i32, i32, i32) -> (i32, i32, i32, i1, i1)
      cf.br ^publish(%alias_mixed#0, %alias_mixed#1, %alias_mixed#2, %alias_mixed#3, %alias_mixed#4 : i32, i32, i32, i1, i1)
    ^publish(%new_acc: i32, %new_low: i32, %new_high: i32, %new_flag: i1, %new_result: i1):
      cf.br ^sleep(%narrow, %true, %new_acc, %new_low, %new_high, %new_flag, %new_result : i44, i1, i32, i32, i32, i1, i1)
    }
    llhd.drv %visible, %value after %immediate : i44
    %observed = llhd.prb %visible : i44
    // Drive hoisting must leave the sibling DPI process's callback timing and
    // return values intact while this region reacts to changing inputs.
    %comb_signal = llhd.sig name "combinational_value" : <i8>
    llhd.combinational {
      cf.cond_br %enable, ^on, ^off
    ^on:
      %byte = comb.extract %data from 0 : (i64) -> i8
      llhd.drv %comb_signal, %byte after %immediate : i8
      llhd.yield
    ^off:
      %zero8 = hw.constant 0 : i8
      llhd.drv %comb_signal, %zero8 after %immediate : i8
      llhd.yield
    }
    %comb_value = llhd.prb %comb_signal : i8
    hw.output %observed, %valid, %comb_value, %accumulator, %low, %high, %flag, %boolean_result : i44, i1, i8, i32, i32, i32, i1, i1
  }
}
