module TaskAbiTop(output int after, value, state, output bit plain_value);
 import "DPI-C" context task task_probe(input int mode, input bit[64:0] wide,
                                       output int result, inout int counter);
 import "DPI-C" task plain_probe(input shortint data, output bit result);
 task automatic wrapped(input int mode);
  task_probe(mode,65'h1_76543210_fedcba98,value,state);
  after=after+1;
 endtask
 initial begin:boot
  after=0;value=0;state=7;plain_value=0;
  wrapped(0);
  plain_probe(-123,plain_value);
  if(after!=1||value!=123||state!=12||plain_value!=1)
   $fatal(1,"normal imported task completion changed output/inout values");
  wrapped(1);
  after=99;
 end
endmodule
