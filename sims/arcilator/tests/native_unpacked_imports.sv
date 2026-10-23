module UnpackedImport(input logic clock,reset,enable,input int seed,output int observed);
  import "DPI-C" context function int unpacked_mix(input int seed,input int values[0:2],output byte result[-2:0],inout shortint work[3:1]);
  import "DPI-C" context unpacked_mix = function int mix_alias(input int seed,input int values[7:9],output byte result[9:7],inout shortint work[-3:-1]);
  int values[0:2];
  byte result[-2:0];
  shortint work[3:1];
  import "DPI-C" function void unpacked_vectors(input int seed,input bit[64:0] data[1:-1][4:5],output logic[128:0] result[2:0][0:1]);
  bit[64:0] vector_input[1:-1][4:5];
  logic[128:0] vector_output[2:0][0:1];
  bit[128:0] expected_vector;
  bit vector_bad;
  int row,col,n;
  int actual;
  always @(posedge clock) begin
    if(reset) observed<=0;
    else if(enable) begin
      values[0]=seed^7;values[1]=seed-11;values[2]=seed*3;
      work[3]=shortint'(seed+3);work[2]=shortint'(seed-5);work[1]=shortint'(seed^16'h8181);
      actual=seed[0] ? mix_alias(seed,values,result,work) : unpacked_mix(seed,values,result,work);
      for(row=1;row>=-1;row--) for(col=4;col<=5;col++)
        vector_input[row][col]={1'b1,32'(seed+row*17+col),32'(seed^(row*31+col*3))};
      unpacked_vectors(seed,vector_input,vector_output);
      vector_bad=0;
      for(n=0;n<6;n++) begin
        row=1-n/2;col=4+n%2;
        expected_vector[31:0]=32'((seed^(row*31+col*3))^32'hd19a3b27)&~32'hff00ff00;
        expected_vector[63:32]=32'(seed+row*17+col+71)&~32'h00ff00ff;
        expected_vector[95:64]=~32'(seed^(row*31+col*3));
        expected_vector[127:96]=32'(seed)^32'habcddcba;
        expected_vector[128]=1;
        if(n==5 && seed[2])expected_vector=0;
        if(vector_output[2-n/2][n%2]!=expected_vector)vector_bad=1;
      end
      observed<=vector_bad | (actual!=((seed^7)+(seed-11)+seed*3)) |
        (result[-2]!=byte'(-seed)) | (result[-1]!=(seed[1]?0:byte'(seed+19))) | (result[0]!=(seed[1]?0:byte'(seed^8'ha5))) |
        (work[3]!=shortint'(seed+20)) | (work[2]!=shortint'(seed-28)) | (work[1]!=shortint'((seed^16'h8181)+31));
    end
  end
endmodule
