module ArcRTLRealFormat(input clock, reset, enable, input [63:0] bits,
                        output logic [7:0] count);
  always @(posedge clock) begin
    if (reset) count <= 0;
    else if (enable) begin
      automatic real value = $bitstoreal(bits);
      $write("NATIVE_REAL %0d [%12.3f] [%-20.4e] [%0.9g]\n",
             count, value, value, value);
      count <= count + 1;
    end
  end
endmodule
