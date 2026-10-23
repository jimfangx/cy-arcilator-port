`timescale 1ns/1ps
module ForeignDelay(input d, output q);
  assign #0.1 q = d;
endmodule

module EventTest;
  reg clock = 0;
  wire q, expected;
  DelayHarness dut(.*);
  assign #0.1 expected = clock;
  always #0.137 clock = ~clock;
  initial begin
    repeat (100) begin
      #0.139;
      if (q !== expected) $fatal(1, "delayed foreign feedback mismatch");
    end
    $display("PASS: delayed foreign events settle without event multiplication");
    $finish;
  end
endmodule
