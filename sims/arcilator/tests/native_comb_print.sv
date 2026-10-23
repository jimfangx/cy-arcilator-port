module PrintRTL(input logic clock,reset,enable,literal_enable,
                input logic [7:0] data, output logic [7:0] result,q,
                output logic io_success);
assign io_success=0;
always_comb begin
 result=data^8'h5a;
 if(enable) $display("NATIVE_COMB_PRINT %0d",data);
end
always_comb begin
 if(literal_enable) $write("NATIVE_COMB_LITERAL\n");
end
always_ff @(posedge clock) if(reset) q<=0; else q<=result;
endmodule
