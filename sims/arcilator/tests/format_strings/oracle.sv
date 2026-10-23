module FormatNumericOracle;
 `include "functions.svh"
 integer tag, cycle, n;
 logic [64:0] wide;
 string label, inner, outer;
 initial begin
  for(tag=0;tag<2;tag++) begin
   for(cycle=0;cycle<32;cycle++) begin
    if(cycle%11!=0 && cycle%3!=1) begin
     n=cycle*7-80;
     wide={1'b1,64'hfedcba9876543210}^{33'b0,n[31:0]};
     if((n&3)==0)label="";else if(tag==0)label="aLpHa";else label="bEta";
     inner=branch_render(n,render(n,wide,label));
     outer=nested(inner,wide,n,tag);
     $display("FILE tag=%0d value=[%s]",tag,outer);
    end
   end
  end
 end
endmodule
