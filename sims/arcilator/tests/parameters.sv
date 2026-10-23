`timescale 1ns/1ps
module ParameterModel #(parameter string S="", parameter int N=0, parameter bit B=0, parameter bit F=1, parameter real R=0.0)(output wire ok);
  assign ok = S == "quote\" slash\\ line\n é" && N == -7 && B == 1 && F == 0 && R == 1.25;
endmodule
module ParameterTest;
  wire ok;
  Parameters dut(.*);
  initial begin
    #1;
    if (!ok) $fatal(1, "foreign parameters changed their values");
    $display("PASS: boolean, signed integer, real, escaped and UTF-8 foreign parameters");
    $finish;
  end
endmodule
