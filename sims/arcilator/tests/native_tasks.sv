module ArcRTLTasks(input clock, reset, enable, input [7:0] data,
                   output logic [15:0] q, counter);
  task automatic leaf(input logic [7:0] value, input logic select,
                      output logic [15:0] result);
    if (select) result = {value, 8'h5a};
    else result = {~value, 8'ha5};
  endtask

  task automatic nested(input logic [7:0] value,
                        inout logic [15:0] accumulator,
                        output logic [15:0] result);
    logic [15:0] first, second;
    leaf(value, value[0], first);
    leaf(value ^ 8'h33, !value[0], second);
    result = first ^ second ^ accumulator;
    accumulator = accumulator + 16'h137;
    $write("NATIVE_TASK %0d %0d\n", result, accumulator);
  endtask

  always @(posedge clock) begin
    if (reset) begin
      q <= 0;
      counter <= 0;
    end else if (enable) begin
      automatic logic [15:0] sampled, next_count;
      next_count = counter;
      nested(data, next_count, sampled);
      q <= sampled;
      counter <= next_count;
    end
  end
endmodule
