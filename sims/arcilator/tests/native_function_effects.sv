module FunctionEffects(input bit clock,reset,enable,halt,input bit[7:0] data,
 output bit[7:0] observed,output integer startup);
 function automatic integer noisy(input integer n);
  $display("NATIVE_HIER_PLAIN n=%0d",n);
  return n+7;
 endfunction
 function automatic integer failure();
  $fatal(1,"native function failure");
  return 9;
 endfunction
 initial startup=noisy(5);
 always @(posedge clock) begin
  if(reset)observed<=0;
  else if(enable)observed<=noisy(data);
  if(halt)observed<=failure();
 end
endmodule
