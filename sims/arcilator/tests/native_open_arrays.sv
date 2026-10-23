module OpenImport(input logic clock,reset,enable,input int seed,output int observed);
  import "DPI-C" context function int open_mix(input int seed,input int values[],output byte result[],inout shortint work[]);
  import "DPI-C" context open_mix = function int mix_alias(input int seed,input int values[],output byte result[],inout shortint work[]);
  int values[0:2];
  byte result[-2:0];
  shortint work[3:1];
  import "DPI-C" function void open_vectors(input int seed,input bit[64:0] data[][],output logic[128:0] result[][]);
  bit[64:0] vector_input[1:-1][4:5];
  logic[128:0] vector_output[2:0][0:1];
  bit[128:0] expected_vector;
  bit vector_bad;
  int row,col,n;
  import "DPI-C" function void open_cube(input int seed,inout longint data[][][]);
  import "DPI-C" function void open_scalars(input int seed,input bit data[],output logic result[]);
  import "DPI-C" function int open_shape(input int data[]);
  import "DPI-C" open_shape = function int shape_alias(input int data[]);
  import "DPI-C" function int open_mixed(input int data[2:1][]);
  int shifted[9:8];
  int mixed[7:6][-1:1];
  int sum_actual;
  longint cube[-1:0][4:3][0:1];
  bit scalar_input[-3:-1];
  logic scalar_output[3:1];
  bit extra_bad;
  int x,y,z;
  int actual;
  always @(posedge clock) begin
    if(reset) observed<=0;
    else if(enable) begin
      values[0]=seed^7;values[1]=seed-11;values[2]=seed*3;
      work[3]=shortint'(seed+3);work[2]=shortint'(seed-5);work[1]=shortint'(seed^16'h8181);
      if(seed[0]) actual=mix_alias(seed,values,result,work);
      else actual=open_mix(seed,values,result,work);
      for(row=1;row>=-1;row--) for(col=4;col<=5;col++)
        vector_input[row][col]={1'b1,32'(seed+row*17+col),32'(seed^(row*31+col*3))};
      open_vectors(seed,vector_input,vector_output);
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
      for(x=-1;x<=0;x++) for(y=4;y>=3;y--) for(z=0;z<=1;z++)
        cube[x][y][z]=64'(seed)+x*101+y*13+z;
      open_cube(seed,cube);
      extra_bad=0;
      for(x=-1;x<=0;x++) for(y=4;y>=3;y--) for(z=0;z<=1;z++)
        if(cube[x][y][z]!=(64'(seed)+x*101+y*13+z+17)) extra_bad=1;
      scalar_input[-3]=seed[0];scalar_input[-2]=seed[1];scalar_input[-1]=seed[2];
      open_scalars(seed,scalar_input,scalar_output);
      if(scalar_output[3]!=seed[0] || scalar_output[2]!=0 || scalar_output[1]!=0) extra_bad=1;
      shifted[9]=seed;shifted[8]=seed+1;
      sum_actual=open_shape(shifted);
      for(x=7;x>=6;x--)for(y=-1;y<=1;y++)mixed[x][y]=seed+x*7+y;
      sum_actual+=shape_alias(values);
      sum_actual+=open_mixed(mixed);
      if(sum_actual!=(seed*8+274+(seed^7)+(seed-11)+seed*3))extra_bad=1;
      observed<=extra_bad | vector_bad | (actual!=((seed^7)+(seed-11)+seed*3)) |
        (result[-2]!=byte'(-seed)) | (result[-1]!=(seed[1]?0:byte'(seed+19))) | (result[0]!=(seed[1]?0:byte'(seed^8'ha5))) |
        (work[3]!=shortint'(seed+20)) | (work[2]!=shortint'(seed-28)) | (work[1]!=shortint'((seed^16'h8181)+31));
    end
  end
endmodule
