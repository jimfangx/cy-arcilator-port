module DPIABIMetadata;
  timeunit 10ns;
  timeprecision 100ps;
  typedef bit [0:31] bits_t;
  typedef logic [32:0] logic_t;
  typedef enum int { ZERO = 0 } atom_t;
  typedef struct packed { bit [3:0] a; bit [4:0] b; } packed_bits_t;
  typedef union packed { logic [8:0] a; logic [8:0] b; } packed_logic_t;
  import "DPI-C" actual_types = function void types_alias(
    input bit a, input logic b, input byte c, input byte unsigned d,
    input shortint e, input int f, input longint g,
    input bits_t h, input logic_t i, input atom_t j,
    input packed_bits_t k, input packed_logic_t l,
    input integer m, input time n, input real o, input shortreal p,
    input realtime q, input string r, input chandle s,
    input int t[], output int u, inout longint v);
  import "DPI-C" pure actual_pure = function int pure_alias(input int value);
  import "DPI-C" context actual_context = function void context_alias(input int value);
  import "DPI-C" actual_task = task task_alias(input int value);
  int array[2], result;
  longint accumulator;
  initial begin
    types_alias(0,0,0,0,0,0,0,32'b0,33'b0,ZERO,9'b0,9'b0,
                0,0,0.0,shortreal'(0.0),0.0,"moore.metadata",null,
                array,result,accumulator);
    result = pure_alias(result);
    context_alias(result);
    task_alias(result);
  end
endmodule
