module FeedbackRTL(input logic clock,reset,enable,oscillate,
 input logic [7:0] data,mask,output logic [7:0] first,second,q,
 output logic io_success);
 logic [7:0] feedback;
 assign feedback=first^(oscillate?8'b0:mask);
 assign io_success=0;
 always_comb begin
  first=oscillate?~feedback:data+8'd1;
  second=feedback^8'ha5;
  if(enable&&!oscillate) $display("NATIVE_LOOP_PRINT %0d %0d",data,feedback);
 end
 always_ff @(posedge clock) if(reset)q<=0;else q<=second;
endmodule
