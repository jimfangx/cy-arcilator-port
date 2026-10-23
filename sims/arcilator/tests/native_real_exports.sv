module RealExportBody #(parameter int ID=1)(input clock,enable,input logic[63:0] input_bits,output logic[63:0] observed_real,output logic[31:0] observed_short);
 real saved_real=1.25*ID;
 shortreal saved_short=2.5*ID;
 always @(posedge clock) if(enable) begin
   saved_real=$bitstoreal(input_bits);
   saved_short=$bitstoshortreal(input_bits[31:0]);
 end
 assign observed_real=$realtobits(saved_real);
 assign observed_short=$shortrealtobits(saved_short);
 export "DPI-C" function real_identity;
 function automatic real real_identity(input real value);return value;endfunction
 export "DPI-C" function short_identity;
 function automatic shortreal short_identity(input shortreal value);return value;endfunction
 export "DPI-C" function real_transform;
 function automatic real real_transform(input real x,inout real y,output real z);
   real old_y;
   old_y=y; y=x*1.25+old_y; z=old_y-x*0.5; saved_real=y;
   return z+real'(ID);
 endfunction
 export "DPI-C" function short_transform;
 function automatic shortreal short_transform(input shortreal x,inout shortreal y,output shortreal z);
   shortreal old_y;
   old_y=y; y=shortreal'(x*1.25+old_y); z=shortreal'(old_y-x*0.5); saved_short=y;
   return shortreal'(z+ID);
 endfunction
 export "DPI-C" function real_saved;
 function automatic real real_saved();return saved_real;endfunction
 export "DPI-C" function short_saved;
 function automatic shortreal short_saved();return saved_short;endfunction
endmodule
module RealExportFirst(input clock,enable,input logic[63:0] input_bits,output logic[63:0] observed_real,output logic[31:0] observed_short);
 RealExportBody #(.ID(1)) child(.*);
endmodule
module RealExportSecond(input clock,enable,input logic[63:0] input_bits,output logic[63:0] observed_real,output logic[31:0] observed_short);
 RealExportBody #(.ID(2)) child(.*);
endmodule
