module TestPlusargs(input bit clock, output bit [6:0] matched, initial_matched);
 initial initial_matched = {$test$plusargs("π") != 0,
   $test$plusargs("UPPER") != 0, $test$plusargs("x y") != 0,
   $test$plusargs("disable_nv_clk_gate_asserts") != 0,
   $test$plusargs("") != 0, $test$plusargs("foo=") != 0,
   $test$plusargs("foo") != 0};
 always @(posedge clock) matched <= {$test$plusargs("π") != 0,
   $test$plusargs("UPPER") != 0, $test$plusargs("x y") != 0,
   $test$plusargs("disable_nv_clk_gate_asserts") != 0,
   $test$plusargs("") != 0, $test$plusargs("foo=") != 0,
   $test$plusargs("foo") != 0};
endmodule
