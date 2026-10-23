module {
  sim.func.dpi private @record(in %site: i32, in %data: i32, return result: i32) attributes {verilogName="chipyard_sensitized_record"}
  sim.func.dpi private @record_alias(in %renamed_site: i32, in %renamed_data: i32, return renamed_result: i32) attributes {verilogName="chipyard_sensitized_record"}
  sim.func.dpi private @once() attributes {verilogName="chipyard_sensitized_once"}
  sim.func.dpi private @mixed(in %mode: i1, in %data: i32, inout %accumulator: i32, out low: i32, out flag: i1, return valid: i1) attributes {verilogName="chipyard_sensitized_mixed"}
  hw.module @SensitizedHarness(in %clock: i1, in %reset: i1, in %enable: i1, in %a: i32, in %b: i32, in %select: i1, in %noise: i8, in %unrelated: i8, in %aggregate_bits: i65, in %array_bits: i64, in %float_bits: i64, out aggregate: i65, out array: i64, out float_value: i32, out sensitive_clock: i32, out procedural_gate: i32, out sampled_comb: i32, out bare_clone: i32, out bare: i32, out gated: i32, out comb: i32, out mixed_acc: i32, out mixed_low: i32, out mixed_flag: i1, out mixed_valid: i1, out constant: i32, out clocked: i32, out procedural: i32, out passthrough: i8) {
    %s0 = hw.constant 0 : i32
    %s1 = hw.constant 1 : i32
    %s10 = hw.constant 10 : i32
    %s11 = hw.constant 11 : i32
    %s12 = hw.constant 12 : i32
    %s20 = hw.constant 20 : i32
    %s21 = hw.constant 21 : i32
    %s40 = hw.constant 40 : i32
    %c123 = hw.constant 123 : i32
    %zero = hw.constant 0 : i32
    %true = hw.constant true
    %clock_type = seq.to_clock %clock
    %bare = sim.func.dpi.call @record(%s0, %a) : (i32, i32) -> i32
    %bare_clone = sim.func.dpi.call @record_alias(%s0, %a) : (i32, i32) -> i32
    %gated = sim.func.dpi.call @record_alias(%s1, %a) enable %enable : (i32, i32) -> i32
    sim.func.dpi.call @once() : () -> ()
    sim.func.dpi.call @once() : () -> ()
    sim.func.dpi.call @once() enable %select : () -> ()
    %constant = sim.func.dpi.call @record(%s40, %c123) : (i32, i32) -> i32
    %comb = llhd.combinational -> i32 {
      cf.cond_br %select, ^yes, ^no
    ^yes:
      %unused = sim.func.dpi.call @record(%s10, %a) : (i32, i32) -> i32
      %first = sim.func.dpi.call @record_alias(%s10, %a) : (i32, i32) -> i32
      %second = sim.func.dpi.call @record(%s11, %first) : (i32, i32) -> i32
      %result_yes = comb.xor %first, %second : i32
      cf.br ^join(%result_yes: i32)
    ^no:
      %result_no = sim.func.dpi.call @record(%s12, %a) : (i32, i32) -> i32
      cf.br ^join(%result_no: i32)
    ^join(%value: i32):
      %noise32 = comb.concat %c0_i24, %noise : i24, i8
      %combined = comb.xor %value, %noise32 : i32
      llhd.yield %combined : i32
    }
    %c0_i24 = hw.constant 0 : i24
    %mixed_acc, %mixed_low, %mixed_flag, %mixed_valid = llhd.combinational -> i32, i32, i1, i1 {
      %results:4 = sim.func.dpi.call @mixed(%select, %a, %b) : (i1, i32, i32) -> (i32, i32, i1, i1)
      llhd.yield %results#0, %results#1, %results#2, %results#3 : i32, i32, i1, i1
    }
    %clocked = sim.func.dpi.call @record(%s21, %a) clock %clock_type enable %enable : (i32, i32) -> i32
    %procedural = llhd.process -> i32 {
      cf.br ^sleep(%zero: i32)
    ^sleep(%saved: i32):
      llhd.wait yield (%saved: i32), (%clock: i1), ^wake(%clock: i1)
    ^wake(%previous: i1):
      %low = comb.xor %previous, %true : i1
      %rising = comb.and %low, %clock : i1
      cf.cond_br %rising, ^edge, ^sleep(%saved: i32)
    ^edge:
      cf.cond_br %reset, ^sleep(%zero: i32), ^active
    ^active:
      cf.cond_br %enable, ^call, ^sleep(%saved: i32)
    ^call:
      %result = sim.func.dpi.call @record(%s20, %a) : (i32, i32) -> i32
      cf.br ^sleep(%result: i32)
    }
    %s22 = hw.constant 22 : i32
    %procedural_gate = llhd.process -> i32 {
      cf.br ^sleep(%zero: i32)
    ^sleep(%saved: i32):
      llhd.wait yield (%saved: i32), (%clock: i1), ^wake(%clock: i1)
    ^wake(%previous: i1):
      %low = comb.xor %previous, %true : i1
      %rising = comb.and %low, %clock : i1
      cf.cond_br %rising, ^call, ^sleep(%saved: i32)
    ^call:
      %result = sim.func.dpi.call @record(%s22, %a) enable %enable : (i32, i32) -> i32
      cf.br ^sleep(%result: i32)
    }
    %s50 = hw.constant 50 : i32
    %s51 = hw.constant 51 : i32
    %s52 = hw.constant 52 : i32
    %s53 = hw.constant 53 : i32
    %s54 = hw.constant 54 : i32
    %pair = hw.bitcast %aggregate_bits : (i65) -> !hw.struct<upper: i32, lower: i33>
    %aggregate_value = llhd.combinational -> !hw.struct<upper: i32, lower: i33> {
      %upper = hw.struct_extract %pair["upper"] : !hw.struct<upper: i32, lower: i33>
      %lower = hw.struct_extract %pair["lower"] : !hw.struct<upper: i32, lower: i33>
      %result = sim.func.dpi.call @record(%s50, %upper) : (i32, i32) -> i32
      %combined = hw.struct_create (%result, %lower) : !hw.struct<upper: i32, lower: i33>
      llhd.yield %combined : !hw.struct<upper: i32, lower: i33>
    }
    %aggregate = hw.bitcast %aggregate_value : (!hw.struct<upper: i32, lower: i33>) -> i65
    %array_input = hw.bitcast %array_bits : (i64) -> !hw.array<2xi32>
    %array_value = llhd.combinational -> !hw.array<2xi32> {
      %zero_index = hw.constant false
      %lower = hw.array_get %array_input[%zero_index] : !hw.array<2xi32>, i1
      %result = sim.func.dpi.call @record(%s51, %lower) : (i32, i32) -> i32
      %combined = hw.array_inject %array_input[%zero_index], %result : !hw.array<2xi32>, i1
      llhd.yield %combined : !hw.array<2xi32>
    }
    %array = hw.bitcast %array_value : (!hw.array<2xi32>) -> i64
    llhd.combinational {
      %value = comb.concat %c0_i24, %noise : i24, i8
      %unused = sim.func.dpi.call @record(%s52, %value) : (i32, i32) -> i32
      llhd.yield
    }
    %fp = arith.bitcast %float_bits : i64 to f64
    %float_value = llhd.combinational -> i32 {
      %bits = arith.bitcast %fp : f64 to i64
      %argument = comb.extract %bits from 0 : (i64) -> i32
      %result = sim.func.dpi.call @record(%s53, %argument) : (i32, i32) -> i32
      llhd.yield %result : i32
    }
    %sensitive_clock = llhd.combinational -> i32 {
      %bit = seq.from_clock %clock_type
      %argument = arith.extui %bit : i1 to i32
      %result = sim.func.dpi.call @record(%s54, %argument) : (i32, i32) -> i32
      llhd.yield %result : i32
    }
    %sampled_comb = seq.firreg %comb clock %clock_type reset sync %reset, %zero {name = "sampled_comb"} : i32
    hw.output %aggregate, %array, %float_value, %sensitive_clock, %procedural_gate, %sampled_comb, %bare_clone, %bare, %gated, %comb, %mixed_acc, %mixed_low, %mixed_flag, %mixed_valid, %constant, %clocked, %procedural, %unrelated : i65, i64, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i1, i1, i32, i32, i32, i8
  }
}
