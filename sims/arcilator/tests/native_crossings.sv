module ArcRTLReturning #(parameter MASK = 8'h5a)
  (input clock, reset, input [7:0] data, output reg [7:0] q);
  wire [7:0] native_q;
  wire [15:0] native_pair;
  // The body comes from the native HW module, through an interface declaration.
  SimTSI leaf(.clock(clock), .reset(reset), .data(data), .q(native_q), .pair(native_pair));
  localparam NATIVE_WIDTH = $bits(leaf.q);
  initial if (NATIVE_WIDTH != 8) $fatal(1, "native boundary width changed");
  function automatic [7:0] with_width(input [7:0] value);
    with_width = value ^ 8'($bits(leaf.q));
  endfunction
  // Native struct layout: high byte is ~q, low byte is q. This expression
  // recovers q and distinguishes the packed field order.
  wire [7:0] recovered = native_pair[15:8] + (native_pair[7:0] << 1) + 8'd1;
  always @(posedge clock)
    if (reset) q <= MASK;
    else q <= with_width(recovered) ^ MASK ^ 8'd8;
endmodule
