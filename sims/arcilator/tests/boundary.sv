`timescale 1ns/1ps
module ForeignRegister(input clock, reset, input [31:0] d, output reg [31:0] q);
  always @(posedge clock) if (reset) q <= 0; else q <= d;
endmodule

module ForeignWideRegister(input clock, reset, input [128:0] d, output reg [128:0] q);
  always @(posedge clock) if (reset) q <= 0; else q <= d;
endmodule

module ForeignClockBuffer(input in, output out);
  assign out = in;
endmodule

module ForeignCDriver(input [31:0] d, output [31:0] q);
  import "DPI-C" function int arcilator_c_driver(input int value);
  assign q = arcilator_c_driver(d);
endmodule

module BoundaryTest;
  reg clock = 0;
  reg reset = 1;
  reg clock2 = 0;
  wire force_fail = $test$plusargs("force-fail");
  wire force_finish = $test$plusargs("force-finish");
  wire force_stop = $test$plusargs("force-stop");
  wire [31:0] mem_q;
  wire [128:0] wide_q;
  reg [128:0] ref_wide = 0, ref_foreign_wide = 0;
  reg [31:0] count = 0;
  wire [31:0] replica_a, replica_b;
  wire [31:0] c_q;
  wire [2050:0] async_wide;
  reg [2050:0] ref_async_wide = 0;
  reg [31:0] ref_replica_a = 0, ref_replica_b = 0;
  reg [31:0] memory [0:3];
  initial for (int i = 0; i < 4; ++i) memory[i] = 0;
  always #1.7 clock2 = ~clock2;
  always @(posedge clock2) begin
    if (reset) ref_replica_b <= 0; else ref_replica_b <= ref_replica_b + 7;
    if (reset) begin count <= 0; ref_wide <= 0; ref_foreign_wide <= 0; end
    else begin
      count <= count + 1;
      ref_wide <= ref_foreign_wide + 129'h10000000000000001;
      ref_foreign_wide <= ref_wide;
    end
    memory[count[1:0]] <= count;
  end
  wire [31:0] native_q, foreign_q, async_q;
  Harness dut(.*);
  reg [31:0] ref_native = 0, ref_foreign = 0, ref_async = 0;
  always #1 clock = ~clock;
  always @(posedge clock) begin
    if (reset) ref_replica_a <= 0; else ref_replica_a <= ref_replica_a + 1;
    if (reset) begin ref_native <= 0; ref_foreign <= 0; end
    else begin ref_native <= ref_foreign + 1; ref_foreign <= ref_native; end
  end
  always @(posedge clock or posedge reset)
    if (reset) ref_async <= 0; else ref_async <= ref_foreign + 1;
  always @(posedge clock or posedge reset)
    if (reset) ref_async_wide <= (2051'd1 << 2050) | 2051'd5;
    else ref_async_wide <= ref_async_wide + (2051'd1 << 1025) + 2051'd3;
  task check;
    if (async_wide !== ref_async_wide)
      $fatal(1, "split asynchronous register mismatch");
    if (c_q !== ((ref_replica_a ^ 32'h13579bdf) + 32'd1))
      $fatal(1, "native/C DPI/native feedback mismatch");
    if ({replica_a, replica_b} !== {ref_replica_a, ref_replica_b})
      $fatal(1, "replicated stateful modules shared state or clock domains");
    if (mem_q !== memory[count[1:0]] || wide_q !== ref_foreign_wide)
      $fatal(1, "memory/wide/multiclock mismatch time=%t", $time);
    if ({native_q, foreign_q, async_q} !== {ref_native, ref_foreign, ref_async})
      $fatal(1, "boundary mismatch time=%t: got %d,%d,%d expected %d,%d,%d", $time,
        native_q, foreign_q, async_q, ref_native, ref_foreign, ref_async);
  endtask
  initial begin
    #4.2; reset = 0;
    repeat (20) begin @(negedge clock); #0.1; check(); end
    // Assert reset between edges: async state must change immediately.
    #0.2; reset = 1; #0.1; check();
    #2; check(); reset = 0;
    repeat (20) begin @(negedge clock); #0.1; check(); end
    $display("PASS: sequential feedback, async reset, multiclock memory, 129-bit DPI");
    $finish;
  end
endmodule
