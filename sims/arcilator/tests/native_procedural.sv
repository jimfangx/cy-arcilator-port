`timescale 1ps/1fs

typedef struct packed {
  logic [7:0] state;
  logic [15:0] lanes;
} arc_payload_t;

module ArcRTLProcedural #(parameter BIAS = 3)
  (input clock, reset, enable, input [7:0] data,
   input [1:0][7:0] lanes, output reg [7:0] q,
   output reg [7:0] ticks, output reg [63:0] stamp,
   output reg [63:0] clock_stamp, held_stamp,
   output arc_payload_t payload, output [1:0][7:0] inverted,
   output [7:0] unused_diagnostic);
  localparam string LABEL = "NATIVE_PROCEDURAL";
  assign payload = {q, lanes};
  assign inverted = lanes ^ 16'h5aa5;
  assign unused_diagnostic = q ^ ticks;
  always @(posedge clock) begin
    if (reset) q <= 0;
    else if (enable) begin
      q <= q + data + BIAS;
      $write("%s %0d %0d\n", LABEL, q, $time);
    end
  end
  always @(posedge clock)
    if (reset) clock_stamp <= 0;
    else if (enable) clock_stamp <= $time;
  initial begin
    automatic time sampled;
    held_stamp = 0;
    #2;
    sampled = $time;
    #3;
    held_stamp = sampled;
  end
  initial begin
    ticks = 0;
    stamp = 0;
    repeat (4) begin
      #1.5;
      ticks = ticks + 1;
      stamp = $time;
    end
  end
endmodule
