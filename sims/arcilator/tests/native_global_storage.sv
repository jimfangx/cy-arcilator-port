package SharedPackage;
 logic[12:0] shadow=13'd291;
 logic[64:0] wide=65'h1_0123456789abcdef;
 logic[12:0] mem[0:2]='{13'd17,13'd19,13'd23};
 function automatic logic[12:0] increment(input logic[12:0] amount);
  static logic[12:0] total=13'd341;
  total=total+amount;
  shadow=total;
  return total;
 endfunction
endpackage
module GlobalWriter #(parameter int TAG=0)(input clock,enable,input logic[12:0] data,
 input logic[64:0] index,input logic[1:0] address,output logic[12:0] q=0);
 always @(posedge clock)if(enable)begin
  q<=SharedPackage::increment(data+13'(TAG));
  SharedPackage::wide[index]=data[0];
  SharedPackage::mem[address]<=data;
 end
endmodule
module GlobalReader(output logic[12:0] shadow,output logic[64:0] wide,
 output logic[12:0] m0,m1,m2);
`ifdef ARC_GLOBAL_STARTUP
 initial begin
  SharedPackage::shadow=SharedPackage::increment(13'd7);
  SharedPackage::mem[1]<=13'd500;
  SharedPackage::mem[2]=SharedPackage::shadow;
 end
`endif
 assign shadow=SharedPackage::shadow;
 assign wide=SharedPackage::wide;
 assign m0=SharedPackage::mem[0];
 assign m1=SharedPackage::mem[1];
 assign m2=SharedPackage::mem[2];
endmodule
