`include "util_dpi.vh"

module VortexDpiProbe(
  input logic clock, reset, enable, arithmetic_enable,
  input logic signed_a, signed_b, signed_division, assertion_failure, trace_enable,
  input logic [63:0] a, b, pc,
  input int warp,
  output logic [63:0] low_product, high_product, quotient, remainder, uuid
);
  int instance_id;
  `INT_TYPE low_value, high_value, quotient_value, remainder_value;
  initial instance_id = dpi_register();
  always @(posedge clock) begin
    if (reset) begin
      low_product <= 0;
      high_product <= 0;
      quotient <= 0;
      remainder <= 0;
      uuid <= dpi_uuid_gen(1, warp, pc);
    end else if (enable) begin
      dpi_imul(arithmetic_enable, signed_a, signed_b,
               `INT_TYPE'(a), `INT_TYPE'(b), low_value, high_value);
      dpi_idiv(arithmetic_enable, signed_division,
               `INT_TYPE'(a), `INT_TYPE'(b), quotient_value, remainder_value);
      low_product <= low_value;
      high_product <= high_value;
      quotient <= quotient_value;
      remainder <= remainder_value;
      uuid <= dpi_uuid_gen(0, warp, pc);
      dpi_assert(instance_id, !assertion_failure, 3);
      if (trace_enable) begin
        dpi_trace_start();
        dpi_trace(2, $sformatf("VORTEX_DPI warp=%0d pc=%016h\n", warp, pc));
        dpi_trace(4, "VORTEX_DPI_HIDDEN\n");
        dpi_trace_stop();
      end
    end
  end
endmodule
