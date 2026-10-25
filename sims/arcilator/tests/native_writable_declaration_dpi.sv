package declaration_context_pkg;
  import "DPI-C" context function longint unsigned declaration_package_seed(input int tag);
endpackage
import "DPI-C" function void declaration_observe(input int tag, input longint unsigned seed, input longint unsigned derived);
import "DPI-C" function int unsigned declaration_tick(input int tag, input int unsigned data);
import "DPI-C" function longint unsigned declaration_discard(input int tag);
import "DPI-C" function chandle declaration_handle(input int tag);
import "DPI-C" function longint unsigned declaration_handle_value(input chandle handle, input int tag);
module NativeDeclarationDPI #(parameter TAG = 17)(
  input clock, reset, input [31:0] data,
  output [63:0] value, derived, shared,
  output reg [63:0] observed, output reg [31:0] live, output [128:0] wide
);
  import "DPI-C" context function longint unsigned declaration_seed(input int tag);
  import "DPI-C" context function longint unsigned declaration_step(input longint unsigned seed, input int tag);
  int cookie = TAG + 7;
  export "DPI-C" function declaration_get_cookie;
  function int declaration_get_cookie(); return cookie; endfunction
  export "DPI-C" function declaration_set_cookie;
  function void declaration_set_cookie(input int value); cookie = value; endfunction
  longint unsigned snapshot = declaration_seed(TAG);
  longint unsigned next_snapshot = declaration_step(snapshot, TAG);
  longint unsigned shared_snapshot = snapshot + 64'h100000001;
  chandle handle = declaration_handle(TAG);
  longint unsigned handle_value = declaration_handle_value(handle, TAG);
  longint unsigned package_snapshot = declaration_context_pkg::declaration_package_seed(TAG);
  longint unsigned unused_snapshot = declaration_discard(TAG);
  export "DPI-C" function declaration_get_snapshot;
  function longint unsigned declaration_get_snapshot(); return snapshot; endfunction
  export "DPI-C" function declaration_set_snapshot;
  function void declaration_set_snapshot(input longint unsigned value); snapshot = value; endfunction
  if (TAG == 29) begin
    always @(posedge clock or posedge reset)
      if (reset) snapshot <= 64'h8000000100000055 + 64'(TAG);
      else snapshot <= snapshot + 64'(data);
  end else begin
    always @(posedge clock)
      if (reset) snapshot <= 64'h8000000100000055 + 64'(TAG);
      else snapshot <= snapshot + 64'(data);
  end
  reg [128:0] wide_snapshot = {1'b1, snapshot, snapshot ^ 64'hfedcba9876543210};
  always @(posedge clock)
    if (reset) wide_snapshot <= 129'h123456789abcdef00123456789abcdef01 ^ 129'(TAG);
    else wide_snapshot <= (wide_snapshot << 1) ^ 129'(data);
  assign wide = wide_snapshot;
  assign value = snapshot;
  assign derived = next_snapshot;
  assign shared = shared_snapshot;
  initial begin
    observed = next_snapshot + shared_snapshot;
    declaration_observe(TAG, snapshot, next_snapshot);
  end
  always @(posedge clock)
    if (!reset) live <= declaration_tick(TAG, data);
endmodule
