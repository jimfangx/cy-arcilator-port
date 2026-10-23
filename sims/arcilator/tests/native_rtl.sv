module ArcRTLLeaf #(parameter MASK = 8'h12) (input [7:0] data, output [7:0] result);
  assign result = data ^ MASK;
endmodule

module ArcRTLState #(parameter INIT = 0)
  (input clock, input reset, input enable, input [7:0] data, output reg [7:0] q);
  wire [7:0] transformed;
  ArcRTLLeaf leaf(data, transformed);
  always @(posedge clock)
    if (reset) q <= INIT;
    else if (enable) q <= transformed;
endmodule

// The imported compiler represents these as bits, while the Chisel boundary
// represents them as clocks. The importer must convert in both directions.
module ArcRTLClock(input source, output clock_out);
  assign clock_out = source;
endmodule
