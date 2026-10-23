module PointerImport(input logic clock,reset,enable,input int seed,output int observed);
 import "DPI-C" function chandle get_handle(input int seed);
 import "DPI-C" context function int pointer_arrays(input chandle input_data[1:0],output chandle output_data[-1:0],inout chandle work_data[3:2]);
 import "DPI-C" context pointer_arrays = function int pointer_alias(input chandle input_data[7:6],output chandle output_data[4:3],inout chandle work_data[-1:0]);
 chandle data[1:0],result[-1:0],work[3:2];
 import "DPI-C" function void open_pointer_arrays(input int seed,input chandle values[][2:1],output chandle result[][]);
 import "DPI-C" context function void string_arrays(input int seed,input string data[2:-1],output string result[-2:1],inout string work[1:0]);
 import "DPI-C" function void clobber_strings();
 import "DPI-C" function void open_string_arrays(input string data[][][],output string result[][][]);
 chandle nested[2:1][-2:-1],nested_out[-1:0][4:5];
 string text[2:-1],text_out[-2:1],text_work[1:0];
 string cube_in[-1:0][5:4][0:1],cube_out[2:1][-2:-1][7:6];
 int i,j,k,n;
 bit bad;
 int actual;
 always @(posedge clock) if(reset)observed<=0;
 else if(enable)begin
  data[1]=get_handle(seed);data[0]=get_handle(seed+1);
  work[3]=data[0];work[2]=null;
  if(seed[0])actual=pointer_alias(data,result,work);
  else actual=pointer_arrays(data,result,work);
  bad=0;
  for(i=2;i>=1;i--)for(j=-2;j<=-1;j++)nested[i][j]=get_handle(seed+(2-i)*2+j+2);
  open_pointer_arrays(seed,nested,nested_out);
  for(i=0;i<2;i++)for(j=0;j<2;j++)
    if(nested_out[-1+i][4+j]!=nested[2-i][-2+j])bad=1;
  text[2]="alpha";text[1]=seed[0]?"":"beta";text[0]="你好";text[-1]="z";
  text_work[1]="left";text_work[0]="right";
  string_arrays(seed,text,text_out,text_work);
  clobber_strings();
  if(text_out[-2]!="alpha" || text_out[-1]!="" || text_out[0]!="你好" ||
     text_out[1]!=(seed[1]?"":"tail") || text_work[1]!="right" || text_work[0]!="left")bad=1;
  for(i=-1;i<=0;i++)for(j=5;j>=4;j--)for(k=0;k<=1;k++)cube_in[i][j][k]=k?"你好":"";
  open_string_arrays(cube_in,cube_out);
  for(i=2;i>=1;i--)for(j=-2;j<=-1;j++)for(k=7;k>=6;k--)
    if(cube_out[i][j][k]!=(k==6?"你好":""))bad=1;
  observed<=bad |(actual!=7) | (result[-1]!=data[1]) | (result[0]!=null) | (work[3]!=data[0]) | (work[2]!=data[1]);
 end
endmodule
