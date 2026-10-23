`timescale 1ns/1ps
module ProbeTest;
  reg clock=0, reset=1;
  reg [7:0] drive=0, expected=0;
  wire [7:0] read;
  ProbeHarness dut(.*);
  always #1 clock=~clock;
  always @(posedge clock) expected <= reset ? 8'd0 : drive;
  initial begin
    #4.2; reset=0;
    repeat (30) begin
      @(negedge clock); drive = drive + 8'd17;
      #0.1;
      if (read !== expected) $fatal(1,"cross-module probe changed register data");
    end
    $display("PASS: cross-module probe of native register");
    $finish;
  end
endmodule
