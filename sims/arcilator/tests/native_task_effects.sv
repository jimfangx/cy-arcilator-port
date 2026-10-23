module TaskEffects(input bit clock,reset,enable,halt,input bit[7:0] data,
 output bit[7:0] observed,output integer startup);
 function automatic integer noisy(input integer n);
  $display("NATIVE_HIER_PLAIN n=%0d",n);
  return n+7;
 endfunction
 task automatic failure();
  $fatal(1,"native task failure");
 endtask
 initial startup=noisy(5);
 always @(posedge clock) begin
  if(reset)observed<=0;
  else if(enable)observed<=noisy(data);
  if(halt)begin failure();observed<=9;end
 end
endmodule
