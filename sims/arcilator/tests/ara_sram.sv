module OriginalSramNarrow(input clock,reset_n,req,we,input logic[3:0] address,
 input logic[12:0] data,input logic[1:0] mask,output logic[12:0] observed);
 tc_sram #(.NumWords(9),.DataWidth(13),.NumPorts(1),.Latency(1),.SimInit("zeros"))
 mem(.clk_i(clock),.rst_ni(reset_n),.req_i(req),.we_i(we),.addr_i(address),
     .wdata_i(data),.be_i(mask),.rdata_o(observed));
endmodule
module OriginalSramWide(input clock,reset_n,req,we,input logic[3:0] address,
 input logic[64:0] data,input logic[8:0] mask,output logic[64:0] observed);
 tc_sram #(.NumWords(9),.DataWidth(65),.NumPorts(1),.Latency(1),.SimInit("zeros"))
 mem(.clk_i(clock),.rst_ni(reset_n),.req_i(req),.we_i(we),.addr_i(address),
     .wdata_i(data),.be_i(mask),.rdata_o(observed));
endmodule
