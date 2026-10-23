module NativeLatchesForeign(
  input clock, peer_clock, enable, test_enable, open, nba_open,
  input [128:0] data,
  output logic [128:0] q = 129'h1_fedcba9876543210_0123456789abcdef,
  output logic [128:0] nba_q = 129'h0_0123456789abcdef_fedcba9876543210,
  output wire [128:0] aggregate_q,
  output logic [128:0] sampled = 0, sampled_nba = 0,
  output logic [31:0] count = 0,
  output wire gated_clock
);
  typedef struct packed {logic marker; logic [63:0] high, low;} value_t;
  value_t aggregate = '{marker:1'b1, high:64'h5555555555555555, low:64'haaaaaaaaaaaaaaaa};
  always_latch if (open) q = data;
  always_latch if (nba_open) nba_q <= data;
  always_latch if (open) aggregate = value_t'(data);
  assign aggregate_q = aggregate;
  prim_generic_clock_gating gate(.clk_i(clock), .en_i(enable),
    .test_en_i(test_enable), .clk_o(gated_clock));
  always @(posedge gated_clock) count <= count + 1;
  always @(posedge peer_clock) begin
    sampled <= q;
    sampled_nba <= nba_q;
  end
endmodule
