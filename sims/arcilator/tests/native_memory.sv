module ArcRTLMemory #(parameter INIT = 32'h19a) (
  input clock, reset, enable,
  input [1:0] op,
  input [4:0] address, read_address,
  input [12:0] data,
  input [1:0] mask,
  output wire [12:0] observed,
  output reg [12:0] before_q, after_q,
  output wire [259:0] stack, next_stack
);
  reg [12:0] memory [0:8];
  initial for (int i = 0; i < 9; i++) memory[i] = 13'((i * 33) ^ INIT);
  assign observed = memory[read_address];
  always @(posedge clock) begin
    if (reset) begin
      for (int i = 0; i < 9; i++) memory[i] = 13'((i * 33) ^ INIT);
      before_q <= 0;
      after_q <= 0;
    end else if (enable) begin
      before_q <= memory[address];
      case (op)
        0: memory[address] <= data;
        1: begin
          if (mask[0]) memory[address][7:0] <= data[7:0];
          if (mask[1]) memory[address][12:8] <= data[12:8];
        end
        2: begin
          memory[address][3:0] <= data[3:0];
          memory[address][7:4] = data[7:4];
        end
        3: begin
          memory[address] <= data;
          memory[address][7:0] <= ~data[7:0];
          memory[address][12:8] = ~data[12:8];
        end
      endcase
      after_q <= memory[address];
    end
  end
  typedef struct packed { logic valid; logic [63:0] address; } entry_t;
  entry_t [3:0] stack_q, stack_d;
  always_comb begin
    stack_d = stack_q;
    if (enable) begin
      if (op[0]) begin
        stack_d[3:1] = stack_q[2:0];
        stack_d[0].valid = 1;
        stack_d[0].address = (64'(INIT) << 32) | 64'(data);
      end
      if (op[1]) begin
        stack_d[2:0] = stack_q[3:1];
        stack_d[3].valid = 0;
        stack_d[3].address = 0;
      end
      if (&op) begin
        stack_d = stack_q;
        stack_d[0].valid = 1;
        stack_d[0].address = (64'(INIT) << 32) | 64'(data);
      end
    end
  end
  always @(posedge clock)
    if (reset) stack_q <= '0;
    else stack_q <= stack_d;
  assign stack = stack_q;
  assign next_stack = stack_d;
endmodule
