module MultiLoader2 #(parameter int BITS=13,N0=3,N1=4,N2=1,
 LOW0=-1,LOW1=5,LOW2=0,DESC0=1,DESC1=0,DESC2=0,
 L0=DESC0?LOW0+N0-1:LOW0,R0=DESC0?LOW0:LOW0+N0-1,
 L1=DESC1?LOW1+N1-1:LOW1,R1=DESC1?LOW1:LOW1+N1-1,
 L2=DESC2?LOW2+N2-1:LOW2,R2=DESC2?LOW2:LOW2+N2-1,
 SL=DESC0?LOW0+2:LOW0+1,SR=DESC0?LOW0+1:LOW0+2)
 (input clock,output observed);
 logic [BITS-1:0] memory[L0:R0][L1:R1];
 assign observed=1'b1;
 export "DPI-C" function multi_set;
 function int multi_set(input int a,input int b,input int c,input bit[511:0] value);
  if(!(a>=LOW0&&a<LOW0+N0&&b>=LOW1&&b<LOW1+N1&&c==0))return 0;
  memory[a][b]=value[BITS-1:0];return 1;
 endfunction
 export "DPI-C" function multi_get;
 function int multi_get(input int a,input int b,input int c,output bit[511:0] value);
  if(!(a>=LOW0&&a<LOW0+N0&&b>=LOW1&&b<LOW1+N1&&c==0))return 0;
  value=0;value[BITS-1:0]=memory[a][b];return 1;
 endfunction
 export "DPI-C" task multi_load;
 task multi_load(input string file,input int mode,input int start,input int finish);
  case(mode)
   0:$readmemh(file,memory);
   1:$readmemh(file,memory,start);
   2:$readmemh(file,memory,start,finish);
   3:$readmemb(file,memory);
   4:$readmemb(file,memory,start,finish);
   5:$readmemh(file,memory[SL:SR]);
   6:$readmemh(file,memory[SL:SR],start,finish);
   7:$readmemh(file,memory[LOW0+1]);
  endcase
 endtask
endmodule

module MultiLoader3 #(parameter int BITS=13,N0=3,N1=4,N2=1,
 LOW0=-1,LOW1=5,LOW2=0,DESC0=1,DESC1=0,DESC2=0,
 L0=DESC0?LOW0+N0-1:LOW0,R0=DESC0?LOW0:LOW0+N0-1,
 L1=DESC1?LOW1+N1-1:LOW1,R1=DESC1?LOW1:LOW1+N1-1,
 L2=DESC2?LOW2+N2-1:LOW2,R2=DESC2?LOW2:LOW2+N2-1,
 SL=DESC0?LOW0+2:LOW0+1,SR=DESC0?LOW0+1:LOW0+2)
 (input clock,output observed);
 logic [BITS-1:0] memory[L0:R0][L1:R1][L2:R2];
 assign observed=1'b1;
 export "DPI-C" function multi_set;
 function int multi_set(input int a,input int b,input int c,input bit[511:0] value);
  if(!(a>=LOW0&&a<LOW0+N0&&b>=LOW1&&b<LOW1+N1&&c>=LOW2&&c<LOW2+N2))return 0;
  memory[a][b][c]=value[BITS-1:0];return 1;
 endfunction
 export "DPI-C" function multi_get;
 function int multi_get(input int a,input int b,input int c,output bit[511:0] value);
  if(!(a>=LOW0&&a<LOW0+N0&&b>=LOW1&&b<LOW1+N1&&c>=LOW2&&c<LOW2+N2))return 0;
  value=0;value[BITS-1:0]=memory[a][b][c];return 1;
 endfunction
 export "DPI-C" task multi_load;
 task multi_load(input string file,input int mode,input int start,input int finish);
  case(mode)
   0:$readmemh(file,memory);
   1:$readmemh(file,memory,start);
   2:$readmemh(file,memory,start,finish);
   3:$readmemb(file,memory);
   4:$readmemb(file,memory,start,finish);
   5:$readmemh(file,memory[SL:SR]);
   6:$readmemh(file,memory[SL:SR],start,finish);
   7:$readmemh(file,memory[LOW0+1]);
  endcase
 endtask
endmodule

module Multi2A(input clock,output observed);
 MultiLoader2 #(.BITS(13),.LOW0(-1),.LOW1(5),.LOW2(0),.DESC0(1),.DESC1(0),.DESC2(0),.N1(4),.N2(1)) ram(.*);
endmodule

module Multi2B(input clock,output observed);
 MultiLoader2 #(.BITS(65),.LOW0(2),.LOW1(-3),.LOW2(0),.DESC0(0),.DESC1(1),.DESC2(0),.N1(4),.N2(1)) ram(.*);
endmodule

module Multi3A(input clock,output observed);
 MultiLoader3 #(.BITS(9),.LOW0(-2),.LOW1(3),.LOW2(-4),.DESC0(0),.DESC1(1),.DESC2(0),.N1(2),.N2(4)) ram(.*);
endmodule

module Multi3B(input clock,output observed);
 MultiLoader3 #(.BITS(129),.LOW0(4),.LOW1(-3),.LOW2(2),.DESC0(1),.DESC1(0),.DESC2(1),.N1(2),.N2(4)) ram(.*);
endmodule
