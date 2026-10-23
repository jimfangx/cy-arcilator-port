module ArcRTLPortRules #(
  parameter int WIDTH = 5,
  parameter int BIAS = 0,
  parameter string TAG = "default"
) (
  input clock, reset,
  input [WIDTH-1:0] \signed ,
  input [WIDTH-1:0] wide,
  input logic optional_bit = 1'b1,
  input logic [WIDTH-1:0] optional_vector = {WIDTH{1'b1}} ^ 2,
  input floating,
  output [WIDTH-1:0] narrow_seen,
  output [WIDTH-1:0] unsigned_out,
  output signed [WIDTH-1:0] signed_out,
  output [12:0] truncated,
  output [12:0] defaults,
  output [7:0] flags,
  output reg [WIDTH-1:0] q,
  output [12:0] unused_extra
);
  assign narrow_seen = \signed ;
  assign unsigned_out = wide;
  assign signed_out = wide;
  assign truncated = (13'(wide) ^ 13'(BIAS)) | 13'h1c00;
  assign defaults = 13'(optional_vector);
  assign flags = (8'(TAG.len()) << 2) | {6'd0, floating, optional_bit};
  assign unused_extra = ~defaults;
  always @(posedge clock)
    if (reset) q <= WIDTH'(BIAS);
    else q <= wide ^ \signed ^ WIDTH'(BIAS);
endmodule
