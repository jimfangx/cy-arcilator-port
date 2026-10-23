task global_tick(output bit [7:0] o,inout bit [7:0] n,input bit [7:0] a=11);
 bit [7:0] once=a+7;
 if(a[1])o=once+a;
 n=n+once;a=a^8'h40;
endtask
module GlobalFormalUnit(input bit clock,reset,enable,use_default,input bit [7:0] data,
 output bit [7:0] q=0,counter=0,last_arg,sticky,out_slot,inout_slot);
 bit [7:0] next_count,result;
 always @(posedge clock) begin
  if(reset)begin q<=0;counter<=0;end
  else if(enable)begin
   next_count=counter;
   if(use_default)global_tick(result,next_count);else global_tick(result,next_count,data);
   q<=result;counter<=next_count;
  end
 end
 assign last_arg=global_tick.a;assign sticky=global_tick.once;
 assign out_slot=global_tick.o;assign inout_slot=global_tick.n;
endmodule
module StaticFormalUnit(input bit clock,reset,enable,use_default,input bit [7:0] data,
 output bit [7:0] q=0,counter=0,last_arg,sticky,out_slot,inout_slot);
 task tick(output bit [7:0] o,inout bit [7:0] n,input bit [7:0] a=9);
   bit [7:0] once = a + 5;
   if(a[0]) o=once+a;
   n=n+once;
   a=a^8'h80;
 endtask
 bit [7:0] next_count,result;
 always @(posedge clock) begin
   if(reset) begin q<=0;counter<=0;end
   else if(enable) begin
     next_count=counter;
     if(use_default) tick(result,next_count);else tick(result,next_count,data);
     q<=result;counter<=next_count;
   end
 end
 assign last_arg=tick.a;
 assign sticky=tick.once;
 assign out_slot=tick.o;
 assign inout_slot=tick.n;
endmodule
module StaticFormals(input bit clock,reset,enable_a,enable_b,use_default,input bit[7:0] data,
 output bit[7:0] q0,q1,count0,count1,last0,last1,sticky0,sticky1,out0,out1,n0,n1,q2,count2,last2,sticky2,out2,n2);
 GlobalFormalUnit g(clock,reset,enable_a^enable_b,use_default,data^8'haa,q2,count2,last2,sticky2,out2,n2);
 StaticFormalUnit a(clock,reset,enable_a,use_default,data,q0,count0,last0,sticky0,out0,n0);
 StaticFormalUnit b(clock,reset,enable_b,use_default,data^8'h55,q1,count1,last1,sticky1,out1,n1);
endmodule
