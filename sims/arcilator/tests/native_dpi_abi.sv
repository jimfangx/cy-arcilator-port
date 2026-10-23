module NativeDPIABIForeign(
  input clock, enable,
  input [31:0] data, fp32,
  input [63:0] fp64,
  input [128:0] wide,
  output int atom,
  output bit atom_bit,
  output logic atom_logic,
  output logic [128:0] vector_bits,
  output logic [64:0] vector_logic,
  output logic [32:0] accumulator, clocked,
  output [63:0] real_bits,
  output [31:0] shortreal_bits,
  output [63:0] clocked_real_bits,
  output [31:0] clocked_shortreal_bits
);
  import "DPI-C" chipyard_abi_atom = function int atom_alias(
    input byte a, input byte unsigned b,
    input shortint c, input shortint unsigned d,
    input int e, input int unsigned f,
    input longint g, input longint unsigned h,
    input bit i, input logic j, input bit [0:0] m,
    input logic [0:0] n, output bit k, output logic l);
  import "DPI-C" chipyard_abi_vectors = function void vectors_alias(
    input bit [31:0] a, input logic [31:0] b,
    input bit [128:0] c, input logic [64:0] d,
    inout logic [32:0] e, output bit [128:0] f,
    output logic [64:0] g);
  import "DPI-C" chipyard_abi_real = function real real_alias(
    input real a, input shortreal b, output shortreal c);
  import "DPI-C" chipyard_abi_clocked = function void clocked_alias(
    input bit a, input int b, output logic [32:0] c);
  shortreal clocked_short_result;
  real clocked_real_result;
  shortreal short_result;
  real real_result;
  always_comb begin
    atom = atom_alias(byte'(data),data[15:8],shortint'(data),data[31:16],
                      int'(data),~data,longint'(int'(data)),{data,~data},
                      data[0],data[1],data[2],data[3],atom_bit,atom_logic);
  end
  always_comb begin
    accumulator = {data,1'b1};
    vectors_alias(data,~data,wide,{data,~data,1'b1},accumulator,
                  vector_bits,vector_logic);
  end
  always_comb begin
    real_result = real_alias($bitstoreal(fp64),$bitstoshortreal(fp32),short_result);
  end
  assign real_bits = $realtobits(real_result);
  assign shortreal_bits = $shortrealtobits(short_result);
  assign clocked_real_bits = $realtobits(clocked_real_result);
  assign clocked_shortreal_bits = $shortrealtobits(clocked_short_result);
  always @(posedge clock)
    clocked_real_result = real_alias($bitstoreal(fp64),$bitstoshortreal(fp32),clocked_short_result);
  always @(posedge clock) clocked_alias(enable,data,clocked);
endmodule
