`timescale 1ps/1fs
typedef struct packed {
  logic [5:0] tag;
  logic [2:0][16:0] lanes;
} ArcUndrivenBundle;
typedef struct packed {
  logic [16:0] a, b, c;
} ArcMergedBundle;
module ArcRTLSignalCoherence (
  input clock, reset,
  input [43:0] step,
  input [7:0] low,
  input [1:0] selector,
  input [16:0] payload,
  output reg [43:0] counter,
  output wire [256:0] floating,
  output ArcUndrivenBundle bundle,
  output reg [128:0] seeded = 129'h1_fedcba9876543210_0123456789abcdef,
  output wire [16:0] partial,
  output ArcMergedBundle merged
);
  wire [43:0] next_value = reset ? 44'd0 : counter + step;
  for (genvar lane = 0; lane < 5; lane++) begin : repeated
    // These procedural drivers have exactly the same clock and next value.
    always @(posedge clock) counter <= next_value;
  end
  assign partial[7:0] = low;
  function automatic ArcMergedBundle choose(input logic [1:0] s, input logic [16:0] data);
    ArcMergedBundle half;
    case (s)
      0: begin half.a = data; half.b = data ^ 17'h12345; end
      1: begin half.a = data + 17'h1abcd; half.b = ~data; end
      2: begin half.a = data ^ 17'h15555; half.b = data + 17'h11111; end
      default: begin half.a = ~data; half.b = data ^ 17'h1aaaa; end
    endcase
    half.c = data + 17'h123;
    choose = half;
  endfunction
  assign merged = choose(selector, payload);
endmodule
