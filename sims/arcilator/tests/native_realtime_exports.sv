module RealtimeNano(input clock,enable,input logic[63:0] input_bits,output logic[63:0] observed);
 timeunit 1ns; timeprecision 1fs;
 realtime uninitialized;
 localparam realtime CONSTANT=-3.125;
 export "DPI-C" function rt_zero;
 function automatic realtime rt_zero();return uninitialized;endfunction
 export "DPI-C" function rt_constant;
 function automatic realtime rt_constant();return CONSTANT;endfunction
 realtime saved=1.25;
 always @(posedge clock) if(enable) saved=$bitstoreal(input_bits);
 assign observed=$realtobits(saved);
 import "DPI-C" context function realtime rt_c_identity(input realtime x);
 export "DPI-C" function rt_identity;
 function automatic realtime rt_identity(input realtime x);return x;endfunction
 export "DPI-C" function rt_import_identity;
 function automatic realtime rt_import_identity(input realtime x);return rt_c_identity(x);endfunction
 export "DPI-C" function rt_saved;
 function automatic realtime rt_saved();return saved;endfunction
 export "DPI-C" function rt_transform;
 function automatic realtime rt_transform(input realtime x,inout realtime y,output realtime z);
  realtime old_y;old_y=y;y=x*1.25+old_y;z=old_y-x*0.5;saved=y;return z+1.25;
 endfunction
 export "DPI-C" function rt_now;
 function automatic realtime local_now();return $realtime;endfunction
 function automatic realtime rt_now();return local_now();endfunction
 export "DPI-C" function rt_direct_time;
 function automatic realtime rt_direct_time();return $realtime;endfunction
 export "DPI-C" function rt_literal;
 function automatic realtime rt_literal();return 1250ps;endfunction
endmodule

module RealtimePico(input clock,enable,input logic[63:0] input_bits,output logic[63:0] observed);
 timeunit 100ps; timeprecision 1fs;
 realtime uninitialized;
 localparam realtime CONSTANT=-3.125;
 export "DPI-C" function rt_zero;
 function automatic realtime rt_zero();return uninitialized;endfunction
 export "DPI-C" function rt_constant;
 function automatic realtime rt_constant();return CONSTANT;endfunction
 realtime saved=2.5;
 always @(posedge clock) if(enable) saved=$bitstoreal(input_bits);
 assign observed=$realtobits(saved);
 import "DPI-C" context function realtime rt_c_identity(input realtime x);
 export "DPI-C" function rt_identity;
 function automatic realtime rt_identity(input realtime x);return x;endfunction
 export "DPI-C" function rt_import_identity;
 function automatic realtime rt_import_identity(input realtime x);return rt_c_identity(x);endfunction
 export "DPI-C" function rt_saved;
 function automatic realtime rt_saved();return saved;endfunction
 export "DPI-C" function rt_transform;
 function automatic realtime rt_transform(input realtime x,inout realtime y,output realtime z);
  realtime old_y;old_y=y;y=x*1.25+old_y;z=old_y-x*0.5;saved=y;return z+2.5;
 endfunction
 export "DPI-C" function rt_now;
 function automatic realtime local_now();return $realtime;endfunction
 function automatic realtime rt_now();return local_now();endfunction
 export "DPI-C" function rt_direct_time;
 function automatic realtime rt_direct_time();return $realtime;endfunction
 export "DPI-C" function rt_literal;
 function automatic realtime rt_literal();return 1250ps;endfunction
endmodule
