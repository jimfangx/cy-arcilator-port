`timescale 1ns/1ps
module AnalogDrive(input d, inout pad);
  assign pad = d;
endmodule
module AnalogRead(inout pad, output q);
  assign q = pad;
endmodule
module AnalogTest;
  reg drive = 0;
  wire read;
  AnalogHarness dut(.*);
  initial begin
    repeat (12) begin
      #1.7; drive = ~drive;
      #0.1;
      if (read !== drive) $fatal(1, "foreign analog net disconnected");
    end
    $display("PASS: analog attach preserves foreign net connectivity");
    $finish;
  end
endmodule
