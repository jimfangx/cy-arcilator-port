package P; timeunit 10us; timeprecision 1ns;
  typedef struct packed {logic signed [2:0] hi; bit [0:4] lo;} payload_t;
endpackage
interface Bus; timeunit 1ns; timeprecision 10fs; logic [9:3] lane; endinterface
module Leaf #(parameter N=5)(input logic [N+2:3] a, output logic [0:N-1] y);
  timeunit 10ns; timeprecision 1ps;
  logic signed [2:6] dead_internal;
  logic [7:0] mem [5:2];
  P::payload_t packet;
  for(genvar k=2;k<4;k++) begin: g
    logic [k:0] local_net;
  end
  assign y=a;
endmodule
module Top(input logic [4:0] a, output logic [4:0] y);
  timeunit 1us; timeprecision 100ps;
  wire [4:0] other;
  Leaf left(a,y); Leaf right(a,other); Bus bus();
  wire [6:0] wide_other;
  Leaf #(7) wider({a,2'b01},wide_other);
  wire [4:0] escaped_other;
  Leaf \escaped.instance (a,escaped_other);
endmodule
module Unused; timeunit 100s; timeprecision 1fs; endmodule
