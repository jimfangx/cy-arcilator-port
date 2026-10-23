`timescale 1ns/1ps
module TraceTest;
  reg clock = 0, reset = 1, stall = 1;
  wire [31:0] count, length;
  wire [72:0] sample;
  wire [63:0] checksum, data;
  wire [7:0] keep;
  wire [47:0] mac;
  wire valid, ready, last;
  TraceHarness dut(.*);
  always #1 clock = ~clock;

  reg [63:0] words [0:2];
  reg [7:0] masks [0:2];
  integer received = 0;
  reg [72:0] expected_sample = 0;
  reg [63:0] expected_checksum = 0;
  always @(posedge clock) begin
    if (!reset && ready) begin
      if (!valid || received >= 3) $fatal(1, "unexpected TraceROM handshake");
      if (data !== words[received] || keep !== masks[received] || last !== (received == 2))
        $fatal(1, "TraceROM DPI payload mismatch at beat %0d", received);
      expected_sample = {last, keep, data};
      expected_checksum = expected_checksum ^ words[received];
      received = received + 1;
    end
  end
  initial begin
    words[0] = 64'hfedcba9876543210; masks[0] = 8'hff;
    words[1] = 64'h0123456789abcdef; masks[1] = 8'hff;
    words[2] = 64'h8000000000001357; masks[2] = 8'h03;
    repeat (2) @(negedge clock);
    reset = 0;
    for (int cycle = 0; cycle < 25; cycle++) begin
      @(negedge clock);
      stall = cycle % 3 == 0;
      #0.01;
      if (count !== received || sample !== expected_sample || checksum !== expected_checksum)
        $fatal(1, "native TraceROM consumer state mismatch");
      if (length !== 3 || mac !== 48'h102030405060)
        $fatal(1, "TraceROM initialization mismatch");
    end
    if (received != 3 || valid) $fatal(1, "TraceROM did not drain exactly three beats");
    $display("PASS: original TraceROM DPI data, masks, last, initialization and native backpressure");
    $finish;
  end
endmodule
