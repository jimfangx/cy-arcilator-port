module ArcRTLInitial #(parameter SIZE = 8, BIAS = 3, ADDRW = $clog2(SIZE)) (
  input clock, reset, enable,
  input [ADDRW-1:0] waddr, raddr,
  input [12:0] data,
  output [12:0] selected,
  output reg [7:0] scalar
);
  reg [12:0] memory [SIZE-1:0];
  initial begin
    for (integer i = 0; i < SIZE; i++) memory[i] = 13'(i * 73 + 17 + BIAS);
    scalar = 8'(BIAS);
    for (integer j = 0; j < 5; j++) scalar = scalar + 8'(j * 7 + 3);
    for (longint k = 64'h100000002; k < 64'h100000008; k++) scalar += 8'(k);
    for (bit [3:0] k = 14; k != 2; k++) scalar += 8'(k);
  end
  always @(posedge clock) begin
    if (reset) begin
      memory <= '{default: 13'(BIAS + 37)};
      scalar <= 8'(BIAS + 11);
    end else if (enable) begin
      memory[waddr] <= data;
      scalar <= scalar + 3;
    end
  end
  assign selected = memory[raddr];
endmodule
