module NativeFeedbackStateForeign(
  input clock, set_i, clear_i, oscillate,
  input [128:0] set_bits, clear_bits,
  output logic q = 1'b1,
  output logic [128:0] wide = 129'h1_fedcba9876543210_0123456789abcdef,
  output logic sampled = 0,
  output logic unstable = 0
);
  always_comb q = (q | set_i) & ~clear_i;
  always_comb wide = (wide | set_bits) & ~clear_bits;
  always_comb unstable = oscillate ? ~unstable : 1'b0;
  always @(posedge clock) sampled <= q;
endmodule
