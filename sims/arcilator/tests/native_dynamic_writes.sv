module DynamicWrites(input logic[8:0] active,input logic[26:0] banks,
 input logic[2:0] tag_index,payload_index,read_index,
 input logic[4:0] tag_value,input logic[12:0] payload_value,
 input logic[1:0] write_enable,
 output logic[71:0] matrix,output logic[89:0] fields,
 output logic[4:0] read_tag,output logic[12:0] read_payload,chained_payload);
 logic[7:0][8:0] requests;
 for(genvar q=0;q<9;q++)begin
  always_comb begin
   for(int b=0;b<8;b++)requests[b][q]=1'b0;
   if(active[q])requests[banks[3*q+:3]][q]=1'b1;
  end
 end
 typedef struct packed {logic[4:0] tag;logic[12:0] payload;} entry_t;
 entry_t[4:0] entries;
 always_comb begin
  for(int i=0;i<5;i++)entries[i].tag='0;
  if(write_enable[0])entries[tag_index].tag=tag_value;
 end
 always_comb begin
  for(int i=0;i<5;i++)entries[i].payload='0;
  if(write_enable[1])entries[payload_index].payload=payload_value;
 end
 assign matrix=requests;
 assign fields=entries;
 assign read_tag=entries[read_index].tag;
 assign read_payload=entries[read_index].payload;
 assign chained_payload=entries[entries[0].tag[2:0]].payload;
endmodule
