module ScopeUnit(input clock,reset,enable,input logic[12:0] data,
 output logic[12:0] first=0,second=0,default_first=0,default_second=0);
 function automatic logic[12:0] step(input logic rst,en,input logic[12:0] amount);
  static logic[12:0] counter=13'd341;
  if(rst)counter=341;else if(en)counter=counter+amount;
  return counter;
 endfunction
 function logic[12:0] default_step(input logic rst,en,input logic[12:0] amount);
  logic[12:0] counter=13'd77;
  if(rst)counter=77;else if(en)counter=counter+amount;
  return counter;
 endfunction
 function automatic logic[12:0] wrapper(input logic rst,en,input logic[12:0] amount);
  return step(rst,en,amount);
 endfunction
 always @(posedge clock)begin
  first<=wrapper(reset,enable,data);
  second<=wrapper(reset,enable,data);
  default_first<=default_step(reset,enable,data+13'd1);
  default_second<=default_step(reset,enable,data+13'd3);
 end
endmodule
module StaticScopes(input clock,reset,enable_a,enable_b,latch_enable,
 input logic[12:0] data_a,data_b,latch_data,
 output logic[12:0] a0,a1,b0,b1,as0,as1,bs0,bs1,latched,fresh_seen);
 always_comb begin
  static logic[12:0] held=13'd291;
  automatic logic[12:0] fresh;
  if(reset)begin held=291;fresh=0;end
  else if(latch_enable)begin held=latch_data;fresh=latch_data;end
  latched=held;fresh_seen=fresh;
 end
 ScopeUnit a(clock,reset,enable_a,data_a,a0,a1,as0,as1);
 ScopeUnit b(clock,reset,enable_b,data_b,b0,b1,bs0,bs1);
endmodule
