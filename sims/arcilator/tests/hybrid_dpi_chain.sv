`timescale 1ns/1ps
module DPIChainTest;
  reg clock = 0, reset = 0;
  reg [31:0] data = 0;
  wire [31:0] sampled, count;
  reg [31:0] expected = 0, expected_count = 0;
  DPIChainHarness dut(.*);
  task check;
    if (sampled !== expected || count !== expected_count)
      $fatal(1, "hybrid DPI chain mismatch at %t: %x/%x, count=%d/%d",
             $time, sampled, expected, count, expected_count);
  endtask
  initial begin
    for (int cycle = 0; cycle < 512; ++cycle) begin
      clock = 0;
      #1; check();
      data = (cycle / 3) * 32'h01234567;
      reset = cycle % 17 == 0;
      if (reset) begin
        expected = 0;
        expected_count = 0;
      end else begin
        expected = data ^ expected_count;
        for (int site = 0; site < 16; ++site)
          expected = ((expected << (site + 1)) | (expected >> (31 - site))) ^
                     (32'h9e3779b9 * (site + 1));
        expected_count = expected_count + 1;
      end
      clock = 1;
      #1; check();
      #1; check();
    end
    $display("PASS: 512 hybrid hidden DPI chain cycles, pre-edge sampling and stable-input guards");
    $finish;
  end
endmodule
