module DelayCell #(parameter int W=13, parameter int D=1000)(input logic[W-1:0] value,output wire[W-1:0] q);
 timeunit 1fs;timeprecision 1fs;
 assign #(D) q=value;
endmodule
module DelayedSignalsTop(input clock,input logic[12:0] value,input logic[64:0] wide65,input logic[192:0] wide193,output wire[12:0] q13,qfast,qchain,output wire[64:0] q65,output wire[192:0] q193,output wire[12:0] constant_value,output wire delayed_clock,output logic[31:0] count);
 timeunit 1fs;timeprecision 1fs;
 DelayCell #(.W(13),.D(1000)) a(value,q13);
 DelayCell #(.W(13),.D(250)) b(value,qfast);
 DelayCell #(.W(13),.D(750)) c(qfast,qchain);
 DelayCell #(.W(65),.D(1750)) d(wide65,q65);
 DelayCell #(.W(193),.D(1500)) e(wide193,q193);
 DelayCell #(.W(1),.D(750)) f(clock,delayed_clock);
 assign #300 constant_value=13'h123;
 initial count=0;
 always @(posedge delayed_clock) count<=count+1;
endmodule
