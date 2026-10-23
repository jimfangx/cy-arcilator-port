// Constant-disabled PMP-style loops reproduce SDK canonicalizer growth.
module ArcRTLCFGLoops #(parameter XLEN=64)(input [2:0] select, input [15:0][7:0] q, input [63:0] data, output logic [15:0][7:0] d);
always_comb begin
 d=q;
 if(select==0) begin
  for(int i=0;i<8;i++) if (!q[i][7]) d[i] = data[(i*8)+:8];
 end else if(select==1) begin
  if(XLEN==32) begin
   for(int i=0;i<4;i++) if(!q[i+4][7]) d[i+4]=data[(i*8)+:8];
  end
 end else if(select==2) begin
  for(int i=0;i<8;i++) if(!q[i+8][7]) d[i+8]=data[(i*8)+:8];
 end else if(select==3) begin
  if(XLEN==32) begin
   for(int i=0;i<4;i++) if(!q[i+12][7]) d[i+12]=data[(i*8)+:8];
  end
 end
end
endmodule
