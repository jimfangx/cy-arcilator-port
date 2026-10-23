module Loader #(parameter int WORDS=9,BITS=13,LOW=3,DESC=0,
  LEFT=DESC?LOW+WORDS-1:LOW,RIGHT=DESC?LOW:LOW+WORDS-1)
 (input clock,we,input logic[31:0] address,input logic[BITS-1:0] data,
  output logic[BITS-1:0] observed);
 logic[BITS-1:0] memory[LEFT:RIGHT];
 initial for(int i=LOW;i<LOW+WORDS;i++)memory[i]='1;
 always @(posedge clock)if(we)memory[address]<=data;
 assign observed=memory[address];
 typedef enum int {ZERO=0,ONE=1,TWO=2} enum_word;
 enum_word enum_memory[LOW:LOW+WORDS-1];
 initial for(int i=LOW;i<LOW+WORDS;i++)enum_memory[i]=ZERO;
 export "DPI-C" task readmem_enum_file;
 task readmem_enum_file(input string file);$readmemh(file,enum_memory);endtask
 export "DPI-C" function readmem_enum_get;
 function int readmem_enum_get(input int index);return int'(enum_memory[index]);endfunction

 export "DPI-C" function readmem_set;
 function int readmem_set(input int index,input bit[511:0] value);
  if(index<LOW||index>=LOW+WORDS)return 0;
  memory[index]=value[BITS-1:0];return 1;
 endfunction
 export "DPI-C" function readmem_get;
 function int readmem_get(input int index,output bit[511:0] value);
  if(index<LOW||index>=LOW+WORDS)return 0;
  value=0;value[BITS-1:0]=memory[index];return 1;
 endfunction
 export "DPI-C" task simutil_memload;
 task simutil_memload(input string file);$readmemh(file,memory);endtask
 export "DPI-C" task memload_range;
 task memload_range(input string file,input int mode,input int start,input int finish);
  case(mode)
   0:$readmemh(file,memory);
   1:$readmemh(file,memory,start);
   2:$readmemh(file,memory,start,finish);
   3:$readmemb(file,memory);
   4:$readmemb(file,memory,start,finish);
  endcase
 endtask
endmodule
module ReadAscending(input clock,we,input logic[31:0] address,
 input logic[12:0] data,output logic[12:0] observed);
 Loader ram(.*);
endmodule
module ReadDescending(input clock,we,input logic[31:0] address,
 input logic[64:0] data,output logic[64:0] observed);
 Loader #(.WORDS(9),.BITS(65),.LOW(-2),.DESC(1)) ram(.*);
endmodule
