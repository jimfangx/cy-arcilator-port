module ExportRam #(parameter int unsigned WORDS=9, BITS=13)(input clock,we,
 input logic[31:0] address,input logic[BITS-1:0] data,output logic[BITS-1:0] observed);
 logic[BITS-1:0] memory[WORDS-1:0];
 initial for(int i=0;i<WORDS;i++)memory[i]=BITS'(i*17+3);
 always @(posedge clock)if(we)memory[address]<=data;
 assign observed=memory[address];
 export "DPI-C" function set_word;
 function int set_word(input int index,input bit[511:0] value);
  if(index>=WORDS)return 0;
  memory[index]=value[BITS-1:0];return 1;
 endfunction
 export "DPI-C" function get_word;
 function int get_word(input int index,output bit[511:0] value);
  if(index>=WORDS)return 0;
  value=0;value[BITS-1:0]=memory[index];return 1;
 endfunction

 export "DPI-C" function set_word_nba;
 function int set_word_nba(input int index,input bit[511:0] value);
  int previous;
  if(index>=WORDS)return -1;
  previous=int'(memory[index]);
  memory[index]<=value[BITS-1:0];return previous;
 endfunction
 import "DPI-C" context function int native_host_update(input int index,input bit[511:0] value);
 import "DPI-C" context function int native_host_probe(input int index);

 export "DPI-C" function callback_write;
 function int callback_write(input int index,input bit[511:0] value);
  int result;
  result=native_host_update(index,value);
  result+=native_host_probe(index);
  if(index>=WORDS)return result;
  return memory[index]==value[BITS-1:0]?result:-77;
 endfunction
 export "DPI-C" function string_probe;
 function int string_probe(input string value,input int index);
   return int'(value.len())*257+int'(value.getc(index));
 endfunction
 export "DPI-C" function string_transform;
 function string string_transform(input string value,input bit upper);
   if(upper)return {"PREFIX:",value.toupper(),":END"};
   return {"prefix:",value.tolower(),":end"};
 endfunction
 export "DPI-C" function string_order;
 function int string_order(input string left,input string right);
   return left<right ? -1 : left==right ? 0 : 1;
 endfunction
 export "DPI-C" function string_compare;
 function int string_compare(input string left,input string right);
   return left.compare(right);
 endfunction
 export "DPI-C" task string_update;
 task string_update(inout string value,output string other);
   other=value;
   value={value,"-updated"};
 endtask
 string saved="";
 export "DPI-C" function string_save;
 function void string_save(input string value);saved=value;endfunction
 export "DPI-C" function string_restore;
 function string string_restore();return saved;endfunction
 export "DPI-C" function string_unpack;
 function string string_unpack(input bit[128:0] value);return string'(value);endfunction
 typedef bit[64:0] packed_string_t;
 export "DPI-C" function string_pack;
 function void string_pack(input string value,output bit[64:0] packed_value);packed_value=packed_string_t'(value);endfunction
 bit[12:0] task_state=13'd99;
 export "DPI-C" task update_counter;
 task update_counter(input int delta,output int value);
   task_state=13'(task_state+delta);
   value=int'(task_state);
 endtask
 export "DPI-C" task no_op;
 task no_op(); endtask
 export "DPI-C" task clear_counter;
 task clear_counter(); task_state=13'd99; endtask
 int task_discard;
 always @(posedge clock)if(we)update_counter(1,task_discard);
 bit[12:0] secret=13'd123;
 export "DPI-C" function secret_set;
 function int secret_set(input int value);secret=13'(value);return secret;endfunction
 export "DPI-C" function secret_get;
 function int secret_get();return secret;endfunction
 export "DPI-C" function word_info;
 function int word_info(input int value);return value ^ (WORDS*100+BITS);endfunction
 export "DPI-C" function mix_abi;
 function longint signed mix_abi(input byte signed a,input shortint unsigned b,
   input bit mode,input logic gate,inout logic[64:0] value,output int signed score);
   value = value ^ 65'h1_123456789abcdef0;
   score = int'(a)-int'(b);
   return longint'(score)*37 + (mode?7:0) + (gate?11:0);
 endfunction
endmodule

module ExportNarrow(input clock,we,input logic[31:0] address,
 input logic[12:0] data,output logic[12:0] observed);
 ExportRam #(.WORDS(9),.BITS(13)) ram(.*);
endmodule
module ExportWide(input clock,we,input logic[31:0] address,
 input logic[64:0] data,output logic[64:0] observed);
 ExportRam #(.WORDS(7),.BITS(65)) ram(.*);
endmodule
