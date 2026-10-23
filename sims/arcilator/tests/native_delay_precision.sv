module DelayCell #(parameter int W=13, parameter int D=1000)(input logic[W-1:0] value,output wire[W-1:0] q);
 timeunit 1fs;timeprecision 1fs;
 assign #(D) q=value;
endmodule
module DelayPrecisionTop(input clock,input logic[12:0] value,input logic[64:0] wide65,input logic[192:0] wide193,output wire[12:0] q13,qfast,qchain,output wire[64:0] q65,output wire[192:0] q193,output wire[12:0] constant_value,output wire delayed_clock,output logic[31:0] count,output wire[192:0] qround,output wire[64:0] qtime,output wire[12:0] qexpr,output wire[12:0] qnvdla,output wire[12:0] qwait,output wire[31:0] cast_dynamic,output wire[31:0] cast_negative,output wire[31:0] cast_tie_pos,output wire[31:0] cast_tie_neg);
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
 PrecisionScope p(wide193,wide65,value,qround,qtime,qexpr,qwait);
 NvdlaDelayScope n(value,qnvdla);
 assign cast_dynamic=int'(real'(value)/10.0);
 assign cast_negative=int'(-real'(value)/10.0);
 assign cast_tie_pos=int'(2.5);
 assign cast_tie_neg=int'(-2.5);
endmodule

module PrecisionScope(input logic[192:0] wide193,input logic[64:0] wide65,input logic[12:0] value,output wire[192:0] qround,output wire[64:0] qtime,output wire[12:0] qexpr,output logic[12:0] qwait);
 timeunit 1ps;timeprecision 100fs;
 assign #0.26 qround=wide193;
 assign #0.2496ps qtime=wide65;
 assign #(0.2496ps+0.0001ps) qexpr=value;
 initial begin qwait=0;#0.26;qwait=13'h155;#0.2496ps;qwait=13'h2aa;#0.35;qwait=13'h333;end
endmodule
module NvdlaDelayScope(input logic[12:0] value,output wire[12:0] q);
 timeunit 10ps;timeprecision 1ps;
 assign #0.1 q=value;
endmodule
