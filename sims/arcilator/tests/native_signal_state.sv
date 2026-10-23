`timescale 1ps/1fs
module ArcRTLSignalState (
  input clock, reset, write,
  input [128:0] data,
  output reg [128:0] q = 129'h1_fedcba9876543210_0123456789abcdef
);
  // A timed suspension keeps this process from becoming a simple firreg.
  // Its nonblocking writes still commit after peers sample the old value.
  always @(posedge clock) begin
    if (reset) q <= 129'h0_55aa55aa55aa55aa_aa55aa55aa55aa55;
    else if (write) q <= data;
    #0.25;
  end
endmodule
