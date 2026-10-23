module {
  hw.module @NativePropertiesHarness(in %clock: i1, in %reset: i1, in %enable: i1, in %constant_check: i1, in %a: i1, in %b: i1, in %c: i1, in %d: i1, out state: i1, out io_success: i1) {
    %false = hw.constant false
    %true = hw.constant true
    %clk = seq.to_clock %clock
    %state = seq.firreg %d clock %clk reset sync %reset, %false : i1
    %not_reset = comb.xor %reset, %true : i1
    %active = comb.and %enable, %not_reset : i1
    %constant_active = comb.and %active, %constant_check : i1
    %inner = ltl.implication %a, %b : i1, i1
    %not_c = ltl.not %c : i1
    %joined = ltl.and %inner, %not_c : !ltl.property, !ltl.property
    %nested = ltl.implication %d, %joined : i1, !ltl.property
    verif.clocked_assert %nested if %active, posedge %clock label "nested" : !ltl.property
    %concat = ltl.concat %a, %b : i1, i1
    %delay = ltl.delay %concat, 0, 0 : !ltl.sequence
    %repeat = ltl.repeat %delay, 1, 0 : !ltl.sequence
    %intersect = ltl.intersect %repeat, %c : !ltl.sequence, i1
    %negative = ltl.not %intersect : !ltl.sequence
    verif.clocked_assert %negative if %active, negedge %clock label "negative" : !ltl.property
    %both = ltl.implication %b, %c : i1, i1
    verif.clocked_assert %both if %active, edge %clock label "both" : !ltl.property
    %pre_edge = ltl.implication %d, %state : i1, i1
    verif.clocked_assert %pre_edge if %active, posedge %clock label "pre_edge" : !ltl.property
    %always_true = ltl.boolean_constant true
    %always_false = ltl.boolean_constant false
    verif.clocked_assert %always_true if %constant_active, posedge %clock label "constant_true" : !ltl.property
    verif.clocked_assert %always_false if %constant_active, posedge %clock label "constant_false" : !ltl.property
    hw.output %state, %false : i1, i1
  }
}
