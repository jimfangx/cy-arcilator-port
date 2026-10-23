module FormatLeaf(input bit clock, reset, enable, input int tag, value,
                  output int observed);
  import "DPI-C" context function string host_label(input int tag, value);
  import "DPI-C" context function int host_check(input int tag, value,
                                                input string inner, outer);
  import "DPI-C" context function string host_side(input int tag, input string value);
  integer file;
  initial file = $fopen($sformatf("format-native-%0d.log", tag), "w");
  final $fclose(file);
  `include "format_strings/functions.svh"
  always @(posedge clock) begin
    if (reset) observed <= 0;
    else if (enable) begin
      logic [64:0] wide;
      string label, inner, outer;
      wide = {1'b1, 64'hfedcba9876543210} ^ {33'b0, value[31:0]};
      label = host_label(tag, value);
      inner = branch_render(value, render(value, wide, label));
      outer = nested(inner, wide, value, tag);
      observed <= host_check(tag, value, inner, outer);
      $display("NATIVE_FORMAT tag=%0d msg=[%s]", tag, host_side(tag, outer));
      $fdisplay(file, "FILE tag=%0d value=[%s]", tag, outer);
      $fflush();
    end
  end
endmodule
module FormatStringsTop(input bit clock, reset, enable, input int value,
                        output int first, second);
  FormatLeaf left(clock, reset, enable, 0, value, first);
  FormatLeaf right(clock, reset, enable, 1, value, second);
endmodule
