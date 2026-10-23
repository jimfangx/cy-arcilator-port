module CapturedCell #(parameter int SEED=17)(input clock,input logic[1:0] index,output logic[12:0] value);
 logic[12:0] arr[0:3];
 initial for(int i=0;i<4;i++)arr[i]=SEED+i;
 assign value=arr[index];
 function automatic logic[12:0] mem_read(input logic[1:0] address);return arr[address];endfunction
 task mem_wr_raw_subbank(input logic[1:0] address,input logic[12:0] data);arr[address]=data;endtask
endmodule
module CapturedBank #(parameter int SECOND_SEED=233)(input clock,input logic[1:0] index,output logic[12:0] first_value,second_value,first_function,second_function);
 CapturedCell #(.SEED(17)) iow0(clock,index,first_value);
 CapturedCell #(.SEED(SECOND_SEED)) iow1(clock,index,second_value);
 function automatic logic[12:0] mem_read(input logic bank,input logic[1:0] address);
  if(bank)return iow1.mem_read(address);else return iow0.mem_read(address);
 endfunction
 assign first_function=mem_read(0,index);
 assign second_function=mem_read(1,index);
 task mem_wr_raw(input logic[2:0] address,input logic[12:0] data);
  if(address[0])iow1.mem_wr_raw_subbank(address[2:1],data);
  else iow0.mem_wr_raw_subbank(address[2:1],data);
 endtask
endmodule
module CapturedFixture #(parameter int SECOND_SEED=233)(input clock,write,input logic[2:0] address,input logic[12:0] data,input logic[1:0] index,output logic[12:0] first_value,second_value,first_function,second_function);
 CapturedBank #(.SECOND_SEED(SECOND_SEED)) ITOP(clock,index,first_value,second_value,first_function,second_function);
 task mem_wr_raw(input logic[2:0] address,input logic[12:0] data);ITOP.mem_wr_raw(address,data);endtask
 always @(posedge clock)if(write)mem_wr_raw(address,data);
endmodule
module CapturedDuplicateFixture(input clock,write,input logic[2:0] address,input logic[12:0] data,input logic[1:0] index,output logic[12:0] first_value,second_value,first_function,second_function);
 CapturedFixture #(.SECOND_SEED(17)) dut(.*);
endmodule
