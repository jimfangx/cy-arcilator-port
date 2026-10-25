package global_declaration_pkg;
  import "DPI-C" function longint unsigned global_seed(input int tag);
  import "DPI-C" function longint unsigned global_step(input longint unsigned value, input int tag);
  import "DPI-C" function longint unsigned global_discard(input int tag);
  import "DPI-C" function real global_real(input int tag);
  import "DPI-C" function shortreal global_short(input int tag);
  longint unsigned value = global_seed(43);
  longint unsigned derived = global_step(value, 7);
  logic [128:0] wide = {1'b1, value, derived};
  longint unsigned upper = wide[127:64];
  longint unsigned unused_value = global_discard(9);
  real metric = -17.0 / global_real(43);
  shortreal fractional = global_short(43);
endpackage
import "DPI-C" function longint unsigned global_module_seed(input longint unsigned value, input longint unsigned upper, input int tag);
import "DPI-C" function void global_observe(input int tag, input longint unsigned value, input longint unsigned local_value);
module GlobalDeclarationDPI #(parameter TAG=17)(
  input clock, reset, enable, input [31:0] data,
  output [63:0] value, derived, local_value, observed, metric_bits,
  output [128:0] wide, output [31:0] small_bits
);
  longint unsigned local_snapshot = global_module_seed(global_declaration_pkg::derived, global_declaration_pkg::upper, TAG);
  reg [63:0] initial_observation;
  initial begin
    initial_observation = global_declaration_pkg::value ^ local_snapshot;
    global_observe(TAG, global_declaration_pkg::value, local_snapshot);
  end
  always @(posedge clock) if (!reset && enable) begin
    global_declaration_pkg::value <= global_declaration_pkg::value + 64'(data) + 64'(TAG);
    global_declaration_pkg::wide <= (global_declaration_pkg::wide << 1) ^ 129'(data);
  end
  assign value = global_declaration_pkg::value;
  assign derived = global_declaration_pkg::derived;
  assign local_value = local_snapshot;
  assign observed = initial_observation;
  assign wide = global_declaration_pkg::wide;
  assign metric_bits = $realtobits(global_declaration_pkg::metric);
  assign small_bits = $shortrealtobits(global_declaration_pkg::fractional);
endmodule
