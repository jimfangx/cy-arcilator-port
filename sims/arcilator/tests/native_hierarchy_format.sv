module HierarchyLeaf #(parameter integer N=0, BIAS=3)
 (input bit clock,reset,enable,input bit [7:0] data,output bit [7:0] q);
 initial $display("NATIVE_HIER_INIT m=[%m] M=[%M]");
 always @(posedge clock) begin
  if(reset)q<=BIAS+N;
  else if(enable)begin
   q<=data+BIAS+N;
   $display("NATIVE_HIER n=%0d v=%0d m=[%m] M=[%M]",N,data+BIAS+N);
  end
 end
endmodule
module HierarchyFormat #(parameter integer BIAS=3)
 (input bit clock,reset,enable,input bit[7:0] data,output bit[7:0] observed);
 wire [1:0][7:0] q;
 for(genvar n=0;n<2;n++)begin:banks
  HierarchyLeaf #(.N(n),.BIAS(BIAS)) \leaf.dot (clock,reset,enable,data,q[n]);
 end
 assign observed=q[0]+q[1];
endmodule
