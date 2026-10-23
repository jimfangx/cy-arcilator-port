module ArcRTLSlices (
  input clock_a, clock_b, reset, enable,
  input [7:0] data,
  input [1:0] index,
  output reg [7:0] bits_q,
  output reg [3:0][8:0] lanes_q,
  output wire [13:0] fields_q,
  output wire [8:0] selected,
  output wire [15:0] prefix_q
);
  typedef struct packed {
    logic [8:0] hi;
    struct packed { logic [2:0] upper; logic [1:0] lower; } lo;
  } fields_t;
  fields_t fields;
  for (genvar i = 0; i < 8; i++) begin
    if (i % 2 == 0) begin
      always @(posedge clock_a)
        if (reset) bits_q[i] <= 1'((8'h96 >> i) & 1);
        else if (enable) bits_q[i] <= data[i] ^ bits_q[(i + 1) % 8];
    end else begin
      always @(posedge clock_b)
        if (reset) bits_q[i] <= 1'((8'h96 >> i) & 1);
        else if (enable) bits_q[i] <= data[i] ^ bits_q[(i + 1) % 8];
    end
  end
  for (genvar i = 0; i < 4; i++) begin
    if (i % 2 == 0) begin
      always @(posedge clock_a)
        if (reset) lanes_q[i] <= 9'(i + 17);
        else if (enable) lanes_q[i] <= lanes_q[i] + lanes_q[(i + 1) % 4] + 9'(data) + 9'(i + 1);
    end else begin
      always @(posedge clock_b)
        if (reset) lanes_q[i] <= 9'(i + 17);
        else if (enable) lanes_q[i] <= lanes_q[i] + lanes_q[(i + 1) % 4] + 9'(data) + 9'(i + 1);
    end
  end
  always @(posedge clock_a)
    if (reset) fields.hi <= 9'h123;
    else if (enable) fields.hi <= fields.hi + 9'(data) + 9'(fields.lo.upper) + 1;
  always @(posedge clock_b)
    if (reset) fields.lo.upper <= 3'h5;
    else if (enable) fields.lo.upper <= fields.lo.upper ^ data[2:0];
  always @(posedge clock_a)
    if (reset) fields.lo.lower <= 2'h2;
    else if (enable) fields.lo.lower <= fields.lo.lower + data[1:0] + fields.hi[1:0];
  assign fields_q = fields;
  assign selected = lanes_q[index];
  // Distinct continuous writers build a dependency tree in a packed array.
  // Dependencies must remain visible across packing and constant-index reads.
  wire [4:0][15:0] prefix;
  assign prefix[0] = {8'h0, data};
  for (genvar i = 0; i < 4; i++) begin
    assign prefix[i + 1] = prefix[i] ^ (prefix[i] << (1 << i));
  end
  assign prefix_q = prefix[4] ^ prefix[index];
endmodule
